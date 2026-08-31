// kernel/lib/isr.c — Stubs d'entrée des interruptions + dispatcher C
//
// Chaque exception CPU (0-31) et chaque IRQ matérielle (32-47 après
// remap) a besoin d'un petit stub assembleur qui :
//   1) pousse un code d'erreur bidon si le CPU n'en pousse pas déjà un
//   2) pousse le numéro de vecteur
//   3) saute vers isr_common_stub, qui sauvegarde tous les registres
//      (pusha), appelle interrupt_handler() en C, restaure, et fait
//      iret pour reprendre le code interrompu.
//
// Écrit en assembleur inline GCC top-level (macros GAS .macro/.endm)
// plutôt qu'en .asm séparé : le build script ne compile que des .c,
// pas la peine d'y toucher.
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "../gfx/screen.h"

// ============================================================
// Table de l'IDT
// ============================================================
static idt_entry_t g_idt[256];
static idt_ptr_t   g_idt_ptr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    g_idt[num].base_low  = (uint16_t)(base & 0xFFFF);
    g_idt[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
    g_idt[num].sel       = sel;
    g_idt[num].always0   = 0;
    g_idt[num].flags     = flags;
}

static inline void idt_flush(uint32_t ptr_addr) {
    __asm__ volatile ("lidt (%0)" :: "r"(ptr_addr));
}

// ============================================================
// Déclarations des 48 stubs (32 exceptions + 16 IRQ) définis dans le
// bloc asm plus bas.
// ============================================================
#define DECL_ISR(n) extern void isr##n(void);
DECL_ISR(0)  DECL_ISR(1)  DECL_ISR(2)  DECL_ISR(3)
DECL_ISR(4)  DECL_ISR(5)  DECL_ISR(6)  DECL_ISR(7)
DECL_ISR(8)  DECL_ISR(9)  DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15)
DECL_ISR(16) DECL_ISR(17) DECL_ISR(18) DECL_ISR(19)
DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27)
DECL_ISR(28) DECL_ISR(29) DECL_ISR(30) DECL_ISR(31)
#undef DECL_ISR

#define DECL_IRQ(n) extern void irq##n(void);
DECL_IRQ(0)  DECL_IRQ(1)  DECL_IRQ(2)  DECL_IRQ(3)
DECL_IRQ(4)  DECL_IRQ(5)  DECL_IRQ(6)  DECL_IRQ(7)
DECL_IRQ(8)  DECL_IRQ(9)  DECL_IRQ(10) DECL_IRQ(11)
DECL_IRQ(12) DECL_IRQ(13) DECL_IRQ(14) DECL_IRQ(15)
#undef DECL_IRQ

// ============================================================
// Les stubs eux-mêmes + le tronc commun.
//
// Layout pile au moment du "call interrupt_handler" (voir regs_t
// dans idt.h) : edi esi ebp esp_dummy ebx edx ecx eax [pusha]
//               int_no err_code [stubs]
//               eip cs eflags [poussés par le CPU]
// ============================================================
__asm__ (
    ".macro ISR_NOERR num\n"
    ".global isr\\num\n"
    "isr\\num:\n"
    "    cli\n"
    "    pushl $0\n"        // code d'erreur a la con (le CPU n'en pousse pas pour ce vecteur)
    "    pushl $\\num\n"
    "    jmp isr_common_stub\n"
    ".endm\n"

    ".macro ISR_ERR num\n"
    ".global isr\\num\n"
    "isr\\num:\n"
    "    cli\n"
    // le CPU a déjà poussé le code d'erreur pour ce vecteur
    "    pushl $\\num\n"
    "    jmp isr_common_stub\n"
    ".endm\n"

    ".macro IRQ num, mapped\n"
    ".global irq\\num\n"
    "irq\\num:\n"
    "    cli\n"
    "    pushl $0\n"
    "    pushl $\\mapped\n"
    "    jmp isr_common_stub\n"
    ".endm\n"

    "ISR_NOERR 0\n"  "ISR_NOERR 1\n"  "ISR_NOERR 2\n"  "ISR_NOERR 3\n"
    "ISR_NOERR 4\n"  "ISR_NOERR 5\n"  "ISR_NOERR 6\n"  "ISR_NOERR 7\n"
    "ISR_ERR   8\n"  "ISR_NOERR 9\n"  "ISR_ERR   10\n" "ISR_ERR   11\n"
    "ISR_ERR   12\n" "ISR_ERR   13\n" "ISR_ERR   14\n" "ISR_NOERR 15\n"
    "ISR_NOERR 16\n" "ISR_ERR   17\n" "ISR_NOERR 18\n" "ISR_NOERR 19\n"
    "ISR_NOERR 20\n" "ISR_NOERR 21\n" "ISR_NOERR 22\n" "ISR_NOERR 23\n"
    "ISR_NOERR 24\n" "ISR_NOERR 25\n" "ISR_NOERR 26\n" "ISR_NOERR 27\n"
    "ISR_NOERR 28\n" "ISR_NOERR 29\n" "ISR_NOERR 30\n" "ISR_NOERR 31\n"

    "IRQ 0, 32\n"  "IRQ 1, 33\n"  "IRQ 2, 34\n"  "IRQ 3, 35\n"
    "IRQ 4, 36\n"  "IRQ 5, 37\n"  "IRQ 6, 38\n"  "IRQ 7, 39\n"
    "IRQ 8, 40\n"  "IRQ 9, 41\n"  "IRQ 10, 42\n" "IRQ 11, 43\n"
    "IRQ 12, 44\n" "IRQ 13, 45\n" "IRQ 14, 46\n" "IRQ 15, 47\n"

    ".global isr_common_stub\n"
    "isr_common_stub:\n"
    "    pusha\n"
    "    mov  %esp, %eax\n"
    "    push %eax\n"              // argument : regs_t* r = esp courant
    "    call interrupt_handler\n"
    "    add  $4, %esp\n"          // dépiler l'argument
    "    popa\n"
    "    add  $8, %esp\n"          // dépiler int_no + err_code
    "    sti\n"
    "    iret\n"
);

// ============================================================
// Table des handlers IRQ enregistrés (32 vecteurs "logiques" 0-15)
// ============================================================
static irq_handler_t g_irq_handlers[16] = {0};

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) g_irq_handlers[irq] = handler;
}

void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) g_irq_handlers[irq] = 0;
}

// ============================================================
// Exceptions CPU — pas de quoi les récupérer proprement pour l'instant
// (pas de gestion mémoire virtuelle/paging), donc on affiche un écran
// de fault lisible et on arrête le CPU plutôt que de continuer dans un
// état corrompu (ce qui donnait avant des freezes/reboots silencieux
// façon triple fault).
// ============================================================
static const char* const EXCEPTION_NAMES[32] = {
    "Division par zero",            "Debug",
    "NMI (interruption non masquable)", "Breakpoint",
    "Overflow",                     "Depassement de limite (BOUND)",
    "Opcode invalide",              "Peripherique indisponible",
    "DOUBLE FAULT",                 "Depassement segment coproc (reserve)",
    "TSS invalide",                 "Segment absent",
    "Faute de pile",                "Protection generale (GPF)",
    "PAGE FAULT",                   "Reserve",
    "Erreur x87 FPU",               "Alignement invalide",
    "Machine check",                "Exception SIMD",
    "Virtualisation",               "Controle de securite",
    "Reserve", "Reserve", "Reserve", "Reserve",
    "Reserve", "Reserve", "Reserve", "Reserve",
    "Reserve", "Reserve"
};

static void panic_freeze(regs_t* r) {
    print_string("\n\n*** KERNEL PANIC ***\n");
    print_string("Exception : ");
    print_string(EXCEPTION_NAMES[r->int_no]);
    print_string("\nVecteur   : ");
    print_dec(r->int_no);
    print_string("\nCode err  : 0x");
    print_hex(r->err_code);
    print_string("\nEIP       : 0x");
    print_hex(r->eip);
    print_string("\n\nSysteme arrete.\n");
    interrupts_disable();
    for (;;) { __asm__ volatile ("hlt"); }
}

// ============================================================
// Dispatcher central — appelé par isr_common_stub pour CHAQUE
// interruption, exception ou IRQ confondues.
// ============================================================
void interrupt_handler(regs_t* r) {
    if (r->int_no < 32) {
        panic_freeze(r);
        return; // jamais atteint (panic_freeze ne revient pas)
    }

    uint8_t irq = (uint8_t)(r->int_no - 32);
    // IMPORTANT : l'EOI est envoyé AVANT le handler, pas après.
    // Nécessaire depuis l'introduction du scheduler (sched.c) : le
    // handler du timer (IRQ0) peut déclencher une commutation de thread
    // qui "abandonne" cette pile pour sauter sur une autre sans jamais
    // revenir exécuter le reste de cette fonction. Si l'EOI était envoyé
    // après, il ne serait alors émis que bien plus tard (quand cette
    // tâche serait réélue) — entre-temps, le PIC 8259 considère IRQ0
    // (et toute IRQ de priorité inférieure) comme "en service" et
    // bloque leur livraison : plus aucun tick, donc plus aucune
    // préemption, dès la toute première commutation. L'IF CPU restant à
    // 0 jusqu'au sti de l'épilogue plus bas, envoyer l'EOI plus tôt ne
    // risque pas de provoquer de ré-entrance.
    pic_send_eoi(irq);
    if (g_irq_handlers[irq]) {
        g_irq_handlers[irq](r);
    }
}

// ============================================================
// Initialisation complète : IDT + PIC remap + chargement (lidt)
// ============================================================
void idt_init(void) {
    g_idt_ptr.limit = (uint16_t)(sizeof(idt_entry_t) * 256 - 1);
    g_idt_ptr.base  = (uint32_t)&g_idt;

    for (int i = 0; i < 256; i++) idt_set_gate((uint8_t)i, 0, 0, 0);

    #define SET_ISR(n) idt_set_gate(n, (uint32_t)isr##n, 0x08, 0x8E)
    SET_ISR(0);  SET_ISR(1);  SET_ISR(2);  SET_ISR(3);
    SET_ISR(4);  SET_ISR(5);  SET_ISR(6);  SET_ISR(7);
    SET_ISR(8);  SET_ISR(9);  SET_ISR(10); SET_ISR(11);
    SET_ISR(12); SET_ISR(13); SET_ISR(14); SET_ISR(15);
    SET_ISR(16); SET_ISR(17); SET_ISR(18); SET_ISR(19);
    SET_ISR(20); SET_ISR(21); SET_ISR(22); SET_ISR(23);
    SET_ISR(24); SET_ISR(25); SET_ISR(26); SET_ISR(27);
    SET_ISR(28); SET_ISR(29); SET_ISR(30); SET_ISR(31);
    #undef SET_ISR

    #define SET_IRQ(n) idt_set_gate(32 + n, (uint32_t)irq##n, 0x08, 0x8E)
    SET_IRQ(0);  SET_IRQ(1);  SET_IRQ(2);  SET_IRQ(3);
    SET_IRQ(4);  SET_IRQ(5);  SET_IRQ(6);  SET_IRQ(7);
    SET_IRQ(8);  SET_IRQ(9);  SET_IRQ(10); SET_IRQ(11);
    SET_IRQ(12); SET_IRQ(13); SET_IRQ(14); SET_IRQ(15);
    #undef SET_IRQ

    // Remap AVANT de charger l'IDT : sinon la moindre IRQ perdue entre
    // les deux entrainerait un vecteur CPU 0-31 -> faux "exception".
    pic_remap(32, 40);

    idt_flush((uint32_t)&g_idt_ptr);
    // Toujours pas de sti ici — voir interrupts_enable() appelé
    // explicitement par le caller une fois timer_init() fait.
}
