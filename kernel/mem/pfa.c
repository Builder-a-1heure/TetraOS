#include "pfa.h"
#include "../lib/utils.h"
#define FRAME_SIZE 4096U
static uint8_t *bitmap = 0;
static size_t bitmap_bits = 0;
static uintptr_t phys_base_addr = 0;
static size_t total_frames = 0;
static uint8_t bitmap_storage[1<<20];
static inline void set_bit(size_t i) { bitmap[i >> 3] |= (1 << (i & 7)); }
static inline void clear_bit(size_t i) { bitmap[i >> 3] &= ~(1 << (i & 7)); }
static inline int test_bit(size_t i) { return (bitmap[i >> 3] >> (i & 7)) & 1; }
void pfa_init(uintptr_t phys_base, size_t mem_size_bytes) {
    phys_base_addr = phys_base;
    total_frames = mem_size_bytes / FRAME_SIZE;
    bitmap_bits = total_frames;
    size_t bitmap_bytes = (bitmap_bits + 7) / 8;
    bitmap = bitmap_storage;
    if (bitmap_bytes > sizeof(bitmap_storage)) {
        memset(bitmap, 0xFF, sizeof(bitmap_storage));
        bitmap_bits = sizeof(bitmap_storage)*8;
        total_frames = bitmap_bits;
    } else {
        memset(bitmap, 0, bitmap_bytes);
    }
    if (bitmap_bits > 0) set_bit(0);
}
uintptr_t pfa_alloc_frame(void) {
    for (size_t i = 0; i < bitmap_bits; ++i) {
        if (!test_bit(i)) { set_bit(i); return phys_base_addr + i * FRAME_SIZE; }
    }
    return (uintptr_t)0;
}
void pfa_free_frame(uintptr_t frame_addr) {
    if (frame_addr < phys_base_addr) return;
    size_t idx = (frame_addr - phys_base_addr) / FRAME_SIZE;
    if (idx < bitmap_bits) clear_bit(idx);
}
size_t pfa_total_frames(void) { return total_frames; }
size_t pfa_free_frames(void) { size_t cnt=0; for(size_t i=0;i<bitmap_bits;++i) if(!test_bit(i)) ++cnt; return cnt; }
