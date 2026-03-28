// desktop.c — Bureau graphique TetraOS
//
// Responsabilités :
//   - Dessin du fond, taskbar, icônes
//   - Gestion souris + curseur
//   - Fenêtre terminal (chrome : titlebar, bordure, bouton [X], drag)
//   - Rendu du contenu texte depuis g_term[] (terminal.c)
//   - Appel à terminal_run() sur clic icône Terminal
//
// Ce fichier ne contient PAS de logique shell. Tout ce qui concerne
// le buffer texte et le shell est dans shell/terminal.c.

#include "../ui/desktop.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../ui/session.h"
#include "../lib/utils.h"
#include "../shell/terminal.h"
#include <stdint.h>

// ============================================================
// Palette
// ============================================================
#define DT_BG_TOP       0x00000D2A
#define DT_BG_BOT       0x00000000
#define DT_TASKBAR_BG   0x00050510
#define DT_TASKBAR_LINE 0x00002244
#define DT_ICON_BG      0x00001133
#define DT_ICON_BORDER  0x000055CC
#define DT_ICON_HOVER   0x00003366
#define DT_ICON_TEXT    0x00FFFFFF
#define DT_CLOCK_FG     0x00AAAACC
#define DT_WHITE        0x00FFFFFF
#define DT_GRAY         0x00AAAAAA

// Palette fenêtre terminal
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
#define WIN_EDGE_LIGHT  0x00C8E8FF  // bordure « verre » (focus)
#define WIN_EDGE_DIM    0x006888B0  // bordure sans focus
#define WIN_SEP_LINE    0x00406088

// Dimensions fenêtre
#define WIN_MARGIN_X    80
#define WIN_MARGIN_Y    60
#define TITLEBAR_H      28
#define WIN_PAD         8
#define WIN_INNER       2   // marge intérieure (bordure vitrée)
#define WIN_DRAG_BORDER 6   // bordures latérales/bas = zone de déplacement

// ============================================================
// Icône bureau
// ============================================================
#define ICON_W   72
#define ICON_H   72

typedef struct {
    int x, y;
    const char* label;
    int hovered;
} DesktopIcon;

// ============================================================
// État global du bureau (privé)
// ============================================================
static DesktopIcon g_icon_terminal;

static int g_win_open     = 0;
static int g_win_x        = 0;
static int g_win_y        = 0;
static int g_win_w        = 0;
static int g_win_h        = 0;
static int g_win_dragging = 0;
static int g_drag_off_x   = 0;
static int g_drag_off_y   = 0;
static int g_prev_left    = 0;
static int s_term_prev_left = 0;  // détection clic dans on_term_mouse (terminal bloque desktop_run)

// Curseur terminal : gfx_fill_rect ne met pas à jour le cache VESA — on invalide
// la cellule précédente avant chaque redraw incrémental pour redessiner le glyphe.
static int s_term_cursor_valid = 0;
static int s_term_cursor_col   = 0;  // grille écran (px / FONT_W)
static int s_term_cursor_row   = 0;  // grille écran (py / FONT_H)

// ============================================================
// Forward declarations
// ============================================================
static void desktop_redraw_all(void);
static void term_redraw_content(void);
static void draw_background(void);
static void draw_taskbar(void);
static void draw_icon(DesktopIcon* ic);
static void draw_window(int focused);
static void draw_terminal_content(int full_sync);
static void win_compute_dims(void);
static void win_compute_term_size(void);
static int  hit_titlebar(int mx, int my);
static int  hit_close(int mx, int my);
static int  hit_window_drag_region(int mx, int my);
static void paint_titlebar_chrome(uint32_t tb_bg);

// ============================================================
// Callbacks installés dans terminal.c
// ============================================================

// Appelé par terminal.c après chaque écriture dans le buffer
static void on_term_redraw(void) {
    term_redraw_content();
}

// Appelé par input_dispatch_char à chaque paquet souris complet (terminal graphique)
static void on_term_mouse(void) {
    int left     = g_mouse.btn_left;
    int left_down = left && !s_term_prev_left;
    s_term_prev_left = left;

    if (left_down && hit_window_drag_region(g_mouse.x, g_mouse.y)) {
        g_win_dragging = 1;
        g_drag_off_x   = g_mouse.x - g_win_x;
        g_drag_off_y   = g_mouse.y - g_win_y;
    }
    if (!left) g_win_dragging = 0;

    if (g_win_dragging && left) {
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
        mouse_erase_cursor();
        screen_begin_ui();
        paint_titlebar_chrome(WIN_TITLE_ACT);
        screen_end_ui();
        mouse_draw_cursor();
    }
}

// ============================================================
// Dimensions fenêtre
// ============================================================
static void win_compute_dims(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    g_win_x = WIN_MARGIN_X;
    g_win_y = WIN_MARGIN_Y;
    g_win_w = (int)sw - WIN_MARGIN_X * 2;
    g_win_h = (int)sh - WIN_MARGIN_Y * 2 - 32;
}

static void win_compute_term_size(void) {
    int text_w = g_win_w - 2 * WIN_INNER - 2 * WIN_PAD;
    int text_h = g_win_h - TITLEBAR_H - 2 * WIN_INNER - 2 * WIN_PAD;
    g_term_cols_vis = text_w / FONT_W;
    g_term_rows_vis = text_h / FONT_H;
    if (g_term_cols_vis > TERM_COLS) g_term_cols_vis = TERM_COLS;
    if (g_term_cols_vis < 1) g_term_cols_vis = 1;
    if (g_term_rows_vis < 1) g_term_rows_vis = 1;
}

// ============================================================
// Dessin bureau
// ============================================================
static void draw_background(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    gfx_gradient_v(0, 0, (int)sw, (int)sh, DT_BG_TOP, DT_BG_BOT);
    // gfx_* / vesa_put_pixel ne mettent pas à jour le cache des glyphes : sans ça,
    // vesa_draw_glyph croit encore à l’ancien contenu et ne redessine pas le texte.
    vesa_invalidate_all();
}

static void draw_taskbar(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    int tbh = 32, tby = (int)sh - tbh;
    gfx_fill_rect(0, tby, (int)sw, tbh, DT_TASKBAR_BG);
    gfx_fill_rect(0, tby, (int)sw, 1,   DT_TASKBAR_LINE);
    gfx_draw_text(10, tby + (tbh - FONT_H) / 2,
                  session_get_current_name(), DT_CLOCK_FG, DT_TASKBAR_BG);
    gfx_draw_text_centered(0, tby + (tbh - FONT_H) / 2, (int)sw,
                           "TetraOS Desktop", DT_WHITE, DT_TASKBAR_BG);
}

static void draw_icon(DesktopIcon* ic) {
    int x = ic->x - ICON_W / 2;
    int y = ic->y - ICON_H / 2;
    uint32_t bg  = ic->hovered ? DT_ICON_HOVER : DT_ICON_BG;
    uint32_t bdr = ic->hovered ? WIN_BORDER_ACT : DT_ICON_BORDER;

    gfx_fill_rect(x, y, ICON_W, ICON_H, bg);
    gfx_draw_rect(x, y, ICON_W, ICON_H, bdr);

    int inner_x = x + 6, inner_y = y + 8;
    int inner_w = ICON_W - 12, inner_h = ICON_H - 22;
    gfx_fill_rect(inner_x, inner_y, inner_w, inner_h, 0x00000000);
    gfx_draw_rect(inner_x, inner_y, inner_w, inner_h, DT_ICON_BORDER);
    gfx_fill_rect(inner_x + 1, inner_y + 1, inner_w - 2, 5, DT_ICON_BORDER);
    gfx_draw_text(inner_x + 3, inner_y + 8,  ">", WIN_PROMPT,   0x00000000);
    gfx_draw_text(inner_x + 3, inner_y + 18, "_", WIN_TEXT_FG, 0x00000000);

    int label_y = y + ICON_H + 3;
    int cx = ic->x;
    int cy = label_y + FONT_H / 2;
    uint32_t swp = vesa_width(), shp = vesa_height();
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if ((uint32_t)cx >= swp) cx = (int)swp - 1;
    if ((uint32_t)cy >= shp) cy = (int)shp - 1;
    uint32_t label_bg = vesa_get_pixel(cx, cy);
    gfx_draw_text_centered(x, label_y, ICON_W, ic->label, DT_ICON_TEXT, label_bg);
}

static int icon_hit(DesktopIcon* ic, int mx, int my) {
    return mx >= ic->x - ICON_W / 2 && mx <= ic->x + ICON_W / 2 &&
           my >= ic->y - ICON_H / 2 && my <= ic->y + ICON_H / 2;
}

// ============================================================
// Dessin fenêtre terminal
// ============================================================
static void paint_titlebar_chrome(uint32_t tb_bg) {
    gfx_fill_rect(g_win_x + WIN_INNER, g_win_y + WIN_INNER,
                  g_win_w - 2 * WIN_INNER, TITLEBAR_H, tb_bg);
    gfx_draw_text_centered(g_win_x, g_win_y + (TITLEBAR_H - FONT_H) / 2,
                           g_win_w, "Terminal", DT_WHITE, tb_bg);
    int bx = g_win_x + g_win_w - 24;
    int by = g_win_y + (TITLEBAR_H - 16) / 2;
    uint32_t close_bg = (g_mouse.x >= bx && g_mouse.x <= bx + 16 &&
                         g_mouse.y >= by && g_mouse.y <= by + 16)
                        ? WIN_CLOSE_HOV : WIN_CLOSE_BG;
    gfx_fill_rect(bx, by, 16, 16, close_bg);
    gfx_draw_rect(bx, by, 16, 16, DT_WHITE);
    gfx_draw_text(bx + 4, by + 2, "X", DT_WHITE, close_bg);
}

static void draw_window(int focused) {
    uint32_t titlebar_color = focused ? WIN_TITLE_ACT : WIN_TITLEBAR;
    uint32_t edge_col       = focused ? WIN_EDGE_LIGHT : WIN_EDGE_DIM;

    gfx_fill_rect(g_win_x + 6, g_win_y + 6, g_win_w, g_win_h, 0x00181824);
    gfx_fill_rect(g_win_x + 3, g_win_y + 3, g_win_w, g_win_h, 0x000C1018);

    gfx_fill_rect(g_win_x, g_win_y, g_win_w, g_win_h, WIN_BG);

    gfx_stroke_rect_blend(g_win_x, g_win_y, g_win_w, g_win_h, edge_col, 105);
    gfx_stroke_rect_blend(g_win_x + 1, g_win_y + 1, g_win_w - 2, g_win_h - 2,
                          0x00FFFFFF, 42);

    int hi_y = g_win_y + WIN_INNER;
    for (int xi = g_win_x + WIN_INNER; xi < g_win_x + g_win_w - WIN_INNER; xi++)
        gfx_blend_pixel(xi, hi_y, 0x00FFFFFF, 50);

    paint_titlebar_chrome(titlebar_color);

    int sep_y = g_win_y + WIN_INNER + TITLEBAR_H;
    for (int xi = g_win_x + WIN_INNER; xi < g_win_x + g_win_w - WIN_INNER; xi++)
        gfx_blend_pixel(xi, sep_y, WIN_SEP_LINE, 115);
}

// full_sync : après redraw complet du bureau (draw_window, drag, etc.) — le framebuffer
// a été rempli par gfx_* sans mettre à jour le cache VESA ; on resynchronise tout.
// Sinon : redraw incrémental — vesa_draw_glyph ne repeint que ce qui a changé.
static void draw_terminal_content(int full_sync) {
    int tx = g_win_x + WIN_INNER + WIN_PAD;
    int ty = g_win_y + WIN_INNER + TITLEBAR_H + 1 + WIN_PAD;

    if (full_sync) {
        vesa_invalidate_all();
        s_term_cursor_valid = 0;
    } else if (s_term_cursor_valid) {
        vesa_invalidate_cell(s_term_cursor_col, s_term_cursor_row);
    }

    for (int row = 0; row < g_term_rows_vis; row++) {
        int src = g_term_view + row;
        if (src < 0 || src >= TERM_LINES) continue;
        for (int col = 0; col < g_term_cols_vis; col++) {
            char     ch = g_term[src][col].ch;
            uint32_t fg = g_term[src][col].fg;
            uint32_t bg = g_term[src][col].bg;
            int px = tx + col * FONT_W;
            int py = ty + row * FONT_H;
            vesa_draw_glyph(px, py, ch, fg, bg);
        }
    }

    int cur_screen_row = g_term_row - g_term_view;
    if (cur_screen_row >= 0 && cur_screen_row < g_term_rows_vis) {
        int cpx = tx + g_term_col * FONT_W;
        int cpy = ty + cur_screen_row * FONT_H + FONT_H - 2;
        gfx_fill_rect(cpx, cpy, FONT_W, 2, WIN_BORDER_ACT);
        s_term_cursor_col   = cpx / FONT_W;
        s_term_cursor_row   = (ty + cur_screen_row * FONT_H) / FONT_H;
        s_term_cursor_valid = 1;
    } else {
        s_term_cursor_valid = 0;
    }
}

// ============================================================
// Redraw complet / partiel
// ============================================================
static void desktop_redraw_all(void) {
    screen_begin_ui();
    draw_background();
    draw_taskbar();
    if (!g_win_open) {
        g_icon_terminal.hovered = icon_hit(&g_icon_terminal, g_mouse.x, g_mouse.y);
        draw_icon(&g_icon_terminal);
    } else {
        draw_window(1);
        draw_terminal_content(1);
    }
    screen_end_ui();
}

static void term_redraw_content(void) {
    screen_begin_ui();
    draw_terminal_content(0);
    screen_end_ui();
}

// ============================================================
// Hit-tests
// ============================================================
static int hit_titlebar(int mx, int my) {
    return mx >= g_win_x && mx <= g_win_x + g_win_w - 28 &&
           my >= g_win_y && my <= g_win_y + TITLEBAR_H;
}

static int hit_close(int mx, int my) {
    int bx = g_win_x + g_win_w - 24;
    int by = g_win_y + (TITLEBAR_H - 16) / 2;
    return mx >= bx && mx <= bx + 16 && my >= by && my <= by + 16;
}

// Barre de titre + bordures (hors zone texte centrale) pour déplacer la fenêtre
static int hit_window_drag_region(int mx, int my) {
    if (hit_close(mx, my)) return 0;
    if (mx < g_win_x || mx >= g_win_x + g_win_w || my < g_win_y || my >= g_win_y + g_win_h)
        return 0;
    if (hit_titlebar(mx, my)) return 1;
    int il = g_win_x + WIN_DRAG_BORDER;
    int ir = g_win_x + g_win_w - WIN_DRAG_BORDER;
    int it = g_win_y + WIN_INNER + TITLEBAR_H + 1;
    int ib = g_win_y + g_win_h - WIN_DRAG_BORDER;
    if (mx >= il && mx < ir && my >= it && my < ib) return 0;
    return 1;
}

// ============================================================
// Ouverture / fermeture du terminal
// ============================================================
static void open_terminal(void) {
    g_win_open = 1;
    win_compute_dims();
    win_compute_term_size();
    terminal_init(WIN_TEXT_FG, WIN_TEXT_BG);

    // Installer les callbacks pour que terminal.c puisse
    // déclencher un redraw et notifier les événements souris
    terminal_set_redraw_fn(on_term_redraw);
    s_term_prev_left = g_mouse.btn_left;
    terminal_set_mouse_handler(on_term_mouse);

    mouse_erase_cursor();
    desktop_redraw_all();
    mouse_draw_cursor();

    // Lance la boucle shell — bloquant jusqu'au logout ou fermeture
    terminal_run();

    // Retour ici après logout ou fermeture par [X]
    terminal_set_redraw_fn(0);
    terminal_set_mouse_handler(0);
}

static void close_terminal(void) {
    g_win_open   = 0;
    g_print_hook = 0;
    terminal_init(WIN_TEXT_FG, WIN_TEXT_BG);
    mouse_erase_cursor();
    desktop_redraw_all();
    mouse_draw_cursor();
}

// ============================================================
// BOUCLE PRINCIPALE DU BUREAU
// ============================================================
void desktop_run(void) {
    win_compute_dims();
    win_compute_term_size();
    terminal_init(WIN_TEXT_FG, WIN_TEXT_BG);

    uint32_t sw = vesa_width(), sh = vesa_height();

    g_icon_terminal.x       = 80;
    g_icon_terminal.y       = (int)sh / 2 - 20;
    g_icon_terminal.label   = "Terminal";
    g_icon_terminal.hovered = 0;
    g_win_open    = 0;
    g_win_dragging = 0;
    g_prev_left   = 0;

    desktop_redraw_all();
    mouse_draw_cursor();

    while (g_session_manager.logged_in) {

        // ── Polling souris ────────────────────────────────────
        int mouse_moved = 0;
        while (mouse_poll()) mouse_moved = 1;

        if (mouse_moved) {
            mouse_erase_cursor();
            mouse_draw_cursor();
        }

        int cur_left  = g_mouse.btn_left;
        int prev_left = g_prev_left;

        // ── Événements clic / hover ───────────────────────────
        if (mouse_moved || cur_left != prev_left) {

            if (g_win_open) {
                if (cur_left && !prev_left &&
                    hit_window_drag_region(g_mouse.x, g_mouse.y)) {
                    g_win_dragging = 1;
                    g_drag_off_x = g_mouse.x - g_win_x;
                    g_drag_off_y = g_mouse.y - g_win_y;
                }
                if (!cur_left) g_win_dragging = 0;

                // Drag en cours
                if (g_win_dragging && cur_left) {
                    g_win_x = g_mouse.x - g_drag_off_x;
                    g_win_y = g_mouse.y - g_drag_off_y;
                    if (g_win_x < 0) g_win_x = 0;
                    if (g_win_y < 0) g_win_y = 0;
                    if (g_win_x + g_win_w > (int)sw) g_win_x = (int)sw - g_win_w;
                    if (g_win_y + g_win_h > (int)sh - 32)
                        g_win_y = (int)sh - 32 - g_win_h;
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                }

                // Hover titlebar (couleur bouton [X])
                if (mouse_moved && !g_win_dragging) {
                    mouse_erase_cursor();
                    screen_begin_ui();
                    paint_titlebar_chrome(WIN_TITLE_ACT);
                    screen_end_ui();
                    mouse_draw_cursor();
                }

                // Clic [X] → fermer (ne peut arriver que si le terminal
                // n'est pas actif — sinon terminal_run() est en cours)
                if (cur_left && !prev_left && hit_close(g_mouse.x, g_mouse.y)) {
                    close_terminal();
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

                // Double-clic (ou simple clic) → ouvrir le terminal
                if (cur_left && !prev_left && hov) {
                    open_terminal();
                    // open_terminal() est bloquant (terminal_run() en interne).
                    // On arrive ici après logout ou si terminal_run retourne.
                    if (!g_session_manager.logged_in) break;
                    // Si on est encore connecté : fermeture normale de la fenêtre
                    g_win_open = 0;
                    mouse_erase_cursor();
                    desktop_redraw_all();
                    mouse_draw_cursor();
                }
            }
        }

        g_prev_left = cur_left;
    }

    screen_exit_ui();
}
