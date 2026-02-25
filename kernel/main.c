#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"
#include "tex.h"
#include "session.h"
#include "vesa.h"
#include "vesaanim.h"
#include "mouse.h"
#include "desktop.h"

__attribute__((naked)) __attribute__((section(".text.start")))
void start(void) {
    asm volatile (
        /* 1. Pile au-dessus du BSS */
        "mov $0x600000, %esp\n"
        "mov %esp, %ebp\n"

        /* 2. Segments mode protégé */
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n"
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        "mov %ax, %ss\n"

        /* 3. Zéro-initialiser le BSS — INDISPENSABLE en bare metal */
        /* sans ça les variables globales/statiques ont du garbage    */
        "mov $_bss_start, %edi\n"
        "mov $_bss_end,   %ecx\n"
        "sub %edi, %ecx\n"          /* ecx = taille du BSS en bytes */
        "shr $2, %ecx\n"            /* /4 → nombre de dwords */
        "xor %eax, %eax\n"
        "rep stosl\n"               /* memset BSS à 0 */

        /* 4. Lancer le kernel */
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
    // Initialisation écran EN PREMIER — détecte VESA ou VGA
    screen_init();
    clear_screen();

    print_string("ETAPE 1 : Debut kmain()\n");

    print_string("ETAPE 2 : Ecran initialise");
    print_string(vesa_active() ? " (VESA 1920x1080)\n" : " (VGA 80x25)\n");

    print_string("ETAPE 3 : Initialisation ATA\n");
    ata_init();

    print_string("ETAPE 4 : Initialisation systeme de fichiers\n");
    fs_init();

    vesa_boot_anim();

    print_string("ETAPE 5 : Initialisation systeme de sessions\n");
    session_init();
    session_load();

    print_string("ETAPE 5b : Initialisation souris PS/2\n");
    mouse_init();
    
    // Boucle principale : login -> bureau -> logout -> login
    while (1) {
        if (session_do_login_flow() != 0) {
            print_string("Erreur fatale lors du login\n");
            while(1) { asm volatile ("nop"); }
        }
        // Bureau graphique — remplace le shell texte brut
        desktop_run();
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

void cmd_sl(void) {
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

// ============================================================================
// SHELL NAMESPACED - SYSTÈME DE DISPATCH module.commande
// ============================================================================

// Utilitaire : parse "module.cmd arg1 arg2..." -> module, cmd, args
// Retourne 1 si le format module.cmd est respecté, 0 sinon
int parse_namespaced(const char* input,
                             char* mod_out,  int mod_max,
                             char* cmd_out,  int cmd_max,
                             char* args_out, int args_max) {
    // Trouver le '.'
    int dot = -1;
    for (int i = 0; input[i] != '\0'; i++) {
        if (input[i] == '.') { dot = i; break; }
    }
    if (dot < 0) return 0;

    // module = tout avant le '.'
    int mlen = dot < mod_max - 1 ? dot : mod_max - 1;
    for (int i = 0; i < mlen; i++) mod_out[i] = input[i];
    mod_out[mlen] = '\0';

    // cmd+args = tout après le '.'
    const char* after = input + dot + 1;

    // Trouver l'espace séparateur cmd/args
    int sp = -1;
    for (int i = 0; after[i] != '\0'; i++) {
        if (after[i] == ' ') { sp = i; break; }
    }

    if (sp < 0) {
        // Pas d'argument
        int clen = 0;
        while (after[clen] != '\0' && clen < cmd_max - 1) { cmd_out[clen] = after[clen]; clen++; }
        cmd_out[clen] = '\0';
        args_out[0] = '\0';
    } else {
        int clen = sp < cmd_max - 1 ? sp : cmd_max - 1;
        for (int i = 0; i < clen; i++) cmd_out[i] = after[i];
        cmd_out[clen] = '\0';
        const char* a = after + sp + 1;
        while (*a == ' ') a++;
        int alen = 0;
        while (a[alen] != '\0' && alen < args_max - 1) { args_out[alen] = a[alen]; alen++; }
        args_out[alen] = '\0';
    }
    return 1;
}

// --------------------------------------------------------------------------
// Dispatch ray64.*
// --------------------------------------------------------------------------
void dispatch_ray64(const char* cmd, const char* args) {
    // ray64.ls
    if (strcmp(cmd, "ls") == 0) {
        if (!session_has_permission(PERM_LIST_FILES)) { print_string("Permission refusee\n"); return; }
        fs_ls();
    }
    // ray64.tree
    else if (strcmp(cmd, "tree") == 0) {
        if (!session_has_permission(PERM_LIST_FILES)) { print_string("Permission refusee\n"); return; }
        fs_tree();
    }
    // ray64.info
    else if (strcmp(cmd, "info") == 0) {
        if (!session_has_permission(PERM_LIST_FILES)) { print_string("Permission refusee\n"); return; }
        fs_list();
    }
    // ray64.pwd
    else if (strcmp(cmd, "pwd") == 0) {
        fs_pwd();
    }
    // ray64.cd <path>
    else if (strcmp(cmd, "cd") == 0) {
        if (args[0] == '\0') { fs_cd("/"); return; }
        if (fs_cd(args) != 0) {
            print_string("ray64.cd : dossier introuvable : ");
            print_string(args);
            print_string("\n");
        }
    }
    // ray64.home
    else if (strcmp(cmd, "home") == 0) {
        session_set_cwd_to_home();
        print_string("ray64.home : /home/");
        print_string(session_get_current_name());
        print_string("\n");
    }
    // ray64.mkdir <nom>
    else if (strcmp(cmd, "mkdir") == 0) {
        if (!session_has_permission(PERM_FS_WRITE)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("ray64.mkdir : nom requis\n"); return; }
        if (fs_mkdir(args) >= 0) {
            print_string("ray64.mkdir : cree -> ");
            print_string(args);
            print_string("\n");
        } else {
            print_string("ray64.mkdir : echec\n");
        }
    }
    // ray64.touch <nom>
    else if (strcmp(cmd, "touch") == 0) {
        if (!session_has_permission(PERM_FS_WRITE)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("ray64.touch : nom requis\n"); return; }
        if (fs_add(args) >= 0) {
            print_string("ray64.touch : cree -> ");
            print_string(args);
            print_string("\n");
        } else {
            print_string("ray64.touch : echec\n");
        }
    }
    // ray64.edit <fichier>
    else if (strcmp(cmd, "edit") == 0) {
        if (!session_has_permission(PERM_FS_WRITE)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("ray64.edit : nom requis\n"); return; }
        int idx = fs_find(args);
        if (idx < 0) {
            print_string("ray64.edit : creation -> ");
            print_string(args);
            print_string("\n");
            if (fs_add(args) < 0) { print_string("ray64.edit : echec creation\n"); return; }
        }
        windowed_write(args);
    }
    // ray64.cat <fichier>
    else if (strcmp(cmd, "cat") == 0) {
        if (!session_has_permission(PERM_FS_READ)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("ray64.cat : nom requis\n"); return; }
        uint8_t buffer[2048];
        int result = fs_read_file(args, buffer, sizeof(buffer) - 1);
        if (result > 0) {
            buffer[result] = '\0';
            print_string((char*)buffer);
            if (buffer[result - 1] != '\n') print_char('\n');
        } else if (result == 0) {
            print_string("(fichier vide)\n");
        } else {
            print_string("ray64.cat : impossible de lire : ");
            print_string(args);
            print_string("\n");
        }
    }
    // ray64.rm <nom>
    else if (strcmp(cmd, "rm") == 0) {
        if (!session_has_permission(PERM_FS_DELETE)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("ray64.rm : nom requis\n"); return; }
        if (fs_delete(args) == 0) {
            print_string("ray64.rm : supprime -> ");
            print_string(args);
            print_string("\n");
        } else {
            print_string("ray64.rm : impossible de supprimer : ");
            print_string(args);
            print_string("\n");
        }
    }
    // ray64.acl <nom>  →  afficher les permissions d'un nœud
    else if (strcmp(cmd, "acl") == 0) {
        if (args[0] == '\0') { print_string("ray64.acl : nom requis\n"); return; }
        int idx = fs_find(args);
        if (idx < 0) {
            print_string("ray64.acl : introuvable : ");
            print_string(args);
            print_string("\n");
        } else {
            print_string(args);
            print_string(" : ");
            fs_acl_print((uint32_t)idx);
            print_string("\n");
        }
    }
    // ray64.chmod <nom> <perms>
    // <perms> peut être :
    //   un nombre décimal (ex: 448 = 0700, 504 = 0770, 511 = 0777)
    //   ou un mot-clé : private(0700) shared(0770) public(0774) open(0777)
    else if (strcmp(cmd, "chmod") == 0) {
        if (!session_has_permission(PERM_FS_WRITE)) { print_string("Permission refusee\n"); return; }

        // Parser "nom perms" depuis args
        char cmod_name[64];
        char cmod_perms[32];
        int sp = -1;
        for (int ii = 0; args[ii] != '\0'; ii++) {
            if (args[ii] == ' ') { sp = ii; break; }
        }
        if (sp < 0) {
            print_string("ray64.chmod : usage : ray64.chmod <nom> <perms>\n");
            print_string("  perms : private(0700) shared(0770) public(0774) open(0777)\n");
            print_string("         ou valeur decimale (ex: 448=0700 504=0770 511=0777)\n");
            return;
        }
        int nlen = sp < 63 ? sp : 63;
        for (int ii = 0; ii < nlen; ii++) cmod_name[ii] = args[ii];
        cmod_name[nlen] = '\0';
        const char* pa = args + sp + 1;
        while (*pa == ' ') pa++;
        int plen = 0;
        while (pa[plen] != '\0' && plen < 31) { cmod_perms[plen] = pa[plen]; plen++; }
        cmod_perms[plen] = '\0';

        // Résoudre la valeur de permissions
        uint16_t new_perms = 0;
        if (strcmp(cmod_perms, "private") == 0) {
            new_perms = ACL_OWNER_FULL;                        // 0700 owner seul
        } else if (strcmp(cmod_perms, "shared") == 0) {
            new_perms = ACL_OWNER_FULL | ACL_ADMIN_FULL;       // 0770 owner+admin
        } else if (strcmp(cmod_perms, "public") == 0) {
            new_perms = ACL_OWNER_FULL | ACL_ADMIN_FULL | ACL_OTHER_R; // 0774 lecture pour tous
        } else if (strcmp(cmod_perms, "open") == 0) {
            new_perms = ACL_OWNER_FULL | ACL_ADMIN_FULL |
                        ACL_OTHER_R | ACL_OTHER_W | ACL_OTHER_X; // 0777 tout le monde
        } else {
            // Valeur décimale
            uint32_t val = 0;
            for (int ii = 0; cmod_perms[ii] != '\0'; ii++) {
                if (cmod_perms[ii] >= '0' && cmod_perms[ii] <= '9') {
                    val = val * 10 + (uint32_t)(cmod_perms[ii] - '0');
                } else {
                    print_string("ray64.chmod : valeur invalide : ");
                    print_string(cmod_perms);
                    print_string("\n");
                    return;
                }
            }
            new_perms = (uint16_t)(val & 0x1FF);
        }

        if (fs_chmod(cmod_name, new_perms) == 0) {
            print_string("ray64.chmod : ");
            print_string(cmod_name);
            print_string(" -> ");
            int idx2 = fs_find(cmod_name);
            if (idx2 >= 0) { fs_acl_print((uint32_t)idx2); }
            print_string("\n");
        }
    }
    // ray64.chown <nom> <session>  (admin seulement)
    else if (strcmp(cmd, "chown") == 0) {
        if (!session_is_admin()) { print_string("ray64.chown : admin requis\n"); return; }

        char co_name[64];
        char co_owner[SESSION_NAME_LEN];
        int sp = -1;
        for (int ii = 0; args[ii] != '\0'; ii++) {
            if (args[ii] == ' ') { sp = ii; break; }
        }
        if (sp < 0) {
            print_string("ray64.chown : usage : ray64.chown <nom> <session>\n");
            return;
        }
        int nlen = sp < 63 ? sp : 63;
        for (int ii = 0; ii < nlen; ii++) co_name[ii] = args[ii];
        co_name[nlen] = '\0';
        const char* oa = args + sp + 1;
        while (*oa == ' ') oa++;
        int olen = 0;
        while (oa[olen] != '\0' && olen < SESSION_NAME_LEN - 1) { co_owner[olen] = oa[olen]; olen++; }
        co_owner[olen] = '\0';

        // Résoudre le nom de session → uid
        int uid_idx = session_get_index_by_name(co_owner);
        if (uid_idx < 0) {
            print_string("ray64.chown : session introuvable : ");
            print_string(co_owner);
            print_string("\n");
            return;
        }

        if (fs_chown(co_name, (uint16_t)uid_idx) == 0) {
            print_string("ray64.chown : ");
            print_string(co_name);
            print_string(" -> proprietaire : ");
            print_string(co_owner);
            print_string("\n");
        }
    }
    else {
        print_string("ray64 : commande inconnue '");
        print_string(cmd);
        print_string("'\n");
        print_string("  ray64.ls / ray64.tree / ray64.info / ray64.pwd\n");
        print_string("  ray64.cd <path> / ray64.home\n");
        print_string("  ray64.mkdir <n> / ray64.touch <n> / ray64.edit <n>\n");
        print_string("  ray64.cat <n> / ray64.rm <n>\n");
        print_string("  ray64.acl <n> / ray64.chmod <n> <perms> / ray64.chown <n> <session>\n");
    }
}

// --------------------------------------------------------------------------
// Dispatch session.*
// --------------------------------------------------------------------------
void dispatch_session(const char* cmd, const char* args) {
    // session.whoami
    if (strcmp(cmd, "whoami") == 0) {
        print_string("session : ");
        print_string(session_get_current_name());
        if (session_is_admin()) print_string(" [ADMIN]");
        print_string("\n");
    }
    // session.list
    else if (strcmp(cmd, "list") == 0) {
        session_list();
    }
    // session.logout
    else if (strcmp(cmd, "logout") == 0) {
        print_string("session.logout : deconnexion...\n");
        session_logout();
    }
    // session.adduser
    else if (strcmp(cmd, "adduser") == 0) {
        cmd_adduser();
    }
    // session.deluser
    else if (strcmp(cmd, "deluser") == 0) {
        cmd_deluser();
    }
    // session.cdhome <nom>  (admin)
    else if (strcmp(cmd, "cdhome") == 0) {
        if (!session_is_admin()) { print_string("Permission refusee (admin requis)\n"); return; }
        if (args[0] == '\0') { print_string("session.cdhome : nom de session requis\n"); return; }
        session_admin_browse_home(args);
    }
    else {
        print_string("session : commande inconnue '");
        print_string(cmd);
        print_string("'\n");
        print_string("  session.whoami / session.list\n");
        print_string("  session.logout / session.adduser / session.deluser\n");
        if (session_is_admin()) print_string("  session.cdhome <nom>\n");
    }
}

// --------------------------------------------------------------------------
// Dispatch sys.*
// --------------------------------------------------------------------------
void dispatch_sys(const char* cmd, const char* args) {
    (void)args;
    // sys.help
    if (strcmp(cmd, "help") == 0) {
        print_string("========================================\n");
        print_string("   TetraOS - Commandes Namespacees      \n");
        print_string("========================================\n\n");

        print_string("[ray64] Systeme de fichiers RAY64\n");
        print_string("  ray64.ls              Lister le dossier courant\n");
        print_string("  ray64.tree            Arborescence complete\n");
        print_string("  ray64.info            Noeuds FS (debug)\n");
        print_string("  ray64.pwd             Chemin courant\n");
        print_string("  ray64.cd <path>       Changer de dossier\n");
        print_string("  ray64.home            Aller dans son home\n");
        print_string("  ray64.mkdir <nom>     Creer un dossier\n");
        print_string("  ray64.touch <nom>     Creer un fichier vide\n");
        print_string("  ray64.edit  <nom>     Editer un fichier\n");
        print_string("  ray64.cat   <nom>     Afficher un fichier\n");
        print_string("  ray64.rm    <nom>     Supprimer\n");
        print_string("  ray64.acl   <nom>     Voir les permissions d'un noeud\n");
        print_string("  ray64.chmod <nom> <perms>  Changer les permissions\n");
        print_string("    perms: private(700) shared(770) public(774) open(777)\n");
        print_string("           ou valeur decimale (ex: 448)\n");
        print_string("  ray64.chown <nom> <session>  Changer proprietaire [admin]\n\n");

        print_string("[session] Gestion des sessions\n");
        print_string("  session.whoami        Session actuelle\n");
        print_string("  session.list          Lister les sessions\n");
        print_string("  session.logout        Se deconnecter\n");
        print_string("  session.adduser       Creer une session  [admin]\n");
        print_string("  session.deluser       Supprimer une session  [admin]\n");
        print_string("  session.cdhome <nom>  Acceder au home d'une session  [admin]\n\n");

        print_string("[tex] Moteur de scripts TetraExecutable\n");
        print_string("  tex.run  <fichier>    Executer un script .tex\n");
        print_string("  tex.list              Lister les scripts\n\n");

        print_string("[sys] Systeme\n");
        print_string("  sys.help              Cette aide\n");
        print_string("  sys.clear             Effacer l'ecran\n");
        print_string("  sys.shutdown          Eteindre\n");
        print_string("  sys.log               Journal des versions\n\n");

        print_string("[divers]\n");
        print_string("  sl                    ...\n");
    }
    // sys.clear
    else if (strcmp(cmd, "clear") == 0) {
        if (!session_has_permission(PERM_CLEAR_SCREEN)) { print_string("Permission refusee\n"); return; }
        clear_screen();
    }
    // sys.shutdown
    else if (strcmp(cmd, "shutdown") == 0) {
        if (!session_has_permission(PERM_SYSTEM_SHUTDOWN)) { print_string("Permission refusee\n"); return; }
        clear_screen();
        print_string("   /$$$$$$$$          /$$                         /$$$$$$   /$$$$$$  \n");
        print_string("  |__  $$__/         | $$                        /$$__  $$ /$$__  $$ \n");
        print_string("     | $$  /$$$$$$  /$$$$$$    /$$$$$$  /$$$$$$ | $$  \\ $$| $$  \\__/ \n");
        print_string("     | $$ /$$__  $$|_  $$_/   /$$__  $$|____  $$| $$  | $$|  $$$$$$  \n");
        print_string("     | $$| $$$$$$$$  | $$    | $$  \\__/ /$$$$$$$| $$  | $$ \\____  $$ \n");
        print_string("     | $$| $$_____/  | $$ /$$| $$      /$$__  $$| $$  | $$ /$$  \\ $$ \n");
        print_string("     | $$|  $$$$$$$  |  $$$$/| $$     |  $$$$$$$|  $$$$$$/|  $$$$$$/ \n");
        print_string("     |__/ \\_______/   \\___/  |__/      \\_______/ \\______/  \\______/  \n");
        print_string("\nsys.shutdown : extinction en cours...\n");
        delay_spin(120000000);
        outw(0x604, 0x2000);
    }
    // sys.log
    else if (strcmp(cmd, "log") == 0) {
        print_string("--[ TetraOS - Journal des versions ]--\n");
        print_string("V1.5 :\n");
        print_string(" -Commandes namespacees (ray64.*, session.*, sys.*, tex.*)\n");
        print_string(" -Espaces de stockage isoles par session (/home/<nom>)\n");
        print_string(" -Acces admin aux homes via session.cdhome\n");
        print_string("V1.4 :\n");
        print_string(" -Systeme de sessions multi-utilisateurs\n");
        print_string(" -Gestion des permissions par session\n");
        print_string(" -Menu de selection au demarrage\n");
        print_string(" -Roles admin/utilisateur\n");
        print_string("V1.3 :\n");
        print_string(" -Ajout du system tex (TetraExecutable)\n");
        print_string("V1.2 :\n");
        print_string(" -Traduction en francais\n");
        print_string("V1.1 :\n");
        print_string(" -Ajout de tree, cd, pwd, mkdir, rm\n");
        print_string("V1.0 :\n");
        print_string(" -Ajout du system RAY64 FS\n");
    }
    else {
        print_string("sys : commande inconnue '");
        print_string(cmd);
        print_string("'\n");
        print_string("  sys.help / sys.clear / sys.shutdown / sys.log\n");
    }
}

// --------------------------------------------------------------------------
// Dispatch tex.*
// --------------------------------------------------------------------------
void dispatch_tex(const char* cmd, const char* args) {
    // tex.run <fichier>
    if (strcmp(cmd, "run") == 0) {
        if (!session_has_permission(PERM_TEX_EXECUTE)) { print_string("Permission refusee\n"); return; }
        if (args[0] == '\0') { print_string("tex.run : nom de fichier requis\n"); return; }
        print_string("tex.run : execution de ");
        print_string(args);
        print_string("\n---\n");
        if (tex_execute(args) < 0) {
            print_string("tex.run : erreur lors de l'execution\n");
        }
        print_string("---\n");
    }
    // tex.list
    else if (strcmp(cmd, "list") == 0) {
        if (!session_has_permission(PERM_LIST_FILES)) { print_string("Permission refusee\n"); return; }
        print_string("tex.list : scripts .tex disponibles\n");
        // Parcourir le FS et afficher les .tex
        extern FSTable g_fs;
        extern uint32_t g_cwd;
        int found = 0;
        FSNode* dir = &g_fs.nodes[g_cwd];
        for (uint32_t i = 0; i < dir->child_count; i++) {
            uint32_t ci = dir->children[i];
            if (ci >= g_fs.node_count) continue;
            FSNode* child = &g_fs.nodes[ci];
            if (child->is_dir) continue;
            // Chercher extension .tex
            const char* n = child->name;
            int nlen = 0;
            while (n[nlen] != '\0') nlen++;
            if (nlen > 4 && n[nlen-4] == '.' && n[nlen-3] == 't' && n[nlen-2] == 'e' && n[nlen-1] == 'x') {
                print_string("  ");
                print_string(child->name);
                print_string("\n");
                found++;
            }
        }
        if (!found) print_string("  (aucun script .tex dans ce dossier)\n");
    }
    else {
        print_string("tex : commande inconnue '");
        print_string(cmd);
        print_string("'\n");
        print_string("  tex.run <fichier> / tex.list\n");
    }
}

// ============================================================================
// SHELL PRINCIPAL
// ============================================================================

void tetra_shell(void) {
    char input[256];
    
    uint8_t readme_buffer[2048];
    int readme_result = fs_read_file("README.txt", readme_buffer, sizeof(readme_buffer) - 1);
    if (readme_result > 0) {
        readme_buffer[readme_result] = '\0';
        print_string((char*)readme_buffer);
        print_string("\n");
    }
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
    print_string("       TetraOS Shell v1.5 (Fr)          \n");
    print_string("========================================\n");
    print_string("Tapez 'sys.help' pour la liste des commandes\n\n");

    while (g_session_manager.logged_in) {
        // Prompt : user@TetraOS:/path/courant# ou $
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

        print_string(session_is_admin() ? "# " : "$ ");

        // Lecture de la commande
        int i = 0;
        while (1) {
            char c = keyboard_get_char();
            if (c == '\r' || c == '\n') {
                input[i] = '\0';
                print_char('\n');
                break;
            } else if ((c == '\b' || c == 127) && i > 0) {
                i--;
                print_string("\b \b");
            } else if (c == 16) {
                // Flèche haut → scroll vers le haut dans l'historique
                screen_scroll_up();
            } else if (c == 14) {
                // Flèche bas → scroll vers le bas
                screen_scroll_down();
            } else if (c >= 32 && c <= 126 && i < 255) {
                // Dès qu'on tape, revenir en vue bottom si on scrollait
                if (!screen_at_bottom()) screen_goto_bottom();
                input[i++] = c;
                print_char(c);
            } else if (c == 27) {
                input[0] = '\0';
                print_string("^C\n");
                break;
            }
        }

        const char* s = input;
        while (*s == ' ') s++;
        if (strlen(s) == 0) continue;

        // ====== EASTER EGG ======
        if (strcmp(s, "sl") == 0) {
            cmd_sl();
            continue;
        }

        // ====== DISPATCH NAMESPACED ======
        char mod[32], cmd[64], args[192];
        if (parse_namespaced(s, mod, sizeof(mod), cmd, sizeof(cmd), args, sizeof(args))) {

            if (strcmp(mod, "ray64") == 0) {
                dispatch_ray64(cmd, args);
                // Si logout demandé depuis session.*, il faut quitter la boucle
            }
            else if (strcmp(mod, "session") == 0) {
                dispatch_session(cmd, args);
                // session.logout met logged_in à 0, la boucle while s'arrête
            }
            else if (strcmp(mod, "sys") == 0) {
                dispatch_sys(cmd, args);
            }
            else if (strcmp(mod, "tex") == 0) {
                dispatch_tex(cmd, args);
            }
            else {
                print_string("module inconnu '");
                print_string(mod);
                print_string("' - modules disponibles : ray64, session, sys, tex\n");
            }
        }
        else {
            // Commande sans namespace
            print_string("commande inconnue : '");
            print_string(s);
            print_string("'\n");
            print_string("Format : <module>.<commande> [args]\n");
            print_string("Tapez 'sys.help' pour la liste complete\n");
        }
    }
}