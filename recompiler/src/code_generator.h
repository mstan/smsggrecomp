/*
 * code_generator.h — Z80 -> C emission for the SMS/GG recompiler.
 *
 * Built execution-driven (PRINCIPLES #10/#15): cg_probe answers "what opcodes
 * /control-flow shapes does this ROM actually use?" before emission commits, so
 * coverage targets reality. cg_emit produces the generated C (no stubs, #12 —
 * an unhandled opcode is a hard generation error, never a silent stub).
 */
#pragma once
#include "rom_parser.h"
#include "function_finder.h"
#include "game_config.h"

/* Decode every discovered function; print an opcode/prefix/control-flow
 * coverage report. Emits nothing. */
void cg_probe(const SmsRom *rom, const FuncList *fl);

/* Translate every discovered function to C and write the generated TUs into
 * out_dir: <prefix>_full.c (function bodies), <prefix>_dispatch.c (address ->
 * function lookup), <prefix>_layout.c (g_game_layout), <prefix>_funcs.h.
 * No stubs: an untranslatable opcode aborts generation (PRINCIPLES #12). */
void cg_emit(const SmsRom *rom, const FuncList *fl, const GameConfig *cfg,
             const char *out_dir);
