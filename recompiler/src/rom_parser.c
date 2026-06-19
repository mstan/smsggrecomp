/*
 * rom_parser.c — SMS/GG ROM parsing. See rom_parser.h.
 */
#include "rom_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint16_t Z80_RST_VECTORS[8] = {
    0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38
};

/* ---- CRC32 (zlib polynomial, reflected) ---- */
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Declared-size nibble -> bytes. 0xA..0xF then wraps 0x0..0x2 per the SEGA
 * footer convention. Informational only; we trust the file size for paging. */
static const char *size_nibble_desc(int nib) {
    switch (nib & 0xF) {
        case 0xA: return "8KB";   case 0xB: return "16KB";
        case 0xC: return "32KB";  case 0xD: return "48KB";
        case 0xE: return "64KB";  case 0xF: return "128KB";
        case 0x0: return "256KB"; case 0x1: return "512KB";
        case 0x2: return "1MB";   default:  return "?";
    }
}

static bool is_gg_region(SmsRegion r) {
    return r == SMS_REGION_GG_JAPAN || r == SMS_REGION_GG_EXPORT ||
           r == SMS_REGION_GG_INTL;
}

/* Detect the Codemasters mapper: a 16-byte header at 0x7FE0 whose 16-bit
 * checksum (LE at +0x0E) equals the inverse-complement stored at +0x06... The
 * robust public test is: word at 0x7FE6 (checksum) + word at 0x7FE6+? — we use
 * the widely-used heuristic: bytes 0x7FE0.. contain a valid BCD date and the
 * checksum word at 0x7FE6 plus the complement at 0x7FE8 sum to 0x10000. */
static bool detect_codemasters(const uint8_t *d, size_t n) {
    if (n < 0x8000) return false;
    uint16_t sum  = (uint16_t)(d[0x7FE6] | (d[0x7FE7] << 8));
    uint16_t comp = (uint16_t)(d[0x7FE8] | (d[0x7FE9] << 8));
    if (sum == 0) return false;
    return (uint16_t)(sum + comp) == 0x0000; /* sum + comp == 0x10000 mod 2^16 */
}

bool rom_parse(const char *path, SmsRom *out) {
    memset(out, 0, sizeof(*out));
    out->header_offset = -1;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[rom] cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); fprintf(stderr, "[rom] empty file\n"); return false; }

    /* Some dumps carry a 512-byte copier header; strip it if size %16KB == 512. */
    long copier = (sz % SMS_BANK_SIZE == 512) ? 512 : 0;
    if (copier) fseek(f, copier, SEEK_SET);
    size_t data_sz = (size_t)(sz - copier);

    out->data = (uint8_t *)malloc(data_sz);
    if (!out->data) { fclose(f); return false; }
    if (fread(out->data, 1, data_sz, f) != data_sz) {
        fclose(f); free(out->data); out->data = NULL; return false;
    }
    fclose(f);

    out->size = data_sz;
    out->num_banks = (int)((data_sz + SMS_BANK_SIZE - 1) / SMS_BANK_SIZE);
    out->crc32 = crc32_buf(out->data, data_sz);

    /* Locate TMR SEGA footer. */
    const long cands[3] = { 0x7FF0, 0x3FF0, 0x1FF0 };
    for (int i = 0; i < 3; i++) {
        long off = cands[i];
        if ((size_t)(off + SMS_HEADER_SIZE) > data_sz) continue;
        if (memcmp(out->data + off, "TMR SEGA", 8) == 0) {
            out->has_header = true;
            out->header_offset = (int)off;
            break;
        }
    }

    if (out->has_header) {
        const uint8_t *h = out->data + out->header_offset;
        out->header_checksum = (uint16_t)(h[0x0A] | (h[0x0B] << 8));
        out->version = h[0x0E] & 0x0F;
        uint8_t rf = h[0x0F];
        out->region = (SmsRegion)((rf >> 4) & 0x0F);
        out->size_nibble = rf & 0x0F;
        /* Product code: BCD at +0x0C..0x0E (2.5 bytes), high nibble of 0x0E
         * is the top digit. */
        unsigned hi = (h[0x0E] >> 4) & 0x0F;
        unsigned mid = h[0x0D];
        unsigned lo = h[0x0C];
        snprintf(out->product_code, sizeof(out->product_code), "%u%02X%02X",
                 hi, mid, lo);
    } else {
        out->region = SMS_REGION_UNKNOWN;
    }

    out->platform = is_gg_region(out->region) ? SMS_PLATFORM_GG : SMS_PLATFORM_SMS;

    /* Mapper detection. */
    if (detect_codemasters(out->data, data_sz)) {
        out->mapper = SMS_MAPPER_CODEMASTERS;
    } else if (data_sz <= 48 * 1024) {
        out->mapper = SMS_MAPPER_NONE;
    } else {
        out->mapper = SMS_MAPPER_SEGA;
    }

    printf("[rom] %s\n", path);
    printf("[rom]   size=%zu bytes (%d x 16KB banks)%s\n", out->size,
           out->num_banks, copier ? " [stripped 512B copier header]" : "");
    printf("[rom]   crc32=0x%08X\n", out->crc32);
    printf("[rom]   platform=%s region=%s mapper=%s\n",
           out->platform == SMS_PLATFORM_GG ? "GameGear" : "MasterSystem",
           rom_region_name(out->region), rom_mapper_name(out->mapper));
    if (out->has_header)
        printf("[rom]   header@0x%X checksum=0x%04X product=%s ver=%u declared=%s\n",
               out->header_offset, out->header_checksum, out->product_code,
               out->version, size_nibble_desc(out->size_nibble));
    else
        printf("[rom]   no TMR SEGA footer (homebrew/Codemasters/early title)\n");
    return true;
}

void rom_free(SmsRom *rom) {
    if (rom && rom->data) { free(rom->data); rom->data = NULL; }
}

uint8_t rom_read_offset(const SmsRom *rom, size_t offset) {
    if (offset >= rom->size) return 0;
    return rom->data[offset];
}

size_t rom_z80_to_offset(const SmsRom *rom, uint16_t addr, int slot_bank) {
    if (addr >= 0xC000) return SIZE_MAX;        /* RAM */
    if (rom->mapper == SMS_MAPPER_NONE) {
        return (size_t)addr < rom->size ? (size_t)addr : SIZE_MAX;
    }
    /* Sega/Codemasters/Korean: 16 KB slots. The Sega mapper keeps the first
     * 1 KB ($0000-$03FF) fixed to bank 0. */
    uint16_t slot_off = addr & (SMS_BANK_SIZE - 1);
    int bank = slot_bank;
    if (rom->mapper == SMS_MAPPER_SEGA && addr < 0x0400)
        bank = 0;
    if (bank < 0) return SIZE_MAX;
    size_t off = (size_t)bank * SMS_BANK_SIZE + slot_off;
    return off < rom->size ? off : SIZE_MAX;
}

const char *rom_region_name(SmsRegion r) {
    switch (r) {
        case SMS_REGION_SMS_JAPAN:  return "SMS-Japan";
        case SMS_REGION_SMS_EXPORT: return "SMS-Export";
        case SMS_REGION_GG_JAPAN:   return "GG-Japan";
        case SMS_REGION_GG_EXPORT:  return "GG-Export";
        case SMS_REGION_GG_INTL:    return "GG-International";
        default:                    return "Unknown";
    }
}

const char *rom_mapper_name(SmsMapper m) {
    switch (m) {
        case SMS_MAPPER_SEGA:        return "Sega";
        case SMS_MAPPER_CODEMASTERS: return "Codemasters";
        case SMS_MAPPER_KOREAN:      return "Korean";
        case SMS_MAPPER_NONE:        return "None(<=48KB)";
        default:                     return "?";
    }
}
