#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

// ============================================================
// MODE TEXTE VGA (fallback)
// ============================================================
#define VIDEO_ADDRESS   0xB8000
#define VGA_ROWS        25
#define VGA_COLS        80
#define WHITE_ON_BLACK  0x0F
#define DARK_ON_WHITE   0xF0

// ============================================================
// DIMENSIONS TEXTE DYNAMIQUES
// ============================================================
extern int g_text_cols;
extern int g_text_rows;

// ============================================================
// POLICE
// ============================================================
#define FONT_W  8
#define FONT_H  16

// ============================================================
// SCROLLBACK BUFFER
// ============================================================
#define SCROLLBACK_LINES  200
#define MAX_COLS          240

typedef struct {
    char    ch;
    uint8_t attr;
} VGACell;

extern VGACell scrollback[SCROLLBACK_LINES][MAX_COLS];
extern int sb_write_row;
extern int sb_view;
extern int cursor_row;
extern int cursor_col;

// ============================================================
// Initialisation
// ============================================================
void screen_init(void);

// ============================================================
// HOOK DE SORTIE — redirige print_char/print_string vers une fonction externe
// Utilisé par le terminal fenêtré du bureau. NULL = comportement normal.
// ============================================================
typedef void (*CharOutputFn)(char);
extern CharOutputFn g_print_hook;

// ============================================================
// API texte scrollback
// ============================================================
void clear_screen(void);
void print_char(char c);
void print_string(const char* str);
void set_cursor(int row, int col);
void update_cursor(void);
void print_hex(uint32_t num);
void print_dec(uint32_t num);
void print_int(int num);
void draw_box(int x1, int y1, int x2, int y2);
void clear_area(int x1, int y1, int x2, int y2);

void screen_render(void);
void screen_scroll_up(void);
void screen_scroll_down(void);
int  screen_at_bottom(void);
void screen_goto_bottom(void);
void screen_clear_visible(void);

// ============================================================
// API dirty-flag / rendu groupé UI
// Utiliser screen_begin_ui() avant de dessiner un écran complet,
// screen_end_ui() à la fin : un seul flush, zéro flicker.
// ============================================================
extern int g_ui_drawing;
void screen_begin_ui(void);
void screen_end_ui(void);
void screen_exit_ui(void);
void screen_flush(void);
void screen_invalidate(void);

// ============================================================
// PRIMITIVES GÉOMÉTRIQUES (pixels, VESA uniquement)
// ============================================================

// Rectangles
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);

// Ligne Bresenham
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

// Cercles
void gfx_draw_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);

// Dégradés
void gfx_gradient_h(int x, int y, int w, int h,
                    uint32_t col_left, uint32_t col_right);
void gfx_gradient_v(int x, int y, int w, int h,
                    uint32_t col_top,  uint32_t col_bot);

// Texte en pixels (hors scrollback)
void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void gfx_draw_text_centered(int x, int y, int w,
                             const char* str, uint32_t fg, uint32_t bg);

#endif