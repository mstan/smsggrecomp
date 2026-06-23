/* sljit_smoke.c — P0 gate: prove sljit builds and JITs on this toolchain.
 * Builds a leaf function `sljit_sw f(void){ return 42; }` at runtime, runs it,
 * asserts the result. Not part of any game build — a standalone bring-up test. */
#include "sljitLir.h"
#include <stdio.h>

int main(void){
    struct sljit_compiler *c = sljit_create_compiler(NULL);
    if (!c){ printf("FAIL: sljit_create_compiler returned NULL\n"); return 2; }

    /* sljit_sw f(void): 1 scratch reg, 0 saved, 0 locals; return immediate 42. */
    sljit_emit_enter(c, 0, SLJIT_ARGS0(W), 1, 0, 0);
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, 42);

    void *code = sljit_generate_code(c, 0, NULL);
    sljit_s32 err = sljit_get_compiler_error(c);
    sljit_free_compiler(c);
    if (!code){ printf("FAIL: sljit_generate_code returned NULL (err=%d)\n", err); return 2; }

    sljit_sw (*f)(void) = (sljit_sw (*)(void))code;
    sljit_sw r = f();
    printf("sljit smoke: f() = %ld (expect 42)\n", (long)r);

    int ok = (r == 42);
    sljit_free_code(code, NULL);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
