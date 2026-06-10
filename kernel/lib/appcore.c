// lib/appcore.c — AppCore v2 : framework UI de TetraOS
// Un seul lecteur du port 0x64, un seul redraw par cycle.

#include "appcore.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../ui/desktop.h"

// ============================================================
// Structures internes
// ============================================================

typedef struct {
    int    used;
    WinID  win;
    int    x, y, w, h;
    char   label[64];
    int    enabled;
    int    hovered;
    int    pressed;
    int    touched;
    int    dirty;        // 1 = ce bouton seul doit être repeint
    AppClickCb cb;
} Button;

typedef struct {
    int      used;
    WinID    win;
    int      x, y;
    char     text[128];
    uint32_t color;
    int      dirty;      // 1 = ce label seul doit être repeint
} Label;

typedef struct {
    int  is_dir;
    char text[AC_LST_ITEM_LEN];
} LstItem;

typedef struct {
    int      used;
    WinID    win;
    int      x, y, w, h;
    int      item_h;
    LstItem  items[AC_LST_MAX_ITEMS];
    int      count;
    int      selected;
    int      scroll;
    int      hovered;
    int      clicked;
    int      activated;
    int      last_click_idx;
    int      dirty;      // 1 = listbox entière à repeindre
} Listbox;

typedef struct {
    int      used;
    WinID    win;
    int      x, y, w, h;
    AppDrawCb cb;
    int      dirty;
} DrawArea;

typedef struct {
    int    used;
    int    open;
    int    x, y, w, h;
    char   title[64];
    int    dirty;
    int    dirty_title;      // 1 = seule la titlebar a changé (focus) — évite full redraw
    int    dragging;
    int    drag_ox, drag_oy;
    int    prev_x, prev_y;   // position au tick précédent (pour erase-on-move)
    int    moved;            // 1 si la fenêtre a bougé ce tick
    AppCloseCb close_cb;
    AppKeyCb   key_cb;
} Window;

// ============================================================
// État global
// ============================================================
static Window   g_wins[AC_MAX_WINDOWS];
static Button   g_btns[AC_MAX_BUTTONS];
static Label    g_lbls[AC_MAX_LABELS];
static Listbox  g_lsts[AC_MAX_LISTBOXES];
static DrawArea g_daws[AC_MAX_DRAWAREAS];
static int      g_redrawn_this_tick;

static WinID    g_zorder[AC_MAX_WINDOWS];
static int      g_zcount;
static int      g_prev_left;

// Callback de fond desktop — void cb(int x, int y, int w, int h)
// Type défini inline pour éviter tout problème d'ordre d'include.
typedef void (*FeBgCb_t)(int, int, int, int);
static FeBgCb_t g_bg_cb = 0;

// ============================================================
// Helpers
// ============================================================
static int str_len(const char* s) { int n=0; while(s[n]) n++; return n; }

static void str_cpy(char* dst, const char* src, int max) {
    int i=0;
    while (i < max-1 && src[i]) { dst[i]=src[i]; i++; }
    dst[i]='\0';
}

static int hit(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx>=rx && mx<rx+rw && my>=ry && my<ry+rh;
}

static int win_cx(WinID wid) { return g_wins[wid].x; }
static int win_cy(WinID wid) { return g_wins[wid].y + AC_WIN_TITLE_H; }
static int win_total_h(WinID wid) { return g_wins[wid].h + AC_WIN_TITLE_H; }

// ============================================================
// Z-order
// ============================================================
static void z_bring_front(WinID wid) {
    int found=-1;
    for (int i=0; i<g_zcount; i++)
        if (g_zorder[i]==wid) { found=i; break; }
    if (found<0 || found==g_zcount-1) return;
    for (int i=found; i<g_zcount-1; i++) g_zorder[i]=g_zorder[i+1];
    g_zorder[g_zcount-1]=wid;
}

static void z_remove(WinID wid) {
    int found=-1;
    for (int i=0; i<g_zcount; i++)
        if (g_zorder[i]==wid) { found=i; break; }
    if (found<0) return;
    for (int i=found; i<g_zcount-1; i++) g_zorder[i]=g_zorder[i+1];
    g_zcount--;
}

static WinID focused_win(void) {
    for (int i=g_zcount-1; i>=0; i--)
        if (g_wins[g_zorder[i]].open) return g_zorder[i];
    return APPCORE_INVALID;
}

static WinID z_top_at(int mx, int my) {
    for (int i=g_zcount-1; i>=0; i--) {
        WinID wid=g_zorder[i];
        if (!g_wins[wid].open) continue;
        if (hit(mx,my, g_wins[wid].x, g_wins[wid].y,
                g_wins[wid].w, win_total_h(wid)))
            return wid;
    }
    return APPCORE_INVALID;
}

// ============================================================
// Dessin boutons / labels
// ============================================================
static void draw_button(WinID wid, int bid) {
    Button* b=&g_btns[bid];
    int ax=win_cx(wid)+b->x, ay=win_cy(wid)+b->y;
    uint32_t bg = !b->enabled ? AC_BTN_DISABLED
                : b->pressed  ? AC_BTN_PRESS
                : b->hovered  ? AC_BTN_HOV
                :               AC_BTN_BG;
    uint32_t fg = b->enabled ? AC_BTN_FG : AC_BTN_FG_DIS;
    gfx_fill_rect(ax, ay, b->w, b->h, bg);
    gfx_draw_rect(ax, ay, b->w, b->h, AC_BTN_BORDER);
    int tlen=str_len(b->label);
    gfx_draw_text(ax+(b->w-tlen*FONT_W)/2, ay+(b->h-FONT_H)/2,
                  b->label, fg, bg);
}

static void draw_label(WinID wid, int lid) {
    Label* l=&g_lbls[lid];
    int ax=win_cx(wid)+l->x, ay=win_cy(wid)+l->y;
    gfx_draw_text(ax, ay, l->text, l->color, AC_WIN_BG);
}

// ============================================================
// Dessin listbox
// ============================================================
static void draw_listbox(WinID wid, int lid) {
    Listbox* lb=&g_lsts[lid];
    int ax=win_cx(wid)+lb->x, ay=win_cy(wid)+lb->y;
    int w=lb->w, h=lb->h;

    // Fond + bordure
    gfx_fill_rect(ax, ay, w, h, AC_LST_BG);
    gfx_draw_rect(ax, ay, w, h, AC_LST_BORDER);

    // Scrollbar (si nécessaire)
    int visible = h / lb->item_h;
    int sb_w    = (lb->count > visible) ? 10 : 0;
    int list_w  = w - sb_w - 2;

    // Items
    for (int i=0; i<visible; i++) {
        int idx = lb->scroll + i;
        if (idx >= lb->count) break;

        int ix = ax + 1;
        int iy = ay + 1 + i * lb->item_h;
        int iw = list_w;
        int ih = lb->item_h;

        uint32_t bg = (idx == lb->selected) ? AC_LST_SEL_BG
                    : (idx == lb->hovered)  ? AC_LST_HOV_BG
                    :                         AC_LST_ITEM_BG;
        uint32_t fg = (idx == lb->selected) ? AC_LST_SEL_FG
                    : lb->items[idx].is_dir  ? AC_LST_DIR_FG
                    :                          AC_LST_ITEM_FG;

        gfx_fill_rect(ix, iy, iw, ih, bg);

        // Icône dossier/fichier
        const char* icon = lb->items[idx].is_dir ? "> " : "  ";
        gfx_draw_text(ix+2, iy+(ih-FONT_H)/2, icon, fg, bg);
        gfx_draw_text(ix+2+2*FONT_W, iy+(ih-FONT_H)/2,
                      lb->items[idx].text, fg, bg);
    }

    // Scrollbar
    if (sb_w > 0 && lb->count > 0) {
        int sb_x = ax + w - sb_w - 1;
        gfx_fill_rect(sb_x, ay+1, sb_w, h-2, AC_LST_SCROLL_BG);
        int thumb_h = (visible * (h-2)) / lb->count;
        if (thumb_h < 10) thumb_h = 10;
        int thumb_y = ay+1 + (lb->scroll * (h-2-thumb_h)) / (lb->count - visible + 1);
        gfx_fill_rect(sb_x+1, thumb_y, sb_w-2, thumb_h, AC_LST_SCROLL_FG);
    }
}

// ============================================================
// Dessin draw areas
// ============================================================
static void draw_drawarea(WinID wid, int did) {
    DrawArea* da=&g_daws[did];
    int ax=win_cx(wid)+da->x, ay=win_cy(wid)+da->y;
    if (da->cb) da->cb(wid, did, ax, ay, da->w, da->h);
}

// ============================================================
// Dessin fenêtre complète
// ============================================================
// Repeint uniquement la titlebar d'une fenêtre (changement de focus).
// Coût : ~(w * AC_WIN_TITLE_H) pixels au lieu de w*h — typiquement 50× moins.
static void draw_window_titlebar(WinID wid, int has_focus) {
    Window* w = &g_wins[wid];
    int wx = w->x, wy = w->y, ww = w->w;
    uint32_t tc0 = has_focus ? AC_WIN_TITLE_GRAD0 : 0x00001833;
    uint32_t tc1 = has_focus ? AC_WIN_TITLE_GRAD1 : 0x00000C1A;
    gfx_gradient_v(wx, wy, ww, AC_WIN_TITLE_H, tc0, tc1);
    gfx_draw_line(wx+1, wy+1, wx+ww-2, wy+1,
                  has_focus ? 0x00224488 : 0x00111A33);
    int tlen = str_len(w->title);
    gfx_draw_text(wx+(ww-tlen*FONT_W)/2, wy+(AC_WIN_TITLE_H-FONT_H)/2,
                  w->title, 0x00FFFFFF, 0);
    int cx = wx + ww - AC_WIN_TITLE_H;
    gfx_fill_rect(cx, wy, AC_WIN_TITLE_H, AC_WIN_TITLE_H, AC_WIN_CLOSE_BG);
    gfx_draw_text(cx+(AC_WIN_TITLE_H-FONT_W)/2, wy+(AC_WIN_TITLE_H-FONT_H)/2,
                  "X", 0x00FFFFFF, AC_WIN_CLOSE_BG);
    uint32_t bdr = has_focus ? AC_WIN_BORDER_FOC : AC_WIN_BORDER;
    gfx_draw_line(wx, wy+AC_WIN_TITLE_H, wx+ww-1, wy+AC_WIN_TITLE_H, bdr);
    // Recalculer la bordure latérale du haut seulement
    gfx_fill_rect(wx, wy, 1, AC_WIN_TITLE_H, bdr);
    gfx_fill_rect(wx+ww-1, wy, 1, AC_WIN_TITLE_H, bdr);
    gfx_fill_rect(wx, wy, ww, 1, bdr);
    gfx_stroke_rect_blend(wx+1, wy+1, ww-2, AC_WIN_TITLE_H, 0x00AACCFF, 25);
}

static void draw_window(WinID wid, int has_focus) {
    Window* w=&g_wins[wid];
    int wx=w->x, wy=w->y, ww=w->w, wh=w->h + AC_WIN_TITLE_H;

    gfx_fill_rect(wx, wy, ww, wh, AC_WIN_BG);

    // Titlebar
    uint32_t tc0 = has_focus ? AC_WIN_TITLE_GRAD0 : 0x00001833;
    uint32_t tc1 = has_focus ? AC_WIN_TITLE_GRAD1 : 0x00000C1A;
    gfx_gradient_v(wx, wy, ww, AC_WIN_TITLE_H, tc0, tc1);
    gfx_draw_line(wx+1, wy+1, wx+ww-2, wy+1,
                  has_focus ? 0x00224488 : 0x00111A33);

    int tlen=str_len(w->title);
    gfx_draw_text(wx+(ww-tlen*FONT_W)/2, wy+(AC_WIN_TITLE_H-FONT_H)/2,
                  w->title, 0x00FFFFFF, 0);

    // Bouton [X]
    int cx=wx+ww-AC_WIN_TITLE_H;
    gfx_fill_rect(cx, wy, AC_WIN_TITLE_H, AC_WIN_TITLE_H, AC_WIN_CLOSE_BG);
    gfx_draw_text(cx+(AC_WIN_TITLE_H-FONT_W)/2, wy+(AC_WIN_TITLE_H-FONT_H)/2,
                  "X", 0x00FFFFFF, AC_WIN_CLOSE_BG);

    // Séparateur + bordure
    uint32_t bdr = has_focus ? AC_WIN_BORDER_FOC : AC_WIN_BORDER;
    gfx_draw_line(wx, wy+AC_WIN_TITLE_H, wx+ww-1, wy+AC_WIN_TITLE_H, bdr);
    gfx_draw_rect(wx, wy, ww, wh, bdr);
    gfx_stroke_rect_blend(wx+1, wy+1, ww-2, wh-2, 0x00AACCFF, 25);

    // Widgets
    for (int i=0; i<AC_MAX_BUTTONS;   i++)
        if (g_btns[i].used && g_btns[i].win==wid) draw_button(wid, i);
    for (int i=0; i<AC_MAX_LABELS;    i++)
        if (g_lbls[i].used && g_lbls[i].win==wid) draw_label(wid, i);
    for (int i=0; i<AC_MAX_LISTBOXES; i++)
        if (g_lsts[i].used && g_lsts[i].win==wid) draw_listbox(wid, i);
    for (int i=0; i<AC_MAX_DRAWAREAS; i++)
        if (g_daws[i].used && g_daws[i].win==wid) draw_drawarea(wid, i);
}

// ============================================================
// Effacement d'une zone avec le fond desktop
// ============================================================

// Repeint le fond sur la zone (x,y,w,h) — via le callback desktop si dispo,
// sinon fond noir uni. Utilisé pour effacer l'ancienne position d'une fenêtre.
static void erase_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    // Clamp aux limites écran
    int sw = (int)vesa_width(), sh = (int)vesa_height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    if (g_bg_cb) {
        // Le desktop repeint son dégradé sur cette zone exacte
        g_bg_cb(x, y, w, h);
    } else {
        // Fallback : noir
        gfx_fill_rect(x, y, w, h, 0x00000000);
    }
}

// ============================================================
// Redraw partiel — ne repeint que les widgets dirty
// ============================================================

// Repeint un bouton dirty sans toucher à la fenêtre entière.
static void redraw_button(int bid) {
    Button* b = &g_btns[bid];
    if (!b->used || !b->dirty) return;
    WinID wid = b->win;
    if (!g_wins[wid].used || !g_wins[wid].open) { b->dirty=0; return; }
    mouse_erase_cursor();
    draw_button(wid, bid);
    mouse_draw_cursor();
    b->dirty = 0;
}

// Repeint une listbox dirty sans toucher à la fenêtre entière.
static void redraw_listbox(int lid) {
    Listbox* lb = &g_lsts[lid];
    if (!lb->used || !lb->dirty) return;
    WinID wid = lb->win;
    if (!g_wins[wid].used || !g_wins[wid].open) { lb->dirty=0; return; }
    mouse_erase_cursor();
    draw_listbox(wid, lid);
    mouse_draw_cursor();
    lb->dirty = 0;
}

// Repeint un label dirty.
static void redraw_label(int lid) {
    Label* l = &g_lbls[lid];
    if (!l->used || !l->dirty) return;
    WinID wid = l->win;
    if (!g_wins[wid].used || !g_wins[wid].open) { l->dirty=0; return; }
    mouse_erase_cursor();
    // Effacer l'ancienne zone texte (128 chars max × font dims)
    int ax = win_cx(wid)+l->x, ay = win_cy(wid)+l->y;
    gfx_fill_rect(ax, ay, 128*FONT_W, FONT_H, AC_WIN_BG);
    draw_label(wid, lid);
    mouse_draw_cursor();
    l->dirty = 0;
}

// Retourne 1 si au moins une fenêtre entière est dirty (nécessite full redraw).
static int any_win_dirty(void) {
    for (int i=0;i<AC_MAX_WINDOWS;i++)
        if (g_wins[i].used && g_wins[i].open && (g_wins[i].dirty || g_wins[i].dirty_title)) return 1;
    return 0;
}

// Retourne 1 si au moins un widget individuel est dirty (redraw partiel possible).
static int any_widget_dirty(void) {
    for (int i=0;i<AC_MAX_BUTTONS;   i++) if (g_btns[i].used && g_btns[i].dirty) return 1;
    for (int i=0;i<AC_MAX_LISTBOXES; i++) if (g_lsts[i].used && g_lsts[i].dirty) return 1;
    for (int i=0;i<AC_MAX_LABELS;    i++) if (g_lbls[i].used && g_lbls[i].dirty) return 1;
    for (int i=0;i<AC_MAX_DRAWAREAS; i++) if (g_daws[i].used && g_daws[i].dirty) return 1;
    return 0;
}

// Redraw COMPLET de toutes les fenêtres (déplacement, focus change, etc.)
static void redraw_all(void) {
    mouse_erase_cursor();
    screen_begin_ui();

    // Passe 1 : effacer les anciens pixels des fenêtres déplacées
    for (int i=g_zcount-1; i>=0; i--) {
        WinID wid=g_zorder[i];
        Window* w=&g_wins[wid];
        if (!w->used || !w->open || !w->moved) continue;
        erase_rect(w->prev_x, w->prev_y, w->w, w->h+AC_WIN_TITLE_H);
    }

    // Passe 2 : redessiner les fenêtres dans l'ordre Z.
    // Si dirty_title seul (changement de focus, pas de move ni contenu),
    // on ne repeint que la titlebar — 50× moins de pixels à écrire.
    WinID focus=focused_win();
    for (int i=0; i<g_zcount; i++) {
        WinID wid=g_zorder[i];
        if (!g_wins[wid].open) continue;
        int title_only = g_wins[wid].dirty_title
                      && !g_wins[wid].dirty
                      && !g_wins[wid].moved;
        if (title_only)
            draw_window_titlebar(wid, wid==focus);
        else
            draw_window(wid, wid==focus);
    }

    screen_end_ui();
    mouse_draw_cursor();

    // Reset tous les flags dirty
    for (int i=0;i<AC_MAX_WINDOWS;  i++) { g_wins[i].dirty=0; g_wins[i].moved=0; g_wins[i].dirty_title=0; }
    for (int i=0;i<AC_MAX_BUTTONS;  i++) g_btns[i].dirty=0;
    for (int i=0;i<AC_MAX_LISTBOXES;i++) g_lsts[i].dirty=0;
    for (int i=0;i<AC_MAX_LABELS;   i++) g_lbls[i].dirty=0;
    for (int i=0;i<AC_MAX_DRAWAREAS;i++) g_daws[i].dirty=0;
    g_redrawn_this_tick=1;
}

// Redraw PARTIEL : ne repeint que les widgets individuellement dirty.
// Utilisé quand seuls des boutons/listboxes/labels ont changé d'état
// (hover, sélection) — évite de tout repeindre pour un simple survol.
static void redraw_dirty_widgets(void) {
    // Labels
    for (int i=0;i<AC_MAX_LABELS;i++) redraw_label(i);
    // Boutons
    for (int i=0;i<AC_MAX_BUTTONS;i++) redraw_button(i);
    // Listboxes
    for (int i=0;i<AC_MAX_LISTBOXES;i++) redraw_listbox(i);
    // DrawAreas (toujours full si elles sont dirty)
    WinID focus=focused_win();
    for (int i=0;i<AC_MAX_DRAWAREAS;i++) {
        if (!g_daws[i].used||!g_daws[i].dirty) continue;
        WinID wid=g_daws[i].win;
        if (g_wins[wid].open) {
            mouse_erase_cursor();
            draw_drawarea(wid,i);
            mouse_draw_cursor();
        }
        g_daws[i].dirty=0;
        (void)focus;
    }
    g_redrawn_this_tick=1;
}

static int any_dirty(void) __attribute__((unused));
static int any_dirty(void) {
    return any_win_dirty() || any_widget_dirty();
}

// ============================================================
// Clavier non-bloquant — délègue au dispatcher centralisé
// ============================================================
// On n'accède PLUS directement au port 0x64/0x60 ici.
// input_poll_char() gère le tri clavier/souris via le bit5.
extern char input_poll_char(void);

static char read_key_nb(void) {
    return input_poll_char();
}

// ============================================================
// Gestion souris — dirty par widget, pas par fenêtre entière
// ============================================================
static void process_mouse(void) {
    int mx=g_mouse.x, my=g_mouse.y;
    int left     =  g_mouse.btn_left;
    int left_down=  left && !g_prev_left;
    int left_up  = !left &&  g_prev_left;

    WinID top=z_top_at(mx,my);

    // Changement de focus → repeindre seulement la titlebar de chaque fenêtre.
    // dirty_title déclenche draw_window_titlebar() au lieu de draw_window() complet
    // → ~50× moins de pixels à écrire pour un simple clic de focus.
    if (left_down && top!=APPCORE_INVALID && top!=focused_win()) {
        z_bring_front(top);
        for (int i=0; i<AC_MAX_WINDOWS; i++)
            if (g_wins[i].used && g_wins[i].open)
                g_wins[i].dirty_title = 1;  // titlebar seule, pas le contenu
    }

    WinID focus=focused_win();
    if (focus!=APPCORE_INVALID) {
        Window* fw=&g_wins[focus];
        int close_x=fw->x+fw->w-AC_WIN_TITLE_H;
        int in_title=hit(mx,my, fw->x, fw->y, fw->w-AC_WIN_TITLE_H, AC_WIN_TITLE_H);

        if (left_down && in_title) {
            fw->dragging=1; fw->drag_ox=mx-fw->x; fw->drag_oy=my-fw->y;
            fw->prev_x = fw->x; fw->prev_y = fw->y;
        }
        if (!left) fw->dragging=0;
        if (fw->dragging && left) {
            int nx=mx-fw->drag_ox, ny=my-fw->drag_oy;
            int sw=(int)vesa_width(), sh=(int)vesa_height();
            if (nx<0) nx=0; if (ny<0) ny=0;
            if (nx+fw->w>sw) nx=sw-fw->w;
            if (ny+win_total_h(focus)>sh-32) ny=sh-32-win_total_h(focus);
            if (nx != fw->x || ny != fw->y) {
                fw->prev_x = fw->x; fw->prev_y = fw->y;
                fw->x=nx; fw->y=ny;
                fw->moved=1;
                fw->dirty=1;
            }
        }
        int in_close=hit(mx,my, close_x, fw->y, AC_WIN_TITLE_H, AC_WIN_TITLE_H);
        if (left_down && in_close) {
            if (fw->close_cb) fw->close_cb(focus);
            else app_close_window(focus);
        }
    }

    // ── Boutons : dirty uniquement sur le bouton qui change d'état ──────
    for (int i=0; i<AC_MAX_BUTTONS; i++) {
        Button* b=&g_btns[i];
        if (!b->used || !b->enabled) continue;
        WinID wid=b->win;
        if (!g_wins[wid].open) continue;
        if (wid!=focused_win()) {
            // Dépresser/déhover si on perd le focus
            if (b->hovered||b->pressed) {
                b->hovered=b->pressed=0;
                b->dirty=1;   // ← seulement ce bouton
            }
            continue;
        }
        int ax=win_cx(wid)+b->x, ay=win_cy(wid)+b->y;
        int over=hit(mx,my,ax,ay,b->w,b->h);

        int was_hov=b->hovered;
        b->hovered=over;
        // Changement de hover → redraw de CE bouton seulement
        if (b->hovered!=was_hov) b->dirty=1;

        if (left_down && over) {
            b->pressed=1;
            b->dirty=1;
        }
        if (left_up && b->pressed) {
            b->pressed=0;
            if (over) { b->touched=1; if (b->cb) b->cb(i); }
            b->dirty=1;
        }
    }

    // ── Listboxes : dirty uniquement sur la listbox qui change d'état ───
    for (int i=0; i<AC_MAX_LISTBOXES; i++) {
        Listbox* lb=&g_lsts[i];
        if (!lb->used) continue;
        WinID wid=lb->win;
        if (!g_wins[wid].open || wid!=focused_win()) continue;

        int ax=win_cx(wid)+lb->x, ay=win_cy(wid)+lb->y;
        int visible=lb->h/lb->item_h;

        int new_hov=-1;
        if (hit(mx,my,ax,ay,lb->w,lb->h)) {
            int rel=(my-ay)/lb->item_h;
            int idx=lb->scroll+rel;
            if (idx>=0 && idx<lb->count) new_hov=idx;
        }
        if (new_hov!=lb->hovered) {
            lb->hovered=new_hov;
            lb->dirty=1;    // ← seulement cette listbox
        }

        if (left_down && new_hov>=0) {
            if (new_hov==lb->last_click_idx && new_hov==lb->selected)
                lb->activated=1;
            lb->last_click_idx=new_hov;
            lb->selected=new_hov;
            lb->clicked=1;
            lb->dirty=1;
        }

        if (lb->selected>=0) {
            if (lb->selected < lb->scroll)
                { lb->scroll=lb->selected; lb->dirty=1; }
            if (lb->selected >= lb->scroll+visible)
                { lb->scroll=lb->selected-visible+1; lb->dirty=1; }
        }
    }

    g_prev_left=left;
}

// ============================================================
// API publique — Init
// ============================================================
void app_init(void) {
    for (int i=0; i<AC_MAX_WINDOWS;   i++) g_wins[i].used=0;
    for (int i=0; i<AC_MAX_BUTTONS;   i++) g_btns[i].used=0;
    for (int i=0; i<AC_MAX_LABELS;    i++) g_lbls[i].used=0;
    for (int i=0; i<AC_MAX_LISTBOXES; i++) g_lsts[i].used=0;
    for (int i=0; i<AC_MAX_DRAWAREAS; i++) g_daws[i].used=0;
    g_zcount=0; g_prev_left=0; g_redrawn_this_tick=0;
}

// Remet appcore dans l'état initial après une session (logout).
// Libère tous les widgets et reset le bg_callback pour éviter que le
// callback de l'ancienne session (ex: redraw_background_rect du bureau)
// soit appelé pendant le login suivant.
void app_reset(void) {
    app_init();
    g_bg_cb = (void*)0;
}

int app_running(void) {
    for (int i=0; i<AC_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].open) return 1;
    return 0;
}

static char g_last_tick_key = 0;   // touche lue par le dernier app_tick()

char app_tick_get_key(void) {
    char k = g_last_tick_key;
    g_last_tick_key = 0;   // consommer
    return k;
}

void app_tick(void) {
    g_redrawn_this_tick=0;
    char key=read_key_nb();
    g_last_tick_key = key;
    // NOTE : on ne rappelle PAS mouse_poll() ici.
    // input_poll_char() (appelé via read_key_nb) gère déjà le dispatch
    // clavier/souris sur le port 0x64 et appelle mouse_poll() lui-même
    // quand l'octet vient de la souris. Un double appel à mouse_poll()
    // consommerait un octet du prochain paquet souris (ou pire, un
    // scancode clavier), ce qui corrompt la saisie clavier.
    process_mouse();

    if (key) {
        WinID focus=focused_win();
        if (focus!=APPCORE_INVALID && g_wins[focus].key_cb)
            g_wins[focus].key_cb(focus, key);
        // Scroll listbox avec flèches — dirty seulement sur la listbox
        for (int i=0; i<AC_MAX_LISTBOXES; i++) {
            Listbox* lb=&g_lsts[i];
            if (!lb->used || lb->win!=focus) continue;
            int visible=lb->h/lb->item_h;
            if (key==16 && lb->selected>0) {
                lb->selected--; lb->clicked=1;
                if (lb->selected<lb->scroll) lb->scroll=lb->selected;
                lb->dirty=1;    // ← listbox seule, pas la fenêtre entière
            } else if (key==14 && lb->selected<lb->count-1) {
                lb->selected++; lb->clicked=1;
                if (lb->selected>=lb->scroll+visible) lb->scroll++;
                lb->dirty=1;
            } else if (key=='\n' && lb->selected>=0) {
                lb->activated=1;
            }
        }
    }

    // Choix : full redraw si une fenêtre entière est dirty (déplacement,
    // focus change, fermeture, app_set_label qui doit effacer l'ancienne zone),
    // sinon redraw partiel uniquement sur les widgets dirty.
    if (any_win_dirty()) {
        redraw_all();
    } else if (any_widget_dirty()) {
        redraw_dirty_widgets();
    }
}

// ============================================================
// API publique — Fenêtres
// ============================================================
WinID app_new_window(const char* title, int x, int y, int w, int h) {
    for (int i=0; i<AC_MAX_WINDOWS; i++) {
        if (g_wins[i].used) continue;
        Window* win=&g_wins[i];
        win->used=1; win->open=1;
        win->x=x; win->y=y; win->w=w; win->h=h;
        win->prev_x=x; win->prev_y=y;
        win->moved=0;
        str_cpy(win->title, title, 64);
        win->dirty=1; win->dragging=0;
        win->close_cb=0; win->key_cb=0;
        g_zorder[g_zcount++]=i;
        return i;
    }
    return APPCORE_INVALID;
}

void app_close_window(WinID wid) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return;
    g_wins[wid].open=0; g_wins[wid].used=0;
    for (int i=0; i<AC_MAX_BUTTONS;   i++) if (g_btns[i].used&&g_btns[i].win==wid) g_btns[i].used=0;
    for (int i=0; i<AC_MAX_LABELS;    i++) if (g_lbls[i].used&&g_lbls[i].win==wid) g_lbls[i].used=0;
    for (int i=0; i<AC_MAX_LISTBOXES; i++) if (g_lsts[i].used&&g_lsts[i].win==wid) g_lsts[i].used=0;
    for (int i=0; i<AC_MAX_DRAWAREAS; i++) if (g_daws[i].used&&g_daws[i].win==wid) g_daws[i].used=0;
    z_remove(wid);
    for (int i=0; i<AC_MAX_WINDOWS; i++) if (g_wins[i].used) g_wins[i].dirty=1;
    desktop_run();
}

void app_set_title(WinID wid, const char* title) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return;
    str_cpy(g_wins[wid].title, title, 64); g_wins[wid].dirty=1;
}

void app_on_close(WinID wid, AppCloseCb cb) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return;
    g_wins[wid].close_cb=cb;
}

void app_on_key(WinID wid, AppKeyCb cb) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return;
    g_wins[wid].key_cb=cb;
}

void app_get_win_pos(WinID wid, int* ox, int* oy) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used)
        { if(ox)*ox=0; if(oy)*oy=0; return; }
    if(ox)*ox=g_wins[wid].x; if(oy)*oy=g_wins[wid].y;
}

void app_mark_dirty(WinID wid) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return;
    g_wins[wid].dirty=1;
}

int app_was_redrawn(void) { return g_redrawn_this_tick; }

// ============================================================
// API publique — Boutons
// ============================================================
BtnID app_new_button(WinID wid, int x, int y, int w, int h, const char* label) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return APPCORE_INVALID;
    for (int i=0; i<AC_MAX_BUTTONS; i++) {
        if (g_btns[i].used) continue;
        Button* b=&g_btns[i];
        b->used=1; b->win=wid; b->x=x; b->y=y; b->w=w; b->h=h;
        str_cpy(b->label, label, 64);
        b->enabled=1; b->hovered=0; b->pressed=0; b->touched=0; b->cb=0;
        g_wins[wid].dirty=1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_set_button_enabled(BtnID bid, int en) {
    if (bid<0||bid>=AC_MAX_BUTTONS||!g_btns[bid].used) return;
    g_btns[bid].enabled=en; g_wins[g_btns[bid].win].dirty=1;
}

void app_set_button_label(BtnID bid, const char* label) {
    if (bid<0||bid>=AC_MAX_BUTTONS||!g_btns[bid].used) return;
    str_cpy(g_btns[bid].label, label, 64); g_wins[g_btns[bid].win].dirty=1;
}

int app_button_touched(BtnID bid) {
    if (bid<0||bid>=AC_MAX_BUTTONS||!g_btns[bid].used) return 0;
    if (g_btns[bid].touched) { g_btns[bid].touched=0; return 1; }
    return 0;
}

void app_on_click(BtnID bid, AppClickCb cb) {
    if (bid<0||bid>=AC_MAX_BUTTONS||!g_btns[bid].used) return;
    g_btns[bid].cb=cb;
}

// ============================================================
// API publique — Labels
// ============================================================
LblID app_new_label(WinID wid, int x, int y, const char* text) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return APPCORE_INVALID;
    for (int i=0; i<AC_MAX_LABELS; i++) {
        if (g_lbls[i].used) continue;
        Label* l=&g_lbls[i];
        l->used=1; l->win=wid; l->x=x; l->y=y;
        str_cpy(l->text, text, 128); l->color=AC_LBL_FG;
        g_wins[wid].dirty=1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_set_label_text(LblID lid, const char* text) {
    if (lid<0||lid>=AC_MAX_LABELS||!g_lbls[lid].used) return;
    str_cpy(g_lbls[lid].text, text, 128);
    g_lbls[lid].dirty=1;   // ← label dirty, pas la fenêtre entière
}

void app_set_label_color(LblID lid, uint32_t color) {
    if (lid<0||lid>=AC_MAX_LABELS||!g_lbls[lid].used) return;
    g_lbls[lid].color=color;
    g_lbls[lid].dirty=1;   // ← idem
}

// ============================================================
// API publique — ListBox
// ============================================================
LstID app_new_listbox(WinID wid, int x, int y, int w, int h, int item_h) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return APPCORE_INVALID;
    for (int i=0; i<AC_MAX_LISTBOXES; i++) {
        if (g_lsts[i].used) continue;
        Listbox* lb=&g_lsts[i];
        lb->used=1; lb->win=wid;
        lb->x=x; lb->y=y; lb->w=w; lb->h=h; lb->item_h=item_h;
        lb->count=0; lb->selected=-1; lb->scroll=0;
        lb->hovered=-1; lb->clicked=0; lb->activated=0; lb->last_click_idx=-1;
        g_wins[wid].dirty=1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_listbox_add(LstID lid, const char* text, int is_dir) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return;
    Listbox* lb=&g_lsts[lid];
    if (lb->count>=AC_LST_MAX_ITEMS) return;
    str_cpy(lb->items[lb->count].text, text, AC_LST_ITEM_LEN);
    lb->items[lb->count].is_dir=is_dir;
    lb->count++;
    g_wins[lb->win].dirty=1;
}

void app_listbox_clear(LstID lid) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return;
    Listbox* lb=&g_lsts[lid];
    lb->count=0; lb->selected=-1; lb->scroll=0;
    lb->hovered=-1; lb->clicked=0; lb->activated=0; lb->last_click_idx=-1;
    g_wins[lb->win].dirty=1;
}

int app_listbox_selected(LstID lid) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return -1;
    return g_lsts[lid].selected;
}

const char* app_listbox_selected_text(LstID lid) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return 0;
    Listbox* lb=&g_lsts[lid];
    if (lb->selected<0||lb->selected>=lb->count) return 0;
    return lb->items[lb->selected].text;
}

int app_listbox_activated(LstID lid) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return 0;
    if (g_lsts[lid].activated) { g_lsts[lid].activated=0; return 1; }
    return 0;
}

int app_listbox_clicked(LstID lid) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return 0;
    if (g_lsts[lid].clicked) { g_lsts[lid].clicked=0; return 1; }
    return 0;
}

void app_listbox_scroll_to(LstID lid, int idx) {
    if (lid<0||lid>=AC_MAX_LISTBOXES||!g_lsts[lid].used) return;
    g_lsts[lid].scroll=idx; g_wins[g_lsts[lid].win].dirty=1;
}

// ============================================================
// API publique — Draw Area
// ============================================================
DawID app_new_drawarea(WinID wid, int x, int y, int w, int h, AppDrawCb cb) {
    if (wid<0||wid>=AC_MAX_WINDOWS||!g_wins[wid].used) return APPCORE_INVALID;
    for (int i=0; i<AC_MAX_DRAWAREAS; i++) {
        if (g_daws[i].used) continue;
        DrawArea* da=&g_daws[i];
        da->used=1; da->win=wid;
        da->x=x; da->y=y; da->w=w; da->h=h; da->cb=cb; da->dirty=1;
        g_wins[wid].dirty=1;
        return i;
    }
    return APPCORE_INVALID;
}

void app_drawarea_invalidate(DawID did) {
    if (did<0||did>=AC_MAX_DRAWAREAS||!g_daws[did].used) return;
    g_daws[did].dirty=1;
    g_wins[g_daws[did].win].dirty=1;
}
// ============================================================
// API publique — Callback fond desktop
// ============================================================

// Enregistre le callback appelé pour repeindre le fond sur une zone donnée.
// À appeler depuis desktop.c après app_init() :
//   app_set_bg_callback(my_desktop_bg_partial);
// Le callback reçoit (x, y, w, h) et doit repeindre exactement cette zone.
void app_set_bg_callback(void (*cb)(int, int, int, int)) {
    g_bg_cb = (FeBgCb_t)cb;
}