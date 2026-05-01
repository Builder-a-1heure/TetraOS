// ui/desktop.c — Bureau graphique TetraOS
//
// Responsabilités UNIQUEMENT :
//   - Fond dégradé + taskbar
//   - Icônes dynamiques (scanner TEX)
//   - Hover + clic → launch_app()
//   - Souris + curseur
//
// Tout ce qui concerne le terminal (buffer, chrome fenêtre, titlebar,
// boucle shell) est dans apps/terminal.c.
//
// Découverte des apps :
//   tex_scan() parcourt [_kernel_start, _kernel_end[ à la recherche
//   du magic 0x54455800. Chaque TexHeader valide avec APP_FLAG_DESKTOP
//   devient une icône. desktop.c ne connaît aucune app à la compilation.

#include "desktop.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../ui/session.h"
#include "../lib/utils.h"
#include "../apps/app.h"
#include <stdint.h>

// ============================================================
// Symboles linker — bornes de la mémoire kernel
// ============================================================
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

// ============================================================
// Palette bureau
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
#define DT_ACCENT       0x0000AAFF

// Couleurs graphismes icônes
#define DT_TERM_PROMPT  0x0033CCFF
#define DT_TERM_FG      0x00CCDDFF
#define DT_PAGE_BG      0x00D8E8F8
#define DT_PAGE_LN      0x003366AA
#define DT_FOLD_TOP     0x00CC9900
#define DT_FOLD_BG      0x00FFCC33
#define DT_GEAR_COL     0x00AAAACC

// ============================================================
// Icônes bureau
// ============================================================
#define ICON_W            72
#define ICON_H            72
#define ICON_LABEL_H      14
#define ICON_STRIDE       (ICON_H + ICON_LABEL_H + 16)
#define ICON_COL_X        80
#define ICON_START_Y      60
#define DESKTOP_MAX_ICONS 8

typedef struct {
    int        x, y;
    int        hovered;
    TexHeader* hdr;
} IconState;

// ============================================================
// État global
// ============================================================
static IconState g_icons[DESKTOP_MAX_ICONS];
static int       g_icon_count = 0;
static int       g_prev_left  = 0;

// ============================================================
// Scanner TEX
// ============================================================
static void tex_scan(void) {
    g_icon_count = 0;

    // Scanner uniquement sur des adresses alignées à 4 octets.
    // Le magic 0x54455800 peut apparaître par hasard dans le code ou
    // les données — l'alignement + les validations ci-dessous éliminent
    // les faux positifs qui faisaient planter le bureau au démarrage.
    uintptr_t start = (uintptr_t)_kernel_start;
    if (start & 3) start = (start + 4) & ~(uintptr_t)3;
    uintptr_t end = (uintptr_t)_kernel_end;
    if (end > sizeof(TexHeader)) end -= sizeof(TexHeader);

    for (uintptr_t addr = start; addr < end && g_icon_count < DESKTOP_MAX_ICONS; addr += 4) {
        if (*(uint32_t*)addr != TEX_MAGIC) continue;

        TexHeader* hdr = (TexHeader*)addr;

        // Nom ASCII valide et non vide
        if (hdr->name[0] < 32 || hdr->name[0] >= 127) continue;

        // entry dans les bornes kernel
        uint8_t* eptr = (uint8_t*)(uintptr_t)hdr->entry;
        if (eptr < _kernel_start || eptr >= _kernel_end) continue;

        // reserved doit être 0 (non encore utilisé)
        if (hdr->reserved != 0) continue;

        if (hdr->flags & APP_FLAG_DESKTOP) {
            g_icons[g_icon_count].hdr     = hdr;
            g_icons[g_icon_count].x       = ICON_COL_X;
            g_icons[g_icon_count].y       = ICON_START_Y + g_icon_count * ICON_STRIDE;
            g_icons[g_icon_count].hovered = 0;
            g_icon_count++;
        }
    }
}

// ============================================================
// Hit-test icône
// ============================================================
static int icon_hit(int idx, int mx, int my) {
    int cx = g_icons[idx].x, cy = g_icons[idx].y;
    return mx >= cx - ICON_W/2 && mx <= cx + ICON_W/2 &&
           my >= cy - ICON_H/2 && my <= cy + ICON_H/2;
}

// ============================================================
// Dessin graphisme icône selon AppIconType
// ============================================================
static void draw_icon_gfx(int ix, int iy, AppIconType type) {
    int x = ix + 6, y = iy + 8;
    int w = ICON_W - 12, h = ICON_H - 22;

    switch (type) {

        case APPICON_TERMINAL:
            gfx_fill_rect(x, y, w, h, 0x00000000);
            gfx_draw_rect(x, y, w, h, DT_ICON_BORDER);
            gfx_fill_rect(x+1, y+1, w-2, 5, DT_ICON_BORDER);
            gfx_draw_text(x+3, y+8,  ">", DT_TERM_PROMPT, 0x00000000);
            gfx_draw_text(x+3, y+18, "_", DT_TERM_FG,     0x00000000);
            break;

        case APPICON_TEXTEDIT: {
            int pw = w-4, ph = h-2, px = x+2, py = y;
            gfx_fill_rect(px, py, pw, ph, DT_PAGE_BG);
            gfx_draw_rect(px, py, pw, ph, DT_PAGE_LN);
            int fold = 8;
            for (int fi = 0; fi < fold; fi++)
                gfx_fill_rect(px+pw-fold+fi, py+fi, fold-fi, 1, 0x00FFFFFF);
            gfx_fill_rect(px+pw-fold, py, fold, fold, 0x00AABBCC);
            for (int li = 0; li < 3; li++)
                gfx_fill_rect(px+4, py+8+li*8, (li==2)?pw/2:pw-8, 2, DT_PAGE_LN);
            break;
        }

        case APPICON_FILEMAN: {
            int bx = x+2, by = y+6, bw = w-4, bh = h-8;
            gfx_fill_rect(bx, by,   bw/2, 6,    DT_FOLD_TOP);
            gfx_fill_rect(bx, by+6, bw,   bh-6, DT_FOLD_BG);
            gfx_draw_rect(bx, by+6, bw,   bh-6, DT_FOLD_TOP);
            break;
        }

        case APPICON_SETTINGS: {
            int cx = x+w/2, cy = y+h/2, r = h/2-4, t = 4;
            gfx_fill_rect(cx-r/2, cy-r/2, r,   r,   DT_GEAR_COL);
            gfx_fill_rect(cx-t/2, y+1,    t,   r/2, DT_GEAR_COL);
            gfx_fill_rect(cx-t/2, y+h-r/2,t,   r/2, DT_GEAR_COL);
            gfx_fill_rect(x+1,    cy-t/2, r/2, t,   DT_GEAR_COL);
            gfx_fill_rect(x+w-r/2,cy-t/2, r/2, t,   DT_GEAR_COL);
            break;
        }

        default:
        case APPICON_GENERIC:
            gfx_fill_rect(x, y, w, h, 0x00001A2A);
            gfx_draw_rect(x, y, w, h, DT_ICON_BORDER);
            gfx_draw_text(x+w/2-4, y+h/2-FONT_H/2, "?", DT_WHITE, 0x00001A2A);
            break;
    }
}

// ============================================================
// Dessin d'une icône (fond + graphisme + label)
// ============================================================
static void draw_icon(int idx) {
    IconState* st  = &g_icons[idx];
    TexHeader* hdr = st->hdr;
    int x   = st->x - ICON_W/2;
    int y   = st->y - ICON_H/2;
    int hov = st->hovered;

    gfx_fill_rect(x, y, ICON_W, ICON_H, hov ? DT_ICON_HOVER : DT_ICON_BG);
    gfx_draw_rect(x, y, ICON_W, ICON_H, hov ? DT_ACCENT     : DT_ICON_BORDER);
    draw_icon_gfx(x, y, (AppIconType)hdr->icon_type);

    // Label — fond échantillonné depuis le framebuffer
    int ly = y + ICON_H + 3;
    int cx = st->x, cy = ly + FONT_H/2;
    uint32_t sw = vesa_width(), sh = vesa_height();
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
    if ((uint32_t)cx >= sw) cx = (int)sw-1;
    if ((uint32_t)cy >= sh) cy = (int)sh-1;
    gfx_draw_text_centered(x, ly, ICON_W, hdr->name, DT_ICON_TEXT,
                           vesa_get_pixel(cx, cy));
}

static void draw_all_icons(void) {
    for (int i = 0; i < g_icon_count; i++) draw_icon(i);
}

// ============================================================
// Background + taskbar
// ============================================================

// Repeint uniquement la zone de fond derrière une icône (pas tout l'écran).
// Le dégradé est calculé avec les mêmes paramètres que draw_background()
// de façon à ce que la couleur de chaque ligne soit identique.
static void redraw_background_rect(int x, int y, int w, int h) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    uint8_t r0 = (DT_BG_TOP >> 16) & 0xFF;
    uint8_t g0 = (DT_BG_TOP >>  8) & 0xFF;
    uint8_t b0 = (DT_BG_TOP      ) & 0xFF;
    uint8_t r1 = (DT_BG_BOT >> 16) & 0xFF;
    uint8_t g1 = (DT_BG_BOT >>  8) & 0xFF;
    uint8_t b1 = (DT_BG_BOT      ) & 0xFF;
    int total_h = (int)sh;
    if (total_h < 2) total_h = 2;
    int x2 = x + w; if (x2 > (int)sw) x2 = (int)sw;
    int y2 = y + h; if (y2 > (int)sh) y2 = (int)sh;
    for (int row = y; row < y2; row++) {
        uint8_t r = (uint8_t)((int)r0 + ((int)(r1 - r0) * row) / (total_h - 1));
        uint8_t gc = (uint8_t)((int)g0 + ((int)(g1 - g0) * row) / (total_h - 1));
        uint8_t b = (uint8_t)((int)b0 + ((int)(b1 - b0) * row) / (total_h - 1));
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)gc << 8) | b;
        gfx_fill_rect(x, row, x2 - x, 1, c);
    }
}

static void draw_background(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    gfx_gradient_v(0, 0, (int)sw, (int)sh, DT_BG_TOP, DT_BG_BOT);
    vesa_invalidate_all();
}

static void draw_taskbar(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    int tbh = 32, tby = (int)sh - tbh;
    gfx_fill_rect(0, tby, (int)sw, tbh, DT_TASKBAR_BG);
    gfx_fill_rect(0, tby, (int)sw, 1,   DT_TASKBAR_LINE);
    gfx_draw_text(10, tby + (tbh-FONT_H)/2,
                  session_get_current_name(), DT_CLOCK_FG, DT_TASKBAR_BG);
    gfx_draw_text_centered(0, tby + (tbh-FONT_H)/2, (int)sw,
                           "TetraOS Desktop", DT_WHITE, DT_TASKBAR_BG);
}

// ============================================================
// Redraw bureau complet
// ============================================================
static void desktop_redraw(void) {
    screen_begin_ui();
    draw_background();
    draw_taskbar();
    for (int i = 0; i < g_icon_count; i++)
        g_icons[i].hovered = icon_hit(i, g_mouse.x, g_mouse.y);
    draw_all_icons();
    screen_end_ui();
}

// ============================================================
// Lancement d'une app
// ============================================================
static void launch_app(int idx) {
    mouse_erase_cursor();
    g_icons[idx].hdr->entry();
    // Retour ici après fermeture de l'app
    mouse_erase_cursor();
    desktop_redraw();
    mouse_draw_cursor();
}

// ============================================================
// Loop du Desktop
// ============================================================
void desktop_run(void) {
    tex_scan();

    g_prev_left = 0;
    desktop_redraw();
    mouse_draw_cursor();

    while (g_session_manager.logged_in) {

        int mouse_moved = 0;
        while (mouse_poll()) mouse_moved = 1;

        int cur_left  = g_mouse.btn_left;
        int prev_left = g_prev_left;

        if (mouse_moved || cur_left != prev_left) {

            // Détecter quelles icônes changent d'état hover
            int any_changed = 0;
            for (int i = 0; i < g_icon_count; i++) {
                int hov = icon_hit(i, g_mouse.x, g_mouse.y);
                if (hov != g_icons[i].hovered) {
                    any_changed = 1;
                    break;
                }
            }

            // On efface le curseur une seule fois pour les deux cas
            mouse_erase_cursor();

            if (any_changed) {
                // Redessiner UNIQUEMENT les icônes dont le hover a changé.
                // On ne touche pas au fond global ni à la taskbar — aucun flash.
                for (int i = 0; i < g_icon_count; i++) {
                    int hov = icon_hit(i, g_mouse.x, g_mouse.y);
                    if (hov != g_icons[i].hovered) {
                        g_icons[i].hovered = hov;
                        // Zone de l'icône + son label (marge basse ICON_LABEL_H + 6)
                        int ix = g_icons[i].x - ICON_W/2;
                        int iy = g_icons[i].y - ICON_H/2;
                        int iw = ICON_W;
                        int ih = ICON_H + ICON_LABEL_H + 6;
                        // Repeindre le fond uniquement sur cette zone
                        redraw_background_rect(ix, iy, iw, ih);
                        // Redessiner l'icône avec son nouvel état hover
                        draw_icon(i);
                    }
                }
            }

            // Redessiner le curseur à sa nouvelle position (qu'il ait bougé ou non)
            mouse_draw_cursor();

            if (cur_left && !prev_left) {
                for (int i = 0; i < g_icon_count; i++) {
                    if (icon_hit(i, g_mouse.x, g_mouse.y)) {
                        launch_app(i);
                        if (!g_session_manager.logged_in) goto desktop_exit;
                        break;
                    }
                }
            }
        }

        g_prev_left = cur_left;
    }

desktop_exit:
    screen_exit_ui();
}