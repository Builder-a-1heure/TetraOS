// main.c

#include "screen.h"
#include "input.h"
#include "reapfs.h"
#include "utils.h"
#include <stdint.h>
#include "io.h"
#include "ata.h"
#include "ui.h"

struct reapfs_global {
    reapfs_super_t super;
    reapfs_inode_t nodes[REAPFS_MAX_INODES];
};

static struct reapfs_global g_fs;
static int g_cwd_ino = 0;  // Inode du répertoire courant

/* --- CWD and helper functions adapted to modern FS API --- */
static char g_cwd_path[256] = "/";

/* build absolute path from cwd and name */
static void build_path(const char *name, char *out, size_t out_sz) {
    if (!name || name[0] == '\0') { strncpy(out, g_cwd_path, out_sz-1); out[out_sz-1]='\0'; return; }
    if (name[0] == '/') { strncpy(out, name, out_sz-1); out[out_sz-1] = '\0'; return; }
    if (strcmp(g_cwd_path, "/") == 0) snprintf(out, out_sz, "/%s", name);
    else snprintf(out, out_sz, "%s/%s", g_cwd_path, name);
}

/* change directory implementation: returns 0 on success */
static int fs_cd_impl(const char *path) {
    if (!path) return -1;
    char candidate[512];
    build_path(path, candidate, sizeof(candidate));
    int ino = find_inode_by_path(candidate);
    if (ino >= 0 && g_fs.nodes[ino].is_dir) {
        g_cwd_ino = ino;
        strncpy(g_cwd_path, candidate, sizeof(g_cwd_path)-1);
        g_cwd_path[sizeof(g_cwd_path)-1]='\0';
        return 0;
    }
    return -1;
}


/* find in cwd using fs_ls output; returns 1 if found, 0 otherwise */
static int fs_find_impl(const char *name) {
    if (!name) return 0;
    char buf[4096];
    if (fs_ls(g_cwd_path, buf, sizeof(buf)) != FS_OK) return 0;
    char *p = buf;
    while (*p) {
        char entry[256]; int i=0;
        while (*p && *p != '\n' && i < (int)sizeof(entry)-1) entry[i++] = *p++;
        entry[i]='\0';
        if (*p == '\n') p++;
        char *tab = strchr(entry, '\t');
        if (tab) *tab = '\0';
        if (strcmp(entry, name) == 0) return 1;
    }
    return 0;
}

/* read/write helpers for shell */
static int fs_write_file_impl(const char* name, const uint8_t* data, uint32_t size) {
    if (!name) return -1;
    char path[512]; build_path(name, path, sizeof(path));
    fs_create(path); /* create if not exists */
    reapfs_fd_t fd = fs_open(path, 1);
    if (fd < 0) return -1;
    int w = fs_write(fd, data, size);
    fs_close(fd);
    return (w >= 0) ? 0 : -1;
}

static int fs_read_file_impl(const char* name, uint8_t* out, uint32_t max_len) {
    if (!name || !out) return -1;
    char path[512]; build_path(name, path, sizeof(path));
    reapfs_fd_t fd = fs_open(path, 0);
    if (fd < 0) return -1;
    int r = fs_read(fd, out, max_len);
    fs_close(fd);
    return r;
}

static int fs_delete_impl(const char* name) {
    if (!name) return -1;
    char path[512]; build_path(name, path, sizeof(path));
    return (fs_remove(path) == FS_OK) ? 0 : -1;
}

static void fs_list_impl(void) {
    char buf[4096];
    fs_ls(g_cwd_path, buf, sizeof(buf));
    printf("%s", buf);
}

static int fs_mkdir_wrapper(const char* name) {
    if (!name) return -1;
    char path[512]; build_path(name, path, sizeof(path));
    return (fs_mkdir(path) == FS_OK) ? 0 : -1;
}



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
    clear_screen();
    print_string("=== DEBUT BOOT ===\n");
    
    print_string("[1/5] Init ecran...\n");
    // (déjà fait par clear_screen)
    
    print_string("[2/5] Init ATA...\n");
    ata_init();
    print_string("[2/5] ATA OK\n");
    
    print_string("[3/5] Init FS...\n");
    if (fs_init() != FS_OK) {
        print_string("ERREUR: fs_init() a echoue!\n");
        while(1) { asm("hlt"); }
    }
    print_string("[3/5] FS OK\n");
    
    print_string("[4/5] Lancement shell...\n");
    tetra_shell();
    
    print_string("[5/5] Fin shell (anormal)\n");
    while(1) { asm("hlt"); }
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
print_string("Fonction non disponible en 1.0, desole ...");
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
    int bytes_read = fs_read_file_impl(filename, existing_data, sizeof(existing_data) - 1);
    if (bytes_read > 0) {
        existing_data[bytes_read] = '\0';
        strncpy(content, (char*)existing_data, sizeof(content) - 1);
        cursor_pos = strlen(content);
    }
    
    while (1) {
        draw_editor_window(filename, content, cursor_pos);
        
        char c = keyboard_get_char();
        
        if (c == 27) { // ESC - Sauvegarder et quitter
            fs_write_file_impl(filename, (uint8_t*)content, strlen(content));
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
        fs_draw_ls();

        print_string("root@TetraOS:");
        char parts[16][32];
        int depth = 0;
        uint32_t cur = g_cwd_ino;  // Commence à l'inode courant

        while (cur != 0 && depth < 16) {
            memset(parts[depth], 0, 32);
            strncpy(parts[depth], g_fs.nodes[cur].name, 31);
            cur = g_fs.nodes[cur].parent;  // Utilise le champ parent
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
            print_string("  encrypt <file> <pass>  - Encrypt a file with password\n");
            print_string("  decrypt <file> <pass>  - Decrypt a file with password\n");
            print_string("  clone <src> <dst>      - Clone a file instantly (COW)\n");
            print_string("  snapshot <name>        - Create filesystem snapshot\n");
            print_string("  snaplist               - List all snapshots\n");
            print_string("  restore <name>         - Restore a snapshot\n");
            print_string("  snapdel <name>         - Delete a snapshot\n");
            print_string("  stat <file>            - Show file metadata\n");
            print_string("  chmod <file> <mode>    - Change file permissions\n");
            print_string("  compress <file>        - Compress a file\n");
            print_string("  checksum <file>        - Verify file integrity\n");
            print_string("  volencrypt <pass>      - Enable volume encryption\n");

        }
        else if (strcmp(s, "formate") == 0){
            fs_init();
        }

        else if (strcmp(s, "exit") == 0) {
            outw(0x604, 0x2000);
        }
        else if (strcmp(s, "clear") == 0) {
            clear_screen();
        }
        else if (strcmp(s, "ls") == 0) {
            do { char __fsbuf[4096]; fs_ls(g_cwd_path, __fsbuf, sizeof(__fsbuf)); printf("%s", __fsbuf); } while(0);
        }
        else if (strcmp(s, "fs") == 0) {
            fs_debug_print();
        }
        else if (strcmp(s, "pwd") == 0) {
            print_string("Vous etes ici : ");
            printf(g_cwd_path);
            print_string("\n");
        }
        else if (strncmp(s, "cd ", 3) == 0) {
            char *path = (char*)(s + 3);
            while (*path == ' ') path++;
            if (fs_cd_impl(path) != 0) {
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
            if (fs_create(name) != 0) {
                print_string("add: failed\n");
            }
        }
        else if (strncmp(s, "open ", 5) == 0) {
            char *name = (char*)(s + 6);
            while (*name == ' ') name++;
            
            int idx = fs_find_impl(name);
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
            int result = fs_read_file_impl(name, buffer, sizeof(buffer) - 1);
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
        
                
        // --- ENCRYPT ---
        else if (strncmp(s, "encrypt ", 8) == 0) {
            char *args = (char*)(s + 8);
            while (*args == ' ') args++;
            
            // Parser: filename et password séparés par espace
            char filename[128] = {0};
            char password[128] = {0};
            int i = 0;
            
            // Lire filename
            while (*args && *args != ' ' && i < 127) {
                filename[i++] = *args++;
            }
            filename[i] = '\0';
            
            // Skip espaces
            while (*args == ' ') args++;
            
            // Lire password
            i = 0;
            while (*args && i < 127) {
                password[i++] = *args++;
            }
            password[i] = '\0';
            
            if (strlen(filename) == 0 || strlen(password) == 0) {
                print_string("Usage: encrypt <filename> <password>\n");
            } else {
                char path[512];
                build_path(filename, path, sizeof(path));
                if (fs_encrypt_file(path, password) == 0) {
                    print_string("File encrypted successfully\n");
                } else {
                    print_string("Encryption failed\n");
                }
            }
        }

        // --- DECRYPT ---
        else if (strncmp(s, "decrypt ", 8) == 0) {
            char *args = (char*)(s + 8);
            while (*args == ' ') args++;
            
            char filename[128] = {0};
            char password[128] = {0};
            int i = 0;
            
            while (*args && *args != ' ' && i < 127) {
                filename[i++] = *args++;
            }
            filename[i] = '\0';
            
            while (*args == ' ') args++;
            
            i = 0;
            while (*args && i < 127) {
                password[i++] = *args++;
            }
            password[i] = '\0';
            
            if (strlen(filename) == 0 || strlen(password) == 0) {
                print_string("Usage: decrypt <filename> <password>\n");
            } else {
                char path[512];
                build_path(filename, path, sizeof(path));
                if (fs_decrypt_file(path, password) == 0) {
                    print_string("File decrypted successfully\n");
                } else {
                    print_string("Decryption failed (wrong password?)\n");
                }
            }
        }

        // --- CLONE ---
        else if (strncmp(s, "clone ", 6) == 0) {
            char *args = (char*)(s + 6);
            while (*args == ' ') args++;
            
            char src[128] = {0};
            char dst[128] = {0};
            int i = 0;
            
            while (*args && *args != ' ' && i < 127) {
                src[i++] = *args++;
            }
            src[i] = '\0';
            
            while (*args == ' ') args++;
            
            i = 0;
            while (*args && i < 127) {
                dst[i++] = *args++;
            }
            dst[i] = '\0';
            
            if (strlen(src) == 0 || strlen(dst) == 0) {
                print_string("Usage: clone <source> <destination>\n");
            } else {
                char src_path[512], dst_path[512];
                build_path(src, src_path, sizeof(src_path));
                build_path(dst, dst_path, sizeof(dst_path));
                
                if (fs_clone_file(src_path, dst_path) == 0) {
                    print_string("File cloned successfully (COW)\n");
                } else {
                    print_string("Clone failed\n");
                }
            }
        }

        // --- SNAPSHOT ---
        else if (strncmp(s, "snapshot ", 9) == 0) {
            char *name = (char*)(s + 9);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: snapshot <name>\n");
            } else {
                if (fs_snapshot_create(name) == 0) {
                    print_string("Snapshot '");
                    print_string(name);
                    print_string("' created\n");
                } else {
                    print_string("Snapshot creation failed\n");
                }
            }
        }

        // --- SNAPLIST ---
        else if (strcmp(s, "snaplist") == 0) {
            char snapshots[2048];
            if (fs_snapshot_list(snapshots, sizeof(snapshots)) == 0) {
                print_string("Available snapshots:\n");
                print_string(snapshots);
            } else {
                print_string("No snapshots found\n");
            }
        }

        // --- RESTORE ---
        else if (strncmp(s, "restore ", 8) == 0) {
            char *name = (char*)(s + 8);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: restore <snapshot_name>\n");
            } else {
                print_string("WARNING: This will restore the filesystem to snapshot '");
                print_string(name);
                print_string("'. Continue? (y/n): ");
                
                char confirm = keyboard_get_char();
                print_char(confirm);
                print_char('\n');
                
                if (confirm == 'y' || confirm == 'Y') {
                    if (fs_snapshot_restore(name) == 0) {
                        print_string("Snapshot restored successfully\n");
                        print_string("Reloading filesystem...\n");
                        fs_init(); // Reload FS state
                    } else {
                        print_string("Restore failed\n");
                    }
                } else {
                    print_string("Restore cancelled\n");
                }
            }
        }

        // --- SNAPDEL ---
        else if (strncmp(s, "snapdel ", 8) == 0) {
            char *name = (char*)(s + 8);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: snapdel <snapshot_name>\n");
            } else {
                if (fs_snapshot_delete(name) == 0) {
                    print_string("Snapshot deleted\n");
                } else {
                    print_string("Delete failed\n");
                }
            }
        }

        // --- STAT ---
        else if (strncmp(s, "stat ", 5) == 0) {
            char *name = (char*)(s + 5);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: stat <filename>\n");
            } else {
                char path[512];
                build_path(name, path, sizeof(path));
                
                fs_stat_t stat_info;
                if (fs_stat(path, &stat_info) == 0) {
                    print_string("File: ");
                    print_string(name);
                    print_string("\n");
                    
                    print_string("  Size: ");
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "%u", stat_info.size);
                    print_string(tmp);
                    print_string(" bytes\n");
                    
                    print_string("  Type: ");
                    print_string(stat_info.is_dir ? "Directory\n" : "File\n");
                    
                    print_string("  Permissions: ");
                    snprintf(tmp, sizeof(tmp), "0%o", stat_info.mode);
                    print_string(tmp);
                    print_string("\n");
                    
                    print_string("  Encrypted: ");
                    print_string(stat_info.encrypted ? "Yes\n" : "No\n");
                    
                    print_string("  Compressed: ");
                    print_string(stat_info.compressed ? "Yes\n" : "No\n");
                    
                    print_string("  Created: ");
                    snprintf(tmp, sizeof(tmp), "%u", stat_info.created_time);
                    print_string(tmp);
                    print_string("\n");
                    
                    print_string("  Modified: ");
                    snprintf(tmp, sizeof(tmp), "%u", stat_info.modified_time);
                    print_string(tmp);
                    print_string("\n");
                    
                    print_string("  Checksum: 0x");
                    snprintf(tmp, sizeof(tmp), "%08x", stat_info.checksum);
                    print_string(tmp);
                    print_string("\n");
                } else {
                    print_string("stat: file not found\n");
                }
            }
        }

        // --- CHMOD ---
        else if (strncmp(s, "chmod ", 6) == 0) {
            char *args = (char*)(s + 6);
            while (*args == ' ') args++;
            
            char filename[128] = {0};
            char mode_str[16] = {0};
            int i = 0;
            
            while (*args && *args != ' ' && i < 127) {
                filename[i++] = *args++;
            }
            filename[i] = '\0';
            
            while (*args == ' ') args++;
            
            i = 0;
            while (*args && i < 15) {
                mode_str[i++] = *args++;
            }
            mode_str[i] = '\0';
            
            if (strlen(filename) == 0 || strlen(mode_str) == 0) {
                print_string("Usage: chmod <filename> <mode>\n");
                print_string("Example: chmod file.txt 644\n");
            } else {
                // Parse octal mode
                uint32_t mode = 0;
                for (i = 0; mode_str[i]; i++) {
                    if (mode_str[i] >= '0' && mode_str[i] <= '7') {
                        mode = mode * 8 + (mode_str[i] - '0');
                    }
                }
                
                char path[512];
                build_path(filename, path, sizeof(path));
                
                if (fs_chmod(path, mode) == 0) {
                    print_string("Permissions changed\n");
                } else {
                    print_string("chmod failed\n");
                }
            }
        }

        // --- COMPRESS ---
        else if (strncmp(s, "compress ", 9) == 0) {
            char *name = (char*)(s + 9);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: compress <filename>\n");
            } else {
                char path[512];
                build_path(name, path, sizeof(path));
                
                if (fs_compress_file(path) == 0) {
                    print_string("File compressed successfully\n");
                } else {
                    print_string("Compression failed\n");
                }
            }
        }

        // --- CHECKSUM ---
        else if (strncmp(s, "checksum ", 9) == 0) {
            char *name = (char*)(s + 9);
            while (*name == ' ') name++;
            
            if (strlen(name) == 0) {
                print_string("Usage: checksum <filename>\n");
            } else {
                char path[512];
                build_path(name, path, sizeof(path));
                
                uint32_t checksum;
                if (fs_verify_checksum(path, &checksum) == 0) {
                    print_string("File integrity: OK\n");
                    print_string("Checksum: 0x");
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "%08x", checksum);
                    print_string(tmp);
                    print_string("\n");
                } else {
                    print_string("Checksum verification FAILED - file may be corrupted!\n");
                }
            }
        }

        // --- VOLENCRYPT ---
        else if (strncmp(s, "volencrypt ", 11) == 0) {
            char *password = (char*)(s + 11);
            while (*password == ' ') password++;
            
            if (strlen(password) == 0) {
                print_string("Usage: volencrypt <password>\n");
            } else {
                print_string("WARNING: This will encrypt the entire volume!\n");
                print_string("All new files will be automatically encrypted.\n");
                print_string("Continue? (y/n): ");
                
                char confirm = keyboard_get_char();
                print_char(confirm);
                print_char('\n');
                
                if (confirm == 'y' || confirm == 'Y') {
                    if (fs_set_volume_encryption(password) == 0) {
                        print_string("Volume encryption enabled\n");
                    } else {
                        print_string("Failed to enable volume encryption\n");
                    }
                } else {
                    print_string("Operation cancelled\n");
                }
            }
        }
        else {
            print_string("Command not found: ");
            print_string(s);
            print_string("\nType 'help' for available commands\n");
        }
    }
}