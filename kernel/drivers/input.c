#include "../gfx/screen.h"
#include "../fs/fs.h"
#include "../drivers/input.h"
#include "../drivers/mouse.h"
#include <stdint.h>

char input_buffer[512];
int input_index = 0;
int shift_pressed = 0;
int ctrl_pressed = 0;

// CORRECTION BUG #1 : Keyboard map AZERTY corrigé (clavier français)
// Scancodes correspondant aux touches dans l'ordre du scancode
unsigned char keyboard_map[256] = {
    0,      // 0x00 - Aucune touche
    27,     // 0x01 - ESC
    '&',    // 0x02 - 1
    'e',    // 0x03 - 2 (é)
    '"',    // 0x04 - 3
    '\'',   // 0x05 - 4 (apostrophe)
    '(',    // 0x06 - 5
    '-',    // 0x07 - 6
    'e',    // 0x08 - 7 (è)
    '_',    // 0x09 - 8
    'c',    // 0x0A - 9 (ç)
    'a',    // 0x0B - 0 (à)
    ')',    // 0x0C - )
    '=',    // 0x0D - =
    '\b',   // 0x0E - Backspace
    '\t',   // 0x0F - Tab
    'a',    // 0x10 - A
    'z',    // 0x11 - Z
    'e',    // 0x12 - E
    'r',    // 0x13 - R
    't',    // 0x14 - T
    'y',    // 0x15 - Y
    'u',    // 0x16 - U
    'i',    // 0x17 - I
    'o',    // 0x18 - O
    'p',    // 0x19 - P
    '^',    // 0x1A - ^
    '$',    // 0x1B - $
    '\n',   // 0x1C - Enter
    0,      // 0x1D - Ctrl gauche
    'q',    // 0x1E - Q
    's',    // 0x1F - S
    'd',    // 0x20 - D
    'f',    // 0x21 - F
    'g',    // 0x22 - G
    'h',    // 0x23 - H
    'j',    // 0x24 - J
    'k',    // 0x25 - K
    'l',    // 0x26 - L
    'm',    // 0x27 - M
    'u',    // 0x28 - ù
    '*',    // 0x29 - ² (clavier FR)
    0,      // 0x2A - Shift gauche
    '*',    // 0x2B - * (étoile)
    'w',    // 0x2C - W
    'x',    // 0x2D - X
    'c',    // 0x2E - C *** CORRECTION : c'est ICI le scancode du 'c' ***
    'v',    // 0x2F - V
    'b',    // 0x30 - B
    'n',    // 0x31 - N
    ',',    // 0x32 - ,
    ';',    // 0x33 - ;
    ':',    // 0x34 - :
    '!',    // 0x35 - !
    0,      // 0x36 - Shift droit
    '*',    // 0x37 - * (pavé numérique)
    0,      // 0x38 - Alt
    ' ',    // 0x39 - Espace
};

// Keyboard map avec Shift enfoncé (AZERTY - clavier français)
unsigned char keyboard_map_shift[256] = {
    0,      // 0x00
    27,     // 0x01 - ESC
    '1',    // 0x02
    '2',    // 0x03
    '3',    // 0x04
    '4',    // 0x05
    '5',    // 0x06
    '6',    // 0x07
    '7',    // 0x08
    '8',    // 0x09
    '9',    // 0x0A
    '0',    // 0x0B
    '+',    // 0x0C
    '=',    // 0x0D
    '\b',   // 0x0E
    '\t',   // 0x0F
    'A',    // 0x10
    'Z',    // 0x11
    'E',    // 0x12
    'R',    // 0x13
    'T',    // 0x14
    'Y',    // 0x15
    'U',    // 0x16
    'I',    // 0x17
    'O',    // 0x18
    'P',    // 0x19
    '[',    // 0x1A
    ']',    // 0x1B
    '\n',   // 0x1C
    0,      // 0x1D
    'Q',    // 0x1E
    'S',    // 0x1F
    'D',    // 0x20
    'F',    // 0x21
    'G',    // 0x22
    'H',    // 0x23
    'J',    // 0x24
    'K',    // 0x25
    'L',    // 0x26
    'M',    // 0x27
    '%',    // 0x28
    '~',    // 0x29
    0,      // 0x2A
    '#',    // 0x2B
    'W',    // 0x2C
    'X',    // 0x2D
    'C',    // 0x2E - C majuscule
    'V',    // 0x2F
    'B',    // 0x30
    'N',    // 0x31
    '?',    // 0x32
    '.',    // 0x33
    '/',    // 0x34
    's',    // 0x35
    0,      // 0x36
    '*',    // 0x37
    0,      // 0x38
    ' ',    // 0x39
};

extern char input_buffer[512];
extern int input_index;
extern int shift_pressed;

static void (*s_mouse_packet_handler)(void) = NULL;

void input_set_mouse_packet_handler(void (*fn)(void)) {
    s_mouse_packet_handler = fn;
}

static void on_mouse_packet_complete(void) {
    if (s_mouse_packet_handler)
        s_mouse_packet_handler();
    else {
        mouse_erase_cursor();
        mouse_draw_cursor();
    }
}

int keyboard_read_scancode() {
    unsigned char status;
    do {
        __asm__ __volatile__("inb %1, %0" : "=a"(status) : "Nd"(0x64));
    } while (!(status & 1));

    unsigned char scancode;
    __asm__ __volatile__("inb %1, %0" : "=a"(scancode) : "Nd"(0x60));
    return scancode;
}

void handle_input() {
    while (1) {
        unsigned char scancode = keyboard_read_scancode();
        char c = 0;

        // Gestion des modificateurs (Shift)
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }
        
        // Ignorer les releases (bit 7 set) sauf pour Shift déjà géré
        if (scancode & 0x80) continue;

        // Cas spécial pour la barre espace (scancode 0x39)
        if (scancode == 0x39) {
            c = ' ';
        }
        // Gestion normale des autres touches
        else if (scancode < (int)(sizeof(keyboard_map)/sizeof(keyboard_map[0]))) {
            c = shift_pressed ? keyboard_map_shift[scancode] : keyboard_map[scancode];
        }

        // Traitement du caractère
        if (c == '\n') {
            print_char('\n');
            input_buffer[input_index] = 0;
            input_index = 0;
            print_string("TetraOS/ > ");
        } else if (c == '\b') {
            if (input_index > 0) {
                input_index--;
                print_char('\b');
            }
        } else if (c && input_index < 127) {
            input_buffer[input_index++] = c;
            print_char(c);
        }
    }
}

char get_input_char() {
    while (1) {
        unsigned char scancode = keyboard_read_scancode();
        
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }
        if (scancode & 0x80) continue;
        
        if (scancode == 0x01) return 27; // ESC
        if (scancode == 0x0E) return '\b'; // Backspace
        
        if (scancode < (sizeof(keyboard_map)/sizeof(keyboard_map[0]))) {
            char c = shift_pressed ? keyboard_map_shift[scancode] : keyboard_map[scancode];
            if (ctrl_pressed && (c == 'c' || c == 'C')) return 3;
            if (c) return c;
        }
    }
}

// ============================================================
// DISPATCHER CENTRALISÉ 8042
// Principe : on lit le status register EN PREMIER et on route.
//   bit0 = 1 : donnée disponible dans le buffer
//   bit5 = 1 : elle vient de la souris (port auxiliaire)
//   bit5 = 0 : elle vient du clavier
// On ne lit JAMAIS le port 0x60 sans avoir vérifié bit5 d'abord.
// ============================================================
char input_dispatch_char(void) {
    while (1) {
        uint8_t st;
        __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));

        if (!(st & 1)) {
            __asm__ __volatile__("nop");
            continue; // buffer vide
        }

        // Paquet souris déjà commencé : les octets 2 et 3 doivent aller à la souris
        // même si le bit 5 du status est à 0 (QEMU / VirtualBox le signalent mal).
        if (mouse_in_packet()) {
            if (mouse_poll())
                on_mouse_packet_complete();
            return 0;
        }

        if (st & (1 << 5)) {
            if (mouse_poll())
                on_mouse_packet_complete();
            return 0;
        }

        // ── Donnée clavier ─────────────────────────────────────
        uint8_t scancode;
        __asm__ __volatile__("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));

        // Modificateurs
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return 0; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return 0; }
        if (scancode == 0x1D) { ctrl_pressed = 1;  return 0; }
        if (scancode == 0x9D) { ctrl_pressed = 0;  return 0; }

        // Key-up → ignorer
        if (scancode & 0x80) return 0;

        // Touches spéciales
        if (scancode == 0x01) return 27;   // ESC
        if (scancode == 0x0E) return '\b'; // Backspace
        if (scancode == 0x48) return 16;   // Flèche haut
        if (scancode == 0x50) return 14;   // Flèche bas
        if (scancode == 0x4B) return 17;   // Flèche gauche
        if (scancode == 0x4D) return 18;   // Flèche droite

        // Touche normale
        if (scancode < 256) {
            char c = shift_pressed ? (char)keyboard_map_shift[scancode]
                                   : (char)keyboard_map[scancode];
            if (ctrl_pressed) {
                ctrl_pressed = 0;
                if (c == 'c' || c == 'C') return 3;
                if (c == 's' || c == 'S') return 19;
                if (c == 'q' || c == 'Q') return 17;
                return 0;
            }
            if (c) return c;
        }
        return 0;
    }
}

// keyboard_get_char : bloque jusqu'à obtenir un vrai caractère clavier
// (ignore les événements souris mais les traite au passage)
char keyboard_get_char(void) {
    while (1) {
        char c = input_dispatch_char();
        if (c != 0) return c;
    }
}