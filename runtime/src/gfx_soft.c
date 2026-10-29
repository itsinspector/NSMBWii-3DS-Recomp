/* Reference software renderer (PC debug build): rasterizes GX triangles into
 * an emulated EFB with the full TEV, alpha test, depth and blending, handles
 * EFB copies (to XFB = present, to texture = written back to guest memory),
 * shows frames in the PC window and dumps them as PNG (NSMBW_DUMP_FRAMES=N).
 *
 * Draws are queued with their state decoded once ("DrawState"); the queue is
 * rasterized in parallel by worker threads, each owning interleaved bands of
 * rows, and flushed only when the EFB contents are needed (copies, present). */
#ifndef __3DS__
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "platform.h"
#include "gx_state.h"

#define EFB_W 640
#define EFB_H 528
#define BAND_SHIFT 3                     /* rows per band: 8 */

static uint8_t s_efb[EFB_H][EFB_W][4];  /* RGBA */
static float s_depth[EFB_H][EFB_W];
static uint32_t s_frames, s_dump_every;
static int s_skip_raster;               /* NSMBW_FAST: only rasterize near dumps */

void png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
void soft_present(void);

/* -------------------------------------------------------- draw state */

enum { IN_ZERO, IN_ONE, IN_HALF, IN_TEXC, IN_TEXA, IN_RASC, IN_RASA, IN_KONST, IN_REG };

typedef struct {
    int tex;                 /* texture map (0-7) or -1 */
    int coord;
    int chan;                /* 0, 1 or -1 (zero) */
    uint8_t tswap[4], rswap[4];
    /* color inputs a,b,c,d: kind + register index, alpha likewise */
    uint8_t ck[4], cr[4], ca_[4];  /* ca_: read register alpha replicated */
    uint8_t ak[4], ar[4];
    float kc[3], ka;
    uint8_t cbias, csub, cclamp, cscale, cdest, ccmp, cmode;
    uint8_t abias, asub, aclamp, ascale, adest, acmp, amode;
} Stage;

typedef struct {
    int nstages;
    Stage st[16];
    float reg_init[4][4];
    int out_c, out_a;
    uint8_t aref[2], acomp[2], alogic;
    int zenable, zfunc, zupdate;
    int blend, sf, df, bsub, cupd, aupd, const_alpha_en;
    float const_alpha;
    const GXTexture *tex[8];
    uint8_t wrap_s[8], wrap_t[8], linear[8];
    uint32_t coord_mask;     /* texcoords to interpolate */
    int chan_mask;
    int sc_x0, sc_y0, sc_x1, sc_y1;
} DrawState;

typedef struct {
    float x, y, z, invw;
    float col[2][4];
    float tex[8][3];
} SVert;

typedef struct {
    uint32_t state;          /* index into s_states */
    uint32_t first, count;   /* triangles (3 SVerts each) */
} DrawCmd;

static DrawState *s_states;
static uint32_t s_nstates, s_states_cap;
static SVert *s_tris;
static uint32_t s_ntris, s_tris_cap;
static DrawCmd *s_cmds;
static uint32_t s_ncmds, s_cmds_cap;

static float konst_frac(uint32_t sel)
{
    static const float f[8] = {1.0f, 7 / 8.0f, 6 / 8.0f, 5 / 8.0f, 4 / 8.0f, 3 / 8.0f, 2 / 8.0f, 1 / 8.0f};
    return f[sel & 7];
}

static void konst_color(uint32_t sel, float out[3])
{
    if (sel < 8) out[0] = out[1] = out[2] = konst_frac(sel);
    else if (sel >= 0x0C && sel <= 0x0F) memcpy(out, g_gx.tev_konst[sel - 0x0C], 3 * sizeof(float));
    else if (sel >= 0x10) out[0] = out[1] = out[2] = g_gx.tev_konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
    else out[0] = out[1] = out[2] = 0;
}

static float konst_alpha(uint32_t sel)
{
    if (sel < 8) return konst_frac(sel);
    if (sel >= 0x10) return g_gx.tev_konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
    return 0;
}

static void swap_table(int table, uint8_t out[4])
{
    uint32_t k0 = g_gx.bp[BP_KSEL(table * 2)], k1 = g_gx.bp[BP_KSEL(table * 2 + 1)];
    out[0] = (uint8_t)BITS(k0, 0, 2);
    out[1] = (uint8_t)BITS(k0, 2, 2);
    out[2] = (uint8_t)BITS(k1, 0, 2);
    out[3] = (uint8_t)BITS(k1, 2, 2);
}

static void color_input(uint32_t sel, uint8_t *kind, uint8_t *reg, uint8_t *alpha)
{
    *reg = 0;
    *alpha = 0;
    switch (sel) {
    case 0: case 2: case 4: case 6: *kind = IN_REG; *reg = (uint8_t)(sel / 2); break;
    case 1: case 3: case 5: case 7: *kind = IN_REG; *reg = (uint8_t)(sel / 2); *alpha = 1; break;
    case 8: *kind = IN_TEXC; break;
    case 9: *kind = IN_TEXA; break;
    case 10: *kind = IN_RASC; break;
    case 11: *kind = IN_RASA; break;
    case 12: *kind = IN_ONE; break;
    case 13: *kind = IN_HALF; break;
    case 14: *kind = IN_KONST; break;
    default: *kind = IN_ZERO; break;
    }
}

static void alpha_input(uint32_t sel, uint8_t *kind, uint8_t *reg)
{
    *reg = 0;
    switch (sel) {
    case 0: case 1: case 2: case 3: *kind = IN_REG; *reg = (uint8_t)sel; break;
    case 4: *kind = IN_TEXA; break;
    case 5: *kind = IN_RASA; break;
    case 6: *kind = IN_KONST; break;
    default: *kind = IN_ZERO; break;
    }
}

static void build_state(DrawState *ds)
{
    memset(ds, 0, sizeof *ds);
    uint32_t gen = g_gx.bp[BP_GENMODE];
    ds->nstages = (int)BITS(gen, 10, 4) + 1;
    memcpy(ds->reg_init, g_gx.tev_reg, sizeof ds->reg_init);
    for (int s = 0; s < ds->nstages; s++) {
        Stage *st = &ds->st[s];
        uint32_t tref = g_gx.bp[BP_TREF(s)] >> ((s & 1) * 12);
        uint32_t map = BITS(tref, 0, 3), coord = BITS(tref, 3, 3), en = BITS(tref, 6, 1), chan = BITS(tref, 7, 3);
        uint32_t cenv = g_gx.bp[BP_TEV_COLOR(s)], aenv = g_gx.bp[BP_TEV_ALPHA(s)];
        uint32_t ksel = g_gx.bp[BP_KSEL(s / 2)] >> ((s & 1) * 10);
        st->tex = en ? (int)map : -1;
        st->coord = (int)coord;
        st->chan = chan < 2 ? (int)chan : -1;
        swap_table((int)BITS(aenv, 2, 2), st->tswap);
        swap_table((int)BITS(aenv, 0, 2), st->rswap);
        uint32_t csel[4] = {BITS(cenv, 12, 4), BITS(cenv, 8, 4), BITS(cenv, 4, 4), BITS(cenv, 0, 4)};
        uint32_t asel[4] = {BITS(aenv, 13, 3), BITS(aenv, 10, 3), BITS(aenv, 7, 3), BITS(aenv, 4, 3)};
        for (int k = 0; k < 4; k++) {
            color_input(csel[k], &st->ck[k], &st->cr[k], &st->ca_[k]);
            alpha_input(asel[k], &st->ak[k], &st->ar[k]);
        }
        konst_color(BITS(ksel, 4, 5), st->kc);
        st->ka = konst_alpha(BITS(ksel, 9, 5));
        st->cbias = (uint8_t)BITS(cenv, 16, 2);
        st->csub = (uint8_t)BITS(cenv, 18, 1);
        st->cclamp = (uint8_t)BITS(cenv, 19, 1);
        st->cscale = (uint8_t)BITS(cenv, 20, 2);
        st->cdest = (uint8_t)BITS(cenv, 22, 2);
        st->ccmp = st->cbias == 3;
        st->cmode = (uint8_t)(BITS(cenv, 20, 2) << 1 | BITS(cenv, 18, 1));
        st->abias = (uint8_t)BITS(aenv, 16, 2);
        st->asub = (uint8_t)BITS(aenv, 18, 1);
        st->aclamp = (uint8_t)BITS(aenv, 19, 1);
        st->ascale = (uint8_t)BITS(aenv, 20, 2);
        st->adest = (uint8_t)BITS(aenv, 22, 2);
        st->acmp = st->abias == 3;
        st->amode = (uint8_t)(BITS(aenv, 20, 2) << 1 | BITS(aenv, 18, 1));
        if (st->tex >= 0) {
            ds->coord_mask |= 1u << coord;
            if (!ds->tex[map]) {
                ds->tex[map] = gx_get_texture((int)map);
                uint32_t mode = g_gx.bp[BP_TX_MODE0(map)];
                ds->wrap_s[map] = (uint8_t)BITS(mode, 0, 2);
                ds->wrap_t[map] = (uint8_t)BITS(mode, 2, 2);
                ds->linear[map] = (uint8_t)BITS(mode, 4, 1);
            }
        }
        if (st->chan >= 0)
            ds->chan_mask |= 1 << st->chan;
    }
    ds->out_c = ds->st[ds->nstages - 1].cdest;
    ds->out_a = ds->st[ds->nstages - 1].adest;

    uint32_t ac = g_gx.bp[BP_ALPHACOMPARE];
    ds->aref[0] = (uint8_t)BITS(ac, 0, 8);
    ds->aref[1] = (uint8_t)BITS(ac, 8, 8);
    ds->acomp[0] = (uint8_t)BITS(ac, 16, 3);
    ds->acomp[1] = (uint8_t)BITS(ac, 19, 3);
    ds->alogic = (uint8_t)BITS(ac, 22, 2);

    uint32_t zm = g_gx.bp[BP_ZMODE], cm = g_gx.bp[BP_CMODE0], cm1 = g_gx.bp[BP_CMODE1];
    ds->zenable = (int)BITS(zm, 0, 1);
    ds->zfunc = (int)BITS(zm, 1, 3);
    ds->zupdate = ds->zenable && BITS(zm, 4, 1);
    ds->blend = (int)BITS(cm, 0, 1);
    ds->sf = (int)BITS(cm, 8, 3);
    ds->df = (int)BITS(cm, 5, 3);
    ds->bsub = (int)BITS(cm, 11, 1);
    ds->cupd = (int)BITS(cm, 3, 1);
    ds->aupd = (int)BITS(cm, 4, 1);
    ds->const_alpha_en = (int)BITS(cm1, 8, 1);
    ds->const_alpha = BITS(cm1, 0, 8) / 255.0f;

    uint32_t tl = g_gx.bp[BP_SCISSOR_TL], br = g_gx.bp[BP_SCISSOR_BR], off = g_gx.bp[BP_SCISSOR_OFFSET];
    int ox = (int)BITS(off, 0, 10) * 2, oy = (int)BITS(off, 10, 10) * 2;
    ds->sc_x0 = (int)BITS(tl, 12, 11) - ox;
    ds->sc_y0 = (int)BITS(tl, 0, 11) - oy;
    ds->sc_x1 = (int)BITS(br, 12, 11) - ox + 1;
    ds->sc_y1 = (int)BITS(br, 0, 11) - oy + 1;
    if (ds->sc_x0 < 0) ds->sc_x0 = 0;
    if (ds->sc_y0 < 0) ds->sc_y0 = 0;
    if (ds->sc_x1 > EFB_W) ds->sc_x1 = EFB_W;
    if (ds->sc_y1 > EFB_H) ds->sc_y1 = EFB_H;
}

/* ------------------------------------------------------------- sampling */

static inline int wrap_coord(int c, int size, uint32_t mode)
{
    if (mode == 1) {                                   /* repeat */
        if ((size & (size - 1)) == 0)
            return c & (size - 1);
        c %= size;
        return c < 0 ? c + size : c;
    }
    if (mode == 2) {                                   /* mirror */
        int period = size * 2;
        c %= period;
        if (c < 0) c += period;
        return c < size ? c : period - 1 - c;
    }
    return c < 0 ? 0 : c >= size ? size - 1 : c;       /* clamp */
}

static inline void unpack(uint32_t c, float out[4])
{
    static const float k = 1.0f / 255.0f;
    out[0] = (c & 255) * k;
    out[1] = ((c >> 8) & 255) * k;
    out[2] = ((c >> 16) & 255) * k;
    out[3] = (c >> 24) * k;
}

static void sample(const DrawState *ds, int map, float s, float t, float out[4])
{
    const GXTexture *tex = ds->tex[map];
    if (!tex) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    int w = tex->width, h = tex->height;
    float u = s * w - 0.5f, v = t * h - 0.5f;
    if (!ds->linear[map]) {
        int x = wrap_coord((int)floorf(u + 0.5f), w, ds->wrap_s[map]);
        int y = wrap_coord((int)floorf(v + 0.5f), h, ds->wrap_t[map]);
        unpack(tex->rgba[y * w + x], out);
        return;
    }
    float fu = floorf(u), fv = floorf(v);
    int x0 = (int)fu, y0 = (int)fv;
    float fx = u - fu, fy = v - fv;
    int xa = wrap_coord(x0, w, ds->wrap_s[map]), xb = wrap_coord(x0 + 1, w, ds->wrap_s[map]);
    int ya = wrap_coord(y0, h, ds->wrap_t[map]), yb = wrap_coord(y0 + 1, h, ds->wrap_t[map]);
    uint32_t c00 = tex->rgba[ya * w + xa], c10 = tex->rgba[ya * w + xb];
    uint32_t c01 = tex->rgba[yb * w + xa], c11 = tex->rgba[yb * w + xb];
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    for (int k = 0; k < 4; k++) {
        int sh = 8 * k;
        out[k] = (((c00 >> sh) & 255) * w00 + ((c10 >> sh) & 255) * w10 +
                  ((c01 >> sh) & 255) * w01 + ((c11 >> sh) & 255) * w11) * (1.0f / 255.0f);
    }
}

/* ------------------------------------------------------------------ TEV */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t to8(float v) { return (uint32_t)(clampf(v, 0, 1) * 255.0f + 0.5f); }

static int compare_alpha(uint32_t comp, uint32_t a, uint32_t ref)
{
    switch (comp) {
    case 0: return 0;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return 1;
    }
}

static int tev_compare(uint8_t mode, const float a[3], const float b[3], int k)
{
    int eq = mode & 1;
    switch (mode >> 1) {
    case 0: return eq ? to8(a[0]) == to8(b[0]) : to8(a[0]) > to8(b[0]);
    case 1: {
        uint32_t va = to8(a[1]) << 8 | to8(a[0]), vb = to8(b[1]) << 8 | to8(b[0]);
        return eq ? va == vb : va > vb;
    }
    case 2: {
        uint32_t va = to8(a[2]) << 16 | to8(a[1]) << 8 | to8(a[0]);
        uint32_t vb = to8(b[2]) << 16 | to8(b[1]) << 8 | to8(b[0]);
        return eq ? va == vb : va > vb;
    }
    default: return eq ? to8(a[k]) == to8(b[k]) : to8(a[k]) > to8(b[k]);
    }
}

/* evaluates the TEV for one pixel; returns 0 if discarded by alpha test */
static int run_tev(const DrawState *ds, const float col[2][4], float tex[8][3], float out[4])
{
    static const float bias_v[4] = {0, 0.5f, -0.5f, 0};
    static const float scale_v[4] = {1, 2, 4, 0.5f};
    float reg[4][4];
    memcpy(reg, ds->reg_init, sizeof reg);
    for (int s = 0; s < ds->nstages; s++) {
        const Stage *st = &ds->st[s];
        float texc[4] = {1, 1, 1, 1}, rasc[4] = {0, 0, 0, 0};
        if (st->tex >= 0) {
            float t4[4], q = tex[st->coord][2];
            if (q == 0) q = 1;
            float iq = 1.0f / q;
            sample(ds, st->tex, tex[st->coord][0] * iq, tex[st->coord][1] * iq, t4);
            for (int k = 0; k < 4; k++)
                texc[k] = t4[st->tswap[k]];
        }
        if (st->chan >= 0)
            for (int k = 0; k < 4; k++)
                rasc[k] = col[st->chan][st->rswap[k]];

        float in[4][3], ain[4];
        for (int k = 0; k < 4; k++) {
            float *v = in[k];
            switch (st->ck[k]) {
            case IN_REG:
                if (st->ca_[k]) v[0] = v[1] = v[2] = reg[st->cr[k]][3];
                else { v[0] = reg[st->cr[k]][0]; v[1] = reg[st->cr[k]][1]; v[2] = reg[st->cr[k]][2]; }
                break;
            case IN_TEXC: v[0] = texc[0]; v[1] = texc[1]; v[2] = texc[2]; break;
            case IN_TEXA: v[0] = v[1] = v[2] = texc[3]; break;
            case IN_RASC: v[0] = rasc[0]; v[1] = rasc[1]; v[2] = rasc[2]; break;
            case IN_RASA: v[0] = v[1] = v[2] = rasc[3]; break;
            case IN_ONE: v[0] = v[1] = v[2] = 1; break;
            case IN_HALF: v[0] = v[1] = v[2] = 0.5f; break;
            case IN_KONST: v[0] = st->kc[0]; v[1] = st->kc[1]; v[2] = st->kc[2]; break;
            default: v[0] = v[1] = v[2] = 0; break;
            }
            switch (st->ak[k]) {
            case IN_REG: ain[k] = reg[st->ar[k]][3]; break;
            case IN_TEXA: ain[k] = texc[3]; break;
            case IN_RASA: ain[k] = rasc[3]; break;
            case IN_KONST: ain[k] = st->ka; break;
            default: ain[k] = 0; break;
            }
        }
        /* a, b, c are 8-bit unsigned in hardware */
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 3; k++)
                in[i][k] = clampf(in[i][k], 0, 1);
            ain[i] = clampf(ain[i], 0, 1);
        }
        float res[4];
        if (st->ccmp) {
            for (int k = 0; k < 3; k++)
                res[k] = in[3][k] + (tev_compare(st->cmode, in[0], in[1], k) ? in[2][k] : 0);
        } else {
            float sign = st->csub ? -1.0f : 1.0f, bias = bias_v[st->cbias], sc = scale_v[st->cscale];
            for (int k = 0; k < 3; k++) {
                float l = in[0][k] + (in[1][k] - in[0][k]) * in[2][k];
                res[k] = (in[3][k] + sign * l + bias) * sc;
            }
        }
        if (st->acmp) {
            uint32_t a0 = to8(ain[0]), a1 = to8(ain[1]);
            int ok = (st->amode & 1) ? a0 == a1 : a0 > a1;
            res[3] = ain[3] + (ok ? ain[2] : 0);
        } else {
            float l = ain[0] + (ain[1] - ain[0]) * ain[2];
            res[3] = (ain[3] + (st->asub ? -l : l) + bias_v[st->abias]) * scale_v[st->ascale];
        }
        float *dc = reg[st->cdest];
        for (int k = 0; k < 3; k++)
            dc[k] = st->cclamp ? clampf(res[k], 0, 1) : clampf(res[k], -4.0f, 4.0f);
        reg[st->adest][3] = st->aclamp ? clampf(res[3], 0, 1) : clampf(res[3], -4.0f, 4.0f);
    }
    for (int k = 0; k < 3; k++)
        out[k] = clampf(reg[ds->out_c][k], 0, 1);
    out[3] = clampf(reg[ds->out_a][3], 0, 1);

    uint32_t a8 = to8(out[3]);
    int r0 = compare_alpha(ds->acomp[0], a8, ds->aref[0]);
    int r1 = compare_alpha(ds->acomp[1], a8, ds->aref[1]);
    switch (ds->alogic) {
    case 0: return r0 && r1;
    case 1: return r0 || r1;
    case 2: return r0 != r1;
    default: return r0 == r1;
    }
}

/* ----------------------------------------------------------- rasterizer */

static inline float blend_factor(int f, int is_src, const float src[4], const float dst[4], int k)
{
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return is_src ? dst[k] : src[k];
    case 3: return 1 - (is_src ? dst[k] : src[k]);
    case 4: return src[3];
    case 5: return 1 - src[3];
    case 6: return dst[3];
    default: return 1 - dst[3];
    }
}

static inline int depth_pass(int func, float z, float d)
{
    switch (func) {
    case 0: return 0;
    case 1: return z < d;
    case 2: return z == d;
    case 3: return z <= d;
    case 4: return z > d;
    case 5: return z != d;
    case 6: return z >= d;
    default: return 1;
    }
}

static void shade_pixel(const DrawState *ds, int x, int y, float z, const float col[2][4], float tex[8][3])
{
    if (ds->zenable && !depth_pass(ds->zfunc, z, s_depth[y][x]))
        return;
    float out[4];
    if (!run_tev(ds, col, tex, out))
        return;
    if (ds->zupdate)
        s_depth[y][x] = z;
    uint8_t *px = s_efb[y][x];
    float res[4];
    if (ds->blend) {
        static const float k = 1.0f / 255.0f;
        float dst[4] = {px[0] * k, px[1] * k, px[2] * k, px[3] * k};
        for (int c = 0; c < 4; c++) {
            float s = out[c] * blend_factor(ds->sf, 1, out, dst, c);
            float d = dst[c] * blend_factor(ds->df, 0, out, dst, c);
            res[c] = ds->bsub ? d - s : s + d;
        }
    } else {
        memcpy(res, out, sizeof res);
    }
    if (ds->const_alpha_en)
        res[3] = ds->const_alpha;
    if (ds->cupd) {
        px[0] = (uint8_t)to8(res[0]);
        px[1] = (uint8_t)to8(res[1]);
        px[2] = (uint8_t)to8(res[2]);
    }
    if (ds->aupd)
        px[3] = (uint8_t)to8(res[3]);
}

/* rasterizes one triangle, only rows owned by this worker */
static void raster_triangle(const DrawState *ds, const SVert *a, const SVert *b, const SVert *c,
                            int worker, int nworkers)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0)
        return;
    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x))), maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y))), maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < ds->sc_x0) minx = ds->sc_x0;
    if (miny < ds->sc_y0) miny = ds->sc_y0;
    if (maxx > ds->sc_x1) maxx = ds->sc_x1;
    if (maxy > ds->sc_y1) maxy = ds->sc_y1;
    if (minx >= maxx || miny >= maxy)
        return;
    float inv_area = 1.0f / area;
    /* edge functions: w0 = E0/area etc., linear in x */
    float e0dx = -(c->y - b->y) * inv_area, e1dx = -(a->y - c->y) * inv_area;
    uint32_t cmask = ds->coord_mask;
    int chans = ds->chan_mask;
    for (int y = miny; y < maxy; y++) {
        if (((y >> BAND_SHIFT) % nworkers) != worker)
            continue;
        float py = y + 0.5f, px = minx + 0.5f;
        float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv_area;
        float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv_area;
        for (int x = minx; x < maxx; x++, w0 += e0dx, w1 += e1dx) {
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            float iw = w0 * a->invw + w1 * b->invw + w2 * c->invw;
            float inv = 1.0f / iw;
            float p0 = w0 * a->invw * inv, p1 = w1 * b->invw * inv, p2 = w2 * c->invw * inv;
            float col[2][4], tex[8][3];
            for (int ch = 0; ch < 2; ch++)
                if (chans & (1 << ch))
                    for (int k = 0; k < 4; k++)
                        col[ch][k] = p0 * a->col[ch][k] + p1 * b->col[ch][k] + p2 * c->col[ch][k];
            for (uint32_t m = cmask; m; m &= m - 1) {
                int t = 0;
                while (!((m >> t) & 1)) t++;
                for (int k = 0; k < 3; k++)
                    tex[t][k] = p0 * a->tex[t][k] + p1 * b->tex[t][k] + p2 * c->tex[t][k];
            }
            shade_pixel(ds, x, y, z, col, tex);
        }
    }
}

/* ------------------------------------------------------------- workers */

static int s_nworkers = 1;
static plat_sem *s_work_go[32], *s_work_done;

static void run_queue(int worker)
{
    for (uint32_t i = 0; i < s_ncmds; i++) {
        const DrawCmd *cmd = &s_cmds[i];
        const DrawState *ds = &s_states[cmd->state];
        const SVert *t = &s_tris[cmd->first * 3];
        for (uint32_t k = 0; k < cmd->count; k++)
            raster_triangle(ds, &t[k * 3], &t[k * 3 + 1], &t[k * 3 + 2], worker, s_nworkers);
    }
}

static void worker_main(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (;;) {
        plat_sem_wait(s_work_go[id]);
        run_queue(id);
        plat_sem_post(s_work_done);
    }
}

/* rasterize everything queued so far */
static uint64_t s_flush_us, s_frame_start_us, s_tris_frame;

static void flush(void)
{
    if (!s_ncmds)
        return;
    uint64_t t0 = plat_time_us();
    s_tris_frame += s_ntris;
    for (int i = 1; i < s_nworkers; i++)
        plat_sem_post(s_work_go[i]);
    run_queue(0);
    for (int i = 1; i < s_nworkers; i++)
        plat_sem_wait(s_work_done);
    s_ncmds = s_ntris = s_nstates = 0;
    s_flush_us += plat_time_us() - t0;
}

void soft_texture_will_change(void) { flush(); }

void soft_init(void)
{
    const char *e = getenv("NSMBW_DUMP_FRAMES");
    s_dump_every = e ? (uint32_t)atoi(e) : 0;
    for (int y = 0; y < EFB_H; y++)
        for (int x = 0; x < EFB_W; x++)
            s_depth[y][x] = 1.0f;
    const char *t = getenv("NSMBW_RENDER_THREADS");
    s_nworkers = t ? atoi(t) : (int)plat_cpu_count();
    if (s_nworkers < 1) s_nworkers = 1;
    if (s_nworkers > 32) s_nworkers = 32;
    s_work_done = plat_sem_create();
    for (int i = 1; i < s_nworkers; i++) {
        s_work_go[i] = plat_sem_create();
        plat_thread_start(worker_main, (void *)(intptr_t)i, 128 * 1024);
    }
    rt_log("software renderer: %d threads", s_nworkers);
}

/* ------------------------------------------------------------ queueing */

static void to_screen(const GXVertex *v, SVert *o)
{
    float wd = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT]), ht = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 1]);
    float zr = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 2]), xo = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 3]);
    float yo = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 4]), fz = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 5]);
    uint32_t off = g_gx.bp[BP_SCISSOR_OFFSET];
    float ox = (float)(BITS(off, 0, 10) * 2), oy = (float)(BITS(off, 10, 10) * 2);
    float iw = 1.0f / v->pos[3];
    o->x = v->pos[0] * iw * wd + xo - ox;
    o->y = v->pos[1] * iw * ht + yo - oy;
    o->z = clampf((v->pos[2] * iw * zr + fz) / 16777216.0f, 0, 1);
    o->invw = iw;
    memcpy(o->col, v->color, sizeof o->col);
    memcpy(o->tex, v->tex, sizeof o->tex);
}

static void lerp_vert(const GXVertex *a, const GXVertex *b, float t, GXVertex *o)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)o;
    for (size_t i = 0; i < sizeof(GXVertex) / sizeof(float); i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static void push_tri(const SVert *a, const SVert *b, const SVert *c, uint32_t cull)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    /* GX front faces are clockwise on screen (y down): positive area */
    if (area == 0 || cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0))
        return;
    if (s_ntris + 1 > s_tris_cap) {
        s_tris_cap = s_tris_cap ? s_tris_cap * 2 : 4096;
        s_tris = realloc(s_tris, (size_t)s_tris_cap * 3 * sizeof(SVert));
    }
    SVert *o = &s_tris[s_ntris * 3];
    o[0] = *a;
    o[1] = *b;
    o[2] = *c;
    s_ntris++;
}

/* clip against the near plane (z >= -w), then queue */
static void queue_clipped(const GXVertex *t, uint32_t cull)
{
    GXVertex poly[8], tmp;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const GXVertex *a = &t[i], *b = &t[(i + 1) % 3];
        float da = a->pos[2] + a->pos[3], db = b->pos[2] + b->pos[3];
        if (da >= 0)
            poly[n++] = *a;
        if ((da >= 0) != (db >= 0)) {
            lerp_vert(a, b, da / (da - db), &tmp);
            poly[n++] = tmp;
        }
    }
    if (n < 3)
        return;
    SVert s[8];
    for (int i = 0; i < n; i++) {
        if (poly[i].pos[3] <= 1e-6f)
            return;
        to_screen(&poly[i], &s[i]);
    }
    for (int i = 1; i + 1 < n; i++)
        push_tri(&s[0], &s[i], &s[i + 1], cull);
}

void soft_draw_triangles(const GXVertex *v, uint32_t count)
{
    if (s_skip_raster || count < 3)
        return;
    if (s_nstates + 1 > s_states_cap) {
        s_states_cap = s_states_cap ? s_states_cap * 2 : 1024;
        s_states = realloc(s_states, s_states_cap * sizeof *s_states);
    }
    DrawState *ds = &s_states[s_nstates];
    build_state(ds);
    uint32_t first = s_ntris, cull = BITS(g_gx.bp[BP_GENMODE], 14, 2);
    for (uint32_t i = 0; i + 2 < count; i += 3)
        queue_clipped(&v[i], cull);
    if (s_ntris == first)
        return;
    if (s_ncmds + 1 > s_cmds_cap) {
        s_cmds_cap = s_cmds_cap ? s_cmds_cap * 2 : 1024;
        s_cmds = realloc(s_cmds, s_cmds_cap * sizeof *s_cmds);
    }
    s_cmds[s_ncmds++] = (DrawCmd){s_nstates++, first, s_ntris - first};
}

/* ------------------------------------------------------------ EFB copy */

static void write_copy_texture(uint32_t trigger, int x0, int y0, int w, int h)
{
    uint32_t dest = (g_gx.bp[BP_COPY_DEST] & 0xFFFFFF) << 5;
    int half = BITS(trigger, 9, 1);
    uint32_t tpf = BITS(trigger, 3, 4);
    uint32_t fmt = tpf / 2 + (tpf & 1) * 8;           /* EFB copy texture format */
    int intensity = BITS(trigger, 15, 1);
    if (half) { w /= 2; h /= 2; }
    int bw = 4, bh = 4, bytes = 32;
    if (fmt == 6) bytes = 64;
    else if (fmt == 1 || fmt == 7 || fmt == 8 || fmt == 9 || fmt == 10) { bw = 8; bh = 4; }
    else if (fmt == 0) { bw = 8; bh = 8; }
    uint32_t out = dest | 0x80000000u;
    for (int by = 0; by < h; by += bh) {
        for (int bx = 0; bx < w; bx += bw) {
            uint8_t blk[64] = {0};
            for (int yy = 0; yy < bh; yy++) {
                for (int xx = 0; xx < bw; xx++) {
                    int sx = x0 + (bx + xx) * (half ? 2 : 1), sy = y0 + (by + yy) * (half ? 2 : 1);
                    if (sx >= EFB_W) sx = EFB_W - 1;
                    if (sy >= EFB_H) sy = EFB_H - 1;
                    const uint8_t *p = s_efb[sy][sx];
                    int i = yy * bw + xx;
                    uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
                    uint32_t lum = (r * 77 + g * 150 + b * 29) >> 8;
                    switch (fmt) {
                    case 0: { uint32_t v = (intensity ? lum : r) >> 4; blk[i / 2] |= (uint8_t)(v << ((i & 1) ? 0 : 4)); break; }
                    case 1: case 8: blk[i] = (uint8_t)(intensity ? lum : r); break;
                    case 7: blk[i] = (uint8_t)a; break;
                    case 9: blk[i] = (uint8_t)g; break;
                    case 10: blk[i] = (uint8_t)b; break;
                    case 3: blk[2 * i] = (uint8_t)a; blk[2 * i + 1] = (uint8_t)(intensity ? lum : r); break;
                    case 4: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
                              blk[2 * i] = (uint8_t)(v >> 8); blk[2 * i + 1] = (uint8_t)v; break; }
                    case 5: { uint16_t v = (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
                              blk[2 * i] = (uint8_t)(v >> 8); blk[2 * i + 1] = (uint8_t)v; break; }
                    default: blk[2 * i] = (uint8_t)a; blk[2 * i + 1] = (uint8_t)r;
                             blk[32 + 2 * i] = (uint8_t)g; blk[32 + 2 * i + 1] = (uint8_t)b; break;
                    }
                }
            }
            guest_write(out, blk, (uint32_t)bytes);
            out += (uint32_t)bytes;
        }
    }
    gx_texture_invalidate();
}

void soft_efb_copy(uint32_t trigger)
{
    flush();
    uint32_t tl = g_gx.bp[BP_EFB_TL], sz = g_gx.bp[BP_EFB_SIZE];
    int x0 = (int)BITS(tl, 0, 10), y0 = (int)BITS(tl, 10, 10);
    int w = (int)BITS(sz, 0, 10) + 1, h = (int)BITS(sz, 10, 10) + 1;
    if (BITS(trigger, 14, 1))
        soft_present();
    else
        write_copy_texture(trigger, x0, y0, w, h);
    if (BITS(trigger, 11, 1)) {                   /* clear the source rectangle */
        uint32_t ar = g_gx.bp[BP_CLEAR_AR], gb = g_gx.bp[BP_CLEAR_GB];
        float z = (g_gx.bp[BP_CLEAR_Z] & 0xFFFFFF) / 16777216.0f;
        uint32_t cm = g_gx.bp[BP_CMODE0];
        int zupd = BITS(g_gx.bp[BP_ZMODE], 4, 1);
        uint8_t c[4] = {(uint8_t)(ar & 255), (uint8_t)(gb >> 8), (uint8_t)(gb & 255), (uint8_t)(ar >> 8)};
        int x1 = x0 + w > EFB_W ? EFB_W : x0 + w, y1 = y0 + h > EFB_H ? EFB_H : y0 + h;
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                if (BITS(cm, 3, 1)) {
                    s_efb[y][x][0] = c[0];
                    s_efb[y][x][1] = c[1];
                    s_efb[y][x][2] = c[2];
                }
                if (BITS(cm, 4, 1))
                    s_efb[y][x][3] = c[3];
                if (zupd)
                    s_depth[y][x] = z;
            }
        }
    }
}

void soft_present(void)
{
    flush();
    s_frames++;
    gx_texture_frame();
    {
        static uint64_t acc_total, acc_flush, acc_tris;
        uint64_t now = plat_time_us();
        if (s_frame_start_us) {
            acc_total += now - s_frame_start_us;
            acc_flush += s_flush_us;
            acc_tris += s_tris_frame;
        }
        s_frame_start_us = now;
        s_flush_us = 0;
        s_tris_frame = 0;
        if (s_frames % 120 == 0 && acc_total) {
            rt_log("[perf] %.1f fps, raster %.0f%% of frame time, %llu tris/frame",
                   120 * 1e6 / acc_total, 100.0 * acc_flush / acc_total, (unsigned long long)(acc_tris / 120));
            acc_total = acc_flush = acc_tris = 0;
        }
    }
    if (s_dump_every && getenv("NSMBW_FAST"))
        s_skip_raster = (s_frames + 3) % s_dump_every > 3;

    uint32_t sz = g_gx.bp[BP_EFB_SIZE];
    int w = (int)BITS(sz, 0, 10) + 1, h = (int)BITS(sz, 10, 10) + 1;
    if (w > EFB_W) w = EFB_W;
    if (h > EFB_H) h = EFB_H;
    static uint8_t img[EFB_H * EFB_W * 4];
    for (int y = 0; y < h; y++)
        memcpy(img + (size_t)y * w * 4, s_efb[y], (size_t)w * 4);
    plat_show_frame(img, w, h);

    if (!s_dump_every || s_frames % s_dump_every)
        return;
    for (int i = 0; i < w * h; i++)
        img[i * 4 + 3] = 255;
    char path[512];
    snprintf(path, sizeof path, "%sframes", plat_data_dir());
    plat_mkdir(path);
    snprintf(path, sizeof path, "%sframes/frame_%05lu.png", plat_data_dir(), (unsigned long)s_frames);
    png_write_rgba(path, img, w, h);
}
#endif
