/* Guest CPU state and memory accessors shared by recompiled code and runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef _MSC_VER
#include <stdlib.h>
#include <intrin.h>
#define bswap16(x) _byteswap_ushort(x)
#define bswap32(x) _byteswap_ulong(x)
#define bswap64(x) _byteswap_uint64(x)
static __forceinline uint32_t ppc_clz(uint32_t v) { unsigned long i; return _BitScanReverse(&i, v) ? 31 - i : 32; }
#define RT_INLINE static __forceinline
#else
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)
static inline uint32_t ppc_clz(uint32_t v) { return v ? __builtin_clz(v) : 32; }
#define RT_INLINE static inline __attribute__((always_inline))
#endif

typedef union { double d; uint64_t u; } fpr_t;
typedef struct { uint8_t lt, gt, eq, so; } crf_t;

typedef struct PPCContext {
    uint32_t r[32];
    fpr_t f[32];        /* ps0 / plain FPR */
    double ps1[32];     /* paired-single slot 1 */
    crf_t cr[8];
    uint32_t lr, ctr;
    uint32_t ca, so, ov; /* XER bits */
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t msr;
} PPCContext;

/* Guest address -> host pointer, 64 KiB pages. Unmapped pages (including the
 * hardware register ranges, for accesses the lifter couldn't see statically)
 * point at a small shared scratch page. */
extern uint8_t *g_memmap[65536];

RT_INLINE uint8_t *host_ptr(uint32_t a) { return g_memmap[a >> 16] + (a & 0xFFFF); }

RT_INLINE uint32_t ROTL32(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }

#ifdef _MSC_VER
/* x64 handles unaligned accesses; MSVC at /O1 would emit real memcpy calls */
#define RD(T, p) (*(const T __unaligned *)(p))
#define WR(T, p, v) (*(T __unaligned *)(p) = (v))
#else
#define RD(T, p) ({ T _v; memcpy(&_v, (p), sizeof(T)); _v; })
#define WR(T, p, v) do { T _v = (v); memcpy((p), &_v, sizeof(T)); } while (0)
#endif

RT_INLINE uint32_t ld8(uint32_t a)  { return *host_ptr(a); }
RT_INLINE uint32_t ld16(uint32_t a) { return bswap16(RD(uint16_t, host_ptr(a))); }
RT_INLINE uint32_t ld32(uint32_t a) { return bswap32(RD(uint32_t, host_ptr(a))); }
RT_INLINE uint64_t ld64(uint32_t a) { return bswap64(RD(uint64_t, host_ptr(a))); }
RT_INLINE uint32_t ld16_le(uint32_t a) { return RD(uint16_t, host_ptr(a)); }
RT_INLINE uint32_t ld32_le(uint32_t a) { return RD(uint32_t, host_ptr(a)); }
RT_INLINE void st8(uint32_t a, uint8_t v)   { *host_ptr(a) = v; }
RT_INLINE void st16(uint32_t a, uint16_t v) { WR(uint16_t, host_ptr(a), bswap16(v)); }
RT_INLINE void st32(uint32_t a, uint32_t v) { WR(uint32_t, host_ptr(a), bswap32(v)); }
RT_INLINE void st64(uint32_t a, uint64_t v) { WR(uint64_t, host_ptr(a), bswap64(v)); }
RT_INLINE void st16_le(uint32_t a, uint16_t v) { WR(uint16_t, host_ptr(a), v); }
RT_INLINE void st32_le(uint32_t a, uint32_t v) { WR(uint32_t, host_ptr(a), v); }

RT_INLINE double ldf32(uint32_t a) { uint32_t v = ld32(a); float f; memcpy(&f, &v, 4); return f; }
RT_INLINE void stf32(uint32_t a, double d) { float f = (float)d; uint32_t v; memcpy(&v, &f, 4); st32(a, v); }

/* Hardware register access (GX FIFO, VI, PI, ...) — implemented by the runtime. */
uint32_t mmio_ld8(uint32_t a);
uint32_t mmio_ld16(uint32_t a);
uint32_t mmio_ld32(uint32_t a);
uint64_t mmio_ld64(uint32_t a);
void mmio_st8(uint32_t a, uint8_t v);
void mmio_st16(uint32_t a, uint16_t v);
void mmio_st32(uint32_t a, uint32_t v);
void mmio_st64(uint32_t a, uint64_t v);
void mmio_stf32(uint32_t a, double v);
double mmio_ldf32(uint32_t a);

/* Checked accessors, used in functions that compute hardware addresses at
 * runtime (0xCC000000-0xCDFFFFFF go to the register emulation). */
#define IS_HW(a) (((a) >> 25) == (0xCC000000u >> 25))
RT_INLINE uint32_t chk_ld8(uint32_t a)  { return IS_HW(a) ? mmio_ld8(a) : ld8(a); }
RT_INLINE uint32_t chk_ld16(uint32_t a) { return IS_HW(a) ? mmio_ld16(a) : ld16(a); }
RT_INLINE uint32_t chk_ld32(uint32_t a) { return IS_HW(a) ? mmio_ld32(a) : ld32(a); }
RT_INLINE uint64_t chk_ld64(uint32_t a) { return IS_HW(a) ? mmio_ld64(a) : ld64(a); }
RT_INLINE void chk_st8(uint32_t a, uint8_t v)   { if (IS_HW(a)) mmio_st8(a, v); else st8(a, v); }
RT_INLINE void chk_st16(uint32_t a, uint16_t v) { if (IS_HW(a)) mmio_st16(a, v); else st16(a, v); }
RT_INLINE void chk_st32(uint32_t a, uint32_t v) { if (IS_HW(a)) mmio_st32(a, v); else st32(a, v); }
RT_INLINE void chk_st64(uint32_t a, uint64_t v) { if (IS_HW(a)) mmio_st64(a, v); else st64(a, v); }
RT_INLINE void chk_stf32(uint32_t a, double v)  { if (IS_HW(a)) mmio_stf32(a, v); else stf32(a, v); }
#define chk_ld16_le ld16_le
#define chk_ld32_le ld32_le
#define chk_st16_le st16_le
#define chk_st32_le st32_le

static inline void ppc_set_cr0(PPCContext *c, uint32_t v) {
    int32_t s = (int32_t)v;
    c->cr[0].lt = s < 0; c->cr[0].gt = s > 0; c->cr[0].eq = s == 0; c->cr[0].so = c->so;
}
static inline void ppc_cmp(PPCContext *c, int f, int32_t a, int32_t b) {
    c->cr[f].lt = a < b; c->cr[f].gt = a > b; c->cr[f].eq = a == b; c->cr[f].so = c->so;
}
static inline void ppc_cmpl(PPCContext *c, int f, uint32_t a, uint32_t b) {
    c->cr[f].lt = a < b; c->cr[f].gt = a > b; c->cr[f].eq = a == b; c->cr[f].so = c->so;
}
static inline void ppc_fcmp(PPCContext *c, int f, double a, double b) {
    int un = isnan(a) || isnan(b);
    c->cr[f].lt = !un && a < b; c->cr[f].gt = !un && a > b;
    c->cr[f].eq = !un && a == b; c->cr[f].so = un;
}
static inline void ppc_set_cr1(PPCContext *c) {
    uint32_t v = c->fpscr >> 28;
    c->cr[1].lt = v >> 3 & 1; c->cr[1].gt = v >> 2 & 1; c->cr[1].eq = v >> 1 & 1; c->cr[1].so = v & 1;
}
static inline uint32_t ppc_get_cr(PPCContext *c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 4 | c->cr[i].lt << 3 | c->cr[i].gt << 2 | c->cr[i].eq << 1 | c->cr[i].so;
    return v;
}
static inline void ppc_set_cr(PPCContext *c, uint32_t v, uint32_t fxm) {
    for (int i = 0; i < 8; i++) {
        if (!(fxm & (0x80 >> i))) continue;
        uint32_t n = v >> (28 - 4 * i);
        c->cr[i].lt = n >> 3 & 1; c->cr[i].gt = n >> 2 & 1; c->cr[i].eq = n >> 1 & 1; c->cr[i].so = n & 1;
    }
}
static inline uint32_t ppc_get_xer(PPCContext *c) { return c->so << 31 | c->ov << 30 | c->ca << 29; }
static inline void ppc_set_xer(PPCContext *c, uint32_t v) { c->so = v >> 31 & 1; c->ov = v >> 30 & 1; c->ca = v >> 29 & 1; }

static inline uint32_t ppc_sraw(PPCContext *c, uint32_t s, uint32_t n) {
    int32_t v = (int32_t)s;
    if (n & 0x20) { c->ca = v < 0; return v < 0 ? 0xFFFFFFFFu : 0; }
    n &= 31;
    c->ca = v < 0 && n && (s & ((1u << n) - 1));
    return (uint32_t)(v >> n);
}
static inline uint32_t ppc_divw(uint32_t a, uint32_t b) {
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return ((int32_t)a < 0 && b == 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint64_t ppc_fctiw(PPCContext *c, double d, int toward_zero) {
    int32_t r;
    if (isnan(d)) r = (int32_t)0x80000000;
    else if (d >= 2147483647.0) r = 0x7FFFFFFF;
    else if (d <= -2147483648.0) r = (int32_t)0x80000000;
    else r = toward_zero || (c->fpscr & 3) == 1 ? (int32_t)d : (int32_t)nearbyint(d);
    return 0xFFF8000000000000ull | (uint32_t)r;
}
static inline double ppc_frsqrte(double d) { return 1.0 / sqrt(d); }
static inline void ppc_mtfsf(PPCContext *c, uint32_t fm, uint32_t v) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) if (fm & (1 << i)) m |= 0xFu << (4 * i);
    c->fpscr = (c->fpscr & ~m) | (v & m);
}
static inline void ppc_mcrfs(PPCContext *c, int d, int s) {
    uint32_t n = c->fpscr >> (28 - 4 * s);
    c->cr[d].lt = n >> 3 & 1; c->cr[d].gt = n >> 2 & 1; c->cr[d].eq = n >> 1 & 1; c->cr[d].so = n & 1;
}
static inline void ppc_dcbz(uint32_t a) { memset(host_ptr(a & ~31u), 0, 32); }

/* Function-entry trace (build with -DRT_TRACE_ENABLED): ring buffer of the last
 * guest functions entered, dumped on fatal errors. */
#ifdef RT_TRACE_ENABLED
extern uint32_t g_trace_ring[1024];
extern uint32_t g_trace_pos;
#define RT_TRACE(a) (g_trace_ring[g_trace_pos++ & 1023] = (a))
#else
#define RT_TRACE(a) ((void)0)
#endif

/* Implemented by the runtime. */
extern volatile uint32_t g_rt_pending;     /* nonzero: an interrupt may be deliverable */
void rt_poll_slow(PPCContext *c);
jmp_buf *rt_save_context(PPCContext *c);  /* OSSaveContext call sites */
RT_INLINE void rt_poll(PPCContext *c) { if (g_rt_pending) rt_poll_slow(c); }
void ppc_call_indirect(PPCContext *c, uint32_t target);
void ppc_unimplemented(PPCContext *c, uint32_t addr, uint32_t word);
void ppc_fell_off(PPCContext *c, uint32_t addr);
void ppc_trap(PPCContext *c, uint32_t addr);
void ppc_syscall(PPCContext *c, uint32_t addr);
void ppc_rfi(PPCContext *c);
void ppc_mtmsr(PPCContext *c, uint32_t v);
uint32_t ppc_mfspr(PPCContext *c, int spr);
void ppc_mtspr(PPCContext *c, int spr, uint32_t v);
uint32_t ppc_mftb(PPCContext *c, int spr);
void ppc_psq_l(PPCContext *c, uint32_t ea, int w, int gqr, int frd);
void ppc_psq_st(PPCContext *c, uint32_t ea, int w, int gqr, int frs);
void ppc_lswi(PPCContext *c, uint32_t ea, int rd, int n);
void ppc_stswi(PPCContext *c, uint32_t ea, int rs, int n);

typedef void (*ppc_func_t)(PPCContext *c);
typedef struct { uint32_t addr; ppc_func_t fn; } ppc_func_entry_t;
extern const ppc_func_entry_t g_ppc_funcs[];
extern const uint32_t g_ppc_func_count;
