// lib/appcore.h — AppCore v2 : framework UI de TetraOS
// ============================================================
//
// API orientée "framework" : une instance App globale gère
// toutes les fenêtres, le focus, l'ordre-Z, la souris et le
// clavier. L'application ne touche jamais aux drivers.
//
// Usage type :
//
//   void my_app(void) {
//       app_init();
//
//       WinID w = app_new_window("Ma fenêtre", 100, 80, 400, 300);
//       BtnID b = app_new_button(w, 20, 20, 120, 32, "Cliquez !");
//       LblID l = app_new_label (w, 20, 70, "Statut : en attente");
//
//       // Callback optionnel
//       app_on_click(b, on_btn_click);
//
//       while (app_running()) {
//           app_tick();   // souris + clavier + redraw si nécessaire
//
//           // Poll style
//           if (app_button_touched(b)) {
//               app_set_label_text(l, "Bouton cliqué !");
//           }
//       }
//   }
//
// ============================================================

#ifndef APPCORE_H
#define APPCORE_H

#include <stdint.h>

// ============================================================
// Handles opaques
// ============================================================
typedef int WinID;   // identifiant de fenêtre  (-1 = invalide)
typedef int BtnID;   // identifiant de bouton   (-1 = invalide)
typedef int LblID;   // identifiant de label    (-1 = invalide)

#define APPCORE_INVALID  -1

// ============================================================
// Callbacks
// ============================================================
typedef void (*AppClickCb)(BtnID btn);    // appelé au relâchement
typedef void (*AppCloseCb)(WinID win);    // appelé quand [X] cliqué
typedef void (*AppKeyCb)(WinID win, char key); // touche non-consommée

// ============================================================
// Palette (modifiable avant app_init)
// ============================================================
// Fenêtre
#define AC_WIN_BG          0x00101820
#define AC_WIN_TITLE_H     26
#define AC_WIN_TITLE_GRAD0 0x00003A88
#define AC_WIN_TITLE_GRAD1 0x00001844
#define AC_WIN_TITLE_FOCUS 0x0000AAFF
#define AC_WIN_BORDER      0x000055CC
#define AC_WIN_BORDER_FOC  0x0000AAFF
#define AC_WIN_CLOSE_BG    0x00AA1111
#define AC_WIN_CLOSE_HOV   0x00DD2222
#define AC_WIN_SHADOW_A    120         // alpha ombre

// Bouton
#define AC_BTN_BG          0x00002255
#define AC_BTN_HOV         0x00003377
#define AC_BTN_PRESS       0x000011AA
#define AC_BTN_DISABLED    0x00111111
#define AC_BTN_BORDER      0x000066CC
#define AC_BTN_FG          0x00FFFFFF
#define AC_BTN_FG_DIS      0x00555555

// Label
#define AC_LBL_FG          0x00AABBCC

// ============================================================
// Limites internes
// ============================================================
#define AC_MAX_WINDOWS   16
#define AC_MAX_BUTTONS   64
#define AC_MAX_LABELS    64

// ============================================================
// API — Initialisation
// ============================================================

// À appeler UNE FOIS avant tout, au démarrage de l'application.
void app_init(void);

// Retourne 1 tant qu'il reste au moins une fenêtre ouverte.
int  app_running(void);

// Traite un cycle complet : souris, clavier, redraw.
// À appeler en boucle dans while(app_running()).
void app_tick(void);

// ============================================================
// API — Fenêtres
// ============================================================

// Crée une nouvelle fenêtre. w, h = dimensions zone client.
// Retourne un WinID, ou APPCORE_INVALID si limite atteinte.
WinID app_new_window(const char* title, int x, int y, int w, int h);

// Ferme et supprime une fenêtre (libère ses widgets).
void  app_close_window(WinID wid);

// Change le titre d'une fenêtre.
void  app_set_title(WinID wid, const char* title);

// Enregistre un callback appelé quand [X] est cliqué.
// Si NULL, la fenêtre se ferme automatiquement.
void  app_on_close(WinID wid, AppCloseCb cb);

// ============================================================
// API — Boutons
// ============================================================

// Crée un bouton dans la fenêtre wid.
// x, y relatifs à la zone client. Retourne un BtnID.
BtnID app_new_button(WinID wid, int x, int y, int w, int h,
                     const char* label);

// Active ou grise un bouton (grisé = non cliquable).
void  app_set_button_enabled(BtnID bid, int enabled);

// Change le libellé d'un bouton.
void  app_set_button_label(BtnID bid, const char* label);

// --- Poll style ---
// Retourne 1 si le bouton vient d'être cliqué (se remet à 0 après lecture).
int   app_button_touched(BtnID bid);

// Retourne 1 si AppCore a effectué un redraw_all() lors du dernier app_tick().
// Permet aux apps qui dessinent par-dessus le chrome de savoir quand
// repeindre leur contenu (sinon il est écrasé par le redraw chrome).
// Remis à 0 automatiquement au début de chaque app_tick().
int  app_was_redrawn(void);

// --- Callback style ---
// Enregistre un callback appelé automatiquement au clic.
void  app_on_click(BtnID bid, AppClickCb cb);

// ============================================================
// API — Labels
// ============================================================

// Crée un label texte dans la fenêtre wid.
LblID app_new_label(WinID wid, int x, int y, const char* text);

// Modifie le texte d'un label existant.
void  app_set_label_text(LblID lid, const char* text);

// Modifie la couleur du texte d'un label.
void  app_set_label_color(LblID lid, uint32_t color);

// ============================================================
// API — Clavier global
// ============================================================

// Enregistre un callback appelé pour chaque touche non-consommée
// par un widget, sur la fenêtre wid.
void  app_on_key(WinID wid, AppKeyCb cb);

// Récupère la position absolue (coin haut-gauche) de la fenêtre wid.
// Utile pour détecter un drag depuis l'extérieur d'AppCore.
void app_get_win_pos(WinID wid, int* out_x, int* out_y);

#endif // APPCORE_H