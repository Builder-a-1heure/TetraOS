// kernel/lib/timer.h — Timer système (PIT channel 0, IRQ0)
//
// C'est la brique qui manquait pour du multithreading préemptif futur :
// un tick périodique et fiable qui interrompt le code courant. Pour
// l'instant on se contente d'incrémenter un compteur de ticks (utile
// pour un sleep() propre) — le vrai context switch viendra se brancher
// sur timer_register_tick_callback() à l'étape suivante.
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// Configure le PIT channel 0 pour interrompre à `hz` Hz, installe le
// handler IRQ0 et démasque cette IRQ (les autres restent masquées).
void timer_init(uint32_t hz);

// Nombre de ticks écoulés depuis timer_init().
uint32_t timer_get_ticks(void);

// Callback optionnel appelé à CHAQUE tick, depuis le contexte IRQ0
// (donc court et rapide — pas d'allocation, pas de blocage). C'est ici
// que s'accrochera le scheduler préemptif plus tard.
typedef void (*timer_tick_cb_t)(void);
void timer_set_tick_callback(timer_tick_cb_t cb);

#endif
