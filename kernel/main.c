#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"
#include "tex.h"
#include "session.h"

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
void read_line(char* buffer, int max_len);

void kmain(void) {
    print_string("ETAPE 1 : Debut kmain()\n");

    print_string("ETAPE 2 : Initialisation ecran\n");
    clear_screen();

    print_string("ETAPE 3 : Initialisation ATA\n");
    ata_init();

    print_string("ETAPE 4 : Initialisation systeme de fichiers\n");
    fs_init();

    print_string("ETAPE 5 : Initialisation systeme de sessions\n");
    session_init();
    session_load();
    
    // Boucle principale : login -> shell -> logout -> login
    // Le login est géré ENTIÈREMENT par session.c
    while (1) {
        // Processus de login complet (autonome, aucune vérification de permissions)
        if (session_do_login_flow() != 0) {
            print_string("Erreur fatale lors du login\n");
            while(1) { asm volatile ("nop"); }
        }
        
        // À ce stade, l'utilisateur est connecté
        // Lancer le shell avec vérifications de permissions
        tetra_shell();
        
        // Si on arrive ici, c'est qu'il y a eu un logout
        print_string("\nDeconnexion...\n");
    }

    print_string("ETAPE 6 : Retour du shell (anormal)\n");
    while(1) { asm volatile ("nop"); }
}

void read_line(char* buffer, int max_len) {
    int idx = 0;
    
    while (1) {
        char c = keyboard_get_char();
        
        if (c == '\n' || c == '\r') {
            buffer[idx] = '\0';
            print_char('\n');
            break;
        }
        else if ((c == '\b' || c == 127) && idx > 0) {
            idx--;
            print_string("\b \b");
        }
        else if (c >= 32 && c <= 126 && idx < max_len - 1) {
            buffer[idx++] = c;
            print_char(c);
        }
    }
}

// ============================================================================
// COMMANDES DE SESSION
// ============================================================================

void cmd_adduser(void) {
    if (!session_has_permission(PERM_SESSION_CREATE)) {
        print_string("Erreur: Permission refusee (admin requis)\n");
        return;
    }
    
    char name[SESSION_NAME_LEN];
    char password[SESSION_PASSWORD_LEN];
    char is_admin_str[10];
    
    print_string("Nom de la session: ");
    read_line(name, SESSION_NAME_LEN);
    
    print_string("Mot de passe: ");
    read_line(password, SESSION_PASSWORD_LEN);
    
    print_string("Admin? (o/n): ");
    read_line(is_admin_str, 10);
    
    uint8_t is_admin = (is_admin_str[0] == 'o' || is_admin_str[0] == 'O') ? 1 : 0;
    
    int result = session_create(name, password, is_admin);
    
    if (result >= 0) {
        print_string("Session '");
        print_string(name);
        print_string("' creee avec succes!\n");
    } else {
        print_string("Erreur: Impossible de creer la session\n");
    }
}

void cmd_deluser(void) {
    if (!session_has_permission(PERM_SESSION_DELETE)) {
        print_string("Erreur: Permission refusee (admin requis)\n");
        return;
    }
    
    char name[SESSION_NAME_LEN];
    
    print_string("Nom de la session a supprimer: ");
    read_line(name, SESSION_NAME_LEN);
    
    if (strcmp(name, session_get_current_name()) == 0) {
        print_string("Erreur: Impossible de supprimer votre propre session\n");
        return;
    }
    
    int result = session_delete(name);
    
    if (result == 0) {
        print_string("Session '");
        print_string(name);
        print_string("' supprimee\n");
    } else {
        print_string("Erreur: Session non trouvee\n");
    }
}

void cmd_users(void) {
    session_list();
}

void cmd_whoami(void) {
    print_string("Session actuelle: ");
    print_string(session_get_current_name());
    
    if (session_is_admin()) {
        print_string(" [ADMIN]");
    }
    
    print_string("\n");
}

// ============================================================================
// FONCTIONS AUXILIAIRES EXISTANTES
// ============================================================================

static void delay_spin(uint32_t loops) {
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < loops; i++) {
        x += i;
    }
    (void)x;
}

static void draw_train_at(void) {
    print_string("Rayu : CPT pour le moment, deso ...");
}

static void cmd_sl(void) {
    for (int pos = -70; pos < MAX_COLS; pos++) {
        clear_screen();
        draw_train_at();
        delay_spin(4000000);
    }
}

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

void windowed_write(const char* filename) {
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

void tetra_shell(void) {
    char input[256];
    
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
        print_string("       TetraOS Shell v1.4 (Fr)      \n");
        print_string("========================================\n");
        print_string("Tapez 'help' pour la liste des commandes\n\n");
    }

    while (g_session_manager.logged_in) {
        print_string(session_get_current_name());
        print_string("@TetraOS:");

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

        if (session_is_admin()) {
            print_string("# ");
        } else {
            print_string("$ ");
        }

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

        // ====== COMMANDES DE SESSION ======
        if (strcmp(s, "adduser") == 0) {
            cmd_adduser();
        }
        else if (strcmp(s, "deluser") == 0) {
            cmd_deluser();
        }
        else if (strcmp(s, "users") == 0) {
            cmd_users();
        }
        else if (strcmp(s, "whoami") == 0) {
            cmd_whoami();
        }
        else if (strcmp(s, "logout") == 0) {
            print_string("Deconnexion...\n");
            session_logout();
            return;
        }
        
        // ====== COMMANDES SYSTEME ======
        else if (strcmp(s, "help") == 0) {
            print_string("Commandes disponibles :\n");
            print_string("  ls              - Lister les fichiers et dossiers\n");
            print_string("  tree            - Afficher l'arborescence\n");
            print_string("  cd <dossier>    - Changer de dossier\n");
            print_string("  pwd             - Afficher le dossier courant\n");
            print_string("  mkdir <nom>     - Creer un dossier\n");
            print_string("  touch <nom>     - Creer un fichier vide\n");
            print_string("  edit <fichier>  - editer un fichier\n");
            print_string("  cat <fichier>   - Afficher le contenu\n");
            print_string("  rm <nom>        - Supprimer un fichier/dossier\n");
            print_string("  tex <fichier>   - Executer un fichier .tex\n");
            print_string("  clear           - Effacer l'ecran\n");
            print_string("  info            - Informations du systeme de fichiers\n");
            print_string("  sl              - Animation\n");
            print_string("  shutdown        - Eteindre le systeme\n");
            print_string("\nGestion des sessions :\n");
            print_string("  whoami          - Session actuelle\n");
            print_string("  users           - Lister les sessions\n");
            print_string("  adduser         - Creer une session (admin)\n");
            print_string("  deluser         - Supprimer une session (admin)\n");
            print_string("  logout          - Se deconnecter\n");
        }
        else if (strcmp(s, "log") == 0) {
            print_string("--[ Journal de log de TetraOS v1.4 ]---\n");
            print_string(" \n");
            print_string("V1.4 :\n");
            print_string(" -Ajout du systeme de sessions multi-utilisateurs\n");
            print_string(" -Gestion des permissions par session\n");
            print_string(" -Menu de selection au demarrage\n");
            print_string(" -Roles admin/utilisateur\n");
            print_string("\n");
            print_string("V1.3 :\n");
            print_string(" -Ajout du system tex (TetraExecutable)\n");
            print_string("V1.2 :\n");
            print_string(" -Traduction en francais\n");
            print_string("V1.1 :\n");
            print_string(" -Ajout de tree, cd, pwd, mkdir, rm\n");
            print_string("V1.0 :\n");
            print_string(" -Ajout du system RAY64 FS\n");
        }
        else if (strcmp(s, "shutdown") == 0) {
            if (!session_has_permission(PERM_SYSTEM_SHUTDOWN)) {
                print_string("Permission refusee\n");
                continue;
            }
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
            if (session_has_permission(PERM_CLEAR_SCREEN)) {
                clear_screen();
            } else {
                print_string("Permission refusee\n");
            }
        }
        else if (strcmp(s, "ls") == 0) {
            if (session_has_permission(PERM_LIST_FILES)) {
                fs_ls();
            } else {
                print_string("Permission refusee\n");
            }
        }
        else if (strcmp(s, "tree") == 0) {
            if (session_has_permission(PERM_LIST_FILES)) {
                fs_tree();
            } else {
                print_string("Permission refusee\n");
            }
        }
        else if (strcmp(s, "info") == 0) {
            if (session_has_permission(PERM_LIST_FILES)) {
                fs_list();
            } else {
                print_string("Permission refusee\n");
            }
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
            if (!session_has_permission(PERM_FS_WRITE)) {
                print_string("Permission refusee\n");
                continue;
            }
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            if (fs_mkdir(name) >= 0) {
                print_string("Dossier cree : ");
                print_string(name);
                print_string("\n");
            } else {
                print_string("mkdir : echec\n");
            }
        }
        else if (strncmp(s, "touch ", 6) == 0) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                print_string("Permission refusee\n");
                continue;
            }
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            if (fs_add(name) >= 0) {
                print_string("Fichier cree : ");
                print_string(name);
                print_string("\n");
            } else {
                print_string("touch : echec\n");
            }
        }
        else if (strncmp(s, "edit ", 5) == 0) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                print_string("Permission refusee\n");
                continue;
            }
            char *name = (char*)(s + 5);
            while (*name == ' ') name++;

            int idx = fs_find(name);
            if (idx < 0) {
                print_string("Creation d'un nouveau fichier : ");
                print_string(name);
                print_string("\n");
                if (fs_add(name) < 0) {
                    print_string("edit : echec\n");
                    continue;
                }
            }

            windowed_write(name);
        }
        else if (strncmp(s, "cat ", 4) == 0) {
            if (!session_has_permission(PERM_FS_READ)) {
                print_string("Permission refusee\n");
                continue;
            }
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
                print_string("cat : impossible de lire : ");
                print_string(name);
                print_string("\n");
            }
        }
        else if (strncmp(s, "rm ", 3) == 0) {
            if (!session_has_permission(PERM_FS_DELETE)) {
                print_string("Permission refusee\n");
                continue;
            }
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
            if (!session_has_permission(PERM_TEX_EXECUTE)) {
                print_string("Permission refusee\n");
                continue;
            }
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