#ifndef VESA_H
#define VESA_H

#include <stdint.h>

// ============================================================
// VESA Framebuffer
// Infos passées par stage2 à l'adresse physique 0x8000 :
//   +0  dword : magic  = 0x56455341 ('VESA')
//   +4  dword : fb_addr (adresse physique du framebuffer)
//   +8  dword : pitch   (bytes par ligne)
//   +12 dword : width
//   +16 dword : height
//   +20 byte  : bpp
// ============================================================

#define VESA_INFO_ADDR  0x8000
#define VESA_MAGIC      0x41534556   // 'VESA' little-endian

typedef struct {
    uint32_t magic;
    uint32_t fb_addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
} __attribute__((packed)) VesaInfo;

// Couleurs 32bpp ARGB
#define COLOR_BLACK      0x00000000
#define COLOR_WHITE      0x00FFFFFF
#define COLOR_GRAY       0x00AAAAAA
#define COLOR_DARK_GRAY  0x00555555
#define COLOR_CYAN       0x0000FFFF
#define COLOR_GREEN      0x0000FF00
#define COLOR_RED        0x00FF0000
#define COLOR_YELLOW     0x00FFFF00
#define COLOR_BG         0x00000000   // fond noir
#define COLOR_FG         0x00AAAAAA   // texte gris clair (style VGA)
#define COLOR_FG_BRIGHT  0x00FFFFFF   // texte blanc (attribut bright)
#define COLOR_SCROLL_BG  0x00AAAAAA   // fond barre de scroll
#define COLOR_SCROLL_FG  0x00000000   // texte barre de scroll

// Initialiser le système VESA
// Retourne 1 si VESA disponible, 0 si fallback VGA texte
int vesa_init(void);

// Retourne 1 si le mode VESA est actif
int vesa_active(void);

// Infos du framebuffer courant
uint32_t vesa_width(void);
uint32_t vesa_height(void);
uint32_t vesa_pitch(void);
uint32_t vesa_fb_addr(void);

// Dessiner un pixel
void vesa_put_pixel(int x, int y, uint32_t color);

// Lire la couleur d'un pixel (utile pour sauvegarder le fond sous le curseur)
uint32_t vesa_get_pixel(int x, int y);

// Forcer le redraw d'une cellule texte au prochain vesa_draw_glyph
// (col/row en coordonnées caractères, pas pixels)
void vesa_invalidate_cell(int col, int row);

// Invalide tout le dirty buffer — force un redraw complet au prochain render
void vesa_invalidate_all(void);

// Invalide les cellules couvertes par un rectangle pixel — à appeler après
// tout write direct dans le framebuffer (gfx_fill_rect, curseur, etc.)
void vesa_invalidate_rect(int x, int y, int w, int h);

// Dessiner un glyphe (caractère) à la position pixel (px, py)
void vesa_draw_glyph(int px, int py, char c, uint32_t fg, uint32_t bg);

// Effacer un bloc texte (en coordonnées glyphe col/row)
void vesa_clear_glyph(int col, int row, uint32_t bg);

// Remplir tout l'écran d'une couleur
void vesa_fill(uint32_t color);

// Nombre de colonnes et lignes texte disponibles en VESA
int vesa_text_cols(void);
int vesa_text_rows(void);

void vesa_clear_back(void);
void vesa_flush(void);

#endif // VESA_H