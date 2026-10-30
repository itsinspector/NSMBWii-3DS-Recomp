/* Host abstraction: the same runtime builds for 3DS (libctru) and for a PC
 * debug build (Win32). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct plat_sem plat_sem;

plat_sem *plat_sem_create(void);
void plat_sem_wait(plat_sem *s);
void plat_sem_post(plat_sem *s);

/* Starts a host thread running fn(arg). */
int plat_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size);
/* a helper that sleeps most of the time but must preempt guest threads when it
 * wakes (the 3DS does not time-slice threads of equal priority) */
int plat_service_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size);
/* the graphics pipeline (GX command processor + renderer): on a core of its
 * own where there is one, so it runs alongside the game */
int plat_gx_thread_start(void (*fn)(void *), void *arg, uint32_t stack_size);

uint64_t plat_time_us(void);         /* monotonic microseconds */
void plat_sleep_us(uint32_t us);
uint32_t plat_cpu_count(void);

void plat_log_write(const char *line); /* one line, no newline */
const char *plat_data_dir(void);       /* writable data: log, frame dumps */
const char *plat_save_dir(void);       /* the emulated NAND (save data), no trailing slash */
const char *plat_asset_dir(void);      /* read-only game files: game.iso, rel_image.bin */
int plat_mkdir(const char *path);

/* Called once per emulated vblank from the guest-visible side; returns 0 when
 * the user asked to quit. */
int plat_frame(void);

/* Abstract buttons, mapped to the sideways Wii Remote by hle_input.c */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_JUMP = 1 << 4, PAD_RUN = 1 << 5, PAD_SHAKE = 1 << 6,
    PAD_PLUS = 1 << 7, PAD_MINUS = 1 << 8, PAD_A = 1 << 9, PAD_B = 1 << 10,
};
uint32_t plat_input(void);

/* Audio out: n stereo frames of big-endian s16 at 32 kHz */
void plat_audio_submit(const uint8_t *be_samples, uint32_t n);

/* PC: show a presented frame (RGBA) in the game window */
void plat_show_frame(const uint8_t *rgba, int w, int h);
