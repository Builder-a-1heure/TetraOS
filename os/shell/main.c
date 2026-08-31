// main.c — Point d'entrée du kernel TetraOS
//
// Responsabilités UNIQUEMENT :
//   - start()    : stub ASM (init pile, BSS, appel kmain)
//   - kmain()    : séquence de boot + boucle principale login→desktop
//   - read_line(): saisie d'une ligne en mode texte (utilisée par shell.c)
//
// Tout le reste est dans les modules dédiés :
//   shell/shell.c    — dispatchers de commandes
//   shell/terminal.c — terminal fenêtré
//   shell/editor.c   — éditeur de texte
//   ui/session.c     — login / sessions
//   ui/desktop.c     — bureau graphique

#include "../gfx/screen.h"
#include "../drivers/input.h"
#include "../drivers/ata.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../gfx/vesaanim.h"
#include "../fs/fs.h"
#include "../ui/session.h"
#include "../ui/desktop.h"
#include "../lib/utils.h"
#include "../lib/io.h"
#include "../lib/idt.h"
#include "../lib/timer.h"
#include "../lib/sched.h"
#include <stdint.h>

__attribute__((naked)) __attribute__((section(".text.start")))
void start(void) {
    asm volatile (
        // CORRECTION : stack remonté de 0xA00000 (10 Mo) à 0x3000000 (48 Mo).
        // Le backbuffer VESA_BB_W*H (désormais 1920x1080x4 = 8,3 Mo de BSS,
        // voir vesa.h) faisait dépasser le kernel au-delà de 10 Mo et
        // rentrait en collision avec l'ancien stack. VM allouée à 64 Mo
        // (-m 64M dans les scripts) → 48 Mo laisse ~16 Mo de marge stack
        // avant la fin de la RAM, largement suffisant.
        "mov $0x3000000, %esp\n"
        "mov %esp, %ebp\n"
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n"
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        "mov %ax, %ss\n"
        "mov $_bss_start, %edi\n"
        "mov $_bss_end,   %ecx\n"
        "sub %edi, %ecx\n"
        "shr $2, %ecx\n"
        "xor %eax, %eax\n"
        "rep stosl\n"
        "call kmain\n"
        "hlt\n"
        "jmp .\n"
    );
}

const char boot_msg[] __attribute__((section(".rodata"))) = "Demarrage...\n";

void read_line(char* buffer, int max_len) {
    int idx = 0;
    while (1) {
        char c = keyboard_get_char();
        if (c == '\n' || c == '\r') {
            buffer[idx] = '\0';
            print_char('\n');
            break;
        } else if ((c == '\b' || c == 127) && idx > 0) {
            idx--;
            print_string("\b \b");
        } else if (c >= 32 && c <= 126 && idx < max_len - 1) {
            buffer[idx++] = c;
            print_char(c);
        }
    }
}

void kmain(void) {
    screen_init();
    clear_screen();

    print_string("ETAPE 1 : Debut kmain()\n");
    print_string("ETAPE 2 : Ecran initialise");
    print_string(vesa_active() ? " (VESA 1920x1080)\n" : " (VGA 80x25)\n");

    print_string("ETAPE 3 : Initialisation ATA\n");
    ata_init();

    print_string("ETAPE 4 : Initialisation systeme de fichiers\n");
    fs_init();

    vesa_boot_anim();

    print_string("ETAPE 5 : Initialisation des sessions\n");
    session_init();
    session_load();

    print_string("ETAPE 6 : Initialisation souris PS/2\n");
    mouse_init();

    print_string("ETAPE 7 : Initialisation IDT/PIC/Timer\n");
    idt_init();     // charge l'IDT + remappe le PIC (IRQ0-15 -> 32-47), tout masque
    timer_init(100); // IRQ0 a 100 Hz, demasque cette IRQ (les autres restent masquees)

    print_string("ETAPE 8 : Initialisation du scheduler\n");
    sched_init(); // tache 0 ("main") = continuation de ce kmain(). Doit venir
                  // APRES timer_init() (branche la preemption sur son tick).
                  // Tant qu'aucun thread_create() n'est appele, comportement
                  // strictement identique a avant (mono-tache).

    interrupts_enable(); // sti — a partir d'ici les interruptions matérielles marchent.
    // NOTE : clavier (IRQ1) et souris (IRQ12) restent volontairement masquees.
    // Le polling existant (input_poll_char(), mouse_poll()) continue de fonctionner
    // sans changement — les faire passer en IRQ est un futur refactor a part.

    // Boucle principale : login -> bureau -> logout -> login
    while (1) {
        if (session_do_login_flow() != 0) {
            print_string("Erreur fatale : echec du login\n");
            while (1) { asm volatile("nop"); }
        }
        desktop_run();
        print_string("\nDeconnexion...\n");
    }

    while (1) { asm volatile("nop"); }
}