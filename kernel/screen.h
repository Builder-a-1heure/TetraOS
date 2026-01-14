#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

#define VIDEO_ADDRESS 0xB8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0F

// Position du curseur
extern int cursor_row;
extern int cursor_col;

void draw_box(int x1, int y1, int x2, int y2);
void clear_area(int x1, int y1, int x2, int y2);
void clear_screen();
void print_char(char c);
void print_string(const char* str);
void set_cursor(int row, int col);
void update_cursor();
void print_hex(uint32_t num);
void print_dec(uint32_t num);
void print_int(int num);
#endif