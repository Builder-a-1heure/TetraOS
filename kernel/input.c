#include "screen.h"
#include "fs.h"
#include "input.h"

char input_buffer[512];
int input_index = 0;
int shift_pressed = 0;
int ctrl_pressed = 0;

unsigned char keyboard_map[256] = {
    0, 27, '&', 'e', '"', '#', '(', '-', 'e', '_', 'c', 'a', '-', '=', '\b',
    '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
    0, 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 'u', '*',
    0, '\\', 'w', 'x', 'c', 'v', 'b', 'n', ',', ')', ':', '!', 0,
    '*', 0, ' '
};

unsigned char keyboard_map_shift[256] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', '=', '\b',
    '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '~', '#', '\n',
    0, 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 'u',
    0, '>', 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 's', 0,
    '*', 0, ' '
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
        char c = 0; // Déclaration de la variable c

        // Gestion des modificateurs (Shift)
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }
        
        // Ignorer les releases (bit 7 set) sauf pour Shift déjà géré
        if (scancode & 0x80) continue;

        // Cas spécial pour la barre espace (scancode 0x39)
        if (scancode == 0x39) {
            c = ' '; // Assignation directe pour la barre espace
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
            if (ctrl_pressed && (c == 'c' || c == 'C')) return 3; // /*/*ctrl+c removed*/ removed*/
            if (c) return c;
        }
    }
}

char keyboard_get_char() {
    while (1) {
        unsigned char scancode = keyboard_read_scancode();
        
        // Gestion des modificateurs
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }
        if (scancode & 0x80) continue;  // Ignorer les key-up events
        
        // Gestion des touches spéciales
        if (scancode == 0x01) return 27;  // ESC
        if (scancode == 0x0E) return ''; // Backspace
        if (scancode == 0x1D) { ctrl_pressed = 1; continue; } // Ctrl down
        if (scancode == 0x9D) { ctrl_pressed = 0; continue; } // Ctrl up
        
        // Touches normales
        if (scancode < (sizeof(keyboard_map)/sizeof(keyboard_map[0]))) {
            char c = shift_pressed ? keyboard_map_shift[scancode] : keyboard_map[scancode];
            if (ctrl_pressed && (c == 'c' || c == 'C')) return 3; // /*/*ctrl+c removed*/ removed*/
            if (c) return c;
        }
    }
}