#include "vesa.h"
#include "vesa_font.h"
#include <stdint.h>

// ============================================================
// État VESA
// ============================================================
static int      g_vesa_active = 0;
static uint32_t g_fb_addr     = 0;
static uint32_t g_pitch       = 0;
static uint32_t g_width       = 0;
static uint32_t g_height      = 0;
static uint8_t  g_bpp         = 0;
static uint32_t g_bpp_bytes   = 0;

int vesa_active(void)       { return g_vesa_active; }
uint32_t vesa_width(void)   { return g_width;   }
uint32_t vesa_height(void)  { return g_height;  }
uint32_t vesa_pitch(void)   { return g_pitch;   }
uint32_t vesa_fb_addr(void) { return g_fb_addr; }
int vesa_text_cols(void)    { return (int)(g_width  / FONT_W); }
int vesa_text_rows(void)    { return (int)(g_height / FONT_H); }

// ============================================================
// DIRTY BUFFER — ne redessine que les cellules modifiées
// On stocke juste le caractère + un index couleur (fg/bg)
// pour minimiser la taille du BSS
// ============================================================
#define DIRTY_COLS  240
#define DIRTY_ROWS  68

// Index de couleur compact (0=BG, 1=FG, 2=FG_BRIGHT, 3=SCROLL_BG, 4=SCROLL_FG)
typedef struct {
    char    ch;
    uint8_t fg_idx;
    uint8_t bg_idx;
} DrawnCell;  // 3 bytes par cellule au lieu de 9
static DrawnCell g_drawn[DIRTY_ROWS][DIRTY_COLS];
static int       g_dirty_init = 0;

// Convertit une couleur 32bpp en index compact
static uint8_t color_to_idx(uint32_t c) {
    if (c == COLOR_FG)         return 1;
    if (c == COLOR_FG_BRIGHT)  return 2;
    if (c == COLOR_SCROLL_BG)  return 3;
    if (c == COLOR_SCROLL_FG)  return 4;
    return 0; // COLOR_BG ou inconnu
}

static void dirty_reset(void) {
    for (int r = 0; r < DIRTY_ROWS; r++)
        for (int c = 0; c < DIRTY_COLS; c++) {
            g_drawn[r][c].ch     = 0xFF;
            g_drawn[r][c].fg_idx = 0xFF;
            g_drawn[r][c].bg_idx = 0xFF;
        }
    g_dirty_init = 1;
}

// ============================================================
// Écriture directe d'un pixel 32bpp (inline pour perf)
// ============================================================
static inline void put32(int x, int y, uint32_t color) {
    *(uint32_t*)((uint8_t*)g_fb_addr + (uint32_t)y * g_pitch + (uint32_t)x * 4) = color;
}

// ============================================================
// Dessin d'un glyphe — chemin critique optimisé 32bpp
// ============================================================
static void draw_glyph_raw(int px, int py, char c, uint32_t fg, uint32_t bg) {
    const uint8_t* glyph = vga_font[(uint8_t)c];

    if (g_bpp_bytes == 4) {
        for (int row = 0; row < FONT_H; row++) {
            int y = py + row;
            if ((uint32_t)y >= g_height) break;
            uint32_t* line = (uint32_t*)((uint8_t*)g_fb_addr + (uint32_t)y * g_pitch + (uint32_t)px * 4);
            uint8_t bits = glyph[row];
            line[0] = (bits & 0x80) ? fg : bg;
            line[1] = (bits & 0x40) ? fg : bg;
            line[2] = (bits & 0x20) ? fg : bg;
            line[3] = (bits & 0x10) ? fg : bg;
            line[4] = (bits & 0x08) ? fg : bg;
            line[5] = (bits & 0x04) ? fg : bg;
            line[6] = (bits & 0x02) ? fg : bg;
            line[7] = (bits & 0x01) ? fg : bg;
        }
    } else {
        // 24bpp / 16bpp
        uint8_t* fb = (uint8_t*)g_fb_addr;
        for (int row = 0; row < FONT_H; row++) {
            int y = py + row;
            if ((uint32_t)y >= g_height) break;
            uint8_t bits = glyph[row];
            uint32_t base = (uint32_t)y * g_pitch + (uint32_t)px * g_bpp_bytes;
            for (int col = 0; col < FONT_W; col++) {
                uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
                uint32_t off = base + (uint32_t)col * g_bpp_bytes;
                if (g_bpp_bytes == 3) {
                    fb[off+0] = color & 0xFF;
                    fb[off+1] = (color >> 8) & 0xFF;
                    fb[off+2] = (color >> 16) & 0xFF;
                } else if (g_bpp_bytes == 2) {
                    uint16_t r5 = (color>>16)&0xFF, g6=(color>>8)&0xFF, b5=color&0xFF;
                    *(uint16_t*)(fb+off) = (uint16_t)(((r5>>3)<<11)|((g6>>2)<<5)|(b5>>3));
                }
            }
        }
    }
}

void vesa_draw_glyph(int px, int py, char c, uint32_t fg, uint32_t bg) {
    int col = px / FONT_W;
    int row = py / FONT_H;
    if (!g_dirty_init) dirty_reset();
    if (col < DIRTY_COLS && row < DIRTY_ROWS) {
        DrawnCell* cell = &g_drawn[row][col];
        uint8_t fi = color_to_idx(fg);
        uint8_t bi = color_to_idx(bg);
        if (cell->ch == c && cell->fg_idx == fi && cell->bg_idx == bi) return;
        cell->ch     = c;
        cell->fg_idx = fi;
        cell->bg_idx = bi;
    }
    draw_glyph_raw(px, py, c, fg, bg);
}

// Force le redraw de la cellule (col, row) au prochain vesa_draw_glyph
void vesa_invalidate_cell(int col, int row) {
    if (!g_dirty_init) dirty_reset();
    if (col >= 0 && col < DIRTY_COLS && row >= 0 && row < DIRTY_ROWS) {
        g_drawn[row][col].ch     = 0xFF;  // valeur impossible → forcera le redraw
        g_drawn[row][col].fg_idx = 0xFF;
        g_drawn[row][col].bg_idx = 0xFF;
    }
}

// Invalide tout le dirty buffer — le prochain render redessinera toutes les cellules
void vesa_invalidate_all(void) {
    dirty_reset();
}

void vesa_clear_glyph(int col, int row, uint32_t bg) {
    vesa_draw_glyph(col * FONT_W, row * FONT_H, ' ', bg, bg);
}

void vesa_put_pixel(int x, int y, uint32_t color) {
    if ((uint32_t)x >= g_width || (uint32_t)y >= g_height) return;
    if (g_bpp_bytes == 4) { put32(x, y, color); return; }
    uint8_t* fb = (uint8_t*)g_fb_addr;
    uint32_t off = (uint32_t)y * g_pitch + (uint32_t)x * g_bpp_bytes;
    if (g_bpp_bytes == 3) { fb[off]=color&0xFF; fb[off+1]=(color>>8)&0xFF; fb[off+2]=(color>>16)&0xFF; }
}

uint32_t vesa_get_pixel(int x, int y) {
    if ((uint32_t)x >= g_width || (uint32_t)y >= g_height) return 0;
    uint8_t* fb = (uint8_t*)g_fb_addr;
    uint32_t off = (uint32_t)y * g_pitch + (uint32_t)x * g_bpp_bytes;
    if (g_bpp_bytes == 4) return *(uint32_t*)(fb + off);
    if (g_bpp_bytes == 3) return (uint32_t)fb[off] | ((uint32_t)fb[off+1]<<8) | ((uint32_t)fb[off+2]<<16);
    return 0;
}

void vesa_fill(uint32_t color) {
    if (!g_vesa_active) return;
    if (g_bpp_bytes == 4) {
        for (uint32_t y = 0; y < g_height; y++) {
            uint32_t* line = (uint32_t*)((uint8_t*)g_fb_addr + y * g_pitch);
            for (uint32_t x = 0; x < g_width; x++) line[x] = color;
        }
    } else {
        for (uint32_t y = 0; y < g_height; y++)
            for (uint32_t x = 0; x < g_width; x++)
                vesa_put_pixel((int)x, (int)y, color);
    }
    dirty_reset();
}

// ============================================================
// vesa_init — lit la structure laissée par stage2 à 0x9000
// ============================================================
int vesa_init(void) {
    volatile uint32_t* info = (volatile uint32_t*)0x9000;

    // Vérifier le magic 'VESA'
    if (info[0] != VESA_MAGIC) return 0;

    uint32_t fb   = info[1];   // +4  : fb_addr
    uint32_t pitch = info[2];  // +8  : pitch
    uint32_t w    = info[3];   // +12 : width
    uint32_t h    = info[4];   // +16 : height
    uint8_t  bpp  = *(volatile uint8_t*)0x9014;  // +20 : bpp

    // Sanity checks stricts
    if (fb == 0 || w == 0 || h == 0 || pitch == 0 || bpp == 0) return 0;
    if (w > 4096 || h > 4096) return 0;   // valeurs impossibles = données corrompues
    if (bpp != 32 && bpp != 24 && bpp != 16) return 0;

    g_fb_addr   = fb;
    g_pitch     = pitch;
    g_width     = w;
    g_height    = h;
    g_bpp       = bpp;
    g_bpp_bytes = bpp / 8;

    // -------------------------------------------------------
    // TEST VISUEL : écrire quelques pixels de couleur vive
    // dans le coin supérieur gauche AVANT tout le reste.
    // Si l'écran reste noir après ça = fb_addr est mauvais.
    // Si on voit des pixels rouges = VESA fonctionne.
    // -------------------------------------------------------
    if (g_bpp_bytes == 4) {
        uint32_t* fb_ptr = (uint32_t*)g_fb_addr;
        // Bande rouge 4px de haut sur toute la largeur
        for (uint32_t y = 0; y < 4 && y < g_height; y++) {
            uint32_t* line = (uint32_t*)((uint8_t*)g_fb_addr + y * g_pitch);
            for (uint32_t x = 0; x < g_width; x++)
                line[x] = 0x00FF0000;  // rouge pur
        }
        // Bande verte ligne 4-7
        for (uint32_t y = 4; y < 8 && y < g_height; y++) {
            uint32_t* line = (uint32_t*)((uint8_t*)g_fb_addr + y * g_pitch);
            for (uint32_t x = 0; x < g_width; x++)
                line[x] = 0x0000FF00;  // vert pur
        }
        // Bande bleue ligne 8-11
        for (uint32_t y = 8; y < 12 && y < g_height; y++) {
            uint32_t* line = (uint32_t*)((uint8_t*)g_fb_addr + y * g_pitch);
            for (uint32_t x = 0; x < g_width; x++)
                line[x] = 0x000000FF;  // bleu pur
        }
        (void)fb_ptr;
    }
    // Si on arrive ici sans triple fault = fb_addr est valide

    g_vesa_active = 1;
    g_dirty_init  = 0;
    return 1;
}