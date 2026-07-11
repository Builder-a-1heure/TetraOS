// apps/terminal.c — Application Terminal TetraOS
//
// App auto-contenue : buffer texte, chrome fenêtre, boucle shell.
// Elle se déclare sur le bureau via TEX_APP — desktop.c n'a pas
// besoin de la connaître à la compilation.
//
// Tout ce qui était éparpillé entre shell/terminal.c (buffer + boucle)
// et ui/desktop.c (chrome fenêtre) est ici réuni. La séparation reste
// claire en interne via des sections commentées.
//
// Signature TEX :
//   TEX_APP place un TexHeader de 32 octets en .rodata avant app_terminal().
//   desktop.c retrouve ce header en scannant la mémoire kernel.
//   app_terminal() est le point d'entrée unique appelé au clic — c'est
//   ici que viendra se greffer la vérification de signature crypto
//   (champ reserved du TexHeader) pour les apps APP_FLAG_SYSTEM.
//
// Dépendances :
//   gfx/screen.h, drivers/vesa.h, drivers/mouse.h, drivers/input.h
//   ui/session.h, fs/fs.h, shell/shell.h, lib/utils.h
//   lib/global.h (g_print_hook)
//   apps/app.h   (TEX_APP, TexHeader)

#include "app.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../ui/session.h"
#include "../fs/fs.h"
#include "../shell/shell.h"
#include "../lib/utils.h"
#include "../lib/global.h"
#include <stdint.h>

// ============================================================
// ── SIGNATURE TEX ────────────────────────────────────────────
//
// Cette ligne est tout ce qu'il faut pour apparaître sur le bureau.
// Le TexHeader généré contient :
//   magic     = 0x54455800
//   name      = "Terminal"
//   icon_type = APPICON_TERMINAL
//   flags     = APP_FLAG_DESKTOP | APP_FLAG_SYSTEM
//   entry     = &app_terminal   ← résolu au link
//   reserved  = 0               ← futur : hash SHA / signature ECDSA
//
// APP_FLAG_SYSTEM signale que cette app devra être vérifiée
// cryptographiquement quand le système de signature sera implémenté.
// ─────────────────────────────────────────────────────────────
TEX_APP("Terminal", APPICON_TERMINAL, 1, 0,
        APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, app_terminal);

// ============================================================
// ── PALETTE FENÊTRE ──────────────────────────────────────────
// ============================================================
#define WIN_BG          0x00050508
#define WIN_TITLEBAR    0x00001133
#define WIN_TITLE_ACT   0x000033AA
#define WIN_BORDER_ACT  0x0000AAFF
#define WIN_TEXT_FG     0x00CCDDFF
#define WIN_TEXT_BG     0x00050508
#define WIN_CLOSE_BG    0x00AA1111
#define WIN_CLOSE_HOV   0x00FF3333
#define WIN_PROMPT_COL  0x0033CCFF
#define WIN_EDGE_LIGHT  0x00C8E8FF
#define WIN_EDGE_DIM    0x006888B0
#define WIN_SEP_LINE    0x00406088
#define WIN_WHITE       0x00FFFFFF

#define WIN_MARGIN_X    80
#define WIN_MARGIN_Y    60
#define TITLEBAR_H      28
#define WIN_PAD         8
#define WIN_INNER       2
#define WIN_DRAG_BORDER 6

// ============================================================
// ── BUFFER TEXTE ─────────────────────────────────────────────
// ============================================================
#define TERM_LINES   500
#define TERM_COLS    200

typedef struct { char ch; uint32_t fg; uint32_t bg; } TermCell;

static TermCell  s_buf[TERM_LINES][TERM_COLS];
static int       s_row      = 0;   // ligne curseur
static int       s_col      = 0;   // colonne curseur
static int       s_view     = 0;   // première ligne visible
static int       s_cols_vis = 0;   // colonnes visibles (calculé depuis taille fenêtre)
static int       s_rows_vis = 0;   // lignes visibles
static uint32_t  s_fg       = WIN_TEXT_FG;
static uint32_t  s_bg       = WIN_TEXT_BG;

static void buf_clear_line(int r) {
    for (int c = 0; c < TERM_COLS; c++) {
        s_buf[r][c].ch = ' ';
        s_buf[r][c].fg = WIN_TEXT_FG;
        s_buf[r][c].bg = WIN_TEXT_BG;
    }
}

static void buf_init(void) {
    s_row = 0; s_col = 0; s_view = 0;
    s_fg  = WIN_TEXT_FG; s_bg = WIN_TEXT_BG;
    for (int r = 0; r < TERM_LINES; r++) buf_clear_line(r);
}

static void buf_newline(void) {
    s_row++;
    if (s_row >= TERM_LINES) {
        for (int r = 0; r < TERM_LINES - 1; r++)
            for (int c = 0; c < TERM_COLS; c++)
                s_buf[r][c] = s_buf[r+1][c];
        s_row = TERM_LINES - 1;
        if (s_view > 0) s_view--;
    }
    buf_clear_line(s_row);
    s_col = 0;
    int bot = s_row - s_rows_vis + 1;
    if (bot < 0) bot = 0;
    if (s_view < bot) s_view = bot;
}

static void buf_putchar(char c) {
    if (c == '\n') { buf_newline(); return; }
    if (c == '\r') { s_col = 0; return; }
    if (c == '\b') {
        if (s_col > 0) {
            s_col--;
            s_buf[s_row][s_col].ch = ' ';
            s_buf[s_row][s_col].fg = WIN_TEXT_FG;
            s_buf[s_row][s_col].bg = WIN_TEXT_BG;
        }
        return;
    }
    if (s_col >= TERM_COLS) buf_newline();
    s_buf[s_row][s_col].ch = c;
    s_buf[s_row][s_col].fg = s_fg;
    s_buf[s_row][s_col].bg = s_bg;
    s_col++;
    int bot = s_row - s_rows_vis + 1;
    if (bot < 0) bot = 0;
    if (s_view < bot) s_view = bot;
}

static void buf_puts(const char* str) {
    while (*str) buf_putchar(*str++);
}

// ============================================================
// ── CHROME FENÊTRE ───────────────────────────────────────────
// ============================================================
static int s_win_x = 0, s_win_y = 0, s_win_w = 0, s_win_h = 0;

static void win_compute(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    s_win_x = WIN_MARGIN_X;
    s_win_y = WIN_MARGIN_Y;
    s_win_w = (int)sw - WIN_MARGIN_X * 2;
    s_win_h = (int)sh - WIN_MARGIN_Y * 2 - 32;
    int tw = s_win_w - 2*WIN_INNER - 2*WIN_PAD;
    int th = s_win_h - TITLEBAR_H - 2*WIN_INNER - 2*WIN_PAD;
    s_cols_vis = tw / FONT_W; if (s_cols_vis > TERM_COLS) s_cols_vis = TERM_COLS;
    s_rows_vis = th / FONT_H;
    if (s_cols_vis < 1) s_cols_vis = 1;
    if (s_rows_vis < 1) s_rows_vis = 1;
}

// curseur clignotant — position précédente pour invalidation VESA
static int s_cur_valid = 0, s_cur_vcol = 0, s_cur_vrow = 0;

static void win_draw_titlebar(void) {
    gfx_fill_rect(s_win_x + WIN_INNER, s_win_y + WIN_INNER,
                  s_win_w - 2*WIN_INNER, TITLEBAR_H, WIN_TITLE_ACT);
    gfx_draw_text_centered(s_win_x, s_win_y + (TITLEBAR_H - FONT_H)/2,
                           s_win_w, "Terminal", WIN_WHITE, WIN_TITLE_ACT);
    int bx = s_win_x + s_win_w - 24;
    int by = s_win_y + (TITLEBAR_H - 16)/2;
    uint32_t cbg = (g_mouse.x >= bx && g_mouse.x <= bx+16 &&
                    g_mouse.y >= by && g_mouse.y <= by+16)
                   ? WIN_CLOSE_HOV : WIN_CLOSE_BG;
    gfx_fill_rect(bx, by, 16, 16, cbg);
    gfx_draw_rect(bx, by, 16, 16, WIN_WHITE);
    gfx_draw_text(bx+4, by+2, "X", WIN_WHITE, cbg);
}

static void win_draw_chrome(void) {
    gfx_fill_rect(s_win_x+6, s_win_y+6, s_win_w, s_win_h, 0x00181824);
    gfx_fill_rect(s_win_x+3, s_win_y+3, s_win_w, s_win_h, 0x000C1018);
    gfx_fill_rect(s_win_x,   s_win_y,   s_win_w, s_win_h, WIN_BG);
    gfx_stroke_rect_blend(s_win_x, s_win_y, s_win_w, s_win_h, WIN_EDGE_LIGHT, 105);
    gfx_stroke_rect_blend(s_win_x+1, s_win_y+1, s_win_w-2, s_win_h-2, WIN_WHITE, 42);
    for (int xi = s_win_x+WIN_INNER; xi < s_win_x+s_win_w-WIN_INNER; xi++)
        gfx_blend_pixel(xi, s_win_y+WIN_INNER, WIN_WHITE, 50);
    win_draw_titlebar();
    int sep = s_win_y + WIN_INNER + TITLEBAR_H;
    for (int xi = s_win_x+WIN_INNER; xi < s_win_x+s_win_w-WIN_INNER; xi++)
        gfx_blend_pixel(xi, sep, WIN_SEP_LINE, 115);
}

static void win_draw_content(int full_sync) {
    int tx = s_win_x + WIN_INNER + WIN_PAD;
    int ty = s_win_y + WIN_INNER + TITLEBAR_H + 1 + WIN_PAD;

    if (full_sync) { vesa_invalidate_all(); s_cur_valid = 0; }
    else if (s_cur_valid) vesa_invalidate_cell(s_cur_vcol, s_cur_vrow);

    for (int row = 0; row < s_rows_vis; row++) {
        int src = s_view + row;
        if (src < 0 || src >= TERM_LINES) continue;
        for (int col = 0; col < s_cols_vis; col++)
            vesa_draw_glyph(tx + col*FONT_W, ty + row*FONT_H,
                            s_buf[src][col].ch,
                            s_buf[src][col].fg,
                            s_buf[src][col].bg);
    }
    int cur_srow = s_row - s_view;
    if (cur_srow >= 0 && cur_srow < s_rows_vis) {
        int cpx = tx + s_col*FONT_W;
        int cpy = ty + cur_srow*FONT_H + FONT_H - 2;
        gfx_fill_rect(cpx, cpy, FONT_W, 2, WIN_BORDER_ACT);
        s_cur_vcol  = cpx / FONT_W;
        s_cur_vrow  = (ty + cur_srow*FONT_H) / FONT_H;
        s_cur_valid = 1;
    } else { s_cur_valid = 0; }
}

static void win_redraw_full(void) {
    screen_begin_ui();
    win_draw_chrome();
    win_draw_content(1);
    screen_end_ui();
}

static void win_redraw_content(void) {
    screen_begin_ui();
    win_draw_content(0);
    screen_end_ui();
}

// ============================================================
// ── HIT-TESTS ────────────────────────────────────────────────
// ============================================================
static int hit_close(int mx, int my) {
    int bx = s_win_x + s_win_w - 24;
    int by = s_win_y + (TITLEBAR_H - 16)/2;
    return mx >= bx && mx <= bx+16 && my >= by && my <= by+16;
}

static int hit_titlebar(int mx, int my) {
    return mx >= s_win_x && mx <= s_win_x+s_win_w-28 &&
           my >= s_win_y && my <= s_win_y+TITLEBAR_H;
}

static int hit_drag(int mx, int my) {
    if (hit_close(mx,my)) return 0;
    if (mx < s_win_x || mx >= s_win_x+s_win_w ||
        my < s_win_y || my >= s_win_y+s_win_h) return 0;
    if (hit_titlebar(mx,my)) return 1;
    int il = s_win_x+WIN_DRAG_BORDER, ir = s_win_x+s_win_w-WIN_DRAG_BORDER;
    int it = s_win_y+WIN_INNER+TITLEBAR_H+1, ib = s_win_y+s_win_h-WIN_DRAG_BORDER;
    if (mx>=il && mx<ir && my>=it && my<ib) return 0;
    return 1;
}

// ============================================================
// ── SAISIE CLAVIER (avec drag souris en parallèle) ───────────
// ============================================================
// Même mécanique que l'ancienne terminal_getchar() :
// on gère le drag/hover fenêtre pendant l'attente d'une touche.

static int  s_dragging  = 0;
static int  s_drag_ox   = 0, s_drag_oy = 0;
static int  s_prev_left = 0;
static int  s_closed    = 0;   // 1 si bouton [X] pressé

static void handle_mouse(void) {
    int left      = g_mouse.btn_left;
    int left_down = left && !s_prev_left;
    s_prev_left   = left;

    if (left_down && hit_drag(g_mouse.x, g_mouse.y)) {
        s_dragging = 1;
        s_drag_ox  = g_mouse.x - s_win_x;
        s_drag_oy  = g_mouse.y - s_win_y;
    }
    if (!left) s_dragging = 0;

    if (s_dragging && left) {
        uint32_t sw = vesa_width(), sh = vesa_height();
        s_win_x = g_mouse.x - s_drag_ox;
        s_win_y = g_mouse.y - s_drag_oy;
        if (s_win_x < 0) s_win_x = 0;
        if (s_win_y < 0) s_win_y = 0;
        if (s_win_x + s_win_w > (int)sw) s_win_x = (int)sw - s_win_w;
        if (s_win_y + s_win_h > (int)sh-32) s_win_y = (int)sh-32-s_win_h;
        mouse_erase_cursor();
        win_redraw_full();
        mouse_draw_cursor();
        return;
    }

    // Hover bouton [X] → repeindre titlebar pour l'effet rouge
    mouse_erase_cursor();
    screen_begin_ui();
    win_draw_titlebar();
    screen_end_ui();
    mouse_draw_cursor();

    // Clic [X]
    if (left_down && hit_close(g_mouse.x, g_mouse.y))
        s_closed = 1;
}

// Même bug que desktop_run() : mouse_poll() en direct avale les octets
// clavier tapés pendant qu'on est dans le terminal (désync du paquet
// souris 3-octets). On passe par input_poll_char() qui route correctement
// via le status byte / mouse_in_packet().
static volatile int s_term_mouse_pending = 0;
static void term_on_mouse_packet(void) {
    s_term_mouse_pending = 1;
}

static char term_getchar(void) {
    while (1) {
        // Draine tout le buffer 8042 disponible en une passe : chaque octet
        // est routé vers clavier OU souris par input_poll_char() lui-même
        // (via le status byte + mouse_in_packet()). On récupère le dernier
        // caractère clavier lu (kc) sans jamais appeler mouse_poll() en direct.
        int  mouse_moved = 0;
        char kc = 0;
        s_term_mouse_pending = 0;
        while (input_has_pending_byte()) {
            char r = input_poll_char();
            if (r) kc = r;
        }
        if (s_term_mouse_pending) mouse_moved = 1;

        if (mouse_moved) {
            mouse_erase_cursor();
            mouse_draw_cursor();
            handle_mouse();
            if (s_closed) return 27; // ESC = fermeture
        }

        if (kc == 0) {
            __asm__ __volatile__("nop");
            continue; // rien à traiter, on reboucle (non bloquant)
        }
        if (kc == 16) { if (s_view > 0) s_view--;          win_redraw_content(); return 0; }
        if (kc == 14) { int bot = s_row-s_rows_vis+1;
                       if (bot < 0) bot = 0;
                       if (s_view < bot) s_view++;         win_redraw_content(); return 0; }
        return kc;
    }
}

// ============================================================
// ── PROMPT ───────────────────────────────────────────────────
// ============================================================
static void print_prompt(void) {
    uint32_t saved = s_fg;
    s_fg = WIN_PROMPT_COL;
    buf_puts(session_get_current_name());
    buf_puts("@TetraOS:");

    // Chemin courant (remonte depuis g_cwd vers la racine)
    char parts[16][32]; int depth = 0;
    uint32_t cur = g_cwd;
    while (cur != 0 && depth < 16) {
        memset(parts[depth], 0, 32);
        strncpy(parts[depth], g_fs.nodes[cur].name, 31);
        cur = g_fs.nodes[cur].parent;
        depth++;
    }
    buf_putchar('/');
    for (int i = depth-1; i >= 0; i--) {
        buf_puts(parts[i]);
        if (i > 0) buf_putchar('/');
    }
    buf_puts(session_is_admin() ? "# " : "$ ");
    s_fg = saved;
}

// ============================================================
// ── POINT D'ENTRÉE TEX ───────────────────────────────────────
//
// C'est la fonction que le kernel appelle au clic sur l'icône.
// Elle est aussi le site de future signature crypto :
//   - son adresse est dans TexHeader.entry
//   - TexHeader.reserved contiendra le hash/signature de cette fonction
//   - le vérificateur lira [entry, entry+N[ et vérifiera avant l'appel
// ─────────────────────────────────────────────────────────────
void app_terminal(void) {
    win_compute();
    buf_init();
    s_closed    = 0;
    s_dragging  = 0;
    s_prev_left = g_mouse.btn_left;

    // Hook de sortie : print_string/print_char → buffer terminal
    g_print_hook = buf_putchar;

    input_set_mouse_packet_handler(term_on_mouse_packet);

    mouse_erase_cursor();
    win_redraw_full();
    mouse_draw_cursor();

    buf_puts("  TetraOS Terminal\n");
    buf_puts("  Tapez 'sys.help' pour l'aide\n\n");
    win_redraw_content();

    char input[256];

    while (g_session_manager.logged_in && !s_closed) {
        print_prompt();
        win_redraw_content();

        int idx = 0; input[0] = '\0';

        while (!s_closed) {
            char c = term_getchar();
            if (c == 0) continue; // scroll

            if (c == '\n' || c == '\r') {
                input[idx] = '\0';
                buf_putchar('\n');
                win_redraw_content();
                break;
            }
            if ((c == '\b' || c == 127) && idx > 0) {
                idx--; input[idx] = '\0';
                buf_putchar('\b');
                win_redraw_content();
                continue;
            }
            if (c == 27) { // ESC ou [X]
                input[0] = '\0'; idx = 0;
                if (!s_closed) { buf_puts("^C\n"); win_redraw_content(); }
                break;
            }
            if (c >= 32 && c <= 126 && idx < 255) {
                int bot = s_row - s_rows_vis + 1;
                if (bot < 0) bot = 0;
                s_view = bot;
                input[idx++] = c;
                buf_putchar(c);
                win_redraw_content();
            }
        }

        if (s_closed) break;

        const char* s = input;
        while (*s == ' ') s++;
        if (strlen(s) == 0) continue;

        char mod[32], cmd[64], args[192];
        if (shell_parse(s, mod, sizeof(mod), cmd, sizeof(cmd), args, sizeof(args)))
            shell_dispatch(mod, cmd, args);
        else if (strcmp(s, "sl") == 0)
            cmd_sl();
        else
            buf_puts("commande inconnue. Tapez 'sys.help'\n");

        win_redraw_content();
    }

    g_print_hook = 0;
}