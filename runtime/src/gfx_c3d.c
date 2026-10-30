/* 3DS render backend (citro3d / PICA200).
 *
 * The EFB is a 512x512 RGBA8 render-target texture: EFB pixel (ex, ey) maps to
 * RT pixel (ex*0.75, (528-ey)*0.75), y up. Vertices come transformed from
 * gx_vertex.c and are mapped to that space on the CPU; GX depth d (0 near)
 * becomes z_ndc = d - 1, so the PICA's reversed depth stores 1 - d.
 * A copy to XFB draws the EFB onto the top screen; copies to textures render
 * the EFB region into a GPU texture registered under the destination address.
 * The TEV is translated to the 6 PICA combiner stages (approximate where the
 * PICA has no equivalent). */
#ifdef __3DS__
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include "runtime.h"
#include "gx_state.h"
#include "vshader_shbin.h"

#define EFB_W 640
#define EFB_H 528
#define RT_SIZE 512
#define RT_SCALE 0.75f

#define VBUF_VERTS 60000

typedef struct {
    float pos[4];
    float col[4];
    float tc[3][2];
} CVert;

static DVLB_s *s_dvlb;
static shaderProgram_s s_prog;
static C3D_AttrInfo s_attr;
static C3D_Tex s_efb_tex;
static C3D_RenderTarget *s_efb_rt, *s_screen;
static CVert *s_vbuf;
static uint32_t s_vpos;
#define IBUF_INDICES 180000
static u16 *s_ibuf;
static uint32_t s_ipos;
static int s_in_frame;
static uint32_t s_frame;

/* ------------------------------------------------------------- textures */

typedef struct {
    uint32_t key;          /* GXTexture key, or EFB copy destination address */
    int efb_copy;
    uint16_t w, h;         /* GX size */
    C3D_Tex tex;           /* power-of-two GPU texture */
    C3D_RenderTarget *rt;  /* for EFB copies, and textures copies land in */
    uint32_t addr, fmt;    /* guest address and GX format of a decoded texture */
    uint16_t gw, gh;       /* its GX size (w, h can be half of it) */
    uint32_t last_use;
    int used;
    int next;              /* hash chain, -1 end */
} GpuTex;

#define GPU_TEX_SLOTS 384
static GpuTex s_gtex[GPU_TEX_SLOTS];

/* lookups by (key, kind, size) through hash chains: a linear scan of all
 * slots for every texture of every draw cost tens of ms per frame */
#define GTEX_BUCKETS 512
static int s_gbucket[GTEX_BUCKETS];
static int s_ghash_ready;

static uint32_t gtex_bucket(uint32_t key, int efb, int w, int h)
{
    uint32_t x = key * 2654435761u ^ (uint32_t)efb * 0x9E3779B9u ^ (uint32_t)(w << 16 | h) * 40503u;
    return (x ^ (x >> 15)) % GTEX_BUCKETS;
}

static void ghash_init(void)
{
    for (int i = 0; i < GTEX_BUCKETS; i++)
        s_gbucket[i] = -1;
    s_ghash_ready = 1;
}

static void ghash_insert(GpuTex *g)
{
    uint32_t b = gtex_bucket(g->key, g->efb_copy, g->w, g->h);
    g->next = s_gbucket[b];
    s_gbucket[b] = (int)(g - s_gtex);
}

static void ghash_remove(GpuTex *g)
{
    int *link = &s_gbucket[gtex_bucket(g->key, g->efb_copy, g->w, g->h)], idx = (int)(g - s_gtex);
    while (*link >= 0) {
        if (*link == idx) {
            *link = g->next;
            return;
        }
        link = &s_gtex[*link].next;
    }
}

static GpuTex *ghash_find(uint32_t key, int efb, int w, int h)
{
    if (!s_ghash_ready)
        ghash_init();
    for (int i = s_gbucket[gtex_bucket(key, efb, w, h)]; i >= 0; i = s_gtex[i].next) {
        GpuTex *g = &s_gtex[i];
        if (g->key == key && g->efb_copy == efb && g->w == w && g->h == h)
            return g;
    }
    return NULL;
}

/* Textures and render targets still referenced by submitted GPU commands
 * (and citro3d refuses to delete a render target inside a frame): freed two
 * frames later, outside a frame. */
typedef struct { C3D_Tex tex; C3D_RenderTarget *rt; uint32_t frame; } Retired;
static Retired s_gretired[GPU_TEX_SLOTS * 2];
static int s_ngretired;

static void free_retired(int all)
{
    int keep = 0;
    for (int i = 0; i < s_ngretired; i++) {
        Retired *r = &s_gretired[i];
        if (all || r->frame + 2 <= s_frame) {
            if (r->rt)
                C3D_RenderTargetDelete(r->rt);
            C3D_TexDelete(&r->tex);
        } else {
            s_gretired[keep++] = *r;
        }
    }
    s_ngretired = keep;
}
static uint32_t s_gtex_bytes;
#define GPU_TEX_BUDGET (14u << 20)   /* linear memory: ~29 MiB free, 4.8 MiB vertex buffer */

static int pow2(int v)
{
    int p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

static void gtex_free(GpuTex *g)
{
    if (!g->used)
        return;
    ghash_remove(g);
    if (s_ngretired < (int)(sizeof s_gretired / sizeof s_gretired[0])) {
        Retired *r = &s_gretired[s_ngretired++];
        r->tex = g->tex;
        r->rt = g->rt;
        r->frame = s_frame;
    }
    s_gtex_bytes -= g->tex.width * g->tex.height * 4;
    memset(g, 0, sizeof *g);
}

static GpuTex *gtex_alloc(void)
{
    GpuTex *best = NULL;
    for (int i = 0; i < GPU_TEX_SLOTS; i++) {
        if (!s_gtex[i].used)
            return &s_gtex[i];
        if (!best || s_gtex[i].last_use < best->last_use)
            best = &s_gtex[i];
    }
    gtex_free(best);
    return best;
}

static void evict_to_budget(void)
{
    while (s_gtex_bytes > GPU_TEX_BUDGET) {
        GpuTex *old = NULL;
        for (int i = 0; i < GPU_TEX_SLOTS; i++)
            if (s_gtex[i].used && s_gtex[i].last_use + 2 < s_frame && (!old || s_gtex[i].last_use < old->last_use))
                old = &s_gtex[i];
        if (!old)
            break;
        gtex_free(old);
    }
}

static inline uint32_t morton8(uint32_t x, uint32_t y)
{
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

/* linear RGBA (R first in memory) -> tiled GPU_RGBA8 (A,B,G,R bytes) */
static void upload_rgba(C3D_Tex *tex, const uint32_t *src, int w, int h)
{
    int W = tex->width, H = tex->height;
    uint32_t *dst = (uint32_t *)tex->data;
    for (int y = 0; y < H; y++) {
        int sy = y < h ? y : h - 1;
        for (int x = 0; x < W; x++) {
            int sx = x < w ? x : w - 1;
            uint32_t c = src[sy * w + sx];
            uint32_t idx = ((uint32_t)(y >> 3) * (W >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7);
            dst[idx] = __builtin_bswap32(c);
        }
    }
    GSPGPU_FlushDataCache(tex->data, tex->size);
}

static GpuTex *find_efb_copy(uint32_t addr, int w, int h) { return ghash_find(addr, 1, w, h); }

static GpuTex *gpu_texture(int unit)
{
    uint32_t img0 = g_gx.bp[BP_TX_IMAGE0(unit)];
    uint32_t addr = (g_gx.bp[BP_TX_IMAGE3(unit)] & 0xFFFFFF) << 5;
    int w = (int)BITS(img0, 0, 10) + 1, h = (int)BITS(img0, 10, 10) + 1;
    GpuTex *g = find_efb_copy(addr, w, h);
    if (g) {
        g->last_use = s_frame;
        return g;
    }
    const GXTexture *t = gx_get_texture(unit);
    if (!t)
        return NULL;
    g = ghash_find(t->key, 0, t->width, t->height);
    if (g) {
        g->last_use = s_frame;
        return g;
    }
    PROF_BEGIN(up);
    g = gtex_alloc();
    if (!C3D_TexInit(&g->tex, (u16)pow2(t->width), (u16)pow2(t->height), GPU_RGBA8)) {
        static int warned;
        if (!warned++)
            rt_log("gfx: texture allocation failed (%dx%d, linear free %lu KiB)", t->width, t->height,
                   (unsigned long)(linearSpaceFree() >> 10));
        return NULL;
    }
    g->used = 1;
    g->key = t->key;
    g->w = t->width;
    g->h = t->height;
    g->addr = addr;
    g->fmt = BITS(img0, 20, 4);
    g->gw = (uint16_t)w;
    g->gh = (uint16_t)h;
    g->last_use = s_frame;
    ghash_insert(g);
    s_gtex_bytes += g->tex.width * g->tex.height * 4;
    upload_rgba(&g->tex, t->rgba, t->width, t->height);
    evict_to_budget();
    PROF_END(up, PROF_TEX);
    return g;
}

/* ---------------------------------------------------------------- frame */

static void begin_frame(void)
{
    if (s_in_frame)
        return;
    PROF_BEGIN(w);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    PROF_END(w, PROF_GPUWAIT);
    C3D_FrameDrawOn(s_efb_rt);
    s_vpos = 0;
    s_ipos = 0;
    s_in_frame = 1;
}

/* Heavy frames outgrow the command buffer (citro3d panics) or the vertex
 * buffer: submit what we have, wait for the GPU, and continue on the EFB.
 * Only the screen target is shown, so this does not present anything. */
static void ensure_space(uint32_t nverts, uint32_t nidx)
{
    /* C3D_GetCmdBufUsage() reports the previous frame: ask libctru directly */
    u32 *buf, size, offset;
    GPUCMD_GetBuffer(&buf, &size, &offset);
    if (!s_in_frame || (offset < size - size / 4 && s_vpos + nverts <= VBUF_VERTS && s_ipos + nidx <= IBUF_INDICES))
        return;
    PROF_BEGIN(w);
    C3D_FrameEnd(0);
    C3D_FrameBegin(0);
    PROF_END(w, PROF_GPUWAIT);
    C3D_FrameDrawOn(s_efb_rt);
    s_vpos = 0;
    s_ipos = 0;
}

static CVert *alloc_verts(uint32_t n)
{
    ensure_space(n, 0);
    if (s_vpos + n > VBUF_VERTS)
        return NULL;
    CVert *v = &s_vbuf[s_vpos];
    s_vpos += n;
    return v;
}

static void draw_verts(CVert *v, uint32_t n)
{
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, v, sizeof(CVert), 5, 0x43210);
    C3D_DrawArrays(GPU_TRIANGLES, 0, (int)n);
}

void gfx_init(void)
{
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8);
    s_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);

    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 4);   /* position */
    AttrInfo_AddLoader(&s_attr, 1, GPU_FLOAT, 4);   /* color */
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);   /* tc0 */
    AttrInfo_AddLoader(&s_attr, 3, GPU_FLOAT, 2);   /* tc1 */
    AttrInfo_AddLoader(&s_attr, 4, GPU_FLOAT, 2);   /* tc2 */
    C3D_SetAttrInfo(&s_attr);

    rt_log("gfx: linear memory free %lu KiB before buffers", (unsigned long)(linearSpaceFree() >> 10));
    s_vbuf = linearAlloc(sizeof(CVert) * VBUF_VERTS);
    s_ibuf = linearAlloc(sizeof(u16) * IBUF_INDICES);
    if (!s_vbuf || !s_ibuf)
        rt_fatal("gfx: no linear memory for the vertex buffers");

    C3D_TexInitVRAM(&s_efb_tex, RT_SIZE, RT_SIZE, GPU_RGBA8);
    s_efb_rt = C3D_RenderTargetCreateFromTex(&s_efb_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    C3D_TexSetFilter(&s_efb_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_efb_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    s_screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_screen, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_DepthMap(true, -1.0f, 0.0f);
}

/* --------------------------------------------------------- pipeline state */

static GPU_TESTFUNC gx_depth_func(uint32_t f)
{
    /* stored depth is 1 - d: comparisons flip direction */
    static const GPU_TESTFUNC map[8] = {GPU_NEVER, GPU_GREATER, GPU_EQUAL, GPU_GEQUAL,
                                        GPU_LESS, GPU_NOTEQUAL, GPU_LEQUAL, GPU_ALWAYS};
    return map[f & 7];
}

static GPU_TESTFUNC gx_alpha_func(uint32_t f)
{
    static const GPU_TESTFUNC map[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
                                        GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    return map[f & 7];
}

static GPU_BLENDFACTOR gx_blend_factor(uint32_t f, int is_src)
{
    switch (f & 7) {
    case 0: return GPU_ZERO;
    case 1: return GPU_ONE;
    case 2: return is_src ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case 3: return is_src ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case 4: return GPU_SRC_ALPHA;
    case 5: return GPU_ONE_MINUS_SRC_ALPHA;
    case 6: return GPU_DST_ALPHA;
    default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}

static void apply_fragment_ops(void)
{
    uint32_t zm = g_gx.bp[BP_ZMODE], cm = g_gx.bp[BP_CMODE0], gen = g_gx.bp[BP_GENMODE];
    int mask = 0;
    if (BITS(cm, 3, 1)) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if (BITS(cm, 4, 1)) mask |= GPU_WRITE_ALPHA;
    if (BITS(zm, 0, 1) && BITS(zm, 4, 1)) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(BITS(zm, 0, 1), gx_depth_func(BITS(zm, 1, 3)), (GPU_WRITEMASK)mask);

    uint32_t cull = BITS(gen, 14, 2);
    C3D_CullFace(cull == 1 ? GPU_CULL_FRONT_CCW : cull == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);

    if (BITS(cm, 0, 1)) {
        GPU_BLENDEQUATION eq = BITS(cm, 11, 1) ? GPU_BLEND_REVERSE_SUBTRACT : GPU_BLEND_ADD;
        GPU_BLENDFACTOR sf = BITS(cm, 11, 1) ? GPU_ONE : gx_blend_factor(BITS(cm, 8, 3), 1);
        GPU_BLENDFACTOR df = BITS(cm, 11, 1) ? GPU_ONE : gx_blend_factor(BITS(cm, 5, 3), 0);
        C3D_AlphaBlend(eq, eq, sf, df, sf, df);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }

    /* alpha test: two comparisons combined; keep the meaningful one */
    uint32_t ac = g_gx.bp[BP_ALPHACOMPARE];
    uint32_t c0 = BITS(ac, 16, 3), c1 = BITS(ac, 19, 3), logic = BITS(ac, 22, 2);
    uint32_t r0 = BITS(ac, 0, 8), r1 = BITS(ac, 8, 8);
    if (c0 == 7 && c1 == 7)
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if (logic == 0 && c1 == 7)
        C3D_AlphaTest(true, gx_alpha_func(c0), (int)r0);
    else if (logic == 0 && c0 == 7)
        C3D_AlphaTest(true, gx_alpha_func(c1), (int)r1);
    else if (logic == 1 && c1 == 0)
        C3D_AlphaTest(true, gx_alpha_func(c0), (int)r0);
    else if (logic == 1 && c0 == 0)
        C3D_AlphaTest(true, gx_alpha_func(c1), (int)r1);
    else
        C3D_AlphaTest(true, gx_alpha_func(c0), (int)r0);
}

/* ---------------------------------------------------- TEV -> combiners */

enum { IN_ZERO, IN_ONE, IN_HALF, IN_SRC };

typedef struct {
    int kind;
    int reg;          /* -1: texture/raster/konst; 0-3: TEV register */
    GPU_TEVSRC src;
    int op;           /* GPU_TEVOP_RGB_* or GPU_TEVOP_A_* (non-inverted) */
    uint32_t constant; /* for constant-backed inputs */
    int needs_const;
} In;

typedef struct {
    int pica_stage;   /* stage whose output holds the value, -1: initial (BP) */
} RegState;

typedef struct {
    int unit[8];          /* texmap -> PICA unit (-1 none) */
    int unit_coord[3];    /* PICA unit -> GX texcoord */
    int unit_flip[3];     /* uploaded texture: the PICA samples rows bottom-up */
    int unit_map[3];
    int nunits;
    int chan;             /* rasterized color channel passed as primary color */
} DrawSetup;

static uint32_t pack_rgba8(const float *c)
{
    uint32_t r = (uint32_t)(fminf(fmaxf(c[0], 0), 1) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(fminf(fmaxf(c[1], 0), 1) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(fminf(fmaxf(c[2], 0), 1) * 255.0f + 0.5f);
    uint32_t a = (uint32_t)(fminf(fmaxf(c[3], 0), 1) * 255.0f + 0.5f);
    return r | g << 8 | b << 16 | a << 24;
}

static float konst_frac(uint32_t sel)
{
    static const float f[8] = {1.0f, 7 / 8.0f, 6 / 8.0f, 5 / 8.0f, 4 / 8.0f, 3 / 8.0f, 2 / 8.0f, 1 / 8.0f};
    return f[sel & 7];
}

static void konst_rgb(uint32_t sel, float out[3])
{
    if (sel < 8) out[0] = out[1] = out[2] = konst_frac(sel);
    else if (sel >= 0x0C && sel <= 0x0F) memcpy(out, g_gx.tev_konst[sel - 0x0C], 3 * sizeof(float));
    else if (sel >= 0x10) out[0] = out[1] = out[2] = g_gx.tev_konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
    else out[0] = out[1] = out[2] = 0;
}

static float konst_a(uint32_t sel)
{
    if (sel < 8) return konst_frac(sel);
    if (sel >= 0x10) return g_gx.tev_konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
    return 0;
}

/* swap table lookup: which source component feeds output component k */
static int swap_comp(int table, int k)
{
    uint32_t k0 = g_gx.bp[BP_KSEL(table * 2)], k1 = g_gx.bp[BP_KSEL(table * 2 + 1)];
    switch (k) {
    case 0: return (int)BITS(k0, 0, 2);
    case 1: return (int)BITS(k0, 2, 2);
    case 2: return (int)BITS(k1, 0, 2);
    default: return (int)BITS(k1, 2, 2);
    }
}

typedef struct {
    C3D_TexEnv env[6];
    int n;
    int buf_mask_rgb, buf_mask_a;
    uint32_t buf_color;
} Program;

/* source for a TEV register read while emitting PICA stage `cur` */
static void reg_source(const RegState *rs, int reg, int alpha, int cur, int buffered, In *in)
{
    int st = rs[reg].pica_stage;
    if (st < 0) {
        in->kind = IN_SRC;
        in->src = GPU_CONSTANT;
        in->needs_const = 1;
        in->constant = pack_rgba8(g_gx.tev_reg[reg]);
        return;
    }
    in->kind = IN_SRC;
    in->src = (st == cur - 1) ? GPU_PREVIOUS : (st == buffered ? GPU_PREVIOUS_BUFFER : GPU_PREVIOUS);
    (void)alpha;
}

static void build_input(int is_alpha, uint32_t sel, int stage_tex_unit, int tswap, int rswap,
                        uint32_t kc, uint32_t ka, const RegState *rs_rgb, const RegState *rs_a,
                        int cur, int buffered, In *in)
{
    memset(in, 0, sizeof *in);
    in->reg = -1;
    in->op = 0;
    if (!is_alpha) {
        switch (sel) {
        case 0: case 2: case 4: case 6:
            reg_source(rs_rgb, (int)sel / 2, 0, cur, buffered, in);
            in->op = GPU_TEVOP_RGB_SRC_COLOR;
            return;
        case 1: case 3: case 5: case 7:
            reg_source(rs_a, (int)sel / 2, 1, cur, buffered, in);
            in->op = GPU_TEVOP_RGB_SRC_ALPHA;
            return;
        case 8: case 9:
            if (stage_tex_unit < 0) { in->kind = IN_ONE; return; }
            in->kind = IN_SRC;
            in->src = (GPU_TEVSRC)(GPU_TEXTURE0 + stage_tex_unit);
            if (sel == 9) {
                int c = swap_comp(tswap, 3);
                in->op = c == 3 ? GPU_TEVOP_RGB_SRC_ALPHA : c == 0 ? GPU_TEVOP_RGB_SRC_R :
                         c == 1 ? GPU_TEVOP_RGB_SRC_G : GPU_TEVOP_RGB_SRC_B;
            } else {
                int r = swap_comp(tswap, 0), g = swap_comp(tswap, 1), b = swap_comp(tswap, 2);
                if (r == g && g == b)
                    in->op = r == 0 ? GPU_TEVOP_RGB_SRC_R : r == 1 ? GPU_TEVOP_RGB_SRC_G :
                             r == 2 ? GPU_TEVOP_RGB_SRC_B : GPU_TEVOP_RGB_SRC_ALPHA;
                else
                    in->op = GPU_TEVOP_RGB_SRC_COLOR;
            }
            return;
        case 10: case 11:
            in->kind = IN_SRC;
            in->src = GPU_PRIMARY_COLOR;
            in->op = sel == 11 ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
            (void)rswap;
            return;
        case 12: in->kind = IN_ONE; return;
        case 13: in->kind = IN_HALF; return;
        case 14: {
            float rgb[3];
            konst_rgb(kc, rgb);
            float c[4] = {rgb[0], rgb[1], rgb[2], konst_a(ka)};
            in->kind = IN_SRC;
            in->src = GPU_CONSTANT;
            in->needs_const = 1;
            in->constant = pack_rgba8(c);
            in->op = GPU_TEVOP_RGB_SRC_COLOR;
            return;
        }
        default: in->kind = IN_ZERO; return;
        }
    }
    switch (sel) {
    case 0: case 1: case 2: case 3:
        reg_source(rs_a, (int)sel, 1, cur, buffered, in);
        in->op = GPU_TEVOP_A_SRC_ALPHA;
        return;
    case 4: {
        if (stage_tex_unit < 0) { in->kind = IN_ONE; return; }
        in->kind = IN_SRC;
        in->src = (GPU_TEVSRC)(GPU_TEXTURE0 + stage_tex_unit);
        int c = swap_comp(tswap, 3);
        in->op = c == 3 ? GPU_TEVOP_A_SRC_ALPHA : c == 0 ? GPU_TEVOP_A_SRC_R : c == 1 ? GPU_TEVOP_A_SRC_G : GPU_TEVOP_A_SRC_B;
        return;
    }
    case 5:
        in->kind = IN_SRC;
        in->src = GPU_PRIMARY_COLOR;
        in->op = GPU_TEVOP_A_SRC_ALPHA;
        return;
    case 6: {
        float c[4] = {0, 0, 0, konst_a(ka)};
        in->kind = IN_SRC;
        in->src = GPU_CONSTANT;
        in->needs_const = 1;
        in->constant = pack_rgba8(c);
        in->op = GPU_TEVOP_A_SRC_ALPHA;
        return;
    }
    default: in->kind = IN_ZERO; return;
    }
}

/* one PICA operation: func(s0, s1, s2) with operands */
typedef struct {
    GPU_COMBINEFUNC func;
    In s[3];
    int inv[3];         /* use one-minus operand */
    int prev_op[3];     /* source is the result of the previous op of this channel */
} Op;

/* reduces d +/- lerp(a,b,c) to at most two PICA ops */
static int plan_channel(In a, In b, In c, In d, int sub, uint32_t bias, Op *ops)
{
    memset(ops, 0, 2 * sizeof *ops);
    In L1 = {0}, L2 = {0};
    int lkind;                          /* 0 zero, 1 single, 2 mul, 3 mul inv, 4 lerp */
    if (c.kind == IN_ZERO) { L1 = a; lkind = a.kind == IN_ZERO ? 0 : 1; }
    else if (c.kind == IN_ONE) { L1 = b; lkind = b.kind == IN_ZERO ? 0 : 1; }
    else if (a.kind == IN_ZERO && b.kind == IN_ZERO) lkind = 0;
    else if (a.kind == IN_ZERO) { L1 = b; L2 = c; lkind = b.kind == IN_ONE ? (L1 = c, 1) : 2; }
    else if (b.kind == IN_ZERO) { L1 = a; L2 = c; lkind = 3; }
    else lkind = 4;

    int dz = d.kind == IN_ZERO;
    if (lkind == 0) {
        ops[0].func = GPU_REPLACE;
        ops[0].s[0] = dz ? (In){IN_ZERO} : d;
        return 1;
    }
    if (dz && !sub) {
        switch (lkind) {
        case 1: ops[0].func = GPU_REPLACE; ops[0].s[0] = L1; return 1;
        case 2: ops[0].func = GPU_MODULATE; ops[0].s[0] = L1; ops[0].s[1] = L2; return 1;
        case 3: ops[0].func = GPU_MODULATE; ops[0].s[0] = L1; ops[0].s[1] = L2; ops[0].inv[1] = 1; return 1;
        default: ops[0].func = GPU_INTERPOLATE; ops[0].s[0] = b; ops[0].s[1] = a; ops[0].s[2] = c; return 1;
        }
    }
    if (dz && sub) {
        ops[0].func = GPU_REPLACE;
        ops[0].s[0] = (In){IN_ZERO};
        return 1;
    }
    if (!sub) {
        if (lkind == 1) {
            ops[0].func = bias == 2 ? GPU_ADD_SIGNED : GPU_ADD;
            ops[0].s[0] = d; ops[0].s[1] = L1;
            return 1;
        }
        if (lkind == 2 || lkind == 3) {
            ops[0].func = GPU_MULTIPLY_ADD;
            ops[0].s[0] = L1; ops[0].s[1] = L2; ops[0].s[2] = d;
            ops[0].inv[1] = lkind == 3;
            return 1;
        }
        ops[0].func = GPU_INTERPOLATE; ops[0].s[0] = b; ops[0].s[1] = a; ops[0].s[2] = c;
        ops[1].func = GPU_ADD; ops[1].s[0] = d; ops[1].prev_op[1] = 1;
        return 2;
    }
    if (lkind == 1) {
        ops[0].func = GPU_SUBTRACT;
        ops[0].s[0] = d; ops[0].s[1] = L1;
        return 1;
    }
    if (lkind == 2 || lkind == 3) {
        ops[0].func = GPU_MODULATE; ops[0].s[0] = L1; ops[0].s[1] = L2; ops[0].inv[1] = lkind == 3;
    } else {
        ops[0].func = GPU_INTERPOLATE; ops[0].s[0] = b; ops[0].s[1] = a; ops[0].s[2] = c;
    }
    ops[1].func = GPU_SUBTRACT; ops[1].s[0] = d; ops[1].prev_op[1] = 1;
    return 2;
}

/* Constants an operation reads: each PICA stage has one constant color, and
 * the combiner buffer starts out as a second one (until a stage updates the
 * buffer). An operand only reads some bytes of it (rgb, alpha or one
 * component), so two constants can share a color when those bytes agree. */
typedef struct {
    uint32_t val, mask;           /* stage constant and the bytes in use */
    uint32_t *bval, *bmask;       /* buffer color, for the whole program */
    int buf_ok;                   /* the buffer still holds its color */
} Consts;

static uint32_t operand_bytes(int is_alpha, int op)
{
    switch (op & ~1) {
    case 0: return is_alpha ? 0xFF000000 : 0x00FFFFFF;     /* alpha / color */
    case 2: return is_alpha ? 0x000000FF : 0xFF000000;     /* r / alpha */
    case 4: return is_alpha ? 0x0000FF00 : 0x000000FF;     /* g / r */
    case 6: return 0x00FF0000;                             /* alpha op: b */
    case 8: return 0x0000FF00;                             /* rgb op: g */
    default: return 0x00FF0000;                            /* rgb op: b */
    }
}

static int fits(uint32_t *val, uint32_t *mask, uint32_t want, uint32_t bytes)
{
    if ((*val ^ want) & *mask & bytes)
        return 0;
    *val = (*val & ~bytes) | (want & bytes);
    *mask |= bytes;
    return 1;
}

/* resolves an In to a PICA source/operand */
static void resolve(const In *in, int inv, int is_alpha, Consts *k, GPU_TEVSRC *src, int *op)
{
    uint32_t want;
    int base_op = in->op;
    switch (in->kind) {
    case IN_ZERO: want = 0x00000000; base_op = 0; break;
    case IN_ONE: want = 0xFFFFFFFF; base_op = 0; break;
    case IN_HALF: want = 0x80808080; base_op = 0; break;
    default:
        if (in->src != GPU_CONSTANT) {
            *src = in->src;
            *op = base_op + (inv ? 1 : 0);
            return;
        }
        want = in->constant;
    }
    uint32_t bytes = operand_bytes(is_alpha, base_op);
    *src = GPU_CONSTANT;
    if (!fits(&k->val, &k->mask, want, bytes) && k->buf_ok && fits(k->bval, k->bmask, want, bytes))
        *src = GPU_PREVIOUS_BUFFER;
    /* else: both colors taken, keep the stage constant (approximation) */
    *op = base_op + (inv ? 1 : 0);
}

static void emit_op(C3D_TexEnv *env, const Op *op, int is_alpha, Consts *k)
{
    GPU_TEVSRC src[3] = {GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS};
    int ops[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (op->prev_op[i]) {
            src[i] = GPU_PREVIOUS;
            ops[i] = 0;
            continue;
        }
        if (op->s[i].kind == IN_ZERO && i > 0 && op->func == GPU_REPLACE)
            continue;
        resolve(&op->s[i], op->inv[i], is_alpha, k, &src[i], &ops[i]);
    }
    C3D_TexEnvSrc(env, is_alpha ? C3D_Alpha : C3D_RGB, src[0], src[1], src[2]);
    if (is_alpha)
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)ops[0], (GPU_TEVOP_A)ops[1], (GPU_TEVOP_A)ops[2]);
    else
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)ops[0], (GPU_TEVOP_RGB)ops[1], (GPU_TEVOP_RGB)ops[2]);
    C3D_TexEnvFunc(env, is_alpha ? C3D_Alpha : C3D_RGB, op->func);
}

static void build_program(const DrawSetup *ds, Program *p)
{
    uint32_t gen = g_gx.bp[BP_GENMODE];
    int nstages = (int)BITS(gen, 10, 4) + 1;
    RegState rs_rgb[4], rs_a[4];
    for (int i = 0; i < 4; i++)
        rs_rgb[i].pica_stage = rs_a[i].pica_stage = -1;
    memset(p, 0, sizeof *p);
    int buffered = -1;          /* PICA stage whose output the buffer holds */
    uint32_t buf_bytes = 0;     /* bytes of the buffer color in use */

    for (int s = 0; s < nstages && p->n < 6; s++) {
        uint32_t tref = g_gx.bp[BP_TREF(s)] >> ((s & 1) * 12);
        uint32_t map = BITS(tref, 0, 3), en = BITS(tref, 6, 1);
        uint32_t cenv = g_gx.bp[BP_TEV_COLOR(s)], aenv = g_gx.bp[BP_TEV_ALPHA(s)];
        uint32_t ksel = g_gx.bp[BP_KSEL(s / 2)] >> ((s & 1) * 10);
        uint32_t kc = BITS(ksel, 4, 5), ka = BITS(ksel, 9, 5);
        int unit = en ? ds->unit[map] : -1;
        int tswap = (int)BITS(aenv, 2, 2), rswap = (int)BITS(aenv, 0, 2);

        /* keep the previous stage's result in the buffer when it is not PREV */
        int cur = p->n;
        if (cur >= 1 && cur <= 4) {
            for (int r = 1; r < 4; r++)
                if (rs_rgb[r].pica_stage == cur - 1 || rs_a[r].pica_stage == cur - 1) {
                    p->buf_mask_rgb |= 1 << (cur - 1);
                    p->buf_mask_a |= 1 << (cur - 1);
                    buffered = cur - 1;
                }
        }

        In ci[4], ai[4];
        uint32_t csel[4] = {BITS(cenv, 12, 4), BITS(cenv, 8, 4), BITS(cenv, 4, 4), BITS(cenv, 0, 4)};
        uint32_t asel[4] = {BITS(aenv, 13, 3), BITS(aenv, 10, 3), BITS(aenv, 7, 3), BITS(aenv, 4, 3)};
        for (int k = 0; k < 4; k++) {
            build_input(0, csel[k], unit, tswap, rswap, kc, ka, rs_rgb, rs_a, cur, buffered, &ci[k]);
            build_input(1, asel[k], unit, tswap, rswap, kc, ka, rs_rgb, rs_a, cur, buffered, &ai[k]);
        }
        Op cop[2], aop[2];
        int nc = plan_channel(ci[0], ci[1], ci[2], ci[3], (int)BITS(cenv, 18, 1), BITS(cenv, 16, 2), cop);
        int na = plan_channel(ai[0], ai[1], ai[2], ai[3], (int)BITS(aenv, 18, 1), BITS(aenv, 16, 2), aop);
        int steps = nc > na ? nc : na;
        if (p->n + steps > 6)
            steps = 6 - p->n;
        for (int k = 0; k < steps; k++) {
            C3D_TexEnv *env = &p->env[p->n];
            C3D_TexEnvInit(env);
            Consts consts = {0, 0, &p->buf_color, &buf_bytes, !p->buf_mask_rgb && !p->buf_mask_a};
            if (k < nc) emit_op(env, &cop[k], 0, &consts);
            else { C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, 0, 0); C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE); }
            if (k < na) emit_op(env, &aop[k], 1, &consts);
            else { C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0); C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE); }
            C3D_TexEnvColor(env, consts.val);
            if (k == steps - 1) {
                static const GPU_TEVSCALE sc_map[4] = {GPU_TEVSCALE_1, GPU_TEVSCALE_2, GPU_TEVSCALE_4, GPU_TEVSCALE_1};
                C3D_TexEnvScale(env, C3D_RGB, sc_map[BITS(cenv, 20, 2)]);
                C3D_TexEnvScale(env, C3D_Alpha, sc_map[BITS(aenv, 20, 2)]);
            }
            p->n++;
        }
        rs_rgb[BITS(cenv, 22, 2)].pica_stage = p->n - 1;
        rs_a[BITS(aenv, 22, 2)].pica_stage = p->n - 1;
    }
    if (p->n == 0) {
        C3D_TexEnvInit(&p->env[0]);
        C3D_TexEnvSrc(&p->env[0], C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        p->n = 1;
    }
}

/* the TEV -> combiner translation only depends on registers that change
 * rarely between draws: cache it */
#define PROG_CACHE 256
typedef struct {
    uint32_t key[96];
    int n;
    Program prog;
} CachedProg;
static CachedProg s_pcache[PROG_CACHE];

static int prog_key(const DrawSetup *ds, uint32_t *k)
{
    uint32_t gen = g_gx.bp[BP_GENMODE];
    int n = 0, nstages = (int)BITS(gen, 10, 4) + 1;
    k[n++] = (uint32_t)nstages;
    for (int i = 0; i < (nstages + 1) / 2; i++)
        k[n++] = g_gx.bp[BP_TREF(i * 2)];
    for (int s = 0; s < nstages; s++) {
        k[n++] = g_gx.bp[BP_TEV_COLOR(s)];
        k[n++] = g_gx.bp[BP_TEV_ALPHA(s)];
    }
    for (int i = 0; i < 8; i++)
        k[n++] = g_gx.bp[BP_KSEL(i)];
    for (int i = 0; i < 8; i++)
        k[n++] = (uint32_t)(ds->unit[i] + 1);
    memcpy(&k[n], g_gx.tev_reg, sizeof g_gx.tev_reg);
    n += 16;
    memcpy(&k[n], g_gx.tev_konst, sizeof g_gx.tev_konst);
    n += 16;
    return n;
}

static const Program *cached_program(const DrawSetup *ds)
{
    uint32_t key[96];
    int n = prog_key(ds, key);
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ key[i]) * 16777619u;
    CachedProg *c = &s_pcache[(h ^ (h >> 16)) % PROG_CACHE];
    if (c->n != n || memcmp(c->key, key, n * sizeof key[0])) {
        memcpy(c->key, key, n * sizeof key[0]);
        c->n = n;
        build_program(ds, &c->prog);
    }
    return &c->prog;
}

static void apply_program(const Program *p)
{
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        if (i < p->n) {
            *env = p->env[i];
            C3D_DirtyTexEnv(env);
        } else {
            C3D_TexEnvInit(env);   /* passthrough of previous */
        }
    }
    C3D_TexEnvBufUpdate(C3D_RGB, p->buf_mask_rgb);
    C3D_TexEnvBufUpdate(C3D_Alpha, p->buf_mask_a);
    C3D_TexEnvBufColor(p->buf_color);
}

/* ---------------------------------------------------------------- draws */

static void setup_textures(DrawSetup *ds, float scale[3][2])
{
    uint32_t gen = g_gx.bp[BP_GENMODE];
    int nstages = (int)BITS(gen, 10, 4) + 1;
    int use_chan[2] = {0, 0};
    for (int i = 0; i < 8; i++)
        ds->unit[i] = -1;
    ds->nunits = 0;
    for (int s = 0; s < nstages; s++) {
        uint32_t tref = g_gx.bp[BP_TREF(s)] >> ((s & 1) * 12);
        uint32_t map = BITS(tref, 0, 3), coord = BITS(tref, 3, 3), en = BITS(tref, 6, 1), chan = BITS(tref, 7, 3);
        if (chan < 2)
            use_chan[chan] = 1;
        if (!en || ds->unit[map] >= 0 || ds->nunits >= 3)
            continue;
        ds->unit[map] = ds->nunits;
        ds->unit_map[ds->nunits] = (int)map;
        ds->unit_coord[ds->nunits] = (int)coord;
        ds->nunits++;
    }
    ds->chan = use_chan[1] && !use_chan[0] ? 1 : 0;
    for (int u = 0; u < 3; u++) {
        scale[u][0] = scale[u][1] = 1.0f;
        ds->unit_flip[u] = 0;
        if (u >= ds->nunits)
            continue;
        int map = ds->unit_map[u];
        GpuTex *g = gpu_texture(map);
        if (!g) {
            ds->unit[map] = -1;
            continue;
        }
        uint32_t mode = g_gx.bp[BP_TX_MODE0(map)];
        static const GPU_TEXTURE_WRAP_PARAM wrap[4] = {GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT};
        C3D_TexSetWrap(&g->tex, wrap[BITS(mode, 0, 2)], wrap[BITS(mode, 2, 2)]);
        uint32_t minf = BITS(mode, 5, 3);
        C3D_TexSetFilter(&g->tex, BITS(mode, 4, 1) ? GPU_LINEAR : GPU_NEAREST,
                         (minf == 1 || minf == 3 || minf == 5) ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexBind(u, &g->tex);
        ds->unit_flip[u] = !g->efb_copy;
        scale[u][0] = (float)g->w / g->tex.width;
        scale[u][1] = (float)g->h / g->tex.height;
    }
}

/* viewport mapping, folded per draw: rt = clip * a + w * b */
typedef struct { float ax, bx, ay, by, az, bz; } RtMap;

static void rt_map(RtMap *m)
{
    float wd = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT]), ht = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 1]);
    float zr = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 2]), xo = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 3]);
    float yo = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 4]), fz = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + 5]);
    uint32_t off = g_gx.bp[BP_SCISSOR_OFFSET];
    float ox = (float)(BITS(off, 0, 10) * 2), oy = (float)(BITS(off, 10, 10) * 2);
    const float k = RT_SCALE / (RT_SIZE / 2);
    /* EFB pixel = clip/w * scale + origin; done in homogeneous form */
    m->ax = wd * k;
    m->bx = (xo - ox) * k - 1.0f;
    m->ay = -ht * k;
    m->by = ((float)EFB_H - (yo - oy)) * k - 1.0f;
    m->az = zr / 16777216.0f;
    m->bz = fz / 16777216.0f - 1.0f;
}

static void to_rt(const GXVertex *v, const DrawSetup *ds, const float scale[3][2], const RtMap *m, CVert *o)
{
    float w = v->pos[3];
    o->pos[0] = v->pos[0] * m->ax + w * m->bx;
    o->pos[1] = v->pos[1] * m->ay + w * m->by;
    o->pos[2] = v->pos[2] * m->az + w * m->bz;
    o->pos[3] = w;
    memcpy(o->col, v->color[ds->chan], sizeof o->col);
    for (int u = 0; u < 3; u++) {
        if (u < ds->nunits) {
            const float *t = v->tex[ds->unit_coord[u]];
            float q = t[2] != 0 ? t[2] : 1.0f;
            o->tc[u][0] = t[0] / q * scale[u][0];
            o->tc[u][1] = t[1] / q * scale[u][1];
            if (ds->unit_flip[u])
                o->tc[u][1] = 1.0f - o->tc[u][1];
        } else {
            o->tc[u][0] = o->tc[u][1] = 0;
        }
    }
}

static void set_scissor(void)
{
    uint32_t tl = g_gx.bp[BP_SCISSOR_TL], br = g_gx.bp[BP_SCISSOR_BR], off = g_gx.bp[BP_SCISSOR_OFFSET];
    int ox = (int)BITS(off, 0, 10) * 2, oy = (int)BITS(off, 10, 10) * 2;
    int x0 = (int)BITS(tl, 12, 11) - ox, y0 = (int)BITS(tl, 0, 11) - oy;
    int x1 = (int)BITS(br, 12, 11) - ox + 1, y1 = (int)BITS(br, 0, 11) - oy + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W) x1 = EFB_W;
    if (y1 > EFB_H) y1 = EFB_H;
    u32 left = (u32)(x0 * RT_SCALE), right = (u32)(x1 * RT_SCALE + 0.5f);
    u32 bottom = (u32)((EFB_H - y1) * RT_SCALE), top = (u32)((EFB_H - y0) * RT_SCALE + 0.5f);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left, bottom, right, top);
}

/* unique vertices are converted once and drawn with an index list (strips
 * and quads otherwise duplicate most vertices) */
void gfx_draw_indexed(const GXVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{
    if (!nidx || nverts > 0xFFFF)
        return;
    begin_frame();
    ensure_space(nverts, nidx);
    if (s_vpos + nverts > VBUF_VERTS || s_ipos + nidx > IBUF_INDICES)
        return;
    CVert *out = &s_vbuf[s_vpos];
    u16 *ind = &s_ibuf[s_ipos];
    s_vpos += nverts;
    s_ipos += nidx;
    DrawSetup ds;
    float scale[3][2];
    RtMap m;
    setup_textures(&ds, scale);
    rt_map(&m);
    for (uint32_t i = 0; i < nverts; i++)
        to_rt(&v[i], &ds, scale, &m, &out[i]);
    for (uint32_t i = 0; i < nidx; i++)
        ind[i] = (u16)idx[i];
    apply_program(cached_program(&ds));
    apply_fragment_ops();
    set_scissor();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, out, sizeof(CVert), 5, 0x43210);
    C3D_DrawElements(GPU_TRIANGLES, (int)nidx, C3D_UNSIGNED_SHORT, ind);
}

void gfx_draw_triangles(const GXVertex *v, uint32_t count)
{
    static uint32_t *seq;
    static uint32_t cap;
    if (count > cap) {
        cap = count + 256;
        seq = realloc(seq, cap * sizeof *seq);
    }
    for (uint32_t i = 0; i < count; i++)
        seq[i] = i;
    gfx_draw_indexed(v, count, seq, count);
}

/* ------------------------------------------------------------- copies */

static void quad(CVert *q, float x0, float y0, float x1, float y1, float z,
                 float s0, float t0, float s1, float t1, const float col[4])
{
    float xs[6] = {x0, x1, x1, x0, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    float ss[6] = {s0, s1, s1, s0, s1, s0}, ts[6] = {t0, t0, t1, t0, t1, t1};
    for (int i = 0; i < 6; i++) {
        q[i].pos[0] = xs[i]; q[i].pos[1] = ys[i]; q[i].pos[2] = z; q[i].pos[3] = 1.0f;
        memcpy(q[i].col, col, sizeof q[i].col);
        q[i].tc[0][0] = ss[i]; q[i].tc[0][1] = ts[i];
        q[i].tc[1][0] = q[i].tc[1][1] = q[i].tc[2][0] = q[i].tc[2][1] = 0;
    }
}

static void simple_env(int textured)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/* EFB rect -> RT texture coordinates */
static void efb_tc(int ex, int ey, float *s, float *t)
{
    *s = ex * RT_SCALE / RT_SIZE;
    *t = (EFB_H - ey) * RT_SCALE / RT_SIZE;
}

static void patch_textures(GpuTex *src, uint32_t dest, uint32_t copy_fmt);

static void copy_to_texture(int x0, int y0, int w, int h, int half, uint32_t copy_fmt)
{
    uint32_t dest = (g_gx.bp[BP_COPY_DEST] & 0xFFFFFF) << 5;
    int tw = half ? w / 2 : w, th = half ? h / 2 : h;
    if (tw < 1 || th < 1)
        return;
    GpuTex *g = find_efb_copy(dest, tw, th);
    if (!g) {
        g = gtex_alloc();
        if (!C3D_TexInitVRAM(&g->tex, (u16)pow2(tw), (u16)pow2(th), GPU_RGBA8) &&
            !C3D_TexInit(&g->tex, (u16)pow2(tw), (u16)pow2(th), GPU_RGBA8))
            return;
        g->rt = C3D_RenderTargetCreateFromTex(&g->tex, GPU_TEXFACE_2D, 0, -1);
        g->used = 1;
        g->efb_copy = 1;
        g->key = dest;
        g->w = (uint16_t)tw;
        g->h = (uint16_t)th;
        ghash_insert(g);
        s_gtex_bytes += g->tex.width * g->tex.height * 4;
    }
    g->last_use = s_frame;
    CVert *q = alloc_verts(6);
    if (!q || !g->rt)
        return;
    /* destination: texel row 0 (t = 0) receives the top EFB row */
    float sx0, st0, sx1, st1;
    efb_tc(x0, y0, &sx0, &st0);
    efb_tc(x0 + w, y0 + h, &sx1, &st1);
    float x1n = 2.0f * tw / g->tex.width - 1.0f, y1n = 2.0f * th / g->tex.height - 1.0f;
    static const float white[4] = {1, 1, 1, 1};
    quad(q, -1, -1, x1n, y1n, -0.5f, sx0, st0, sx1, st1, white);
    C3D_FrameDrawOn(g->rt);
    simple_env(1);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_TexBind(0, &s_efb_tex);
    draw_verts(q, 6);
    C3D_FrameDrawOn(s_efb_rt);
    patch_textures(g, dest, copy_fmt);
}

/* block size of a GX texture format: width, height, bytes */
static void tex_block(uint32_t fmt, int *bw, int *bh, int *bytes)
{
    *bw = 4, *bh = 4, *bytes = 32;
    if (fmt == 6)
        *bytes = 64;
    else if (fmt == 1 || fmt == 2 || fmt == 9)
        *bw = 8;
    else if (fmt == 0 || fmt == 8 || fmt == 14)
        *bw = 8, *bh = 8;
}

/* An EFB copy can land inside a bigger texture (tile animations are copied
 * into the tileset with the tileset's stride). Copies are never written back
 * to RAM here, so draw the copy into every decoded texture that holds the
 * destination, at the texel position the address and stride give. Decoded
 * textures can be half size and are stored bottom-up (see unit_flip). */
static void patch_textures(GpuTex *src, uint32_t dest, uint32_t copy_fmt)
{
    int cbw, cbh, cbytes;
    tex_block(copy_fmt == 7 || copy_fmt == 8 || copy_fmt == 9 || copy_fmt == 10 ? 1 :
              copy_fmt == 11 || copy_fmt == 12 ? 3 : copy_fmt, &cbw, &cbh, &cbytes);
    uint32_t row = (g_gx.bp[BP_COPY_STRIDE] & 0x3FF) * 32;
    if (!row)
        return;
    for (int i = 0; i < GPU_TEX_SLOTS; i++) {
        GpuTex *g = &s_gtex[i];
        if (!g->used || g->efb_copy || !g->gw || g->addr > dest)
            continue;
        int bw, bh, bytes;
        tex_block(g->fmt, &bw, &bh, &bytes);
        uint32_t trow = (uint32_t)((g->gw + bw - 1) / bw) * bytes;
        uint32_t off = dest - g->addr;
        if (off >= trow * ((g->gh + bh - 1) / bh) || trow != row || bw != cbw || bh != cbh || bytes != cbytes)
            continue;
        int x = (int)(off % row / bytes) * bw, y = (int)(off / row) * bh;
        if (x >= g->gw || y >= g->gh)
            continue;
        if (!g->rt && !(g->rt = C3D_RenderTargetCreateFromTex(&g->tex, GPU_TEXFACE_2D, 0, -1)))
            continue;
        CVert *q = alloc_verts(6);
        if (!q)
            return;
        float sx = (float)g->w / g->gw / g->tex.width, sy = (float)g->h / g->gh / g->tex.height;
        float x0 = 2.0f * x * sx - 1.0f, x1 = 2.0f * (x + src->w) * sx - 1.0f;
        float ytop = 1.0f - 2.0f * y * sy, ybot = 1.0f - 2.0f * (y + src->h) * sy;
        static const float white[4] = {1, 1, 1, 1};
        quad(q, x0, ybot, x1, ytop, -0.5f, 0, (float)src->h / src->tex.height, (float)src->w / src->tex.width, 0, white);
        C3D_FrameDrawOn(g->rt);
        simple_env(1);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_TexSetFilter(&src->tex, g->w < g->gw ? GPU_LINEAR : GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, &src->tex);
        draw_verts(q, 6);
    }
    C3D_FrameDrawOn(s_efb_rt);
}

static void clear_rect(int x0, int y0, int w, int h)
{
    uint32_t ar = g_gx.bp[BP_CLEAR_AR], gb = g_gx.bp[BP_CLEAR_GB];
    float d = (g_gx.bp[BP_CLEAR_Z] & 0xFFFFFF) / 16777216.0f;
    float col[4] = {(ar & 255) / 255.0f, (gb >> 8) / 255.0f, (gb & 255) / 255.0f, ((ar >> 8) & 255) / 255.0f};
    CVert *q = alloc_verts(6);
    if (!q)
        return;
    float nx0 = x0 * RT_SCALE / (RT_SIZE / 2) - 1, nx1 = (x0 + w) * RT_SCALE / (RT_SIZE / 2) - 1;
    float ny1 = (EFB_H - y0) * RT_SCALE / (RT_SIZE / 2) - 1, ny0 = (EFB_H - y0 - h) * RT_SCALE / (RT_SIZE / 2) - 1;
    quad(q, nx0, ny0, nx1, ny1, d - 1.0f, 0, 0, 0, 0, col);
    simple_env(0);
    uint32_t cm = g_gx.bp[BP_CMODE0];
    int mask = 0;
    if (BITS(cm, 3, 1)) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if (BITS(cm, 4, 1)) mask |= GPU_WRITE_ALPHA;
    if (BITS(g_gx.bp[BP_ZMODE], 4, 1)) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)mask);
    draw_verts(q, 6);
}

void gfx_efb_copy(uint32_t trigger)
{
    uint32_t tl = g_gx.bp[BP_EFB_TL], sz = g_gx.bp[BP_EFB_SIZE];
    int x0 = (int)BITS(tl, 0, 10), y0 = (int)BITS(tl, 10, 10);
    int w = (int)BITS(sz, 0, 10) + 1, h = (int)BITS(sz, 10, 10) + 1;
    begin_frame();
    if (BITS(trigger, 14, 1))
        gfx_present();
    else
        copy_to_texture(x0, y0, w, h, (int)BITS(trigger, 9, 1), BITS(trigger, 3, 4) / 2 + (BITS(trigger, 3, 4) & 1) * 8);
    if (BITS(trigger, 11, 1)) {
        begin_frame();
        clear_rect(x0, y0, w, h);
    }
}

void gfx_texture_will_change(void) {}

void gfx_present(void)
{
    begin_frame();
    uint32_t sz = g_gx.bp[BP_EFB_SIZE];
    int w = (int)BITS(sz, 0, 10) + 1, h = (int)BITS(sz, 10, 10) + 1;
    CVert *q = alloc_verts(6);
    if (q) {
        float s0, t0, s1, t1;
        efb_tc(0, 0, &s0, &t0);
        efb_tc(w, h, &s1, &t1);
        static const float white[4] = {1, 1, 1, 1};
        /* screen NDC (x right, y up), then the citro3d screen tilt: (x, y) -> (y, -x) */
        float xs[6] = {-1, 1, 1, -1, 1, -1}, ys[6] = {1, 1, -1, 1, -1, -1};
        float ss[6] = {s0, s1, s1, s0, s1, s0}, ts[6] = {t0, t0, t1, t0, t1, t1};
        for (int i = 0; i < 6; i++) {
            q[i].pos[0] = ys[i];
            q[i].pos[1] = -xs[i];
            q[i].pos[2] = -0.5f;
            q[i].pos[3] = 1.0f;
            memcpy(q[i].col, white, sizeof white);
            q[i].tc[0][0] = ss[i];
            q[i].tc[0][1] = ts[i];
            q[i].tc[1][0] = q[i].tc[1][1] = q[i].tc[2][0] = q[i].tc[2][1] = 0;
        }
        C3D_FrameDrawOn(s_screen);
        simple_env(1);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_TexBind(0, &s_efb_tex);
        draw_verts(q, 6);
    }
    C3D_FrameEnd(0);
    s_in_frame = 0;
    s_frame++;
    rt_prof_frame();
    free_retired(0);
    gx_texture_frame();
}
#endif
