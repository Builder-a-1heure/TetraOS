// kernel/lib/timer.c
#include "timer.h"
#include "idt.h"
#include "pic.h"
#include "io.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_FREQ_HZ 1193182u

static volatile uint32_t g_ticks = 0;
static timer_tick_cb_t   g_tick_cb = 0;

static void timer_irq_handler(regs_t* r) {
    (void)r;
    g_ticks++;
    if (g_tick_cb) g_tick_cb();
}

uint32_t timer_get_ticks(void) { return g_ticks; }

void timer_set_tick_callback(timer_tick_cb_t cb) { g_tick_cb = cb; }

void timer_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    uint32_t divisor = PIT_FREQ_HZ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF; // hz trop bas, on plafonne

    // Channel 0, accès lo/hi byte, mode 3 (square wave — génère un tick
    // périodique en continu), binaire.
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_irq_handler);
    pic_clear_mask(0); // seule IRQ démasquée pour l'instant
}
