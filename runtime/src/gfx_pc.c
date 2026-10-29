/* PC: picks the render backend (OpenGL by default, NSMBW_RENDERER=soft for
 * the reference software renderer). */
#ifndef __3DS__
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "gx_state.h"

void soft_init(void);
void soft_draw_triangles(const GXVertex *v, uint32_t count);
void soft_efb_copy(uint32_t trigger);
void soft_present(void);
void soft_texture_will_change(void);
int gl_init(void);
void gl_draw_indexed(const GXVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t nidx);
void gl_efb_copy(uint32_t trigger);
void gl_present(void);
void gl_texture_will_change(void);

static int s_gl;

void gfx_init(void)
{
    const char *r = getenv("NSMBW_RENDERER");
    s_gl = !(r && !strcmp(r, "soft")) && gl_init();
    if (!s_gl)
        soft_init();
    rt_log("renderer: %s", s_gl ? "OpenGL" : "software");
}

void gfx_draw_triangles(const GXVertex *v, uint32_t count) { soft_draw_triangles(v, count); }

void gfx_draw_indexed(const GXVertex *v, uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{
    if (s_gl)
        gl_draw_indexed(v, nverts, idx, nidx);
    else
        gx_expand_indexed(v, idx, nidx, soft_draw_triangles);
}

void gfx_efb_copy(uint32_t trigger) { if (s_gl) gl_efb_copy(trigger); else soft_efb_copy(trigger); }
void gfx_present(void) { if (s_gl) gl_present(); else soft_present(); }
void gfx_texture_will_change(void) { if (s_gl) gl_texture_will_change(); else soft_texture_will_change(); }
#endif
