/*
 * game_spec.h - per-game function-pointer hooks (PRINCIPLES #21).
 *
 * Identity + entry/IRQ callbacks + lifecycle hooks the shared runner invokes.
 * Each game provides exactly one TU defining `const GameSpec g_game_spec`.
 *
 * The headless bring-up runner drives the game through the fixed Z80 vectors
 * via call_by_address (reset 0x0000, IM1 IRQ 0x0038, NMI 0x0066), so a full
 * spec is optional at this stage; this header defines the contract the SDL/
 * full runner will consume.
 */
#ifndef GAME_SPEC_H
#define GAME_SPEC_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { GAME_PLATFORM_SMS = 0, GAME_PLATFORM_GG = 1 } GamePlatform;

typedef struct GameSpec {
    /* identity */
    const char  *display_name;
    const char  *short_name;
    uint32_t     expected_rom_crc32;
    uint32_t     expected_rom_size;
    GamePlatform platform;

    /* entry points (recompiled C, resolved via call_by_address by default) */
    void (*on_post_reset)(void);          /* after ROM load + Z80 reset      */
    void (*on_frame_pre)(uint64_t frame); /* before the frame's VInt         */
    void (*on_frame_post)(uint64_t frame);/* after the frame's VInt          */

    /* optional dispatch override: handle/normalise a target before miss-log;
     * return non-zero if it consumed the address. */
    int  (*dispatch_override)(uint16_t addr);
} GameSpec;

extern const GameSpec g_game_spec;

#endif /* GAME_SPEC_H */
