// kernel/lib/pic.h — Contrôleur d'interruptions 8259 (PIC)
//
// Par défaut (config BIOS), IRQ0-7 sont mappées sur les vecteurs 0x08-0x0F
// (8-15) — ce qui rentre EN PLEIN dans la plage réservée aux exceptions
// CPU (0-31) ! Ex: IRQ7 (0x0F/15) écraserait le vecteur d'exception 15.
// On remappe donc IRQ0-15 vers 32-47, hors de la zone des exceptions.
#ifndef PIC_H
#define PIC_H

#include <stdint.h>

// Remappe le PIC maître (IRQ0-7) sur [offset1, offset1+7] et le PIC
// esclave (IRQ8-15) sur [offset2, offset2+7]. Masque tout à la fin
// (aucune IRQ active par défaut — à démasquer explicitement au cas
// par cas via pic_clear_mask()).
void pic_remap(uint8_t offset1, uint8_t offset2);

// Accuse réception (End Of Interrupt) — OBLIGATOIRE à la fin de tout
// handler d'IRQ, sinon le PIC ne renverra plus jamais cette ligne
// (et les lignes de priorité inférieure restent bloquées).
void pic_send_eoi(uint8_t irq);

// Masque/démasque une IRQ précise (0-15).
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
