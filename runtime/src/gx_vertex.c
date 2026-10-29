/* GX vertex pipeline on the CPU: vertex loader (VCD/VAT, direct and indexed
 * attributes), transform (XF matrices), lighting (color channels) and texture
 * coordinate generation, then primitive assembly into triangle lists. */
#include <math.h>
#include <stdlib.h>
#include "runtime.h"
#include "gx_state.h"

/* raw (model space) vertex as read by the vertex loader */
typedef struct {
    uint32_t pnmtx;
    uint32_t texmtx[8];
    float pos[3];
    float nrm[3][3];       /* normal, binormal, tangent */
    float col[2][4];
    float tex[8][2];
} RawVertex;

static inline float xf(uint32_t addr) { return g_gx.xf_memf[addr & 0xFFF]; }
static float xfr(uint32_t reg) { return gx_xf_float(g_gx.xf_regs[reg]); }

/* ---------------------------------------------------------------- loader */

static uint32_t comp_size(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }

static uint32_t color_size(uint32_t fmt)
{
    static const uint8_t sz[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return sz[fmt & 7];
}

typedef struct { uint32_t cnt, fmt, frac; } TexFmt;

static TexFmt tex_fmt(int vat, int i)
{
    uint32_t a = g_gx.cp_vat[0][vat], b = g_gx.cp_vat[1][vat], c = g_gx.cp_vat[2][vat];
    switch (i) {
    case 0: return (TexFmt){BITS(a, 21, 1), BITS(a, 22, 3), BITS(a, 25, 5)};
    case 1: return (TexFmt){BITS(b, 0, 1), BITS(b, 1, 3), BITS(b, 4, 5)};
    case 2: return (TexFmt){BITS(b, 9, 1), BITS(b, 10, 3), BITS(b, 13, 5)};
    case 3: return (TexFmt){BITS(b, 18, 1), BITS(b, 19, 3), BITS(b, 22, 5)};
    case 4: return (TexFmt){BITS(b, 27, 1), BITS(b, 28, 3), BITS(c, 0, 5)};
    case 5: return (TexFmt){BITS(c, 5, 1), BITS(c, 6, 3), BITS(c, 9, 5)};
    case 6: return (TexFmt){BITS(c, 14, 1), BITS(c, 15, 3), BITS(c, 18, 5)};
    default: return (TexFmt){BITS(c, 23, 1), BITS(c, 24, 3), BITS(c, 27, 5)};
    }
}

static uint32_t compute_vertex_size(int vat)
{
    uint32_t a = g_gx.cp_vat[0][vat], lo = g_gx.cp_vcd_lo, hi = g_gx.cp_vcd_hi;
    uint32_t size = 0, t;
    for (int i = 0; i < 9; i++)
        size += (lo >> i) & 1;
    t = BITS(lo, 9, 2);
    if (t == 1) size += (BITS(a, 0, 1) ? 3 : 2) * comp_size(BITS(a, 1, 3));
    else if (t) size += t - 1;
    t = BITS(lo, 11, 2);
    if (t == 1) size += (BITS(a, 9, 1) ? 9 : 3) * comp_size(BITS(a, 10, 3));
    else if (t) size += (t - 1) * (BITS(a, 9, 1) && BITS(a, 31, 1) ? 3 : 1);
    for (int i = 0; i < 2; i++) {
        t = BITS(lo, 13 + 2 * i, 2);
        if (t == 1) size += color_size(BITS(a, 14 + 4 * i, 3));
        else if (t) size += t - 1;
    }
    for (int i = 0; i < 8; i++) {
        t = BITS(hi, 2 * i, 2);
        TexFmt f = tex_fmt(vat, i);
        if (t == 1) size += (f.cnt ? 2 : 1) * comp_size(f.fmt);
        else if (t) size += t - 1;
    }
    return size;
}

/* reads n components of format fmt from host bytes p (big endian) */
static void read_comps(const uint8_t *p, uint32_t fmt, float scale, int n, float *out)
{
    for (int i = 0; i < n; i++) {
        switch (fmt) {
        case 0: out[i] = p[i] * scale; break;
        case 1: out[i] = (int8_t)p[i] * scale; break;
        case 2: out[i] = (uint16_t)(p[2 * i] << 8 | p[2 * i + 1]) * scale; break;
        case 3: out[i] = (int16_t)(p[2 * i] << 8 | p[2 * i + 1]) * scale; break;
        default: {
            uint32_t u = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
            out[i] = gx_xf_float(u);
        }
        }
    }
}

static void read_color(const uint8_t *p, uint32_t fmt, float *out)
{
    uint32_t r, g, b, a = 255;
    switch (fmt) {
    case 0: { uint32_t v = p[0] << 8 | p[1];
              r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break; }
    case 1: case 2: r = p[0]; g = p[1]; b = p[2]; break;
    case 3: { uint32_t v = p[0] << 8 | p[1];
              r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; break; }
    case 4: { uint32_t v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
              r = (v >> 18) * 255 / 63; g = ((v >> 12) & 63) * 255 / 63; b = ((v >> 6) & 63) * 255 / 63;
              a = (v & 63) * 255 / 63; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = a / 255.0f;
}

/* pointer to attribute data: direct (in the stream) or indexed (array) */
static const uint8_t *attr_ptr(uint32_t type, const uint8_t **cur, int array, uint8_t *tmp, uint32_t size)
{
    if (type == 1) {
        const uint8_t *p = *cur;
        *cur += size;
        return p;
    }
    uint32_t index;
    if (type == 2) {
        index = **cur;
        *cur += 1;
    } else {
        index = (*cur)[0] << 8 | (*cur)[1];
        *cur += 2;
    }
    (void)tmp;
    (void)size;
    /* MEM1/MEM2 are contiguous on the host: read the attribute in place */
    return host_ptr((g_gx.cp_array_base[array] + index * g_gx.cp_array_stride[array]) | 0x80000000u);
}

/* --------------------------------------------------------- per-draw setup
 * Vertex layout (CP) and transform state (XF regs) only change on register
 * writes, which set g_gx_vtx_dirty; everything derived from them is decoded
 * once here instead of for every vertex. */

typedef struct {
    uint8_t type, fmt, n, array;     /* type: 0 none, 1 direct, 2 index8, 3 index16 */
    uint32_t size;
    float scale;
} Attr;

typedef struct {
    int valid, nrm_idx3;
    uint32_t pnmtx_direct, texmtx_direct;   /* bit i: texmtx index i in the stream */
    Attr pos, nrm, col[2], tex[8];
    uint32_t ntex;                          /* last tex attribute present + 1 */
    uint32_t vsize;                         /* bytes per vertex in the stream */
} VtxPlan;

static VtxPlan s_vplan[8];
int g_gx_vtx_dirty = 1;

static struct {
    float P[6];
    int persp, nchan, ngen, dual, need_normal;
    uint32_t cc[2][2];                      /* [channel][color, alpha] control */
    float mat[2][4], amb[2][4];
    uint32_t gen[8], post[8];
    uint32_t pnmtx, texmtx[8];              /* defaults from CP matidx */
} s_xp;

static void unpack_rgba(uint32_t c, float *out);

static void set_attr(Attr *at, uint32_t type, uint32_t fmt, int n, float scale, int array)
{
    at->type = (uint8_t)type;
    at->fmt = (uint8_t)fmt;
    at->n = (uint8_t)n;
    at->array = (uint8_t)array;
    at->scale = scale;
    at->size = n * comp_size(fmt);
}

static void build_vplan(int vat)
{
    VtxPlan *pl = &s_vplan[vat];
    uint32_t a = g_gx.cp_vat[0][vat], lo = g_gx.cp_vcd_lo, hi = g_gx.cp_vcd_hi;
    memset(pl, 0, sizeof *pl);
    pl->pnmtx_direct = lo & 1;
    pl->texmtx_direct = (lo >> 1) & 0xFF;
    set_attr(&pl->pos, BITS(lo, 9, 2), BITS(a, 1, 3), BITS(a, 0, 1) ? 3 : 2,
             1.0f / (1 << BITS(a, 4, 5)), 0);
    {
        uint32_t t = BITS(lo, 11, 2), fmt = BITS(a, 10, 3);
        int nbt = BITS(a, 9, 1);
        float scale = fmt <= 1 ? 1.0f / 64 : fmt <= 3 ? 1.0f / 16384 : 1.0f;
        pl->nrm_idx3 = nbt && BITS(a, 31, 1) && t != 1;
        set_attr(&pl->nrm, t, fmt, pl->nrm_idx3 ? 3 : nbt ? 9 : 3, scale, 1);
    }
    for (int i = 0; i < 2; i++) {
        Attr *at = &pl->col[i];
        at->type = (uint8_t)BITS(lo, 13 + 2 * i, 2);
        at->fmt = (uint8_t)BITS(a, 14 + 4 * i, 3);
        at->array = (uint8_t)(2 + i);
        at->size = color_size(at->fmt);
    }
    for (int i = 0; i < 8; i++) {
        TexFmt f = tex_fmt(vat, i);
        set_attr(&pl->tex[i], BITS(hi, 2 * i, 2), f.fmt, f.cnt ? 2 : 1, 1.0f / (1 << f.frac), 4 + i);
        if (pl->tex[i].type)
            pl->ntex = i + 1;
    }
    pl->vsize = compute_vertex_size(vat);
    pl->valid = 1;
}

static void setup_xf(void)
{
    static int nolight = -1;
    if (nolight < 0)
        nolight = getenv("NSMBW_NOLIGHT") != NULL;
    for (int i = 0; i < 6; i++)
        s_xp.P[i] = xfr(XF_PROJECTION + i);
    s_xp.persp = g_gx.xf_regs[XF_PROJECTION + 6] == 0;
    s_xp.nchan = g_gx.xf_regs[XF_NUM_COLORS] & 3;
    if (s_xp.nchan > 2)
        s_xp.nchan = 2;
    s_xp.need_normal = 0;
    for (int ch = 0; ch < 2; ch++) {
        s_xp.cc[ch][0] = g_gx.xf_regs[XF_COLOR0_CNTRL + ch];
        s_xp.cc[ch][1] = g_gx.xf_regs[XF_ALPHA0_CNTRL + ch];
        if (nolight) {
            s_xp.cc[ch][0] &= ~2u;
            s_xp.cc[ch][1] &= ~2u;
        }
        unpack_rgba(g_gx.xf_regs[XF_MATERIAL0 + ch], s_xp.mat[ch]);
        unpack_rgba(g_gx.xf_regs[XF_AMBIENT0 + ch], s_xp.amb[ch]);
        if (ch < s_xp.nchan && ((s_xp.cc[ch][0] | s_xp.cc[ch][1]) & 2))
            s_xp.need_normal = 1;
    }
    s_xp.ngen = g_gx.xf_regs[XF_NUM_TEXGENS] & 15;
    if (s_xp.ngen > 8)
        s_xp.ngen = 8;
    s_xp.dual = g_gx.xf_regs[XF_DUALTEX] & 1;
    for (int i = 0; i < 8; i++) {
        s_xp.gen[i] = g_gx.xf_regs[XF_TEXGEN0 + i];
        s_xp.post[i] = g_gx.xf_regs[XF_POSTMTX0 + i];
    }
    s_xp.pnmtx = BITS(g_gx.cp_matidx_a, 0, 6);
    for (int i = 0; i < 4; i++)
        s_xp.texmtx[i] = BITS(g_gx.cp_matidx_a, 6 + 6 * i, 6);
    for (int i = 0; i < 4; i++)
        s_xp.texmtx[4 + i] = BITS(g_gx.cp_matidx_b, 6 * i, 6);
}

static const VtxPlan *draw_setup(int vat)
{
    if (g_gx_vtx_dirty) {
        for (int i = 0; i < 8; i++)
            s_vplan[i].valid = 0;
        setup_xf();
        g_gx_vtx_dirty = 0;
    }
    if (!s_vplan[vat].valid)
        build_vplan(vat);
    return &s_vplan[vat];
}

uint32_t gx_vertex_size(int vat) { return draw_setup(vat)->vsize; }

static const uint8_t *load_vertex(const VtxPlan *pl, const uint8_t *p, RawVertex *v)
{
    uint8_t tmp[64];
    v->pnmtx = s_xp.pnmtx;
    memcpy(v->texmtx, s_xp.texmtx, sizeof v->texmtx);
    if (pl->pnmtx_direct)
        v->pnmtx = *p++ & 63;
    if (pl->texmtx_direct)
        for (int i = 0; i < 8; i++)
            if ((pl->texmtx_direct >> i) & 1)
                v->texmtx[i] = *p++ & 63;

    v->pos[2] = 0;
    if (pl->pos.type)
        read_comps(attr_ptr(pl->pos.type, &p, 0, tmp, pl->pos.size), pl->pos.fmt, pl->pos.scale,
                   pl->pos.n, v->pos);
    if (pl->nrm.type) {
        if (pl->nrm_idx3) {
            for (int k = 0; k < 3; k++)
                read_comps(attr_ptr(pl->nrm.type, &p, 1, tmp, pl->nrm.size), pl->nrm.fmt, pl->nrm.scale, 3,
                           v->nrm[k]);
        } else {
            read_comps(attr_ptr(pl->nrm.type, &p, 1, tmp, pl->nrm.size), pl->nrm.fmt, pl->nrm.scale,
                       pl->nrm.n, v->nrm[0]);
            if (pl->nrm.n == 3)
                memset(v->nrm[1], 0, sizeof v->nrm[1] * 2);
        }
    } else {
        memset(v->nrm, 0, sizeof v->nrm);
    }
    for (int i = 0; i < 2; i++) {
        const Attr *at = &pl->col[i];
        if (at->type)
            read_color(attr_ptr(at->type, &p, at->array, tmp, at->size), at->fmt, v->col[i]);
        else
            v->col[i][0] = v->col[i][1] = v->col[i][2] = v->col[i][3] = 1.0f;
    }
    for (uint32_t i = 0; i < pl->ntex; i++) {
        const Attr *at = &pl->tex[i];
        v->tex[i][1] = 0;
        if (at->type)
            read_comps(attr_ptr(at->type, &p, at->array, tmp, at->size), at->fmt, at->scale, at->n, v->tex[i]);
        else
            v->tex[i][0] = 0;
    }
    for (uint32_t i = pl->ntex; i < 8; i++)
        v->tex[i][0] = v->tex[i][1] = 0;
    return p;
}

/* ------------------------------------------------------------- transform */

static void mul34(uint32_t base, const float *in, float *out)   /* 3x4 matrix rows at xf base */
{
    for (int r = 0; r < 3; r++)
        out[r] = xf(base + r * 4) * in[0] + xf(base + r * 4 + 1) * in[1] +
                 xf(base + r * 4 + 2) * in[2] + xf(base + r * 4 + 3) * in[3];
}

static void normalize3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}

static void unpack_rgba(uint32_t c, float *out)
{
    out[0] = (c >> 24) / 255.0f;
    out[1] = ((c >> 16) & 255) / 255.0f;
    out[2] = ((c >> 8) & 255) / 255.0f;
    out[3] = (c & 255) / 255.0f;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* lighting for one channel component set (rgb or alpha) */
static void light_channel(uint32_t cntrl, const float *vcol, const float *mat_reg, const float *amb_reg,
                          const float *pos, const float *nrm, int alpha, float *out)
{
    const float *mat = cntrl & 1 ? vcol : mat_reg;
    if (!(cntrl & 2)) {
        if (alpha) out[3] = mat[3];
        else memcpy(out, mat, 3 * sizeof(float));
        return;
    }
    const float *amb = cntrl & 0x40 ? vcol : amb_reg;
    float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
    uint32_t mask = BITS(cntrl, 2, 4) | BITS(cntrl, 11, 4) << 4;
    uint32_t diffuse = BITS(cntrl, 7, 2), attnfn = BITS(cntrl, 9, 2);
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1 << i)))
            continue;
        uint32_t L = 0x600 + i * 0x10;
        float lcol[4], lpos[3] = {xf(L + 10), xf(L + 11), xf(L + 12)};
        float ldirv[3] = {xf(L + 13), xf(L + 14), xf(L + 15)};
        unpack_rgba(g_gx.xf_mem[L + 3], lcol);
        float ldir[3], attn = 1.0f;
        if (attnfn == 1) {                         /* specular */
            ldir[0] = lpos[0]; ldir[1] = lpos[1]; ldir[2] = lpos[2];
            normalize3(ldir);
            float nd = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            float h = nd >= 0 ? nrm[0] * ldirv[0] + nrm[1] * ldirv[1] + nrm[2] * ldirv[2] : 0;
            if (h < 0) h = 0;
            float ca = xf(L + 4) + xf(L + 5) * h + xf(L + 6) * h * h;
            float da = xf(L + 7) + xf(L + 8) * h + xf(L + 9) * h * h;
            attn = (ca < 0 ? 0 : ca) / (da != 0 ? da : 1);
        } else if (attnfn == 3) {                  /* spotlight with distance attenuation */
            ldir[0] = lpos[0] - pos[0]; ldir[1] = lpos[1] - pos[1]; ldir[2] = lpos[2] - pos[2];
            float d2 = ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2], d = sqrtf(d2);
            if (d > 0) { ldir[0] /= d; ldir[1] /= d; ldir[2] /= d; }
            float c = ldir[0] * ldirv[0] + ldir[1] * ldirv[1] + ldir[2] * ldirv[2];
            if (c < 0) c = 0;
            float ca = xf(L + 4) + xf(L + 5) * c + xf(L + 6) * c * c;
            float da = xf(L + 7) + xf(L + 8) * d + xf(L + 9) * d2;
            attn = (ca < 0 ? 0 : ca) / (da != 0 ? da : 1);
        } else {                                   /* none / directional */
            ldir[0] = lpos[0] - pos[0]; ldir[1] = lpos[1] - pos[1]; ldir[2] = lpos[2] - pos[2];
            normalize3(ldir);
        }
        float k = attn;
        if (diffuse) {
            float nd = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            if (diffuse == 2 && nd < 0) nd = 0;
            k *= nd;
        }
        for (int c = 0; c < 4; c++)
            acc[c] += k * lcol[c];
    }
    if (alpha) out[3] = mat[3] * clamp01(acc[3]);
    else for (int c = 0; c < 3; c++) out[c] = mat[c] * clamp01(acc[c]);
}

int g_gx_debug_vertex;

static void transform(const RawVertex *in, GXVertex *out)
{
    float p4[4] = {in->pos[0], in->pos[1], in->pos[2], 1.0f}, view[3];
    mul34(in->pnmtx * 4, p4, view);

    /* projection */
    const float *P = s_xp.P;
    if (s_xp.persp) {
        out->pos[0] = P[0] * view[0] + P[1] * view[2];
        out->pos[1] = P[2] * view[1] + P[3] * view[2];
        out->pos[2] = P[4] * view[2] + P[5];
        out->pos[3] = -view[2];
    } else {
        out->pos[0] = P[0] * view[0] + P[1];
        out->pos[1] = P[2] * view[1] + P[3];
        out->pos[2] = P[4] * view[2] + P[5];
        out->pos[3] = 1.0f;
    }

    /* normal (eye space) */
    float n[3] = {0, 0, 0};
    if (s_xp.need_normal) {
        uint32_t nb = 0x400 + in->pnmtx * 3;
        for (int r = 0; r < 3; r++)
            n[r] = xf(nb + r * 3) * in->nrm[0][0] + xf(nb + r * 3 + 1) * in->nrm[0][1] + xf(nb + r * 3 + 2) * in->nrm[0][2];
        normalize3(n);
    }

    /* color channels */
    for (int ch = 0; ch < 2; ch++) {
        float *o = out->color[ch];
        if (ch >= s_xp.nchan) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        light_channel(s_xp.cc[ch][0], in->col[ch], s_xp.mat[ch], s_xp.amb[ch], view, n, 0, o);
        light_channel(s_xp.cc[ch][1], in->col[ch], s_xp.mat[ch], s_xp.amb[ch], view, n, 1, o);
    }

    /* texture coordinate generation */
    int ngen = s_xp.ngen;
    for (int i = ngen; i < 8; i++) {
        float *o = out->tex[i];
        o[0] = o[1] = 0;
        o[2] = 1;
    }
    for (int i = 0; i < ngen; i++) {
        float *o = out->tex[i];
        uint32_t info = s_xp.gen[i];
        uint32_t proj = BITS(info, 1, 1), form = BITS(info, 2, 1), type = BITS(info, 4, 3), row = BITS(info, 7, 5);
        if (type == 2 || type == 3) {              /* color texgen */
            o[0] = out->color[type - 2][0];
            o[1] = out->color[type - 2][1];
            o[2] = 1;
            continue;
        }
        if (type == 1) {                           /* emboss: reuse the source texgen */
            uint32_t s = BITS(info, 12, 3);
            if ((int)s < i) {
                o[0] = out->tex[s][0]; o[1] = out->tex[s][1]; o[2] = out->tex[s][2];
            } else {
                o[0] = o[1] = 0; o[2] = 1;
            }
            continue;
        }
        float src[4] = {0, 0, 1, 1};
        switch (row) {
        case 0: memcpy(src, in->pos, 3 * sizeof(float)); break;
        case 1: memcpy(src, in->nrm[0], 3 * sizeof(float)); break;
        case 2: src[0] = in->col[0][0]; src[1] = in->col[0][1]; src[2] = in->col[0][2]; break;
        case 3: memcpy(src, in->nrm[1], 3 * sizeof(float)); break;
        case 4: memcpy(src, in->nrm[2], 3 * sizeof(float)); break;
        default:
            if (row >= 5 && row <= 12) {
                src[0] = in->tex[row - 5][0];
                src[1] = in->tex[row - 5][1];
            }
        }
        if (!form)
            src[2] = 1.0f;
        uint32_t m = in->texmtx[i] * 4;
        float r[3];
        r[0] = xf(m) * src[0] + xf(m + 1) * src[1] + xf(m + 2) * src[2] + xf(m + 3) * src[3];
        r[1] = xf(m + 4) * src[0] + xf(m + 5) * src[1] + xf(m + 6) * src[2] + xf(m + 7) * src[3];
        r[2] = proj ? xf(m + 8) * src[0] + xf(m + 9) * src[1] + xf(m + 10) * src[2] + xf(m + 11) * src[3] : 1.0f;
        if (s_xp.dual) {
            uint32_t post = s_xp.post[i];
            if (BITS(post, 8, 1))
                normalize3(r);
            float s4[4] = {r[0], r[1], r[2], 1.0f};
            mul34(0x500 + BITS(post, 0, 6) * 4, s4, r);
        }
        o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
    }
}

/* -------------------------------------------------------------- assembly */

static GXVertex *s_verts, *s_tris;
static uint32_t s_cap, s_tri_cap;
static uint32_t *s_idx;
static uint32_t s_idx_cap;

void gx_expand_indexed(const GXVertex *v, const uint32_t *idx, uint32_t nidx,
                       void (*draw)(const GXVertex *, uint32_t))
{
    if (nidx > s_tri_cap) {
        s_tri_cap = nidx + 256;
        s_tris = realloc(s_tris, s_tri_cap * sizeof *s_tris);
    }
    for (uint32_t i = 0; i < nidx; i++)
        s_tris[i] = v[idx[i]];
    draw(s_tris, nidx);
}

void gx_draw_primitive(uint8_t op, const uint8_t *data, uint32_t count)
{
    int vat = op & 7, prim = (op >> 3) & 7;    /* 0 quads,2 tris,3 strip,4 fan,5 lines,6 strip,7 points */
    if (!count)
        return;
    if (count > s_cap) {
        s_cap = count + 256;
        s_verts = realloc(s_verts, s_cap * sizeof *s_verts);
    }
    PROF_BEGIN(tl);
    const VtxPlan *pl = draw_setup(vat);
    for (uint32_t i = 0; i < count; i++) {
        RawVertex raw;
        data = load_vertex(pl, data, &raw);
        transform(&raw, &s_verts[i]);
    }
    uint32_t ntri;
    switch (prim) {
    case 0: case 1: ntri = count / 4 * 2; break;
    case 2: ntri = count / 3; break;
    case 3: case 4: ntri = count >= 3 ? count - 2 : 0; break;
    default: return;                               /* lines and points: not drawn yet */
    }
    if (ntri * 3 > s_idx_cap) {
        s_idx_cap = ntri * 3 + 256;
        s_idx = realloc(s_idx, s_idx_cap * sizeof *s_idx);
    }
    uint32_t *o = s_idx;
    switch (prim) {
    case 0: case 1:
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            *o++ = q; *o++ = q + 1; *o++ = q + 2;
            *o++ = q; *o++ = q + 2; *o++ = q + 3;
        }
        break;
    case 2:
        for (uint32_t i = 0; i < ntri * 3; i++)
            *o++ = i;
        break;
    case 3:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) { *o++ = i + 1; *o++ = i; }
            else { *o++ = i; *o++ = i + 1; }
            *o++ = i + 2;
        }
        break;
    case 4:
        for (uint32_t i = 1; i + 1 < count; i++) {
            *o++ = 0; *o++ = i; *o++ = i + 1;
        }
        break;
    }
    PROF_END(tl, PROF_TL);
    PROF_BEGIN(sub);
    gfx_draw_indexed(s_verts, count, s_idx, (uint32_t)(o - s_idx));
    PROF_END(sub, PROF_SUBMIT);
}
