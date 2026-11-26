/* Engine adapter: accepted SMS/GG native synthesis lives in sega-psg-core. */
#include "sn76489.h"
#include "sms_clocks.h"
#include "sega_psg.h"
#include <stdlib.h>
static sega_psg *chip;
void psg_init(void){if(!chip)chip=sega_psg_create(SEGA_PSG_SMS_GG,SMS_Z80_HZ);else sega_psg_reset(chip);if(!chip)abort();}
void psg_write(uint8_t v){sega_psg_write(chip,v);}
void psg_write_stereo(uint8_t v){sega_psg_write_stereo(chip,v);}
void psg_advance(uint32_t c){sega_psg_advance(chip,c);}
size_t psg_frames_available(void){return sega_psg_available(chip);}
size_t psg_render(int16_t *p,size_t n){return sega_psg_render(chip,p,n);}
void psg_drain(void (*sink)(const int16_t *,size_t)){int16_t block[8192*2];size_t n;while((n=psg_render(block,8192)))if(sink)sink(block,n);}
uint32_t psg_sample_rate(void){return SMS_Z80_HZ/16;}
void psg_reset_leftover(void){sega_psg_reset_leftover(chip);}
int psg_save_state(FILE *f){return sega_psg_save(chip,f);}
int psg_load_state(FILE *f){return sega_psg_load(chip,f);}
void psg_dump_state_text(FILE *f){
 char text[1024];int n=sega_psg_state_dump(chip,text,sizeof text);if(n)fwrite(text,1,(size_t)n,f);
}
