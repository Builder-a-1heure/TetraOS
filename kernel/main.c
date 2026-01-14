#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"


__attribute__((naked)) __attribute__((section(".text.start")))
void start(void) {
    // Tout en 1 block pour éviter les problème de linker ud2
    asm volatile (
        "mov $0x90000, %esp\n"   // Stack
        "mov %esp, %ebp\n"       // Frame pointer
        
        // Réinit segments (CRITIQUE!)
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n" 
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        "mov %ax, %ss\n"
        
        "call kmain\n"           // Appel kernel
        "hlt\n"                  // Si retour
        "jmp .\n"                // Boucle sécurité
    );
}

// Message dans la section .rodata
const char boot_msg[] __attribute__((section(".rodata"))) = "Booting...\n";

// --- Déclaration des fonctions ---
void windowed_write(const char* filename);
void tetra_shell(void);


void kmain(void) {
    print_string("ETAPE 1: Debut kmain()\n");
    
    print_string("ETAPE 2: Initialisation ecran\n");
    clear_screen();
    
    print_string("ETAPE 2: Initialisation ata\n");
    ata_init();

    print_string("ETAPE 4: Initialisation fichiersystem\n");
    fs_init();
    
    print_string("ETAPE 5: Lancement shell\n");
    tetra_shell();  // ← Si crash ici, c'est le shell
    
    print_string("ETAPE 6: Retour shell (anormal)\n");
    while(1) { asm volatile ("nop"); }
}

// --- Fonctions existantes ---
static void delay_spin(uint32_t loops) 
{ 
    volatile uint32_t x = 0; 
    for (uint32_t i = 0; i < loops; i++) { 
        x += i; 
    } 
    (void)x; 
}

static void draw_train_at(int x) {
    const char* hyperloop[] = {
        "      ====        ________                ___________ ",
        "  _D _|  |_______/        \\\\__I_I_____===__|_________| ",
        "   |(_)---  |   H\\\\________/ |   |        =|___ ___|   ",
        "   /     |  |   H  |  |     |   |         ||_| |_||   ",
        "  |      |  |   H  |__--------------------| [___] |   ",
        "  | ________|___H__/__|_____/[][]~\\\\_______|       |   ",
        "  |/ |   |-----------I_____I [][] []  D   |=======|__ ",
        "__/ =| o |=-~~\\\\  /~~\\\\  /~~\\\\  /~~\\\\ ____Y___________|__ ",
        " |/-=|___|=    ||    ||    ||    |_____/~\\\\___/        ",
        "  \\\\_/      \\\\O=====O=====O=====O_/      \\\\_/            ",
    };  
    
    int h = sizeof(hyperloop) / sizeof(hyperloop[0]);
    for (int pos = 0; pos < 40; pos++) {
        clear_screen();
        for (int i = 0; i < h; i++) {
            for (int sp = 0; sp < pos; sp++) print_char(' ');
            print_string(hyperloop[i]); 
            print_char('\n');
        }
        delay_spin(6000000);
    }

    for (int i = 0; i < h; i++) {
        int col = x;
        const char* s = hyperloop[i];
        while (*s) {
            if (col >= 0 && col < MAX_COLS) print_char(*s);
            col++;
            s++;
        }
        print_char('\n');
    }
}


static void cmd_sl(void) {
    for (int pos = -70; pos < MAX_COLS; pos++) {
        clear_screen();
        draw_train_at(pos);
        delay_spin(4000000);
    }
}




// --- Éditeur Fenêtré ---
void draw_editor_window(const char* filename, const char* content, int cursor_pos) 
{
    int width = 60;
    int height = 10;
    int start_x = (80 - width) / 2;
    int start_y = (25 - height) / 2;
    
    // Dessiner la fenêtre
    for (int y = start_y; y <= start_y + height; y++) {
        for (int x = start_x; x <= start_x + width; x++) {
            if (y == start_y || y == start_y + height || x == start_x || x == start_x + width) {
                set_cursor(y, x);
                print_char('*');
            } else {
                set_cursor(y, x);
                print_char(' ');
            }
        }
    }
    
    // Titre
    set_cursor(start_y, start_x + 2);
    print_string("Editing: ");
    print_string(filename);
    
    // Contenu
    int content_y = start_y + 2;
    int max_chars = width - 4;
    
    // Afficher le contenu
    for (int i = 0; i < height - 4; i++) {
        set_cursor(content_y + i, start_x + 2);
        for (int j = 0; j < max_chars; j++) {
            int pos = i * max_chars + j;
            if (pos < (int)strlen(content)) {
                print_char(content[pos]);
            } else {
                print_char(' ');
            }
        }
    }
    
    // Barre de statut
    set_cursor(start_y + height - 2, start_x + 2);
    print_string("ESC:Save  /*/*ctrl+c removed*/ removed*/:Cancel");
    
    // Curseur
    int cursor_x = start_x + 2 + (cursor_pos % max_chars);
    int cursor_y = content_y + (cursor_pos / max_chars);
    set_cursor(cursor_y, cursor_x);
}

void windowed_write(const char* filename) 
{
    char content[1024] = {0};
    int cursor_pos = 0;
    int width = 60;
    int height = 10;
    int max_chars = width - 4;
    int max_content = (height - 4) * max_chars;
    
    // Essayer de lire le fichier existant
    uint8_t existing_data[1024];
    int bytes_read = fs_read_file(filename, existing_data, sizeof(existing_data) - 1);
    if (bytes_read > 0) {
        existing_data[bytes_read] = '\0';
        strncpy(content, (char*)existing_data, sizeof(content) - 1);
        cursor_pos = strlen(content);
    }
    
    while (1) {
        draw_editor_window(filename, content, cursor_pos);
        
        char c = keyboard_get_char();
        
        if (c == 27) { // ESC - Sauvegarder et quitter
            fs_write_file(filename, (uint8_t*)content, strlen(content));
            break;
        }
        else if (c == 3) { // /*/*ctrl+c removed*/ removed*/ - Annuler
            break;
        }
        else if (c == '\b' && cursor_pos > 0) {
            // Backspace
            for (int i = cursor_pos - 1; i < (int)strlen(content); i++) {
                content[i] = content[i + 1];
            }
            cursor_pos--;
        }
        else if (c == '\r' || c == '\n') {
            // Nouvelle ligne
            if (cursor_pos < max_content - 1) {
                content[cursor_pos++] = '\n';
            }
        }
        else if (c >= 32 && c < 127 && cursor_pos < max_content - 1) {
            // Caractère normal
            for (int i = strlen(content) + 1; i > cursor_pos; i--) {
                content[i] = content[i - 1];
            }
            content[cursor_pos++] = c;
        }
        
        content[sizeof(content) - 1] = '\0'; // Sécurité
    }
    
    // Effacer la fenêtre
    clear_screen();
}

// --- Shell Style Linux ---
void tetra_shell(void) 
{
    char input[256];
    
    print_string("\n\nTetraOS Shell v1.0\n");
    print_string("Type 'help' for available commands\n\n");
    
    while (1) {
        // Prompt style Linux
        print_string("root@TetraOS:");
        
        // Afficher le chemin actuel
        extern FSTable g_fs;
        extern uint32_t g_cwd;
        char parts[16][32];
        int depth = 0;
        uint32_t cur = g_cwd;
        
        while (cur != 0 && depth < 16) {
            memset(parts[depth], 0, 32);
            strncpy(parts[depth], g_fs.nodes[cur].name, 31);
            cur = g_fs.nodes[cur].parent;
            depth++;
        }
        
        print_char('/');
        for (int i = depth - 1; i >= 0; i--) {
            print_string(parts[i]);
            if (i > 0) print_char('/');
        }
        
        print_string(" # ");
        
        // Lire l'entrée
        int i = 0;
        while (1) {
            char c = keyboard_get_char();
            
            if (c == '\r' || c == '\n') {
                input[i] = '\0';
                print_char('\n');
                break;
            }
            else if ((c == '\b' || c == 127) && i > 0) {
                i--;
                print_string("\b \b");
            }
            else if (c >= 32 && c <= 126 && i < 255) {
                input[i++] = c;
                print_char(c);
            }
            else if (c == 27) { // ESC
                input[0] = '\0';
                print_string("^C\n");
                break;
            }
        }
        
        // Traiter la commande
        if (strlen(input) == 0) continue;
        
        const char* s = input;
        while (*s == ' ') s++;
        
        if (strcmp(s, "help") == 0) {
            print_string("Available commands:\n");
            print_string("  ls              - List files and directories\n");
            print_string("  cd <dir>        - Change current directory\n");
            print_string("  pwd             - Show current working directory\n");
            print_string("  mkdir <dir>     - Create a new directory\n");
            print_string("  new <file>      - Create a new empty file\n");
            print_string("  open <file>     - Open an existing file\n");
            print_string("  cat <file>      - Display file contents\n");
            print_string("  clear           - Clear the screen\n");
            print_string("  sl              - Fun command (train animation)\n");
            print_string("  exit            - Exit the shell\n");

        }
        else if (strcmp(s, "exit") == 0) {
            outw(0x604, 0x2000);
        }
        else if (strcmp(s, "clear") == 0) {
            clear_screen();
        }
        else if (strcmp(s, "ls") == 0) {
            fs_ls();
        }
        else if (strcmp(s, "fs") == 0) {
            fs_tree();
        }
        else if (strcmp(s, "pwd") == 0) {
            fs_pwd();
        }
        else if (strncmp(s, "cd ", 3) == 0) {
            char *path = (char*)(s + 3);
            while (*path == ' ') path++;
            if (fs_cd(path) != 0) {
                print_string("cd: directory not found\n");
            }
        }
        else if (strncmp(s, "mkdir ", 6) == 0) {
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            if (fs_mkdir(name) != 0) {
                print_string("mkdir: failed\n");
            }
        }
        else if (strncmp(s, "new ", 4) == 0) {
            char *name = (char*)(s + 4);
            while (*name == ' ') name++;
            if (fs_add(name) != 0) {
                print_string("add: failed\n");
            }
        }
        else if (strncmp(s, "open ", 5) == 0) {
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            
            int idx = fs_find(name);
            if (idx < 0) {
                print_string("File not found. Use 'add' to create it first.\n");
            } else {
                windowed_write(name);
            }
        }
        else if (strncmp(s, "cat ", 4) == 0) {
            char *name = (char*)(s + 4);
            while (*name == ' ') name++;
            
            uint8_t buffer[1024];
            int result = fs_read_file(name, buffer, sizeof(buffer) - 1);
            if (result > 0) {
                buffer[result] = '\0';
                print_string((char*)buffer);
                print_char('\n');
            } else {
                print_string("cat: file not found or error\n");
            }
        }
        else if (strcmp(s, "sl") == 0) {
            cmd_sl();
        }
        else if (strcmp(s, "exit") == 0) {
            print_string("Logging out...\n");
            break;
        }
        else {
            print_string("Command not found: ");
            print_string(s);
            print_string("\nType 'help' for available commands\n");
        }
    }
}
