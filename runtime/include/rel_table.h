/* REL modules as placed by the recompiler (generated: build/gen/ppc_rels.c) */
#pragma once
#include <stdint.h>

#define REL_MAX_SECTIONS 24

typedef struct {
    uint32_t id;
    const char *name;
    uint32_t nsec;
    uint32_t prolog, epilog, unresolved;
    uint32_t sec_addr[REL_MAX_SECTIONS];
    uint32_t sec_size[REL_MAX_SECTIONS];
    uint8_t sec_exec[REL_MAX_SECTIONS];
} rel_info_t;

extern const rel_info_t g_rel_table[];
extern const uint32_t g_rel_count;
