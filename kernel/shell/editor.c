// editor.c — Éditeur de texte fenêtré TetraOS
//
// Éditeur simple en mode texte (VGA/VESA).
// Navigation : flèches directionnelles
// Sauvegarder : ESC   |   Annuler : Ctrl+C

#include "editor.h"
#include "../gfx/screen.h"
#include "../drivers/input.h"
#include "../fs/fs.h"
#include "../lib/utils.h"
#include <stdint.h>

static int count_lines(const char* content) {
    int lines = 1;
    for (int i = 0; content[i] != '\0'; i++) {
        if (content[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

static const char* get_line_start(const char* content, int line_number) {
    if (line_number == 0) return content;
    
    int current_line = 0;
    for (int i = 0; content[i] != '\0'; i++) {
        if (content[i] == '\n') {
            current_line++;
            if (current_line == line_number) {
                return &content[i + 1];
            }
        }
    }
    return NULL;
}

static int get_line_length(const char* line_start) {
    int len = 0;
    while (line_start[len] != '\0' && line_start[len] != '\n') {
        len++;
    }
    return len;
}

static void pos_to_line_col(const char* content, int pos, int* out_line, int* out_col) {
    int line = 0;
    int col = 0;
    
    for (int i = 0; i < pos && content[i] != '\0'; i++) {
        if (content[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    
    *out_line = line;
    *out_col = col;
}

void draw_editor_window(const char* filename, const char* content, int cursor_pos, int scroll_offset) {
    int width = 76;
    int height = 21;
    int start_x = 2;
    int start_y = 1;
    int content_height = height - 4;
    int content_width = width - 4;
    
    for (int y = start_y; y <= start_y + height; y++) {
        for (int x = start_x; x <= start_x + width; x++) {
            if (y == start_y || y == start_y + height ||
                x == start_x || x == start_x + width) {
                set_cursor(y, x);
                print_char('*');
            } else {
                set_cursor(y, x);
                print_char(' ');
            }
        }
    }

    set_cursor(start_y, start_x + 2);
    print_string("edition : ");
    print_string(filename);

    int content_y = start_y + 2;
    int total_lines = count_lines(content);
    
    for (int screen_line = 0; screen_line < content_height; screen_line++) {
        int actual_line = scroll_offset + screen_line;
        
        set_cursor(content_y + screen_line, start_x + 2);
        
        if (actual_line < total_lines) {
            const char* line_start = get_line_start(content, actual_line);
            if (line_start) {
                int line_len = get_line_length(line_start);
                
                for (int i = 0; i < content_width; i++) {
                    if (i < line_len) {
                        print_char(line_start[i]);
                    } else {
                        print_char(' ');
                    }
                }
            } else {
                for (int i = 0; i < content_width; i++) {
                    print_char(' ');
                }
            }
        } else {
            for (int i = 0; i < content_width; i++) {
                print_char(' ');
            }
        }
    }

    set_cursor(start_y + height - 2, start_x + 2);
    print_string("Fleches:Deplacer ECHAP:Sauver Ctrl+C:Annuler");

    int cursor_line, cursor_col;
    pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
    
    int screen_line = cursor_line - scroll_offset;
    
    if (screen_line >= 0 && screen_line < content_height) {
        if (cursor_col >= content_width) {
            cursor_col = content_width - 1;
        }
        set_cursor(content_y + screen_line, start_x + 2 + cursor_col);
    }
}

void editor_open(const char* filename) {
    char content[1024] = {0};
    int cursor_pos = 0;
    int width = 76;
    int height = 21;
    int content_width = width - 4;
    int content_height = height - 4;
    int scroll_offset = 0;

    uint8_t existing_data[1024];
    int bytes_read = fs_read_file(filename, existing_data, sizeof(existing_data) - 1);
    if (bytes_read > 0) {
        existing_data[bytes_read] = '\0';
        strncpy(content, (char*)existing_data, sizeof(content) - 1);
        cursor_pos = strlen(content);
    }
    
    clear_screen();
    
    while (1) {
        int cursor_line, cursor_col;
        pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
        
        if (cursor_line < scroll_offset) {
            scroll_offset = cursor_line;
        }
        if (cursor_line >= scroll_offset + content_height) {
            scroll_offset = cursor_line - content_height + 1;
        }
        
        draw_editor_window(filename, content, cursor_pos, scroll_offset);

        char c = keyboard_get_char();

        if (c == 27) {
            fs_write_file(filename, (uint8_t*)content, strlen(content));
            clear_screen();
            print_string("Fichier sauvegarde : ");
            print_string(filename);
            print_string("\n");
            break;
        }
        else if (c == 3) {
            clear_screen();
            print_string("edition annulee\n");
            break;
        }
        else if (c == 1) {
            int cursor_line, cursor_col;
            pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
            
            if (cursor_line > 0) {
                const char* prev_line = get_line_start(content, cursor_line - 1);
                if (prev_line) {
                    int prev_line_len = get_line_length(prev_line);
                    int target_col = cursor_col < prev_line_len ? cursor_col : prev_line_len;
                    cursor_pos = (prev_line - content) + target_col;
                }
            }
        }
        else if (c == 2) {
            int cursor_line, cursor_col;
            pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
            
            int total_lines = count_lines(content);
            if (cursor_line < total_lines - 1) {
                const char* next_line = get_line_start(content, cursor_line + 1);
                if (next_line) {
                    int next_line_len = get_line_length(next_line);
                    int target_col = cursor_col < next_line_len ? cursor_col : next_line_len;
                    cursor_pos = (next_line - content) + target_col;
                }
            }
        }
        else if (c == 17) {
            if (cursor_pos > 0) {
                cursor_pos--;
            }
        }
        else if (c == 18) {
            if (cursor_pos < (int)strlen(content)) {
                cursor_pos++;
            }
        }
        else if ((c == '\b' || c == 127) && cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < (int)strlen(content); i++) {
                content[i] = content[i + 1];
            }
            cursor_pos--;
        }
        else if (c == '\r' || c == '\n') {
            if (cursor_pos < (int)sizeof(content) - 2) {
                int len = strlen(content);
                for (int i = len; i >= cursor_pos; i--) {
                    content[i + 1] = content[i];
                }
                content[cursor_pos] = '\n';
                cursor_pos++;
            }
        }
        else if (c >= 32 && c < 127) {
            if (cursor_pos < (int)sizeof(content) - 2) {
                int len = strlen(content);
                for (int i = len; i >= cursor_pos; i--) {
                    content[i + 1] = content[i];
                }
                content[cursor_pos] = c;
                cursor_pos++;
            }
        }

        content[sizeof(content) - 1] = '\0';
    }
}

// ============================================================================
// SHELL NAMESPACED - SYSTÈME DE DISPATCH module.commande
// ============================================================================

// Utilitaire : parse "module.cmd arg1 arg2..." -> module, cmd, args
