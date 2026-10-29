/* GX command processor: parses the command stream the game writes through the
 * write-gather pipe (and the display lists it calls), keeps the CP/XF/BP
 * register state and hands draws to the vertex pipeline (gx_vertex.c). */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "gx_state.h"

void hw_bp_event(uint32_t reg, uint32_t val);     /* hw.c: PE done/token */

GXState g_gx;

typedef struct {
    uint8_t cmd[16];
    uint32_t len;
    uint8_t *payload;        /* collected payload of XF loads and draws */
    uint32_t payload_len, payload_need, payload_cap;
    uint32_t skip;
} Parser;

static Parser s_main;
static uint32_t s_bp_mask = 0xFFFFFF;
static int s_debug = -1;
static int s_depth;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ------------------------------------------------------------- registers */

extern int g_gx_vtx_dirty;   /* gx_vertex.c: per-draw setup must be recomputed */

static void cp_write(uint8_t reg, uint32_t v)
{
    g_gx_vtx_dirty = 1;
    if (reg == 0x30) g_gx.cp_matidx_a = v;
    else if (reg == 0x40) g_gx.cp_matidx_b = v;
    else if (reg == 0x50) g_gx.cp_vcd_lo = v;
    else if (reg == 0x60) g_gx.cp_vcd_hi = v;
    else if (reg >= 0x70 && reg < 0xA0) g_gx.cp_vat[(reg - 0x70) >> 4][reg & 7] = v;
    else if (reg >= 0xA0 && reg < 0xB0) g_gx.cp_array_base[reg & 15] = v & 0x3FFFFFFF;
    else if (reg >= 0xB0 && reg < 0xC0) g_gx.cp_array_stride[reg & 15] = v & 0xFF;
}

static void xf_write(uint32_t addr, uint32_t v)
{
    if (addr < 0x1000) {
        g_gx.xf_mem[addr] = v;
        g_gx.xf_memf[addr] = gx_xf_float(v);
    }
    else if (addr < 0x1058) {
        g_gx.xf_regs[addr - 0x1000] = v;
        g_gx_vtx_dirty = 1;
    }
}

/* indexed XF loads: matrices fetched from the arrays at CP 0xAC-0xAF */
uint32_t g_xf_stats[4];   /* direct loads, indexed loads, indexed into mtx 0, last src */

static void xf_indexed_load(uint8_t op, uint32_t v)
{
    int arr = 12 + ((op - 0x20) >> 3);
    uint32_t index = v >> 16, size = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
    uint32_t src = g_gx.cp_array_base[arr] + index * g_gx.cp_array_stride[arr];
    g_xf_stats[1]++;
    if (addr == 0) {
        g_xf_stats[2]++; g_xf_stats[3] = src;
        extern int g_gx_debug_vertex;
        static int n;
        if (g_gx_debug_vertex && n++ < 6)
            rt_log("  idx load -> 0: index %lu size %lu src %08lX first %08lX %08lX %08lX %08lX",
                   (unsigned long)index, (unsigned long)size, (unsigned long)src,
                   (unsigned long)ld32(src | 0x80000000u), (unsigned long)ld32((src + 4) | 0x80000000u),
                   (unsigned long)ld32((src + 8) | 0x80000000u), (unsigned long)ld32((src + 12) | 0x80000000u));
    }
    for (uint32_t i = 0; i < size; i++)
        xf_write(addr + i, ld32((src + i * 4) | 0x80000000u));
}

static void tlut_load(uint32_t v)
{
    uint32_t src = (g_gx.bp[0x64] & 0xFFFFFF) << 5;
    uint32_t tmem = (v & 0x3FF) << 9;
    uint32_t size = ((v >> 10) & 0x3FF) << 5;
    if (tmem + size > sizeof g_gx.tmem_tlut)
        size = sizeof g_gx.tmem_tlut - tmem;
    guest_read(g_gx.tmem_tlut + tmem, src | 0x80000000u, size);
    gx_texture_invalidate();
}

static void tev_reg_write(uint32_t reg, uint32_t v)
{
    int idx = (reg - 0xE0) / 2, high = (reg - 0xE0) & 1, konst = (v >> 23) & 1;
    float *dst = konst ? g_gx.tev_konst[idx] : g_gx.tev_reg[idx];
    int c0, c1;
    if (konst) {                     /* unsigned 8-bit */
        c0 = v & 255;
        c1 = (v >> 12) & 255;
    } else {                         /* signed 11-bit */
        c0 = (int)(v << 21) >> 21;
        c1 = (int)((v >> 12) << 21) >> 21;
    }
    if (!high) { dst[0] = c0 / 255.0f; dst[3] = c1 / 255.0f; }
    else { dst[2] = c0 / 255.0f; dst[1] = c1 / 255.0f; }
}

volatile uint32_t g_gx_xfb_copies;   /* frames presented, for rt_time_adapt */

static void bp_write(uint32_t reg, uint32_t v)
{
    if (reg == 0xFE) {                       /* mask for the next write */
        s_bp_mask = v & 0xFFFFFF;
        return;
    }
    g_gx.bp[reg] = (g_gx.bp[reg] & ~s_bp_mask) | (v & s_bp_mask);
    s_bp_mask = 0xFFFFFF;
    v = g_gx.bp[reg];
    switch (reg) {
    case 0x45: case 0x47: case 0x48:
        hw_bp_event(reg, v);
        break;
    case BP_COPY_TRIGGER:
        if (v & (1u << 14))
            g_gx_xfb_copies++;
        gfx_efb_copy(v);
        break;
    case 0x65:
        tlut_load(v);
        break;
    default:
        if (reg >= 0xE0 && reg <= 0xE7)
            tev_reg_write(reg, v);
    }
}

/* -------------------------------------------------------------- parsing */

static void parse_bytes(Parser *p, const uint8_t *data, uint32_t n);

static void call_display_list(uint32_t addr, uint32_t size)
{
    if (s_depth > 4 || size > 0x400000) {
        rt_log("GX: display list %08lX size %lX ignored", (unsigned long)addr, (unsigned long)size);
        return;
    }
    uint8_t *buf = malloc(size);
    if (!buf)
        return;
    guest_read(buf, addr | 0x80000000u, size);
    Parser dl = {0};
    s_depth++;
    parse_bytes(&dl, buf, size);
    s_depth--;
    free(dl.payload);
    free(buf);
}

static uint32_t cmd_length(uint8_t op)
{
    if (op >= 0x80) return 3;                   /* draw: op count(2), then vertices */
    switch (op & 0xF8) {
    case 0x08: return 6;                        /* CP: reg(1) val(4) */
    case 0x10: return 5;                        /* XF: count-1(2) addr(2), then data */
    case 0x20: case 0x28: case 0x30: case 0x38: return 5;   /* indexed XF loads */
    case 0x40: return 9;                        /* call DL: addr(4) size(4) */
    case 0x60: return 5;                        /* BP: reg(1) val(3) */
    default: return 1;                          /* NOP, invalidate vertex cache */
    }
}

static void need_payload(Parser *p, uint32_t n)
{
    if (n > p->payload_cap) {
        p->payload_cap = n + 4096;
        p->payload = realloc(p->payload, p->payload_cap);
    }
    p->payload_len = 0;
    p->payload_need = n;
}

/* a command whose payload has been collected */
static void finish_payload(Parser *p)
{
    const uint8_t *c = p->cmd;
    uint8_t op = c[0];
    if (op >= 0x80) {
        gx_draw_primitive(op, p->payload, (uint32_t)c[1] << 8 | c[2]);
    } else {                                    /* XF load */
        uint32_t addr = (uint32_t)c[3] << 8 | c[4];
        if (addr < 0x400) g_xf_stats[0]++;
        for (uint32_t i = 0; i < p->payload_need / 4; i++)
            xf_write(addr + i, be32(p->payload + i * 4));
    }
    p->payload_need = 0;
}

static void execute(Parser *p)
{
    const uint8_t *c = p->cmd;
    uint8_t op = c[0];
    if (s_debug > 0) {
        int valid = op == 0x00 || op == 0x08 || op == 0x10 || op == 0x20 || op == 0x28 || op == 0x30 ||
                    op == 0x38 || op == 0x40 || op == 0x48 || op == 0x61 || (op >= 0x80 && op < 0xC0);
        static int bad;
        if (!valid && bad++ < 3)
            rt_log("GX: unknown opcode %02X (depth %d): stream out of sync", op, s_depth);
    }
    if (op >= 0x80) {
        uint32_t count = (uint32_t)c[1] << 8 | c[2];
        uint32_t n = count * gx_vertex_size(op & 7);
        if (op >= 0xC0 || !n) {
            p->skip = n;
            return;
        }
        need_payload(p, n);
        return;
    }
    switch (op & 0xF8) {
    case 0x08: cp_write(c[1], be32(c + 2)); break;
    case 0x10: need_payload(p, (((uint32_t)c[1] << 8 | c[2]) + 1) * 4); break;
    case 0x20: case 0x28: case 0x30: case 0x38: xf_indexed_load(op & 0xF8, be32(c + 1)); break;
    case 0x40: call_display_list(be32(c + 1), be32(c + 5)); break;
    case 0x60: bp_write(c[1], be32(c + 1) & 0xFFFFFF); break;
    }
}

static void parse_bytes(Parser *p, const uint8_t *data, uint32_t n)
{
    while (n) {
        if (p->skip) {
            uint32_t k = p->skip < n ? p->skip : n;
            p->skip -= k;
            data += k;
            n -= k;
            continue;
        }
        if (p->payload_need) {
            uint32_t k = p->payload_need - p->payload_len;
            if (k > n)
                k = n;
            memcpy(p->payload + p->payload_len, data, k);
            p->payload_len += k;
            data += k;
            n -= k;
            if (p->payload_len == p->payload_need)
                finish_payload(p);
            continue;
        }
        p->cmd[p->len++] = *data++;
        n--;
        if (p->len >= cmd_length(p->cmd[0])) {
            execute(p);
            p->len = 0;
        }
    }
}

void gx_feed(const uint8_t *data, uint32_t n)
{
    if (s_debug < 0) {
        s_debug = getenv("NSMBW_DEBUG_GX") != NULL;
        gfx_init();
    }
    parse_bytes(&s_main, data, n);
}
