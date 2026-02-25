// desktop.c — Bureau graphique TetraOS
// Architecture :
//   desktop_run()
//     └─ boucle principale (fond + icônes + souris)
//         └─ sur clic icône Terminal → terminal_run() [fenêtre modale]
//             └─ tetra_shell() interne avec rendu dans la fenêtre
//
// Tout le dessin passe par les primitives gfx_* de screen.c (pixels VESA).
// Le "scrollback" du terminal fenêtré est un buffer local indépendant
// du scrollback global de screen.c.

#include "desktop.h"
#include "screen.h"
#include "vesa.h"
#include "mouse.h"
#include "input.h"
#include "session.h"
#include "fs.h"
#include "utils.h"
#include "tex.h"
#include <stdint.h>

// ============================================================
// ============================================================
// Palette bureau
// ============================================================
#define DT_BG_TOP       0x00000D2A   // bleu nuit
#define DT_BG_BOT       0x00000000   // noir
#define DT_TASKBAR_BG   0x00050510   // bande bas quasi-noire
#define DT_TASKBAR_LINE 0x00002244   // liseré bleu
#define DT_ICON_BG      0x00001133
#define DT_ICON_BORDER  0x000055CC
#define DT_ICON_HOVER   0x00003366
#define DT_ICON_TEXT    0x00FFFFFF
#define DT_CLOCK_FG     0x00AAAACC
#define DT_WHITE        0x00FFFFFF
#define DT_GRAY         0x00AAAAAA

// Fenêtre terminal
#define WIN_BG          0x00050508
#define WIN_TITLEBAR    0x00001133
#define WIN_TITLE_ACT   0x000033AA
#define WIN_BORDER      0x000055CC
#define WIN_BORDER_ACT  0x0000AAFF
#define WIN_TEXT_FG     0x00CCDDFF
#define WIN_TEXT_BG     0x00050508
#define WIN_CLOSE_BG    0x00AA1111
#define WIN_CLOSE_HOV   0x00FF3333
#define WIN_PROMPT      0x0033CCFF

// ============================================================
// Dimensions fenêtre terminal (relative à la résolution)
// ============================================================
#define WIN_MARGIN_X    80
#define WIN_MARGIN_Y    60
#define TITLEBAR_H      28
#define WIN_PAD         8    // padding interne texte

// ============================================================
// Buffer scrollback interne du terminal fenêtré
// ============================================================
#define TERM_LINES   500
#define TERM_COLS    200

typedef struct {
    char    ch;
    uint32_t fg;
    uint32_t bg;
} TermCell;

static TermCell  g_term[TERM_LINES][TERM_COLS];
static int       g_term_row = 0;   // ligne d'écriture courante
static int       g_term_col = 0;   // colonne d'écriture courante
static int       g_term_view = 0;  // première ligne visible
static int       g_term_cols_vis = 0;
static int       g_term_rows_vis = 0;

// Couleur courante d'écriture
static uint32_t  g_term_fg = WIN_TEXT_FG;
static uint32_t  g_term_bg = WIN_TEXT_BG;

// ============================================================
// Icône sur le bureau
// ============================================================
#define ICON_W   72
#define ICON_H   72
#define ICON_TXT_H 18

typedef struct {
    int x, y;         // position centre
    const char* label;
    int hovered;
} DesktopIcon;

// ============================================================
// Forward declarations (après les types)
// ============================================================
static void desktop_redraw_all(void);
static void term_redraw_content(void);
static void draw_background(void);
static void draw_taskbar(void);
static void draw_icon(DesktopIcon* ic);
static void draw_window(int focused);
static void draw_terminal_content(void);
static void win_compute_dims(void);
static void win_compute_term_size(void);
static void term_init(void);
static void term_putchar(char c);
static void term_puts(const char* s);
static char term_getchar(void);
static void run_terminal_shell(void);

static DesktopIcon g_icon_terminal;

// ============================================================
// État de la fenêtre terminal
// ============================================================
static int g_win_open   = 0;
static int g_win_x      = 0;
static int g_win_y      = 0;
static int g_win_w      = 0;
static int g_win_h      = 0;
static int g_win_dragging = 0;
static int g_drag_off_x = 0;
static int g_drag_off_y = 0;

// ============================================================
// Helpers I/O bas niveau (copie locale pour éviter dépendances)
// ============================================================
static inline uint8_t _inb(uint16_t port) {
    uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}

// ============================================================
// TERMINAL INTERNE — écriture dans le buffer
// ============================================================
static void term_clear_line(int row) {
    for (int c = 0; c < TERM_COLS; c++) {
        g_term[row][c].ch = ' ';
        g_term[row][c].fg = WIN_TEXT_FG;
        g_term[row][c].bg = WIN_TEXT_BG;
    }
}

static void term_init(void) {
    g_term_row  = 0;
    g_term_col  = 0;
    g_term_view = 0;
    for (int r = 0; r < TERM_LINES; r++) term_clear_line(r);
}

static void term_newline(void) {
    g_term_row++;
    if (g_term_row >= TERM_LINES) {
        // Scroll : décaler tout d'une ligne vers le haut
        for (int r = 0; r < TERM_LINES - 1; r++)
            for (int c = 0; c < TERM_COLS; c++)
                g_term[r][c] = g_term[r+1][c];
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

static void term_putchar(char c) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { g_term_col = 0; return; }
    if (c == '\b') {
        if (g_term_col > 0) {
            g_term_col--;
            g_term[g_term_row][g_term_col].ch = ' ';
        }
        return;
    }
    if (g_term_col >= TERM_COLS) term_newline();
    g_term[g_term_row][g_term_col].ch = c;
    g_term[g_term_row][g_term_col].fg = g_term_fg;
    g_term[g_term_row][g_term_col].bg = g_term_bg;
    g_term_col++;
    // Mise à jour vue
    int bottom = g_term_row - g_term_rows_vis + 1;
    if (bottom < 0) bottom = 0;
    if (g_term_view < bottom) g_term_view = bottom;
}

static void term_puts(const char* s) {
    while (*s) term_putchar(*s++);
}

// ============================================================
// DESSIN BUREAU
// ============================================================
static void draw_background(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    gfx_gradient_v(0, 0, (int)sw, (int)sh, DT_BG_TOP, DT_BG_BOT);
}

static void draw_taskbar(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    int tbh = 32;
    int tby = (int)sh - tbh;
    gfx_fill_rect(0, tby, (int)sw, tbh, DT_TASKBAR_BG);
    gfx_fill_rect(0, tby, (int)sw, 1, DT_TASKBAR_LINE);

    // Nom de session à gauche
    const char* uname = session_get_current_name();
    gfx_draw_text(10, tby + (tbh - FONT_H) / 2, uname, DT_CLOCK_FG, DT_TASKBAR_BG);

    // "TetraOS" au centre
    gfx_draw_text_centered(0, tby + (tbh - FONT_H) / 2, (int)sw,
                           "TetraOS Desktop", DT_WHITE, DT_TASKBAR_BG);
}

// Icône terminal : petit rectangle avec ">" dedans
static void draw_icon(DesktopIcon* ic) {
    int x = ic->x - ICON_W/2;
    int y = ic->y - ICON_H/2;
    uint32_t bg  = ic->hovered ? DT_ICON_HOVER : DT_ICON_BG;
    uint32_t bdr = ic->hovered ? WIN_BORDER_ACT : DT_ICON_BORDER;

    gfx_fill_rect(x, y, ICON_W, ICON_H, bg);
    gfx_draw_rect(x, y, ICON_W, ICON_H, bdr);

    // Dessin stylisé d'un terminal : fond noir avec "$ _"
    int inner_x = x + 6, inner_y = y + 8;
    int inner_w = ICON_W - 12, inner_h = ICON_H - 22;
    gfx_fill_rect(inner_x, inner_y, inner_w, inner_h, 0x00000000);
    gfx_draw_rect(inner_x, inner_y, inner_w, inner_h, DT_ICON_BORDER);
    // Barre titre du mini terminal
    gfx_fill_rect(inner_x+1, inner_y+1, inner_w-2, 5, DT_ICON_BORDER);
    // Texte "> _"
    gfx_draw_text(inner_x + 3, inner_y + 8,  ">",  WIN_PROMPT, 0x00000000);
    gfx_draw_text(inner_x + 3, inner_y + 18, "_",  WIN_TEXT_FG, 0x00000000);

    // Label sous l'icône
    gfx_draw_text_centered(x, y + ICON_H + 3, ICON_W,
                           ic->label, DT_ICON_TEXT, DT_BG_BOT);
}

static int icon_hit(DesktopIcon* ic, int mx, int my) {
    return mx >= ic->x - ICON_W/2 && mx <= ic->x + ICON_W/2 &&
           my >= ic->y - ICON_H/2 && my <= ic->y + ICON_H/2;
}

// ============================================================
// DESSIN FENÊTRE TERMINAL
// ============================================================
static void win_compute_dims(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    g_win_x = WIN_MARGIN_X;
    g_win_y = WIN_MARGIN_Y;
    g_win_w = (int)sw - WIN_MARGIN_X * 2;
    g_win_h = (int)sh - WIN_MARGIN_Y * 2 - 32; // 32 = taskbar
}

static void win_compute_term_size(void) {
    // Zone texte = fenêtre moins titlebar moins padding
    int text_w = g_win_w - WIN_PAD * 2;
    int text_h = g_win_h - TITLEBAR_H - WIN_PAD * 2;
    g_term_cols_vis = text_w / FONT_W;
    g_term_rows_vis = text_h / FONT_H;
    if (g_term_cols_vis > TERM_COLS) g_term_cols_vis = TERM_COLS;
    if (g_term_cols_vis < 1) g_term_cols_vis = 1;
    if (g_term_rows_vis < 1) g_term_rows_vis = 1;
}

static void draw_window(int focused) {
    uint32_t titlebar_color = focused ? WIN_TITLE_ACT : WIN_TITLEBAR;
    uint32_t border_color   = focused ? WIN_BORDER_ACT : WIN_BORDER;

    // Ombre portée (3px décalée)
    gfx_fill_rect(g_win_x + 4, g_win_y + 4, g_win_w, g_win_h, 0x00000000);

    // Fond fenêtre
    gfx_fill_rect(g_win_x, g_win_y, g_win_w, g_win_h, WIN_BG);
    gfx_draw_rect(g_win_x, g_win_y, g_win_w, g_win_h, border_color);

    // Titlebar
    gfx_fill_rect(g_win_x + 1, g_win_y + 1, g_win_w - 2, TITLEBAR_H, titlebar_color);

    // Titre
    gfx_draw_text_centered(g_win_x, g_win_y + (TITLEBAR_H - FONT_H) / 2, g_win_w,
                           "Terminal", DT_WHITE, titlebar_color);

    // Bouton fermer [X]
    int bx = g_win_x + g_win_w - 24;
    int by = g_win_y + (TITLEBAR_H - 16) / 2;
    uint32_t close_bg = (g_mouse.x >= bx && g_mouse.x <= bx+16 &&
                         g_mouse.y >= by && g_mouse.y <= by+16)
                        ? WIN_CLOSE_HOV : WIN_CLOSE_BG;
    gfx_fill_rect(bx, by, 16, 16, close_bg);
    gfx_draw_rect(bx, by, 16, 16, DT_WHITE);
    gfx_draw_text(bx + 4, by + 2, "X", DT_WHITE, close_bg);

    // Séparateur titlebar / contenu
    gfx_fill_rect(g_win_x + 1, g_win_y + TITLEBAR_H, g_win_w - 2, 1, border_color);
}

static void draw_terminal_content(void) {
    int tx = g_win_x + WIN_PAD;
    int ty = g_win_y + TITLEBAR_H + WIN_PAD;

    // Fond zone texte
    gfx_fill_rect(tx, ty, g_win_w - WIN_PAD*2, g_win_h - TITLEBAR_H - WIN_PAD*2, WIN_BG);

    // Lignes visibles
    for (int row = 0; row < g_term_rows_vis; row++) {
        int src = g_term_view + row;
        if (src < 0 || src >= TERM_LINES) continue;
        for (int col = 0; col < g_term_cols_vis; col++) {
            char ch = g_term[src][col].ch;
            uint32_t fg = g_term[src][col].fg;
            uint32_t bg = g_term[src][col].bg;
            int px = tx + col * FONT_W;
            int py = ty + row * FONT_H;
            if (ch != ' ' || bg != WIN_BG)
                vesa_draw_glyph(px, py, ch, fg, bg);
        }
    }

    // Curseur
    int cur_screen_row = g_term_row - g_term_view;
    if (cur_screen_row >= 0 && cur_screen_row < g_term_rows_vis) {
        int px = tx + g_term_col * FONT_W;
        int py = ty + cur_screen_row * FONT_H + FONT_H - 2;
        gfx_fill_rect(px, py, FONT_W, 2, WIN_BORDER_ACT);
    }
}

// Vrai si le clic est sur la titlebar (hors bouton fermer)
static int hit_titlebar(int mx, int my) {
    return mx >= g_win_x && mx <= g_win_x + g_win_w - 28 &&
           my >= g_win_y && my <= g_win_y + TITLEBAR_H;
}

static int hit_close(int mx, int my) {
    int bx = g_win_x + g_win_w - 24;
    int by = g_win_y + (TITLEBAR_H - 16) / 2;
    return mx >= bx && mx <= bx+16 && my >= by && my <= by+16;
}

// ============================================================
// RENDU COMPLET DU BUREAU (ou bureau + fenêtre)
// ============================================================
static void desktop_redraw_all(void) {
    screen_begin_ui();
    draw_background();
    draw_taskbar();
    if (!g_win_open) {
        g_icon_terminal.hovered =
            icon_hit(&g_icon_terminal, g_mouse.x, g_mouse.y);
        draw_icon(&g_icon_terminal);
    }
    if (g_win_open) {
        draw_window(1);
        draw_terminal_content();
    }
    screen_end_ui(); // curseur souris dessiné automatiquement par le hook
}

// Redessine uniquement le contenu texte (sans redessiner fond/titlebar)
static void term_redraw_content(void) {
    screen_begin_ui();
    draw_terminal_content();
    screen_end_ui(); // curseur souris dessiné automatiquement par le hook
}

// ============================================================
// REDIRECTEUR print_* VERS LE BUFFER TERMINAL
// Toutes les fonctions de la session/shell utilisent print_string/print_char
// On les hook via des fonctions wrapper — le shell sera appelé avec
// g_term_output_active = 1, et les sorties iront dans term_putchar.
// ============================================================

// Ces fonctions sont définies dans screen.c comme globales.
// On redéfinit leur comportement en remplaçant temporairement
// la sortie via un flag + un hook.
//
// IMPORTANT : on ne peut pas vraiment "hooker" les fonctions C sans
// modifier screen.c. On va plutôt dupliquer la boucle shell ici
// en redirigeant explicitement toute sortie vers term_puts/term_putchar.
// La solution propre : screen.c expose un "output handler" qu'on peut
// rediriger.

// ============================================================
// REDIRECTEUR print_* VERS LE BUFFER TERMINAL
// screen.c expose g_print_hook (CharOutputFn). Quand il est non-NULL,
// print_char/print_string/print_int/etc. appellent g_print_hook(c)
// au lieu d'écrire dans le scrollback normal.
// On l'installe au début de run_terminal_shell() et on le retire à la fin.
// ============================================================

// Imports des dispatchers depuis main.c
extern void dispatch_session(const char* cmd, const char* args);
extern void dispatch_sys(const char* cmd, const char* args);
extern void dispatch_tex(const char* cmd, const char* args);
extern void dispatch_ray64(const char* cmd, const char* args);
extern void cmd_sl(void);

// On a besoin d'intercepter print_string/print_char → g_print_hook dans screen.c
extern CharOutputFn g_print_hook;  // défini dans screen.c / screen.h

// Lecture d'un caractère pendant que le terminal est ouvert
// = polling souris + clavier, avec gestion drag fenêtre
static char term_getchar(void) {
    while (1) {
        // ── Poll souris (direct, sans pré-check bit5) ──────
        if (mouse_poll()) {
            if (g_win_dragging && g_mouse.btn_left) {
                g_win_x = g_mouse.x - g_drag_off_x;
                g_win_y = g_mouse.y - g_drag_off_y;
                uint32_t sw = vesa_width(), sh = vesa_height();
                if (g_win_x < 0) g_win_x = 0;
                if (g_win_y < 0) g_win_y = 0;
                if (g_win_x + g_win_w > (int)sw) g_win_x = (int)sw - g_win_w;
                if (g_win_y + g_win_h > (int)sh - 32) g_win_y = (int)sh - 32 - g_win_h;
                mouse_erase_cursor();
                desktop_redraw_all();
                mouse_draw_cursor();
            } else {
                if (!g_mouse.btn_left) g_win_dragging = 0;
                // Redessiner titlebar (couleur hover bouton fermer)
                mouse_erase_cursor();
                screen_begin_ui();
                draw_window(1);
                screen_end_ui();
                mouse_draw_cursor();
            }
            continue; // souris consommée, retour au début
        }

        // ── Poll clavier (non-bloquant) ─────────────────────
        uint8_t st = _inb(0x64);
        if (!(st & 1)) {
            // Rien à lire du tout — petit yield
            __asm__ __volatile__("nop");
            continue;
        }
        if (st & (1 << 5)) {
            // Bit 5 set mais mouse_poll() n'a rien pris → données souris
            // résiduelles, on les consomme silencieusement
            (void)_inb(0x60);
            continue;
        }

        // Donnée clavier disponible
        extern char keyboard_get_char(void);
        CharOutputFn saved = g_print_hook;
        g_print_hook = 0;
        char c = keyboard_get_char();
        g_print_hook = saved;

        // Scroll terminal avec flèches haut/bas
        if (c == 16) { // flèche haut
            if (g_term_view > 0) { g_term_view--; term_redraw_content(); }
            continue;
        }
        if (c == 14) { // flèche bas
            int bot = g_term_row - g_term_rows_vis + 1;
            if (bot < 0) bot = 0;
            if (g_term_view < bot) { g_term_view++; term_redraw_content(); }
            continue;
        }
        return c;
    }
}

static void run_terminal_shell(void) {
    // Installer le hook de sortie
    g_print_hook = term_putchar;

    // Bannière
    term_puts("  TetraOS Terminal\n");
    term_puts("  Tapez 'sys.help' pour l'aide\n\n");
    term_redraw_content();

    char input[256];

    while (g_session_manager.logged_in) {
        // Prompt
        g_term_fg = WIN_PROMPT;
        term_puts(session_get_current_name());
        term_puts("@TetraOS:");
        // Chemin courant
        extern FSTable g_fs;
        extern uint32_t g_cwd;
        char parts[16][32];
        int depth = 0;
        uint32_t cur = g_cwd;
        while (cur != 0 && depth < 16) {
            memset(parts[depth], 0, 32);
            strncpy(parts[depth], g_fs.nodes[cur].name, 31);
            cur = g_fs.nodes[cur].parent;
            depth++;
        }
        term_putchar('/');
        for (int i = depth-1; i >= 0; i--) {
            term_puts(parts[i]);
            if (i > 0) term_putchar('/');
        }
        term_puts(session_is_admin() ? "# " : "$ ");
        g_term_fg = WIN_TEXT_FG;
        term_redraw_content();

        // Saisie
        int idx = 0;
        input[0] = '\0';
        while (1) {
            char c = term_getchar();
            if (c == '\n' || c == '\r') {
                input[idx] = '\0';
                term_putchar('\n');
                term_redraw_content();
                break;
            } else if ((c == '\b' || c == 127) && idx > 0) {
                idx--;
                input[idx] = '\0';
                term_putchar('\b');
                term_redraw_content();
            } else if (c == 27) {
                input[0] = '\0'; idx = 0;
                term_puts("^C\n");
                term_redraw_content();
                break;
            } else if (c >= 32 && c <= 126 && idx < 255) {
                // Revenir en bas si on scrollait
                int bot = g_term_row - g_term_rows_vis + 1;
                if (bot < 0) bot = 0;
                g_term_view = bot;
                input[idx++] = c;
                term_putchar(c);
                term_redraw_content();
            }
        }

        const char* s = input;
        while (*s == ' ') s++;
        if (strlen(s) == 0) continue;

        // Parser namespace.commande args
        char mod[32], cmd[64], args[192];
        // parse_namespaced est dans main.c, on la redéclare
        extern int parse_namespaced(const char* input,
                                    char* mod_out, int mod_max,
                                    char* cmd_out, int cmd_max,
                                    char* args_out, int args_max);

        if (parse_namespaced(s, mod, sizeof(mod), cmd, sizeof(cmd), args, sizeof(args))) {
            if      (strcmp(mod, "ray64")   == 0) dispatch_ray64(cmd, args);
            else if (strcmp(mod, "session") == 0) dispatch_session(cmd, args);
            else if (strcmp(mod, "sys")     == 0) dispatch_sys(cmd, args);
            else if (strcmp(mod, "tex")     == 0) dispatch_tex(cmd, args);
            else {
                term_puts("module inconnu '");
                term_puts(mod);
                term_puts("' - modules: ray64, session, sys, tex\n");
            }
        } else if (strcmp(s, "sl") == 0) {
            cmd_sl();
        } else {
            term_puts("commande inconnue. Tapez 'sys.help'\n");
        }
        term_redraw_content();
    }

    // Désinstaller le hook
    g_print_hook = 0;
}

// ============================================================
// BOUCLE PRINCIPALE DU BUREAU
// ============================================================

// Clique gauche précédent (pour détecter le rising edge)
static int g_prev_left = 0;

void desktop_run(void) {
    // Plus de hooks — la boucle gère le curseur directement

    win_compute_dims();
    win_compute_term_size();
    term_init();

    uint32_t sw = vesa_width(), sh = vesa_height();
    g_icon_terminal.x     = 80;
    g_icon_terminal.y     = (int)sh / 2 - 20;
    g_icon_terminal.label = "Terminal";
    g_icon_terminal.hovered = 0;

    // Premier dessin du bureau + curseur initial
    desktop_redraw_all();
    mouse_draw_cursor();

    while (g_session_manager.logged_in) {

        // ── 1. Poll souris — appel direct, mouse_poll() vérifie lui-même
        //       le status. On vide tous les octets disponibles.
        int mouse_moved = 0;
        while (mouse_poll()) mouse_moved = 1;

        // ── 2. Mise à jour curseur souris IMMÉDIATE ───────────
        // Si la souris a bougé : on efface l'ancienne position et
        // on redessine à la nouvelle, directement dans le framebuffer.
        // C'est la seule opération nécessaire pour un mouvement simple.
        if (mouse_moved) {
            mouse_erase_cursor();   // restaure les pixels d'arrière-plan
            mouse_draw_cursor();    // sauvegarde le nouveau fond + dessine la flèche
        }

        int cur_left  = g_mouse.btn_left;
        int prev_left = g_prev_left;

        // ── 3. Événements clic / hover ────────────────────────
        if (mouse_moved || cur_left != prev_left) {

            if (g_win_open) {
                // Début drag titlebar
                if (cur_left && !prev_left && hit_titlebar(g_mouse.x, g_mouse.y)) {
                    g_win_dragging = 1;
                    g_drag_off_x = g_mouse.x - g_win_x;
                    g_drag_off_y = g_mouse.y - g_win_y;
                }
                if (!cur_left) g_win_dragging = 0;

                // Drag en cours → redraw complet de la fenêtre
                if (g_win_dragging && cur_left) {
                    g_win_x = g_mouse.x - g_drag_off_x;
                    g_win_y = g_mouse.y - g_drag_off_y;
                    if (g_win_x < 0) g_win_x = 0;
                    if (g_win_y < 0) g_win_y = 0;
                    if (g_win_x + g_win_w > (int)sw) g_win_x = (int)sw - g_win_w;
                    if (g_win_y + g_win_h > (int)sh - 32) g_win_y = (int)sh - 32 - g_win_h;
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                }

                // Hover bouton fermer → redessiner titlebar
                if (mouse_moved) {
                    mouse_erase_cursor();
                    screen_begin_ui();
                    draw_window(1);
                    screen_end_ui();
                    mouse_draw_cursor();
                }

                // Clic [X] → fermer
                if (cur_left && !prev_left && hit_close(g_mouse.x, g_mouse.y)) {
                    g_win_open = 0;
                    g_print_hook = 0;
                    term_init();
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                }

            } else {
                // Bureau : hover icône
                int hov = icon_hit(&g_icon_terminal, g_mouse.x, g_mouse.y);

                if (hov != g_icon_terminal.hovered) {
                    g_icon_terminal.hovered = hov;
                    mouse_erase_cursor();
                    screen_begin_ui();
                    draw_background();
                    draw_taskbar();
                    draw_icon(&g_icon_terminal);
                    screen_end_ui();
                    mouse_draw_cursor();
                }

                // Clic sur l'icône → ouvrir le terminal
                if (cur_left && !prev_left && hov) {
                    g_win_open = 1;
                    win_compute_dims();
                    win_compute_term_size();
                    term_init();
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                    run_terminal_shell();
                    if (!g_session_manager.logged_in) break;
                    g_win_open = 0;
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                }
            }
        }

        g_prev_left = cur_left;
    }

    // Nettoyage
    screen_exit_ui();
}