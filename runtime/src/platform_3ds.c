/* 3DS platform layer (libctru). */
#ifdef __3DS__
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform.h"
#include "runtime.h"

/* Recompiled code nests host calls as deep as the guest call stack. */
u32 __stacksize__ = 1024 * 1024;

struct plat_sem { LightSemaphore s; };

plat_sem *plat_sem_create(void)
{
    plat_sem *s = malloc(sizeof *s);
    LightSemaphore_Init(&s->s, 0, 0x7FFF);
    return s;
}

void plat_sem_wait(plat_sem *s) { LightSemaphore_Acquire(&s->s, 1); }
void plat_sem_post(plat_sem *s) { LightSemaphore_Release(&s->s, 1); }

/* ---- crashes: every thread gets an exception handler that writes
 * sdmc:/3ds/nsmbw/crash.txt (ARM registers, guest state) and exits */
static char s_report[8192];
static size_t s_report_len;

static void report_line(const char *line)
{
    size_t n = strlen(line);
    if (s_report_len + n + 1 >= sizeof s_report)
        return;
    memcpy(s_report + s_report_len, line, n);
    s_report_len += n;
    s_report[s_report_len++] = '\n';
}

/* straight to the FS service: newlib's stdio lock may be held by the thread
 * that crashed or hung */
static void report_write(const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "/3ds/nsmbw/%s", name);
    Handle f;
    if (R_FAILED(FSUSER_OpenFileDirectly(&f, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    u32 written;
    FSFILE_SetSize(f, 0);
    FSFILE_Write(f, &written, 0, s_report, (u32)s_report_len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

static void crash_handler(ERRF_ExceptionInfo *e, CpuRegisters *r)
{
    static const char *const kind[] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
    char buf[160];
    s_report_len = 0;
    snprintf(buf, sizeof buf, "%s at pc %08lX, fault address %08lX (fsr %08lX)",
             e->type < 4 ? kind[e->type] : "exception", (unsigned long)r->pc, (unsigned long)e->far,
             (unsigned long)e->fsr);
    report_line(buf);
    for (int i = 0; i < 13; i += 4) {
        snprintf(buf, sizeof buf, "r%-2d %08lX %08lX %08lX %08lX", i, (unsigned long)r->r[i],
                 (unsigned long)(i + 1 < 13 ? r->r[i + 1] : 0), (unsigned long)(i + 2 < 13 ? r->r[i + 2] : 0),
                 (unsigned long)(i + 3 < 13 ? r->r[i + 3] : 0));
        report_line(buf);
    }
    snprintf(buf, sizeof buf, "sp %08lX lr %08lX cpsr %08lX  (host addresses: arm-none-eabi-addr2line -e nsmbw3ds.elf)",
             (unsigned long)r->sp, (unsigned long)r->lr, (unsigned long)r->cpsr);
    report_line(buf);
    snprintf(buf, sizeof buf, "graphics stage %lu", (unsigned long)g_gfx_stage);
    report_line(buf);
    rt_crash_report(report_line);
    report_write("crash.txt");
    svcExitProcess();
}

#define CRASH_STACK (16 * 1024)

static void catch_crashes(void)
{
    void *stack = malloc(CRASH_STACK);
    if (stack)
        threadOnException(crash_handler, (u8 *)stack + CRASH_STACK, WRITE_DATA_TO_HANDLER_STACK);
}

/* hangs: no presented frame for 30 s writes hang.txt once (graphics stage,
 * guest thread, last guest functions) */
volatile uint32_t g_gfx_stage, g_progress;

static void watchdog(void *arg)
{
    (void)arg;
    uint32_t last = g_progress, still = 0;
    for (;;) {
        svcSleepThread(5000000000LL);
        if (g_progress != last) {
            last = g_progress;
            still = 0;
            continue;
        }
        if (++still != 6)
            continue;
        char buf[96];
        s_report_len = 0;
        snprintf(buf, sizeof buf, "no frame for 30 s: graphics stage %lu, frames %lu", (unsigned long)g_gfx_stage,
                 (unsigned long)g_progress);
        report_line(buf);
        rt_crash_report(report_line);
        report_write("hang.txt");
    }
}

typedef struct { void (*fn)(void *); void *arg; } ThreadStart;

static void thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    catch_crashes();
    s.fn(s.arg);
}

static Thread create_thread(void (*fn)(void *), void *arg, uint32_t stack_size, s32 prio, int core)
{
    ThreadStart *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    s->fn = fn;
    s->arg = arg;
    Thread t = threadCreate(thread_entry, s, stack_size, prio, core, true);
    if (!t)
        free(s);
    return t;
}

int plat_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* same core as the main thread: guest threads never run concurrently */
    Thread t = create_thread(fn, arg, stack_size, prio, -2);
    return t ? 0 : -1;
}

int plat_service_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = create_thread(fn, arg, stack_size, prio > 0x19 ? prio - 1 : prio, -2);
    return t ? 0 : -1;
}

uint64_t plat_time_us(void)
{
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000);
}

void plat_sleep_us(uint32_t us) { svcSleepThread((s64)us * 1000); }

/* core 2 belongs to the application on the New 3DS (New 3DS mode, see the
 * rsf); the Old 3DS only shares core 1 with the system */
static int is_new3ds(void)
{
    bool n = false;
    APT_CheckNew3DS(&n);
    return n;
}

uint32_t plat_cpu_count(void) { return is_new3ds() ? 2 : 1; }

int plat_gx_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = create_thread(fn, arg, stack_size, prio, is_new3ds() ? 2 : -2);
    rt_log("gx: graphics thread on core %d", t && is_new3ds() ? 2 : 0);
    return t ? 0 : -1;
}

static FILE *s_log;

void plat_log_write(const char *line)
{
    printf("%s\n", line);
    if (!s_log)
        s_log = fopen("sdmc:/3ds/nsmbw/log.txt", "w");
    if (s_log) {
        fputs(line, s_log);
        fputc('\n', s_log);
        fflush(s_log);
    }
}

const char *plat_data_dir(void) { return "sdmc:/3ds/nsmbw/"; }

/* the "full" CIA carries the disc image and REL image in its RomFS; the
 * plain one reads them from the SD card */
static int s_romfs;

const char *plat_asset_dir(void)
{
    if (s_romfs) {
        FILE *f = fopen("romfs:/game.iso", "rb");
        if (f) {
            fclose(f);
            return "romfs:/";
        }
    }
    return plat_data_dir();
}

int plat_mkdir(const char *path) { return mkdir(path, 0777); }

/* save data on the SD card, next to the game files: it survives reinstalling
 * the CIA and is easy to back up */
const char *plat_save_dir(void) { return "sdmc:/3ds/nsmbw/save"; }

int plat_frame(void)
{
    if (!aptMainLoop())
        return 0;
    hidScanInput();
    if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT))
        return 0;
    return 1;
}

uint32_t plat_input(void)
{
    u32 k = hidKeysHeld();
    uint32_t b = 0;
    if (k & (KEY_DUP | KEY_CPAD_UP)) b |= PAD_UP;
    if (k & (KEY_DDOWN | KEY_CPAD_DOWN)) b |= PAD_DOWN;
    if (k & (KEY_DLEFT | KEY_CPAD_LEFT)) b |= PAD_LEFT;
    if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) b |= PAD_RIGHT;
    if (k & (KEY_B | KEY_A)) b |= PAD_JUMP;
    if (k & KEY_Y) b |= PAD_RUN;
    if (k & (KEY_X | KEY_L | KEY_R)) b |= PAD_SHAKE;
    if (k & KEY_START) b |= PAD_PLUS;
    if (k & KEY_SELECT) b |= PAD_MINUS;
    return b;
}

/* ---- audio: ndsp channel 0, 32 kHz stereo PCM16 (needs sdmc:/3ds/dspfirm.cdc) */
#define AUDIO_BLOCK 1024
#define AUDIO_NBUF 6
static ndspWaveBuf s_wbuf[AUDIO_NBUF];
static int16_t *s_pcm;
static int s_fill_buf, s_fill_pos, s_audio_ok = -1;

void plat_audio_submit(const uint8_t *be, uint32_t n)
{
    if (s_audio_ok < 0) {
        s_audio_ok = 0;
        if (R_FAILED(ndspInit()))
            return;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, 32000.0f);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        s_pcm = linearAlloc(AUDIO_NBUF * AUDIO_BLOCK * 4);
        memset(s_wbuf, 0, sizeof s_wbuf);
        for (int i = 0; i < AUDIO_NBUF; i++) {
            s_wbuf[i].data_vaddr = &s_pcm[i * AUDIO_BLOCK * 2];
            s_wbuf[i].nsamples = AUDIO_BLOCK;
            s_wbuf[i].status = NDSP_WBUF_DONE;
        }
        s_audio_ok = 1;
    }
    if (!s_audio_ok)
        return;
    for (uint32_t i = 0; i < n; i++) {
        ndspWaveBuf *w = &s_wbuf[s_fill_buf];
        if (s_fill_pos == 0 && w->status != NDSP_WBUF_DONE && w->status != NDSP_WBUF_FREE)
            return;
        int16_t *d = &s_pcm[(s_fill_buf * AUDIO_BLOCK + s_fill_pos) * 2];
        d[0] = (int16_t)(be[i * 4] << 8 | be[i * 4 + 1]);
        d[1] = (int16_t)(be[i * 4 + 2] << 8 | be[i * 4 + 3]);
        if (++s_fill_pos == AUDIO_BLOCK) {
            DSP_FlushDataCache(w->data_vaddr, AUDIO_BLOCK * 4);
            ndspChnWaveBufAdd(0, w);
            s_fill_pos = 0;
            s_fill_buf = (s_fill_buf + 1) % AUDIO_NBUF;
        }
    }
}

void plat_show_frame(const uint8_t *rgba, int w, int h) { (void)rgba; (void)w; (void)h; }

void rt_main(void);

int main(void)
{
    osSetSpeedupEnable(true);
    catch_crashes();
    threadCreate(watchdog, NULL, 16 * 1024, 0x18, -2, true);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    s_romfs = R_SUCCEEDED(romfsInit());
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/nsmbw", 0777);
    rt_main();
    gfxExit();
    return 0;
}
#endif
