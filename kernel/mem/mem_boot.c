#include "../mem/mem_boot.h"
#include "../mem/pfa.h"
void mem_boot_init(uintptr_t phys_base, size_t phys_size) {
    pfa_init(phys_base, phys_size);
}
