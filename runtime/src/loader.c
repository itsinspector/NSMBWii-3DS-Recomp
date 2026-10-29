/* Loads main.dol from the disc image and the REL image produced by the recompiler. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int load_dol_from_iso(uint32_t *entry)
{
    uint8_t hdr[0x440], dh[0x100];
    iso_read(0, hdr, sizeof hdr);
    uint64_t dol = (uint64_t)be32(hdr + 0x420) << 2;
    iso_read(dol, dh, sizeof dh);
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(dh + i * 4), addr = be32(dh + 0x48 + i * 4), len = be32(dh + 0x90 + i * 4);
        if (!len)
            continue;
        uint8_t *buf = malloc(len);
        if (!buf)
            return -1;
        iso_read(dol + off, buf, len);
        guest_write(addr, buf, len);
        free(buf);
    }
    /* bss: __init_data clears it too, but MEM1 starts zeroed anyway */
    *entry = be32(dh + 0xE0);
    return 0;
}

/* What the apploader does besides loading the DOL: copy the FST and bi2.bin to
 * the top of MEM1 and publish them in low memory. Returns the new MEM1 top. */
int load_fst_from_iso(void)
{
    uint8_t hdr[0x440];
    iso_read(0, hdr, sizeof hdr);
    uint64_t fst_off = (uint64_t)be32(hdr + 0x424) << 2;
    uint32_t fst_size = be32(hdr + 0x428) << 2;
    uint32_t fst_max = be32(hdr + 0x42C) << 2;
    if (fst_max < fst_size)
        fst_max = fst_size;

    uint32_t top = 0x8179D500u;                       /* MEM1 arena hi left by the system menu */
    uint32_t fst_addr = (top - fst_max) & ~31u;
    uint32_t bi2_addr = (fst_addr - 0x2000) & ~31u;

    uint8_t *buf = malloc(fst_size > 0x2000 ? fst_size : 0x2000);
    if (!buf)
        return -1;
    iso_read(fst_off, buf, fst_size);
    guest_write(fst_addr, buf, fst_size);
    iso_read(0x440, buf, 0x2000);
    guest_write(bi2_addr, buf, 0x2000);
    free(buf);

    st32(0x80000034, bi2_addr);                       /* arena hi */
    st32(0x80000038, fst_addr);                       /* FST location */
    st32(0x8000003C, fst_max);                        /* FST max length */
    st32(0x800000F4, bi2_addr);                       /* bi2 */
    st32(0x80003110, bi2_addr);                       /* MEM1 arena hi */
    rt_log("FST at %08lX (%lu bytes)", (unsigned long)fst_addr, (unsigned long)fst_size);
    return 0;
}

int load_rel_image(const char *path)
{
    uint8_t h[8];
    FILE *f = fopen(path, "rb");
    if (!f || fread(h, 1, 8, f) != 8) {
        rt_log("cannot read %s", path);
        if (f)
            fclose(f);
        return -1;
    }
    uint32_t base = be32(h), len = be32(h + 4);
    if (base != REL_BASE) {
        rt_log("%s: bad header (rebuild with the same recompiler version)", path);
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc(len);
    if (!buf || fread(buf, 1, len, f) != len) {
        rt_log("%s: truncated", path);
        free(buf);
        fclose(f);
        return -1;
    }
    guest_write(base, buf, len);
    free(buf);
    fclose(f);
    return 0;
}
