#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

/* Constantes d'écran */
#define VIDEO_ADDRESS  0xB8000
#define MAX_COLS       80
#define MAX_ROWS       25
#define WHITE_ON_BLACK 0x0F
#define SCREEN_WIDTH   80
#define SCREEN_HEIGHT  25

/* Variables globales */
extern int cursor_row;
extern int cursor_col;

/* Fonctions */
void print_char(char c);
void print_string(const char* str);
void print_int(int num);
void print_hex(uint32_t num);
void print_dec(uint32_t num);
void clear_screen(void);
void update_cursor(void);
void set_cursor(int row, int col);
void scroll_if_needed(void);
void draw_box(int x1, int y1, int x2, int y2);
void clear_area(int x1, int y1, int x2, int y2);
void print_xy(int x, int y, const char *text);
void screen_fill_rect(int x, int y, int w, int h, char c);
void clear_line(int y);
int screen_get_width(void);

#endif /* SCREEN_H */