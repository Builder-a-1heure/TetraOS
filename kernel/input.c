#include "screen.h"
#include "fs.h"
#include "input.h"
#include "mouse.h"
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

// CORRECTION BUG #3 : Meilleure gestion du flag ctrl_pressed
char keyboard_get_char() {
    // Réinitialiser ctrl_pressed au début pour éviter qu'il reste bloqué
    static int last_ctrl_scancode = 0;

    while (1) {
        // --- Polling souris (non-bloquant) ---
        // On lit le status byte : bit 0 = donnée dispo, bit 5 = vient de la souris
        unsigned char st;
        __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));

        if (st & 1) {
            if (st & (1 << 5)) {
                // Donnée souris → laisser mouse_poll la consommer
                if (mouse_poll()) {
                    mouse_erase_cursor();
                    mouse_draw_cursor();
                }
                continue; // Pas de scancode clavier ici
            }
        } else {
            // Rien à lire du tout → petit yield et on recommence
            __asm__ __volatile__("nop");
            continue;
        }

        // --- Donnée clavier (bit5 = 0, bit0 = 1) ---
        unsigned char scancode;
        __asm__ __volatile__("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));

        // Gestion des modificateurs Shift
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }

        // CORRECTION : Gestion améliorée de Ctrl
        if (scancode == 0x1D) { ctrl_pressed = 1; last_ctrl_scancode = scancode; continue; }
        if (scancode == 0x9D) { ctrl_pressed = 0; last_ctrl_scancode = 0; continue; }

        // Ignorer les key-up events
        if (scancode & 0x80) {
            if (ctrl_pressed && last_ctrl_scancode != 0) {
                static int release_count = 0;
                release_count++;
                if (release_count > 5) { ctrl_pressed = 0; last_ctrl_scancode = 0; release_count = 0; }
            }
            continue;
        }

        // Touches spéciales
        if (scancode == 0x01) return 27;   // ESC
        if (scancode == 0x0E) return '\b'; // Backspace
        if (scancode == 0x48) return 16;   // Flèche haut
        if (scancode == 0x50) return 14;   // Flèche bas
        if (scancode == 0x4B) return 17;   // Flèche gauche
        if (scancode == 0x4D) return 18;   // Flèche droite

        // Touches normales
        if (scancode < (sizeof(keyboard_map)/sizeof(keyboard_map[0]))) {
            char c = shift_pressed ? keyboard_map_shift[scancode] : keyboard_map[scancode];

            if (ctrl_pressed && (c == 'c' || c == 'C')) { ctrl_pressed = 0; return 3; }
            if (ctrl_pressed && (c == 's' || c == 'S')) { ctrl_pressed = 0; return 19; }
            if (ctrl_pressed && (c == 'q' || c == 'Q')) { ctrl_pressed = 0; return 17; }

            if (c) return c;
        }
    }
}