#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"

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

void draw_editor_window(const char* filename, const char* content, int cursor_pos) {
    int width = 60;
    int height = 10;
    int start_x = (80 - width) / 2;
    int start_y = (25 - height) / 2;
    
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
    int max_chars = width - 4;

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

    set_cursor(start_y + height - 2, start_x + 2);
    print_string("ECHAP:Sauvegarder  Ctrl+C:Annuler");

    int cursor_x = start_x + 2 + (cursor_pos % max_chars);
    int cursor_y = content_y + (cursor_pos / max_chars);
    set_cursor(cursor_y, cursor_x);
}

void windowed_write(const char* filename) {
    char content[1024] = {0};
    int cursor_pos = 0;
    int width = 60;
    int height = 10;
    int max_chars = width - 4;
    int max_content = (height - 4) * max_chars;

    uint8_t existing_data[1024];
    int bytes_read = fs_read_file(filename, existing_data, sizeof(existing_data) - 1);
    if (bytes_read > 0) {
        existing_data[bytes_read] = '\0';
        strncpy(content, (char*)existing_data, sizeof(content) - 1);
        cursor_pos = strlen(content);
    }
    clear_screen();
    while (1) {
        draw_editor_window(filename, content, cursor_pos);

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
        else if (c == '\b' && cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < (int)strlen(content); i++) {
                content[i] = content[i + 1];
            }
            cursor_pos--;
        }
        else if (c == '\r' || c == '\n') {
            if (cursor_pos < max_content - 1) {
                for (int i = strlen(content) + 1; i > cursor_pos; i--) {
                    content[i] = content[i - 1];
                }
                content[cursor_pos++] = '\n';
            }
        }
        else if (c >= 32 && c < 127 && cursor_pos < max_content - 1) {
            for (int i = strlen(content) + 1; i > cursor_pos; i--) {
                content[i] = content[i - 1];
            }
            content[cursor_pos++] = c;
        }

        content[sizeof(content) - 1] = '\0';
    }
}

void tetra_shell(void) {
    char input[256];
    print_string("\n");
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
    print_string("       TetraOS Shell v1.2 (Fr)      \n");
    print_string("========================================\n");
    print_string("Tapez 'help' pour la liste des commandes\n\n");

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
            print_string("  clear           - Effacer l'ecran\n");
            print_string("  info            - Afficher les informations du systeme de fichiers\n");
            print_string("  sl              - Animation amusante\n");
            print_string("  shutdown        - eteindre le systeme\n");
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
        else {
            print_string("Commande introuvable : ");
            print_string(s);
            print_string("\n");
            print_string("Tapez 'help' pour la liste des commandes\n");
        }
    }
}
