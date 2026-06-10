#ifndef ATA_H
#define ATA_H

#include <stdint.h>

void ata_init();
int ata_read(uint32_t lba, uint8_t* buffer, uint32_t count);
int ata_read_single(uint32_t lba, uint8_t* buffer);
// Lecture multi-secteurs en une commande ATA — max 127 secteurs par appel.
// Beaucoup plus rapide que ata_read() pour les gros fichiers (ex: wallpaper).
int ata_read_multi(uint32_t lba, uint8_t* buffer, uint8_t count);
int ata_write(uint32_t lba, const uint8_t* buffer, uint32_t count);
int ata_write_single(uint32_t lba, const uint8_t* buffer);

#endif