/*
 * rom_parser.h — Sega Master System / Game Gear ROM parsing.
 *
 * SMS/GG ROMs are raw Z80 images (no iNES-style header). Identity lives in a
 * 16-byte footer signed "TMR SEGA", placed near the end of the first 32 KB:
 * offset 0x7FF0 (>=32 KB ROMs), 0x3FF0 (16 KB), or 0x1FF0 (8 KB). Some early /
 * homebrew / Codemasters carts have no SEGA footer; those are still valid.
 *
 * The Z80 has FIXED entry vectors (not a vector table like the 6502): the CPU
 * begins at PC=0x0000 on reset, RST n traps to 0x00/08/10/18/20/28/30/38, the
 * maskable IRQ (IM 1) vectors to 0x0038, and the NMI (SMS PAUSE button) to
 * 0x0066. The function finder seeds from these.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SMS_BANK_SIZE   0x4000u   /* 16 KB paging granularity */
#define SMS_RAM_SIZE    0x2000u   /* 8 KB system RAM ($C000-$DFFF, mirror $E000) */
#define SMS_HEADER_SIZE 16

/* Fixed Z80 entry vectors (PC values, not table addresses). */
#define Z80_RESET_VECTOR 0x0000u
#define Z80_NMI_VECTOR   0x0066u
#define Z80_IRQ_VECTOR   0x0038u  /* IM 1 */
extern const uint16_t Z80_RST_VECTORS[8]; /* 0x00,08,10,18,20,28,30,38 */

typedef enum {
    SMS_REGION_UNKNOWN = 0,
    SMS_REGION_SMS_JAPAN   = 3,
    SMS_REGION_SMS_EXPORT  = 4,
    SMS_REGION_GG_JAPAN    = 5,
    SMS_REGION_GG_EXPORT   = 6,
    SMS_REGION_GG_INTL     = 7,
} SmsRegion;

typedef enum { SMS_PLATFORM_SMS = 0, SMS_PLATFORM_GG = 1 } SmsPlatform;

typedef enum {
    SMS_MAPPER_SEGA = 0,        /* standard: $FFFC-$FFFF frame registers */
    SMS_MAPPER_CODEMASTERS,     /* $0000/$4000/$8000 frame registers */
    SMS_MAPPER_KOREAN,          /* $A000 single frame register */
    SMS_MAPPER_NONE,            /* <=48 KB, no paging */
} SmsMapper;

typedef struct {
    uint8_t    *data;            /* raw ROM bytes (owned) */
    size_t      size;            /* file size in bytes */
    int         num_banks;       /* ceil(size / 16 KB) */
    uint32_t    crc32;           /* zlib CRC32 of the whole file */

    bool        has_header;      /* TMR SEGA footer present */
    int         header_offset;   /* 0x1FF0/0x3FF0/0x7FF0, or -1 */
    uint16_t    header_checksum; /* footer offset +0x0A (LE) */
    char        product_code[8]; /* decoded BCD product code, NUL-terminated */
    uint8_t     version;         /* low nibble of footer +0x0E */
    int         size_nibble;     /* low nibble of footer +0x0F (declared size code) */

    SmsRegion   region;
    SmsPlatform platform;
    SmsMapper   mapper;
} SmsRom;

/* Parse a .sms/.gg file. Returns false on read error. Always populates as
 * much as it can; missing footer is not an error (has_header=false). */
bool rom_parse(const char *path, SmsRom *out);
void rom_free(SmsRom *rom);

/* Read a byte by absolute ROM file offset (bounds-checked; 0 past end). */
uint8_t rom_read_offset(const SmsRom *rom, size_t offset);

/* Map a logical Z80 address to a ROM file offset given the bank mapped into
 * its 16 KB slot. slot_bank is the bank number programmed into the frame
 * register for the slot containing `addr`. Returns SIZE_MAX if `addr` is not
 * in a ROM-mapped region ($0000-$BFFF). The fixed first 1 KB ($0000-$03FF) of
 * the Sega mapper always reads bank 0 regardless of slot_bank. */
size_t rom_z80_to_offset(const SmsRom *rom, uint16_t addr, int slot_bank);

const char *rom_region_name(SmsRegion r);
const char *rom_mapper_name(SmsMapper m);
