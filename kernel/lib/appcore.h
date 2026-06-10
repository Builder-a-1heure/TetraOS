// lib/appcore.h — AppCore v2 : framework UI de TetraOS
// ============================================================
//
// Widgets disponibles :
//   - Fenêtres  (WinID) : titre, drag, [X], z-order, focus
//   - Boutons   (BtnID) : hover, press, callback ou poll
//   - Labels    (LblID) : texte coloré
//   - ListBox   (LstID) : liste d'items scrollable, sélection
//   - DrawArea  (DawID) : zone de dessin libre avec callback
//
// Usage type :
//
//   void my_app(void) {
//       app_init();
//       WinID w = app_new_window("Titre", 100, 80, 400, 300);
//       LstID l = app_new_listbox(w, 10, 10, 380, 200, 20);
//       app_listbox_add(l, "item 1");
//       app_listbox_add(l, "item 2");
//       while (app_running()) {
//           app_tick();
//           if (app_listbox_clicked(l)) {
//               int sel = app_listbox_selected(l);
//               // ...
//           }
//       }
//   }

#ifndef APPCORE_H
#define APPCORE_H

#include <stdint.h>

// ============================================================
// Handles opaques
// ============================================================
typedef int WinID;   // fenêtre   (-1 = invalide)
typedef int BtnID;   // bouton    (-1 = invalide)
typedef int LblID;   // label     (-1 = invalide)
typedef int LstID;   // listbox   (-1 = invalide)
typedef int DawID;   // draw area (-1 = invalide)

#define APPCORE_INVALID  -1

// ============================================================
// Callbacks
// ============================================================
typedef void (*AppClickCb)(BtnID btn);
typedef void (*AppCloseCb)(WinID win);
typedef void (*AppKeyCb)  (WinID win, char key);
typedef void (*AppDrawCb) (WinID win, DawID daw,
                           int abs_x, int abs_y, int w, int h);
// Callback fond desktop — voir app_set_bg_callback()
typedef void (*AppBgCb)   (int x, int y, int w, int h);

// ============================================================
// Palette fenêtre
// ============================================================
#define AC_WIN_BG           0x00101820
#define AC_WIN_TITLE_H      26
#define AC_WIN_TITLE_GRAD0  0x00003A88
#define AC_WIN_TITLE_GRAD1  0x00001844
#define AC_WIN_TITLE_FOCUS  0x0000AAFF
#define AC_WIN_BORDER       0x000055CC
#define AC_WIN_BORDER_FOC   0x0000AAFF
#define AC_WIN_CLOSE_BG     0x00AA1111
#define AC_WIN_CLOSE_HOV    0x00DD2222
#define AC_WIN_SHADOW_A     120

// Bouton
#define AC_BTN_BG           0x00002255
#define AC_BTN_HOV          0x00003377
#define AC_BTN_PRESS        0x000011AA
#define AC_BTN_DISABLED     0x00111111
#define AC_BTN_BORDER       0x000066CC
#define AC_BTN_FG           0x00FFFFFF
#define AC_BTN_FG_DIS       0x00555555

// Label
#define AC_LBL_FG           0x00AABBCC

// ListBox
#define AC_LST_BG           0x00080D14
#define AC_LST_BORDER       0x00003366
#define AC_LST_ITEM_FG      0x00CCDDEE
#define AC_LST_ITEM_BG      0x00080D14
#define AC_LST_SEL_BG       0x00003388
#define AC_LST_SEL_FG       0x00FFFFFF
#define AC_LST_HOV_BG       0x00001A33
#define AC_LST_DIR_FG       0x0055AAFF   // couleur dossiers
#define AC_LST_SCROLL_BG    0x00060A10
#define AC_LST_SCROLL_FG    0x00224466

// ============================================================
// Limites
// ============================================================
#define AC_MAX_WINDOWS   16
#define AC_MAX_BUTTONS   64
#define AC_MAX_LABELS    64
#define AC_MAX_LISTBOXES 16
#define AC_MAX_DRAWAREAS 16
#define AC_LST_MAX_ITEMS 256
#define AC_LST_ITEM_LEN  48

// ============================================================
// API — Initialisation
// ============================================================
void app_init(void);
void app_reset(void);   // reset complet apres logout (libere widgets + bg_callback)
int  app_running(void);
void app_tick(void);

// Retourne la touche lue par le dernier app_tick() (0 = aucune).
// À utiliser à la place de toute lecture directe du port 0x60/0x64
// dans les apps, pour éviter la double lecture qui fait sauter des touches.
char app_tick_get_key(void);

// ============================================================
// API — Fenêtres
// ============================================================
WinID app_new_window   (const char* title, int x, int y, int w, int h);
void  app_close_window (WinID wid);
void  app_set_title    (WinID wid, const char* title);
void  app_on_close     (WinID wid, AppCloseCb cb);
void  app_on_key       (WinID wid, AppKeyCb cb);
void  app_get_win_pos  (WinID wid, int* out_x, int* out_y);
void  app_mark_dirty   (WinID wid);   // force un redraw au prochain tick
int   app_was_redrawn  (void);

// ============================================================
// API — Boutons
// ============================================================
BtnID app_new_button        (WinID wid, int x, int y, int w, int h, const char* label);
void  app_set_button_enabled(BtnID bid, int enabled);
void  app_set_button_label  (BtnID bid, const char* label);
int   app_button_touched    (BtnID bid);
void  app_on_click          (BtnID bid, AppClickCb cb);

// ============================================================
// API — Labels
// ============================================================
LblID app_new_label      (WinID wid, int x, int y, const char* text);
void  app_set_label_text (LblID lid, const char* text);
void  app_set_label_color(LblID lid, uint32_t color);

// ============================================================
// API — ListBox
// ============================================================

// Crée une listbox dans la fenêtre wid.
// item_h = hauteur d'une ligne en pixels (recommandé : 18-22).
LstID app_new_listbox  (WinID wid, int x, int y, int w, int h, int item_h);

// Ajoute un item. is_dir = 1 → affiché en couleur AC_LST_DIR_FG.
void  app_listbox_add  (LstID lid, const char* text, int is_dir);

// Vide tous les items.
void  app_listbox_clear(LstID lid);

// Retourne l'index sélectionné (-1 si aucun).
int   app_listbox_selected(LstID lid);

// Retourne le texte de l'item sélectionné (NULL si aucun).
const char* app_listbox_selected_text(LstID lid);

// Retourne 1 si l'utilisateur vient de double-cliquer (consomme le flag).
int   app_listbox_activated(LstID lid);

// Retourne 1 si la sélection a changé depuis le dernier appel (consomme le flag).
int   app_listbox_clicked(LstID lid);

// Scroll programmatique.
void  app_listbox_scroll_to(LstID lid, int idx);

// ============================================================
// API — Draw Area
// ============================================================

// Zone de dessin libre. Le callback cb est appelé à chaque redraw
// avec les coordonnées absolues (framebuffer) de la zone.
DawID app_new_drawarea(WinID wid, int x, int y, int w, int h, AppDrawCb cb);

// Force le redraw de la draw area au prochain tick.
void  app_drawarea_invalidate(DawID did);

// Enregistre le callback de fond desktop pour l'effacement propre
// des fenêtres déplacées. À appeler depuis desktop.c après app_init().
void  app_set_bg_callback(void (*cb)(int, int, int, int));

#endif // APPCORE_H