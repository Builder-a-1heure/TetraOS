// lib/appcore.c — AppCore v2 : framework UI de TetraOS
// Un seul lecteur du port 0x64, un seul redraw par cycle.

#include "appcore.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"

// Globaux de input.c réutilisés pour la lecture non-bloquante
extern int           shift_pressed;
extern int           ctrl_pressed;
extern unsigned char keyboard_map[256];
extern unsigned char keyboard_map_shift[256];

// ============================================================
// Structures internes
// ============================================================

typedef struct {
    int    used;
    WinID  win;          // fenêtre parente
    int    x, y, w, h;  // relatif zone client
    char   label[64];
    int    enabled;
    int    hovered;
    int    pressed;
    int    touched;      // flag poll : mis à 1 au relâchement, lu par app_button_touched
    AppClickCb cb;
} Button;

typedef struct {
    int    used;
    WinID  win;
    int    x, y;
    char   text[128];
    uint32_t color;
} Label;

typedef struct {
    int    used;
    int    open;
    int    x, y, w, h;  // h = zone client
    char   title[64];
    int    dirty;
    // Drag
    int    dragging;
    int    drag_ox, drag_oy;
    // Callbacks
    AppCloseCb close_cb;
    AppKeyCb   key_cb;
    // Z-order géré par le gestionnaire global (index dans z_order[])
} Window;

// ============================================================
// État global AppCore
// ============================================================

static Window  g_wins[AC_MAX_WINDOWS];
static Button  g_btns[AC_MAX_BUTTONS];
static Label   g_lbls[AC_MAX_LABELS];
static int g_redrawn_this_tick; 

// Z-order : z_order[0] = fenêtre du dessous, z_order[top-1] = fenêtre au-dessus
static WinID   g_zorder[AC_MAX_WINDOWS];
static int     g_zcount;

static int     g_prev_left;   // état bouton gauche au cycle précédent

// ============================================================
// Helpers
// ============================================================

static int str_len(const char* s) { int n=0; while(s[n]) n++; return n; }

static void str_cpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max-1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int hit(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx>=rx && mx<rx+rw && my>=ry && my<ry+rh;
}

// Coordonnées absolues du coin haut-gauche de la zone client
static int win_cx(WinID wid) { return g_wins[wid].x; }
static int win_cy(WinID wid) { return g_wins[wid].y + AC_WIN_TITLE_H; }

// Hauteur totale de la fenêtre (titre + client)
static int win_total_h(WinID wid) { return g_wins[wid].h + AC_WIN_TITLE_H; }

// ============================================================
// Z-order : mise au premier plan
// ============================================================

static void z_bring_front(WinID wid) {
    // Retirer wid de sa position actuelle
    int found = -1;
    for (int i = 0; i < g_zcount; i++)
        if (g_zorder[i] == wid) { found = i; break; }
    if (found < 0) return;
    for (int i = found; i < g_zcount-1; i++)
        g_zorder[i] = g_zorder[i+1];
    g_zorder[g_zcount-1] = wid;
}

static void z_remove(WinID wid) {
    int found = -1;
    for (int i = 0; i < g_zcount; i++)
        if (g_zorder[i] == wid) { found = i; break; }
    if (found < 0) return;
    for (int i = found; i < g_zcount-1; i++)
        g_zorder[i] = g_zorder[i+1];
    g_zcount--;
}

// Fenêtre au-dessus sous le point (mx,my), -1 si aucune
static WinID z_top_at(int mx, int my) {
    for (int i = g_zcount-1; i >= 0; i--) {
        WinID wid = g_zorder[i];
        if (!g_wins[wid].open) continue;
        if (hit(mx, my, g_wins[wid].x, g_wins[wid].y,
                g_wins[wid].w, win_total_h(wid)))
            return wid;
    }
    return APPCORE_INVALID;
}

// Fenêtre qui a le focus (en haut du Z-order)
static WinID focused_win(void) {
    for (int i = g_zcount-1; i >= 0; i--) {
        WinID wid = g_zorder[i];
        if (g_wins[wid].open) return wid;
    }
    return APPCORE_INVALID;
}

// ============================================================
// Dessin
// ============================================================

static void draw_button(WinID wid, int bid) {
    Button* b = &g_btns[bid];
    int ax = win_cx(wid) + b->x;
    int ay = win_cy(wid) + b->y;

    uint32_t bg  = !b->enabled ? AC_BTN_DISABLED :
                    b->pressed  ? AC_BTN_PRESS    :
                    b->hovered  ? AC_BTN_HOV      : AC_BTN_BG;
    uint32_t fg  = b->enabled ? AC_BTN_FG : AC_BTN_FG_DIS;
    uint32_t bdr = b->enabled ? AC_BTN_BORDER : 0x00222222;

    if (b->enabled && !b->pressed)
        gfx_gradient_v(ax, ay, b->w, b->h, bg + 0x00101010, bg);
    else
        gfx_fill_rect(ax, ay, b->w, b->h, bg);

    gfx_draw_rect(ax, ay, b->w, b->h, bdr);

    if (b->enabled && !b->pressed)
        gfx_draw_line(ax+1, ay+1, ax+b->w-2, ay+1, 0x00334466);

    int tw = str_len(b->label) * 8;
    int tx = ax + (b->w - tw) / 2 + (b->pressed ? 1 : 0);
    int ty = ay + (b->h - 16) / 2 + (b->pressed ? 1 : 0);
    gfx_draw_text(tx, ty, b->label, fg, bg);
}

static void draw_label(WinID wid, int lid) {
    Label* l = &g_lbls[lid];
    int ax = win_cx(wid) + l->x;
    int ay = win_cy(wid) + l->y;
    gfx_draw_text(ax, ay, l->text, l->color, AC_WIN_BG);
}

static void draw_window(WinID wid, int has_focus) {
    Window* w = &g_wins[wid];
    int wx = w->x, wy = w->y;
    int ww = w->w, wh = win_total_h(wid);

    // Ombre
    gfx_fill_rect_blend(wx+4, wy+4, ww, wh, 0x00000000, AC_WIN_SHADOW_A);

    // Fond client
    gfx_fill_rect(wx, wy, ww, wh, AC_WIN_BG);

    // Barre de titre
    uint32_t tc0 = has_focus ? AC_WIN_TITLE_GRAD0 : 0x00001833;
    uint32_t tc1 = has_focus ? AC_WIN_TITLE_GRAD1 : 0x00000C1A;
    gfx_gradient_v(wx, wy, ww, AC_WIN_TITLE_H, tc0, tc1);
    gfx_draw_line(wx+1, wy+1, wx+ww-2, wy+1,
                  has_focus ? 0x00224488 : 0x00111A33);

    // Titre centré
    int tlen = str_len(w->title);
    gfx_draw_text(wx + (ww - tlen*8)/2,
                  wy + (AC_WIN_TITLE_H - 16)/2,
                  w->title, 0x00FFFFFF, 0);

    // Bouton [X]
    int cx = wx + ww - AC_WIN_TITLE_H;
    uint32_t close_bg = AC_WIN_CLOSE_BG;
    gfx_fill_rect(cx, wy, AC_WIN_TITLE_H, AC_WIN_TITLE_H, close_bg);
    gfx_draw_text(cx + (AC_WIN_TITLE_H-8)/2,
                  wy + (AC_WIN_TITLE_H-16)/2,
                  "X", 0x00FFFFFF, close_bg);

    // Séparateur + bordure
    uint32_t bdr = has_focus ? AC_WIN_BORDER_FOC : AC_WIN_BORDER;
    gfx_draw_line(wx, wy+AC_WIN_TITLE_H, wx+ww-1, wy+AC_WIN_TITLE_H, bdr);
    gfx_draw_rect(wx, wy, ww, wh, bdr);
    gfx_stroke_rect_blend(wx+1, wy+1, ww-2, wh-2, 0x00AACCFF, 25);

    // Widgets
    for (int i = 0; i < AC_MAX_BUTTONS; i++)
        if (g_btns[i].used && g_btns[i].win == wid)
            draw_button(wid, i);
    for (int i = 0; i < AC_MAX_LABELS; i++)
        if (g_lbls[i].used && g_lbls[i].win == wid)
            draw_label(wid, i);
}

// Redraw complet de toutes les fenêtres (Z-order bas → haut)
static void redraw_all(void) {
    mouse_erase_cursor();
    screen_begin_ui();
    WinID focus = focused_win();
    for (int i = 0; i < g_zcount; i++) {
        WinID wid = g_zorder[i];
        if (g_wins[wid].open)
            draw_window(wid, wid == focus);
    }
    screen_end_ui();
    mouse_draw_cursor();
    // Marquer toutes les fenêtres comme propres
    for (int i = 0; i < AC_MAX_WINDOWS; i++)
        g_wins[i].dirty = 0;
}

static int any_dirty(void) {
    for (int i = 0; i < AC_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].open && g_wins[i].dirty)
            return 1;
    return 0;
}

// ============================================================
// Lecture clavier non-bloquante (un seul lecteur de 0x64)
// Retourne 0 si rien, ou si donnée souris (traitée au passage).
// ============================================================
static char read_key_nb(void) {
    uint8_t st;
    __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
    if (!(st & 1)) return 0;

    // Donnée souris → on la traite ici et on retourne 0
    if (mouse_in_packet() || (st & (1 << 5))) {
        if (mouse_poll()) {
            mouse_erase_cursor();
            mouse_draw_cursor();
        }
        return 0;
    }

    uint8_t sc;
    __asm__ __volatile__("inb %1, %0" : "=a"(sc) : "Nd"((uint16_t)0x60));

    if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; return 0; }
    if (sc == 0x1D) { ctrl_pressed = 1;  return 0; }
    if (sc == 0x9D) { ctrl_pressed = 0;  return 0; }
    if (sc & 0x80)  return 0;

    // Scancodes spéciaux
    if (sc == 0x01) return 27;
    if (sc == 0x0E) return '\b';
    if (sc == 0x0F) return '\t';
    if (sc == 0x1C) return '\n';
    if (sc == 0x48) return 16;   // flèche haut
    if (sc == 0x50) return 14;   // flèche bas
    if (sc == 0x4B) return 17;   // flèche gauche
    if (sc == 0x4D) return 18;   // flèche droite

    if (sc < 128) {
        char c = shift_pressed ? (char)keyboard_map_shift[sc]
                               : (char)keyboard_map[sc];
        if (ctrl_pressed) {
            ctrl_pressed = 0;
            if (c >= 'a' && c <= 'z') return c - 'a' + 1;  // Ctrl+A=1 … Ctrl+Z=26
            if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
            return 0;
        }
        return c;
    }
    return 0;
}

// ============================================================
// Gestion souris — un cycle
// ============================================================
static void process_mouse(void) {
    int mx = g_mouse.x, my = g_mouse.y;
    int left      = g_mouse.btn_left;
    int left_down = left && !g_prev_left;
    int left_up   = !left && g_prev_left;

    WinID top = z_top_at(mx, my);

    // --- Clic : amène la fenêtre au focus ---
    if (left_down && top != APPCORE_INVALID) {
        z_bring_front(top);
        // Marquer toutes les fenêtres dirty pour redessiner avec les bonnes couleurs de focus
        for (int i = 0; i < AC_MAX_WINDOWS; i++)
            if (g_wins[i].used) g_wins[i].dirty = 1;
    }

    // --- Drag de la fenêtre au focus ---
    WinID focus = focused_win();

    if (focus != APPCORE_INVALID) {
        Window* fw = &g_wins[focus];

        // Zone barre de titre (hors bouton [X])
        int close_x = fw->x + fw->w - AC_WIN_TITLE_H;
        int in_title = hit(mx, my, fw->x, fw->y,
                           fw->w - AC_WIN_TITLE_H, AC_WIN_TITLE_H);

        if (left_down && in_title) {
            fw->dragging = 1;
            fw->drag_ox  = mx - fw->x;
            fw->drag_oy  = my - fw->y;
        }
        if (!left) fw->dragging = 0;

        if (fw->dragging && left) {
            int nx = mx - fw->drag_ox;
            int ny = my - fw->drag_oy;
            int sw = (int)vesa_width(), sh = (int)vesa_height();
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + fw->w > sw) nx = sw - fw->w;
            if (ny + win_total_h(focus) > sh - 32)
                ny = sh - 32 - win_total_h(focus);
            fw->x = nx; fw->y = ny;
            fw->dirty = 1;
        }

        // --- Bouton [X] ---
        int in_close = hit(mx, my, close_x, fw->y,
                           AC_WIN_TITLE_H, AC_WIN_TITLE_H);
        if (left_down && in_close) {
            if (fw->close_cb)
                fw->close_cb(focus);
            else
                app_close_window(focus);
        }
    }

    // --- Hover et clic sur les boutons ---
    for (int i = 0; i < AC_MAX_BUTTONS; i++) {
        Button* b = &g_btns[i];
        if (!b->used || !b->enabled) continue;
        WinID wid = b->win;
        if (!g_wins[wid].open) continue;

        // Les boutons ne répondent que si leur fenêtre est au focus
        if (wid != focused_win()) {
            if (b->hovered || b->pressed) {
                b->hovered = b->pressed = 0;
                g_wins[wid].dirty = 1;
            }
            continue;
        }

        int ax = win_cx(wid) + b->x;
        int ay = win_cy(wid) + b->y;
        int over = hit(mx, my, ax, ay, b->w, b->h);

        int was_hov = b->hovered;
        b->hovered = over;
        if (b->hovered != was_hov) g_wins[wid].dirty = 1;

        if (left_down && over) {
            b->pressed = 1;
            g_wins[wid].dirty = 1;
        }
        if (left_up && b->pressed) {
            b->pressed = 0;
            if (over) {
                b->touched = 1;          // flag poll
                if (b->cb) b->cb(i);     // callback
            }
            g_wins[wid].dirty = 1;
        }
    }

    g_prev_left = left;
}

// ============================================================
// API publique
// ============================================================

void app_init(void) {
    for (int i = 0; i < AC_MAX_WINDOWS; i++) g_wins[i].used = 0;
    for (int i = 0; i < AC_MAX_BUTTONS; i++) g_btns[i].used = 0;
    for (int i = 0; i < AC_MAX_LABELS;  i++) g_lbls[i].used  = 0;
    g_zcount    = 0;
    g_prev_left = 0;
}

int app_running(void) {
    for (int i = 0; i < AC_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].open) return 1;
    return 0;
}

void app_tick(void) {
    // 1. Lecture clavier (gère aussi la souris si donnée vient du port aux)
    char key = read_key_nb();

    // 2. Lecture état souris et traitement (hover, drag, clic)
    mouse_poll();
    process_mouse();

    // 3. Dispatch clavier vers la fenêtre au focus
    if (key) {
        WinID focus = focused_win();
        if (focus != APPCORE_INVALID && g_wins[focus].key_cb)
            g_wins[focus].key_cb(focus, key);
    }

    // 4. Redraw si nécessaire
    if (any_dirty())
        redraw_all();
}

// --- Fenêtres ---

WinID app_new_window(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < AC_MAX_WINDOWS; i++) {
        if (g_wins[i].used) continue;
        Window* win = &g_wins[i];
        win->used     = 1;
        win->open     = 1;
        win->x = x; win->y = y;
        win->w = w; win->h = h;
        str_cpy(win->title, title, 64);
        win->dirty    = 1;
        win->dragging = 0;
        win->close_cb = 0;
        win->key_cb   = 0;
        g_zorder[g_zcount++] = i;
        return i;
    }
    return APPCORE_INVALID;
}

void app_close_window(WinID wid) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used) return;
    g_wins[wid].open = 0;
    g_wins[wid].used = 0;
    // Libérer les widgets
    for (int i = 0; i < AC_MAX_BUTTONS; i++)
        if (g_btns[i].used && g_btns[i].win == wid) g_btns[i].used = 0;
    for (int i = 0; i < AC_MAX_LABELS; i++)
        if (g_lbls[i].used && g_lbls[i].win == wid)  g_lbls[i].used  = 0;
    z_remove(wid);
    // Redraw pour effacer la fenêtre fermée
    for (int i = 0; i < AC_MAX_WINDOWS; i++)
        if (g_wins[i].used) g_wins[i].dirty = 1;
}

void app_set_title(WinID wid, const char* title) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used) return;
    str_cpy(g_wins[wid].title, title, 64);
    g_wins[wid].dirty = 1;
}

void app_on_close(WinID wid, AppCloseCb cb) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used) return;
    g_wins[wid].close_cb = cb;
}

// --- Boutons ---

BtnID app_new_button(WinID wid, int x, int y, int w, int h,
                     const char* label) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used)
        return APPCORE_INVALID;
    for (int i = 0; i < AC_MAX_BUTTONS; i++) {
        if (g_btns[i].used) continue;
        Button* b = &g_btns[i];
        b->used = 1; b->win = wid;
        b->x=x; b->y=y; b->w=w; b->h=h;
        str_cpy(b->label, label, 64);
        b->enabled = 1; b->hovered = 0;
        b->pressed = 0; b->touched = 0;
        b->cb = 0;
        g_wins[wid].dirty = 1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_set_button_enabled(BtnID bid, int enabled) {
    if (bid < 0 || bid >= AC_MAX_BUTTONS || !g_btns[bid].used) return;
    g_btns[bid].enabled = enabled;
    g_wins[g_btns[bid].win].dirty = 1;
}

void app_set_button_label(BtnID bid, const char* label) {
    if (bid < 0 || bid >= AC_MAX_BUTTONS || !g_btns[bid].used) return;
    str_cpy(g_btns[bid].label, label, 64);
    g_wins[g_btns[bid].win].dirty = 1;
}

int app_button_touched(BtnID bid) {
    if (bid < 0 || bid >= AC_MAX_BUTTONS || !g_btns[bid].used) return 0;
    if (g_btns[bid].touched) {
        g_btns[bid].touched = 0;
        return 1;
    }
    return 0;
}

void app_on_click(BtnID bid, AppClickCb cb) {
    if (bid < 0 || bid >= AC_MAX_BUTTONS || !g_btns[bid].used) return;
    g_btns[bid].cb = cb;
}

// --- Labels ---

LblID app_new_label(WinID wid, int x, int y, const char* text) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used)
        return APPCORE_INVALID;
    for (int i = 0; i < AC_MAX_LABELS; i++) {
        if (g_lbls[i].used) continue;
        Label* l = &g_lbls[i];
        l->used = 1; l->win = wid;
        l->x=x; l->y=y;
        str_cpy(l->text, text, 128);
        l->color = AC_LBL_FG;
        g_wins[wid].dirty = 1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_set_label_text(LblID lid, const char* text) {
    if (lid < 0 || lid >= AC_MAX_LABELS || !g_lbls[lid].used) return;
    // Effacer l'ancien texte en fond avant d'écrire le nouveau
    str_cpy(g_lbls[lid].text, text, 128);
    g_wins[g_lbls[lid].win].dirty = 1;
}

void app_set_label_color(LblID lid, uint32_t color) {
    if (lid < 0 || lid >= AC_MAX_LABELS || !g_lbls[lid].used) return;
    g_lbls[lid].color = color;
    g_wins[g_lbls[lid].win].dirty = 1;
}

// --- Clavier ---

void app_on_key(WinID wid, AppKeyCb cb) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used) return;
    g_wins[wid].key_cb = cb;
}

 
void app_get_win_pos(WinID wid, int* out_x, int* out_y) {
    if (wid < 0 || wid >= AC_MAX_WINDOWS || !g_wins[wid].used) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        return;
    }
    if (out_x) *out_x = g_wins[wid].x;
    if (out_y) *out_y = g_wins[wid].y;
}
 
int app_was_redrawn(void) {
    return g_redrawn_this_tick;
}
 
