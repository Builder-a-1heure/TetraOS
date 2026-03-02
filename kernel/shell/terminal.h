#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

// ============================================================
// terminal.h — Terminal fenêtré TetraOS
//
// Le terminal est un buffer texte interne (TERM_LINES × TERM_COLS)
// rendu dans une fenêtre graphique par desktop.c.
//
// Flux d'utilisation depuis desktop.c :
//   1. terminal_init(win_x, win_y, win_w, win_h)  ← après ouverture fenêtre
//   2. terminal_run()                              ← boucle shell bloquante
//   3. (retour automatique sur logout ou fermeture)
// ============================================================

// Dimensions du buffer interne
#define TERM_LINES   500
#define TERM_COLS    200

// Couleurs par défaut (définies dans desktop.c, passées à l'init)
typedef struct {
    char     ch;
    uint32_t fg;
    uint32_t bg;
} TermCell;

// Buffer accessible depuis desktop.c pour le rendu
extern TermCell  g_term[TERM_LINES][TERM_COLS];
extern int       g_term_row;        // ligne d'écriture courante
extern int       g_term_col;        // colonne courante
extern int       g_term_view;       // première ligne visible
extern int       g_term_cols_vis;   // colonnes visibles (calculé par desktop)
extern int       g_term_rows_vis;   // lignes visibles   (calculé par desktop)
extern uint32_t  g_term_fg;         // couleur fg courante
extern uint32_t  g_term_bg;         // couleur bg courante

// ── Init / reset ──────────────────────────────────────────────
// Vide le buffer et réinitialise curseur + vue.
// fg_default / bg_default : couleurs de fond des cellules vides.
void terminal_init(uint32_t fg_default, uint32_t bg_default);

// ── Écriture dans le buffer ───────────────────────────────────
// Appelées aussi depuis les dispatchers shell via g_print_hook.
void terminal_putchar(char c);
void terminal_puts(const char* s);

// ── Lecture depuis le clavier ─────────────────────────────────
// Bloquant. Gère aussi les événements souris (drag, hover) pendant l'attente.
// desktop.c installe un callback pour les événements souris :
//   terminal_set_mouse_handler(fn)
// La fonction fn est appelée par terminal_getchar() à chaque événement souris.
typedef void (*TermMouseHandlerFn)(void);
void terminal_set_mouse_handler(TermMouseHandlerFn fn);
char terminal_getchar(void);

// ── Boucle principale du shell ────────────────────────────────
// Lance le shell interactif dans le terminal. Bloquant jusqu'au logout
// ou jusqu'à fermeture de la fenêtre (g_session_manager.logged_in = 0).
// Installe / désinstalle g_print_hook automatiquement.
void terminal_run(void);

// ── Scroll ────────────────────────────────────────────────────
void terminal_scroll_up(void);
void terminal_scroll_down(void);

// ── Callback de redraw ────────────────────────────────────────
// desktop.c installe sa fonction de redraw pour que le terminal
// puisse demander un rafraîchissement visuel après chaque écriture.
void terminal_set_redraw_fn(void (*fn)(void));

#endif // TERMINAL_H
