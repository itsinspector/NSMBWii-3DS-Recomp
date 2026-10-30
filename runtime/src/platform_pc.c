/* Win32 debug build of the platform layer. */
#ifndef __3DS__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <mmsystem.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"

struct plat_sem { HANDLE h; };

plat_sem *plat_sem_create(void)
{
    plat_sem *s = malloc(sizeof *s);
    s->h = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    return s;
}

void plat_sem_wait(plat_sem *s) { WaitForSingleObject(s->h, INFINITE); }
void plat_sem_post(plat_sem *s) { ReleaseSemaphore(s->h, 1, NULL); }

typedef struct { void (*fn)(void *); void *arg; } start_t;

/* threads that may run guest code, for the watchdog's stack samples */
static HANDLE s_threads[64];
static void *s_thread_fn[64];       /* entry point, to name threads in profiles */
static uint32_t s_thread_hits[64];
static int s_nthreads;

/* NSMBW_FTZ=1: flush denormals like the 3DS VFP does (to compare behaviour) */
static void apply_ftz(void)
{
    if (getenv("NSMBW_FTZ"))
        _mm_setcsr(_mm_getcsr() | 0x8040);
}

static DWORD WINAPI thread_tramp(LPVOID p)
{
    start_t st = *(start_t *)p;
    free(p);
    apply_ftz();
    st.fn(st.arg);
    return 0;
}

int plat_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    start_t *st = malloc(sizeof *st);
    st->fn = fn;
    st->arg = arg;
    HANDLE h = CreateThread(NULL, stack_size, thread_tramp, st, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h)
        return -1;
    if (stack_size >= 64 * 1024 && s_nthreads < 64) {
        s_thread_fn[s_nthreads] = (void *)fn;
        s_threads[s_nthreads++] = h;
    }
    else
        CloseHandle(h);
    return 0;
}

/* ---- watchdog: NSMBW_WATCHDOG=<seconds> prints symbolized host stacks of
 * guest threads periodically (function names f_XXXXXXXX = guest addresses) */
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

static void dump_thread(HANDLE th, int idx)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(th) == (DWORD)-1)
        return;
    if (GetThreadContext(th, &ctx)) {
        STACKFRAME64 f;
        memset(&f, 0, sizeof f);
        f.AddrPC.Offset = ctx.Rip;
        f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp;
        f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp;
        f.AddrStack.Mode = AddrModeFlat;
        char line[1024];
        int len = snprintf(line, sizeof line, "  host thread %d:", idx);
        for (int depth = 0; depth < 14; depth++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), th, &f, &ctx, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), f.AddrPC.Offset, &disp, sym))
                len += snprintf(line + len, sizeof line - len, " %s", sym->Name);
            if (len > (int)sizeof line - 64)
                break;
        }
        plat_log_write(line);
    }
    ResumeThread(th);
}

static DWORD WINAPI watchdog(LPVOID p)
{
    DWORD secs = (DWORD)(uintptr_t)p;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    for (;;) {
        Sleep(secs * 1000);
        plat_log_write("[watchdog]");
        for (int i = 0; i < s_nthreads; i++)
            dump_thread(s_threads[i], i);
        extern void rt_debug_dump(void);
        if (getenv("NSMBW_WATCHDOG_TRACE"))
            rt_debug_dump();
    }
}

/* stack of the calling thread, symbolized (f_XXXXXXXX = guest functions) */
void plat_dump_stack(void)
{
    static int sym_init;
    if (!sym_init) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        sym_init = 1;
    }
    void *frames[48];
    USHORT n = CaptureStackBackTrace(1, 48, frames, NULL);
    char line[2048];
    int len = snprintf(line, sizeof line, "  stack:");
    for (USHORT i = 0; i < n && len < (int)sizeof line - 80; i++) {
        char buf[sizeof(SYMBOL_INFO) + 128];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 127;
        DWORD64 disp;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &disp, sym))
            len += snprintf(line + len, sizeof line - len, " %s", sym->Name);
    }
    plat_log_write(line);
}

/* ---- sampling profiler: NSMBW_PROFILE=<seconds> samples the running guest
 * thread's instruction pointer every ~1 ms and prints the hottest functions */
#define PROF_SLOTS 4096
static struct { DWORD64 addr; uint32_t hits; } s_prof[PROF_SLOTS];
static uint32_t s_prof_total;

static int s_prof_thread;

static void prof_sample(HANDLE th)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(th) == (DWORD)-1)
        return;
    BOOL ok = GetThreadContext(th, &ctx);
    ResumeThread(th);
    if (!ok)
        return;
    /* bucket by function start */
    char buf[sizeof(SYMBOL_INFO) + 64];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 63;
    DWORD64 disp = 0, key = ctx.Rip;
    if (SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, sym)) {
        if (!strncmp(sym->Name, "Nt", 2) || !strncmp(sym->Name, "Zw", 2))
            return;                      /* a blocked thread */
        key = sym->Address;
    }
    uint32_t h = (uint32_t)(key * 2654435761u) % PROF_SLOTS;
    for (int i = 0; i < PROF_SLOTS; i++, h = (h + 1) % PROF_SLOTS) {
        if (s_prof[h].addr == key || !s_prof[h].addr) {
            s_prof[h].addr = key;
            s_prof[h].hits++;
            s_prof_total++;
            s_thread_hits[s_prof_thread]++;
            return;
        }
    }
}

static int prof_cmp(const void *a, const void *b)
{
    return (int)((const uint32_t *)b)[2] - (int)((const uint32_t *)a)[2];
}

static void prof_report(void)
{
    static struct { DWORD64 addr; uint32_t hits; uint32_t pad; } sorted[PROF_SLOTS];
    int n = 0;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (s_prof[i].addr) {
            sorted[n].addr = s_prof[i].addr;
            sorted[n].hits = s_prof[i].hits;
            n++;
        }
    qsort(sorted, n, sizeof sorted[0], prof_cmp);
    char line[256];
    snprintf(line, sizeof line, "[profile] %lu samples", (unsigned long)s_prof_total);
    plat_log_write(line);
    for (int i = 0; i < n && i < 45; i++) {
        char buf[sizeof(SYMBOL_INFO) + 128];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 127;
        DWORD64 disp;
        const char *name = SymFromAddr(GetCurrentProcess(), sorted[i].addr, &disp, sym) ? sym->Name : "?";
        snprintf(line, sizeof line, "  %5.1f%%  %s", 100.0 * sorted[i].hits / s_prof_total, name);
        plat_log_write(line);
    }
    for (int t = 0; t < s_nthreads; t++) {
        if (!s_thread_hits[t])
            continue;
        char buf[sizeof(SYMBOL_INFO) + 128];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 127;
        DWORD64 disp;
        const char *name = s_thread_fn[t] && SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)s_thread_fn[t], &disp, sym)
                               ? sym->Name : "main";
        snprintf(line, sizeof line, "  thread %d (%s): %lu busy samples", t, name, (unsigned long)s_thread_hits[t]);
        plat_log_write(line);
    }
    memset(s_thread_hits, 0, sizeof s_thread_hits);
    memset(s_prof, 0, sizeof s_prof);
    s_prof_total = 0;
}

static DWORD WINAPI profiler(LPVOID p)
{
    DWORD secs = (DWORD)(uintptr_t)p;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    timeBeginPeriod(1);
    DWORD start = GetTickCount();
    for (;;) {
        Sleep(1);
        /* the guest thread that holds the baton is the one not blocked */
        /* NSMBW_PROFILE_THREAD=<n>: only the n-th registered thread (0 main, then
         * in creation order) */
        static int only = -2;
        if (only == -2)
            only = getenv("NSMBW_PROFILE_THREAD") ? atoi(getenv("NSMBW_PROFILE_THREAD")) : -1;
        for (int i = 0; i < s_nthreads; i++)
            if (only < 0 || i == only) {
                s_prof_thread = i;
                prof_sample(s_threads[i]);
            }
        if (GetTickCount() - start >= secs * 1000) {
            prof_report();
            start = GetTickCount();
        }
    }
}

int plat_service_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    return plat_thread_start(fn, arg, stack_size);
}

uint64_t plat_time_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000ull +
           (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000ull / freq.QuadPart;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sleep() follows the timer resolution, which Windows 11 may keep at 15.6 ms
 * for some processes; a high-resolution waitable timer sleeps ~1 ms reliably */
void plat_sleep_us(uint32_t us)
{
    static __declspec(thread) HANDLE timer;
    if (us < 500) {
        Sleep(0);
        return;
    }
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(us / 1000);
}

int plat_gx_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size)
{
    return plat_thread_start(fn, arg, stack_size);
}

uint32_t plat_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

static FILE *s_log;

void plat_log_write(const char *line)
{
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (!s_log) {
        char path[512];
        snprintf(path, sizeof path, "%slog.txt", plat_data_dir());
        s_log = fopen(path, "w");
    }
    if (s_log) {
        fputs(line, s_log);
        fputc('\n', s_log);
        fflush(s_log);
    }
}

static int has_iso(const char *dir)
{
    char path[600];
    snprintf(path, sizeof path, "%sgame.iso", dir);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* NSMBW_DATA, else ./pcdata, else a pcdata/ next to the exe or up to 3 levels
 * above it (double-clicking build/pc/nsmbw_pc.exe runs from build/pc) */
const char *plat_asset_dir(void) { return plat_data_dir(); }

const char *plat_data_dir(void)
{
    static char dir[512];
    if (dir[0])
        return dir;
    const char *env = getenv("NSMBW_DATA");
    if (env) {
        snprintf(dir, sizeof dir, "%s/", env);
        return dir;
    }
    snprintf(dir, sizeof dir, "pcdata/");
    if (has_iso(dir))
        return dir;
    char exe[512];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (n && n < sizeof exe) {
        for (int up = 0; up < 4; up++) {
            char *slash = strrchr(exe, 0x5C);   /* backslash */
            if (!slash)
                break;
            *slash = 0;
            snprintf(dir, sizeof dir, "%s/pcdata/", exe);
            if (has_iso(dir))
                return dir;
        }
    }
    snprintf(dir, sizeof dir, "pcdata/");
    return dir;
}

int plat_mkdir(const char *path) { return _mkdir(path); }

/* save data: NSMBW_SAVE, else %LOCALAPPDATA%/NSMBWii */
const char *plat_save_dir(void)
{
    static char dir[512];
    if (dir[0])
        return dir;
    const char *env = getenv("NSMBW_SAVE"), *local = getenv("LOCALAPPDATA");
    if (env)
        snprintf(dir, sizeof dir, "%s", env);
    else if (local)
        snprintf(dir, sizeof dir, "%s/NSMBWii", local);
    else
        snprintf(dir, sizeof dir, "%snand", plat_data_dir());
    for (char *p = dir; *p; p++)
        if (*p == 0x5C)              /* backslash */
            *p = '/';
    return dir;
}

int plat_frame(void)
{
    static int frames;
    const char *max = getenv("NSMBW_MAX_FRAMES");
    return !(max && ++frames >= atoi(max));
}

/* Keyboard: arrows, Z = jump, X = run, C = shake, Enter = +, Backspace = -.
 * NSMBW_INPUT="frame:mask,frame:mask,..." scripts input for headless runs. */
uint32_t plat_input(void)
{
    static const struct { int vk; uint32_t pad; } map[] = {
        {VK_UP, PAD_UP}, {VK_DOWN, PAD_DOWN}, {VK_LEFT, PAD_LEFT}, {VK_RIGHT, PAD_RIGHT},
        {'Z', PAD_JUMP}, {'X', PAD_RUN}, {'C', PAD_SHAKE}, {VK_RETURN, PAD_PLUS},
        {VK_BACK, PAD_MINUS}, {'A', PAD_A}, {'B', PAD_B},
    };
    uint32_t b = 0;
    HWND fg = GetForegroundWindow(), con = GetConsoleWindow();
    extern HWND g_game_window;
    if (fg && (fg == con || fg == g_game_window))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (GetAsyncKeyState(map[i].vk) & 0x8000)
                b |= map[i].pad;
    const char *script = getenv("NSMBW_INPUT");
    if (script) {
        extern uint32_t hw_frame_count(void);
        uint32_t now = hw_frame_count(), mask = 0;
        for (const char *p = script; *p;) {
            unsigned long f = strtoul(p, (char **)&p, 10);
            if (*p != ':')
                break;
            unsigned long m = strtoul(p + 1, (char **)&p, 0);
            if (f <= now)
                mask = (uint32_t)m;
            if (*p == ',')
                p++;
        }
        b |= mask;
    }
    return b;
}

/* ---- audio: waveOut, 32 kHz stereo s16; blocks of 1024 frames */
#define AUDIO_BLOCK 1024
#define AUDIO_NBUF 8
static HWAVEOUT s_wave;
static WAVEHDR s_hdr[AUDIO_NBUF];
static int16_t s_pcm[AUDIO_NBUF][AUDIO_BLOCK * 2];
static int s_fill_buf, s_fill_pos, s_audio_ok = -1;

void plat_audio_submit(const uint8_t *be, uint32_t n)
{
    if (s_audio_ok < 0) {
        s_audio_ok = 0;
        if (getenv("NSMBW_NOAUDIO"))
            return;
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof wf);
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = 32000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = 32000 * 4;
        if (waveOutOpen(&s_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            return;
        for (int i = 0; i < AUDIO_NBUF; i++) {
            s_hdr[i].lpData = (LPSTR)s_pcm[i];
            s_hdr[i].dwBufferLength = sizeof s_pcm[i];
            s_hdr[i].dwFlags = WHDR_DONE;
        }
        s_audio_ok = 1;
    }
    if (!s_audio_ok)
        return;
    for (uint32_t i = 0; i < n; i++) {
        WAVEHDR *h = &s_hdr[s_fill_buf];
        if (s_fill_pos == 0 && !(h->dwFlags & WHDR_DONE))
            return;                       /* all buffers queued: drop (we run ahead) */
        int16_t *d = &s_pcm[s_fill_buf][s_fill_pos * 2];
        d[0] = (int16_t)(be[i * 4] << 8 | be[i * 4 + 1]);
        d[1] = (int16_t)(be[i * 4 + 2] << 8 | be[i * 4 + 3]);
        if (++s_fill_pos == AUDIO_BLOCK) {
            if (h->dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(s_wave, h, sizeof *h);
            h->dwFlags = 0;
            waveOutPrepareHeader(s_wave, h, sizeof *h);
            waveOutWrite(s_wave, h, sizeof *h);
            s_fill_pos = 0;
            s_fill_buf = (s_fill_buf + 1) % AUDIO_NBUF;
        }
    }
}

/* ---- game window: shows every presented frame, closing it quits */
HWND g_game_window;
volatile int g_gl_active;          /* the OpenGL backend owns the window's surface */
static uint8_t *s_frame_bgra;
static int s_frame_w, s_frame_h;
static CRITICAL_SECTION s_frame_lock;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        if (g_gl_active) {
            ValidateRect(h, NULL);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        EnterCriticalSection(&s_frame_lock);
        if (s_frame_bgra) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = s_frame_w;
            bi.bmiHeader.biHeight = -s_frame_h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, s_frame_w, s_frame_h, s_frame_bgra, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        LeaveCriticalSection(&s_frame_lock);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        ExitProcess(0);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID p)
{
    /* p != NULL: hidden window (headless runs still need it for OpenGL) */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "nsmbw_pc";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 960, 684};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_game_window = CreateWindowA("nsmbw_pc", "NSMBW recomp (PC debug)  -  frecce, Z=2/salto, X=1/corsa, C=scuoti, A, Invio=+",
                                  WS_OVERLAPPEDWINDOW | (p ? 0 : WS_VISIBLE), CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

void plat_show_frame(const uint8_t *rgba, int w, int h)
{
    if (!g_game_window)
        return;
    EnterCriticalSection(&s_frame_lock);
    if (w != s_frame_w || h != s_frame_h) {
        free(s_frame_bgra);
        s_frame_bgra = malloc((size_t)w * h * 4);
        s_frame_w = w;
        s_frame_h = h;
    }
    for (int i = 0; i < w * h; i++) {
        s_frame_bgra[i * 4 + 0] = rgba[i * 4 + 2];
        s_frame_bgra[i * 4 + 1] = rgba[i * 4 + 1];
        s_frame_bgra[i * 4 + 2] = rgba[i * 4 + 0];
        s_frame_bgra[i * 4 + 3] = 255;
    }
    LeaveCriticalSection(&s_frame_lock);
    InvalidateRect(g_game_window, NULL, FALSE);
}

/* when started by double-click the console closes with the process: keep it
 * open so errors can be read */
static void pause_if_own_console(void)
{
    DWORD ids[4];
    if (GetConsoleProcessList(ids, 4) == 1) {
        puts("");
        fputs("Premi Invio per chiudere...", stdout);
        fflush(stdout);
        getchar();
    }
}

void rt_main(void);

int main(void)
{
    atexit(pause_if_own_console);
    timeBeginPeriod(1);              /* 1 ms Sleep() granularity */
    apply_ftz();
    InitializeCriticalSection(&s_frame_lock);
    CloseHandle(CreateThread(NULL, 0, window_thread, getenv("NSMBW_NOWINDOW") ? (LPVOID)1 : NULL, 0, NULL));
    for (int i = 0; i < 100 && !g_game_window; i++)
        Sleep(10);
    HANDLE self;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    s_threads[s_nthreads++] = self;
    const char *pr = getenv("NSMBW_PROFILE");
    if (pr && atoi(pr) > 0)
        CloseHandle(CreateThread(NULL, 0, profiler, (LPVOID)(uintptr_t)atoi(pr), 0, NULL));
    const char *wd = getenv("NSMBW_WATCHDOG");
    if (wd && atoi(wd) > 0)
        CloseHandle(CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(wd), 0, NULL));
    rt_main();
    return 0;
}
#endif
