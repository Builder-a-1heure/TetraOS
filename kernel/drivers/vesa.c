#include "../drivers/vesa.h"
#include "../drivers/vesa_font.h"
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

// ============================================================
// Backbuffer — composition par couches sans flicker
//
// Toutes les primitives de dessin écrivent ici.
// vesa_flip() copie le backbuffer dans le framebuffer physique
// en une seule passe — une seule écriture visible par frame.
//
// Taille fixe 800×600×4 = 1.83 MB dans le BSS (pas de coût dans .bin).
// Si la résolution est plus petite, seule la portion utilisée est blittée.
// ============================================================
static uint32_t g_backbuf[VESA_BB_W * VESA_BB_H];

// Retourne un pointeur direct sur le backbuffer (pour wallpaper_blit, etc.)
uint32_t* vesa_backbuf(void) { return g_backbuf; }

// Flush : copie le backbuffer dans le framebuffer physique.
// À appeler une fois par frame, après avoir tout composé.
void vesa_flip(void) {
    if (!g_vesa_active) return;
    uint32_t w = g_width  < VESA_BB_W ? g_width  : VESA_BB_W;
    uint32_t h = g_height < VESA_BB_H ? g_height : VESA_BB_H;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)g_fb_addr + y * g_pitch);
        uint32_t* src = g_backbuf + y * VESA_BB_W;
        for (uint32_t x = 0; x < w; x++) dst[x] = src[x];
    }
}

// Quand activé, draw_glyph_raw ne peint PAS les pixels de fond (bg) —
// le wallpaper reste visible derrière le texte.
// Activé par vesa_set_transparent_bg(1) au démarrage du bureau.
static int g_transparent_bg   = 0;
void vesa_set_transparent_bg(int on) { g_transparent_bg = on; }

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

// Dirty buffer : stocke le dernier état rendu de chaque cellule.
// On utilise les couleurs 32bpp complètes pour éviter les collisions
// entre couleurs non répertoriées (bug texte UI qui disparaît).
// Optimisation mémoire : on tronque à 24 bits (bits 23:0) — suffisant
// pour distinguer toutes les couleurs de la palette.
typedef struct {
    char     ch;
    uint8_t  fg_b0;   // couleur fg bits  7:0
    uint8_t  fg_b1;   // couleur fg bits 15:8
    uint8_t  fg_b2;   // couleur fg bits 23:16
    uint8_t  bg_b0;
    uint8_t  bg_b1;
    uint8_t  bg_b2;
} DrawnCell;  // 7 bytes par cellule
static DrawnCell g_drawn[DIRTY_ROWS][DIRTY_COLS];
static int       g_dirty_init = 0;

static void dirty_reset(void) {
    for (int r = 0; r < DIRTY_ROWS; r++)
        for (int c = 0; c < DIRTY_COLS; c++) {
            // 0xFF dans ch = valeur impossible → force le redraw
            g_drawn[r][c].ch   = (char)0xFF;
            g_drawn[r][c].fg_b0 = 0xFF;
            g_drawn[r][c].fg_b1 = 0xFF;
            g_drawn[r][c].fg_b2 = 0xFF;
            g_drawn[r][c].bg_b0 = 0xFF;
            g_drawn[r][c].bg_b1 = 0xFF;
            g_drawn[r][c].bg_b2 = 0xFF;
        }
    g_dirty_init = 1;
}

// Marque toutes les cellules comme "propres" → le prochain render_vesa()
// ne redessinera rien. Utilisé par screen_begin_ui() pour empêcher que
// render_vesa() écrase le wallpaper avec les cellules texte du scrollback.
static void dirty_clear(void) {
    if (!g_dirty_init) dirty_reset();
    for (int r = 0; r < DIRTY_ROWS; r++)
        for (int c = 0; c < DIRTY_COLS; c++)
            g_drawn[r][c].ch = ' ';  // espace = rien à redessiner
    g_dirty_init = 1;
}

void vesa_invalidate_none(void) { dirty_clear(); }

// ============================================================
// Écriture d'un pixel dans le backbuffer (32bpp uniquement)
// put32 n'écrit JAMAIS directement dans g_fb_addr — c'est vesa_flip() qui flush.
static inline void put32(int x, int y, uint32_t color) {
    if ((uint32_t)x >= VESA_BB_W || (uint32_t)y >= VESA_BB_H) return;
    g_backbuf[y * VESA_BB_W + x] = color;
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
            if (g_transparent_bg) {
                // Mode bureau : ne peindre que les pixels FG (texte),
                // laisser le wallpaper intact sur les pixels de fond.
                if (bits & 0x80) line[0] = fg;
                if (bits & 0x40) line[1] = fg;
                if (bits & 0x20) line[2] = fg;
                if (bits & 0x10) line[3] = fg;
                if (bits & 0x08) line[4] = fg;
                if (bits & 0x04) line[5] = fg;
                if (bits & 0x02) line[6] = fg;
                if (bits & 0x01) line[7] = fg;
            } else {
                line[0] = (bits & 0x80) ? fg : bg;
                line[1] = (bits & 0x40) ? fg : bg;
                line[2] = (bits & 0x20) ? fg : bg;
                line[3] = (bits & 0x10) ? fg : bg;
                line[4] = (bits & 0x08) ? fg : bg;
                line[5] = (bits & 0x04) ? fg : bg;
                line[6] = (bits & 0x02) ? fg : bg;
                line[7] = (bits & 0x01) ? fg : bg;
            }
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
        // Comparaison couleur complète sur 24 bits (bits 23:0)
        uint8_t fg0 = fg & 0xFF, fg1 = (fg>>8)&0xFF, fg2 = (fg>>16)&0xFF;
        uint8_t bg0 = bg & 0xFF, bg1 = (bg>>8)&0xFF, bg2 = (bg>>16)&0xFF;
        if (cell->ch == c &&
            cell->fg_b0 == fg0 && cell->fg_b1 == fg1 && cell->fg_b2 == fg2 &&
            cell->bg_b0 == bg0 && cell->bg_b1 == bg1 && cell->bg_b2 == bg2)
            return;
        cell->ch    = c;
        cell->fg_b0 = fg0; cell->fg_b1 = fg1; cell->fg_b2 = fg2;
        cell->bg_b0 = bg0; cell->bg_b1 = bg1; cell->bg_b2 = bg2;
    }
    draw_glyph_raw(px, py, c, fg, bg);
}

// Force le redraw de la cellule (col, row) au prochain vesa_draw_glyph
void vesa_invalidate_cell(int col, int row) {
    if (!g_dirty_init) dirty_reset();
    if (col >= 0 && col < DIRTY_COLS && row >= 0 && row < DIRTY_ROWS) {
        g_drawn[row][col].ch    = (char)0xFF;
        g_drawn[row][col].fg_b0 = 0xFF; g_drawn[row][col].fg_b1 = 0xFF; g_drawn[row][col].fg_b2 = 0xFF;
        g_drawn[row][col].bg_b0 = 0xFF; g_drawn[row][col].bg_b1 = 0xFF; g_drawn[row][col].bg_b2 = 0xFF;
    }
}

// Invalide tout le dirty buffer — le prochain render redessinera toutes les cellules
void vesa_invalidate_all(void) {
    dirty_reset();
}

// Invalide les cellules du dirty buffer couvertes par le rectangle pixel (x,y,w,h).
// À appeler après tout write direct dans le framebuffer (gfx_fill_rect, curseur…)
// pour que le prochain vesa_draw_glyph force le redraw des glyphes concernés.
void vesa_invalidate_rect(int x, int y, int w, int h) {
    if (!g_dirty_init) dirty_reset();
    if (w <= 0 || h <= 0) return;
    int col0 = x / FONT_W;
    int row0 = y / FONT_H;
    int col1 = (x + w - 1) / FONT_W;
    int row1 = (y + h - 1) / FONT_H;
    if (col0 < 0) col0 = 0;
    if (row0 < 0) row0 = 0;
    if (col1 >= DIRTY_COLS) col1 = DIRTY_COLS - 1;
    if (row1 >= DIRTY_ROWS) row1 = DIRTY_ROWS - 1;
    for (int r = row0; r <= row1; r++) {
        for (int c = col0; c <= col1; c++) {
            g_drawn[r][c].ch    = (char)0xFF;
            g_drawn[r][c].fg_b0 = 0xFF; g_drawn[r][c].fg_b1 = 0xFF; g_drawn[r][c].fg_b2 = 0xFF;
            g_drawn[r][c].bg_b0 = 0xFF; g_drawn[r][c].bg_b1 = 0xFF; g_drawn[r][c].bg_b2 = 0xFF;
        }
    }
}

void vesa_clear_glyph(int col, int row, uint32_t bg) {
    vesa_draw_glyph(col * FONT_W, row * FONT_H, ' ', bg, bg);
}

void vesa_put_pixel(int x, int y, uint32_t color) {
    if ((uint32_t)x >= VESA_BB_W || (uint32_t)y >= VESA_BB_H) return;
    g_backbuf[y * VESA_BB_W + x] = color;
}

uint32_t vesa_get_pixel(int x, int y) {
    if ((uint32_t)x >= VESA_BB_W || (uint32_t)y >= VESA_BB_H) return 0;
    return g_backbuf[y * VESA_BB_W + x];
}

void vesa_fill(uint32_t color) {
    if (!g_vesa_active) return;
    uint32_t w = g_width  < VESA_BB_W ? g_width  : VESA_BB_W;
    uint32_t h = g_height < VESA_BB_H ? g_height : VESA_BB_H;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* line = g_backbuf + y * VESA_BB_W;
        for (uint32_t x = 0; x < w; x++) line[x] = color;
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