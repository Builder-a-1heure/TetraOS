#include "../gfx/screen.h"
#include "../drivers/vesa.h"

int g_text_cols = VGA_COLS;
int g_text_rows = VGA_ROWS;

// Déclarations forward — définis plus bas comme static
static void render_vesa(void);
static void render_vga(void);

void screen_init(void) {
    if (vesa_init()) {
        g_text_cols = vesa_text_cols();
        g_text_rows = vesa_text_rows();
        if (g_text_cols > MAX_COLS)        g_text_cols = MAX_COLS;
        if (g_text_rows > SCROLLBACK_LINES) g_text_rows = SCROLLBACK_LINES;
    } else {
        g_text_cols = VGA_COLS;
        g_text_rows = VGA_ROWS;
    }
}

// ============================================================
// Port I/O
// ============================================================
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ============================================================
// SCROLLBACK BUFFER
// ============================================================
VGACell scrollback[SCROLLBACK_LINES][MAX_COLS];
int sb_write_row = 0;
int sb_view      = 0;
int cursor_row   = 0;
int cursor_col   = 0;

static int sb_bottom_view(void) {
    int v = sb_write_row - g_text_rows + 1;
    return v < 0 ? 0 : v;
}

static void sb_clear_line(int row) {
    for (int c = 0; c < g_text_cols; c++) {
        scrollback[row][c].ch   = ' ';
        scrollback[row][c].attr = WHITE_ON_BLACK;
    }
}

static void sb_newline(void) {
    sb_write_row++;
    if (sb_write_row >= SCROLLBACK_LINES) {
        for (int r = 0; r < SCROLLBACK_LINES - 1; r++)
            for (int c = 0; c < MAX_COLS; c++)
                scrollback[r][c] = scrollback[r + 1][c];
        sb_write_row = SCROLLBACK_LINES - 1;
        if (sb_view > 0) sb_view--;
    }
    sb_clear_line(sb_write_row);
    cursor_col = 0;
    int bottom = sb_bottom_view();
    if (sb_view >= bottom - 1) sb_view = bottom;
}

static void sb_write_char(char c) {
    if (c == '\n') { sb_newline(); return; }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            scrollback[sb_write_row][cursor_col].ch   = ' ';
            scrollback[sb_write_row][cursor_col].attr = WHITE_ON_BLACK;
        }
        int bottom = sb_bottom_view();
        if (sb_view < bottom) sb_view = bottom;
        return;
    }
    if (cursor_col >= g_text_cols) sb_newline();
    scrollback[sb_write_row][cursor_col].ch   = c;
    scrollback[sb_write_row][cursor_col].attr = WHITE_ON_BLACK;
    cursor_col++;
    int bottom = sb_bottom_view();
    if (sb_view < bottom) sb_view = bottom;
}

// ============================================================
// RENDU VGA TEXTE
// ============================================================
static void render_vga(void) {
    volatile char* vram = (volatile char*)VIDEO_ADDRESS;
    int at_bottom = screen_at_bottom();
    int rows = g_text_rows;
    int cols = g_text_cols;

    for (int row = 0; row < rows; row++) {
        int src_row = sb_view + row;
        if (!at_bottom && row == rows - 1) break;
        for (int col = 0; col < cols; col++) {
            int off = 2 * (row * cols + col);
            if (src_row >= 0 && src_row < SCROLLBACK_LINES) {
                vram[off]     = scrollback[src_row][col].ch;
                vram[off + 1] = scrollback[src_row][col].attr;
            } else {
                vram[off] = ' '; vram[off+1] = WHITE_ON_BLACK;
            }
        }
    }
    if (!at_bottom) {
        const char* msg = "[ SCROLL - fleche bas pour revenir ]          ";
        int base = 2 * ((rows-1) * cols);
        for (int col = 0; col < cols; col++) {
            int off = base + 2*col;
            vram[off]   = msg[col] ? msg[col] : ' ';
            vram[off+1] = DARK_ON_WHITE;
        }
    }
    if (at_bottom) {
        int scr_row = sb_write_row - sb_view;
        if (scr_row < 0) scr_row = 0;
        if (scr_row >= rows) scr_row = rows - 1;
        unsigned short pos = (unsigned short)(scr_row * cols + cursor_col);
        outb(0x3D4, 14); outb(0x3D5, (pos >> 8) & 0xFF);
        outb(0x3D4, 15); outb(0x3D5,  pos        & 0xFF);
    } else {
        unsigned short pos = (unsigned short)(rows * cols);
        outb(0x3D4, 14); outb(0x3D5, (pos >> 8) & 0xFF);
        outb(0x3D4, 15); outb(0x3D5,  pos        & 0xFF);
    }
}

// ============================================================
// DIRTY FLAG — évite de redessiner tout l'écran à chaque caractère
// ============================================================
static int g_screen_dirty = 0;

// Flag indiquant qu'on est en train de composer un écran UI graphique.
// Quand il vaut 1, screen_render() ne flush pas immédiatement.
int g_ui_drawing = 0;

// Marquer l'écran comme devant être redessiner
void screen_invalidate(void) { g_screen_dirty = 1; }

// Début d'un bloc de dessin UI — suspend les flushes automatiques
void screen_begin_ui(void) { g_ui_drawing = 1; g_screen_dirty = 0; }

// Début d'un bloc de dessin UI intermédiaire — flush les changements graphiques
// sans effacer l'écran (pour les mises à jour partielles dans une UI active)
void screen_end_ui(void) {
    g_ui_drawing = 0;
    if (g_screen_dirty) {
        if (vesa_active()) render_vesa();
        else               render_vga();
        g_screen_dirty = 0;
    }
    // NOTE : le curseur souris est géré par la boucle appelante, pas ici.
}

// Sortie définitive d'une UI pour retourner au terminal
void screen_exit_ui(void) {
    g_ui_drawing = 0;
    g_screen_dirty = 0;
    // Invalider le curseur sauvegardé : le framebuffer va être écrasé par
    // vesa_fill(), les pixels sauvegardés seraient ceux de l'ancien contexte
    // (ex: login). Sans ça, le premier mouse_erase_cursor() dans le nouveau
    // contexte (bureau) restaurerait des pixels corrompus.
    extern void mouse_reset_cursor(void);
    mouse_reset_cursor();
    if (vesa_active()) {
        vesa_fill(COLOR_BG);
        vesa_invalidate_all();
        render_vesa();
    } else {
        render_vga();
    }
}

// Forcer un rendu immédiat (reset le flag)
void screen_flush(void) {
    if (vesa_active()) render_vesa();
    else               render_vga();
    g_screen_dirty = 0;
}

// ============================================================
// RENDU VESA — dirty cell, zéro vesa_fill(), zéro flash
// ============================================================

// Position du curseur dessiné lors du dernier render
// (pour l'effacer proprement avant de le redessiner ailleurs)
static int g_cursor_drawn_col = -1;
static int g_cursor_drawn_row = -1;

static void render_vesa(void) {
    int at_bottom = screen_at_bottom();
    int rows = g_text_rows;
    int cols = g_text_cols;

    // --- 1. Effacer l'ancien curseur en redessinant la cellule qu'il occupait ---
    if (g_cursor_drawn_col >= 0 && g_cursor_drawn_row >= 0) {
        int dc = g_cursor_drawn_col;
        int dr = g_cursor_drawn_row;
        // Forcer le redraw de cette cellule pour effacer la barre curseur
        int src = sb_view + dr;
        char ch_under = (src >= 0 && src < SCROLLBACK_LINES) ? scrollback[src][dc].ch : ' ';
        vesa_invalidate_cell(dc, dr); // marque dirty pour forcer le redraw
        vesa_draw_glyph(dc * FONT_W, dr * FONT_H, ch_under, COLOR_FG, COLOR_BG);
        g_cursor_drawn_col = -1;
        g_cursor_drawn_row = -1;
    }

    // --- 2. Redessiner toutes les cellules modifiées (dirty tracking dans vesa.c) ---
    for (int row = 0; row < rows; row++) {
        int src_row = sb_view + row;

        // Dernière ligne en mode scroll = bandeau de message
        if (!at_bottom && row == rows - 1) {
            const char* msg = "[ SCROLL - fleche bas pour revenir ]";
            for (int col = 0; col < cols; col++) {
                char ch = msg[col] ? msg[col] : ' ';
                vesa_draw_glyph(col * FONT_W, row * FONT_H, ch, COLOR_SCROLL_FG, COLOR_SCROLL_BG);
            }
            continue;
        }

        for (int col = 0; col < cols; col++) {
            char ch = ' ';
            if (src_row >= 0 && src_row < SCROLLBACK_LINES)
                ch = scrollback[src_row][col].ch;
            // vesa_draw_glyph est no-op si la cellule n'a pas changé (dirty check interne)
            vesa_draw_glyph(col * FONT_W, row * FONT_H, ch, COLOR_FG, COLOR_BG);
        }
    }

    // --- 3. Dessiner le nouveau curseur (deux pixels en bas du glyphe courant) ---
    if (at_bottom) {
        int scr_row = sb_write_row - sb_view;
        if (scr_row >= 0 && scr_row < rows) {
            int px = cursor_col * FONT_W;
            int py = scr_row * FONT_H + FONT_H - 2;
            for (int r = 0; r < 2; r++)
                for (int c = 0; c < FONT_W; c++)
                    vesa_put_pixel(px + c, py + r, COLOR_FG_BRIGHT);
            g_cursor_drawn_col = cursor_col;
            g_cursor_drawn_row = scr_row;
        }
    }
}

// ============================================================
// screen_render — ne redessine que si nécessaire (dirty flag)
// ============================================================
void screen_render(void) {
    // On accumule via le dirty flag ; screen_flush() effectue le rendu réel.
    // Pour la compatibilité avec les appelants existants qui veulent un rendu
    // immédiat après print_*, on flush directement ici uniquement quand on
    // est hors d'un bloc graphique UI (g_ui_drawing == 0).
    if (!g_ui_drawing) {
        if (vesa_active()) render_vesa();
        else               render_vga();
    } else {
        // Dans un bloc UI, on se contente de marquer dirty
        g_screen_dirty = 1;
    }
}

// ============================================================
// PRIMITIVES GÉOMÉTRIQUES (coordonnées pixels)
// ============================================================

void gfx_blend_pixel(int x, int y, uint32_t src, uint8_t alpha) {
    if (!vesa_active()) return;
    if (alpha == 0) return;
    uint32_t sw = vesa_width(), sh = vesa_height();
    if ((uint32_t)x >= sw || (uint32_t)y >= sh) return;
    if (alpha >= 255) {
        vesa_put_pixel(x, y, src);
        return;
    }
    uint32_t dst = vesa_get_pixel(x, y);
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t inv = 255u - (uint32_t)alpha;
    uint32_t r = (sr * alpha + dr * inv) / 255u;
    uint32_t g = (sg * alpha + dg * inv) / 255u;
    uint32_t b = (sb * alpha + db * inv) / 255u;
    vesa_put_pixel(x, y, (r << 16) | (g << 8) | b);
}

void gfx_fill_rect_blend(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!vesa_active() || alpha == 0) return;
    int x2 = x + w, y2 = y + h;
    uint32_t sw = vesa_width(), sh = vesa_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x2 > sw) x2 = (int)sw;
    if ((uint32_t)y2 > sh) y2 = (int)sh;
    for (int py = y; py < y2; py++)
        for (int px = x; px < x2; px++)
            gfx_blend_pixel(px, py, color, alpha);
    vesa_invalidate_rect(x, y, x2 - x, y2 - y);
}

void gfx_stroke_rect_blend(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!vesa_active() || w <= 0 || h <= 0 || alpha == 0) return;
    int x2 = x + w - 1, y2 = y + h - 1;
    for (int xi = x; xi <= x2; xi++) {
        gfx_blend_pixel(xi, y, color, alpha);
        if (h > 1) gfx_blend_pixel(xi, y2, color, alpha);
    }
    for (int yi = y + 1; yi < y2; yi++) {
        gfx_blend_pixel(x, yi, color, alpha);
        gfx_blend_pixel(x2, yi, color, alpha);
    }
    vesa_invalidate_rect(x, y, w, h);
}

// Remplir un rectangle plein
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa_active()) return;
    int x2 = x + w;
    int y2 = y + h;
    uint32_t sw = vesa_width();
    uint32_t sh = vesa_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x2 > sw) x2 = (int)sw;
    if ((uint32_t)y2 > sh) y2 = (int)sh;
    for (int py = y; py < y2; py++)
        for (int px = x; px < x2; px++)
            vesa_put_pixel(px, py, color);
    // Invalider les cellules glyphe recouvertes : sans ça, vesa_draw_glyph()
    // croirait les cellules inchangées (dirty check) et ne redessinerait pas
    // le texte qui sera dessiné par-dessus ce rectangle.
    vesa_invalidate_rect(x, y, x2 - x, y2 - y);
}

// Contour d'un rectangle
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa_active()) return;
    gfx_fill_rect(x,         y,         w, 1, color);  // haut
    gfx_fill_rect(x,         y + h - 1, w, 1, color);  // bas
    gfx_fill_rect(x,         y,         1, h, color);  // gauche
    gfx_fill_rect(x + w - 1, y,         1, h, color);  // droite
}

// Ligne (Bresenham)
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!vesa_active()) return;
    int ox0 = x0, oy0 = y0;
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        vesa_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    // Invalider la bounding box de la ligne
    int lx = ox0 < x1 ? ox0 : x1;
    int ly = oy0 < y1 ? oy0 : y1;
    int lw = (ox0 > x1 ? ox0 : x1) - lx + 1;
    int lh = (oy0 > y1 ? oy0 : y1) - ly + 1;
    vesa_invalidate_rect(lx, ly, lw, lh);
}

// Cercle (Midpoint)
void gfx_draw_circle(int cx, int cy, int r, uint32_t color) {
    if (!vesa_active()) return;
    int x = r, y = 0, err = 0;
    while (x >= y) {
        vesa_put_pixel(cx + x, cy + y, color);
        vesa_put_pixel(cx + y, cy + x, color);
        vesa_put_pixel(cx - y, cy + x, color);
        vesa_put_pixel(cx - x, cy + y, color);
        vesa_put_pixel(cx - x, cy - y, color);
        vesa_put_pixel(cx - y, cy - x, color);
        vesa_put_pixel(cx + y, cy - x, color);
        vesa_put_pixel(cx + x, cy - y, color);
        y++;
        if (err <= 0) { err += 2*y + 1; }
        else          { x--; err += 2*(y - x) + 1; }
    }
    vesa_invalidate_rect(cx - r, cy - r, 2*r + 1, 2*r + 1);
}

// Cercle plein
void gfx_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (!vesa_active()) return;
    for (int y = -r; y <= r; y++) {
        int half = 0;
        int y2 = y * y, r2 = r * r;
        // calcul racine carrée entière de r²-y²
        while ((half+1)*(half+1) + y2 <= r2) half++;
        gfx_fill_rect(cx - half, cy + y, 2*half + 1, 1, color);
    }
}

// Dégradé horizontal gauche→droite
void gfx_gradient_h(int x, int y, int w, int h,
                    uint32_t col_left, uint32_t col_right) {
    if (!vesa_active()) return;
    uint8_t r0 = (col_left  >> 16) & 0xFF;
    uint8_t g0 = (col_left  >>  8) & 0xFF;
    uint8_t b0 = (col_left       ) & 0xFF;
    uint8_t r1 = (col_right >> 16) & 0xFF;
    uint8_t g1 = (col_right >>  8) & 0xFF;
    uint8_t b1 = (col_right      ) & 0xFF;
    for (int col = 0; col < w; col++) {
        uint8_t r = (uint8_t)((int)r0 + ((int)(r1 - r0) * col) / (w - 1));
        uint8_t g = (uint8_t)((int)g0 + ((int)(g1 - g0) * col) / (w - 1));
        uint8_t b = (uint8_t)((int)b0 + ((int)(b1 - b0) * col) / (w - 1));
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        gfx_fill_rect(x + col, y, 1, h, c);
    }
}

// Dégradé vertical haut→bas
void gfx_gradient_v(int x, int y, int w, int h,
                    uint32_t col_top, uint32_t col_bot) {
    if (!vesa_active()) return;
    uint8_t r0 = (col_top >> 16) & 0xFF;
    uint8_t g0 = (col_top >>  8) & 0xFF;
    uint8_t b0 = (col_top      ) & 0xFF;
    uint8_t r1 = (col_bot >> 16) & 0xFF;
    uint8_t g1 = (col_bot >>  8) & 0xFF;
    uint8_t b1 = (col_bot      ) & 0xFF;
    for (int row = 0; row < h; row++) {
        uint8_t r = (uint8_t)((int)r0 + ((int)(r1 - r0) * row) / (h - 1));
        uint8_t g = (uint8_t)((int)g0 + ((int)(g1 - g0) * row) / (h - 1));
        uint8_t b = (uint8_t)((int)b0 + ((int)(b1 - b0) * row) / (h - 1));
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        gfx_fill_rect(x, y + row, w, 1, c);
    }
}

// Texte centré dans un rectangle (coordonnées pixels)
void gfx_draw_text_centered(int x, int y, int w,
                             const char* str, uint32_t fg, uint32_t bg) {
    if (!vesa_active()) return;
    int len = 0;
    while (str[len]) len++;
    int text_w = len * FONT_W;
    int tx = x + (w - text_w) / 2;
    for (int i = 0; i < len; i++)
        vesa_draw_glyph(tx + i * FONT_W, y, str[i], fg, bg);
}

// Texte libre en pixels
void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    if (!vesa_active()) return;
    for (int i = 0; str[i]; i++)
        vesa_draw_glyph(x + i * FONT_W, y, str[i], fg, bg);
}

// ============================================================
// API scrollback & print (inchangée)
// ============================================================
int screen_at_bottom(void) { return sb_view >= sb_bottom_view(); }

void screen_goto_bottom(void) {
    sb_view = sb_bottom_view();
    screen_render();
}

void screen_scroll_up(void) {
    if (sb_view > 0) { sb_view--; screen_render(); }
}

void screen_scroll_down(void) {
    int b = sb_bottom_view();
    if (sb_view < b) { sb_view++; screen_render(); }
}

void screen_clear_visible(void) {
    for (int r = 0; r < g_text_rows; r++) {
        int t = sb_view + r;
        if (t < SCROLLBACK_LINES) sb_clear_line(t);
    }
    sb_write_row = sb_view;
    cursor_col   = 0;
    if (vesa_active()) {
        vesa_fill(COLOR_BG);
        vesa_invalidate_all();
    }
    screen_render();
}

// ============================================================
// HOOK DE SORTIE — permet au desktop de rediriger print_* vers
// son buffer terminal interne. NULL = comportement normal scrollback.
// ============================================================
typedef void (*CharOutputFn)(char);
CharOutputFn g_print_hook = 0;

void print_char(char c) {
    if (g_print_hook) { g_print_hook(c); return; }
    sb_write_char(c);
    screen_render();
}

void print_string(const char* str) {
    if (g_print_hook) { while (*str) g_print_hook(*str++); return; }
    while (*str) sb_write_char(*str++);
    screen_render();
}

void clear_screen(void) {
    sb_write_row = 0;
    sb_view      = 0;
    cursor_col   = 0;
    cursor_row   = 0;
    for (int r = 0; r < SCROLLBACK_LINES; r++) sb_clear_line(r);
    if (vesa_active()) {
        // Effacer physiquement le framebuffer ET invalider le dirty tracker
        // pour forcer le redraw complet — sinon les anciennes cellules
        // semblent "inchangées" (espace/noir = espace/noir) et ne sont
        // pas redessinées, laissant des artifacts visuels.
        vesa_fill(COLOR_BG);
        vesa_invalidate_all();
    }
    screen_render();
}

void update_cursor(void) { screen_render(); }

void set_cursor(int row, int col) {
    cursor_row = row;
    cursor_col = col;
    screen_render();
}

void print_int(int num) {
    if (num == 0) { print_char('0'); return; }
    if (num < 0)  { print_char('-'); num = -num; }
    char buf[12]; int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (--i >= 0) print_char(buf[i]);
}

void print_hex(uint32_t num) {
    print_char('0'); print_char('x');
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t n = (num >> i) & 0xF;
        print_char(n < 10 ? '0' + n : 'A' + n - 10);
    }
}

void print_dec(uint32_t num) {
    if (num == 0) { print_char('0'); return; }
    char buf[11]; int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (i--) print_char(buf[i]);
}

void draw_box(int x1, int y1, int x2, int y2) {
    if (vesa_active()) {
        int px1 = x1 * FONT_W, py1 = y1 * FONT_H;
        int px2 = x2 * FONT_W, py2 = y2 * FONT_H;
        int w = px2 - px1 + FONT_W;
        int h = py2 - py1 + FONT_H;
        gfx_draw_rect(px1, py1, w, h, COLOR_FG);
    } else {
        volatile char* v = (volatile char*)VIDEO_ADDRESS;
        #define VS(r,c,ch) { int o=2*((r)*g_text_cols+(c)); v[o]=(ch); v[o+1]=WHITE_ON_BLACK; }
        VS(y1,x1,'+') VS(y1,x2,'+') VS(y2,x1,'+') VS(y2,x2,'+')
        for (int x=x1+1;x<x2;x++){VS(y1,x,'-') VS(y2,x,'-')}
        for (int y=y1+1;y<y2;y++){VS(y,x1,'|') VS(y,x2,'|')}
        #undef VS
    }
}

void clear_area(int x1, int y1, int x2, int y2) {
    if (vesa_active()) {
        int px = x1 * FONT_W, py = y1 * FONT_H;
        int w  = (x2 - x1 + 1) * FONT_W;
        int h  = (y2 - y1 + 1) * FONT_H;
        gfx_fill_rect(px, py, w, h, COLOR_BG);
    } else {
        volatile char* v = (volatile char*)VIDEO_ADDRESS;
        for (int y=y1;y<=y2;y++) for (int x=x1;x<=x2;x++) {
            int o=2*(y*g_text_cols+x); v[o]=' '; v[o+1]=WHITE_ON_BLACK;
        }
    }
}