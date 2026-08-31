// kernel/lib/pic.c
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4 0x01  // ICW4 sera envoyé
#define ICW1_INIT 0x10  // Démarre la séquence d'initialisation

#define ICW4_8086 0x01  // Mode 8086/88 (pas mode 8080 archaïque)

#define PIC_EOI   0x20

// Petite pause I/O — nécessaire entre deux écritures sur le PIC sur du
// matériel réel/QEMU un peu strict (écrire sur port 0x80, "unused I/O
// port" habituel pour temporiser un cycle bus sans dépendre du CPU).
static inline void io_wait(void) {
    outb(0x80, 0);
}

void pic_remap(uint8_t offset1, uint8_t offset2) {
    // ICW1 : démarre la séquence d'init sur les deux PIC (cascade)
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    // ICW2 : offset des vecteurs (où atterrissent IRQ0 et IRQ8 dans l'IDT)
    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    // ICW3 : topologie de la cascade (maître↔esclave sur IRQ2)
    outb(PIC1_DATA, 0x04); io_wait(); // esclave branché sur IRQ2 du maître
    outb(PIC2_DATA, 0x02); io_wait(); // identité cascade de l'esclave

    // ICW4 : mode 8086
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Masquer TOUTES les IRQ par défaut — chaque sous-système (timer,
    // futur clavier/souris en IRQ...) démasque explicitement la sienne
    // via pic_clear_mask() une fois son handler installé.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI); // IRQ esclave : accuser les DEUX PIC
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}
