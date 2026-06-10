#ifndef WALLPAPER_H_INCLUDED
#define WALLPAPER_H_INCLUDED

#include <stdint.h>

// Résolution exacte du wallpaper stocké sur disque.
// Format : RGB24 flat, row-major, R G B R G B ...
// Taille disque : 1920 * 1080 * 3 = 6 220 800 bytes (+ padding secteur éventuel)
#define WALLPAPER_W     1920u
#define WALLPAPER_H     1080u
#define WALLPAPER_SIZE  (WALLPAPER_W * WALLPAPER_H * 3u)   // 6 220 800 bytes

// 1 si le wallpaper a été blitté avec succès dans le framebuffer.
// Le bureau utilise ce flag pour savoir s'il doit tomber sur le fallback noir.
extern int g_wallpaper_loaded;

// Charge wallpaper.bin depuis le FS et le blit immédiatement dans le
// framebuffer VESA pixel par pixel. Pas de buffer RAM intermédiaire.
// Retourne 1 en cas de succès, 0 sinon (fichier absent, FS pas prêt, etc.)
int wallpaper_load(void);

// Reblit la zone (x, y, w, h) depuis le disque.
// Appelé par redraw_background_rect() pour réparer ce qu'une fenêtre a couvert.
// Si le wallpaper n'est pas chargé, remplit en noir.
void wallpaper_blit_rect(int x, int y, int w, int h);

#endif
