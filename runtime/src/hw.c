/* Hardware registers seen through mmio_* accesses (the lifter emits those when
 * the base register is a known 0xCCxxxxxx/0xCDxxxxxx constant).
 *
 * Values live in the scratch page that unmapped guest pages point at, so
 * accesses the lifter could not see statically still read back what was
 * written. Registers with side effects are handled here. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "platform.h"

/* PI interrupt causes */
#define PI_RSW      0x00000002u
#define PI_EXI      0x00000010u
#define PI_AI       0x00000020u
#define PI_DSP      0x00000040u
#define PI_VI       0x00000100u
#define PI_PE_TOKEN 0x00000200u
#define PI_PE_FINISH 0x00000400u
#define PI_WII_IPC  0x00004000u
#define PI_RSWST    0x00010000u   /* reset switch state: 1 = released */

static uint32_t s_pi_mask;
static volatile long s_pe_ctrl;     /* PE interrupt control/status (0xCC00100A) */
static volatile uint16_t s_pe_token;
static uint16_t s_vi_di[4][2];      /* display interrupt registers */
static uint64_t s_next_retrace_us;
static uint32_t s_retrace_count;
static uint32_t s_fifo_base, s_fifo_end, s_fifo_wptr;
static uint32_t s_unknown_logged;
static uint32_t s_stat_gx_bytes, s_stat_draw_done, s_stat_tokens, s_stat_vi_irq;

#define FRAME_US 16683               /* NTSC 59.94 Hz */

static uint32_t reg_off(uint32_t a) { return a & 0xFFFF; }

static int is_pi(uint32_t a) { return (a & 0xFFFFF000u) == 0xCC003000u; }
static int is_vi(uint32_t a) { return (a & 0xFFFFFF00u) == 0xCC002000u; }
static int is_pe(uint32_t a) { return (a & 0xFFFFFF00u) == 0xCC001000u; }
static int is_cp(uint32_t a) { return (a & 0xFFFFFF00u) == 0xCC000000u; }
static int is_wgpipe(uint32_t a) { return (a & ~7u) == 0xCC008000u; }
static int is_exi(uint32_t a) { return (a & 0xFF7FFF00u) == 0xCD006800u || (a & 0xFFFFFF00u) == 0xCC006800u; }
static int is_dsp(uint32_t a) { return (a & 0xFFFFFF00u) == 0xCC005000u; }

uint16_t dsp_read16(uint32_t off);
void dsp_write16(uint32_t off, uint16_t v);
uint32_t dsp_read32(uint32_t off);
void dsp_write32(uint32_t off, uint32_t v);
int dsp_interrupt_pending(void);
void dsp_update(void);

static int is_ai(uint32_t a) { return (a & 0xFF7FFFE0u) == 0xCD006C00u || (a & 0xFFFFFFE0u) == 0xCC006C00u; }

/* AI: the sample counter runs at 48 kHz while AICR.PSTAT (bit 0) is set */
static uint32_t s_aicr;
static uint64_t s_ai_start_us;
static uint32_t s_ai_base_count;

static uint32_t ai_sample_count(void)
{
    if (!(s_aicr & 1))
        return s_ai_base_count;
    return s_ai_base_count + (uint32_t)((rt_time_us() - s_ai_start_us) * 48000 / 1000000);
}

void hw_init(void)
{
    s_next_retrace_us = rt_time_us() + FRAME_US;
}

/* ------------------------------------------------------------------ VI */

static uint32_t vi_current_line(void)
{
    uint64_t now = rt_time_us();
    uint64_t into = FRAME_US - (s_next_retrace_us > now ? s_next_retrace_us - now : 0);
    return 1 + (uint32_t)(into * 525 / FRAME_US) % 525;
}

static void gx_publish(void);
static int s_gx_threaded;
static volatile uint64_t s_gx_busy_us;

void hw_update(void)
{
    if (s_gx_threaded > 0)
        gx_publish();
    dsp_update();
    uint64_t now = rt_time_us();
    if (now >= s_next_retrace_us) {
        s_next_retrace_us += FRAME_US;
        if (s_next_retrace_us < now)       /* we fell behind: don't burst */
            s_next_retrace_us = now + FRAME_US;
        s_retrace_count++;
        {
            static uint32_t frames;
            if (++frames == 120) {
                rt_time_adapt();
                frames = 0;
            }
        }
        if (s_vi_di[0][0] & 0x1000) {
            s_vi_di[0][0] |= 0x8000;
            s_stat_vi_irq++;
        }
        {
            /* no GX traffic for 5 s while the VI runs: the game is stuck waiting */
            static uint32_t last_bytes, stall;
            if (s_stat_gx_bytes != last_bytes) {
                last_bytes = s_stat_gx_bytes;
                stall = 0;
            } else if (++stall == 300) {
                extern void rt_guest_threads_dump(void);
                rt_log("no GX commands for 300 frames");
                rt_guest_threads_dump();
            }
        }
        if (s_retrace_count % 120 == 0 && s_gx_threaded > 0) {
            rt_log("[gx] %.2f ms per VI frame on the GX thread", s_gx_busy_us / 120000.0);
            s_gx_busy_us = 0;
        }
        if (s_retrace_count % 300 == 0)
            rt_log("[stats] frame %lu: vi irq %lu, gx %lu KiB, draw-done %lu, tokens %lu (last %04X)",
                   (unsigned long)s_retrace_count, (unsigned long)s_stat_vi_irq,
                   (unsigned long)(s_stat_gx_bytes >> 10), (unsigned long)s_stat_draw_done,
                   (unsigned long)s_stat_tokens, s_pe_token);
        if (!plat_frame())
            rt_fatal("quit requested after %lu frames", (unsigned long)s_retrace_count);
    }
}

static uint32_t pi_cause(void)
{
    uint32_t c = PI_RSWST;
    for (int i = 0; i < 4; i++)
        if ((s_vi_di[i][0] & 0x9000) == 0x9000)
            c |= PI_VI;
    if ((s_pe_ctrl & 0x5) == 0x5)
        c |= PI_PE_TOKEN;
    if ((s_pe_ctrl & 0xA) == 0xA)
        c |= PI_PE_FINISH;
    if (dsp_interrupt_pending())
        c |= PI_DSP;
    return c;
}

int hw_external_pending(void) { return (pi_cause() & s_pi_mask & ~PI_RSWST) != 0; }

uint32_t hw_frame_count(void) { return s_retrace_count; }
uint64_t hw_next_retrace_us(void) { return s_next_retrace_us; }

/* -------------------------------------------------------------- GX FIFO */

void gx_feed(const uint8_t *data, uint32_t n);

/* PE status bits are set by the GX thread and cleared by the guest */
#ifdef _MSC_VER
#include <intrin.h>
static void pe_set(long bits) { _InterlockedOr(&s_pe_ctrl, bits); }
static int pe_cas(long old, long v) { return _InterlockedCompareExchange(&s_pe_ctrl, v, old) == old; }
static void full_fence(void) { _mm_mfence(); }
#else
static void pe_set(long bits) { __atomic_fetch_or(&s_pe_ctrl, bits, __ATOMIC_SEQ_CST); }
static int pe_cas(long old, long v)
{
    return __atomic_compare_exchange_n(&s_pe_ctrl, &old, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static void full_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

/* GX thread: like the real GPU, the command processor (parsing, vertex
 * transform, texture decoding, batching for the host renderer) runs
 * alongside the CPU. The guest appends to a ring buffer; bytes are published
 * in chunks and at sync points (interrupt polls, PE/CP register reads). Draw
 * done and token interrupts are raised by the GX thread when it gets there.
 * NSMBW_GX_THREAD=0 runs it synchronously instead. */
#ifdef __3DS__
#define RING_SIZE (2u << 20)        /* a few tens of KiB per frame */
#else
#define RING_SIZE (8u << 20)
#endif
#define PUBLISH_CHUNK 2048
static uint8_t *s_ring;
static volatile uint32_t s_ring_w, s_ring_r;   /* published write / consumed read */
static uint32_t s_ring_wl;                     /* producer's local write position */
static volatile long s_gx_sleeping;
static plat_sem *s_gx_wake;

static void gx_publish(void)
{
    if (s_ring_wl == s_ring_w)
        return;
    full_fence();
    s_ring_w = s_ring_wl;
    full_fence();
    if (s_gx_sleeping) {
        s_gx_sleeping = 0;
        plat_sem_post(s_gx_wake);
    }
}

static void gx_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t r = s_ring_r, w = s_ring_w;
        if (r == w) {
            /* spin briefly, then sleep until the producer publishes */
            int spins = 0;
            while (s_ring_w == r && spins++ < 2000)
                plat_sleep_us(0);
            if (s_ring_w == r) {
                s_gx_sleeping = 1;
                full_fence();
                if (s_ring_w == r)
                    plat_sem_wait(s_gx_wake);
                s_gx_sleeping = 0;
            }
            continue;
        }
        full_fence();
        uint32_t n = w - r, off = r % RING_SIZE;
        if (n > RING_SIZE - off)
            n = RING_SIZE - off;
        if (n > 65536)
            n = 65536;
        uint64_t t0 = plat_time_us();
        gx_feed(s_ring + off, n);
        s_gx_busy_us += plat_time_us() - t0;
        full_fence();
        s_ring_r = r + n;
    }
}

static void gx_put(const uint8_t *b, uint32_t n)
{
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("NSMBW_GX_THREAD");
        s_gx_threaded = e ? atoi(e) != 0 : plat_cpu_count() > 1;
        if (s_gx_threaded) {
            s_ring = malloc(RING_SIZE);
            s_gx_wake = plat_sem_create();
            if (!s_ring || plat_gx_thread_start(gx_thread, NULL, 1024 * 1024) < 0)
                s_gx_threaded = 0;
        }
    }
    if (!s_gx_threaded) {
        gx_feed(b, n);
        return;
    }
    while (s_ring_wl + n - s_ring_r > RING_SIZE) {     /* full: let the GX thread catch up */
        gx_publish();
        plat_sleep_us(0);
    }
    for (uint32_t i = 0; i < n; i++)
        s_ring[(s_ring_wl + i) % RING_SIZE] = b[i];
    s_ring_wl += n;
    if (s_ring_wl - s_ring_w >= PUBLISH_CHUNK)
        gx_publish();
}

/* wait until the GX thread has processed everything written so far */
void gx_sync(void)
{
    if (s_gx_threaded <= 0)
        return;
    gx_publish();
    while (s_ring_r != s_ring_wl)
        plat_sleep_us(0);
}

static uint32_t s_cp_base, s_cp_end;    /* GP (read side) FIFO, from CP registers */
static uint16_t s_cp_regs[0x40];

/* BP writes with side effects, called by the command processor (gx.c) */
void hw_bp_event(uint32_t reg, uint32_t val)
{
    switch (reg) {
    case 0x45:                      /* PE_DONE: draw done -> finish interrupt */
        if (val & 2) {
            s_stat_draw_done++;
            pe_set(0x8);
            rt_raise();
        }
        break;
    case 0x47:                      /* PE_TOKEN_INT */
        s_stat_tokens++;
        s_pe_token = (uint16_t)val;
        pe_set(0x4);
        rt_raise();
        break;
    case 0x48:                      /* PE_TOKEN */
        s_pe_token = (uint16_t)val;
        break;
    }
}

/* The write-gather pipe stores into the CPU FIFO (PI registers). The GPU only
 * consumes it when its FIFO is the same buffer ("linked"); while a display
 * list is being recorded the CPU FIFO points at the list instead. */
static int gp_linked(void)
{
    return s_cp_base == 0 || ((s_cp_base ^ s_fifo_base) & 0x1FFFFFE0u) == 0;
}

static void fifo_put(const uint8_t *b, uint32_t n)
{
    uint32_t base = s_fifo_base & 0x1BFFFFFFu, end = s_fifo_end & 0x1BFFFFFFu;
    for (uint32_t i = 0; i < n && base && end > base; i++) {
        uint32_t w = s_fifo_wptr & 0x1BFFFFFFu;    /* bit 26 is the wrap flag */
        if (w < base || w > end) {
            s_fifo_wptr = s_fifo_base;
            w = base;
        }
        st8(w, b[i]);
        s_fifo_wptr++;
        if (s_fifo_end && (s_fifo_wptr & 0x1BFFFFFFu) > (s_fifo_end & 0x1BFFFFFFu))
            s_fifo_wptr = s_fifo_base | 0x04000000u;     /* wrap flag */
    }
    s_stat_gx_bytes += n;
    if (gp_linked())
        gx_put(b, n);
}

void gx_fifo_write8(uint8_t v) { fifo_put(&v, 1); }

void gx_fifo_write16(uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    fifo_put(b, 2);
}

void gx_fifo_write32(uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fifo_put(b, 4);
}

/* ---------------------------------------------------------- register I/O */

static void unknown(const char *what, uint32_t a, uint32_t v)
{
    if (s_unknown_logged++ < 200)
        rt_log("mmio %s %08lX = %08lX", what, (unsigned long)a, (unsigned long)v);
}

static uint32_t reg_read32(uint32_t a)
{
    if (is_dsp(a))
        return dsp_read32(a & 0xFF);
    if (is_pi(a)) {
        switch (reg_off(a) & 0xFFF) {
        case 0x000: return pi_cause();
        case 0x004: return s_pi_mask;
        case 0x00C: return s_fifo_base;
        case 0x010: return s_fifo_end;
        case 0x014: return s_fifo_wptr;
        case 0x024: return 0;                 /* reset code: cold boot */
        case 0x02C: return 0x246500B1;        /* Flipper/Hollywood revision */
        }
    }
    if (is_vi(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (o == 0x2C)
            return vi_current_line() << 16 | 1;
        if (o >= 0x30 && o < 0x40) {
            int i = (o - 0x30) / 4;
            return (uint32_t)s_vi_di[i][0] << 16 | s_vi_di[i][1];
        }
    }
    if (is_exi(a) && (reg_off(a) & 0xF) == 0xC)
        return 0;                             /* transfer never busy */
    if (is_ai(a)) {
        switch (reg_off(a) & 0x1F) {
        case 0x00: return s_aicr;
        case 0x08: return ai_sample_count();
        }
    }
    return ld32(a);                           /* scratch-backed register file */
}

static uint32_t reg_read16(uint32_t a)
{
    if (is_dsp(a))
        return dsp_read16(a & 0xFF);
    if (is_vi(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (o == 0x2C) return vi_current_line();
        if (o == 0x2E) return 1;
        if (o >= 0x30 && o < 0x40)
            return s_vi_di[(o - 0x30) / 4][(o & 2) >> 1];
    }
    if (is_pe(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (s_gx_threaded > 0)
            gx_publish();
        if (o == 0x0A) return (uint16_t)s_pe_ctrl;
        if (o == 0x0E) return s_pe_token;
    }
    if (is_cp(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (o == 0x00) {                      /* CP status: GP idle once the GX thread caught up */
            gx_sync();
            return 0x000C;
        }
    }
    if (is_pi(a) || is_exi(a))
        return reg_read32(a & ~3u) >> ((a & 2) ? 0 : 16) & 0xFFFF;
    return ld16(a);
}

static void reg_write32(uint32_t a, uint32_t v)
{
    if (is_dsp(a)) {
        dsp_write32(a & 0xFF, v);
        return;
    }
    st32(a, v);
    if (is_pi(a)) {
        switch (reg_off(a) & 0xFFF) {
        case 0x004: s_pi_mask = v; rt_raise(); return;
        case 0x00C: s_fifo_base = v; return;
        case 0x010: s_fifo_end = v; return;
        case 0x014: s_fifo_wptr = v & 0x3FFFFFFF; return;
        }
        return;
    }
    if (is_vi(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (o >= 0x30 && o < 0x40) {
            int i = (o - 0x30) / 4;
            uint16_t hi = (uint16_t)(v >> 16);
            s_vi_di[i][0] = (hi & 0x7FFF) | (s_vi_di[i][0] & hi & 0x8000);
            s_vi_di[i][1] = (uint16_t)v;
        }
        return;
    }
    if (is_exi(a) && (reg_off(a) & 0xF) == 0xC) {
        st32(a, v & ~1u);                     /* transfer completes instantly */
        st32((a & ~0xFu), ld32(a & ~0xFu) | 0x8);   /* CSR: TCINT */
        return;
    }
    if (is_ai(a)) {
        switch (reg_off(a) & 0x1F) {
        case 0x00: {
            uint32_t was = s_aicr & 1;
            if (v & 0x20)                     /* SCRESET */
                s_ai_base_count = 0;
            else if (was && !(v & 1))
                s_ai_base_count = ai_sample_count();
            if (!was && (v & 1))
                s_ai_start_us = rt_time_us();
            if (v & 0x20)
                s_ai_start_us = rt_time_us();
            s_aicr = v & ~0x20u & ~0x8u;      /* SCRESET self-clears, AIINT never set */
            return;
        }
        case 0x08:
            s_ai_base_count = v;
            s_ai_start_us = rt_time_us();
            return;
        }
    }
}

static void reg_write16(uint32_t a, uint16_t v)
{
    if (is_dsp(a)) {
        dsp_write16(a & 0xFF, v);
        return;
    }
    if (is_vi(a)) {
        uint32_t o = reg_off(a) & 0xFF;
        if (o >= 0x30 && o < 0x40) {
            int i = (o - 0x30) / 4;
            if (o & 2) {
                s_vi_di[i][1] = v;
            } else {
                s_vi_di[i][0] = (v & 0x7FFF) | (s_vi_di[i][0] & v & 0x8000);
            }
            return;
        }
    }
    if (is_cp(a)) {
        uint32_t o = (reg_off(a) & 0x7F) >> 1;
        s_cp_regs[o] = v;
        s_cp_base = (uint32_t)s_cp_regs[0x22 >> 1] << 16 | s_cp_regs[0x20 >> 1];
        s_cp_end = (uint32_t)s_cp_regs[0x26 >> 1] << 16 | s_cp_regs[0x24 >> 1];
        st16(a, v);
        return;
    }
    if (is_pe(a) && (reg_off(a) & 0xFF) == 0x0A) {
        /* bits 2/3 are write-1-to-clear status, bits 0/1 enables */
        for (;;) {
            long old = s_pe_ctrl;
            if (pe_cas(old, (long)((v & 0x3) | (old & 0xC & ~(v & 0xC)))))
                break;
        }
        rt_raise();
        return;
    }
    if (is_pi(a) || is_exi(a)) {
        uint32_t w = reg_read32(a & ~3u);
        w = (a & 2) ? (w & 0xFFFF0000u) | v : (w & 0xFFFFu) | (uint32_t)v << 16;
        reg_write32(a & ~3u, w);
        return;
    }
    st16(a, v);
}

/* ---------------------------------------------------------------- mmio_* */

uint32_t mmio_ld8(uint32_t a)  { return ld8(a); }
uint32_t mmio_ld16(uint32_t a) { return reg_read16(a); }
uint32_t mmio_ld32(uint32_t a) { return reg_read32(a); }
uint64_t mmio_ld64(uint32_t a) { return (uint64_t)reg_read32(a) << 32 | reg_read32(a + 4); }
double mmio_ldf32(uint32_t a)  { unknown("ldf32", a, 0); return ldf32(a); }

void mmio_st8(uint32_t a, uint8_t v)
{
    if (is_wgpipe(a))
        gx_fifo_write8(v);
    else
        st8(a, v);
}

void mmio_st16(uint32_t a, uint16_t v)
{
    if (is_wgpipe(a))
        gx_fifo_write16(v);
    else
        reg_write16(a, v);
}

void mmio_st32(uint32_t a, uint32_t v)
{
    if (is_wgpipe(a))
        gx_fifo_write32(v);
    else
        reg_write32(a, v);
}

void mmio_st64(uint32_t a, uint64_t v)
{
    mmio_st32(a, (uint32_t)(v >> 32));
    mmio_st32(a + 4, (uint32_t)v);
}

void mmio_stf32(uint32_t a, double v)
{
    float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, 4);
    mmio_st32(a, u);
}
