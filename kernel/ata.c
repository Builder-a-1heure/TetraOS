#include "ata.h"
#include "io.h"
#include "screen.h"
#include "utils.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECT_COUNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_SEL   0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CTRL        0x3F6

#define ATA_BSY  0x80
#define ATA_DRQ  0x08
#define ATA_ERR  0x01

static void io_wait() {
    for (int i = 0; i < 4; ++i) inb(0x80);
}

static int wait_bsy_clear(int timeout) {
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_BSY)) return 0;
    }
    return -1;
}

void ata_init(void) {
    print_string("ATA: initialisation...\n");

    // Reset du contrôleur
    print_string("ATA: reset controller...\n");
    outb(ATA_CTRL, 0x04);
    io_wait();
    outb(ATA_CTRL, 0x00);
    io_wait();

    // Sélectionne le disque master
    print_string("ATA: selecting master drive...\n");
    outb(ATA_DRIVE_SEL, 0xE0);  // Master drive, LBA mode
    io_wait();

    // Lecture des registres pour debug
    uint8_t error = inb(ATA_ERROR);
    uint8_t sect_count = inb(ATA_SECT_COUNT);
    uint8_t lba_low = inb(ATA_LBA_LOW);
    uint8_t lba_mid = inb(ATA_LBA_MID);
    uint8_t lba_high = inb(ATA_LBA_HIGH);
    uint8_t status = inb(ATA_STATUS);

    print_string("ATA: error="); print_hex(error); print_string("\n");
    print_string("ATA: sect_count="); print_hex(sect_count); print_string("\n");
    print_string("ATA: lba_low="); print_hex(lba_low); print_string("\n");
    print_string("ATA: lba_mid="); print_hex(lba_mid); print_string("\n");
    print_string("ATA: lba_high="); print_hex(lba_high); print_string("\n");
    print_string("ATA: status="); print_hex(status); print_string("\n");

    // Test d'identification
    print_string("ATA: sending IDENTIFY command...\n");
    outb(ATA_SECT_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, 0xEC);  // IDENTIFY command
    io_wait();

    // Attend que le disque soit prêt
    int timeout = 1000000;
    while (--timeout) {
        status = inb(ATA_STATUS);
        if (status & ATA_ERR) {
            print_string("ATA: error during identify\n");
            return;
        }
        if (status & ATA_DRQ) break;  // Data ready
    }

    if (timeout == 0) {
        print_string("ATA: timeout during identify\n");
        return;
    }

    // Si on arrive ici, le disque a répondu!
    print_string("ATA: disque detecte et identifie!\n");

    // Lecture des données d'identification (512 bytes)
    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(ATA_DATA);
        // Vous pouvez traiter les données ici si besoin
    }

    print_string("ATA: disque pret\n");
}

int ata_read_single(uint32_t lba, uint8_t* buffer) {
    //print_string("+++ ATA: in ata_read_single\n");
    // select drive master + LBA bits
    outb(ATA_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();

    if (wait_bsy_clear(100000) != 0) { print_string("ATA: busy after select\n"); return -1; }

    outb(ATA_SECT_COUNT, 1);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, 0x20); // READ SECTORS

    int timeout = 1000000;
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_ERR) { print_string("ATA read error (status ERR)\n"); return -1; }
        if (status & ATA_DRQ) break;
    }
    if (timeout == 0) { print_string("ATA timeout (read DRQ)\n"); return -1; }

    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(ATA_DATA);
        buffer[i * 2]     = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(data >> 8);
    }
    return 0;
}

int ata_write_single(uint32_t lba, const uint8_t* buffer) {
    //print_string("+++ ATA: in ata_write_single\n");
    outb(ATA_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();

    if (wait_bsy_clear(100000) != 0) { print_string("ATA: busy after select\n"); return -1; }

    outb(ATA_SECT_COUNT, 1);
    outb(ATA_LBA_LOW,  lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND,  0x30);  // WRITE SECTOR

    int timeout = 1000000;
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_ERR) { print_string("ATA write error (during DRQ)\n"); return -1; }
        if (!(status & ATA_BSY) && (status & ATA_DRQ)) break;
    }
    if (timeout == 0) { print_string("ATA timeout (write DRQ)\n"); return -1; }

    for (int i = 0; i < 256; i++) {
        uint16_t data = (buffer[i * 2 + 1] << 8) | buffer[i * 2];
        outw(ATA_DATA, data);
    }

    // wait write completion
    timeout = 1000000;
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_ERR) { print_string("ATA write error (finish)\n"); return -1; }
        if (!(status & ATA_BSY)) break;
    }
    if (timeout == 0) { print_string("ATA timeout (write finish)\n"); return -1; }

    // Optionnel sur vrai disque : flush cache
    // outb(ATA_COMMAND, 0xE7);
    // wait...

    return 0;
}

int ata_read(uint32_t lba, uint8_t* buffer, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        int r = ata_read_single(lba + i, buffer + i * 512);
        if (r != 0) return r;
    }
    return 0;
}

int ata_write(uint32_t lba, const uint8_t* buffer, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        int r = ata_write_single(lba + i, buffer + i * 512);
        if (r != 0) return r;
    }
    return 0;
}
