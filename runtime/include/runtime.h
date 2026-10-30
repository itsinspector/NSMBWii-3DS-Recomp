#pragma once
#include "ppc_context.h"

#define REL_BASE 0x93400000u
/* 128 KiB below the RELs, hidden from the game's MEM2 arena, for runtime use
 * (exception context, IOS request buffers). */
#define RT_SCRATCH_BASE (REL_BASE - 0x20000u)
#define RT_EXC_CONTEXT  (RT_SCRATCH_BASE + 0x0000u)   /* OSContext, 0x2C8 bytes */

#define MSR_EE 0x8000u

extern uint8_t *g_mem1, *g_mem2;

#if defined(_MSC_VER)
#define RT_NORETURN __declspec(noreturn)
#define RT_PRINTF(a, b)
#else
#define RT_NORETURN __attribute__((noreturn))
#define RT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#endif

/* memory.c */
int mem_init(void);
void lowmem_init(const uint8_t *disc_header);
void *guest_ptr(uint32_t addr);
void guest_write(uint32_t addr, const void *src, uint32_t len);
void guest_read(void *dst, uint32_t addr, uint32_t len);

/* iso.c: unencrypted Wii disc image, game partition */
int iso_open(const char *path);
int iso_read(uint64_t part_offset, void *dst, uint32_t len);   /* partition-relative */
int iso_read_raw(uint64_t disc_offset, void *dst, uint32_t len);
uint64_t iso_partition_offset(void);

/* loader.c */
int load_dol_from_iso(uint32_t *entry);
int load_fst_from_iso(void);
int load_rel_image(const char *path);

/* cpu.c */
ppc_func_t lookup_function(uint32_t addr);
const char *symbol_name(uint32_t addr, uint32_t *offset);   /* nearest symbol <= addr */
uint32_t symbol_addr(const char *name);                      /* 0 if unknown */

/* hle_os.c */
uint64_t rt_ticks(void);           /* Wii timebase (60.75 MHz) */
void rt_os_boot(uint32_t entry);   /* runs __start on the main host thread */
void rt_dec_write(uint32_t v);
uint32_t rt_dec_read(void);
void rt_queue_callback(uint32_t fn, uint32_t arg0, uint32_t arg1);  /* "interrupt context" call */
/* Guest-visible time (VI, decrementer, AI/DSP). It follows real time but
 * slows down when the host cannot keep up, as on a slower console. */
uint64_t rt_time_us(void);
uint64_t rt_guest_to_real_us(uint64_t guest);
void rt_time_adapt(void);
/* coarse time accounting per subsystem (3DS builds; logged every 120 frames) */
enum { PROF_TL, PROF_SUBMIT, PROF_TEX, PROF_AUDIO, PROF_GPUWAIT, PROF_IDLE, PROF_SLOTS };
#ifdef __3DS__
uint64_t plat_time_us(void);
extern uint64_t g_prof_us[PROF_SLOTS];
#define PROF_BEGIN(name) uint64_t prof_t0_##name = plat_time_us()
#define PROF_END(name, slot) (g_prof_us[slot] += plat_time_us() - prof_t0_##name)
/* per-primitive sections: the clock is an SVC, so time one primitive in 8
 * (g_prof_tick, advanced per primitive) and scale */
extern uint32_t g_prof_tick;
#define PROF_SBEGIN(name) uint64_t prof_t0_##name = (g_prof_tick & 7) ? 0 : plat_time_us()
#define PROF_SEND(name, slot) \
    (prof_t0_##name ? (void)(g_prof_us[slot] += (plat_time_us() - prof_t0_##name) * 8) : (void)0)
#else
#define PROF_BEGIN(name) ((void)0)
#define PROF_END(name, slot) ((void)0)
#define PROF_SBEGIN(name) ((void)0)
#define PROF_SEND(name, slot) ((void)0)
#endif
void rt_prof_frame(void);
void rt_crash_report(void (*out)(const char *line));
/* 3DS hang/crash reports: where the graphics backend is, and frames shown */
extern volatile uint32_t g_gfx_stage, g_progress;
void rt_raise(void);               /* something became deliverable */

/* hw.c: hardware registers */
void hw_init(void);
void hw_update(void);              /* time-driven devices (VI retrace) */
int hw_external_pending(void);     /* PI cause & mask */
void gx_fifo_write8(uint8_t v);
void gx_fifo_write16(uint16_t v);
void gx_fifo_write32(uint32_t v);

/* hle_ios.c */
void ios_init(void);

void rt_log(const char *fmt, ...) RT_PRINTF(1, 2);
RT_NORETURN void rt_fatal(const char *fmt, ...) RT_PRINTF(1, 2);
