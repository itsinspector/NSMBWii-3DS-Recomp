/* IOS HLE at the IOS_* client API level (ipcclt.c).
 *
 * Synchronous calls return the result in r3. Async calls complete
 * immediately and queue the callback, which runs as if from the IPC
 * interrupt. Devices:
 *   /dev/di      disc reads from the ISO
 *   /dev/fs      ISFS directory/attribute ioctls on <data>/nand/
 *   /<path>      NAND files (SYSCONF, save data) on <data>/nand/<path>
 *   /dev/es      title ID / data dir
 *   /dev/stm/... power events (eventhook never fires)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "platform.h"

#ifdef _MSC_VER
#include <io.h>
#define unlink _unlink
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define IPC_OK        0
#define IPC_EACCES   -1
#define IPC_EEXIST   -2
#define IPC_EINVAL   -4
#define IPC_ENOENT   -6
#define FS_EEXIST  -105
#define FS_ENOENT  -106

#define TITLE_ID 0x00010000534D4E45ull   /* SMNE */

enum { DEV_NONE, DEV_DI, DEV_FS, DEV_ES, DEV_STM_IMM, DEV_STM_EVENT, DEV_FILE, DEV_DUMMY };

typedef struct {
    int type;
    FILE *f;
    char path[128];
    int mode;
} Fd;

#define MAX_FD 32
static Fd s_fds[MAX_FD];

static void nand_host_path(const char *nand, char *out, size_t n)
{
    snprintf(out, n, "%snand%s", plat_data_dir(), nand);
}

static void guest_cstr(uint32_t addr, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        out[i] = (char)ld8(addr + (uint32_t)i);
        if (!out[i])
            break;
    }
    out[i] = 0;
}

static void mkdirs_for(const char *host_path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", host_path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            plat_mkdir(tmp);
            *p = '/';
        }
    }
}

/* ------------------------------------------------------------------ open */

static int ios_open(const char *path, int mode)
{
    int fd;
    for (fd = 0; fd < MAX_FD && s_fds[fd].type != DEV_NONE; fd++)
        ;
    if (fd == MAX_FD)
        return -22;
    Fd *d = &s_fds[fd];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->mode = mode;

    if (!strcmp(path, "/dev/di")) d->type = DEV_DI;
    else if (!strcmp(path, "/dev/fs")) d->type = DEV_FS;
    else if (!strcmp(path, "/dev/es")) d->type = DEV_ES;
    else if (!strcmp(path, "/dev/stm/immediate")) d->type = DEV_STM_IMM;
    else if (!strcmp(path, "/dev/stm/eventhook")) d->type = DEV_STM_EVENT;
    else if (!strncmp(path, "/dev/", 5)) {
        rt_log("IOS_Open(%s): device not emulated", path);
        return IPC_ENOENT;
    } else {
        char host[512];
        nand_host_path(path, host, sizeof host);
        const char *m = mode == 1 ? "rb" : "r+b";
        d->f = fopen(host, m);
        if (!d->f && mode != 1) {
            d->f = fopen(host, "rb");   /* read-only fallback */
        }
        if (!d->f) {
            rt_log("IOS_Open(%s, %d): not found", path, mode);
            return FS_ENOENT;
        }
        d->type = DEV_FILE;
    }
    rt_log("IOS_Open(%s, %d) = %d", path, mode, fd);
    return fd;
}

static Fd *get_fd(int fd)
{
    if (fd < 0 || fd >= MAX_FD || s_fds[fd].type == DEV_NONE)
        return NULL;
    return &s_fds[fd];
}

static int ios_close(int fd)
{
    Fd *d = get_fd(fd);
    if (!d)
        return IPC_EINVAL;
    if (d->f)
        fclose(d->f);
    d->type = DEV_NONE;
    return 0;
}

/* transfers go through a fixed buffer in 64 KiB pieces: on the 3DS a
 * multi-MiB malloc can fail, and the heap is shared with the guest RAM */
static uint8_t s_xfer[0x10000];

static int ios_read(int fd, uint32_t buf, uint32_t len)
{
    Fd *d = get_fd(fd);
    if (!d || d->type != DEV_FILE)
        return IPC_EINVAL;
    int n = 0;
    while ((uint32_t)n < len) {
        uint32_t k = len - n < sizeof s_xfer ? len - n : sizeof s_xfer;
        int got = (int)fread(s_xfer, 1, k, d->f);
        guest_write(buf + n, s_xfer, got);
        n += got;
        if ((uint32_t)got < k)
            break;
    }
    return n;
}

static int ios_write(int fd, uint32_t buf, uint32_t len)
{
    Fd *d = get_fd(fd);
    if (!d || d->type != DEV_FILE)
        return IPC_EINVAL;
    int n = 0;
    while ((uint32_t)n < len) {
        uint32_t k = len - n < sizeof s_xfer ? len - n : sizeof s_xfer;
        guest_read(s_xfer, buf + n, k);
        int put = (int)fwrite(s_xfer, 1, k, d->f);
        n += put;
        if ((uint32_t)put < k)
            break;
    }
    fflush(d->f);
    return n;
}

static int ios_seek(int fd, int32_t off, int whence)
{
    Fd *d = get_fd(fd);
    if (!d || d->type != DEV_FILE)
        return IPC_EINVAL;
    if (fseek(d->f, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END))
        return -101;
    return (int)ftell(d->f);
}

/* ---------------------------------------------------------------- /dev/di */

static int di_ioctl(uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len)
{
    (void)in_len;
    switch (cmd) {
    case 0x70: {                    /* ReadDiskID */
        uint8_t id[0x20];
        iso_read(0, id, sizeof id);
        guest_write(out, id, out_len < 0x20 ? out_len : 0x20);
        return 1;
    }
    case 0x71: {                    /* Read: len, offset>>2 (partition relative) */
        uint32_t len = ld32(in + 4);
        uint64_t off = (uint64_t)ld32(in + 8) << 2;
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("NSMBW_DEBUG_DI") != NULL;
        if (dbg) {
            extern uint32_t hw_frame_count(void);
            rt_log("DI read %08llX len %lX (vsync %lu)", (unsigned long long)off, (unsigned long)len,
                   (unsigned long)hw_frame_count());
        }
        for (uint32_t n = 0; n < len; n += sizeof s_xfer) {
            uint32_t k = len - n < sizeof s_xfer ? len - n : sizeof s_xfer;
            iso_read(off + n, s_xfer, k);
            guest_write(out + n, s_xfer, k);
        }
        return 1;
    }
    case 0x8D: {                    /* UnencryptedRead: raw disc offset */
        uint32_t len = ld32(in + 4);
        uint64_t off = (uint64_t)ld32(in + 8) << 2;
        for (uint32_t n = 0; n < len; n += sizeof s_xfer) {
            uint32_t k = len - n < sizeof s_xfer ? len - n : sizeof s_xfer;
            iso_read_raw(off + n, s_xfer, k);
            guest_write(out + n, s_xfer, k);
        }
        return 1;
    }
    case 0x7A: case 0x95: case 0x96: case 0xE0:  /* cover/status/control/error regs */
        if (out && out_len >= 4)
            st32(out, 0);
        return 1;
    case 0x88:                      /* GetCoverStatus: disc present */
        if (out && out_len >= 4)
            st32(out, 2);
        return 1;
    case 0x12:                      /* Inquiry */
        if (out && out_len >= 8) {
            st32(out, 0x00000002);
            st32(out + 4, 0x20060526);
        }
        return 1;
    default:
        return 1;                   /* reset, seek, stop motor, spinup...: OK */
    }
}

static int di_ioctlv(uint32_t cmd, int nin, int nout, uint32_t vec)
{
    (void)nin; (void)nout; (void)vec;
    if (cmd == 0x8B || cmd == 0x8C) /* Open/ClosePartition */
        return 1;
    rt_log("DI ioctlv %02lX ignored", (unsigned long)cmd);
    return 1;
}

/* ---------------------------------------------------------------- /dev/fs */

static int fs_attr_path(uint32_t in, char *nand)
{
    guest_cstr(in + 6, nand, 64);
    return 0;
}

static int fs_ioctl(uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len)
{
    char nand[128], host[512];
    (void)in_len;
    switch (cmd) {
    case 2:                         /* GetStats */
        if (out_len >= 0x1C) {
            st32(out, 0x4000);      /* cluster size */
            st32(out + 4, 0x5000);  /* free clusters */
            st32(out + 8, 0);
            st32(out + 12, 0x2000);
            st32(out + 16, 0);
            st32(out + 20, 0x1000); /* free inodes */
            st32(out + 24, 0);
        }
        return 0;
    case 3:                         /* CreateDirectory */
        fs_attr_path(in, nand);
        nand_host_path(nand, host, sizeof host);
        mkdirs_for(host);
        return plat_mkdir(host) == 0 ? 0 : FS_EEXIST;
    case 5:                         /* SetAttr */
        return 0;
    case 6: {                       /* GetAttr: in path, out attr block */
        guest_cstr(in, nand, 64);
        nand_host_path(nand, host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (!f) {
#ifndef _MSC_VER
            DIR *dir = opendir(host);
            if (!dir)
                return FS_ENOENT;
            closedir(dir);
#else
            if (_access(host, 0))
                return FS_ENOENT;
#endif
        } else {
            fclose(f);
        }
        if (out && out_len >= 0x4C) {
            memset(guest_ptr(out), 0, 0x4C);
            st32(out, 0x1000);      /* owner */
            st16(out + 4, 0x0001);  /* group */
            guest_write(out + 6, nand, (uint32_t)strlen(nand) + 1);
            st8(out + 0x46, 3);
            st8(out + 0x47, 3);
            st8(out + 0x48, 3);
        }
        return 0;
    }
    case 7:                         /* Delete */
        guest_cstr(in, nand, 64);
        nand_host_path(nand, host, sizeof host);
        return unlink(host) == 0 ? 0 : FS_ENOENT;
    case 8: {                       /* Rename */
        char nand2[128], host2[512];
        guest_cstr(in, nand, 64);
        guest_cstr(in + 64, nand2, 64);
        nand_host_path(nand, host, sizeof host);
        nand_host_path(nand2, host2, sizeof host2);
        mkdirs_for(host2);
        unlink(host2);
        return rename(host, host2) == 0 ? 0 : FS_ENOENT;
    }
    case 9: {                       /* CreateFile */
        fs_attr_path(in, nand);
        nand_host_path(nand, host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (f) {
            fclose(f);
            return FS_EEXIST;
        }
        mkdirs_for(host);
        f = fopen(host, "wb");
        if (!f)
            return FS_ENOENT;
        fclose(f);
        return 0;
    }
    default:
        rt_log("FS ioctl %lu ignored", (unsigned long)cmd);
        return 0;
    }
}

static int fs_ioctlv(uint32_t cmd, int nin, int nout, uint32_t vec)
{
    (void)nin; (void)nout;
    if (cmd == 4) {                 /* ReadDirectory: report empty dirs */
        if (nout >= 1) {
            uint32_t cnt_out = ld32(vec + 8 * (nin + nout - 1));
            st32(cnt_out, 0);
        }
        return 0;
    }
    if (cmd == 12) {                /* GetUsage */
        if (nout >= 2) {
            st32(ld32(vec + 8 * nin), 0);
            st32(ld32(vec + 8 * (nin + 1)), 0);
        }
        return 0;
    }
    rt_log("FS ioctlv %lu ignored", (unsigned long)cmd);
    return 0;
}

static int file_ioctl(Fd *d, uint32_t cmd, uint32_t out, uint32_t out_len)
{
    if (cmd == 11 && out_len >= 8) {    /* GetFileStats: size, position */
        long pos = ftell(d->f);
        fseek(d->f, 0, SEEK_END);
        st32(out, (uint32_t)ftell(d->f));
        fseek(d->f, pos, SEEK_SET);
        st32(out + 4, (uint32_t)pos);
        return 0;
    }
    return IPC_EINVAL;
}

/* ---------------------------------------------------------------- /dev/es */

static int es_ioctlv(uint32_t cmd, int nin, int nout, uint32_t vec)
{
    uint32_t outv = vec + 8 * nin;
    switch (cmd) {
    case 0x20:                      /* GetTitleID */
        if (nout >= 1)
            st64(ld32(outv), TITLE_ID);
        return 0;
    case 0x1D: {                    /* GetDataDir */
        const char dir[] = "/title/00010000/534d4e45/data";
        if (nout >= 1)
            guest_write(ld32(outv), dir, sizeof dir);
        return 0;
    }
    default:
        rt_log("ES ioctlv %02lX ignored", (unsigned long)cmd);
        return 0;
    }
}

/* ------------------------------------------------------------- dispatch */

static int do_ioctl(int fd, uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len,
                    int *never_completes)
{
    Fd *d = get_fd(fd);
    if (!d)
        return IPC_EINVAL;
    switch (d->type) {
    case DEV_DI: return di_ioctl(cmd, in, in_len, out, out_len);
    case DEV_FS: return fs_ioctl(cmd, in, in_len, out, out_len);
    case DEV_FILE: return file_ioctl(d, cmd, out, out_len);
    case DEV_STM_EVENT:
        *never_completes = 1;       /* fires on power/reset button only */
        return 0;
    default:
        return 0;
    }
}

static int do_ioctlv(int fd, uint32_t cmd, int nin, int nout, uint32_t vec)
{
    Fd *d = get_fd(fd);
    if (!d)
        return IPC_EINVAL;
    switch (d->type) {
    case DEV_DI: return di_ioctlv(cmd, nin, nout, vec);
    case DEV_FS: return fs_ioctlv(cmd, nin, nout, vec);
    case DEV_ES: return es_ioctlv(cmd, nin, nout, vec);
    default: return 0;
    }
}

static void complete(uint32_t cb, uint32_t usr, int ret)
{
    if (cb)
        rt_queue_callback(cb, (uint32_t)ret, usr);
}

/* IOS_Open(path, mode) / IOS_OpenAsync(path, mode, cb, usr) */
void hle_IOS_Open(PPCContext *c)
{
    char path[128];
    guest_cstr(c->r[3], path, sizeof path);
    c->r[3] = (uint32_t)ios_open(path, (int)c->r[4]);
}

void hle_IOS_OpenAsync(PPCContext *c)
{
    char path[128];
    guest_cstr(c->r[3], path, sizeof path);
    complete(c->r[5], c->r[6], ios_open(path, (int)c->r[4]));
    c->r[3] = 0;
}

void hle_IOS_Close(PPCContext *c) { c->r[3] = (uint32_t)ios_close((int)c->r[3]); }

void hle_IOS_CloseAsync(PPCContext *c)
{
    complete(c->r[4], c->r[5], ios_close((int)c->r[3]));
    c->r[3] = 0;
}

void hle_IOS_Read(PPCContext *c) { c->r[3] = (uint32_t)ios_read((int)c->r[3], c->r[4], c->r[5]); }

void hle_IOS_ReadAsync(PPCContext *c)
{
    complete(c->r[6], c->r[7], ios_read((int)c->r[3], c->r[4], c->r[5]));
    c->r[3] = 0;
}

void hle_IOS_Write(PPCContext *c) { c->r[3] = (uint32_t)ios_write((int)c->r[3], c->r[4], c->r[5]); }

void hle_IOS_WriteAsync(PPCContext *c)
{
    complete(c->r[6], c->r[7], ios_write((int)c->r[3], c->r[4], c->r[5]));
    c->r[3] = 0;
}

void hle_IOS_Seek(PPCContext *c) { c->r[3] = (uint32_t)ios_seek((int)c->r[3], (int32_t)c->r[4], (int)c->r[5]); }

void hle_IOS_SeekAsync(PPCContext *c)
{
    complete(c->r[6], c->r[7], ios_seek((int)c->r[3], (int32_t)c->r[4], (int)c->r[5]));
    c->r[3] = 0;
}

/* IOS_Ioctl(fd, cmd, in, inLen, out, outLen) */
void hle_IOS_Ioctl(PPCContext *c)
{
    int never = 0;
    c->r[3] = (uint32_t)do_ioctl((int)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], &never);
}

/* IOS_IoctlAsync(fd, cmd, in, inLen, out, outLen, cb, usr) */
void hle_IOS_IoctlAsync(PPCContext *c)
{
    int never = 0;
    int ret = do_ioctl((int)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], &never);
    if (!never)
        complete(c->r[9], c->r[10], ret);
    c->r[3] = 0;
}

/* IOS_Ioctlv(fd, cmd, nIn, nOut, vec) */
void hle_IOS_Ioctlv(PPCContext *c)
{
    c->r[3] = (uint32_t)do_ioctlv((int)c->r[3], c->r[4], (int)c->r[5], (int)c->r[6], c->r[7]);
}

/* IOS_IoctlvAsync(fd, cmd, nIn, nOut, vec, cb, usr) */
void hle_IOS_IoctlvAsync(PPCContext *c)
{
    complete(c->r[8], c->r[9], do_ioctlv((int)c->r[3], c->r[4], (int)c->r[5], (int)c->r[6], c->r[7]));
    c->r[3] = 0;
}

void ios_init(void)
{
    char host[512];
    nand_host_path("/", host, sizeof host);
    plat_mkdir(host);
}
