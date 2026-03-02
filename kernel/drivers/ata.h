#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// Prototpyes de ata init
void ata_init();
// Prototypes (0 = success, non-0 = error)
int ata_read(uint32_t lba, uint8_t* buffer, uint32_t count);
int ata_write(uint32_t lba, const uint8_t* buffer, uint32_t count);
int ata_read_single(uint32_t lba, uint8_t* buffer);
int ata_write_single(uint32_t lba, const uint8_t* buffer);

#endif
