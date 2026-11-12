// Small, careful inline hook for x86-64.
//
// Unlike a plain "copy 14 bytes" detour it:
//   * follows jmp thunks (sceXxx -> jmp real_impl) and hooks the real function,
//   * relocates RIP-relative and rel32 instructions it moves into the stub,
//   * refuses (instead of crashing) when the prologue cannot be moved safely,
//   * refuses when two hooks would overlap.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *target;     // function actually patched (after following thunks)
    void    *orig;       // callable original (stub), NULL if not hooked
    size_t   patched;    // bytes overwritten at target
    uint8_t  saved[32];  // original bytes, for unhook
} hook_rec;

// Returns 0 on success, otherwise a short reason in *why.
int  hook_install(hook_rec *h, void *func, void *hook_fn, const char **why);
void hook_remove(hook_rec *h);
