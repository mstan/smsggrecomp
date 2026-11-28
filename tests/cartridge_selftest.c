#undef NDEBUG
#include <assert.h>
#include "../common/sms_cartridge.h"
int main(void){
    uint8_t *raw=(uint8_t*)calloc(1,32768+512);assert(raw);
    memcpy(raw+512+0x7ff0,"TMR SEGA",8);
    SmsCartridge a,b;
    assert(sms_cartridge_view(raw+512,32768,&a));
    assert(sms_cartridge_view(raw,32768+512,&b));
    assert(a.offset==0&&b.offset==512&&a.size==b.size&&a.crc32==b.crc32);
    assert(a.original_crc32!=b.original_crc32&&sms_cartridge_has_header(&b));
    assert(sms_cartridge_original_extent(&b,512+0x7ff0,8)==b.data+0x7ff0);
    assert(!sms_cartridge_original_extent(&b,b.original_size-1,2));
    assert(!sms_cartridge_original_extent(&b,SIZE_MAX,1));
    assert(!sms_cartridge_view(raw,0,&a));assert(!sms_cartridge_view(raw,8191,&a));
    assert(!sms_cartridge_view(raw,32767,&a));
    assert(!sms_cartridge_view(raw,SMS_CARTRIDGE_MAX+16384,&a));
    FILE *file=fopen("cartridge-fixture.sms","wb");assert(file);
    assert(fwrite(raw,1,32768+512,file)==32768+512);assert(!fclose(file));
    assert(sms_cartridge_read("cartridge-fixture.sms",&a));
    assert(a.offset==512&&a.crc32==b.crc32&&!memcmp(a.original,raw,a.original_size));
    sms_cartridge_release(&a);assert(!remove("cartridge-fixture.sms"));
    assert(!sms_cartridge_read("missing-cartridge.sms",&a));
    free(raw);return 0;
}
