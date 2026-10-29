/* DSP (0xCC005000) emulated at the mailbox level, plus the AI DMA that feeds
 * audio out.
 *
 *  - Boot: after reset the "ROM" mails 0x8071FEED; the CPU then sends five
 *    (0x80F3xxxx, value) pairs describing the microcode (DSP task). We don't
 *    run DSP code: once booted, the HLE AX microcode answers with 0xDCD10000.
 *  - AX: every 5 ms the AI DMA interrupt makes AX send 0xBABE0080 + the
 *    address of a command list; ax_hle.c executes it (mixing into the next AI
 *    buffer) and the "microcode" yields with 0xDCD10002, which the task
 *    manager answers (0xCDD10003) by calling AX's frame-done callback.
 *  - AI DMA: a buffer of 32 kHz stereo s16 (big endian) is played every
 *    len*32/4 samples; each period raises AIDINT and goes to the host. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "platform.h"

#define CSR_RES       0x0001
#define CSR_PIINT     0x0002
#define CSR_HALT      0x0004
#define CSR_AIDINT    0x0008
#define CSR_AIDINTMSK 0x0010
#define CSR_ARINT     0x0020
#define CSR_ARINTMSK  0x0040
#define CSR_DSPINT    0x0080
#define CSR_DSPINTMSK 0x0100
#define CSR_INT_BITS  (CSR_AIDINT | CSR_ARINT | CSR_DSPINT)

void ax_process_cmdlist(uint32_t addr);   /* ax_hle.c */

enum { DSP_OFF, DSP_ROM, DSP_UCODE };
enum { AX_WAIT_SIZE, AX_WAIT_ADDR, AX_WAIT_TASK };
static int s_ax_state = AX_WAIT_SIZE;

static uint16_t s_csr = CSR_HALT;
static int s_state = DSP_OFF;
static uint32_t s_out[64];                /* DSP -> CPU mail queue */
static uint8_t s_out_irq[64];             /* raise DSPINT when this mail reaches the head */
static int s_out_head, s_out_tail;
static uint16_t s_in_hi;                  /* CPU -> DSP, high half of a mail being written */
static uint32_t s_in_mails[16];
static int s_in_count;

/* AI DMA */
static uint16_t s_dma_hi, s_dma_lo, s_dma_ctrl;
static uint32_t s_dma_cur, s_dma_left;
static uint64_t s_next_aid_us;
static int s_debug = -1;

void dsp_push_mail(uint32_t m, int interrupt)
{
    int was_empty = s_out_head == s_out_tail;
    int next = (s_out_tail + 1) % 64;
    if (next == s_out_head)
        return;
    s_out[s_out_tail] = m;
    s_out_irq[s_out_tail] = (uint8_t)interrupt;
    s_out_tail = next;
    /* one interrupt per mail, raised when it is the one the CPU will read */
    if (interrupt && was_empty) {
        s_csr |= CSR_DSPINT;
        rt_raise();
    }
}

static int out_pending(void) { return s_out_head != s_out_tail; }

static void dsp_reset(void)
{
    s_out_head = s_out_tail = 0;
    s_in_count = 0;
    s_ax_state = AX_WAIT_SIZE;
    s_state = DSP_ROM;
    dsp_push_mail(0x8071FEED, 0);             /* boot ROM ready */
}

static void received_mail(uint32_t m)
{
    if (s_debug < 0)
        s_debug = getenv("NSMBW_DEBUG_DSP") != NULL;
    if (s_debug)
        rt_log("DSP <- %08lX", (unsigned long)m);
    if (s_state == DSP_ROM) {
        /* five (command, value) pairs, the last starting the microcode */
        s_in_mails[s_in_count++] = m;
        if (s_in_count == 10) {
            s_in_count = 0;
            s_state = DSP_UCODE;
            s_ax_state = AX_WAIT_SIZE;
            dsp_push_mail(0xDCD10000, 1);      /* DSP_INIT */
        }
        return;
    }
    switch (s_ax_state) {
    case AX_WAIT_SIZE:
        if ((m >> 16) == 0xBABE)
            s_ax_state = AX_WAIT_ADDR;
        break;
    case AX_WAIT_ADDR:
        ax_process_cmdlist(m);
        dsp_push_mail(0xDCD10002, 1);          /* DSP_YIELD: frame done */
        s_ax_state = AX_WAIT_TASK;
        break;
    case AX_WAIT_TASK:
        switch (m | 0xCDD10000u) {
        case 0xCDD10000:                       /* MAIL_RESUME */
            dsp_push_mail(0xDCD10001, 1);      /* DSP_RESUME */
            s_ax_state = AX_WAIT_SIZE;
            break;
        case 0xCDD10002:                       /* MAIL_RESET: back to the boot ROM */
            dsp_reset();
            break;
        default:                               /* MAIL_CONTINUE, MAIL_NEW_UCODE */
            s_ax_state = AX_WAIT_SIZE;
            break;
        }
        break;
    }
}

/* ---------------------------------------------------------- registers */

uint16_t dsp_read16(uint32_t off)
{
    switch (off & 0xFF) {
    case 0x00: return s_in_hi & 0x7FFF;                         /* mail to DSP: already taken */
    case 0x02: return 0;
    case 0x04: return out_pending() ? (uint16_t)(0x8000 | (s_out[s_out_head] >> 16)) : 0;
    case 0x06: {
        if (!out_pending())
            return 0;
        uint16_t lo = (uint16_t)s_out[s_out_head];
        if (s_debug > 0)
            rt_log("DSP -> %08lX", (unsigned long)s_out[s_out_head]);
        s_out_head = (s_out_head + 1) % 64;
        if (s_out_head != s_out_tail && s_out_irq[s_out_head]) {
            s_csr |= CSR_DSPINT;
            rt_raise();
        }
        return lo;
    }
    case 0x0A: return s_csr;
    case 0x30: return s_dma_hi;
    case 0x32: return s_dma_lo;
    case 0x36: return s_dma_ctrl;
    case 0x3A: return (uint16_t)(s_dma_left / 32);
    default: return 0;
    }
}

void dsp_write16(uint32_t off, uint16_t v)
{
    switch (off & 0xFF) {
    case 0x00:
        s_in_hi = v;
        break;
    case 0x02:
        if (s_state != DSP_OFF)
            received_mail((uint32_t)(s_in_hi & 0x7FFF) << 16 | v | ((uint32_t)(s_in_hi & 0x8000) << 16));
        s_in_hi &= 0x7FFF;
        break;
    case 0x0A: {
        if (s_debug < 0) s_debug = getenv("NSMBW_DEBUG_DSP") != NULL;
        if (s_debug > 0) rt_log("DSP CSR <- %04X (was %04X)", v, s_csr);
        uint16_t clear = v & CSR_INT_BITS;
        int was_halted = s_csr & CSR_HALT;
        s_csr = (uint16_t)((s_csr & CSR_INT_BITS & ~clear) | (v & ~CSR_INT_BITS & ~CSR_RES));
        /* the DSP starts (boot ROM) on reset or when released from halt */
        if ((v & CSR_RES) || (was_halted && !(v & CSR_HALT)))
            dsp_reset();
        rt_raise();
        break;
    }
    case 0x30: s_dma_hi = v; break;
    case 0x32: s_dma_lo = v; break;
    case 0x36: {
        int was = s_dma_ctrl & 0x8000;
        s_dma_ctrl = v;
        if (!was && (v & 0x8000)) {       /* DMA started */
            s_dma_cur = ((uint32_t)s_dma_hi << 16 | s_dma_lo) & 0x03FFFFE0u;
            s_dma_left = (uint32_t)(v & 0x7FFF) * 32;
            s_next_aid_us = rt_time_us() + 5000;
        }
        break;
    }
    }
}

uint32_t dsp_read32(uint32_t off) { return (uint32_t)dsp_read16(off) << 16 | dsp_read16(off + 2); }

void dsp_write32(uint32_t off, uint32_t v)
{
    dsp_write16(off, (uint16_t)(v >> 16));
    dsp_write16(off + 2, (uint16_t)v);
}

int dsp_interrupt_pending(void)
{
    return ((s_csr & CSR_AIDINT) && (s_csr & CSR_AIDINTMSK)) ||
           ((s_csr & CSR_ARINT) && (s_csr & CSR_ARINTMSK)) ||
           ((s_csr & CSR_DSPINT) && (s_csr & CSR_DSPINTMSK));
}

/* ------------------------------------------------------------- AI DMA */

void dsp_update(void)
{
    if (!(s_dma_ctrl & 0x8000))
        return;
    uint64_t now = rt_time_us();
    if (now < s_next_aid_us)
        return;
    uint32_t len = (uint32_t)(s_dma_ctrl & 0x7FFF) * 32;
    uint32_t samples = len / 4;                         /* stereo s16 frames */
    uint64_t period = (uint64_t)samples * 1000000 / 32000;
    if (!period)
        period = 5000;
    s_next_aid_us += period;
    if (s_next_aid_us < now)                            /* fell behind: don't burst */
        s_next_aid_us = now + period;
    /* play the current buffer, then latch the (possibly new) start address */
    uint32_t addr = ((uint32_t)s_dma_hi << 16 | s_dma_lo) & 0x03FFFFE0u;
    if (len && len <= 0x4000) {
        /* the AI plays (right, left) pairs; the host wants (left, right) */
        static uint8_t lr[0x4000];
        const uint8_t *src = (const uint8_t *)guest_ptr(addr | 0x80000000u);
        for (uint32_t i = 0; i < samples; i++) {
            memcpy(lr + i * 4, src + i * 4 + 2, 2);
            memcpy(lr + i * 4 + 2, src + i * 4, 2);
        }
        plat_audio_submit(lr, samples);
        /* peak meter for debugging (NSMBW_DEBUG_DSP) */
        static int peak, blocks;
        for (uint32_t i = 0; i < samples * 2; i++) {
            int v = (int16_t)(lr[i * 2] << 8 | lr[i * 2 + 1]);
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        if (++blocks >= 1000) {
            if (s_debug > 0)
                rt_log("[audio] peak %d over %d blocks", peak, blocks);
            peak = blocks = 0;
        }
    }
    s_dma_cur = addr;
    s_dma_left = len;
    s_csr |= CSR_AIDINT;
    rt_raise();
}
