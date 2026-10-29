/* Guest physical memory (MEM1 24 MiB + MEM2 64 MiB) and the address map. */
#include <stdlib.h>
#include "runtime.h"

uint8_t *g_memmap[65536];
uint8_t *g_mem1, *g_mem2;
static uint8_t *s_scratch;     /* 64 KiB (+ slack for accesses straddling the end) */
static uint8_t *s_locked_cache;

#define MEM1_SIZE 0x01800000u
#define MEM2_SIZE 0x04000000u

static void map(uint32_t guest, uint8_t *host, uint32_t size)
{
    for (uint32_t off = 0; off < size; off += 0x10000)
        g_memmap[(guest + off) >> 16] = host + off;
}

int mem_init(void)
{
    /* +16 so an 8-byte access at the last bytes of a region stays in bounds */
    g_mem1 = calloc(1, MEM1_SIZE + 16);
    g_mem2 = calloc(1, MEM2_SIZE + 16);
    s_scratch = calloc(1, 0x10000 + 16);
    s_locked_cache = calloc(1, 0x10000 + 16);
    if (!g_mem1 || !g_mem2 || !s_scratch || !s_locked_cache) {
        rt_log("mem_init: out of host memory (need ~90 MiB, New 3DS required)");
        return -1;
    }
    for (int i = 0; i < 65536; i++)
        g_memmap[i] = s_scratch;
    /* physical, cached (0x8/0x9) and uncached (0xC/0xD) views */
    map(0x00000000, g_mem1, MEM1_SIZE);
    map(0x80000000, g_mem1, MEM1_SIZE);
    map(0xC0000000, g_mem1, MEM1_SIZE);
    map(0x10000000, g_mem2, MEM2_SIZE);
    map(0x90000000, g_mem2, MEM2_SIZE);
    map(0xD0000000, g_mem2, MEM2_SIZE);
    /* locked L1 cache (16 KiB at 0xE0000000) */
    map(0xE0000000, s_locked_cache, 0x10000);
    return 0;
}

void *guest_ptr(uint32_t addr) { return host_ptr(addr); }

void guest_write(uint32_t addr, const void *src, uint32_t len)
{
    /* chunked: MEM1/MEM2 are contiguous on the host, but be safe at region ends */
    const uint8_t *s = src;
    while (len) {
        uint32_t n = 0x10000 - (addr & 0xFFFF);
        if (n > len)
            n = len;
        memcpy(host_ptr(addr), s, n);
        addr += n;
        s += n;
        len -= n;
    }
}

void guest_read(void *dst, uint32_t addr, uint32_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint32_t n = 0x10000 - (addr & 0xFFFF);
        if (n > len)
            n = len;
        memcpy(d, host_ptr(addr), n);
        addr += n;
        d += n;
        len -= n;
    }
}

void lowmem_init(const uint8_t *disc_header)
{
    guest_write(0x80000000, disc_header, 0x20);
    st32(0x80000020, 0x0D15EA5E);        /* boot magic */
    st32(0x80000024, 1);
    st32(0x80000028, MEM1_SIZE);
    st32(0x8000002C, 0x00000023);        /* console type: retail Wii */
    st32(0x80000030, 0);                 /* arena lo: let OSInit compute it */
    st32(0x80000034, 0x817FEC60);        /* arena hi */
    st32(0x800000F0, MEM1_SIZE);
    st32(0x800000F8, 243000000);         /* bus clock */
    st32(0x800000FC, 729000000);         /* CPU clock */
    st32(0x800030D8, 0);                 /* system time base */
    st32(0x80003100, MEM1_SIZE);
    st32(0x80003104, MEM1_SIZE);
    st32(0x8000310C, 0);                 /* MEM1 arena lo: 0 = use the linker's __ArenaLo */
    st32(0x80003110, 0x8179D500);        /* MEM1 arena hi (system menu value) */
    st32(0x80003118, MEM2_SIZE);
    st32(0x8000311C, MEM2_SIZE);
    st32(0x80003120, REL_BASE);          /* end of MEM2 visible to the game */
    st32(0x80003124, 0x90000800);        /* usable MEM2 start */
    st32(0x80003128, RT_SCRATCH_BASE);   /* usable MEM2 end: runtime + RELs above */
    st32(0x80003130, RT_SCRATCH_BASE);   /* IOS heap */
    st32(0x80003134, REL_BASE);
    st32(0x80003138, 0x00000011);        /* Hollywood revision */
    st32(0x80003140, 0x00380000);        /* IOS56 */
    st32(0x80003144, 0x00030310);        /* IOS build date */
    st32(0x80003158, 0x0000FF16);        /* GDDR vendor code */
    st32(0x80003188, 0x00380000);
}
