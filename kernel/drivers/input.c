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
// DISPATCHER CENTRALISÉ 8042 — version robuste
//
// Stratégie de tri clavier vs souris :
//   1. Lire le status byte (port 0x64)
//   2. Si bit5=1  → donnée souris, déléguer à mouse_poll()
//   3. Si on est en cours de réception d'un paquet souris
//      (mouse_in_packet()==1) → continuer avec mouse_poll()
//   4. Sinon → clavier
//
// Cas QEMU/VirtualBox : bit5 n'est pas toujours fiable.
// On s'appuie donc en PRIORITÉ sur mouse_in_packet() qui suit
// l'état interne du paquet 3-octets, et bit5 en secondaire.
//
// GUARD anti-fuite : si un paquet souris est en cours depuis
// plus de N polls sans completion, on le reset (évite de
// bouffer des octets clavier par erreur).
// ============================================================

// Bits du status register PS/2 (port 0x64)
#define STATUS_OUTPUT_FULL  (1 << 0)  // Données dispo en lecture
#define STATUS_MOUSE_DATA   (1 << 5)  // Les données viennent de la souris

// Compteur de guard pour reset de paquet souris bloqué
static int g_mouse_pkt_guard = 0;
#define MOUSE_PKT_GUARD_MAX 8   // Max polls consécutifs en mode paquet

// Traite un scancode → retourne caractère (0 = rien/modificateur)
// VERSION CANONIQUE — utilisée partout, input.c ET appcore.c (via délégation)
static char process_keyboard_scancode(uint8_t scancode) {
    // Key-up events (bit7 = 1)
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return 0; }
    if (scancode == 0x9D)                      { ctrl_pressed  = 0; return 0; }
    if (scancode & 0x80) return 0;  // tout autre key-up → ignorer

    // Modificateurs (key-down)
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return 0; }
    if (scancode == 0x1D)                      { ctrl_pressed  = 1; return 0; }

    // Touches spéciales
    if (scancode == 0x01) return 27;   // ESC
    if (scancode == 0x0E) return '\b'; // Backspace
    if (scancode == 0x0F) return '\t'; // Tab
    if (scancode == 0x1C) return '\n'; // Enter
    if (scancode == 0x48) return 16;   // ↑
    if (scancode == 0x50) return 14;   // ↓
    if (scancode == 0x4B) return 17;   // ←
    if (scancode == 0x4D) return 18;   // →
    if (scancode == 0x49) return 11;   // Page Up   (code 11)
    if (scancode == 0x51) return 12;   // Page Down (code 12)
    if (scancode == 0x47) return 2;    // Home
    if (scancode == 0x4F) return 3;    // End

    // Caractères normaux
    if (scancode < 128) {
        char c = shift_pressed ? (char)keyboard_map_shift[scancode]
                               : (char)keyboard_map[scancode];
        if (ctrl_pressed && c) {
            ctrl_pressed = 0;
            // Ctrl+lettre → codes de contrôle normalisés
            char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            if (lc == 'c') return 3;   // Ctrl+C → ETX
            if (lc == 's') return 19;  // Ctrl+S → DC3 (sauvegarder)
            if (lc == 'n') return 14;  // Ctrl+N → SO  (nouveau) — MÊME code que flèche bas
            // Note : Ctrl+N = 14 = flèche bas par convention POSIX.
            // Les apps qui ont besoin de distinguer utilisent 29 via leur propre logique.
            if (lc == 'q') return 17;  // Ctrl+Q → DC1
            if (lc == 'z') return 26;  // Ctrl+Z → SUB
            if (lc >= 'a' && lc <= 'z') return (char)(lc - 'a' + 1);
            return 0;
        }
        return c;
    }
    return 0;
}

// NON-BLOQUANT : retourne 0 immédiatement si aucune donnée disponible.
// Traite les paquets souris au passage via le callback enregistré.
// C'est le SEUL point d'entrée vers le port 0x60 en mode UI.
char input_poll_char(void) {
    uint8_t st;
    __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));

    if (!(st & STATUS_OUTPUT_FULL)) {
        // Aucune donnée — si on était en milieu de paquet, incrémenter le guard
        if (mouse_in_packet()) {
            g_mouse_pkt_guard++;
            if (g_mouse_pkt_guard >= MOUSE_PKT_GUARD_MAX) {
                // Paquet bloqué → forcer un reset via mouse_poll_reset()
                extern void mouse_reset_packet(void);
                mouse_reset_packet();
                g_mouse_pkt_guard = 0;
            }
        } else {
            g_mouse_pkt_guard = 0;
        }
        return 0;
    }

    // Donnée disponible — décider si c'est clavier ou souris :
    // Priorité 1 : on est déjà en cours de réception d'un paquet souris
    if (mouse_in_packet()) {
        g_mouse_pkt_guard = 0;
        if (mouse_poll()) on_mouse_packet_complete();
        return 0;
    }

    // Priorité 2 : le bit5 dit explicitement que c'est de la souris
    if (st & STATUS_MOUSE_DATA) {
        if (mouse_poll()) on_mouse_packet_complete();
        return 0;
    }

    // Le bit5=0 ET on n'est pas en milieu de paquet → c'est du clavier.
    // Mais on vérifie quand même le premier octet : si bit3=1, c'est suspect
    // (pourrait être un début de paquet souris que bit5 a raté).
    uint8_t scancode;
    __asm__ __volatile__("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));

    return process_keyboard_scancode(scancode);
}

// BLOQUANT : boucle jusqu'à avoir un vrai caractère clavier.
char input_dispatch_char(void) {
    while (1) {
        char c = input_poll_char();
        if (c) return c;
        // Petite pause pour ne pas saturer le bus
        __asm__ __volatile__("nop");
    }
}

// keyboard_get_char : bloque jusqu'à un vrai caractère (ignore events souris)
char keyboard_get_char(void) {
    while (1) {
        char c = input_dispatch_char();
        if (c) return c;
    }
}