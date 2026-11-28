/* Bounded, read-only SMS/GG original-image view. Runtime overlays use a
 * separate writable copy of data; identities always describe original bytes.
 * No footer/region/mapper policy is inferred here: rom_parser owns that. */
#ifndef SMS_CARTRIDGE_H
#define SMS_CARTRIDGE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SMS_CARTRIDGE_MAX (16u * 1024u * 1024u)
typedef struct SmsCartridge {
    uint8_t *original;
    size_t original_size, offset, size;
    const uint8_t *data;
    uint32_t original_crc32, crc32;
} SmsCartridge;
static uint32_t sms_cartridge_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i=0;i<n;i++) {
        c ^= p[i];
        for (int bit=0;bit<8;bit++) c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return ~c;
}
/* The existing compiler profile strips a 512-byte copier prefix only when
 * the remaining image is a whole number of 16 KiB banks. 8 KiB carts remain
 * valid without a prefix. Reject partial images and unbounded allocations. */
static int sms_cartridge_view(const uint8_t *bytes, size_t size, SmsCartridge *out) {
    memset(out,0,sizeof *out);
    size_t offset=size%16384u==512u?512u:0u;
    if (!bytes || size<offset+8192u || size-offset>SMS_CARTRIDGE_MAX ||
        (size-offset)%8192u) return 0;
    out->original_size=size;out->offset=offset;out->size=size-offset;
    out->data=bytes+offset;
    out->original_crc32=sms_cartridge_crc32(bytes,size);
    out->crc32=offset?sms_cartridge_crc32(out->data,out->size):out->original_crc32;
    return 1;
}
static int sms_cartridge_read(const char *path, SmsCartridge *out) {
    memset(out,0,sizeof *out);
    FILE *file=fopen(path,"rb");if(!file)return 0;
    if(fseek(file,0,SEEK_END)){fclose(file);return 0;}
    long length=ftell(file);
    if(length<8192 || (unsigned long)length>SMS_CARTRIDGE_MAX+512u ||
       fseek(file,0,SEEK_SET)){fclose(file);return 0;}
    uint8_t *bytes=(uint8_t*)malloc((size_t)length);
    if(!bytes){fclose(file);return 0;}
    size_t count=fread(bytes,1,(size_t)length,file);int closed=fclose(file);
    if(count!=(size_t)length || closed || !sms_cartridge_view(bytes,count,out)){
        free(bytes);return 0;
    }
    out->original=bytes;return 1;
}
static void sms_cartridge_release(SmsCartridge *image) {
    free(image->original);memset(image,0,sizeof *image);
}
static int sms_cartridge_has_header(const SmsCartridge *image) {
    const size_t offsets[]={0x7ff0u,0x3ff0u,0x1ff0u};
    for(size_t i=0;i<3;i++)if(offsets[i]+16u<=image->size &&
        !memcmp(image->data+offsets[i],"TMR SEGA",8))return 1;
    return 0;
}
/* Version-1 donor offsets address the original file, including its prefix.
 * Keeping this explicit preserves existing descriptor identities/offsets. */
static const uint8_t *sms_cartridge_original_extent(const SmsCartridge *image,
                                                    size_t offset,size_t size) {
    if(offset>image->original_size || size>image->original_size-offset)return NULL;
    return image->data-image->offset+offset;
}
#endif
