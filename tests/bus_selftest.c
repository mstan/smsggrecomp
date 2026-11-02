#include "glue.h"
#include "include/sms_runtime.h"
#include <stdio.h>
#include <stdlib.h>
void call_by_address(uint16_t a){(void)a;abort();}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    unsigned sizes[]={1024,32768,49152,49153};
    for(unsigned t=0;t<sizeof sizes/sizeof sizes[0];t++){
        unsigned size=sizes[t];FILE *f=fopen(argv[1],"wb");if(!f)return 2;
        for(unsigned i=0;i<size;i++)fputc((i*13+(i>>8)*17+(i>>14)*31)&255,f);
        fclose(f);if(!glue_load_rom(argv[1]))return 2;glue_init(false,1);
        for(unsigned bank=0;bank<256;bank++){
            for(int slot=0;slot<3;slot++)sms_write8((uint16_t)(0xfffd+slot),(uint8_t)(bank+slot));
            for(unsigned a=0;a<65536;a+=17){
                if(a>=0xc000){sms_write8((uint16_t)a,(uint8_t)a);if(sms_read8((uint16_t)(a^0x2000))!=(uint8_t)a)return 1;}
                else{
                    unsigned slot=a>>14,selected=(bank+slot)&255;
                    unsigned off=a<1024?a:(selected*16384+(a&16383))%size;
                    unsigned expected=(off*13+(off>>8)*17+(off>>14)*31)&255;
                    if(sms_read8((uint16_t)a)!=expected){fprintf(stderr,"size=%u bank=%u addr=%04x\n",size,bank,a);return 1;}
                }
            }
        }
    }
    remove(argv[1]);puts("banked ROM, fixed 1KiB, RAM mirrors, odd ROM sizes and reload pass");return 0;
}
