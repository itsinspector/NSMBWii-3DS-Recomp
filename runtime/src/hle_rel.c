/* Dynamic linking HLE. The game loads each REL from disc and calls OSLink on
 * the copy; our RELs are already relocated and recompiled at fixed addresses,
 * so OSLink only rewrites the module header the way the real linker would,
 * but pointing at those addresses. The loaded copy is left unused. */
#include "runtime.h"
#include "rel_table.h"

#define OS_MODULE_LIST 0x800030C8u   /* head, tail */

static const rel_info_t *find_rel(uint32_t id)
{
    for (uint32_t i = 0; i < g_rel_count; i++)
        if (g_rel_table[i].id == id)
            return &g_rel_table[i];
    return NULL;
}

static int link_module(uint32_t info, uint32_t bss)
{
    (void)bss;
    uint32_t id = ld32(info);
    const rel_info_t *rel = find_rel(id);
    if (!rel) {
        rt_log("OSLink: unknown module id %lu", (unsigned long)id);
        return 0;
    }
    uint32_t nsec = ld32(info + 0x0C);
    uint32_t sec_off = ld32(info + 0x10);
    if (sec_off < 0x80000000u)            /* still an offset: first link of this copy */
        sec_off += info;
    if (nsec > rel->nsec)
        nsec = rel->nsec;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t si = sec_off + i * 8;
        if (rel->sec_addr[i])
            st32(si, rel->sec_addr[i] | rel->sec_exec[i]);
    }
    st32(info + 0x10, sec_off);
    uint32_t name = ld32(info + 0x14);
    if (name && name < 0x80000000u)
        st32(info + 0x14, name + info);
    st32(info + 0x34, rel->prolog);
    st32(info + 0x38, rel->epilog);
    st32(info + 0x3C, rel->unresolved);

    /* append to the OS module list */
    uint32_t tail = ld32(OS_MODULE_LIST + 4);
    st32(info + 4, 0);
    st32(info + 8, tail);
    if (tail)
        st32(tail + 4, info);
    else
        st32(OS_MODULE_LIST, info);
    st32(OS_MODULE_LIST + 4, info);
    rt_log("OSLink: %s (id %lu) -> prolog %08lX", rel->name, (unsigned long)id, (unsigned long)rel->prolog);
    return 1;
}

/* OSLink(OSModuleInfo *info, void *bss) / OSLinkFixed(...) */
void hle_OSLink(PPCContext *c) { c->r[3] = (uint32_t)link_module(c->r[3], c->r[4]); }

/* OSUnlink(OSModuleInfo *info): unlink from the list, modules stay resident */
void hle_OSUnlink(PPCContext *c)
{
    uint32_t info = c->r[3];
    uint32_t next = ld32(info + 4), prev = ld32(info + 8);
    if (next) st32(next + 8, prev); else st32(OS_MODULE_LIST + 4, prev);
    if (prev) st32(prev + 4, next); else st32(OS_MODULE_LIST, next);
    c->r[3] = 1;
}
