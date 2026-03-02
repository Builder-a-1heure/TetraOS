#pragma once
#include <stdint.h>
#define BOOT_INFO_ADDR 0x9000
#define BOOT_INFO_SIG  0xB00F1
typedef struct {
    uint32_t phys_base;
    uint32_t phys_size;
    uint32_t signature;
} boot_info_t;
void bootinfo_try_init(void);
