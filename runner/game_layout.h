/*
 * game_layout.h - per-game RAM layout (PRINCIPLES #21).
 *
 * The recompiler emits <prefix>_layout.c defining `const GameLayout
 * g_game_layout` from [ram_layout] in game.toml. Shared runner code reads
 * g_game_layout instead of hardcoding per-game RAM addresses. Fields grow as
 * analysis resolves more of the layout; unset addresses are 0.
 */
#ifndef GAME_LAYOUT_H
#define GAME_LAYOUT_H

#include <stdint.h>

typedef struct {
    uint16_t game_mode;       /* game-mode/state byte (mode dispatch)   */
    uint16_t vblank_count;    /* frame counter incremented in the VInt  */
    uint16_t player_object;   /* player object block base               */
} GameLayout;

extern const GameLayout g_game_layout;

#endif /* GAME_LAYOUT_H */
