/* Build with SMS_GPU and SMS_VDP_REFERENCE: the scalar reference must actually
 * reach presentation, even on a machine with working OpenGL. */
#include "host_sdl.h"
#include <stdio.h>
int main(void){
    host_set_hidden(true);host_set_internal_scale(16);
    if(!host_init(256,192,0,0,256,192,1,"Reference presentation test"))return 2;
    if(host_uses_gpu()){fputs("reference incorrectly selected GPU jobs\n",stderr);return 1;}
    host_report(stdout);host_shutdown();
    puts("reference build presents native scalar pixels");return 0;
}
