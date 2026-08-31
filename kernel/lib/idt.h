// kernel/lib/idt.h — Interrupt Descriptor Table
//
// Base indispensable pour gérer proprement les interruptions CPU
// (exceptions : division par zéro, page fault...) et matérielles
// (IRQ : timer, clavier, souris, disque...) au lieu du polling pur
// utilisé jusqu'ici.
//
// Ne touche PAS au bootloader/stage2 : on réutilise le sélecteur de
// code déjà posé par stage2.asm (CODE_SEG = 0x08).
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// ============================================================
// Descripteur IDT (interrupt gate 32 bits) — format x86 standard
// ============================================================
typedef struct __attribute__((packed)) {
    uint16_t base_low;   // bits 0-15 de l'adresse du handler
    uint16_t sel;        // sélecteur de segment de code (GDT) — 0x08 ici
    uint8_t  always0;    // toujours 0
    uint8_t  flags;      // present | DPL | type de gate
    uint16_t base_high;  // bits 16-31 de l'adresse du handler
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

// ============================================================
// État CPU sauvegardé sur la pile par isr_common_stub, dans l'ORDRE
// EXACT du push (pusha, puis int_no/err_code, puis ce que le CPU
// pousse automatiquement à l'entrée de l'interruption).
//
// On ne sauvegarde volontairement PAS les registres de segment
// (ds/es/fs/gs) : le kernel tourne toujours en flat single-segment
// (DATA_SEG=0x10 partout, jamais de ring3) donc rien à restaurer.
// ============================================================
typedef struct {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // pusha
    uint32_t int_no, err_code;                             // poussés par les stubs isrN/irqN
    uint32_t eip, cs, eflags;                               // poussés par le CPU
} regs_t;

typedef void (*irq_handler_t)(regs_t* r);

// Initialise l'IDT complète (256 entrées), remappe le PIC (IRQ0-15 →
// vecteurs 32-47 pour ne plus entrer en collision avec les exceptions
// CPU 0-31), masque TOUTES les IRQ par défaut, et charge l'IDT (lidt).
// N'active PAS les interruptions (pas de sti) — appeler interrupts_enable()
// explicitement une fois tout configuré (voir timer_init()).
void idt_init(void);

// Pose une entrée dans l'IDT.
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

// Enregistre/désenregistre un handler pour une IRQ (0-15).
// N'oublie pas de démasquer l'IRQ correspondante via pic_clear_mask()
// (voir pic.h) — sinon le handler ne sera jamais appelé.
void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);

static inline void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile ("cli"); }

#endif
