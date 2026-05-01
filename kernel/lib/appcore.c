// lib/appcore.c — AppCore v2 : framework UI de TetraOS
// Un seul lecteur du port 0x64, un seul redraw par cycle.

#include "appcore.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"

extern int           shift_pressed;
extern int           ctrl_pressed;
extern unsigned char keyboard_map[256];
extern unsigned char keyboard_map_shift[256];

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
    AppClickCb cb;
} Button;

typedef struct {
    int      used;
    WinID    win;
    int      x, y;
    char     text[128];
    uint32_t color;
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
    int      selected;   // index sélectionné (-1 = aucun)
    int      scroll;     // première ligne visible
    int      hovered;    // index survolé (-1 = aucun)
    int      clicked;    // flag : sélection changée
    int      activated;  // flag : double-clic
    int      last_click_idx; // pour détecter double-clic
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
    int    dragging;
    int    drag_ox, drag_oy;
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
// Redraw global
// ============================================================
static void redraw_all(void) {
    mouse_erase_cursor();
    screen_begin_ui();
    WinID focus=focused_win();
    for (int i=0; i<g_zcount; i++) {
        WinID wid=g_zorder[i];
        if (g_wins[wid].open) draw_window(wid, wid==focus);
    }
    screen_end_ui();
    mouse_draw_cursor();
    for (int i=0; i<AC_MAX_WINDOWS; i++) g_wins[i].dirty=0;
    for (int i=0; i<AC_MAX_DRAWAREAS; i++) g_daws[i].dirty=0;
    g_redrawn_this_tick=1;
}

static int any_dirty(void) {
    for (int i=0; i<AC_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].open && g_wins[i].dirty) return 1;
    return 0;
}

// ============================================================
// Clavier non-bloquant
// ============================================================
static char read_key_nb(void) {
    uint8_t st;
    __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
    if (!(st & 1)) return 0;
    if (mouse_in_packet() || (st & (1<<5))) {
        if (mouse_poll()) { mouse_erase_cursor(); mouse_draw_cursor(); }
        return 0;
    }
    uint8_t sc;
    __asm__ __volatile__("inb %1, %0" : "=a"(sc) : "Nd"((uint16_t)0x60));
    if (sc==0x2A||sc==0x36) { shift_pressed=1; return 0; }
    if (sc==0xAA||sc==0xB6) { shift_pressed=0; return 0; }
    if (sc==0x1D)            { ctrl_pressed=1;  return 0; }
    if (sc==0x9D)            { ctrl_pressed=0;  return 0; }
    if (sc & 0x80)  return 0;
    if (sc==0x01) return 27;
    if (sc==0x0E) return '\b';
    if (sc==0x0F) return '\t';
    if (sc==0x1C) return '\n';
    if (sc==0x48) return 16;
    if (sc==0x50) return 14;
    if (sc==0x4B) return 17;
    if (sc==0x4D) return 18;
    if (sc<128) {
        char c = shift_pressed ? (char)keyboard_map_shift[sc]
                               : (char)keyboard_map[sc];
        if (ctrl_pressed) {
            ctrl_pressed=0;
            if (c=='s'||c=='S') return 19;   // Ctrl+S → sauvegarder
            if (c=='n'||c=='N') return 29;   // Ctrl+N → nouveau (29, distinct de flèche bas=14)
            if (c>='a'&&c<='z') return c-'a'+1;
            if (c>='A'&&c<='Z') return c-'A'+1;
            return 0;
        }
        return c;
    }
    return 0;
}

// ============================================================
// Gestion souris
// ============================================================
static void process_mouse(void) {
    int mx=g_mouse.x, my=g_mouse.y;
    int left     =  g_mouse.btn_left;
    int left_down=  left && !g_prev_left;
    int left_up  = !left &&  g_prev_left;

    WinID top=z_top_at(mx,my);
    if (left_down && top!=APPCORE_INVALID) {
        z_bring_front(top);
        for (int i=0; i<AC_MAX_WINDOWS; i++)
            if (g_wins[i].used) g_wins[i].dirty=1;
    }

    WinID focus=focused_win();
    if (focus!=APPCORE_INVALID) {
        Window* fw=&g_wins[focus];
        int close_x=fw->x+fw->w-AC_WIN_TITLE_H;
        int in_title=hit(mx,my, fw->x, fw->y, fw->w-AC_WIN_TITLE_H, AC_WIN_TITLE_H);
        if (left_down && in_title) {
            fw->dragging=1; fw->drag_ox=mx-fw->x; fw->drag_oy=my-fw->y;
        }
        if (!left) fw->dragging=0;
        if (fw->dragging && left) {
            int nx=mx-fw->drag_ox, ny=my-fw->drag_oy;
            int sw=(int)vesa_width(), sh=(int)vesa_height();
            if (nx<0) nx=0; if (ny<0) ny=0;
            if (nx+fw->w>sw) nx=sw-fw->w;
            if (ny+win_total_h(focus)>sh-32) ny=sh-32-win_total_h(focus);
            fw->x=nx; fw->y=ny; fw->dirty=1;
        }
        int in_close=hit(mx,my, close_x, fw->y, AC_WIN_TITLE_H, AC_WIN_TITLE_H);
        if (left_down && in_close) {
            if (fw->close_cb) fw->close_cb(focus);
            else app_close_window(focus);
        }
    }

    // Boutons
    for (int i=0; i<AC_MAX_BUTTONS; i++) {
        Button* b=&g_btns[i];
        if (!b->used || !b->enabled) continue;
        WinID wid=b->win;
        if (!g_wins[wid].open) continue;
        if (wid!=focused_win()) {
            if (b->hovered||b->pressed) { b->hovered=b->pressed=0; g_wins[wid].dirty=1; }
            continue;
        }
        int ax=win_cx(wid)+b->x, ay=win_cy(wid)+b->y;
        int over=hit(mx,my,ax,ay,b->w,b->h);
        int was=b->hovered; b->hovered=over;
        if (b->hovered!=was) g_wins[wid].dirty=1;
        if (left_down && over) { b->pressed=1; g_wins[wid].dirty=1; }
        if (left_up && b->pressed) {
            b->pressed=0;
            if (over) { b->touched=1; if (b->cb) b->cb(i); }
            g_wins[wid].dirty=1;
        }
    }

    // Listboxes
    for (int i=0; i<AC_MAX_LISTBOXES; i++) {
        Listbox* lb=&g_lsts[i];
        if (!lb->used) continue;
        WinID wid=lb->win;
        if (!g_wins[wid].open || wid!=focused_win()) continue;

        int ax=win_cx(wid)+lb->x, ay=win_cy(wid)+lb->y;
        int visible=lb->h/lb->item_h;

        // Hover
        int new_hov=-1;
        if (hit(mx,my,ax,ay,lb->w,lb->h)) {
            int rel=(my-ay)/lb->item_h;
            int idx=lb->scroll+rel;
            if (idx>=0 && idx<lb->count) new_hov=idx;
        }
        if (new_hov!=lb->hovered) { lb->hovered=new_hov; g_wins[wid].dirty=1; }

        // Scroll molette (via flèches haut/bas gérées dans app_tick)
        // Clic simple → sélection
        if (left_down && new_hov>=0) {
            if (new_hov==lb->last_click_idx && new_hov==lb->selected) {
                lb->activated=1;   // double-clic détecté
            }
            lb->last_click_idx=new_hov;
            lb->selected=new_hov;
            lb->clicked=1;
            g_wins[wid].dirty=1;
        }

        // Scroll : si sélection hors vue
        if (lb->selected>=0) {
            if (lb->selected < lb->scroll)
                { lb->scroll=lb->selected; g_wins[wid].dirty=1; }
            if (lb->selected >= lb->scroll+visible)
                { lb->scroll=lb->selected-visible+1; g_wins[wid].dirty=1; }
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
    g_last_tick_key = key;   // rendre disponible via app_tick_get_key()
    mouse_poll();
    process_mouse();
    if (key) {
        WinID focus=focused_win();
        if (focus!=APPCORE_INVALID && g_wins[focus].key_cb)
            g_wins[focus].key_cb(focus, key);
        // Scroll listbox avec flèches
        for (int i=0; i<AC_MAX_LISTBOXES; i++) {
            Listbox* lb=&g_lsts[i];
            if (!lb->used || lb->win!=focus) continue;
            int visible=lb->h/lb->item_h;
            if (key==16 && lb->selected>0) {             // haut
                lb->selected--; lb->clicked=1;
                if (lb->selected<lb->scroll) lb->scroll=lb->selected;
                g_wins[focus].dirty=1;
            } else if (key==14 && lb->selected<lb->count-1) { // bas
                lb->selected++; lb->clicked=1;
                if (lb->selected>=lb->scroll+visible) lb->scroll++;
                g_wins[focus].dirty=1;
            } else if (key=='\n' && lb->selected>=0) {
                lb->activated=1;
            }
        }
    }
    if (any_dirty()) redraw_all();
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
    str_cpy(g_lbls[lid].text, text, 128); g_wins[g_lbls[lid].win].dirty=1;
}

void app_set_label_color(LblID lid, uint32_t color) {
    if (lid<0||lid>=AC_MAX_LABELS||!g_lbls[lid].used) return;
    g_lbls[lid].color=color; g_wins[g_lbls[lid].win].dirty=1;
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