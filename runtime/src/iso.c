/* Reads the user's unencrypted Wii disc image (same format extract_disc.py
 * accepts). All disc access in the game goes through /dev/di, which ends here. */
#include <stdio.h>
#include "runtime.h"

static FILE *s_iso;
static uint64_t s_part_data;   /* absolute offset of the game partition's data */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int seek64(FILE *f, uint64_t off)
{
#ifdef _MSC_VER
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

int iso_read_raw(uint64_t off, void *dst, uint32_t len)
{
    if (!s_iso || seek64(s_iso, off))
        return -1;
    size_t got = fread(dst, 1, len, s_iso);
    if (got < len)   /* trimmed image: unused tail reads as zeros */
        memset((uint8_t *)dst + got, 0, len - got);
    return 0;
}

int iso_read(uint64_t off, void *dst, uint32_t len) { return iso_read_raw(s_part_data + off, dst, len); }

uint64_t iso_partition_offset(void) { return s_part_data; }

int iso_open(const char *path)
{
    uint8_t hdr[0x62], b[8];
    s_iso = fopen(path, "rb");
    if (!s_iso)
        return -1;
    iso_read_raw(0, hdr, sizeof hdr);
    if (be32(hdr + 0x18) != 0x5D1C9EA3) {
        rt_log("%s: not a Wii disc image", path);
        return -1;
    }
    if (!hdr[0x60] || !hdr[0x61]) {
        rt_log("%s: encrypted image; convert it to an unencrypted ISO first", path);
        return -1;
    }
    for (int group = 0; group < 4; group++) {
        iso_read_raw(0x40000 + group * 8, b, 8);
        uint32_t count = be32(b), table = be32(b + 4) << 2;
        for (uint32_t i = 0; i < count; i++) {
            iso_read_raw(table + i * 8ull, b, 8);
            if (be32(b + 4) != 0)
                continue;
            uint64_t part = (uint64_t)be32(b) << 2;
            iso_read_raw(part + 0x2B8, b, 4);
            s_part_data = part + ((uint64_t)be32(b) << 2);
            return 0;
        }
    }
    rt_log("%s: no game partition", path);
    return -1;
}
