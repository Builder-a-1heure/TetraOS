// terminal.c — Terminal fenêtré TetraOS
//
// Responsabilités :
//   - Buffer texte interne (g_term[TERM_LINES][TERM_COLS])
//   - Écriture dans ce buffer (terminal_putchar / terminal_puts)
//   - Lecture clavier + gestion souris en parallèle (terminal_getchar)
//   - Boucle shell interactive (terminal_run)
//
// Le RENDU visuel (pixels) est fait dans desktop.c qui lit g_term[]
// et appelle les primitives gfx_*. Ce fichier ne touche JAMAIS au
// framebuffer directement.

#include "terminal.h"
#include "shell.h"
#include "../gfx/screen.h"
#include "../drivers/input.h"
#include "../drivers/mouse.h"
#include "../drivers/vesa.h"
#include "../ui/session.h"
#include "../fs/fs.h"
#include "../lib/utils.h"
#include <stdint.h>

// ============================================================
// Buffer global (lu par desktop.c pour le rendu)
// ============================================================
TermCell  g_term[TERM_LINES][TERM_COLS];
int       g_term_row     = 0;
int       g_term_col     = 0;
int       g_term_view    = 0;
int       g_term_cols_vis = 0;
int       g_term_rows_vis = 0;
uint32_t  g_term_fg      = 0x00CCDDFF;
uint32_t  g_term_bg      = 0x00050508;

// Couleurs "par défaut" (installées à l'init, utilisées pour term_clear_line)
static uint32_t s_default_fg = 0x00CCDDFF;
static uint32_t s_default_bg = 0x00050508;

// Hook souris : desktop.c peut installer un callback pour gérer
// les événements souris pendant terminal_getchar()
static TermMouseHandlerFn s_mouse_handler = 0;

// ============================================================
// Buffer interne — helpers privés
// ============================================================
static void term_clear_line(int row) {
    for (int c = 0; c < TERM_COLS; c++) {
        g_term[row][c].ch = ' ';
        g_term[row][c].fg = s_default_fg;
        g_term[row][c].bg = s_default_bg;
    }
}

static void term_newline(void) {
    g_term_row++;
    if (g_term_row >= TERM_LINES) {
        // Scroll : décale tout d'une ligne vers le haut
        for (int r = 0; r < TERM_LINES - 1; r++)
            for (int c = 0; c < TERM_COLS; c++)
                g_term[r][c] = g_term[r + 1][c];
        g_term_row = TERM_LINES - 1;
        if (g_term_view > 0) g_term_view--;
    }
    term_clear_line(g_term_row);
    g_term_col = 0;
    // Garder la vue sur la dernière ligne
    int bottom = g_term_row - g_term_rows_vis + 1;
    if (bottom < 0) bottom = 0;
    if (g_term_view < bottom) g_term_view = bottom;
}

// ============================================================
// API publique — Init
// ============================================================
void terminal_init(uint32_t fg_default, uint32_t bg_default) {
    s_default_fg = fg_default;
    s_default_bg = bg_default;
    g_term_fg    = fg_default;
    g_term_bg    = bg_default;
    g_term_row   = 0;
    g_term_col   = 0;
    g_term_view  = 0;
    for (int r = 0; r < TERM_LINES; r++) term_clear_line(r);
}

// ============================================================
// API publique — Écriture
// ============================================================
void terminal_putchar(char c) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { g_term_col = 0; return; }
    if (c == '\b') {
        if (g_term_col > 0) {
            g_term_col--;
            g_term[g_term_row][g_term_col].ch = ' ';
            g_term[g_term_row][g_term_col].fg = s_default_fg;
            g_term[g_term_row][g_term_col].bg = s_default_bg;
        }
        return;
    }
    if (g_term_col >= TERM_COLS) term_newline();
    g_term[g_term_row][g_term_col].ch = c;
    g_term[g_term_row][g_term_col].fg = g_term_fg;
    g_term[g_term_row][g_term_col].bg = g_term_bg;
    g_term_col++;
    // Avancer la vue si on écrit sous la zone visible
    int bottom = g_term_row - g_term_rows_vis + 1;
    if (bottom < 0) bottom = 0;
    if (g_term_view < bottom) g_term_view = bottom;
}

void terminal_puts(const char* s) {
    while (*s) terminal_putchar(*s++);
}

// ============================================================
// API publique — Scroll
// ============================================================
void terminal_scroll_up(void) {
    if (g_term_view > 0) g_term_view--;
}

void terminal_scroll_down(void) {
    int bot = g_term_row - g_term_rows_vis + 1;
    if (bot < 0) bot = 0;
    if (g_term_view < bot) g_term_view++;
}

// ============================================================
// API publique — Hook souris
// ============================================================
void terminal_set_mouse_handler(TermMouseHandlerFn fn) {
    s_mouse_handler = fn;
}

// ============================================================
// API publique — Lecture clavier (avec gestion souris en parallèle)
// ============================================================
char terminal_getchar(void) {
    while (1) {
        uint8_t st;
        __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));

        if (!(st & 1)) {
            __asm__ __volatile__("nop");
            continue;
        }

        if (st & (1 << 5)) {
            // Données souris : consommer le paquet
            if (mouse_poll() && s_mouse_handler) {
                s_mouse_handler();
            }
            continue;
        }

        // Données clavier
        char c = input_dispatch_char();
        if (c == 0) continue;

        // Flèches haut/bas → scroll, pas de caractère retourné
        if (c == 16) { terminal_scroll_up();   return 0; }
        if (c == 14) { terminal_scroll_down();  return 0; }

        return c;
    }
}

// ============================================================
// Prompt interne
// ============================================================
static void print_prompt(void) {
    // Couleur prompt
    uint32_t saved_fg = g_term_fg;
    g_term_fg = 0x0033CCFF;  // WIN_PROMPT

    terminal_puts(session_get_current_name());
    terminal_puts("@TetraOS:");

    // Chemin courant (reconstruit depuis g_cwd → racine)
    char parts[16][32];
    int depth = 0;
    uint32_t cur = g_cwd;
    while (cur != 0 && depth < 16) {
        memset(parts[depth], 0, 32);
        strncpy(parts[depth], g_fs.nodes[cur].name, 31);
        cur = g_fs.nodes[cur].parent;
        depth++;
    }
    terminal_putchar('/');
    for (int i = depth - 1; i >= 0; i--) {
        terminal_puts(parts[i]);
        if (i > 0) terminal_putchar('/');
    }
    terminal_puts(session_is_admin() ? "# " : "$ ");

    g_term_fg = saved_fg;
}

// ============================================================
// API publique — Boucle shell
// ============================================================

// Redraw callback : installé par desktop.c via terminal_set_mouse_handler,
// mais on a aussi besoin de redraw après chaque putchar. Desktop installe
// une fonction de redraw que le terminal appelle après écriture.
static void (*s_redraw_fn)(void) = 0;

void terminal_set_redraw_fn(void (*fn)(void)) {
    s_redraw_fn = fn;
}

static void do_redraw(void) {
    if (s_redraw_fn) s_redraw_fn();
}

void terminal_run(void) {
    // Installer le hook de sortie pour que print_string/print_char
    // aillent dans le buffer terminal au lieu du scrollback VGA
    g_print_hook = terminal_putchar;

    // Bannière
    terminal_puts("  TetraOS Terminal\n");
    terminal_puts("  Tapez 'sys.help' pour l'aide\n\n");
    do_redraw();

    char input[256];

    while (g_session_manager.logged_in) {
        print_prompt();
        do_redraw();

        // ── Saisie d'une ligne ─────────────────────────────
        int idx = 0;
        input[0] = '\0';

        while (1) {
            char c = terminal_getchar();

            // Scroll (retourné comme 0 par terminal_getchar)
            if (c == 0) { do_redraw(); continue; }

            if (c == '\n' || c == '\r') {
                input[idx] = '\0';
                terminal_putchar('\n');
                do_redraw();
                break;
            }
            if ((c == '\b' || c == 127) && idx > 0) {
                idx--;
                input[idx] = '\0';
                terminal_putchar('\b');
                do_redraw();
                continue;
            }
            if (c == 27) {  // ESC = Ctrl+C
                input[0] = '\0';
                idx = 0;
                terminal_puts("^C\n");
                do_redraw();
                break;
            }
            if (c >= 32 && c <= 126 && idx < 255) {
                // Si on scrollait, revenir en bas avant de taper
                int bot = g_term_row - g_term_rows_vis + 1;
                if (bot < 0) bot = 0;
                g_term_view = bot;

                input[idx++] = c;
                terminal_putchar(c);
                do_redraw();
            }
        }

        // ── Dispatch de la commande ────────────────────────
        const char* s = input;
        while (*s == ' ') s++;
        if (strlen(s) == 0) continue;

        char mod[32], cmd[64], args[192];
        if (shell_parse(s, mod, sizeof(mod), cmd, sizeof(cmd),
                            args, sizeof(args))) {
            shell_dispatch(mod, cmd, args);
        } else if (strcmp(s, "sl") == 0) {
            cmd_sl();
        } else {
            terminal_puts("commande inconnue. Tapez 'sys.help'\n");
        }
        do_redraw();
    }

    // Désinstaller le hook de sortie
    g_print_hook = 0;
}
