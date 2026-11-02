/*
 * sms_vdp.c - SMS/GG VDP timing + memory core. See sms_vdp.h.
 *
 * Faithful to the mode-4 register/IRQ interface; rendering is not implemented
 * yet (headless bring-up). Interrupt behaviour follows the documented SMS VDP:
 *   - Frame interrupt latches at the start of line 192 (enabled by reg1 bit5).
 *   - Line interrupt uses a down-counter reloaded from reg10 across the active
 *     region; underflow latches a line interrupt (enabled by reg0 bit4).
 *   - Reading the status port returns and CLEARS both interrupt flags.
 */
#include "sms_vdp.h"
#include "sms_clocks.h"
#include <string.h>

SmsVdp g_vdp;

static VdpWriteObserver g_vdp_obs;
static bool raster_enabled;
static VdpFrameJob prepared_frame;
static uint32_t reference_frame[SMS_SCREEN_W*SMS_SCREEN_H];
static void capture_line(int y);
void vdp_enable_raster(bool enabled){raster_enabled=enabled;}
void vdp_set_write_observer(VdpWriteObserver f){ g_vdp_obs = f; }

void vdp_reset(bool is_gg){
    memset(&g_vdp, 0, sizeof(g_vdp));
    memset(&prepared_frame,0,sizeof prepared_frame);
    memset(reference_frame,0,sizeof reference_frame);
    g_vdp.is_gg = is_gg;
    g_vdp.line_counter = 0xFF;
    /* Mode-4 sensible defaults; games overwrite these immediately. */
    g_vdp.reg[2] = 0xFF;   /* name table base   */
    g_vdp.reg[5] = 0xFF;   /* sprite table base */
}

void vdp_control_write(uint8_t v){
    if (!g_vdp.ctrl_second){
        g_vdp.latch = v;
        g_vdp.ctrl_second = true;
        g_vdp.addr = (uint16_t)((g_vdp.addr & 0x3F00) | v);
        return;
    }
    g_vdp.ctrl_second = false;
    g_vdp.addr = (uint16_t)((g_vdp.latch) | ((v & 0x3F) << 8));
    g_vdp.code = (uint8_t)(v >> 6);
    if (g_vdp.code == 0){
        /* read setup: prefetch and post-increment */
        g_vdp.readbuf = g_vdp.vram[g_vdp.addr & 0x3FFF];
        g_vdp.addr = (uint16_t)((g_vdp.addr + 1) & 0x3FFF);
    } else if (g_vdp.code == 2){
        /* register write: reg number in low nibble, value is the first byte */
        g_vdp.reg[v & 0x0F] = g_vdp.latch;
        if (g_vdp_obs) g_vdp_obs(VDPW_REG, (uint16_t)(v & 0x0F), g_vdp.latch);
    }
}

void vdp_data_write(uint8_t v){
    g_vdp.ctrl_second = false;
    if (g_vdp.code == 3){
        if (g_vdp.is_gg){
            /* GG: 12-bit CRAM, two bytes per entry; even byte latched. */
            if ((g_vdp.addr & 1) == 0) g_vdp.cram_latch = v;
            else {
                uint8_t idx = (uint8_t)(g_vdp.addr & 0x3E);
                g_vdp.cram[idx]   = g_vdp.cram_latch;
                g_vdp.cram[idx+1] = (uint8_t)(v & 0x0F);
                if (g_vdp_obs) g_vdp_obs(VDPW_CRAM, idx, v);   /* on commit */
            }
        } else {
            g_vdp.cram[g_vdp.addr & 0x1F] = v;
            if (g_vdp_obs) g_vdp_obs(VDPW_CRAM, (uint16_t)(g_vdp.addr & 0x1F), v);
        }
    } else {
        g_vdp.vram[g_vdp.addr & 0x3FFF] = v;
        if (g_vdp_obs) g_vdp_obs(VDPW_VRAM, (uint16_t)(g_vdp.addr & 0x3FFF), v);
    }
    g_vdp.readbuf = v;
    g_vdp.addr = (uint16_t)((g_vdp.addr + 1) & 0x3FFF);
}

uint8_t vdp_data_read(void){
    g_vdp.ctrl_second = false;
    uint8_t r = g_vdp.readbuf;
    g_vdp.readbuf = g_vdp.vram[g_vdp.addr & 0x3FFF];
    g_vdp.addr = (uint16_t)((g_vdp.addr + 1) & 0x3FFF);
    return r;
}

uint8_t vdp_status_read(void){
    uint8_t r = (uint8_t)(g_vdp.status | 0x1F);  /* low bits read as 1 */
    g_vdp.status &= (uint8_t)~0xE0;               /* clear INT/ovr/collision */
    g_vdp.frame_irq = false;
    g_vdp.line_irq  = false;
    g_vdp.ctrl_second = false;
    return r;
}

uint8_t vdp_vcounter(void){
    /* SMS NTSC mode-4 V-counter: 0x00..0xDA, then a jump-back to 0xD5 (line 219)
     * running to 0xFF (line 261) — total 262 lines. Matches GPGX vc_table
     * {0xDA,0xF2}. (Active display is lines 0..191 < 0xDA, so this only changes
     * vblank-region reads — but it's now hardware-correct everywhere.) */
    int l = g_vdp.line;
    return (uint8_t)(l <= 0xDA ? l : l - 6);
}

/* SMS 256-pixel-mode H-counter. The real chip exposes a 342-pixel/line counter
 * whose read value rises 0x00..0x93 over the visible span then jumps to
 * 0xE9..0xFF across HBLANK. This is a clean-room piecewise approximation from
 * the sub-line cycle (228 T-states/line, ~1.5 pixel-clocks per T-state); it is
 * NOT the exact per-cycle table (which is emulator-specific and unused by the
 * tested titles), but it is monotone + correctly shaped, vs the prior constant
 * 0. sub_line_cyc is clamped to [0,227]. */
uint8_t vdp_hcounter(int sub_line_cyc){
    if (sub_line_cyc < 0) sub_line_cyc = 0;
    if (sub_line_cyc > 227) sub_line_cyc = 227;
    int px = (sub_line_cyc * 342) / 228;          /* pixel position 0..341 */
    if (px <= 0x127)        return (uint8_t)(px >> 1);                 /* 0x00..0x93 */
    return (uint8_t)(((px - 0x128) >> 1) + 0xE9);                      /* 0xE9..0xFF */
}

void vdp_step_line(void){
    if(raster_enabled && g_vdp.line<SMS_SCREEN_H)capture_line(g_vdp.line);
    /* Line-interrupt counter: active across lines 0..192, reload otherwise. */
    if (g_vdp.line <= SMS_ACTIVE_LINES){
        if (g_vdp.line_counter == 0){
            g_vdp.line_counter = g_vdp.reg[10];
            g_vdp.line_irq = true;
        } else {
            g_vdp.line_counter--;
        }
    } else {
        g_vdp.line_counter = g_vdp.reg[10];
    }

    g_vdp.line++;
    if (g_vdp.line == SMS_VBLANK_LINE){
        g_vdp.status |= 0x80;
        g_vdp.frame_irq = true;
    }
    if (g_vdp.line >= SMS_LINES_PER_FRAME){
        g_vdp.line = 0;
    }
}

bool vdp_irq_asserted(void){
    return (g_vdp.frame_irq && (g_vdp.reg[1] & 0x20)) ||
           (g_vdp.line_irq  && (g_vdp.reg[0] & 0x10));
}

/* ---- mode-4 rendering ---- */
/* Resolve CRAM entry (0..31) to ARGB8888. SMS: 6-bit --BBGGRR (2 bits/chan).
 * GG: 12-bit, 2 bytes/entry (----RRRR ----GGGG ----BBBB packed as lo,hi). */
static uint32_t cram_argb(int idx){
    if (g_vdp.is_gg){
        uint8_t lo = g_vdp.cram[(idx*2) & 0x3F];
        uint8_t hi = g_vdp.cram[(idx*2+1) & 0x3F];
        int r = lo & 0x0F, g = (lo >> 4) & 0x0F, b = hi & 0x0F;
        r = (r << 4) | r; g = (g << 4) | g; b = (b << 4) | b;
        return 0xFF000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
    } else {
        uint8_t c = g_vdp.cram[idx & 0x1F];
        int r = c & 3, g = (c >> 2) & 3, b = (c >> 4) & 3;
        return 0xFF000000u | (uint32_t)((r*85) << 16) | (uint32_t)((g*85) << 8) | (uint32_t)(b*85);
    }
}

/* Fetch the 4-bit colour of pixel (px,py) within tile `tile` (planar, 32 B). */
static int tile_pixel(int tile, int px, int py){
    const uint8_t *p = &g_vdp.vram[((tile * 32) + py * 4) & 0x3FFF];
    int bit = 7 - px;
    return  ((p[0] >> bit) & 1)
         | (((p[1] >> bit) & 1) << 1)
         | (((p[2] >> bit) & 1) << 2)
         | (((p[3] >> bit) & 1) << 3);
}

int g_vdp_force_display = 0;   /* debug: render even when display is disabled */

static void render_reference_lines(uint32_t *fb,int first,int last){
    const uint8_t r0 = g_vdp.reg[0], r1 = g_vdp.reg[1];
    uint32_t backdrop = cram_argb(16 + (g_vdp.reg[7] & 0x0F));

    /* display disabled -> whole frame is backdrop */
    if (!(r1 & 0x40) && !g_vdp_force_display){
        for (int i = SMS_SCREEN_W*first; i < SMS_SCREEN_W*last; i++) fb[i] = backdrop;
        return;
    }

    int nt_base  = (g_vdp.reg[2] & 0x0E) << 10;            /* name table */
    int hscroll  = g_vdp.reg[8];
    int vscroll  = g_vdp.reg[9];

    /* per-pixel priority of the background (for sprite occlusion) */
    static uint8_t bg_prio[SMS_SCREEN_W * SMS_SCREEN_H];

    /* ---- background ---- */
    for (int y = first; y < last; y++){
        for (int x = 0; x < SMS_SCREEN_W; x++){
            int hs = (r0 & 0x40) && y < 16  ? 0 : hscroll;   /* lock top 2 rows */
            int vs = (r0 & 0x80) && x >= 192 ? 0 : vscroll;  /* lock right cols  */
            int bx = (x - hs) & 0xFF;
            int by = (y + vs) % 224;

            int col = (bx >> 3) & 31;
            int row = by >> 3;
            int ent_addr = (nt_base + (row * 32 + col) * 2) & 0x3FFF;
            int entry = g_vdp.vram[ent_addr] | (g_vdp.vram[ent_addr + 1] << 8);

            int tile  = entry & 0x1FF;
            int hflip = entry & 0x200;
            int vflip = entry & 0x400;
            int pal   = (entry & 0x800) ? 16 : 0;
            int prio  = entry & 0x1000;

            int px = bx & 7, py = by & 7;
            if (hflip) px = 7 - px;
            if (vflip) py = 7 - py;
            int c = tile_pixel(tile, px, py);

            fb[y * SMS_SCREEN_W + x] = cram_argb(pal + c);
            bg_prio[y * SMS_SCREEN_W + x] = (uint8_t)(prio && c != 0);
        }
    }

    /* mask leftmost 8 pixels with backdrop (reg0 bit5) */
    if (r0 & 0x20)
        for (int y = first; y < last; y++)
            for (int x = 0; x < 8; x++)
                fb[y * SMS_SCREEN_W + x] = backdrop;

    /* ---- sprites ---- */
    int sat   = (g_vdp.reg[5] & 0x7E) << 7;
    int sh    = (r1 & 0x02) ? 16 : 8;          /* 8x16 sprites */
    int sbase = (g_vdp.reg[6] & 0x04) ? 256 : 0; /* pattern bank (tile +256) */
    int shift = (r0 & 0x08) ? 8 : 0;            /* early clock: X-8 */

    for (int i = 0; i < 64; i++){
        int yraw = g_vdp.vram[(sat + i) & 0x3FFF];
        if (yraw == 0xD0) break;                /* terminator (192-line mode) */
        int sy = (yraw + 1) & 0xFF;
        int sx = g_vdp.vram[(sat + 128 + i*2)     & 0x3FFF] - shift;
        int st = g_vdp.vram[(sat + 128 + i*2 + 1) & 0x3FFF];
        if (r1 & 0x02) st &= 0xFE;

        for (int ry = 0; ry < sh; ry++){
            int y = sy + ry;
            if (y < first || y >= last) continue;
            int tile = sbase + st + (ry >> 3);
            int py = ry & 7;
            for (int rx = 0; rx < 8; rx++){
                int x = sx + rx;
                if (x < 0 || x >= SMS_SCREEN_W) continue;
                int c = tile_pixel(tile, rx, py);
                if (c == 0) continue;            /* transparent */
                if (bg_prio[y * SMS_SCREEN_W + x]) continue; /* behind hi-prio bg */
                fb[y * SMS_SCREEN_W + x] = cram_argb(16 + c); /* sprite palette */
            }
        }
    }
}

void vdp_render_reference(uint32_t *fb){render_reference_lines(fb,0,SMS_SCREEN_H);}
static uint32_t row_word(int tile, int py){
    const uint8_t *p=&g_vdp.vram[(tile*32+py*4)&0x3fff];
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static unsigned row_pixel(uint32_t w, unsigned x){
    unsigned b=7-x;
    return ((w>>b)&1) | (((w>>(b+8))&1)<<1) |
           (((w>>(b+16))&1)<<2) | (((w>>(b+24))&1)<<3);
}
static void prepare_line(VdpLineJob *j, int y){
    memset(j,0,sizeof *j);
    unsigned r0=g_vdp.reg[0],r1=g_vdp.reg[1];
    for (int i=0;i<32;i++) j->word[96+i/4][i%4]=cram_argb(i);
    j->word[104][0]=cram_argb(16+(g_vdp.reg[7]&15));
    j->word[104][1]=(r1&0x40) || g_vdp_force_display;
    j->word[104][3]=r0&0x20;
    if (!j->word[104][1]) return;
    int nt=(g_vdp.reg[2]&14)<<10;
    for (int block=0;block<32;block++){
        int x=block*8;
        int hs=(r0&0x40) && y<16 ? 0 : g_vdp.reg[8];
        int vs=(r0&0x80) && x>=192 ? 0 : g_vdp.reg[9];
        int bx=(x-hs)&255, by=(y+vs)%224;
        for (int side=0;side<2;side++){
            int col=((bx>>3)+side)&31;
            int a=(nt+((by>>3)*32+col)*2)&0x3fff;
            unsigned entry=g_vdp.vram[a] | g_vdp.vram[(a+1)&0x3fff]<<8;
            int py=(by&7)^((entry&0x400)?7:0);
            j->word[block][side]=row_word(entry&511,py);
            j->word[block][2+side]=entry | (unsigned)(bx&7)<<16;
        }
    }
    int sat=(g_vdp.reg[5]&0x7e)<<7;
    int sh=(r1&2)?16:8, base=(g_vdp.reg[6]&4)?256:0;
    unsigned n=0;
    for (int i=0;i<64;i++){
        int raw=g_vdp.vram[(sat+i)&0x3fff];
        if (raw==0xd0) break;
        int sy=(raw+1)&255, ry=y-sy;
        if (ry<0 || ry>=sh) continue;
        int sx=g_vdp.vram[(sat+128+i*2)&0x3fff]-((r0&8)?8:0);
        if (sx>=256 || sx+8<=0) continue;
        int tile=g_vdp.vram[(sat+129+i*2)&0x3fff];
        if (r1&2) tile&=~1;
        j->word[32+n][0]=row_word(base+tile+(ry>>3),ry&7);
        j->word[32+n][1]=(uint32_t)sx;
        n++;
    }
    j->word[104][2]=n;
}
void vdp_prepare_frame(VdpFrameJob *f){
    for (int y=0;y<SMS_SCREEN_H;y++) prepare_line(&f->line[y],y);
}
static void capture_line(int y){
    prepare_line(&prepared_frame.line[y],y);
#ifdef SMS_VDP_REFERENCE
    render_reference_lines(reference_frame,y,y+1);
#endif
}
void vdp_render_jobs(const VdpFrameJob *f,uint32_t *fb){
    for (int y=0;y<SMS_SCREEN_H;y++){
        const VdpLineJob *j=&f->line[y];
        for (int x=0;x<SMS_SCREEN_W;x++){
            uint32_t color=j->word[104][0];
            if (j->word[104][1]){
                const uint32_t *b=j->word[x/8];
                unsigned u=(unsigned)(x&7)+(b[2]>>16), side=u>>3;
                unsigned flags=b[2+side], px=(u&7)^((flags&0x200)?7:0);
                unsigned c=row_pixel(b[side],px), pal=(flags&0x800)?16:0;
                color=j->word[96+(pal+c)/4][(pal+c)%4];
                if (j->word[104][3] && x<8) color=j->word[104][0];
                if (!(flags&0x1000) || !c){
                    for (int s=(int)j->word[104][2]-1;s>=0;s--){
                        const uint32_t *sp=j->word[32+s];
                        int dx=x-(int32_t)sp[1];
                        if (dx<0 || dx>=8) continue;
                        c=row_pixel(sp[0],(unsigned)dx);
                        if (c){ color=j->word[100+c/4][c%4]; break; }
                    }
                }
            }
            fb[y*SMS_SCREEN_W+x]=color;
        }
    }
}
const VdpFrameJob *vdp_frame_jobs(void){ return &prepared_frame; }
void vdp_render_frame(uint32_t *fb){
    if(!raster_enabled)vdp_prepare_frame(&prepared_frame);
#ifdef SMS_VDP_REFERENCE
    if(raster_enabled)memcpy(fb,reference_frame,sizeof reference_frame);
    else vdp_render_reference(fb);
#else
    vdp_render_jobs(&prepared_frame,fb);
#endif
}
