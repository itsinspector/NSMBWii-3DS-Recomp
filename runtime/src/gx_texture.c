/* GX texture decoding to linear RGBA8 with a small cache.
 * Formats: I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR (only level 0). */
#include <stdlib.h>
#include "runtime.h"
#include "gx_state.h"

static uint32_t s_frame;

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | g << 8 | b << 16 | a << 24;          /* bytes R,G,B,A in memory (little endian) */
}

static uint32_t from565(uint16_t v)
{
    uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    return rgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
}

static uint32_t from5a3(uint16_t v)
{
    if (v & 0x8000) {
        uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    }
    uint32_t a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return rgba(r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1);
}

static uint32_t from_ia8(uint16_t v)
{
    uint32_t i = v & 255, a = v >> 8;
    return rgba(i, i, i, a);
}

static uint32_t tlut_color(uint32_t tlut_reg, uint32_t index)
{
    uint32_t off = ((tlut_reg & 0x3FF) << 9) + index * 2;
    uint32_t fmt = (tlut_reg >> 10) & 3;
    off &= sizeof g_gx.tmem_tlut - 2;
    uint16_t v = (uint16_t)(g_gx.tmem_tlut[off] << 8 | g_gx.tmem_tlut[off + 1]);
    return fmt == 0 ? from_ia8(v) : fmt == 1 ? from565(v) : from5a3(v);
}

/* block dimensions per format */
static void block_dims(uint32_t fmt, int *bw, int *bh, int *bytes)
{
    switch (fmt) {
    case 0: case 8: case 14: *bw = 8; *bh = 8; *bytes = 32; break;   /* I4, C4, CMPR */
    case 1: case 2: case 9: *bw = 8; *bh = 4; *bytes = 32; break;    /* I8, IA4, C8 */
    case 6: *bw = 4; *bh = 4; *bytes = 64; break;                    /* RGBA8 */
    default: *bw = 4; *bh = 4; *bytes = 32; break;                   /* IA8 565 5A3 C14X2 */
    }
}

/* decoded pixels go through PUT: with a shift > 0 only every 2^shift-th
 * pixel is kept (large textures at reduced size on the 3DS) */
static int s_shift;
#define PUT(dst, w, px, py, c) do {         if (!s_shift) (dst)[(py) * (w) + (px)] = (c);         else if (!(((px) | (py)) & ((1 << s_shift) - 1)))             (dst)[((py) >> s_shift) * (((w) + (1 << s_shift) - 1) >> s_shift) + ((px) >> s_shift)] = (c);     } while (0)

static void decode_cmpr_sub(const uint8_t *s, uint32_t *dst, int stride, int x0, int y0, int w, int h)
{
    uint16_t c0 = (uint16_t)(s[0] << 8 | s[1]), c1 = (uint16_t)(s[2] << 8 | s[3]);
    uint32_t pal[4];
    pal[0] = from565(c0);
    pal[1] = from565(c1);
    uint32_t r0 = pal[0] & 255, g0 = (pal[0] >> 8) & 255, b0 = (pal[0] >> 16) & 255;
    uint32_t r1 = pal[1] & 255, g1 = (pal[1] >> 8) & 255, b1 = (pal[1] >> 16) & 255;
    if (c0 > c1) {
        pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
    for (int y = 0; y < 4; y++) {
        uint8_t bits = s[4 + y];
        for (int x = 0; x < 4; x++) {
            int px = x0 + x, py = y0 + y;
            if (px < w && py < h)
                PUT(dst, stride, px, py, pal[(bits >> (6 - 2 * x)) & 3]);
        }
    }
}

static void decode(const uint8_t *src, uint32_t fmt, int w, int h, uint32_t tlut, uint32_t *dst)
{
    int bw, bh, bytes;
    block_dims(fmt, &bw, &bh, &bytes);
    for (int by = 0; by < h; by += bh) {
        for (int bx = 0; bx < w; bx += bw) {
            const uint8_t *b = src;
            src += bytes;
            if (fmt == 14) {                            /* CMPR: 2x2 DXT1 sub-blocks */
                for (int k = 0; k < 4; k++)
                    decode_cmpr_sub(b + k * 8, dst, w, bx + (k & 1) * 4, by + (k >> 1) * 4, w, h);
                continue;
            }
            for (int y = 0; y < bh; y++) {
                for (int x = 0; x < bw; x++) {
                    int px = bx + x, py = by + y, i = y * bw + x;
                    uint32_t c;
                    switch (fmt) {
                    case 0: { uint32_t v = (b[i / 2] >> ((i & 1) ? 0 : 4)) & 15; v *= 17; c = rgba(v, v, v, v); break; }
                    case 1: c = rgba(b[i], b[i], b[i], b[i]); break;
                    case 2: { uint32_t a = (b[i] >> 4) * 17, l = (b[i] & 15) * 17; c = rgba(l, l, l, a); break; }
                    case 3: c = from_ia8((uint16_t)(b[2 * i] << 8 | b[2 * i + 1])); break;
                    case 4: c = from565((uint16_t)(b[2 * i] << 8 | b[2 * i + 1])); break;
                    case 5: c = from5a3((uint16_t)(b[2 * i] << 8 | b[2 * i + 1])); break;
                    case 6: c = rgba(b[2 * i + 1], b[32 + 2 * i], b[32 + 2 * i + 1], b[2 * i]); break;
                    case 8: c = tlut_color(tlut, (b[i / 2] >> ((i & 1) ? 0 : 4)) & 15); break;
                    case 9: c = tlut_color(tlut, b[i]); break;
                    case 10: c = tlut_color(tlut, ((b[2 * i] << 8 | b[2 * i + 1]) & 0x3FFF)); break;
                    default: c = rgba(255, 0, 255, 255); break;
                    }
                    if (px < w && py < h)
                        PUT(dst, w, px, py, c);
                }
            }
        }
    }
}

static uint32_t data_size(uint32_t fmt, int w, int h)
{
    int bw, bh, bytes;
    block_dims(fmt, &bw, &bh, &bytes);
    return (uint32_t)(((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes);
}

/* Hashes every byte: games animate tiles by rewriting a few blocks of a big
 * tileset texture (? blocks, coins), which a sampled hash misses. Four
 * independent lanes over 32-byte steps keep a full pass cheap. */
static uint32_t hash(const uint8_t *p, uint32_t n)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = n, h1 = k, h2 = ~k, h3 = n * k;
    uint32_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * k; h0 ^= h0 >> 31;
        h1 = (h1 ^ w[1]) * k; h1 ^= h1 >> 31;
        h2 = (h2 ^ w[2]) * k; h2 ^= h2 >> 31;
        h3 = (h3 ^ w[3]) * k; h3 ^= h3 >> 31;
    }
    for (; i < n; i++)
        h0 = (h0 ^ p[i]) * k;
    uint64_t h = h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
    h ^= h >> 33;
    return (uint32_t)(h ^ (h >> 32));
}

/* Set-associative cache (SETS x WAYS) keyed by identity (address, format,
 * size, palette); the content hash is re-checked at most once per generation.
 * The generation advances every frame and whenever an EFB copy or TLUT load
 * may have changed texture memory.
 *
 * Backends may still read a replaced texture's pixels from draws queued in
 * the previous frame, so old buffers are freed two frames later. */
#define SETS 1024
#define WAYS 8
#ifdef __3DS__
#define BUDGET (8u << 20)       /* the heap also holds 88 MiB of guest RAM */
#else
#define BUDGET (512u << 20)
#endif

static uint32_t s_generation = 1;

typedef struct {
    GXTexture tex;
    uint32_t ident, content, verified_gen, bytes;
} Entry;
static Entry s_entries[SETS][WAYS];
static size_t s_bytes;

static struct { void *p; uint32_t frame; } *s_retired;
static uint32_t s_nretired, s_retired_cap;

static void retire(Entry *e)
{
    if (!e->tex.rgba)
        return;
    if (s_nretired == s_retired_cap) {
        s_retired_cap = s_retired_cap ? s_retired_cap * 2 : 256;
        s_retired = realloc(s_retired, s_retired_cap * sizeof *s_retired);
    }
    s_retired[s_nretired].p = e->tex.rgba;
    s_retired[s_nretired].frame = s_frame;
    s_nretired++;
    s_bytes -= e->bytes;
    e->tex.rgba = NULL;
    e->ident = 0;
    e->bytes = 0;
}

static void evict_to_budget(void)
{
    /* oldest first: textures unused for a while, then anything not used this frame */
    static const uint32_t ages[] = {600, 120, 10, 1};
    for (int k = 0; k < 4 && s_bytes > BUDGET; k++)
        for (int s = 0; s < SETS && s_bytes > BUDGET; s++)
            for (int w = 0; w < WAYS; w++) {
                Entry *e = &s_entries[s][w];
                if (e->tex.rgba && e->tex.last_use + ages[k] <= s_frame)
                    retire(e);
            }
}

const GXTexture *gx_get_texture(int unit)
{
    uint32_t img0 = g_gx.bp[BP_TX_IMAGE0(unit)];
    uint32_t addr = (g_gx.bp[BP_TX_IMAGE3(unit)] & 0xFFFFFF) << 5;
    uint32_t tlut = g_gx.bp[BP_TX_TLUT(unit)];
    int w = (int)BITS(img0, 0, 10) + 1, h = (int)BITS(img0, 10, 10) + 1;
    uint32_t fmt = BITS(img0, 20, 4);
    if (!addr)
        return NULL;
    int paletted = fmt >= 8 && fmt <= 10;
    uint32_t ident = addr ^ (fmt << 28) ^ ((uint32_t)w << 16) ^ (uint32_t)h ^ (paletted ? tlut * 2654435761u : 0);
    if (!ident)
        ident = 1;
    Entry *set = s_entries[(ident * 2654435761u) >> 22];   /* 1024 sets */
    Entry *e = NULL;
    for (int i = 0; i < WAYS; i++)
        if (set[i].ident == ident && set[i].tex.rgba) {
            e = &set[i];
            break;
        }
    if (e && e->verified_gen == s_generation) {
        e->tex.last_use = s_frame;
        return &e->tex;
    }
    uint32_t size = data_size(fmt, w, h);
    const uint8_t *src = guest_ptr(addr | 0x80000000u);
    uint32_t content = hash(src, size);
    if (paletted)
        content ^= hash(g_gx.tmem_tlut + ((tlut & 0x3FF) << 9), fmt == 8 ? 32 : 512) * 31;
    if (e && e->content == content) {
        e->verified_gen = s_generation;
        e->tex.last_use = s_frame;
        return &e->tex;
    }
    if (!e) {
        /* empty way, else the least recently used one */
        e = &set[0];
        for (int i = 0; i < WAYS; i++) {
            if (!set[i].tex.rgba) {
                e = &set[i];
                break;
            }
            if (set[i].tex.last_use < e->tex.last_use)
                e = &set[i];
        }
    }
    /* soft renderer: queued draws reference the slot itself */
    if (e->tex.rgba)
        gfx_texture_will_change();
    retire(e);
    /* the 3DS shows 400x240: textures above 512 are kept at half size (memory,
     * decode and upload time); GX coordinates are normalized, so this is free */
    int shift = 0;
#ifdef __3DS__
    while ((w >> shift) > 512 || (h >> shift) > 512)
        shift++;
#endif
    int dw = (w + (1 << shift) - 1) >> shift, dh = (h + (1 << shift) - 1) >> shift;
    e->bytes = (uint32_t)dw * dh * 4;
    e->tex.rgba = malloc(e->bytes);
    if (!e->tex.rgba) {
        /* out of host memory: drop everything not drawn this frame and retry */
        static int warned;
        if (!warned++)
            rt_log("gx: out of memory for a %dx%d texture (cache %lu KiB)", w, h, (unsigned long)(s_bytes >> 10));
        for (int s = 0; s < SETS; s++)
            for (int k = 0; k < WAYS; k++)
                if (s_entries[s][k].tex.rgba && s_entries[s][k].tex.last_use != s_frame)
                    retire(&s_entries[s][k]);
        for (uint32_t i = 0; i < s_nretired; i++)
            free(s_retired[i].p);
        s_nretired = 0;
        e->tex.rgba = malloc(e->bytes);
        if (!e->tex.rgba) {
            e->bytes = 0;
            e->ident = 0;
            return NULL;
        }
    }
    e->tex.key = ident ^ content;
    e->tex.width = (uint16_t)dw;
    e->tex.height = (uint16_t)dh;
    e->tex.last_use = s_frame;
    e->ident = ident;
    e->content = content;
    e->verified_gen = s_generation;
    s_bytes += e->bytes;
    s_shift = shift;
    PROF_BEGIN(dec);
    decode(src, fmt, w, h, tlut, e->tex.rgba);
    PROF_END(dec, PROF_TEX);
    s_shift = 0;
    return &e->tex;
}

void gx_texture_invalidate(void) { s_generation++; }

void gx_texture_frame(void)
{
    s_frame++;
    s_generation++;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < s_nretired; i++) {
        if (s_retired[i].frame + 2 <= s_frame)
            free(s_retired[i].p);
        else
            s_retired[keep++] = s_retired[i];
    }
    s_nretired = keep;
    if (s_bytes > BUDGET)
        evict_to_budget();
}
