#include "hook.h"
#include "log.h"
#include <string.h>
#include <orbis/libkernel.h>
#include <HDE64.h>          // decoder shipped with the GoldHEN Plugins SDK

#define JMP_ABS_LEN 14      // FF 25 00000000 <u64 address>
#define STUB_SIZE   0x100
#define PROT_RWX    7
#define MAP_PRIV_ANON (0x1000 | 0x2)

#define MAX_PATCHED 64
static struct { uint8_t *start; size_t len; } g_patched[MAX_PATCHED];
static int g_patched_count;

static void write_jmp_abs(uint8_t *at, const void *dest)
{
    at[0] = 0xFF; at[1] = 0x25;
    memset(at + 2, 0, 4);
    uint64_t d = (uint64_t)dest;
    memcpy(at + 6, &d, 8);
}

static int fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static uint8_t *follow_thunks(uint8_t *p)
{
    for (int i = 0; i < 4; i++) {
        if (p[0] == 0xE9) {                                  // jmp rel32
            int32_t rel; memcpy(&rel, p + 1, 4);
            p = p + 5 + rel;
        } else if (p[0] == 0xEB) {                           // jmp rel8
            p = p + 2 + (int8_t)p[1];
        } else if (p[0] == 0xFF && p[1] == 0x25) {           // jmp [rip+x]
            int32_t rel; memcpy(&rel, p + 2, 4);
            uint8_t *slot = p + 6 + rel;
            memcpy(&p, slot, 8);
        } else {
            break;
        }
    }
    return p;
}

// Allocates executable memory within +-2GB of near (so rel32 fixups fit).
static uint8_t *alloc_stub_near(uint8_t *near)
{
    for (int64_t dist = 0x1000000; dist < 0x70000000; dist += 0x4000000) {
        for (int dir = -1; dir <= 1; dir += 2) {
            uint64_t hint = ((uint64_t)near + dir * dist) & ~0x3FFFULL;
            void *mem = NULL;
            if (sceKernelMmap((void *)hint, STUB_SIZE, PROT_RWX, MAP_PRIV_ANON, -1, 0, &mem) < 0 || !mem)
                continue;
            if (fits_i32((int64_t)((uint8_t *)mem - near)))
                return mem;
            sceKernelMunmap(mem, STUB_SIZE);
        }
    }
    return NULL;
}

static int overlaps(uint8_t *t, size_t len)
{
    for (int i = 0; i < g_patched_count; i++)
        if (t < g_patched[i].start + g_patched[i].len && g_patched[i].start < t + len)
            return 1;
    return 0;
}

static void remember_patch(uint8_t *t, size_t len)
{
    if (g_patched_count < MAX_PATCHED) {
        g_patched[g_patched_count].start = t;
        g_patched[g_patched_count].len = len;
        g_patched_count++;
    }
}

// Export stubs like "E9 rel32" followed by NOP/INT3 padding: there is room to
// write the 14 byte jump right over the stub, and the original is simply the
// stub's jump target - nothing has to be copied or relocated.
static int try_patch_thunk(hook_rec *h, uint8_t *f, void *hook_fn)
{
    if (f[0] != 0xE9) return 0;
    for (int i = 5; i < JMP_ABS_LEN; i++)
        if (f[i] != 0x90 && f[i] != 0xCC) return 0;
    if (overlaps(f, JMP_ABS_LEN)) return 0;

    int32_t rel; memcpy(&rel, f + 1, 4);
    uint8_t *target = f + 5 + rel;
    memcpy(h->saved, f, JMP_ABS_LEN);
    if (sceKernelMprotect(f, JMP_ABS_LEN, PROT_RWX) < 0) return 0;
    uint8_t patch[JMP_ABS_LEN];
    write_jmp_abs(patch, hook_fn);
    memcpy(f, patch, JMP_ABS_LEN);

    h->target = f;
    h->orig = target;
    h->patched = JMP_ABS_LEN;
    remember_patch(f, JMP_ABS_LEN);
    return 1;
}

int hook_install(hook_rec *h, void *func, void *hook_fn, const char **why)
{
    memset(h, 0, sizeof(*h));
    *why = "";
    if (!func || !hook_fn) { *why = "null address"; return -1; }

    if (try_patch_thunk(h, (uint8_t *)func, hook_fn))
        return 0;

    uint8_t *t = follow_thunks((uint8_t *)func);

    // Decode enough whole instructions to cover the 14 byte jump.
    size_t len = 0;
    int needs_fixup = 0;
    while (len < JMP_ABS_LEN) {
        hde64s hs;
        unsigned n = hde64_disasm(t + len, &hs);
        if (hs.flags & F_ERROR)                      { *why = "undecodable prologue"; return -1; }
        if (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC)
                                                     { *why = "function shorter than 14 bytes"; return -1; }
        int riprel = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
        if (hs.flags & F_RELATIVE) {
            // only rel32 call/jmp/jcc can be relocated
            int rel32 = hs.opcode == 0xE8 || hs.opcode == 0xE9 ||
                        (hs.opcode == 0x0F && (hs.opcode2 & 0xF0) == 0x80);
            if (!rel32)                              { *why = "short branch in prologue"; return -1; }
            needs_fixup = 1;
        }
        if (riprel) needs_fixup = 1;
        len += n;
    }
    if (len > sizeof(h->saved))                      { *why = "prologue too long"; return -1; }

    if (overlaps(t, len))                            { *why = "overlaps another hook"; return -1; }

    uint8_t *stub = alloc_stub_near(t);
    if (!stub) {
        if (needs_fixup)                             { *why = "no memory near target"; return -1; }
        void *mem = NULL;
        if (sceKernelMmap(NULL, STUB_SIZE, PROT_RWX, MAP_PRIV_ANON, -1, 0, &mem) < 0 || !mem)
                                                     { *why = "mmap failed"; return -1; }
        stub = mem;
    }

    // Copy instructions one by one, fixing up anything relative to RIP.
    size_t off = 0;
    while (off < len) {
        hde64s hs;
        unsigned n = hde64_disasm(t + off, &hs);
        memcpy(stub + off, t + off, n);
        int64_t delta = (int64_t)(t - stub);          // same offset in both copies
        if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
            // disp32 sits after prefixes/opcode/modrm(/sib); immediates follow it.
            size_t imm = (hs.flags & F_IMM8) ? 1 : (hs.flags & F_IMM16) ? 2 :
                         (hs.flags & F_IMM32) ? 4 : (hs.flags & F_IMM64) ? 8 : 0;
            size_t dpos = n - imm - 4;
            int32_t d; memcpy(&d, stub + off + dpos, 4);
            int64_t nd = (int64_t)d + delta;
            if (!fits_i32(nd)) { sceKernelMunmap(stub, STUB_SIZE); *why = "rip-relative out of range"; return -1; }
            int32_t d32 = (int32_t)nd; memcpy(stub + off + dpos, &d32, 4);
        } else if (hs.flags & F_RELATIVE) {
            int32_t r; memcpy(&r, stub + off + n - 4, 4);
            int64_t nr = (int64_t)r + delta;
            if (!fits_i32(nr)) { sceKernelMunmap(stub, STUB_SIZE); *why = "branch out of range"; return -1; }
            int32_t r32 = (int32_t)nr; memcpy(stub + off + n - 4, &r32, 4);
        }
        off += n;
    }
    write_jmp_abs(stub + len, t + len);              // continue in the original

    // Patch the target.
    memcpy(h->saved, t, len);
    if (sceKernelMprotect(t, len, PROT_RWX) < 0) {
        sceKernelMunmap(stub, STUB_SIZE);
        *why = "mprotect failed";
        return -1;
    }
    uint8_t patch[32];
    memset(patch, 0x90, len);
    write_jmp_abs(patch, hook_fn);
    memcpy(t, patch, len);

    h->target = t;
    h->orig = stub;
    h->patched = len;
    remember_patch(t, len);
    return 0;
}

void hook_remove(hook_rec *h)
{
    if (!h->orig) return;
    sceKernelMprotect(h->target, h->patched, PROT_RWX);
    memcpy(h->target, h->saved, h->patched);
    // The stub is left mapped: another thread could still be executing it.
    h->orig = NULL;
}
