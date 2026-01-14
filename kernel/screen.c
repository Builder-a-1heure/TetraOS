#include "screen.h"

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

int cursor_row = 0;
int cursor_col = 0;

int get_offset(int row, int col) {
    return 2 * (row * MAX_COLS + col);
}


void scroll_if_needed() {
    if (cursor_row >= MAX_ROWS) {
        volatile char* video = (char*)VIDEO_ADDRESS;
        // Move lines up by one
        for (int y = 1; y < MAX_ROWS; y++) {
            for (int x = 0; x < MAX_COLS; x++) {
                int dst = 2 * ((y-1) * MAX_COLS + x);
                int src = 2 * (y * MAX_COLS + x);
                video[dst] = video[src];
                video[dst+1] = video[src+1];
            }
        }
        // Clear last line
        for (int x = 0; x < MAX_COLS; x++) {
            int off = 2 * ((MAX_ROWS-1) * MAX_COLS + x);
            video[off] = ' ';
            video[off+1] = WHITE_ON_BLACK;
        }
        cursor_row = MAX_ROWS - 1;
    }
}

void print_char(char c) {
    volatile char* video = (char*) VIDEO_ADDRESS;

    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        scroll_if_needed();
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = MAX_COLS - 1;
        }
        int offset = get_offset(cursor_row, cursor_col);
        video[offset] = ' ';
    } else {
        int offset = get_offset(cursor_row, cursor_col);
        video[offset] = c;
        video[offset + 1] = WHITE_ON_BLACK;
        cursor_col++;
    }

    if (cursor_col >= MAX_COLS) {
        cursor_col = 0;
        cursor_row++;
    }

    if (cursor_row >= MAX_ROWS) {
        clear_screen();
        cursor_row = 0;
        cursor_col = 0;
    }

    update_cursor();
}

void print_int(int num) {
    char buffer[12];
    int i = 0;
    
    if (num == 0) {
        print_char('0');
        return;
    }
    
    if (num < 0) {
        print_char('-');
        num = -num;
    }
    
    while (num > 0) {
        buffer[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    while (--i >= 0) {
        print_char(buffer[i]);
    }
}

void print_string(const char* str) {
    while (*str) {
        print_char(*str++);
    }
}

void clear_screen() {
    volatile char* video = (char*) VIDEO_ADDRESS;
    for (int i = 0; i < MAX_ROWS * MAX_COLS * 2; i += 2) {
        video[i] = ' ';
        video[i + 1] = WHITE_ON_BLACK;
    }
    cursor_row = 0;
    cursor_col = 0;
    update_cursor();
}

void update_cursor() {
    unsigned short pos = cursor_row * MAX_COLS + cursor_col;
    outb(0x3D4, 14);
    outb(0x3D5, (pos >> 8) & 0xFF);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

void set_cursor(int row, int col) {
    cursor_row = row;
    cursor_col = col;
    update_cursor();
}

void print_hex(uint32_t num) {
    print_string("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t nibble = (num >> i) & 0xF;
        print_char(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
}

void print_dec(uint32_t num) {
    if (num == 0) {
        print_char('0');
        return;
    }

    char buf[11];
    int i = 0;
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    while (i--) {
        print_char(buf[i]);
    }
}

void draw_box(int x1, int y1, int x2, int y2) {
    volatile char* video = (char*)VIDEO_ADDRESS;
    
    // Dessiner les coins
    video[get_offset(y1, x1)] = '+';
    video[get_offset(y1, x2)] = '+';
    video[get_offset(y2, x1)] = '+';
    video[get_offset(y2, x2)] = '+';
    
    // Dessiner les bords horizontaux
    for (int x = x1+1; x < x2; x++) {
        video[get_offset(y1, x)] = '-';
        video[get_offset(y2, x)] = '-';
    }
    
    // Dessiner les bords verticaux
    for (int y = y1+1; y < y2; y++) {
        video[get_offset(y, x1)] = '|';
        video[get_offset(y, x2)] = '|';
    }
}

void clear_area(int x1, int y1, int x2, int y2) {
    volatile char* video = (char*)VIDEO_ADDRESS;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            video[get_offset(y, x)] = ' ';
            video[get_offset(y, x)+1] = WHITE_ON_BLACK;
        }
    }
}