/* Common entry point (called by the platform's main): loads the game from the
 * user's disc image and runs the recompiled __start. */
#include <stdio.h>
#include "runtime.h"
#include "platform.h"

void rt_main(void)
{
    char path[512];
    rt_log("NSMBW recomp runtime");

    if (mem_init())
        rt_fatal("not enough memory");

    snprintf(path, sizeof path, "%sgame.iso", plat_asset_dir());
    if (iso_open(path))
        rt_fatal("cannot open %s", path);
    rt_log("disc image: %s", path);

    uint8_t header[0x20];
    iso_read(0, header, sizeof header);
    if (memcmp(header, "SMNE01", 6))
        rt_log("warning: game id %.6s, this build was recompiled from SMNE01", (const char *)header);
    lowmem_init(header);

    uint32_t entry;
    if (load_dol_from_iso(&entry))
        rt_fatal("cannot load main.dol from the disc image");
    if (load_fst_from_iso())
        rt_fatal("cannot load the FST from the disc image");
    snprintf(path, sizeof path, "%srel_image.bin", plat_asset_dir());
    if (load_rel_image(path))
        rt_fatal("missing %s", path);

    hw_init();
    ios_init();
    rt_log("jumping to %08lX", (unsigned long)entry);
    rt_os_boot(entry);
}
