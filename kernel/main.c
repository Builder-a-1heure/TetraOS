#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"
#include "tex.h"

__attribute__((naked)) __attribute__((section(".text.start")))
void start(void) {
    asm volatile (
        "mov $0x90000, %esp\n"
        "mov %esp, %ebp\n"

        "mov $0x10, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n"
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        "mov %ax, %ss\n"

        "call kmain\n"
        "hlt\n"
        "jmp .\n"
    );
}

const char boot_msg[] __attribute__((section(".rodata"))) = "Demarrage...\n";

void windowed_write(const char* filename);
void tetra_shell(void);

void kmain(void) {
    print_string("ETAPE 1 : Debut kmain()\n");

    print_string("ETAPE 2 : Initialisation ecran\n");
    clear_screen();

    print_string("ETAPE 3 : Initialisation ATA\n");
    ata_init();

    print_string("ETAPE 4 : Initialisation systeme de fichiers\n");
    fs_init();

    print_string("ETAPE 5 : Lancement du shell\n");
    tetra_shell();

    print_string("ETAPE 6 : Retour du shell (anormal)\n");
    while(1) { asm volatile ("nop"); }
}

static void delay_spin(uint32_t loops) {
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < loops; i++) {
        x += i;
    }
    (void)x;
}

static void draw_train_at(void) {
    /*const char* steam_locomotive[] = {
        "  _____            _                       ___     ___ ",
        " |_   _|   ___    | |_      _ _   __ _    / _ \\   / __|",
        "   | |    / -_)   |  _|    | '_| / _` |  | (_) |  \\__ \\",
        "  _|_|_   \\___|   _\\__|   _|_|_  \\__,_|   \\___/   |___/",
        "_|\"\"\"\"\"|_|\"\"\"\"\"|_|\"\"\"\"\"|_|\"\"\"\"\"|_|\"\"\"\"\"|_|\"\"\"\"\"|_|\"\"\"\"\"|",
        "`-0-0-'`-0-0-'`-0-0-'`-0-0-'`-0-0-'`-0-0-'`-0-0-'"
    };

    int pos = 0;
    int h = sizeof(steam_locomotive) / sizeof(steam_locomotive[0]);

    while (1) {
        clear_screen();

        // Verifie si une touche est pressee
        if (keyboard_get_char() != 0) {
            break;
        }

        // Affiche le train avec decalage
        for (int i = 0; i < h; i++) {
            for (int sp = 0; sp < pos; sp++) {
                print_char(' ');
            }
            print_string(steam_locomotive[i]);
            print_char('\n');
        }

        pos++;
        if (pos > 80) {
            pos = -70;
        }

        delay_spin(400000);
    }*/
    print_string("Rayu : CPT pour le moment, deso ...");
}


static void cmd_sl(void) {
    for (int pos = -70; pos < MAX_COLS; pos++) {
        clear_screen();
        draw_train_at();
        delay_spin(4000000);
    }
}

// Fonction pour compter le nombre de lignes dans le contenu
static int count_lines(const char* content) {
    int lines = 1;
    for (int i = 0; content[i] != '\0'; i++) {
        if (content[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

// Fonction pour obtenir le début d'une ligne spécifique
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

// Fonction pour obtenir la longueur d'une ligne (sans le \n)
static int get_line_length(const char* line_start) {
    int len = 0;
    while (line_start[len] != '\0' && line_start[len] != '\n') {
        len++;
    }
    return len;
}

// Convertit une position absolue en (ligne, colonne)
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
    int width = 76;   // 80 - 4 pour les marges
    int height = 21;  // 25 - 4 pour titre et aide
    int start_x = 2;
    int start_y = 1;
    int content_height = height - 4;
    int content_width = width - 4;
    
    // Dessiner la bordure
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

    // Titre
    set_cursor(start_y, start_x + 2);
    print_string("edition : ");
    print_string(filename);

    int content_y = start_y + 2;
    
    // Afficher le contenu ligne par ligne avec défilement
    int total_lines = count_lines(content);
    
    for (int screen_line = 0; screen_line < content_height; screen_line++) {
        int actual_line = scroll_offset + screen_line;
        
        set_cursor(content_y + screen_line, start_x + 2);
        
        if (actual_line < total_lines) {
            const char* line_start = get_line_start(content, actual_line);
            if (line_start) {
                int line_len = get_line_length(line_start);
                
                // Afficher jusqu'à content_width caractères
                for (int i = 0; i < content_width; i++) {
                    if (i < line_len) {
                        print_char(line_start[i]);
                    } else {
                        print_char(' ');
                    }
                }
            } else {
                // Ligne vide
                for (int i = 0; i < content_width; i++) {
                    print_char(' ');
                }
            }
        } else {
            // Pas de contenu sur cette ligne
            for (int i = 0; i < content_width; i++) {
                print_char(' ');
            }
        }
    }

    // Barre d'aide en bas
    set_cursor(start_y + height - 2, start_x + 2);
    print_string("Fleches:Deplacer ECHAP:Sauver Ctrl+C:Annuler");

    // Positionner le curseur
    int cursor_line, cursor_col;
    pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
    
    // Ajuster pour le défilement
    int screen_line = cursor_line - scroll_offset;
    
    // S'assurer que le curseur est visible
    if (screen_line >= 0 && screen_line < content_height) {
        if (cursor_col >= content_width) {
            cursor_col = content_width - 1;
        }
        set_cursor(content_y + screen_line, start_x + 2 + cursor_col);
    }
}

void windowed_write(const char* filename) {
    char content[1024] = {0};
    int cursor_pos = 0;
    int width = 76;
    int height = 21;
    int content_width = width - 4;
    int content_height = height - 4;
    int scroll_offset = 0;

    // Charger le fichier existant
    uint8_t existing_data[1024];
    int bytes_read = fs_read_file(filename, existing_data, sizeof(existing_data) - 1);
    if (bytes_read > 0) {
        existing_data[bytes_read] = '\0';
        strncpy(content, (char*)existing_data, sizeof(content) - 1);
        cursor_pos = strlen(content);
    }
    
    clear_screen();
    
    while (1) {
        // Calculer la position du curseur en ligne/colonne
        int cursor_line, cursor_col;
        pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
        
        // Ajuster le défilement pour garder le curseur visible
        if (cursor_line < scroll_offset) {
            scroll_offset = cursor_line;
        }
        if (cursor_line >= scroll_offset + content_height) {
            scroll_offset = cursor_line - content_height + 1;
        }
        
        draw_editor_window(filename, content, cursor_pos, scroll_offset);

        char c = keyboard_get_char();

        if (c == 27) {  // ESC - Sauvegarder
            fs_write_file(filename, (uint8_t*)content, strlen(content));
            clear_screen();
            print_string("Fichier sauvegarde : ");
            print_string(filename);
            print_string("\n");
            break;
        }
        else if (c == 3) {  // Ctrl+C - Annuler
            clear_screen();
            print_string("edition annulee\n");
            break;
        }
        else if (c == 1) {  // Flèche haut
            // Remonter d'une ligne
            int cursor_line, cursor_col;
            pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
            
            if (cursor_line > 0) {
                // Trouver le début de la ligne précédente
                const char* prev_line = get_line_start(content, cursor_line - 1);
                if (prev_line) {
                    int prev_line_len = get_line_length(prev_line);
                    // Positionner au même offset dans la ligne précédente (ou à la fin)
                    int target_col = cursor_col < prev_line_len ? cursor_col : prev_line_len;
                    cursor_pos = (prev_line - content) + target_col;
                }
            }
        }
        else if (c == 2) {  // Flèche bas
            // Descendre d'une ligne
            int cursor_line, cursor_col;
            pos_to_line_col(content, cursor_pos, &cursor_line, &cursor_col);
            
            int total_lines = count_lines(content);
            if (cursor_line < total_lines - 1) {
                // Trouver le début de la ligne suivante
                const char* next_line = get_line_start(content, cursor_line + 1);
                if (next_line) {
                    int next_line_len = get_line_length(next_line);
                    // Positionner au même offset dans la ligne suivante (ou à la fin)
                    int target_col = cursor_col < next_line_len ? cursor_col : next_line_len;
                    cursor_pos = (next_line - content) + target_col;
                }
            }
        }
        else if (c == 17) {  // Flèche gauche
            if (cursor_pos > 0) {
                cursor_pos--;
            }
        }
        else if (c == 18) {  // Flèche droite
            if (cursor_pos < (int)strlen(content)) {
                cursor_pos++;
            }
        }
        else if ((c == '\b' || c == 127) && cursor_pos > 0) {  // Backspace ou Delete
            // Supprimer le caractère avant le curseur
            for (int i = cursor_pos - 1; i < (int)strlen(content); i++) {
                content[i] = content[i + 1];
            }
            cursor_pos--;
        }
        else if (c == '\r' || c == '\n') {  // Entrée - Nouvelle ligne
            if (cursor_pos < (int)sizeof(content) - 2) {
                // Insérer un retour à la ligne
                int len = strlen(content);
                for (int i = len; i >= cursor_pos; i--) {
                    content[i + 1] = content[i];
                }
                content[cursor_pos] = '\n';
                cursor_pos++;
            }
        }
        else if (c >= 32 && c < 127) {  // Caractère imprimable
            if (cursor_pos < (int)sizeof(content) - 2) {
                // Insérer le caractère
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

void tetra_shell(void) {
    char input[256];
    
    // Afficher le contenu du README.txt s'il existe (premier démarrage)
    uint8_t readme_buffer[2048];
    int readme_result = fs_read_file("README.txt", readme_buffer, sizeof(readme_buffer) - 1);
    if (readme_result > 0) {
        readme_buffer[readme_result] = '\0';
        print_string((char*)readme_buffer);
        print_string("\n");
        print_string("   /$$$$$$$$          /$$                         /$$$$$$   /$$$$$$  \n");
        print_string("  |__  $$__/         | $$                        /$$__  $$ /$$__  $$ \n");
        print_string("     | $$  /$$$$$$  /$$$$$$    /$$$$$$  /$$$$$$ | $$  \\ $$| $$  \\__/ \n");
        print_string("     | $$ /$$__  $$|_  $$_/   /$$__  $$|____  $$| $$  | $$|  $$$$$$  \n");
        print_string("     | $$| $$$$$$$$  | $$    | $$  \\__/ /$$$$$$$| $$  | $$ \\____  $$ \n");
        print_string("     | $$| $$_____/  | $$ /$$| $$      /$$__  $$| $$  | $$ /$$  \\ $$ \n");
        print_string("     | $$|  $$$$$$$  |  $$$$/| $$     |  $$$$$$$|  $$$$$$/|  $$$$$$/ \n");
        print_string("     |__/ \\_______/   \\___/  |__/      \\_______/ \\______/  \\______/  \n");
        print_string("\n");
        print_string("========================================\n");
        print_string("       TetraOS Shell v1.3 (Fr)      \n");
        print_string("========================================\n");
        print_string("Tapez 'help' pour la liste des commandes\n\n");
    }

    while (1) {
        print_string("root@TetraOS:");

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

        print_string("# ");

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
            else if (c == 27) {
                input[0] = '\0';
                print_string("^C\n");
                break;
            }
        }

        if (strlen(input) == 0) continue;

        const char* s = input;
        while (*s == ' ') s++;

        if (strcmp(s, "help") == 0) {
            print_string("Commandes disponibles :\n");
            print_string("  ls              - Lister les fichiers et dossiers\n");
            print_string("  tree            - Afficher l'arborescence\n");
            print_string("  cd <dossier>    - Changer de dossier\n");
            print_string("  pwd             - Afficher le dossier courant\n");
            print_string("  mkdir <nom>     - Creer un dossier\n");
            print_string("  touch <nom>     - Creer un fichier vide\n");
            print_string("  edit <fichier>  - editer un fichier (cree si necessaire)\n");
            print_string("  cat <fichier>   - Afficher le contenu d'un fichier\n");
            print_string("  rm <nom>        - Supprimer un fichier/dossier\n");
            print_string("  tex <fichier>   - Executer un fichier .tex\n");
            print_string("  clear           - Effacer l'ecran\n");
            print_string("  info            - Afficher les informations du systeme de fichiers\n");
            print_string("  sl              - Animation amusante\n");
            print_string("  shutdown        - eteindre le systeme\n");
        }
        else if (strcmp(s, "log") == 0) {
            print_string("--[ Journal de log de TetraOS v1.3 ]---\n");
            print_string(" \n");
            print_string("V1.3 :\n");
            print_string(" -Ajout du system tex (TetraExecutable) qui permet maintenant d'avoire des executables en .tex interne a l'OS ... rien que ca.\n");
            print_string(" Voir la docu dans l'OS pour plus d'info sur son fonctionnement.\n");
            print_string("V1.2 :\n");
            print_string(" -Traduction de la plupart de l'OS en francais\n");
            print_string("\n");
            print_string("V1.1 :\n");
            print_string(" -Ajout des fonctions suivantes dans RAY64 FS :\n");
            print_string("   -tree\n");
            print_string("   -cd\n");
            print_string("   -pwd\n");
            print_string("   -mkdir\n");
            print_string("   -rm\n");
            print_string("\n");
            print_string("V1.0\n");
            print_string(" -Ajout du system de fichier RAY64 :\n");
            print_string("   -touch\n");
            print_string("   -edit\n");
            print_string("   -cat\n");
            print_string("   -rm\n");
            print_string("   -info\n");
            print_string(" -Ajout du TetraShell : \n");
            print_string(" -Stabilisation du system d'affichage VGA\n");
        }
else if (strcmp(s, "shutdown") == 0) {
    clear_screen();
    print_string(" /$$$$$$$$          /$$                         /$$$$$$   /$$$$$$  \n");
    print_string("|__  $$__/         | $$                        /$$__  $$ /$$__  $$ \n");
    print_string("   | $$  /$$$$$$  /$$$$$$    /$$$$$$  /$$$$$$ | $$  \\ $$| $$  \\__/ \n");
    print_string("   | $$ /$$__  $$|_  $$_/   /$$__  $$|____  $$| $$  | $$|  $$$$$$  \n");
    print_string("   | $$| $$$$$$$$  | $$    | $$  \\__/ /$$$$$$$| $$  | $$ \\____  $$ \n");
    print_string("   | $$| $$_____/  | $$ /$$| $$      /$$__  $$| $$  | $$ /$$  \\ $$ \n");
    print_string("   | $$|  $$$$$$$  |  $$$$/| $$     |  $$$$$$$|  $$$$$$/|  $$$$$$/ \n");
    print_string("   |__/ \\_______/   \\___/  |__/      \\_______/ \\______/  \\______/  \n");
    print_string("\n");
    print_string("Extinction du systeme en cours...\n");

    delay_spin(120000000);
    outw(0x604, 0x2000);
}

        else if (strcmp(s, "clear") == 0) {
            clear_screen();
        }
        else if (strcmp(s, "ls") == 0) {
            fs_ls();
        }
        else if (strcmp(s, "tree") == 0) {
            fs_tree();
        }
        else if (strcmp(s, "info") == 0) {
            fs_list();
        }
        else if (strcmp(s, "pwd") == 0) {
            fs_pwd();
        }
        else if (strncmp(s, "cd ", 3) == 0) {
            char *path = (char*)(s + 3);
            while (*path == ' ') path++;
            if (fs_cd(path) != 0) {
                print_string("cd : dossier introuvable : ");
                print_string(path);
                print_string("\n");
            }
        }
        else if (strncmp(s, "mkdir ", 6) == 0) {
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            if (fs_mkdir(name) >= 0) {
                print_string("Dossier cree : ");
                print_string(name);
                print_string("\n");
            } else {
                print_string("mkdir : echec (existe dejà ou systeme de fichiers plein)\n");
            }
        }
        else if (strncmp(s, "touch ", 6) == 0) {
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            if (fs_add(name) >= 0) {
                print_string("Fichier cree : ");
                print_string(name);
                print_string("\n");
            } else {
                print_string("touch : echec (existe dejà ou systeme de fichiers plein)\n");
            }
        }
        else if (strncmp(s, "edit ", 5) == 0) {
            char *name = (char*)(s + 5);
            while (*name == ' ') name++;

            int idx = fs_find(name);
            if (idx < 0) {
                print_string("Creation d'un nouveau fichier : ");
                print_string(name);
                print_string("\n");
                if (fs_add(name) < 0) {
                    print_string("edit : echec lors de la creation du fichier\n");
                    continue;
                }
            }

            windowed_write(name);
        }
        else if (strncmp(s, "cat ", 4) == 0) {
            char *name = (char*)(s + 4);
            while (*name == ' ') name++;

            uint8_t buffer[2048];
            int result = fs_read_file(name, buffer, sizeof(buffer) - 1);
            if (result > 0) {
                buffer[result] = '\0';
                print_string((char*)buffer);
                if (buffer[result - 1] != '\n') {
                    print_char('\n');
                }
            } else if (result == 0) {
                print_string("(fichier vide)\n");
            } else {
                print_string("cat : impossible de lire le fichier : ");
                print_string(name);
                print_string("\n");
            }
        }
        else if (strncmp(s, "rm ", 3) == 0) {
            char *name = (char*)(s + 3);
            while (*name == ' ') name++;

            if (fs_delete(name) == 0) {
                print_string("Supprime : ");
                print_string(name);
                print_string("\n");
            } else {
                print_string("rm : impossible de supprimer : ");
                print_string(name);
                print_string("\n");
            }
        }
        else if (strcmp(s, "sl") == 0) {
            cmd_sl();
        }
        else if (strncmp(s, "tex ", 4) == 0) {
            char *name = (char*)(s + 4);
            while (*name == ' ') name++;
            
            print_string("Execution du script TEX: ");
            print_string(name);
            print_string("\n");
            print_string("---\n");
            
            if (tex_execute(name) < 0) {
                print_string("Erreur lors de l'execution\n");
            }
            
            print_string("---\n");
        }
        else {
            print_string("Commande introuvable : ");
            print_string(s);
            print_string("\n");
            print_string("Tapez 'help' pour la liste des commandes\n");
        }
    }
}