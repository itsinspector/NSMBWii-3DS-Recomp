/* PC render backend: OpenGL 3.3 core on a dedicated render thread.
 *
 * The game thread records commands (draws with their GX state snapshot,
 * EFB copies, presents) into a batch; at every present the batch is handed
 * to the render thread, so game logic and rendering overlap by one frame.
 * The EFB is an FBO (optionally at a higher resolution); the TEV of every
 * material becomes a generated GLSL fragment shader (cached by its config);
 * EFB copies to textures stay on the GPU, registered by guest address. */
#ifndef __3DS__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "platform.h"
#include "gx_state.h"

#pragma comment(lib, "opengl32.lib")

#define EFB_W 640
#define EFB_H 528

/* ------------------------------------------------------------ GL loader */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_CONSTANT_ALPHA 0x8003
#define GL_RGBA8 0x8058
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

#define GLFUNCS(X) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))

#define DECL(ret, name, args) typedef ret(APIENTRY *PFN_##name) args; static PFN_##name name;
GLFUNCS(DECL)
#undef DECL

typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);

/* --------------------------------------------------------- command batch */

typedef struct {
    uint32_t key;              /* GXTexture key or 0 */
    uint16_t w, h;
    const uint32_t *rgba;
    uint32_t mode0;
    uint32_t addr;             /* guest address (EFB copy lookup) */
    uint32_t fmt;
} TexRef;

typedef struct {
    uint32_t bp_gen, tref[8], cenv[16], aenv[16], ksel[8], alpha;
    uint32_t zmode, cmode0, cmode1;
    float reg[4][4], konst[4][4];
    TexRef tex[8];
    int sc[4];
    float vp[6], off[2];
    uint32_t first, count;     /* indices in the batch */
} Draw;

enum { CMD_DRAW, CMD_COPY, CMD_PRESENT };

typedef struct {
    int type;
    uint32_t index;            /* draw index */
    uint32_t trigger, dest;    /* copies */
    int rect[4];
    uint32_t clear_ar, clear_gb, clear_z, cmode0, zmode;
} Cmd;

typedef struct {
    Cmd *cmds;
    uint32_t ncmds, cmds_cap;
    Draw *draws;
    uint32_t ndraws, draws_cap;
    GXVertex *verts;
    uint32_t nverts, verts_cap;
    uint32_t *idx;             /* triangle lists into verts */
    uint32_t nidx, idx_cap;
} Batch;

static Batch s_batch[2];
static int s_fill;                 /* batch the game thread fills */
static int s_exec;                 /* batch handed to the render thread */
static plat_sem *s_go, *s_done;
static volatile int s_busy;        /* render thread owns the other batch */
static int s_scale = 2;
static int s_ok;
static int s_gl_debug = -1;     /* NSMBW_GL_DEBUG bits: 1 no depth, 2 no EFB copy textures, 4 no blend, 8 no alpha test */

/* --------------------------------------------------------- recording side */

void gl_present(void);

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    uint32_t c = *cap ? *cap : 1024;
    while (c < need)
        c *= 2;
    *cap = c;
    return realloc(p, (size_t)c * elem);
}

static Cmd *new_cmd(Batch *b)
{
    b->cmds = grow(b->cmds, &b->cmds_cap, b->ncmds + 1, sizeof(Cmd));
    Cmd *c = &b->cmds[b->ncmds++];
    memset(c, 0, sizeof *c);
    return c;
}

/* hand the filled batch to the render thread (waits for the previous one) */
static void submit(int wait_idle)
{
    if (s_busy) {
        plat_sem_wait(s_done);
        s_busy = 0;
    }
    Batch *b = &s_batch[s_fill];
    if (b->ncmds) {
        s_busy = 1;
        s_exec = s_fill;
        plat_sem_post(s_go);
        s_fill ^= 1;
        Batch *n = &s_batch[s_fill];
        n->ncmds = n->ndraws = n->nverts = n->nidx = 0;
    }
    if (wait_idle && s_busy) {
        plat_sem_wait(s_done);
        s_busy = 0;
    }
}

/* draws copy the texture's key and pixel pointer, and gx_texture.c frees
 * replaced pixels only after the batches using them have executed */
void gl_texture_will_change(void) {}

void gl_draw_indexed(const GXVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t count)
{
    if (count < 3)
        return;
    /* built locally: texture lookups may submit (and switch) the batch */
    Draw dl, *d = &dl;
    memset(d, 0, sizeof *d);                   /* padding too: draws are compared with memcmp */
    d->bp_gen = g_gx.bp[BP_GENMODE];
    int nstages = (int)BITS(d->bp_gen, 10, 4) + 1;
    for (int i = 0; i < 8; i++) {
        d->tref[i] = g_gx.bp[0x28 + i];
        d->ksel[i] = g_gx.bp[BP_KSEL(i)];
    }
    for (int s = 0; s < 16; s++) {
        d->cenv[s] = g_gx.bp[BP_TEV_COLOR(s)];
        d->aenv[s] = g_gx.bp[BP_TEV_ALPHA(s)];
    }
    d->alpha = g_gx.bp[BP_ALPHACOMPARE];
    d->zmode = g_gx.bp[BP_ZMODE];
    d->cmode0 = g_gx.bp[BP_CMODE0];
    d->cmode1 = g_gx.bp[BP_CMODE1];
    memcpy(d->reg, g_gx.tev_reg, sizeof d->reg);
    memcpy(d->konst, g_gx.tev_konst, sizeof d->konst);
    memset(d->tex, 0, sizeof d->tex);
    for (int s = 0; s < nstages; s++) {
        uint32_t tref = g_gx.bp[BP_TREF(s)] >> ((s & 1) * 12);
        uint32_t map = BITS(tref, 0, 3);
        if (!BITS(tref, 6, 1) || d->tex[map].mode0 & 0x80000000u)
            continue;
        TexRef *t = &d->tex[map];
        t->mode0 = g_gx.bp[BP_TX_MODE0(map)] | 0x80000000u;   /* bit 31: slot used */
        uint32_t img0 = g_gx.bp[BP_TX_IMAGE0(map)];
        t->addr = (g_gx.bp[BP_TX_IMAGE3(map)] & 0xFFFFFF) << 5;
        t->w = (uint16_t)(BITS(img0, 0, 10) + 1);
        t->h = (uint16_t)(BITS(img0, 10, 10) + 1);
        t->fmt = BITS(img0, 20, 4);
        const GXTexture *gt = gx_get_texture((int)map);
        if (gt) {
            t->key = gt->key;
            t->rgba = gt->rgba;
            t->w = gt->width;
            t->h = gt->height;
        }
    }
    uint32_t tl = g_gx.bp[BP_SCISSOR_TL], br = g_gx.bp[BP_SCISSOR_BR], off = g_gx.bp[BP_SCISSOR_OFFSET];
    int ox = (int)BITS(off, 0, 10) * 2, oy = (int)BITS(off, 10, 10) * 2;
    d->sc[0] = (int)BITS(tl, 12, 11) - ox;
    d->sc[1] = (int)BITS(tl, 0, 11) - oy;
    d->sc[2] = (int)BITS(br, 12, 11) - ox + 1;
    d->sc[3] = (int)BITS(br, 0, 11) - oy + 1;
    for (int i = 0; i < 6; i++)
        d->vp[i] = gx_xf_float(g_gx.xf_regs[XF_VIEWPORT + i]);
    d->off[0] = (float)ox;
    d->off[1] = (float)oy;

    static int logdraws = -1;
    if (logdraws < 0)
        logdraws = getenv("NSMBW_GL_LOGDRAWS") != NULL;
    if (logdraws) {
        static int n;
        extern uint32_t hw_frame_count(void);
        if (hw_frame_count() > 3200 && d->sc[3] > 456 && n++ < 60)
            rt_log("[draw] %u verts vp %.1f %.1f %.1f %.1f %.1f %.1f sc %d %d %d %d off %.0f %.0f v0 %.3f %.3f %.3f %.3f c %.2f %.2f %.2f %.2f",
                   count, d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->vp[4], d->vp[5], d->sc[0], d->sc[1], d->sc[2], d->sc[3],
                   d->off[0], d->off[1], v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].pos[3],
                   v[0].color[0][0], v[0].color[0][1], v[0].color[0][2], v[0].color[0][3]);
    }
    Batch *b = &s_batch[s_fill];
    b->verts = grow(b->verts, &b->verts_cap, b->nverts + nverts, sizeof(GXVertex));
    b->idx = grow(b->idx, &b->idx_cap, b->nidx + count, sizeof(uint32_t));
    memcpy(&b->verts[b->nverts], v, nverts * sizeof(GXVertex));
    for (uint32_t i = 0; i < count; i++)
        b->idx[b->nidx + i] = b->nverts + idx[i];
    b->nverts += nverts;
    /* same state as the previous draw: extend it (one GL draw call instead of many) */
    static int nomerge = -1;
    if (nomerge < 0)
        nomerge = getenv("NSMBW_GL_NOMERGE") != NULL;
    if (!nomerge && b->ncmds && b->cmds[b->ncmds - 1].type == CMD_DRAW) {
        Draw *last = &b->draws[b->cmds[b->ncmds - 1].index];
        if (last->first + last->count == b->nidx && !memcmp(last, d, offsetof(Draw, first))) {
            b->nidx += count;
            last->count += count;
            return;
        }
    }
    b->draws = grow(b->draws, &b->draws_cap, b->ndraws + 1, sizeof(Draw));
    d->first = b->nidx;
    d->count = count;
    b->draws[b->ndraws] = dl;
    b->nidx += count;
    Cmd *c = new_cmd(b);
    c->type = CMD_DRAW;
    c->index = b->ndraws++;
}

void gl_efb_copy(uint32_t trigger)
{
    Batch *b = &s_batch[s_fill];
    uint32_t tl = g_gx.bp[BP_EFB_TL], sz = g_gx.bp[BP_EFB_SIZE];
    Cmd *c = new_cmd(b);
    c->type = BITS(trigger, 14, 1) ? CMD_PRESENT : CMD_COPY;
    c->trigger = trigger;
    c->dest = (g_gx.bp[BP_COPY_DEST] & 0xFFFFFF) << 5;
    c->rect[0] = (int)BITS(tl, 0, 10);
    c->rect[1] = (int)BITS(tl, 10, 10);
    c->rect[2] = (int)BITS(sz, 0, 10) + 1;
    c->rect[3] = (int)BITS(sz, 10, 10) + 1;
    c->clear_ar = g_gx.bp[BP_CLEAR_AR];
    c->clear_gb = g_gx.bp[BP_CLEAR_GB];
    c->clear_z = g_gx.bp[BP_CLEAR_Z];
    c->cmode0 = g_gx.bp[BP_CMODE0];
    c->zmode = g_gx.bp[BP_ZMODE];
    if (c->type == CMD_PRESENT)
        gl_present();
}

void gl_present(void)
{
    static int slow = -1;
    if (slow < 0)
        slow = getenv("NSMBW_SLOW_US") ? atoi(getenv("NSMBW_SLOW_US")) : 0;
    if (slow)
        plat_sleep_us((uint32_t)slow);   /* emulate a slow host (3DS) */
    submit(0);
    gx_texture_frame();
    static uint64_t t0;
    static uint32_t n;
    uint64_t now = plat_time_us();
    if (!t0)
        t0 = now;
    if (++n == 120) {
        rt_log("[perf] %.1f fps (OpenGL)", 120 * 1e6 / (double)(now - t0));
        n = 0;
        t0 = now;
    }
}

/* ------------------------------------------------------- render thread */

static HDC s_dc;
static GLuint s_ibo;
static GLuint s_vao, s_vbo, s_efb_fbo, s_efb_tex, s_efb_depth, s_tmp_fbo;
static uint32_t s_frames, s_dump_every;

typedef struct {
    uint32_t hash;
    uint32_t key[64];
    int nkey;
    GLuint prog;
    GLint u_vp, u_z, u_reg, u_k, u_aref, u_s;
    int u_valid;
    float u_last[4 + 2 + 16 + 16 + 2];  /* uniform values last uploaded */
} Prog;
#define PROG_CACHE 1024
static Prog s_progs[PROG_CACHE];

typedef struct {
    uint32_t key;
    uint16_t w, h;
    GLuint tex;
    uint32_t last;
    int efb_copy;
    uint32_t addr;
    uint32_t mode;             /* wrap/filter last set on this texture (bit 31: valid) */
} GTex;

/* GL state last set by exec_draw; anything else touching state invalidates it */
static struct {
    int valid;
    GLuint prog;
    int active;
    GLuint bound[8];
    int depth, zfunc, zmask, blend, cull, cull_face;
    GLenum eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a;
    float blend_alpha;
    uint32_t cmask;
    int scissor[4];
} s_gs;
#define TEX_CACHE 4096
static GTex s_tex[TEX_CACHE];

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_log("GL shader error: %s\n%s", log, src);
    }
    return s;
}

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_c0; layout(location=2) in vec4 a_c1;\n"
    "layout(location=3) in vec3 a_t0; layout(location=4) in vec3 a_t1; layout(location=5) in vec3 a_t2;\n"
    "layout(location=6) in vec3 a_t3; layout(location=7) in vec3 a_t4; layout(location=8) in vec3 a_t5;\n"
    "layout(location=9) in vec3 a_t6; layout(location=10) in vec3 a_t7;\n"
    "uniform vec4 u_vp; uniform vec2 u_z;\n"
    "out vec4 v_c0; out vec4 v_c1; out vec3 v_t[8];\n"
    "void main(){\n"
    "  vec4 c = a_pos;\n"
    "  float ex = c.x * u_vp.x + u_vp.z * c.w;\n"
    "  float ey = c.y * u_vp.y + u_vp.w * c.w;\n"
    "  float d = c.z * u_z.x + u_z.y * c.w;\n"
    "  gl_Position = vec4(ex / 320.0 - c.w, c.w - ey / 264.0, 2.0 * d - c.w, c.w);\n"
    "  v_c0 = a_c0; v_c1 = a_c1;\n"
    "  v_t[0]=a_t0; v_t[1]=a_t1; v_t[2]=a_t2; v_t[3]=a_t3; v_t[4]=a_t4; v_t[5]=a_t5; v_t[6]=a_t6; v_t[7]=a_t7;\n"
    "}\n";

static const char *comp_name(int i) { static const char *n[4] = {"r", "g", "b", "a"}; return n[i & 3]; }

static void swap_str(const Draw *d, int table, char out[5])
{
    uint32_t k0 = d->ksel[table * 2], k1 = d->ksel[table * 2 + 1];
    out[0] = "rgba"[BITS(k0, 0, 2)];
    out[1] = "rgba"[BITS(k0, 2, 2)];
    out[2] = "rgba"[BITS(k1, 0, 2)];
    out[3] = "rgba"[BITS(k1, 2, 2)];
    out[4] = 0;
}

static void konst_c(uint32_t sel, char *out, size_t n)
{
    static const char *frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (sel < 8) snprintf(out, n, "vec3(%s)", frac[sel]);
    else if (sel >= 0x0C && sel <= 0x0F) snprintf(out, n, "u_k[%u].rgb", sel - 0x0C);
    else if (sel >= 0x10) snprintf(out, n, "vec3(u_k[%u].%s)", (sel - 0x10) & 3, comp_name((int)((sel - 0x10) >> 2)));
    else snprintf(out, n, "vec3(0.0)");
}

static void konst_a(uint32_t sel, char *out, size_t n)
{
    static const char *frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (sel < 8) snprintf(out, n, "%s", frac[sel]);
    else if (sel >= 0x10) snprintf(out, n, "u_k[%u].%s", (sel - 0x10) & 3, comp_name((int)((sel - 0x10) >> 2)));
    else snprintf(out, n, "0.0");
}

static void color_in(uint32_t sel, const char *kc, char *out, size_t n)
{
    static const char *regs[4] = {"r0", "r1", "r2", "r3"};
    switch (sel) {
    case 0: case 2: case 4: case 6: snprintf(out, n, "%s.rgb", regs[sel / 2]); break;
    case 1: case 3: case 5: case 7: snprintf(out, n, "vec3(%s.a)", regs[sel / 2]); break;
    case 8: snprintf(out, n, "tx.rgb"); break;
    case 9: snprintf(out, n, "vec3(tx.a)"); break;
    case 10: snprintf(out, n, "rs.rgb"); break;
    case 11: snprintf(out, n, "vec3(rs.a)"); break;
    case 12: snprintf(out, n, "vec3(1.0)"); break;
    case 13: snprintf(out, n, "vec3(0.5)"); break;
    case 14: snprintf(out, n, "%s", kc); break;
    default: snprintf(out, n, "vec3(0.0)"); break;
    }
}

static void alpha_in(uint32_t sel, const char *ka, char *out, size_t n)
{
    static const char *regs[4] = {"r0", "r1", "r2", "r3"};
    switch (sel) {
    case 0: case 1: case 2: case 3: snprintf(out, n, "%s.a", regs[sel]); break;
    case 4: snprintf(out, n, "tx.a"); break;
    case 5: snprintf(out, n, "rs.a"); break;
    case 6: snprintf(out, n, "%s", ka); break;
    default: snprintf(out, n, "0.0"); break;
    }
}

static const char *alpha_cmp(uint32_t f)
{
    static const char *c[8] = {"false", "a8 < %s", "a8 == %s", "a8 <= %s", "a8 > %s", "a8 != %s", "a8 >= %s", "true"};
    return c[f & 7];
}

/* builds the GLSL fragment shader for a draw's TEV configuration */
static char *gen_fs(const Draw *d)
{
    size_t cap = 32768, len = 0;
    char *s = malloc(cap);
#define P(...) (len += (size_t)snprintf(s + len, cap - len, __VA_ARGS__))
    int nstages = (int)BITS(d->bp_gen, 10, 4) + 1;
    P("#version 330 core\nin vec4 v_c0; in vec4 v_c1; in vec3 v_t[8];\n"
      "uniform sampler2D u_s[8]; uniform vec4 u_reg[4]; uniform vec4 u_k[4]; uniform vec2 u_aref;\n"
      "out vec4 o;\n"
      "float q8(float x){ return floor(clamp(x,0.0,1.0)*255.0+0.5); }\n"
      "void main(){\n vec4 r0=u_reg[0], r1=u_reg[1], r2=u_reg[2], r3=u_reg[3]; vec4 tx=vec4(1.0), rs=vec4(0.0);\n"
      " vec3 a,b,c,dd; float aa,ab,ac,ad; vec3 res; float resa;\n");
    static const char *regs[4] = {"r0", "r1", "r2", "r3"};
    static const char *bias[4] = {"0.0", "0.5", "-0.5", "0.0"};
    static const char *scale[4] = {"1.0", "2.0", "4.0", "0.5"};
    for (int st = 0; st < nstages; st++) {
        uint32_t tref = d->tref[st / 2] >> ((st & 1) * 12);
        uint32_t map = BITS(tref, 0, 3), coord = BITS(tref, 3, 3), en = BITS(tref, 6, 1), chan = BITS(tref, 7, 3);
        uint32_t cenv = d->cenv[st], aenv = d->aenv[st];
        uint32_t ksel = d->ksel[st / 2] >> ((st & 1) * 10);
        char sw[5], kc[48], ka[48], in[4][48], ain[4][48];
        if (en && d->tex[map].mode0) {
            swap_str(d, (int)BITS(aenv, 2, 2), sw);
            P(" tx = texture(u_s[%u], v_t[%u].xy / (v_t[%u].z == 0.0 ? 1.0 : v_t[%u].z)).%s;\n", map, coord, coord, coord, sw);
        } else {
            P(" tx = vec4(1.0);\n");
        }
        if (chan < 2) {
            swap_str(d, (int)BITS(aenv, 0, 2), sw);
            P(" rs = v_c%u.%s;\n", chan, sw);
        } else {
            P(" rs = vec4(0.0);\n");
        }
        konst_c(BITS(ksel, 4, 5), kc, sizeof kc);
        konst_a(BITS(ksel, 9, 5), ka, sizeof ka);
        uint32_t csel[4] = {BITS(cenv, 12, 4), BITS(cenv, 8, 4), BITS(cenv, 4, 4), BITS(cenv, 0, 4)};
        uint32_t asel[4] = {BITS(aenv, 13, 3), BITS(aenv, 10, 3), BITS(aenv, 7, 3), BITS(aenv, 4, 3)};
        for (int k = 0; k < 4; k++) {
            color_in(csel[k], kc, in[k], sizeof in[k]);
            alpha_in(asel[k], ka, ain[k], sizeof ain[k]);
        }
        P(" a=clamp(%s,0.0,1.0); b=clamp(%s,0.0,1.0); c=clamp(%s,0.0,1.0); dd=%s;\n", in[0], in[1], in[2], in[3]);
        P(" aa=clamp(%s,0.0,1.0); ab=clamp(%s,0.0,1.0); ac=clamp(%s,0.0,1.0); ad=%s;\n", ain[0], ain[1], ain[2], ain[3]);
        if (BITS(cenv, 16, 2) == 3) {          /* compare */
            uint32_t m = BITS(cenv, 20, 2) << 1 | BITS(cenv, 18, 1);
            const char *op = (m & 1) ? "==" : ">";
            switch (m >> 1) {
            case 0: P(" res = dd + ((q8(a.r) %s q8(b.r)) ? c : vec3(0.0));\n", op); break;
            case 1: P(" res = dd + ((q8(a.g)*256.0+q8(a.r) %s q8(b.g)*256.0+q8(b.r)) ? c : vec3(0.0));\n", op); break;
            case 2: P(" res = dd + ((q8(a.b)*65536.0+q8(a.g)*256.0+q8(a.r) %s q8(b.b)*65536.0+q8(b.g)*256.0+q8(b.r)) ? c : vec3(0.0));\n", op); break;
            default:
                P(" res = dd + vec3((q8(a.r) %s q8(b.r)) ? c.r : 0.0, (q8(a.g) %s q8(b.g)) ? c.g : 0.0, (q8(a.b) %s q8(b.b)) ? c.b : 0.0);\n",
                  op, op, op);
            }
        } else {
            P(" res = (dd %s mix(a, b, c) + %s) * %s;\n", BITS(cenv, 18, 1) ? "-" : "+",
              bias[BITS(cenv, 16, 2)], scale[BITS(cenv, 20, 2)]);
        }
        if (BITS(aenv, 16, 2) == 3) {
            uint32_t m = BITS(aenv, 20, 2) << 1 | BITS(aenv, 18, 1);
            P(" resa = ad + ((q8(aa) %s q8(ab)) ? ac : 0.0);\n", (m & 1) ? "==" : ">");
        } else {
            P(" resa = (ad %s mix(aa, ab, ac) + %s) * %s;\n", BITS(aenv, 18, 1) ? "-" : "+",
              bias[BITS(aenv, 16, 2)], scale[BITS(aenv, 20, 2)]);
        }
        P(" %s.rgb = %s;\n", regs[BITS(cenv, 22, 2)],
          BITS(cenv, 19, 1) ? "clamp(res, 0.0, 1.0)" : "clamp(res, -4.0, 4.0)");
        P(" %s.a = %s;\n", regs[BITS(aenv, 22, 2)],
          BITS(aenv, 19, 1) ? "clamp(resa, 0.0, 1.0)" : "clamp(resa, -4.0, 4.0)");
    }
    uint32_t lc = BITS(d->cenv[nstages - 1], 22, 2), la = BITS(d->aenv[nstages - 1], 22, 2);
    P(" o = clamp(vec4(%s.rgb, %s.a), 0.0, 1.0);\n", regs[lc], regs[la]);
    uint32_t ac = d->alpha, c0 = BITS(ac, 16, 3), c1 = BITS(ac, 19, 3), logic = BITS(ac, 22, 2);
    if (!(c0 == 7 && c1 == 7 && logic == 0) && !(s_gl_debug & 8)) {
        char e0[48], e1[48];
        snprintf(e0, sizeof e0, alpha_cmp(c0), "u_aref.x");
        snprintf(e1, sizeof e1, alpha_cmp(c1), "u_aref.y");
        static const char *lop[4] = {"&&", "||", "!=", "=="};
        P(" float a8 = q8(o.a);\n if (!((%s) %s (%s))) discard;\n", e0, lop[logic], e1);
    }
    P("}\n");
#undef P
    return s;
}

static int shader_key(const Draw *d, uint32_t *k)
{
    int n = 0, nstages = (int)BITS(d->bp_gen, 10, 4) + 1;
    k[n++] = (uint32_t)nstages;
    for (int i = 0; i < (nstages + 1) / 2; i++)
        k[n++] = d->tref[i];
    for (int s = 0; s < nstages; s++) {
        k[n++] = d->cenv[s];
        k[n++] = d->aenv[s] & 0xFFFFFF;
    }
    for (int i = 0; i < 8; i++)
        k[n++] = d->ksel[i];
    k[n++] = d->alpha & 0xFF0000u;
    uint32_t used = 0;
    for (int i = 0; i < 8; i++)
        if (d->tex[i].mode0)
            used |= 1u << i;
    k[n++] = used;
    return n;
}

static Prog *get_prog(const Draw *d)
{
    uint32_t key[64];
    int n = shader_key(d, key);
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ key[i]) * 16777619u;
    for (int probe = 0; probe < PROG_CACHE; probe++) {
        Prog *p = &s_progs[(h + probe) % PROG_CACHE];
        if (p->prog && p->hash == h && p->nkey == n && !memcmp(p->key, key, n * 4))
            return p;
        if (!p->prog) {
            char *fs = gen_fs(d);
            GLuint prog = glCreateProgram();
            GLuint vs = compile(GL_VERTEX_SHADER, k_vs), f = compile(GL_FRAGMENT_SHADER, fs);
            glAttachShader(prog, vs);
            glAttachShader(prog, f);
            glLinkProgram(prog);
            GLint ok;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[2048];
                glGetProgramInfoLog(prog, sizeof log, NULL, log);
                rt_log("GL link error: %s", log);
            }
            free(fs);
            p->prog = prog;
            p->hash = h;
            p->nkey = n;
            memcpy(p->key, key, n * 4);
            p->u_vp = glGetUniformLocation(prog, "u_vp");
            p->u_z = glGetUniformLocation(prog, "u_z");
            p->u_reg = glGetUniformLocation(prog, "u_reg");
            p->u_k = glGetUniformLocation(prog, "u_k");
            p->u_aref = glGetUniformLocation(prog, "u_aref");
            p->u_s = glGetUniformLocation(prog, "u_s");
            glUseProgram(prog);
            s_gs.prog = prog;
            GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
            glUniform1iv(p->u_s, 8, units);
            return p;
        }
    }
    return &s_progs[0];
}

/* open-addressed table: identity = (EFB copy ? address : decoded key, w, h) */
static uint32_t tex_hash(uint32_t id, int efb, int w, int h)
{
    uint32_t x = id * 2654435761u ^ (uint32_t)efb * 0x9E3779B9u ^ (uint32_t)(w << 16 | h) * 40503u;
    return (x ^ (x >> 15)) % TEX_CACHE;
}

static GTex *find_tex(uint32_t key, int efb, uint32_t addr, int w, int h)
{
    uint32_t id = efb ? addr : key, i = tex_hash(id, efb, w, h);
    for (int probe = 0; probe < 32; probe++, i = (i + 1) % TEX_CACHE) {
        GTex *t = &s_tex[i];
        if (!t->tex)
            return NULL;
        if (t->efb_copy == efb && (efb ? t->addr : t->key) == id && t->w == w && t->h == h)
            return t;
    }
    return NULL;
}

/* slot for a new entry; evicts the least recently used one in the probe run */
static GTex *alloc_tex_for(uint32_t id, int efb, int w, int h)
{
    uint32_t i = tex_hash(id, efb, w, h);
    GTex *oldest = NULL;
    for (int probe = 0; probe < 32; probe++, i = (i + 1) % TEX_CACHE) {
        GTex *t = &s_tex[i];
        if (!t->tex)
            return t;
        if (!oldest || t->last < oldest->last)
            oldest = t;
    }
    glDeleteTextures(1, &oldest->tex);
    memset(oldest, 0, sizeof *oldest);
    s_gs.valid = 0;
    memset(s_gs.bound, 0xFF, sizeof s_gs.bound);   /* the name may be reused */
    return oldest;
}

static GTex *texture_for(const TexRef *r)
{
    GTex *t = (s_gl_debug & 2) ? NULL : find_tex(0, 1, r->addr, r->w, r->h);    /* EFB copies win */
    if (!t && (s_gl_debug & 16)) {
        static uint32_t logged[64];
        static int nlogged;
        for (int i = 0; i < TEX_CACHE; i++)
            if (s_tex[i].tex && s_tex[i].efb_copy && s_tex[i].addr == r->addr) {
                int seen = 0;
                for (int k = 0; k < nlogged; k++)
                    seen |= logged[k] == r->addr;
                if (!seen && nlogged < 64) {
                    logged[nlogged++] = r->addr;
                    rt_log("[gl] texture %08lX %ux%u fmt %lu: EFB copy there is %ux%u", (unsigned long)r->addr,
                           r->w, r->h, (unsigned long)r->fmt, s_tex[i].w, s_tex[i].h);
                }
            }
    }
    if (!t && r->rgba) {
        t = find_tex(r->key, 0, 0, r->w, r->h);
        if (!t) {
            t = alloc_tex_for(r->key, 0, r->w, r->h);
            glGenTextures(1, &t->tex);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            s_gs.bound[s_gs.active & 7] = t->tex;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w, r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, r->rgba);
            t->key = r->key;
            t->w = r->w;
            t->h = r->h;
        }
    }
    if (!t)
        return NULL;
    t->last = s_frames;
    return t;
}

static void set_wrap_filter(uint32_t mode)
{
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[BITS(mode, 0, 2)]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[BITS(mode, 2, 2)]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, BITS(mode, 4, 1) ? GL_LINEAR : GL_NEAREST);
    uint32_t minf = BITS(mode, 5, 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (minf == 1 || minf == 3 || minf == 5) ? GL_LINEAR : GL_NEAREST);
}

static GLenum blend_factor(uint32_t f, int is_src)
{
    switch (f & 7) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

static void set_enabled(GLenum cap, int on, int *cur)
{
    if (s_gs.valid && *cur == on)
        return;
    *cur = on;
    if (on) glEnable(cap); else glDisable(cap);
}

static void exec_draw(const Draw *d)
{
    static int nocache = -1;
    if (nocache < 0) {
        nocache = getenv("NSMBW_GL_NOCACHE") != NULL;
        s_gl_debug = getenv("NSMBW_GL_DEBUG") ? atoi(getenv("NSMBW_GL_DEBUG")) : 0;
    }
    if (nocache)
        s_gs.valid = 0;
    if (!s_gs.valid) {
        memset(&s_gs, 0xFF, sizeof s_gs);      /* nothing matches */
        s_gs.valid = 0;
    }
    Prog *p = get_prog(d);
    if (!s_gs.valid || s_gs.prog != p->prog) {
        glUseProgram(p->prog);
        s_gs.prog = p->prog;
    }
    float u[40];
    u[0] = d->vp[0]; u[1] = d->vp[1]; u[2] = d->vp[3] - d->off[0]; u[3] = d->vp[4] - d->off[1];
    u[4] = d->vp[2] / 16777216.0f; u[5] = d->vp[5] / 16777216.0f;
    memcpy(u + 6, &d->reg[0][0], 16 * sizeof(float));
    memcpy(u + 22, &d->konst[0][0], 16 * sizeof(float));
    u[38] = (float)BITS(d->alpha, 0, 8); u[39] = (float)BITS(d->alpha, 8, 8);
    if (!p->u_valid || memcmp(u, p->u_last, sizeof u)) {
        if (!p->u_valid || memcmp(u, p->u_last, 6 * sizeof(float))) {
            glUniform4f(p->u_vp, u[0], u[1], u[2], u[3]);
            glUniform2f(p->u_z, u[4], u[5]);
        }
        glUniform4fv(p->u_reg, 4, u + 6);
        glUniform4fv(p->u_k, 4, u + 22);
        glUniform2f(p->u_aref, u[38], u[39]);
        memcpy(p->u_last, u, sizeof u);
        p->u_valid = 1;
    }
    for (int i = 0; i < 8; i++) {
        if (!d->tex[i].mode0)
            continue;
        if (s_gs.active != i) {
            glActiveTexture(GL_TEXTURE0 + i);
            s_gs.active = i;
        }
        GTex *t = texture_for(&d->tex[i]);     /* may create (and bind) a texture */
        GLuint id = t ? t->tex : 0;
        if (!s_gs.valid || s_gs.bound[i] != id) {
            glBindTexture(GL_TEXTURE_2D, id);
            s_gs.bound[i] = id;
        }
        uint32_t mode = (d->tex[i].mode0 & 0xFF) | 0x80000000u;
        if (t && t->mode != mode) {
            set_wrap_filter(mode);
            t->mode = mode;
        }
    }

    /* depth */
    int zon = (int)BITS(d->zmode, 0, 1) && !(s_gl_debug & 1);
    set_enabled(GL_DEPTH_TEST, zon, &s_gs.depth);
    int zfunc = zon ? (int)BITS(d->zmode, 1, 3) : s_gs.zfunc;
    int zmask = zon ? (int)BITS(d->zmode, 4, 1) : 0;
    if (!s_gs.valid || zfunc != s_gs.zfunc) {
        static const GLenum f[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        glDepthFunc(f[zfunc & 7]);
        s_gs.zfunc = zfunc;
    }
    if (!s_gs.valid || zmask != s_gs.zmask) {
        glDepthMask(zmask ? GL_TRUE : GL_FALSE);
        s_gs.zmask = zmask;
    }
    /* blending, write masks, constant alpha */
    uint32_t cm = d->cmode0;
    int const_a = BITS(d->cmode1, 8, 1);
    GLenum src = GL_ONE, dst = GL_ZERO, eq = GL_FUNC_ADD;
    if (BITS(cm, 0, 1)) {
        if (BITS(cm, 11, 1)) {
            eq = GL_FUNC_REVERSE_SUBTRACT;
        } else {
            src = blend_factor(BITS(cm, 8, 3), 1);
            dst = blend_factor(BITS(cm, 5, 3), 0);
        }
    }
    int blend = (BITS(cm, 0, 1) || const_a) && !(s_gl_debug & 4);
    set_enabled(GL_BLEND, blend, &s_gs.blend);
    if (blend) {
        GLenum eq_a = const_a ? GL_FUNC_ADD : eq;
        GLenum src_a = const_a ? GL_CONSTANT_ALPHA : src, dst_a = const_a ? GL_ZERO : dst;
        if (!s_gs.valid || s_gs.eq_rgb != eq || s_gs.eq_a != eq_a) {
            glBlendEquationSeparate(eq, eq_a);
            s_gs.eq_rgb = eq;
            s_gs.eq_a = eq_a;
        }
        if (!s_gs.valid || s_gs.src_rgb != src || s_gs.dst_rgb != dst || s_gs.src_a != src_a || s_gs.dst_a != dst_a) {
            glBlendFuncSeparate(src, dst, src_a, dst_a);
            s_gs.src_rgb = src; s_gs.dst_rgb = dst; s_gs.src_a = src_a; s_gs.dst_a = dst_a;
        }
        float ba = BITS(d->cmode1, 0, 8) / 255.0f;
        if (!s_gs.valid || s_gs.blend_alpha != ba) {
            glBlendColor(0, 0, 0, ba);
            s_gs.blend_alpha = ba;
        }
    }
    uint32_t cmask = BITS(cm, 3, 2);
    if (!s_gs.valid || s_gs.cmask != cmask) {
        glColorMask(BITS(cm, 3, 1), BITS(cm, 3, 1), BITS(cm, 3, 1), BITS(cm, 4, 1));
        s_gs.cmask = cmask;
    }
    /* after the y flip to GL, GX cull mode 1 is GL_FRONT and 2 is GL_BACK
     * (checked against the software renderer: EGG light textures and models) */
    uint32_t cull = (s_gl_debug & 128) ? 0 : BITS(d->bp_gen, 14, 2);
    set_enabled(GL_CULL_FACE, cull != 0, &s_gs.cull);
    if (cull && (!s_gs.valid || s_gs.cull_face != (int)cull)) {
        glCullFace(cull == 1 ? GL_FRONT : cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
        s_gs.cull_face = (int)cull;
    }
    int x0 = d->sc[0] < 0 ? 0 : d->sc[0], y0 = d->sc[1] < 0 ? 0 : d->sc[1];
    int x1 = d->sc[2] > EFB_W ? EFB_W : d->sc[2], y1 = d->sc[3] > EFB_H ? EFB_H : d->sc[3];
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    int sc[4] = {x0 * s_scale, (EFB_H - y1) * s_scale, (x1 - x0) * s_scale, (y1 - y0) * s_scale};
    if (!s_gs.valid || memcmp(sc, s_gs.scissor, sizeof sc)) {
        glScissor(sc[0], sc[1], sc[2], sc[3]);
        memcpy(s_gs.scissor, sc, sizeof sc);
    }
    s_gs.valid = 1;
    if ((s_gl_debug & 64) && d->sc[1] >= 456) {
        static int n;
        if (n++ < 3) {
            char *fs = gen_fs(d);
            rt_log("[gl] offscreen draw: %u verts, zmode %08lX cmode0 %08lX cmode1 %08lX gen %08lX %s", d->count,
                   (unsigned long)d->zmode, (unsigned long)d->cmode0, (unsigned long)d->cmode1, (unsigned long)d->bp_gen, fs);
            free(fs);
            for (int i = 0; i < 8; i++)
                if (d->tex[i].mode0) {
                    GTex *t = texture_for(&d->tex[i]);
                    rt_log("[gl]   tex%d addr %08lX %ux%u fmt %lu key %08lX rgba %p -> %s", i, (unsigned long)d->tex[i].addr,
                           d->tex[i].w, d->tex[i].h, (unsigned long)d->tex[i].fmt, (unsigned long)d->tex[i].key,
                           (const void *)d->tex[i].rgba, t ? (t->efb_copy ? "EFB copy" : "decoded") : "MISSING");
                }
        }
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)d->count, GL_UNSIGNED_INT, (const void *)((uintptr_t)d->first * 4));
}

void png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);

void png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
static GLuint s_copy_prog, s_copy_vao;
static GLint s_copy_u_src, s_copy_u_fmt, s_copy_u_intensity;

static const char *k_copy_vs =
    "#version 330 core\n"
    "uniform vec4 u_src;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "    v_uv = u_src.xy + p * u_src.zw;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* fmt: EFB copy texture format (0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3,
 * 6 RGBA8, 7 A8, 8 R8, 9 G8, 10 B8, 11 RG8, 12 GB8) */
static const char *k_copy_fs =
    "#version 330 core\n"
    "uniform sampler2D u_efb;\n"
    "uniform int u_fmt, u_intensity;\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    vec4 c = texture(u_efb, v_uv);\n"
    "    float l = u_intensity != 0 ? dot(c.rgb, vec3(0.299, 0.587, 0.114)) : c.r;\n"
    "    if (u_fmt <= 1 || u_fmt == 8) o = vec4(l);\n"
    "    else if (u_fmt == 2 || u_fmt == 3) o = vec4(l, l, l, c.a);\n"
    "    else if (u_fmt == 4 || u_fmt == 5) o = vec4(c.rgb, 1.0);\n"
    "    else if (u_fmt == 7) o = vec4(c.a);\n"
    "    else if (u_fmt == 9) o = vec4(c.g);\n"
    "    else if (u_fmt == 10) o = vec4(c.b);\n"
    "    else if (u_fmt == 11) o = vec4(c.g, c.g, c.g, c.r);\n"
    "    else if (u_fmt == 12) o = vec4(c.b, c.b, c.b, c.g);\n"
    "    else o = c;\n"
    "}\n";

static void exec_copy(const Cmd *c)
{
    int x0 = c->rect[0], y0 = c->rect[1], w = c->rect[2], h = c->rect[3];
    int half = BITS(c->trigger, 9, 1);
    int tw = half ? w / 2 : w, th = half ? h / 2 : h;
    if (c->type == CMD_COPY && tw > 0 && th > 0) {
        GTex *t = find_tex(0, 1, c->dest, tw, th);
        if (!t) {
            t = alloc_tex_for(c->dest, 1, tw, th);
            glGenTextures(1, &t->tex);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            t->efb_copy = 1;
            t->addr = c->dest;
            t->w = (uint16_t)tw;
            t->h = (uint16_t)th;
        }
        t->last = s_frames;
        /* draw the EFB rectangle into the texture, converted to what sampling
         * the copy's texture format gives (intensity, single channels, no alpha) */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_tmp_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, tw, th);
        glUseProgram(s_copy_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_efb_tex);
        uint32_t tpf = BITS(c->trigger, 3, 4);
        static int efb_dumped;
        if ((s_gl_debug & 32) && !efb_dumped && s_frames >= 2500 && y0 >= 456) {
            efb_dumped = 1;
            int pw = EFB_W * s_scale, ph = EFB_H * s_scale;
            uint8_t *img = malloc((size_t)pw * ph * 4), *flip = malloc((size_t)pw * ph * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
            glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, img);
            for (int y = 0; y < ph; y++)
                memcpy(flip + (size_t)y * pw * 4, img + (size_t)(ph - 1 - y) * pw * 4, (size_t)pw * 4);
            char path[512];
            snprintf(path, sizeof path, "%sframes/efb_full.png", plat_data_dir());
            png_write_rgba(path, flip, pw, ph);
            free(img);
            free(flip);
        }
        if (s_gl_debug & 16) {
            static int n;
            if (n++ < 200)
                rt_log("[gl] copy %d,%d %dx%d -> %08lX %dx%d fmt %lu", x0, y0, w, h, (unsigned long)c->dest, tw, th,
                       (unsigned long)((tpf >> 1) + (tpf & 1) * 8));
        }
        /* texel row 0 = top EFB row */
        glUniform4f(s_copy_u_src, (float)x0 / EFB_W, (float)(EFB_H - y0) / EFB_H, (float)w / EFB_W, -(float)h / EFB_H);
        glUniform1i(s_copy_u_fmt, (int)((tpf >> 1) + (tpf & 1) * 8));
        glUniform1i(s_copy_u_intensity, (int)BITS(c->trigger, 15, 1));
        glBindVertexArray(s_copy_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(s_vao);
        glViewport(0, 0, EFB_W * s_scale, EFB_H * s_scale);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_efb_fbo);
        glEnable(GL_SCISSOR_TEST);
    }
    if (BITS(c->trigger, 11, 1)) {             /* clear the rectangle */
        glScissor(x0 * s_scale, (EFB_H - y0 - h) * s_scale, w * s_scale, h * s_scale);
        GLbitfield bits = 0;
        int cu = BITS(c->cmode0, 3, 1), au = BITS(c->cmode0, 4, 1), zu = BITS(c->zmode, 4, 1);
        glColorMask(cu, cu, cu, au);
        if (cu || au) {
            glClearColor((c->clear_ar & 255) / 255.0f, (c->clear_gb >> 8) / 255.0f, (c->clear_gb & 255) / 255.0f,
                         ((c->clear_ar >> 8) & 255) / 255.0f);
            bits |= GL_COLOR_BUFFER_BIT;
        }
        if (zu) {
            glDepthMask(GL_TRUE);
            glClearDepth((c->clear_z & 0xFFFFFF) / 16777216.0);
            bits |= GL_DEPTH_BUFFER_BIT;
        }
        if (bits)
            glClear(bits);
    }
}

static void exec_present(const Cmd *c)
{
    int w = c->rect[2], h = c->rect[3];
    if (w > EFB_W) w = EFB_W;
    if (h > EFB_H) h = EFB_H;
    s_frames++;
    RECT rc;
    extern HWND g_game_window;
    GetClientRect(g_game_window, &rc);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, (EFB_H - h) * s_scale, w * s_scale, EFB_H * s_scale, 0, 0, rc.right, rc.bottom,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (s_dump_every && s_frames % s_dump_every == 0) {
        int pw = w * s_scale, ph = h * s_scale;
        uint8_t *img = malloc((size_t)pw * ph * 4), *flip = malloc((size_t)pw * ph * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, (EFB_H - h) * s_scale, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, img);
        for (int y = 0; y < ph; y++) {
            memcpy(flip + (size_t)y * pw * 4, img + (size_t)(ph - 1 - y) * pw * 4, (size_t)pw * 4);
            for (int x = 0; x < pw; x++)
                flip[((size_t)y * pw + x) * 4 + 3] = 255;
        }
        char path[512];
        snprintf(path, sizeof path, "%sframes", plat_data_dir());
        plat_mkdir(path);
        snprintf(path, sizeof path, "%sframes/frame_%05lu.png", plat_data_dir(), (unsigned long)s_frames);
        png_write_rgba(path, flip, pw, ph);
        free(img);
        free(flip);
    }
    {
        extern uint64_t g_gl_busy_us;
        static uint64_t t_swap, t_batch_start, acc_swap, acc_frame, n;
        uint64_t t = plat_time_us();
        SwapBuffers(s_dc);
        uint64_t t2 = plat_time_us();
        acc_swap += t2 - t;
        if (t_batch_start)
            acc_frame += t2 - t_batch_start;
        t_batch_start = t2;
        (void)t_swap;
        if (++n == 120) {
            extern uint64_t g_gl_phase_us[4], g_gl_counts[3];
            rt_log("[gl] per frame: %.2f ms interval, %.2f ms busy on render thread, %.2f ms in SwapBuffers",
                   acc_frame / 120000.0, g_gl_busy_us / 120000.0, acc_swap / 120000.0);
            rt_log("[gl]   upload %.2f, draws %.2f, copies %.2f, present %.2f ms; %llu draws, %llu verts, %llu copies",
                   g_gl_phase_us[0] / 120000.0, g_gl_phase_us[1] / 120000.0, g_gl_phase_us[2] / 120000.0,
                   g_gl_phase_us[3] / 120000.0, g_gl_counts[0] / 120, g_gl_counts[1] / 120, g_gl_counts[2] / 120);
            memset(g_gl_phase_us, 0, sizeof g_gl_phase_us);
            memset(g_gl_counts, 0, sizeof g_gl_counts);
            n = acc_swap = acc_frame = g_gl_busy_us = 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glEnable(GL_SCISSOR_TEST);
}

uint64_t g_gl_phase_us[4], g_gl_counts[3];   /* upload, draws, copies, present; draws, verts, copies */

static void exec_batch(Batch *b)
{
    uint64_t t0 = plat_time_us(), t1;
    s_gs.valid = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glViewport(0, 0, EFB_W * s_scale, EFB_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->nverts * sizeof(GXVertex), b->verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->nidx * sizeof(uint32_t), b->idx, GL_STREAM_DRAW);
    g_gl_counts[1] += b->nverts;
    t1 = plat_time_us();
    g_gl_phase_us[0] += t1 - t0;
    t0 = t1;
    for (uint32_t i = 0; i < b->ncmds; i++) {
        const Cmd *c = &b->cmds[i];
        int ph;
        if (c->type == CMD_DRAW) {
            exec_draw(&b->draws[c->index]);
            ph = 1;
            g_gl_counts[0]++;
        } else if (c->type == CMD_COPY) {
            exec_copy(c);
            s_gs.valid = 0;
            ph = 2;
            g_gl_counts[2]++;
        } else {
            exec_present(c);
            exec_copy(c);          /* the XFB copy's clear happens after presenting */
            s_gs.valid = 0;
            ph = 3;
        }
        t1 = plat_time_us();
        g_gl_phase_us[ph] += t1 - t0;
        t0 = t1;
    }
}

static int gl_setup(void)
{
    extern HWND g_game_window;
    if (!g_game_window)
        return 0;
    s_dc = GetDC(g_game_window);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(s_dc, &pfd);
    if (!pf || !SetPixelFormat(s_dc, pf, &pfd))
        return 0;
    HGLRC tmp = wglCreateContext(s_dc);
    if (!tmp || !wglMakeCurrent(s_dc, tmp))
        return 0;
    PFN_wglCreateContextAttribsARB create =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create) {
        int attr[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        HGLRC ctx = create(s_dc, NULL, attr);
        if (ctx) {
            wglMakeCurrent(s_dc, ctx);
            wglDeleteContext(tmp);
        }
    }
#define LOAD(ret, name, args) \
    name = (PFN_##name)wglGetProcAddress(#name); \
    if (!name) { rt_log("GL: missing %s", #name); return 0; }
    GLFUNCS(LOAD)
#undef LOAD
    typedef BOOL(WINAPI * PFN_swap)(int);
    PFN_swap swap = (PFN_swap)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap)
        swap(0);                   /* pacing comes from the emulated VI */

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    GLsizei stride = sizeof(GXVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GXVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(GXVertex, color));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)(offsetof(GXVertex, color) + 16));
    for (int i = 0; i < 8; i++) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void *)(offsetof(GXVertex, tex) + 12 * i));
    }

    glGenTextures(1, &s_efb_tex);
    glBindTexture(GL_TEXTURE_2D, s_efb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * s_scale, EFB_H * s_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &s_efb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_efb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EFB_W * s_scale, EFB_H * s_scale);
    glGenFramebuffers(1, &s_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_efb_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_efb_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("GL: EFB framebuffer incomplete");
        return 0;
    }
    /* GX does not clip against the near/far planes, it clamps depth; flat 2D
     * passes (EGG light textures) sit exactly on the near plane */
    glEnable(0x864F /* GL_DEPTH_CLAMP */);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glGenFramebuffers(1, &s_tmp_fbo);
    s_copy_prog = glCreateProgram();
    glAttachShader(s_copy_prog, compile(GL_VERTEX_SHADER, k_copy_vs));
    glAttachShader(s_copy_prog, compile(GL_FRAGMENT_SHADER, k_copy_fs));
    glLinkProgram(s_copy_prog);
    s_copy_u_src = glGetUniformLocation(s_copy_prog, "u_src");
    s_copy_u_fmt = glGetUniformLocation(s_copy_prog, "u_fmt");
    s_copy_u_intensity = glGetUniformLocation(s_copy_prog, "u_intensity");
    glUseProgram(s_copy_prog);
    glUniform1i(glGetUniformLocation(s_copy_prog, "u_efb"), 0);
    glGenVertexArrays(1, &s_copy_vao);
    rt_log("OpenGL: %s / %s, EFB at %dx", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION), s_scale);
    return 1;
}

static plat_sem *s_ready;
uint64_t g_gl_busy_us;

static void render_thread(void *arg)
{
    (void)arg;
    s_ok = gl_setup();
    plat_sem_post(s_ready);
    if (!s_ok)
        return;
    for (;;) {
        plat_sem_wait(s_go);
        uint64_t t0 = plat_time_us();
        exec_batch(&s_batch[s_exec]);
        g_gl_busy_us += plat_time_us() - t0;
        plat_sem_post(s_done);
    }
}

int gl_init(void)
{
    const char *e = getenv("NSMBW_DUMP_FRAMES");
    s_dump_every = e ? (uint32_t)atoi(e) : 0;
    const char *sc = getenv("NSMBW_RES_SCALE");
    if (sc && atoi(sc) >= 1 && atoi(sc) <= 4)
        s_scale = atoi(sc);
    s_go = plat_sem_create();
    s_done = plat_sem_create();
    s_ready = plat_sem_create();
    plat_thread_start(render_thread, NULL, 1024 * 1024);
    plat_sem_wait(s_ready);
    if (s_ok) {
        extern volatile int g_gl_active;
        g_gl_active = 1;
    }
    return s_ok;
}
#endif
