/* HLE of the Wii AX DSP microcode: executes AX command lists, mixing every
 * running voice (parameter block, "PB") into 3 ms of 32 kHz output.
 *
 * PBs are read and updated in place in guest memory (big-endian u16 words).
 * Two microcode generations exist: the old one has an extra 5-word "updates"
 * block in the PB and different command numbers; it is recognized from the
 * END command of the first command list. */
#include <stdlib.h>
#include "runtime.h"

#define SAMPLES 96                         /* 3 ms at 32 kHz */

void dsp_push_mail(uint32_t mail, int interrupt);

enum { BUF_MAIN_L, BUF_MAIN_R, BUF_MAIN_S, BUF_AUXA_L, BUF_AUXA_R, BUF_AUXA_S,
       BUF_AUXB_L, BUF_AUXB_R, BUF_AUXB_S, BUF_AUXC_L, BUF_AUXC_R, BUF_AUXC_S, NBUF };

static int32_t s_buf[NBUF][SAMPLES];
static uint16_t s_last_main_volume = 0x8000, s_last_aux_volume[3] = {0x8000, 0x8000, 0x8000};
static int s_old = -1;                     /* -1 unknown, 0 new, 1 old microcode */

/* ------------------------------------------------------------ helpers */

static inline uint32_t phys(uint32_t a) { return (a & 0x1FFFFFFF) | 0x80000000u; }
static inline uint16_t rw(uint32_t pb, int w) { return (uint16_t)ld16(pb + 2 * w); }
static inline void ww(uint32_t pb, int w, uint16_t v) { st16(pb + 2 * w, v); }
static inline uint32_t rl(uint32_t pb, int w) { return (uint32_t)rw(pb, w) << 16 | rw(pb, w + 1); }
static inline int16_t clamp16(int64_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* PB word offsets (new layout; old adds 5 after dpop) */
#define PB_NEXT        0
#define PB_SRC_TYPE    4
#define PB_COEF_SEL    5
#define PB_MIXCTRL     6
#define PB_RUNNING     8
#define PB_IS_STREAM   9
#define PB_MIXER       10                  /* 12 x (volume, delta) */
#define PB_DPOP        41
static int U(void) { return s_old == 1 ? 58 : 53; }   /* vol_env offset */
#define PB_VOL         (U() + 0)
#define PB_VOL_DELTA   (U() + 1)
#define PB_LOOPING     (U() + 2)
#define PB_FORMAT      (U() + 3)
#define PB_LOOP_ADDR   (U() + 4)
#define PB_END_ADDR    (U() + 6)
#define PB_CUR_ADDR    (U() + 8)
#define PB_COEFS       (U() + 10)
#define PB_GAIN        (U() + 26)
#define PB_PRED_SCALE  (U() + 27)
#define PB_YN1         (U() + 28)
#define PB_YN2         (U() + 29)
#define PB_RATIO       (U() + 30)
#define PB_FRAC        (U() + 32)
#define PB_LAST        (U() + 33)
#define PB_LOOP_PS     (U() + 37)
#define PB_LOOP_YN1    (U() + 38)
#define PB_LOOP_YN2    (U() + 39)
#define PB_LPF         (U() + 40)          /* on, yn1, a0, b0 */
#define PB_BIQUAD      (U() + 44)          /* on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2 */

/* mixer slots in PB order -> output buffer, and the mixer_control bits that
 * enable them (plain / with ramp) */
static const struct { int buf; uint32_t on, ramp; } k_mix[12] = {
    {BUF_MAIN_L, 0x00000001, 0x00000004}, {BUF_MAIN_R, 0x00000002, 0x00000004},
    {BUF_AUXA_L, 0x00010000, 0x00040000}, {BUF_AUXA_R, 0x00020000, 0x00040000},
    {BUF_AUXB_L, 0x00200000, 0x00800000}, {BUF_AUXB_R, 0x00400000, 0x00800000},
    {BUF_AUXC_L, 0x04000000, 0x10000000}, {BUF_AUXC_R, 0x08000000, 0x10000000},
    {BUF_MAIN_S, 0x00000008, 0x00000010}, {BUF_AUXA_S, 0x00080000, 0x00100000},
    {BUF_AUXB_S, 0x01000000, 0x02000000}, {BUF_AUXC_S, 0x20000000, 0x40000000},
};
/* dpop word for each mixer slot */
static const int k_dpop[12] = {0, 4, 1, 5, 2, 6, 3, 7, 8, 9, 10, 11};

/* --------------------------------------------------------- accelerator */

typedef struct {
    uint32_t pb;
    uint32_t start, end, cur;   /* in format units: nibbles (ADPCM), bytes, words */
    uint16_t format, pred_scale;
    int16_t yn1, yn2;
    int16_t coefs[16];
    int stopped;
} Acc;

static uint8_t mem8(uint32_t byte_addr) { return (uint8_t)ld8(phys(byte_addr)); }

static int16_t acc_read(Acc *a)
{
    int16_t val;
    uint32_t step;
    switch (a->format) {
    case 0x00: {                           /* ADPCM: 16 nibbles per frame, first byte is header */
        if ((a->cur & 15) == 0) {
            a->pred_scale = mem8(a->cur >> 1);
            a->cur += 2;
        }
        int32_t scale = 1 << (a->pred_scale & 0xF);
        int ci = (a->pred_scale >> 4) & 7;
        int32_t c1 = a->coefs[ci * 2], c2 = a->coefs[ci * 2 + 1];
        int32_t nib = (a->cur & 1) ? (mem8(a->cur >> 1) & 0xF) : (mem8(a->cur >> 1) >> 4);
        if (nib >= 8)
            nib -= 16;
        int32_t v = scale * nib + ((0x400 + c1 * a->yn1 + c2 * a->yn2) >> 11);
        val = clamp16(v);
        a->yn2 = a->yn1;
        a->yn1 = val;
        a->cur += 1;
        step = 2;
        break;
    }
    case 0x0A:                             /* PCM16 */
        val = (int16_t)(mem8(a->cur * 2) << 8 | mem8(a->cur * 2 + 1));
        a->yn2 = a->yn1;
        a->yn1 = val;
        a->cur += 1;
        step = 2;
        break;
    case 0x19:                             /* PCM8 */
        val = (int16_t)(mem8(a->cur) << 8);
        a->yn2 = a->yn1;
        a->yn1 = val;
        a->cur += 1;
        step = 1;
        break;
    default:
        return 0;
    }
    if (a->cur == a->end + step - 1) {    /* end of sample */
        a->cur = a->start;
        if (rw(a->pb, PB_LOOPING)) {
            a->pred_scale = rw(a->pb, PB_LOOP_PS);
            if (rw(a->pb, PB_IS_STREAM) != 1) {
                a->yn1 = (int16_t)rw(a->pb, PB_LOOP_YN1);
                a->yn2 = (int16_t)rw(a->pb, PB_LOOP_YN2);
            }
        } else {
            ww(a->pb, PB_RUNNING, 0);
            a->stopped = 1;
        }
    }
    return val;
}

/* ---------------------------------------------------------------- voice */

static void process_voice(uint32_t pb)
{
    if (rw(pb, PB_RUNNING) != 1)
        return;
    Acc a;
    a.pb = pb;
    a.start = rl(pb, PB_LOOP_ADDR);
    a.end = rl(pb, PB_END_ADDR);
    a.cur = rl(pb, PB_CUR_ADDR);
    a.format = rw(pb, PB_FORMAT);
    a.pred_scale = rw(pb, PB_PRED_SCALE);
    a.yn1 = (int16_t)rw(pb, PB_YN1);
    a.yn2 = (int16_t)rw(pb, PB_YN2);
    a.stopped = 0;
    for (int i = 0; i < 16; i++)
        a.coefs[i] = (int16_t)rw(pb, PB_COEFS + i);

    /* sample rate conversion (linear; polyphase needs the DSP coefficient ROM) */
    int16_t samples[SAMPLES], last[4];
    for (int i = 0; i < 4; i++)
        last[i] = (int16_t)rw(pb, PB_LAST + i);
    uint32_t ratio = rl(pb, PB_RATIO), pos = rw(pb, PB_FRAC);
    uint16_t src_type = rw(pb, PB_SRC_TYPE);
    if (src_type == 2) {                   /* none */
        for (int i = 0; i < SAMPLES; i++)
            samples[i] = acc_read(&a);
        memcpy(last, samples + SAMPLES - 4, sizeof last);
    } else {
        int16_t t[4];
        uint32_t idx = 0;
        for (int i = 0; i < 4; i++)
            t[idx++ & 3] = last[i];
        for (int i = 0; i < SAMPLES; i++) {
            pos += ratio;
            while (pos >= 0x10000) {
                t[idx++ & 3] = acc_read(&a);
                pos -= 0x10000;
            }
            uint16_t frac = (uint16_t)pos;
            if (frac) {
                int32_t s0 = t[idx++ & 3], s1 = t[idx++ & 3];
                samples[i] = (int16_t)((s0 * (int32_t)(uint16_t)(0 - frac) + s1 * (int32_t)frac) >> 16);
                idx += 2;
            } else {
                samples[i] = t[idx++ & 3];
                idx += 3;
            }
        }
        for (int i = 3; i >= 0; i--)
            last[i] = t[--idx & 3];
        pos &= 0xFFFF;
    }
    for (int i = 0; i < 4; i++)
        ww(pb, PB_LAST + i, (uint16_t)last[i]);
    ww(pb, PB_FRAC, (uint16_t)pos);
    ww(pb, PB_CUR_ADDR, (uint16_t)(a.cur >> 16));
    ww(pb, PB_CUR_ADDR + 1, (uint16_t)a.cur);
    ww(pb, PB_YN1, (uint16_t)a.yn1);
    ww(pb, PB_YN2, (uint16_t)a.yn2);
    ww(pb, PB_PRED_SCALE, a.pred_scale);

    /* volume envelope (unsigned on Wii) */
    uint16_t vol = rw(pb, PB_VOL);
    int16_t vdelta = (int16_t)rw(pb, PB_VOL_DELTA);
    for (int i = 0; i < SAMPLES; i++) {
        samples[i] = clamp16(((int32_t)samples[i] * vol) >> 15);
        vol = (uint16_t)(vol + vdelta);
    }
    ww(pb, PB_VOL, vol);

    /* low-pass filter */
    if (rw(pb, PB_LPF)) {
        int16_t yn1 = (int16_t)rw(pb, PB_LPF + 1);
        int32_t a0 = rw(pb, PB_LPF + 2), b0 = (int16_t)rw(pb, PB_LPF + 3);
        for (int i = 0; i < SAMPLES; i++)
            yn1 = samples[i] = clamp16((a0 * samples[i] + b0 * yn1) >> 15);
        ww(pb, PB_LPF + 1, (uint16_t)yn1);
    }
    /* biquad (newest microcode only) */
    if (s_old == 0 && rw(pb, PB_BIQUAD)) {
        int16_t xn1 = (int16_t)rw(pb, PB_BIQUAD + 1), xn2 = (int16_t)rw(pb, PB_BIQUAD + 2);
        int16_t yn1 = (int16_t)rw(pb, PB_BIQUAD + 3), yn2 = (int16_t)rw(pb, PB_BIQUAD + 4);
        int32_t b0 = (int16_t)rw(pb, PB_BIQUAD + 5), b1 = (int16_t)rw(pb, PB_BIQUAD + 6);
        int32_t b2 = (int16_t)rw(pb, PB_BIQUAD + 7), a1 = (int16_t)rw(pb, PB_BIQUAD + 8);
        int32_t a2 = (int16_t)rw(pb, PB_BIQUAD + 9);
        for (int i = 0; i < SAMPLES; i++) {
            int16_t x0 = samples[i];
            int64_t t = (int64_t)b0 * x0 + (int64_t)b1 * xn1 + (int64_t)b2 * xn2 +
                        (int64_t)a1 * yn1 + (int64_t)a2 * yn2;
            t <<= 2;
            t += (t & 0x10000) ? 0x8000 : 0x7FFF;
            int16_t y0 = clamp16(t >> 16);
            xn2 = xn1; yn2 = yn1; xn1 = x0; yn1 = y0;
            samples[i] = y0;
        }
        ww(pb, PB_BIQUAD + 1, (uint16_t)xn1);
        ww(pb, PB_BIQUAD + 2, (uint16_t)xn2);
        ww(pb, PB_BIQUAD + 3, (uint16_t)yn1);
        ww(pb, PB_BIQUAD + 4, (uint16_t)yn2);
    }

    /* mix into the enabled buses */
    uint32_t mctrl = rl(pb, PB_MIXCTRL);
    for (int m = 0; m < 12; m++) {
        int on = (mctrl & k_mix[m].on) || (mctrl & k_mix[m].ramp);
        if (!on)
            continue;
        int ramp = (mctrl & k_mix[m].ramp) != 0;
        uint16_t v = rw(pb, PB_MIXER + 2 * m);
        uint16_t d = ramp ? rw(pb, PB_MIXER + 2 * m + 1) : 0;
        int32_t *out = s_buf[k_mix[m].buf];
        int16_t s16 = 0;
        for (int i = 0; i < SAMPLES; i++) {
            s16 = clamp16(((int64_t)samples[i] * v) >> 15);
            out[i] += s16;
            v = (uint16_t)(v + d);
        }
        ww(pb, PB_MIXER + 2 * m, v);
        ww(pb, PB_DPOP + k_dpop[m], (uint16_t)s16);
    }
}

/* ------------------------------------------------------------ commands */

static void setup(uint32_t addr)
{
    /* 20 buffers x (value hi, value lo, delta); the first 12 are ours */
    for (int b = 0; b < NBUF; b++) {
        int32_t value = (int32_t)((uint32_t)ld16(phys(addr + b * 6)) << 16 | ld16(phys(addr + b * 6 + 2)));
        int16_t delta = (int16_t)ld16(phys(addr + b * 6 + 4));
        for (int i = 0; i < SAMPLES; i++)
            s_buf[b][i] = value ? value + i * delta : 0;
    }
}

static void ramp(uint16_t *out, uint16_t from, uint16_t to)
{
    float cur = from;
    for (int i = 0; i < SAMPLES; i++) {
        cur += (to - from) / (float)SAMPLES;
        out[i] = (uint16_t)cur;
    }
}

static void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr, uint16_t volume)
{
    uint16_t r[SAMPLES];
    ramp(r, s_last_aux_volume[aux], volume);
    s_last_aux_volume[aux] = volume;
    int base = BUF_AUXA_L + aux * 3;
    if (write_addr) {
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < SAMPLES; i++)
                st32(phys(write_addr + (c * SAMPLES + i) * 4), (uint32_t)s_buf[base + c][i]);
    }
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < SAMPLES; i++) {
            int64_t s = (int32_t)ld32(phys(read_addr + (c * SAMPLES + i) * 4));
            s_buf[BUF_MAIN_L + c][i] += (int32_t)((s * r[i]) >> 15);
        }
}

static void upload_aux_mix_lrsc(int aux_b, const uint32_t *addr, uint16_t volume)
{
    int l = aux_b ? BUF_AUXB_L : BUF_AUXA_L;
    int c_buf = aux_b ? BUF_AUXC_S : BUF_AUXC_R;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < SAMPLES; i++)
            st32(phys(addr[0] + (c * SAMPLES + i) * 4), (uint32_t)s_buf[l + c][i]);
    for (int i = 0; i < SAMPLES; i++)
        st32(phys(addr[1] + i * 4), (uint32_t)s_buf[c_buf][i]);
    uint16_t r[SAMPLES];
    ramp(r, s_last_aux_volume[aux_b], volume);
    s_last_aux_volume[aux_b] = volume;
    static const int dest[4] = {BUF_MAIN_L, BUF_MAIN_R, BUF_MAIN_S, BUF_AUXC_L};
    for (int m = 0; m < 4; m++)
        for (int i = 0; i < SAMPLES; i++) {
            int64_t s = (int32_t)ld32(phys(addr[2 + m] + i * 4));
            s_buf[dest[m]][i] += (int32_t)((s * r[i]) >> 15);
        }
}

static void output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume)
{
    uint16_t r[SAMPLES];
    ramp(r, s_last_main_volume, volume);
    s_last_main_volume = volume;
    for (int i = 0; i < SAMPLES; i++)
        st32(phys(surround_addr + i * 4), (uint32_t)s_buf[BUF_MAIN_S][i]);
    for (int i = 0; i < SAMPLES; i++) {
        int16_t left = clamp16(((int64_t)s_buf[BUF_MAIN_L][i] * r[i]) >> 15);
        int16_t right = clamp16(((int64_t)s_buf[BUF_MAIN_R][i] * r[i]) >> 15);
        st16(phys(lr_addr + i * 4), (uint16_t)right);       /* AI order: right, left */
        st16(phys(lr_addr + i * 4 + 2), (uint16_t)left);
    }
    dsp_push_mail(0xDCD10004, 1);                           /* DSP_SYNC */
}

static uint32_t cl_addr;
static uint16_t next_word(void)
{
    uint16_t v = (uint16_t)ld16(phys(cl_addr));
    cl_addr += 2;
    return v;
}
static uint32_t next_long(void)
{
    uint32_t hi = next_word();
    return hi << 16 | next_word();
}

/* old microcode: END = 0x0F and PB_ADDR/PROCESS split; new: END = 0x0E */
static int detect_old(uint32_t addr)
{
    /* walk the list with the new numbering; if it runs into 0x0F first, it's old */
    for (int n = 0; n < 64; n++) {
        uint16_t cmd = (uint16_t)ld16(phys(addr));
        if (cmd == 0x0E) return 0;
        if (cmd == 0x0F) return 1;
        static const uint8_t args_new[15] = {2, 2, 2, 2, 2, 5, 5, 5, 13, 13, 4, 5, 5, 8, 0};
        if (cmd > 14) return 1;
        addr += 2 + args_new[cmd] * 2;
    }
    return 0;
}

static void ax_process_cmdlist_impl(uint32_t addr);

void ax_process_cmdlist(uint32_t addr)
{
    PROF_BEGIN(ax);
    ax_process_cmdlist_impl(addr);
    PROF_END(ax, PROF_AUDIO);
}

static void ax_process_cmdlist_impl(uint32_t addr)
{
    if (s_old < 0) {
        s_old = detect_old(addr);
        rt_log("AX: %s microcode command lists", s_old ? "old" : "new");
    }
    cl_addr = addr;
    uint32_t pb_addr = 0;
    for (int guard = 0; guard < 256; guard++) {
        uint16_t cmd = next_word();
        if (s_old) {                               /* renumber to the new set */
            if (cmd == 0x04) { pb_addr = next_long(); continue; }
            if (cmd == 0x05) { cmd = 0xFF; }
            else if (cmd >= 0x06) cmd--;
        }
        switch (cmd) {
        case 0x00: setup(next_long()); break;
        case 0x01: case 0x02: {                    /* add/sub to LR */
            uint32_t a = next_long();
            for (int i = 0; i < SAMPLES; i++) {
                int32_t v = (int32_t)ld32(phys(a + i * 4));
                if (cmd == 0x02) v = -v;
                s_buf[BUF_MAIN_L][i] += v;
                s_buf[BUF_MAIN_R][i] += v;
            }
            break;
        }
        case 0x03: {
            uint32_t a = next_long();
            for (int i = 0; i < SAMPLES; i++)
                s_buf[BUF_MAIN_L][i] += (int32_t)ld32(phys(a + i * 4));
            for (int i = 0; i < SAMPLES; i++)
                s_buf[BUF_MAIN_R][i] -= (int32_t)ld32(phys(a + (SAMPLES + i) * 4));
            break;
        }
        case 0x04:
            pb_addr = next_long();
            /* fall through */
        case 0xFF:
            for (int n = 0; pb_addr && n < 512; n++) {
                uint32_t pb = phys(pb_addr);
                process_voice(pb);
                pb_addr = rl(pb, PB_NEXT);
            }
            break;
        case 0x05: case 0x06: case 0x07: {
            uint16_t vol = next_word();
            uint32_t w = next_long(), r = next_long();
            mix_aux(cmd - 0x05, w, r, vol);
            break;
        }
        case 0x08: case 0x09: {
            uint16_t vol = next_word();
            uint32_t a[6];
            for (int i = 0; i < 6; i++)
                a[i] = next_long();
            upload_aux_mix_lrsc(cmd == 0x09, a, vol);
            break;
        }
        case 0x0A:                                 /* compressor: not emulated */
            cl_addr += 8;
            break;
        case 0x0B: case 0x0C: {
            uint16_t vol = s_old ? 0x8000 : next_word();
            uint32_t surround = next_long(), lr = next_long();
            output(lr, surround, vol);
            break;
        }
        case 0x0D:                                 /* Wii Remote speakers: silent */
            cl_addr += 16;
            break;
        case 0x0E:
            return;
        default:
            rt_log("AX: unknown command %04X", cmd);
            return;
        }
    }
}
