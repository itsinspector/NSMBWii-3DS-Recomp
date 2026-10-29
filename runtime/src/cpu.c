/* Out-of-line pieces of the guest CPU: indirect calls, SPRs, paired-single
 * quantization and traps. */
#include <stdio.h>
#include <string.h>
#include "runtime.h"

ppc_func_t lookup_function(uint32_t addr)
{
    uint32_t lo = 0, hi = g_ppc_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t a = g_ppc_funcs[mid].addr;
        if (a == addr)
            return g_ppc_funcs[mid].fn;
        if (a < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* generated: sorted address/name pairs */
typedef struct { uint32_t addr; const char *name; } sym_t;
extern const sym_t g_ppc_symbols[];
extern const uint32_t g_ppc_symbol_count;

const char *symbol_name(uint32_t addr, uint32_t *offset)
{
    uint32_t lo = 0, hi = g_ppc_symbol_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_ppc_symbols[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo || addr - g_ppc_symbols[lo - 1].addr > 0x10000) {
        if (offset)
            *offset = 0;
        return "?";
    }
    if (offset)
        *offset = addr - g_ppc_symbols[lo - 1].addr;
    return g_ppc_symbols[lo - 1].name;
}

uint32_t symbol_addr(const char *name)
{
    for (uint32_t i = 0; i < g_ppc_symbol_count; i++)
        if (!strcmp(g_ppc_symbols[i].name, name))
            return g_ppc_symbols[i].addr;
    return 0;
}

static const char *where(uint32_t addr)
{
    static char buf[2][160];
    static int k;
    uint32_t off;
    const char *n = symbol_name(addr, &off);
    k ^= 1;
    snprintf(buf[k], sizeof buf[k], "%08lX %s+0x%lX", (unsigned long)addr, n, (unsigned long)off);
    return buf[k];
}

void ppc_call_indirect(PPCContext *c, uint32_t target)
{
    ppc_func_t fn = lookup_function(target);
    if (!fn)
        rt_fatal("indirect call to unknown function %08lX (lr=%s)",
                 (unsigned long)target, where(c->lr));
    fn(c);
}

void ppc_unimplemented(PPCContext *c, uint32_t addr, uint32_t word)
{
    rt_fatal("unimplemented instruction %08lX at %s (lr=%s)", (unsigned long)word, where(addr), where(c->lr));
}

void ppc_fell_off(PPCContext *c, uint32_t addr)
{
    rt_fatal("execution fell off into %s (lr=%s)", where(addr), where(c->lr));
}

void ppc_trap(PPCContext *c, uint32_t addr)
{
    rt_fatal("trap at %s (lr=%s)", where(addr), where(c->lr));
}

void ppc_syscall(PPCContext *c, uint32_t addr)
{
    /* the SDK uses `sc` as a sync barrier (PPCSync); nothing to do */
    (void)c; (void)addr;
}

void ppc_rfi(PPCContext *c)
{
    rt_fatal("rfi reached outside HLE (lr=%s)", where(c->lr));
}

void ppc_mtmsr(PPCContext *c, uint32_t v) { c->msr = v; }

static uint32_t s_spr[1024];

uint32_t ppc_mfspr(PPCContext *c, int spr)
{
    (void)c;
    if (spr == 22)
        return rt_dec_read();
    return s_spr[spr & 1023];
}

/* Locked-cache DMA (DMAU = SPR 922, DMAL = SPR 923): NW4R computes matrices in
 * the locked cache and streams them to RAM with it. Completes immediately. */
static void lc_dma(uint32_t dmau, uint32_t dmal)
{
    uint32_t mem = dmau & ~31u, lc = dmal & ~31u;
    uint32_t blocks = (dmau & 31) << 2 | ((dmal >> 2) & 3);
    if (!blocks)
        blocks = 128;
    uint8_t buf[32];
    for (uint32_t i = 0; i < blocks; i++) {
        if ((dmal >> 4) & 1) {            /* load: memory -> cache */
            memcpy(buf, host_ptr(mem + i * 32), 32);
            memcpy(host_ptr(lc + i * 32), buf, 32);
        } else {                          /* store: cache -> memory */
            memcpy(buf, host_ptr(lc + i * 32), 32);
            memcpy(host_ptr(mem + i * 32), buf, 32);
        }
    }
}

void ppc_mtspr(PPCContext *c, int spr, uint32_t v)
{
    (void)c;
    if (spr == 22)
        rt_dec_write(v);
    if (spr == 923 && (v & 2)) {          /* DMAL with trigger bit */
        lc_dma(s_spr[922], v);
        v &= ~2u;
    }
    s_spr[spr & 1023] = v;
}

uint32_t ppc_mftb(PPCContext *c, int spr)
{
    (void)c;
    uint64_t tb = rt_ticks();
    return spr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

/* ---- paired-single quantized load/store ---- */
static const float s_dequant[64] = {
#define S(n) (1.0f / (float)(1ull << (n)))
    1.0f, S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
    S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31),
#undef S
#define L(n) ((float)(1ull << (32 - (n))))
    L(0), L(1), L(2), L(3), L(4), L(5), L(6), L(7), L(8), L(9), L(10), L(11), L(12), L(13), L(14), L(15),
    L(16), L(17), L(18), L(19), L(20), L(21), L(22), L(23), L(24), L(25), L(26), L(27), L(28), L(29), L(30), L(31),
#undef L
};

static double ps_load_one(uint32_t ea, int type, int scale)
{
    switch (type) {
    case 4: return (uint8_t)ld8(ea) * s_dequant[scale];
    case 5: return (uint16_t)ld16(ea) * s_dequant[scale];
    case 6: return (int8_t)ld8(ea) * s_dequant[scale];
    case 7: return (int16_t)ld16(ea) * s_dequant[scale];
    default: return ldf32(ea);
    }
}

static void ps_store_one(uint32_t ea, int type, int scale, double v)
{
    /* quantize scale is the dequant scale inverted */
    double q = v / s_dequant[scale];
    /* checked stores: GX matrix uploads psq_st straight into the write-gather pipe */
    switch (type) {
    case 4: chk_st8(ea, q < 0 ? 0 : q > 255 ? 255 : (uint8_t)q); break;
    case 5: chk_st16(ea, q < 0 ? 0 : q > 65535 ? 65535 : (uint16_t)q); break;
    case 6: chk_st8(ea, (uint8_t)(int8_t)(q < -128 ? -128 : q > 127 ? 127 : q)); break;
    case 7: chk_st16(ea, (uint16_t)(int16_t)(q < -32768 ? -32768 : q > 32767 ? 32767 : q)); break;
    default: chk_stf32(ea, v); break;
    }
}

static int ps_size(int type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

void ppc_psq_l(PPCContext *c, uint32_t ea, int w, int gqr, int frd)
{
    uint32_t g = c->gqr[gqr];
    int type = (g >> 16) & 7, scale = (g >> 24) & 63;
    double p0 = ps_load_one(ea, type, scale);
    double p1 = w ? 1.0 : ps_load_one(ea + ps_size(type), type, scale);
    c->f[frd].d = p0;
    c->ps1[frd] = p1;
}

void ppc_psq_st(PPCContext *c, uint32_t ea, int w, int gqr, int frs)
{
    uint32_t g = c->gqr[gqr];
    int type = g & 7, scale = (g >> 8) & 63;
    ps_store_one(ea, type, scale, c->f[frs].d);
    if (!w)
        ps_store_one(ea + ps_size(type), type, scale, c->ps1[frs]);
}

void ppc_lswi(PPCContext *c, uint32_t ea, int rd, int n)
{
    for (int i = 0; i < n; i++, ea++) {
        int r = (rd + i / 4) & 31, sh = 24 - 8 * (i & 3);
        if ((i & 3) == 0)
            c->r[r] = 0;
        c->r[r] |= ld8(ea) << sh;
    }
}

void ppc_stswi(PPCContext *c, uint32_t ea, int rs, int n)
{
    for (int i = 0; i < n; i++, ea++)
        st8(ea, c->r[(rs + i / 4) & 31] >> (24 - 8 * (i & 3)));
}
