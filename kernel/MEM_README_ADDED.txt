Memory management subsystem added

V5 changes: boot_info struct written by bootloader at 0x9000 (phys_base=0x10000, phys_size=1GiB). boot_info.h/.c added and bootinfo_try_init() is called at start() if present.
