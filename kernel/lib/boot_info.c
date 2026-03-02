#include "../lib/boot_info.h"
#include "../mem/mem_boot.h"
void bootinfo_try_init(void) {
    boot_info_t *b = (boot_info_t*)BOOT_INFO_ADDR;
    if (b->signature != BOOT_INFO_SIG) return;
    mem_boot_init((uintptr_t)b->phys_base, (size_t)b->phys_size);
}
