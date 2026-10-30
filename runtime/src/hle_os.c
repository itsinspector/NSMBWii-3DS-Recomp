/* OS-level HLE: guest threads, context switches, interrupt delivery, timers.
 *
 * The game's own scheduler (OSThread.c) stays recompiled. What we replace are
 * the CPU primitives it is built on:
 *
 *  - Every guest context that is running or suspended lives on a host thread
 *    with its own host stack. Only the thread holding the "baton" executes
 *    guest code; the others sleep on a semaphore.
 *  - OSSaveContext returns twice. The lifter emits setjmp() at each call site
 *    (in the caller's frame) and calls rt_save_context() to record it.
 *  - OSLoadContext either longjmps back to such a save point (possibly on
 *    another host thread: baton handoff) or starts a fresh context
 *    (OSCreateThread'd: srr0 = entry) on an idle host thread.
 *  - Interrupts are delivered only at safe points: OSEnableInterrupts /
 *    OSRestoreInterrupts and busy-wait loops (rt_poll). Delivery calls the
 *    game's real exception handler, which ends with OSLoadContext.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "platform.h"

/* OSContext layout */
#define CTX_GPR   0x000
#define CTX_CR    0x080
#define CTX_LR    0x084
#define CTX_CTR   0x088
#define CTX_XER   0x08C
#define CTX_FPR   0x090
#define CTX_FPSCR 0x194
#define CTX_SRR0  0x198
#define CTX_SRR1  0x19C
#define CTX_STATE 0x1A2
#define CTX_GQR   0x1A4
#define CTX_PSF   0x1C8

#define OS_CURRENTCONTEXT_PADDR 0x800000C0u
#define OS_CURRENTCONTEXT       0x800000D4u
#define MARK_IRQ                0xFFFFFFF0u   /* srr0 of contexts saved by our dispatcher */

#define EXC_EXTERNAL   4
#define EXC_DECREMENTER 8

/* deep guest call chains; on the 3DS memory is tight (88 MiB of guest RAM in
 * ~110 MiB of heap) and measured depths stay well below this */
#ifdef __3DS__
#define HOST_STACK (512 * 1024)
#else
#define HOST_STACK (2 * 1024 * 1024)
#endif

volatile uint32_t g_rt_pending;

/* ------------------------------------------------------------------ log */

void rt_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log_write(buf);
}

#ifdef RT_TRACE_RING
uint32_t g_trace_ring[1024];
uint32_t g_trace_pos;
#endif

static struct HostThread *s_cur_for_dump(void);

/* called by the PC watchdog: recent guest function entries */
void rt_debug_dump(void)
{
#ifdef RT_TRACE_ENABLED
    const char *env = getenv("NSMBW_TRACE_DEPTH");
    uint32_t depth = env ? (uint32_t)atoi(env) : 64, pos = g_trace_pos;
    for (uint32_t k = depth > 1024 ? 1024 : depth; k > 0; k--) {
        uint32_t a = g_trace_ring[(pos - k) & 1023], off;
        const char *n = symbol_name(a, &off);
        if (!strncmp(n, "_savegpr", 8) || !strncmp(n, "_restgpr", 8))
            continue;
        rt_log("  %08lX %s%s", (unsigned long)a, off ? "~" : "", n);
    }
#endif
}

/* Guest OS threads with their call chains (state, priority, wait queue,
 * return addresses from the stack back chain), for diagnosing hangs. */
void rt_guest_threads_dump(void)
{
    static const char *states[] = {"?", "ready", "running", "?", "waiting", "?", "?", "?", "moribund"};
    uint32_t th = ld32(0x800000DC), cur = ld32(0x800000E4);
    rt_log("guest threads (current %08lX):", (unsigned long)cur);
    PPCContext *c = (PPCContext *)s_cur_for_dump();
    if (c) {
        uint32_t off, sp = c->r[1];
        rt_log(" live: lr %08lX %s+0x%lX r3 %08lX r4 %08lX r5 %08lX r31 %08lX", (unsigned long)c->lr,
               symbol_name(c->lr, &off), (unsigned long)off, (unsigned long)c->r[3], (unsigned long)c->r[4],
               (unsigned long)c->r[5], (unsigned long)c->r[31]);
        for (int k = 0; k < 16 && sp >= 0x80000000u && sp < 0x94000000u; k++) {
            uint32_t next = ld32(sp);
            if (!next || next <= sp)
                break;
            uint32_t ra = ld32(next + 4);
            rt_log("   <- %08lX %s+0x%lX", (unsigned long)ra, symbol_name(ra, &off), (unsigned long)off);
            sp = next;
        }
    }
    for (int n = 0; th && n < 32; n++, th = ld32(th + 0x2FC)) {
        uint32_t state = ld16(th + 0x2C8);
        uint32_t srr0 = ld32(th + 0x198), lr = ld32(th + 0x84), sp = ld32(th + 4), off;
        rt_log(" thread %08lX %s prio %lu queue %08lX pc %08lX %s", (unsigned long)th,
               state < 9 ? states[state] : "?", (unsigned long)ld32(th + 0x2D0), (unsigned long)ld32(th + 0x2DC),
               (unsigned long)srr0, symbol_name(srr0, &off));
        rt_log("   lr %08lX %s", (unsigned long)lr, symbol_name(lr, &off));
        for (int k = 0; k < 14 && sp >= 0x80000000u && sp < 0x94000000u; k++) {
            uint32_t next = ld32(sp);
            if (!next || next <= sp)
                break;
            uint32_t ra = ld32(next + 4);
            rt_log("   <- %08lX %s+0x%lX", (unsigned long)ra, symbol_name(ra, &off), (unsigned long)off);
            sp = next;
        }
    }
}

void rt_fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    PPCContext *c = (PPCContext *)s_cur_for_dump();
    if (c) {
        for (int i = 0; i < 32; i += 4)
            rt_log("  r%-2d %08lX %08lX %08lX %08lX", i, (unsigned long)c->r[i], (unsigned long)c->r[i + 1],
                   (unsigned long)c->r[i + 2], (unsigned long)c->r[i + 3]);
        rt_log("  lr %08lX ctr %08lX msr %08lX", (unsigned long)c->lr, (unsigned long)c->ctr, (unsigned long)c->msr);
    }
#ifdef RT_TRACE_ENABLED
    rt_log("last guest functions entered (oldest first, register save/restore and interrupt toggles omitted):");
    const char *env = getenv("NSMBW_TRACE_DEPTH");
    uint32_t depth = env ? (uint32_t)atoi(env) : 64;
    if (depth > 1024)
        depth = 1024;
    for (uint32_t k = depth; k > 0; k--) {
        uint32_t a = g_trace_ring[(g_trace_pos - k) & 1023];
        uint32_t off;
        const char *n = symbol_name(a, &off);
        if (!strncmp(n, "_savegpr", 8) || !strncmp(n, "_restgpr", 8) || strstr(n, "Interrupts"))
            continue;
        rt_log("  %08lX %s%s", (unsigned long)a, off ? "~" : "", n);
    }
#endif
    exit(1);
}

/* ----------------------------------------------------------------- time */

uint64_t rt_ticks(void)
{
    static uint64_t t0;
    uint64_t us = rt_time_us();
    if (!t0)
        t0 = us;
    return (us - t0) * 243 / 4;     /* 60.75 ticks per microsecond */
}

static uint64_t s_dec_deadline;
static int s_dec_armed;

void rt_dec_write(uint32_t v)
{
    s_dec_deadline = rt_ticks() + (int64_t)(int32_t)v;
    s_dec_armed = 1;
    if ((int32_t)v <= 0)
        g_rt_pending = 1;
}

uint32_t rt_dec_read(void)
{
    return (uint32_t)(int32_t)(int64_t)(s_dec_deadline - rt_ticks());
}

#ifdef RT_TRACE_ENABLED
/* poor man's sampling profiler (trace builds): the timer thread counts the
 * guest function entered last, and the counts are logged every ~10 s */
#define SAMPLE_SLOTS 4096
static struct { uint32_t addr, hits; } s_samples[SAMPLE_SLOTS];
static uint32_t s_nsamples;
static volatile int s_sample_report;

static void take_sample(void)
{
    uint32_t a = g_trace_ring[(g_trace_pos - 1) & 1023];
    uint32_t h = (a * 2654435761u) >> 20;
    for (int i = 0; i < SAMPLE_SLOTS; i++, h = (h + 1) & (SAMPLE_SLOTS - 1)) {
        if (s_samples[h].addr == a || !s_samples[h].addr) {
            s_samples[h].addr = a;
            s_samples[h].hits++;
            break;
        }
    }
    if (++s_nsamples == 10000)
        s_sample_report = 1;
}

static void sample_report(void)
{
    s_sample_report = 0;
    rt_log("[samples] last guest function entered, %lu samples:", (unsigned long)s_nsamples);
    for (int k = 0; k < 30; k++) {
        int best = -1;
        for (int i = 0; i < SAMPLE_SLOTS; i++)
            if (s_samples[i].hits && (best < 0 || s_samples[i].hits > s_samples[best].hits))
                best = i;
        if (best < 0)
            break;
        uint32_t off;
        rt_log("  %5.1f%%  %08lX %s", 100.0 * s_samples[best].hits / s_nsamples, (unsigned long)s_samples[best].addr,
               symbol_name(s_samples[best].addr, &off));
        s_samples[best].hits = 0;
    }
    memset(s_samples, 0, sizeof s_samples);
    s_nsamples = 0;
}
#endif

static void timer_thread(void *arg)
{
    (void)arg;
    for (;;) {
        plat_sleep_us(1000);
#ifdef RT_TRACE_ENABLED
        take_sample();
#endif
        g_rt_pending = 1;
    }
}

void rt_raise(void) { g_rt_pending = 1; }

#ifdef __3DS__
uint64_t g_prof_us[PROF_SLOTS];
uint32_t g_prof_tick;
#endif

/* called once per presented frame by the 3DS backend */
void rt_prof_frame(void)
{
#ifdef __3DS__
    static uint64_t t0;
    static uint32_t n;
    uint64_t now = plat_time_us();
    if (!t0)
        t0 = now;
    if (++n < 120)
        return;
    double total = (double)(now - t0);
    if (total > 0) {
        rt_log("[prof] %.1f fps: T&L %.0f%%, draw submit %.0f%% (textures %.0f%%), audio %.0f%%, GPU wait %.0f%%, idle %.0f%%, game+rest %.0f%%",
               120e6 / total, 100 * g_prof_us[PROF_TL] / total, 100 * g_prof_us[PROF_SUBMIT] / total,
               100 * g_prof_us[PROF_TEX] / total, 100 * g_prof_us[PROF_AUDIO] / total,
               100 * g_prof_us[PROF_GPUWAIT] / total, 100 * g_prof_us[PROF_IDLE] / total,
               100 * (total - g_prof_us[PROF_TL] - g_prof_us[PROF_SUBMIT] - g_prof_us[PROF_AUDIO] -
                      g_prof_us[PROF_GPUWAIT] - g_prof_us[PROF_IDLE]) / total);
    }
    memset(g_prof_us, 0, sizeof g_prof_us);
    n = 0;
    t0 = now;
#endif
}

/* ------------------------------------------------------- virtual clock */

static uint64_t s_vt_base, s_rt_base, s_idle_us;
static uint32_t s_vt_scale = 1024;     /* guest us per real us, x1024 */

uint64_t rt_time_us(void)
{
    uint64_t r = plat_time_us();
    if (!s_rt_base)
        s_rt_base = r;
    return s_vt_base + (r - s_rt_base) * s_vt_scale / 1024;
}

/* real (plat_time_us) time at which the guest clock reads `guest` */
uint64_t rt_guest_to_real_us(uint64_t guest)
{
    if (guest <= s_vt_base)
        return s_rt_base;
    return s_rt_base + (guest - s_vt_base) * 1024 / s_vt_scale;
}

/* Called every 120 guest VI frames. A guest that never idles cannot keep up
 * with real time: its timer-driven threads (Wiimote, sound) then always have
 * work, never sleep, and starve the main thread. Guest time is slowed until
 * the guest idles again, as on a slower console. */
void rt_time_adapt(void)
{
    static uint64_t t0;
    uint64_t now = plat_time_us();
    if (!t0 || now <= t0) {
        t0 = now;
        s_idle_us = 0;
        return;
    }
    uint32_t idle = (uint32_t)(s_idle_us * 1024 / (now - t0)), scale = s_vt_scale;
    t0 = now;
    s_idle_us = 0;
    if (idle < 40)                    /* < 4% idle: falling behind */
        scale = scale * 820 / 1024;
    else if (idle > 250)              /* > 25% idle: room to speed up */
        scale = scale * 1180 / 1024;
    if (scale < 100) scale = 100;     /* 10%: the emulated 3DS at 268 MHz needs it */
    if (scale > 1024) scale = 1024;
    if (scale == s_vt_scale)
        return;
    uint64_t r = plat_time_us();
    s_vt_base = rt_time_us();
    s_rt_base = r;
    s_vt_scale = scale;
    rt_log("[time] guest clock at %u%% of real time", scale * 100 / 1024);
}

/* ------------------------------------------------------- context records */

typedef struct HostThread HostThread;

typedef struct SaveRec {
    uint32_t ctx;           /* guest OSContext address */
    uint32_t marker;        /* srr0 written at save time */
    HostThread *owner;
    int valid;
    jmp_buf jb;
} SaveRec;

enum { CMD_NONE, CMD_RESUME, CMD_START };

struct HostThread {
    PPCContext cpu;
    plat_sem *wake;
    jmp_buf top;
    int cmd;
    SaveRec *rec;
    uint32_t start_ctx;
    int nsaved;
    int id;
    uintptr_t stack_base;   /* host SP at thread entry, for stack depth checks */
    uintptr_t stack_peak;   /* deepest depth seen */
    HostThread *next;
};

#define MAX_RECS 512
static SaveRec s_recs[MAX_RECS];
static HostThread *s_cur;        /* baton holder */

/* cpu is the first member: the baton holder's guest registers */
static HostThread *s_cur_for_dump(void) { return s_cur; }

/* for a host crash handler: the guest thread running and the last guest
 * functions entered, oldest first */
void rt_crash_report(void (*out)(const char *line))
{
    char buf[160];
    HostThread *t = s_cur_for_dump();
    if (t) {
        snprintf(buf, sizeof buf, "guest thread %d: lr %08lX r1 %08lX r3 %08lX ctr %08lX", t->id,
                 (unsigned long)t->cpu.lr, (unsigned long)t->cpu.r[1], (unsigned long)t->cpu.r[3],
                 (unsigned long)t->cpu.ctr);
        out(buf);
    }
#ifdef RT_TRACE_RING
    out("last guest functions entered:");
    for (uint32_t k = 32; k > 0; k--) {
        uint32_t a = g_trace_ring[(g_trace_pos - k) & 1023], off;
        snprintf(buf, sizeof buf, "  %08lX %s", (unsigned long)a, symbol_name(a, &off));
        out(buf);
    }
#endif
}
static HostThread *s_threads;
static int s_nthreads;

static SaveRec *rec_find(uint32_t ctx)
{
    for (int i = 0; i < MAX_RECS; i++)
        if (s_recs[i].ctx == ctx)
            return &s_recs[i];
    return NULL;
}

static SaveRec *rec_get(uint32_t ctx)
{
    SaveRec *r = rec_find(ctx);
    if (r) {
        if (r->valid) {
            rt_log("ctx: record for %08lX (marker %08lX, t%d) replaced by t%d", (unsigned long)ctx,
                   (unsigned long)r->marker, r->owner->id, s_cur->id);
#ifndef __3DS__
            { extern void plat_dump_stack(void); plat_dump_stack(); }
#endif
            r->valid = 0;
            r->owner->nsaved--;
        }
        return r;
    }
    for (int i = 0; i < MAX_RECS; i++)
        if (!s_recs[i].valid) {
            s_recs[i].ctx = ctx;
            return &s_recs[i];
        }
    rt_fatal("too many saved guest contexts");
}

static void ctx_store(PPCContext *c, uint32_t ctx, uint32_t srr0)
{
    for (int i = 0; i < 32; i++)
        st32(ctx + CTX_GPR + i * 4, c->r[i]);
    st32(ctx + CTX_CR, ppc_get_cr(c));
    st32(ctx + CTX_LR, c->lr);
    st32(ctx + CTX_CTR, c->ctr);
    st32(ctx + CTX_XER, ppc_get_xer(c));
    for (int i = 0; i < 32; i++) {
        st64(ctx + CTX_FPR + i * 8, c->f[i].u);
        fpr_t p;
        p.d = c->ps1[i];
        st64(ctx + CTX_PSF + i * 8, p.u);
    }
    st32(ctx + CTX_FPSCR, c->fpscr);
    st32(ctx + CTX_SRR0, srr0);
    st32(ctx + CTX_SRR1, c->msr);
    for (int i = 0; i < 8; i++)
        st32(ctx + CTX_GQR + i * 4, c->gqr[i]);
}

static void ctx_load(PPCContext *c, uint32_t ctx)
{
    for (int i = 0; i < 32; i++)
        c->r[i] = ld32(ctx + CTX_GPR + i * 4);
    ppc_set_cr(c, ld32(ctx + CTX_CR), 0xFF);
    c->lr = ld32(ctx + CTX_LR);
    c->ctr = ld32(ctx + CTX_CTR);
    ppc_set_xer(c, ld32(ctx + CTX_XER));
    for (int i = 0; i < 32; i++) {
        c->f[i].u = ld64(ctx + CTX_FPR + i * 8);
        fpr_t p;
        p.u = ld64(ctx + CTX_PSF + i * 8);
        c->ps1[i] = p.d;
    }
    c->fpscr = ld32(ctx + CTX_FPSCR);
    c->msr = ld32(ctx + CTX_SRR1);
    for (int i = 0; i < 8; i++)
        c->gqr[i] = ld32(ctx + CTX_GQR + i * 4);
}

static int s_debug_ctx = -1;
#define CTXLOG(...) do { if (s_debug_ctx < 0) s_debug_ctx = getenv("NSMBW_DEBUG_CTX") != NULL;                          if (s_debug_ctx) rt_log(__VA_ARGS__); } while (0)

jmp_buf *rt_save_context(PPCContext *c)
{
    uint32_t ctx = c->r[3];
    CTXLOG("[t%d] save ctx %08lX", s_cur->id, (unsigned long)ctx);
    SaveRec *r = rec_get(ctx);
    c->r[3] = 1;                  /* what the context returns when loaded */
    ctx_store(c, ctx, c->lr);
    c->r[3] = 0;                  /* what this call returns now */
    r->marker = c->lr;
    r->owner = s_cur;
    r->valid = 1;
    s_cur->nsaved++;
    return &r->jb;
}

/* ---------------------------------------------------------- host threads */

static void host_thread_main(void *arg);

static RT_NORETURN void wait_for_baton(HostThread *me)
{
    plat_sem_wait(me->wake);
    if (me->cmd == CMD_RESUME) {
        SaveRec *r = me->rec;
        me->cmd = CMD_NONE;
        ctx_load(&me->cpu, r->ctx);
        longjmp(r->jb, 1);
    }
    longjmp(me->top, 1);          /* CMD_START */
}

static RT_NORETURN void handoff(HostThread *t)
{
    HostThread *me = s_cur;
    s_cur = t;
    plat_sem_post(t->wake);
    wait_for_baton(me);
}

static HostThread *new_host_thread(int spawn)
{
    HostThread *t = calloc(1, sizeof *t);
    t->wake = plat_sem_create();
    t->id = s_nthreads++;
    t->next = s_threads;
    s_threads = t;
    if (spawn && plat_thread_start(host_thread_main, t, HOST_STACK))
        rt_fatal("cannot create host thread");
    return t;
}

static HostThread *idle_host_thread(void)
{
    for (HostThread *t = s_threads; t; t = t->next)
        if (t != s_cur && t->nsaved == 0 && t->cmd == CMD_NONE)
            return t;
    rt_log("spawning host thread %d", s_nthreads);
    return new_host_thread(1);
}

static RT_NORETURN void run_fresh(HostThread *me)
{
    PPCContext *c = &me->cpu;
    uint32_t ctx = me->start_ctx;
    me->cmd = CMD_NONE;
    ctx_load(c, ctx);
    uint32_t pc = ld32(ctx + CTX_SRR0);
    rt_log("[t%d] start context %08lX at %08lX (%s)", me->id, (unsigned long)ctx,
           (unsigned long)pc, symbol_name(pc, NULL));
    ppc_call_indirect(c, pc);
    /* the entry returned: continue at its return address (OSExitThread) */
    ppc_call_indirect(c, c->lr);
    rt_fatal("guest thread ran past its exit function");
}

static void host_thread_main(void *arg)
{
    HostThread *me = arg;
    me->stack_base = (uintptr_t)&me;
    if (setjmp(me->top))
        run_fresh(me);
    wait_for_baton(me);
}

static RT_NORETURN void switch_to_context(uint32_t ctx)
{
    uint32_t srr0 = ld32(ctx + CTX_SRR0);
    SaveRec *r = rec_find(ctx);
    CTXLOG("[t%d] load ctx %08lX srr0 %08lX rec %s", s_cur->id, (unsigned long)ctx, (unsigned long)srr0,
           r && r->valid ? (r->marker == srr0 ? "resume" : "stale") : "none");
    st16(ctx + CTX_STATE, (uint16_t)(ld16(ctx + CTX_STATE) & ~2));   /* OSLoadContext clears EXC */
    if (r && r->valid && r->marker == srr0) {
        HostThread *owner = r->owner;
        r->valid = 0;
        owner->nsaved--;
        if (owner == s_cur) {
            ctx_load(&owner->cpu, ctx);
            longjmp(r->jb, 1);
        }
        owner->cmd = CMD_RESUME;
        owner->rec = r;
        handoff(owner);
    }
    /* fresh context (OSCreateThread / OSInitContext set srr0 = entry) */
    if (!lookup_function(srr0)) {
        rt_log("ctx: [t%d] load %08lX srr0 %08lX: no resume record (rec %p valid %d marker %08lX)",
               s_cur->id, (unsigned long)ctx, (unsigned long)srr0, (void *)r, r ? r->valid : -1,
               r ? (unsigned long)r->marker : 0ul);
        for (int i = 0; i < MAX_RECS; i++)
            if (s_recs[i].valid)
                rt_log("  rec %d: ctx %08lX marker %08lX t%d", i, (unsigned long)s_recs[i].ctx,
                       (unsigned long)s_recs[i].marker, s_recs[i].owner->id);
        rt_debug_dump();
    }
    HostThread *t = s_cur->nsaved == 0 ? s_cur : idle_host_thread();
    t->cmd = CMD_START;
    t->start_ctx = ctx;
    if (t == s_cur)
        longjmp(t->top, 1);
    handoff(t);
}

void hle_OSLoadContext(PPCContext *c) { switch_to_context(c->r[3]); }

void hle_OSSaveContext_indirect(PPCContext *c)
{
    rt_fatal("OSSaveContext reached through an indirect call (lr=%08lX)", (unsigned long)c->lr);
}

/* ------------------------------------------------------------ interrupts */

static int s_in_irq;
static uint32_t s_resched_fn;

static void reschedule(PPCContext *c)
{
    if (!s_resched_fn)
        return;
    PPCContext saved = *c;
    ppc_call_indirect(c, s_resched_fn);
    *c = saved;
}

static void dispatch_exception(PPCContext *c, int exc)
{
    CTXLOG("[t%d] exception %d", s_cur->id, exc);
    uint32_t table = ld32(c->r[13] - 20536);   /* OSExceptionTable (see __OSSetExceptionHandler) */
    uint32_t handler = table ? ld32(table + exc * 4) : 0;
    if (!handler)
        return;
    uint32_t ctx = ld32(OS_CURRENTCONTEXT_PADDR) | 0x80000000u;
    PPCContext saved = *c;
    SaveRec *r = rec_get(ctx);
    ctx_store(c, ctx, MARK_IRQ);
    /* like the exception vector stub: the context now holds a full exception
     * state, so SelectThread switches away without OSSaveContext */
    st16(ctx + CTX_STATE, (uint16_t)(ld16(ctx + CTX_STATE) | 2));
    r->marker = MARK_IRQ;
    r->owner = s_cur;
    r->valid = 1;
    s_cur->nsaved++;
    s_in_irq++;
    if (!setjmp(r->jb)) {
        c->msr &= ~MSR_EE;
        c->r[3] = exc;
        c->r[4] = ctx;
        ppc_call_indirect(c, handler);
        if (r->valid) {           /* handler returned instead of OSLoadContext */
            r->valid = 0;
            s_cur->nsaved--;
        }
    }
    s_in_irq--;
    *c = saved;
}

/* Native "interrupt" callbacks (IOS async replies): run with the OS current
 * context pointing at a scratch exception context, like the SDK's handlers do,
 * so the scheduler only sets RunQueueHint instead of switching. */
typedef struct { uint32_t fn, a0, a1; } Callback;
static Callback s_cbq[64];
static int s_cbq_head, s_cbq_tail;

void rt_queue_callback(uint32_t fn, uint32_t a0, uint32_t a1)
{
    int next = (s_cbq_tail + 1) % 64;
    if (next == s_cbq_head)
        rt_fatal("callback queue full");
    s_cbq[s_cbq_tail] = (Callback){fn, a0, a1};
    s_cbq_tail = next;
    g_rt_pending = 1;
}

static void run_callbacks(PPCContext *c)
{
    while (s_cbq_head != s_cbq_tail) {
        Callback cb = s_cbq[s_cbq_head];
        s_cbq_head = (s_cbq_head + 1) % 64;
        PPCContext saved = *c;
        uint32_t cur = ld32(OS_CURRENTCONTEXT);
        memset(guest_ptr(RT_EXC_CONTEXT), 0, 0x2C8);
        st32(OS_CURRENTCONTEXT, RT_EXC_CONTEXT);
        st32(OS_CURRENTCONTEXT_PADDR, RT_EXC_CONTEXT & 0x3FFFFFFF);
        s_in_irq++;
        c->msr &= ~MSR_EE;
        c->r[1] -= 0x100;
        c->r[3] = cb.a0;
        c->r[4] = cb.a1;
        ppc_call_indirect(c, cb.fn);
        s_in_irq--;
        st32(OS_CURRENTCONTEXT, cur);
        st32(OS_CURRENTCONTEXT_PADDR, cur & 0x3FFFFFFF);
        *c = saved;
    }
}

/* host stack depth of guest threads: the recompiled code nests as deep as
 * the guest, and an overflow corrupts the heap silently on the 3DS */
static void note_stack_depth(void)
{
    char here;
    HostThread *t = s_cur;
    if (!t || !t->stack_base)
        return;
    uintptr_t depth = t->stack_base - (uintptr_t)&here;
    if (depth > t->stack_peak + 32 * 1024) {
        t->stack_peak = depth;
        rt_log("[stack] host thread %d at %lu KiB (sp %08lX)", t->id, (unsigned long)(depth >> 10), (unsigned long)(uintptr_t)&here);
    }
}

static void check_interrupts(PPCContext *c)
{
    note_stack_depth();
#ifdef RT_TRACE_ENABLED
    if (s_sample_report)
        sample_report();
#endif
    {
        /* idle: no current guest thread (the scheduler's idle loop) */
        static uint64_t last;
        static int was_idle;
        uint64_t now = plat_time_us();
        if (was_idle && last) {
            s_idle_us += now - last;
#ifdef __3DS__
            g_prof_us[PROF_IDLE] += now - last;
#endif
        }
        was_idle = ld32(0x800000E4) == 0;
        last = now;
    }
    if (!(c->msr & MSR_EE) || s_in_irq)
        return;
    g_rt_pending = 0;
    hw_update();
    int delivered = 0;
    if (s_dec_armed && (int64_t)(rt_ticks() - s_dec_deadline) >= 0) {
        s_dec_armed = 0;
        dispatch_exception(c, EXC_DECREMENTER);
        delivered = 1;
    }
    for (int n = 0; n < 8 && hw_external_pending(); n++) {
        dispatch_exception(c, EXC_EXTERNAL);
        delivered = 1;
    }
    if (s_cbq_head != s_cbq_tail) {
        run_callbacks(c);
        delivered = 1;
    }
    if (delivered)
        reschedule(c);
}

void rt_poll_slow(PPCContext *c) { check_interrupts(c); }

void hle_OSEnableInterrupts(PPCContext *c)
{
    uint32_t old = (c->msr >> 15) & 1;
    c->msr |= MSR_EE;
    c->r[3] = old;
    if (g_rt_pending)
        check_interrupts(c);
}

void hle_OSRestoreInterrupts(PPCContext *c)
{
    uint32_t old = (c->msr >> 15) & 1;
    if (c->r[3])
        c->msr |= MSR_EE;
    else
        c->msr &= ~MSR_EE;
    c->r[3] = old;
    if (g_rt_pending && (c->msr & MSR_EE))
        check_interrupts(c);
}

/* ------------------------------------------------------------ misc stubs */

void hle_nop(PPCContext *c) { (void)c; }

void hle_RealMode(PPCContext *c)
{
    /* RealMode(fn): runs fn with translation off; our memory map has no MMU */
    ppc_call_indirect(c, c->r[3] | 0x80000000u);
}

/* ------------------------------------------------------------------ boot */

void rt_os_boot(uint32_t entry)
{
    s_resched_fn = symbol_addr("__OSReschedule");
    if (!s_resched_fn)
        rt_log("warning: __OSReschedule unknown, no preemption after interrupts");
    plat_service_thread_start(timer_thread, NULL, 16 * 1024);

    HostThread *main = new_host_thread(0);
    s_cur = main;
    if (setjmp(main->top))
        run_fresh(main);
    PPCContext *c = &main->cpu;
    c->r[1] = 0x816FFFF0;
    c->msr = 0x00002032;      /* FP, IR, DR; interrupts off like at boot */
    ppc_call_indirect(c, entry);
    rt_fatal("__start returned");
}
