#pragma once
#include <stdint.h>
#include <stddef.h>
void pfa_init(uintptr_t phys_base, size_t mem_size_bytes);
uintptr_t pfa_alloc_frame(void);
void pfa_free_frame(uintptr_t frame_addr);
size_t pfa_total_frames(void);
size_t pfa_free_frames(void);
