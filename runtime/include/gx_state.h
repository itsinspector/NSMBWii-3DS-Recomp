/* GX (Flipper/Hollywood GPU) state as written through the command stream,
 * shared by the command processor and the render backends. Register layouts
 * follow the hardware (BP = raster/pixel, CP = vertex loader, XF = transform). */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t bp[256];
    uint32_t cp_matidx_a, cp_matidx_b;
    uint32_t cp_vcd_lo, cp_vcd_hi;
    uint32_t cp_vat[3][8];
    uint32_t cp_array_base[16], cp_array_stride[16];
    uint32_t xf_mem[0x1000];     /* matrices (0x000), normal (0x400), post (0x500), lights (0x600) */
    float xf_memf[0x1000];       /* the same words as floats (kept in sync by xf_write) */
    uint32_t xf_regs[0x58];      /* 0x1000..0x1057 */
    uint8_t tmem_tlut[0x80000];  /* palettes loaded with BP 0x64/0x65 */
    /* TEV registers PREV, C0-C2 (signed, /255) and konst K0-K3 (0..1), as
     * rgba. BP 0xE0-0xE7 hold either, selected by bit 23 of each write. */
    float tev_reg[4][4];
    float tev_konst[4][4];
} GXState;

extern GXState g_gx;
/* bumped by writes that change draw state (BP, XF registers, CP matrix
 * indices) and by XF memory writes (matrices): equal counters, same state */
extern uint32_t g_gx_state_gen, g_gx_xfmem_gen;

/* --- XF registers (index from 0x1000) */
#define XF_NUM_COLORS    0x09
#define XF_AMBIENT0      0x0A
#define XF_MATERIAL0     0x0C
#define XF_COLOR0_CNTRL  0x0E
#define XF_ALPHA0_CNTRL  0x10
#define XF_DUALTEX       0x12
#define XF_VIEWPORT      0x1A    /* wd, ht, zrange, xorig, yorig, farz (floats) */
#define XF_PROJECTION    0x20    /* 6 params + type */
#define XF_NUM_TEXGENS   0x3F
#define XF_TEXGEN0       0x40
#define XF_POSTMTX0      0x50

static inline float gx_xf_float(uint32_t raw) { union { uint32_t u; float f; } c = {raw}; return c.f; }

/* --- BP fields */
#define BP_GENMODE        0x00
#define BP_SCISSOR_TL     0x20
#define BP_SCISSOR_BR     0x21
#define BP_TREF(i)        (0x28 + (i) / 2)
#define BP_SSIZE(i)       (0x30 + (i) * 2)
#define BP_ZMODE          0x40
#define BP_CMODE0         0x41
#define BP_CMODE1         0x42
#define BP_PE_CONTROL     0x43
#define BP_EFB_TL         0x49
#define BP_EFB_SIZE       0x4A
#define BP_COPY_DEST      0x4B
#define BP_COPY_STRIDE    0x4D
#define BP_COPY_YSCALE    0x4E
#define BP_CLEAR_AR       0x4F
#define BP_CLEAR_GB       0x50
#define BP_CLEAR_Z        0x51
#define BP_COPY_TRIGGER   0x52
#define BP_SCISSOR_OFFSET 0x59
#define BP_TEV_COLOR(i)   (0xC0 + (i) * 2)
#define BP_TEV_ALPHA(i)   (0xC1 + (i) * 2)
#define BP_ALPHACOMPARE   0xF3
#define BP_KSEL(i)        (0xF6 + (i))

/* texture unit n (0-7): mode0, mode1, image0, image3 (address), tlut */
#define BP_TX_MODE0(n)  ((n) < 4 ? 0x80 + (n) : 0xA0 + (n) - 4)
#define BP_TX_MODE1(n)  ((n) < 4 ? 0x84 + (n) : 0xA4 + (n) - 4)
#define BP_TX_IMAGE0(n) ((n) < 4 ? 0x88 + (n) : 0xA8 + (n) - 4)
#define BP_TX_IMAGE3(n) ((n) < 4 ? 0x94 + (n) : 0xB4 + (n) - 4)
#define BP_TX_TLUT(n)   ((n) < 4 ? 0x98 + (n) : 0xB8 + (n) - 4)

#define BITS(v, lo, n) (((v) >> (lo)) & ((1u << (n)) - 1))

/* --- Transformed vertex handed to the render backends */
typedef struct {
    float pos[4];        /* clip space */
    float color[2][4];   /* lit channel colors, 0..1 */
    float tex[8][3];     /* s, t, q (normalized texture space) */
} GXVertex;

/* Decoded texture ready for sampling / upload */
typedef struct {
    uint32_t key;        /* cache identity */
    uint16_t width, height;
    uint32_t *rgba;      /* width*height, 0xAABBGGRR in memory (R first) */
    uint32_t last_use;
} GXTexture;

/* --- gx_vertex.c */
void gx_draw_primitive(uint8_t op, const uint8_t *data, uint32_t count);
uint32_t gx_vertex_size(int vat);

/* model space vertex as read by the vertex loader */
typedef struct {
    uint32_t pnmtx;
    uint32_t texmtx[8];
    float pos[3];
    float nrm[3][3];       /* normal, binormal, tangent */
    float col[2][4];
    uint8_t col8[2][4];    /* the same colors as bytes */
    float tex[8][2];
} GXRawVertex;
/* texture coordinate generation i on the CPU (regular texgens: matrix of a
 * position, normal or texture coordinate source) */
void gx_texgen(const GXRawVertex *in, int i, float out[3]);
/* lit vertex color of channel ch; gx_prep_lights() first, once per draw */
void gx_prep_lights(void);
void gx_light_color(const GXRawVertex *in, int ch, float out[4]);

/* --- gx_texture.c */
const GXTexture *gx_get_texture(int unit);
void gx_texture_invalidate(void);
void gx_texture_frame(void);

/* --- render backend (gfx_soft.c / gfx_c3d.c) */
void gfx_init(void);
/* unique transformed vertices plus a triangle list of indices into them */
void gfx_draw_indexed(const GXVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t nidx);
/* for backends that only take triangle lists */
void gx_expand_indexed(const GXVertex *v, const uint32_t *idx, uint32_t nidx,
                       void (*draw)(const GXVertex *, uint32_t));
void gfx_draw_triangles(const GXVertex *v, uint32_t count);   /* count vertices, 3 per triangle */
#ifdef __3DS__
/* model space vertices, transformed by the GPU; 0: the draw needs the CPU
 * transform (color and emboss texgens) */
int gfx_draw_raw(const GXRawVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t nidx, int texmtx_direct);
#endif
void gfx_efb_copy(uint32_t trigger);                         /* BP 0x52 */
void gfx_present(void);
void gfx_texture_will_change(void);  /* a decoded texture is about to be replaced */
