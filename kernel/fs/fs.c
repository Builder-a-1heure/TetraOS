// fs.c - RAY64 hierarchical filesystem implementation
#include "../fs/fs.h"
#include "../lib/utils.h"
#include "../gfx/screen.h"
#include "../drivers/ata.h"
#include "../ui/session.h"
#include <stddef.h>
#include <stdint.h>
#include "tex_doc_content.h"

// Global filesystem state
FSTable g_fs;
uint32_t g_cwd = 0;

static uint8_t fs_temp_buffer[FS_TABLE_SECTORS * 512];

#define MAX_READ_ATTEMPTS 3
#define SECTORS_PER_BATCH 256
#define MOVE_CHUNK_SECTORS 128

// Forward declarations
static int ray64_create_node(const char* name, uint8_t is_dir);
int fs_write_file(const char* name, const uint8_t* data, uint32_t size);
// Helpers statiques utilisés par le moteur ACL (définis plus bas)
static void ray64_get_meta(FSNode* node, RAY64NodeMeta* meta);
static void ray64_set_meta(FSNode* node, const RAY64NodeMeta* meta);
static int  ray64_flush_node(uint32_t node_idx);

// ============================================================================
// MOTEUR ACL RAY64
// ============================================================================

// Vérifie si la session courante peut effectuer op sur node_idx
// Logique :
//   1. Admin bypass tout (sauf si acl_lock ET op != admin bits)
//   2. Owner → vérifier bits owner
//   3. Autres → vérifier bits other (bloqué par défaut sur les homes)
int fs_acl_check(uint32_t node_idx, AclOp op) {
    if (node_idx >= g_fs.node_count) return 0;
    FSNode* node = &g_fs.nodes[node_idx];
    if (node->magic != FS_MAGIC) return 0;

    RAY64NodeMeta meta;
    ray64_get_meta(node, &meta);
    uint16_t perms = meta.permissions;
    uint16_t uid   = meta.uid;

    // Session courante
    int is_admin  = session_is_admin();
    uint16_t cur_uid = session_get_uid();

    // Si aucune session n'existe encore (premier boot), tout est permis
    if (g_session_manager.session_count == 0) return 1;
    // Si nœud système pas encore tagué (uid SYSTEM), admin ou owner = tous
    if (uid == UID_SYSTEM || uid == UID_NOOWNER) {
        return is_admin ? 1 : 0; // Seul admin accède aux nœuds système
    }

    // Bit masks selon l'opération
    uint16_t owner_bit = (op == ACL_READ)  ? ACL_OWNER_R :
                         (op == ACL_WRITE) ? ACL_OWNER_W : ACL_OWNER_X;
    uint16_t admin_bit = (op == ACL_READ)  ? ACL_ADMIN_R :
                         (op == ACL_WRITE) ? ACL_ADMIN_W : ACL_ADMIN_X;
    uint16_t other_bit = (op == ACL_READ)  ? ACL_OTHER_R :
                         (op == ACL_WRITE) ? ACL_OTHER_W : ACL_OTHER_X;

    // Admin → vérifie les bits admin
    if (is_admin) {
        return (perms & admin_bit) ? 1 : 0;
    }

    // Propriétaire → vérifie bits owner
    if (cur_uid == uid) {
        return (perms & owner_bit) ? 1 : 0;
    }

    // Autre → vérifie bits other
    return (perms & other_bit) ? 1 : 0;
}

// Définit les permissions et l'uid d'un nœud (usage interne)
void fs_acl_set_node(uint32_t node_idx, uint16_t perms, uint16_t uid) {
    if (node_idx >= g_fs.node_count) return;
    FSNode* node = &g_fs.nodes[node_idx];
    RAY64NodeMeta meta;
    ray64_get_meta(node, &meta);
    meta.permissions = perms;
    meta.uid = uid;
    ray64_set_meta(node, &meta);
    ray64_flush_node(node_idx);
}

// Affiche les permissions d'un nœud de façon lisible
void fs_acl_print(uint32_t node_idx) {
    if (node_idx >= g_fs.node_count) return;
    FSNode* node = &g_fs.nodes[node_idx];
    RAY64NodeMeta meta;
    ray64_get_meta(node, &meta);
    uint16_t p = meta.permissions;

    // Affiche rwxrwxrwx
    print_char((p & ACL_OWNER_R) ? 'r' : '-');
    print_char((p & ACL_OWNER_W) ? 'w' : '-');
    print_char((p & ACL_OWNER_X) ? 'x' : '-');
    print_char((p & ACL_ADMIN_R) ? 'r' : '-');
    print_char((p & ACL_ADMIN_W) ? 'w' : '-');
    print_char((p & ACL_ADMIN_X) ? 'x' : '-');
    print_char((p & ACL_OTHER_R) ? 'r' : '-');
    print_char((p & ACL_OTHER_W) ? 'w' : '-');
    print_char((p & ACL_OTHER_X) ? 'x' : '-');

    // Propriétaire
    print_string("  owner:");
    const char* owner_name = session_get_name_by_uid(meta.uid);
    print_string(owner_name ? owner_name : "system");

    if (meta.acl_lock) print_string("  [LOCK]");
    if (node->is_dir)  print_string("  [DIR]");
}

// chmod : change les permissions d'un nœud dans le cwd courant
int fs_chmod(const char* name, uint16_t new_perms) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("ray64.chmod : introuvable : ");
        print_string(name);
        print_string("\n");
        return -1;
    }

    FSNode* node = &g_fs.nodes[idx];
    RAY64NodeMeta meta;
    ray64_get_meta(node, &meta);

    int is_admin  = session_is_admin();
    uint16_t cur_uid = session_get_uid();

    // Seul le propriétaire OU un admin peut chmod
    // Si acl_lock → seul admin
    if (meta.acl_lock && !is_admin) {
        print_string("ray64.chmod : permissions verrouillee (admin requis)\n");
        return -1;
    }
    if (!is_admin && cur_uid != meta.uid) {
        print_string("ray64.chmod : acces refuse (non proprietaire)\n");
        return -1;
    }

    meta.permissions = new_perms & 0x1FF; // 9 bits max
    ray64_set_meta(node, &meta);
    ray64_flush_node((uint32_t)idx);
    return 0;
}

// chown : change le propriétaire d'un nœud (admin seulement)
int fs_chown(const char* name, uint16_t new_uid) {
    if (!session_is_admin()) {
        print_string("ray64.chown : admin requis\n");
        return -1;
    }

    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("ray64.chown : introuvable : ");
        print_string(name);
        print_string("\n");
        return -1;
    }

    FSNode* node = &g_fs.nodes[idx];
    RAY64NodeMeta meta;
    ray64_get_meta(node, &meta);
    meta.uid = new_uid;
    ray64_set_meta(node, &meta);
    ray64_flush_node((uint32_t)idx);
    return 0;
}

// ============================================================================

// Utility: get current timestamp (placeholder - would use RTC in real system)
static uint64_t ray64_get_timestamp(void) {
    static uint64_t counter = 0;
    return counter++;
}

// Utility: get/set extended metadata from FSNode.reserved
static void ray64_get_meta(FSNode* node, RAY64NodeMeta* meta) {
    if (sizeof(RAY64NodeMeta) <= RAY64_META_SIZE) {
        memcpy(meta, node->reserved, sizeof(RAY64NodeMeta));
    } else {
        memset(meta, 0, sizeof(RAY64NodeMeta));
    }
}

static void ray64_set_meta(FSNode* node, const RAY64NodeMeta* meta) {
    if (sizeof(RAY64NodeMeta) <= RAY64_META_SIZE) {
        memcpy(node->reserved, meta, sizeof(RAY64NodeMeta));
    }
}

// Read/Write helpers with retry logic
static int read_with_retry(uint32_t lba, uint8_t* buf, uint32_t count) {
    const uint32_t CHUNK = SECTORS_PER_BATCH;
    for (int attempt = 0; attempt < MAX_READ_ATTEMPTS; attempt++) {
        int success = 1;
        for (uint32_t i = 0; i < count; i += CHUNK) {
            uint32_t blocks = (count - i) > CHUNK ? CHUNK : (count - i);
            if (ata_read(lba + i, buf + i * 512, blocks) != 0) {
                success = 0;
                break;
            }
        }
        if (success) return 0;
    }
    return -1;
}

static int write_sectors(uint32_t lba, const uint8_t* buf, uint32_t count) {
    const uint32_t CHUNK = SECTORS_PER_BATCH;
    for (uint32_t i = 0; i < count; i += CHUNK) {
        uint32_t blocks = (count - i) > CHUNK ? CHUNK : (count - i);
        if (ata_write(lba + i, buf + i * 512, blocks) != 0) {
            return -1;
        }
    }
    return 0;
}

// Read filesystem header
static int ray64_read_header(void) {
    uint8_t sec[512];
    if (ata_read(FS_TABLE_LBA, sec, 1) != 0) return -1;
    
    uint32_t magic = *(uint32_t*)sec;
    uint32_t node_count = *(uint32_t*)(sec + 4);
    uint32_t version = *(uint32_t*)(sec + 8);
    
    if (magic != FS_MAGIC || version != RAY64_VERSION) return -1;
    if (node_count == 0 || node_count > FS_MAX_NODES) return -1;
    
    return 0;
}

// Flush filesystem metadata to disk
int fs_flush(void) {
    print_string("RAY64: Flushing filesystem metadata\n");
    
    uint32_t needed_sectors = (sizeof(FSTable) + 511) / 512;
    if (needed_sectors > FS_TABLE_SECTORS) {
        print_string("RAY64: Table too large\n");
        return -1;
    }
    
    memset(fs_temp_buffer, 0, needed_sectors * 512);
    memcpy(fs_temp_buffer, &g_fs, sizeof(FSTable));
    
    if (write_sectors(FS_TABLE_LBA, fs_temp_buffer, needed_sectors) != 0) {
        print_string("RAY64: Flush failed\n");
        return -1;
    }
    
    print_string("RAY64: Flush successful\n");
    return 0;
}

// Flush single node metadata
static int ray64_flush_node(uint32_t node_idx) {
    if (node_idx >= g_fs.node_count) return -1;
    
    uint8_t* base = (uint8_t*)&g_fs;
    uint8_t* nodeptr = (uint8_t*)&g_fs.nodes[node_idx];
    ptrdiff_t offset = nodeptr - base;
    if (offset < 0) return -1;
    
    size_t node_size = sizeof(FSNode);
    uint32_t start_sector = (uint32_t)(offset / 512);
    uint32_t end_sector = (uint32_t)((offset + node_size + 511) / 512);
    uint32_t sectors = end_sector - start_sector;
    
    if (sectors == 0) return 0;
    if (start_sector + sectors > FS_TABLE_SECTORS) return -1;
    
    uint32_t bytes = sectors * 512;
    memset(fs_temp_buffer, 0, bytes);
    memcpy(fs_temp_buffer, base + start_sector * 512, bytes);
    
    return write_sectors(FS_TABLE_LBA + start_sector, fs_temp_buffer, sectors);
}

// Format RAY64 filesystem
void fs_format(void) {
    print_string("RAY64: Formatting filesystem\n");
    
    // Initialize entire FSTable structure
    memset(&g_fs, 0, sizeof(FSTable));
    g_fs.magic = FS_MAGIC;
    g_fs.node_count = 1;
    g_fs.version = RAY64_VERSION;
    strncpy((char*)g_fs.volume_name, "RAY64 Volume", 63);
    
    // Create root directory
    FSNode* root = &g_fs.nodes[0];
    memset(root, 0, sizeof(FSNode));
    strncpy(root->name, "/", FS_NAME_LEN - 1);
    root->is_dir = 1;
    root->parent = 0;
    root->magic = FS_MAGIC;
    root->data_start_lba = FS_DATA_BASE_LBA;
    
    // Initialize root metadata
    RAY64NodeMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.create_time = ray64_get_timestamp();
    meta.modify_time = meta.create_time;
    meta.access_time = meta.create_time;
    meta.permissions = 0755;
    meta.link_count = 2; // . and ..
    ray64_set_meta(root, &meta);
    
    // Write to disk
    print_string("RAY64: Writing filesystem table...\n");
    uint32_t total = FS_TABLE_SECTORS;
    uint32_t written = 0;
    uint32_t batch = SECTORS_PER_BATCH;
    
    while (written < total) {
        uint32_t to_write = (total - written) > batch ? batch : (total - written);
        memset(fs_temp_buffer, 0, to_write * 512);
        
        if (written == 0) {
            // Write FSTable header on first batch
            uint32_t c = sizeof(FSTable);
            if (c > to_write * 512) c = to_write * 512;
            memcpy(fs_temp_buffer, &g_fs, c);
        }
        
        if (ata_write(FS_TABLE_LBA + written, fs_temp_buffer, to_write) != 0) {
            print_string("RAY64: Write failed at sector ");
            print_dec(written);
            print_string("\n");
            return;
        }
        
        written += to_write;
        if ((written % (batch * 4)) == 0 || written == total) {
            print_string("RAY64: Progress ");
            print_dec(written);
            print_string("/");
            print_dec(total);
            print_string("\n");
        }
    }
    
    print_string("RAY64: Format completed\n");
    
    // Créer le fichier tex.txt avec la documentation TEX
    print_string("RAY64: Creating tex.txt documentation...\n");
    
    // Créer le fichier tex.txt dans le répertoire racine
    uint32_t saved_cwd = g_cwd;
    g_cwd = 0; // Se placer à la racine
    
    int tex_idx = ray64_create_node("tex.txt", 0);
    if (tex_idx >= 0) {
        // Écrire le contenu de la documentation
        uint32_t doc_len = strlen(TEX_DOCUMENTATION);
        fs_write_file("tex.txt", (const uint8_t*)TEX_DOCUMENTATION, doc_len);
        print_string("RAY64: tex.txt created successfully\n");
    } else {
        print_string("RAY64: Warning - failed to create tex.txt\n");
    }
    
    // Créer un fichier README.txt de bienvenue
    print_string("RAY64: Creating README.txt...\n");
    const char readme_content[] = 
        "=======================================\n"
        "   Bienvenue sur TetraOS v1.3 !       \n"
        "=======================================\n"
        "\n"
        "Ceci est votre premier demarrage.\n"
        "\n"
        "Pour commencer :\n"
        "  - Tapez 'help' pour voir les commandes\n"
        "  - Tapez 'edit tex.txt' pour lire la doc TEX\n"
        "  - Tapez 'tex exemple.tex' pour tester TEX\n"
        "\n"
        "Le systeme TEX permet d'executer des\n"
        "scripts personnalises directement dans\n"
        "TetraOS!\n"

        "=======================================\n";
    
    int readme_idx = ray64_create_node("README.txt", 0);
    if (readme_idx >= 0) {
        uint32_t readme_len = strlen(readme_content);
        fs_write_file("README.txt", (const uint8_t*)readme_content, readme_len);
        print_string("RAY64: README.txt created successfully\n");
    }
    
    // Créer un script d'exemple TEX
    print_string("RAY64: Creating exemple.tex...\n");
    const char exemple_content[] = 
        "// Script exemple pour TEX\n"
        "// Compteur simple\n"
        "\n"
        "clear\n"
        "io.println(\"=== Exemple TEX ===\")\n"
        "io.println(\"Programme compteur\")\n"
        "io.println(\"\")\n"
        "io.input(\"Entrez max: \", max)\n"
        "\n"
        "var i = 0\n"
        "if i < max {\n"
        "  io.print(\"Compteur: \")\n"
        "  io.println(i)\n"
        "  i = i + 1\n"
        "}\n"
        "\n"
        "io.println(\"\")\n"
        "io.println(\"Termine!\")\n"
        "io.println(\"Editez avec: edit exemple.tex\")\n"
        "exit\n";
    
    int exemple_idx = ray64_create_node("exemple.tex", 0);
    if (exemple_idx >= 0) {
        uint32_t exemple_len = strlen(exemple_content);
        fs_write_file("exemple.tex", (const uint8_t*)exemple_content, exemple_len);
        print_string("RAY64: exemple.tex created successfully\n");
    }
    
    g_cwd = saved_cwd; // Restaurer le répertoire courant
}

// Initialize RAY64 filesystem
void fs_init(void) {
    print_string("RAY64: Initializing filesystem\n");
    
    if (ray64_read_header() != 0) {
        print_string("RAY64: Invalid or missing filesystem, formatting\n");
        fs_format();
        return;
    }
    
    uint32_t needed_sectors = (sizeof(FSTable) + 511) / 512;
    if (needed_sectors > FS_TABLE_SECTORS) {
        print_string("RAY64: Table too large, formatting\n");
        fs_format();
        return;
    }
    
    print_string("RAY64: Loading table (");
    print_dec(needed_sectors);
    print_string(" sectors)\n");
    
    if (read_with_retry(FS_TABLE_LBA, fs_temp_buffer, needed_sectors) != 0) {
        print_string("RAY64: Read failed, formatting\n");
        fs_format();
        return;
    }
    
    uint32_t copy_len = sizeof(FSTable);
    if (copy_len > needed_sectors * 512) copy_len = needed_sectors * 512;
    memcpy(&g_fs, fs_temp_buffer, copy_len);
    
    if (g_fs.magic != FS_MAGIC || g_fs.version != RAY64_VERSION) {
        print_string("RAY64: Magic/version mismatch, formatting\n");
        fs_format();
    } else {
        print_string("RAY64: Loaded successfully (");
        print_dec(g_fs.node_count);
        print_string(" nodes)\n");
        print_string("RAY64: Volume: ");
        print_string((char*)g_fs.volume_name);
        print_string("\n");
    }
}

// Find entry in directory (case-insensitive)
int fs_find_in_dir(uint32_t dir_idx, const char* name) {
    if (dir_idx >= g_fs.node_count) return -1;
    
    FSNode* dir = &g_fs.nodes[dir_idx];
    if (!dir->is_dir) return -1;
    
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t child_idx = dir->children[i];
        if (child_idx >= g_fs.node_count) continue;
        
        FSNode* child = &g_fs.nodes[child_idx];
        child->name[FS_NAME_LEN - 1] = '\0';
        
        if (strcasecmp(child->name, name) == 0) {
            return (int)child_idx;
        }
    }
    return -1;
}

int fs_find(const char* name) {
    return fs_find_in_dir(g_cwd, name);
}

// Next free LBA for data allocation
uint32_t fs_next_free_lba(void) {
    uint32_t max_end = FS_DATA_BASE_LBA;
    
    for (uint32_t i = 0; i < g_fs.node_count; i++) {
        FSNode* node = &g_fs.nodes[i];
        if (node->magic == FS_MAGIC && !node->is_dir && node->data_start_lba > 0) {
            uint32_t sectors = (node->size_bytes + 511) / 512;
            uint32_t end = node->data_start_lba + sectors + 1; // +1 for header
            if (end > max_end) max_end = end;
        }
    }
    
    return max_end;
}

// Read file
int fs_read_file(const char* name, uint8_t* out, uint32_t max_len) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("RAY64: File not found\n");
        return -1;
    }

    // ACL : vérifier le droit de lecture sur le nœud
    if (!fs_acl_check((uint32_t)idx, ACL_READ)) {
        print_string("RAY64: Acces refuse (lecture) sur '");
        print_string(name);
        print_string("'\n");
        return -1;
    }
    
    FSNode* file = &g_fs.nodes[idx];
    if (file->is_dir) {
        print_string("RAY64: Cannot read directory\n");
        return -1;
    }
    
    if (file->data_start_lba == 0) {
        print_string("RAY64: File has no data\n");
        return -1;
    }
    
    // Read file header
    FileHeader fh;
    uint8_t sector[512];
    if (ata_read(file->data_start_lba, sector, 1) != 0) {
        print_string("RAY64: Failed to read file header\n");
        return -1;
    }
    memcpy(&fh, sector, sizeof(FileHeader));
    
    if (fh.magic != FILE_MAGIC) {
        print_string("RAY64: Invalid file header\n");
        return -1;
    }
    
    uint32_t to_read = fh.size > max_len ? max_len : fh.size;
    uint32_t sectors = (to_read + 511) / 512;
    uint32_t read_bytes = 0;
    
    // Read data sectors
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_read(file->data_start_lba + 1 + s, sector, 1) != 0) {
            print_string("RAY64: Failed to read data sector\n");
            return -1;
        }
        
        for (uint32_t k = 0; k < 512 && read_bytes < to_read; k++) {
            out[read_bytes++] = sector[k];
        }
    }
    
    // Update access time
    RAY64NodeMeta meta;
    ray64_get_meta(file, &meta);
    meta.access_time = ray64_get_timestamp();
    ray64_set_meta(file, &meta);
    
    return (int)to_read;
}

// Write file
int fs_write_file(const char* name, const uint8_t* data, uint32_t size) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("RAY64: File not found\n");
        return -1;
    }

    // ACL : vérifier le droit d'écriture
    if (!fs_acl_check((uint32_t)idx, ACL_WRITE)) {
        print_string("RAY64: Acces refuse (ecriture) sur '");
        print_string(name);
        print_string("'\n");
        return -1;
    }
    
    FSNode* file = &g_fs.nodes[idx];
    if (file->is_dir) {
        print_string("RAY64: Cannot write to directory\n");
        return -1;
    }
    
    // Allocate LBA if needed
    if (file->data_start_lba == 0) {
        file->data_start_lba = fs_next_free_lba();
    }
    
    uint32_t sectors = (size + 511) / 512;
    uint8_t sector[512];
    uint32_t written = 0;
    
    // Write data sectors
    for (uint32_t s = 0; s < sectors; s++) {
        memset(sector, 0, 512);
        uint32_t to_copy = (size - written) > 512 ? 512 : (size - written);
        if (to_copy > 0) memcpy(sector, data + written, to_copy);
        
        if (ata_write(file->data_start_lba + 1 + s, sector, 1) != 0) {
            print_string("RAY64: Failed to write data sector\n");
            return -1;
        }
        written += to_copy;
    }
    
    // Write file header
    FileHeader fh;
    memset(&fh, 0, sizeof(fh));
    fh.magic = FILE_MAGIC;
    fh.type = 0;
    fh.size = size;
    
    memset(sector, 0, 512);
    memcpy(sector, &fh, sizeof(fh));
    if (ata_write(file->data_start_lba, sector, 1) != 0) {
        print_string("RAY64: Failed to write file header\n");
        return -1;
    }
    
    // Update node metadata
    file->size_bytes = size;
    
    RAY64NodeMeta meta;
    ray64_get_meta(file, &meta);
    meta.modify_time = ray64_get_timestamp();
    ray64_set_meta(file, &meta);
    
    ray64_flush_node((uint32_t)idx);
    
    print_string("RAY64: File written successfully\n");
    return 0;
}

// Create node (file or directory)
static int ray64_create_node(const char* name, uint8_t is_dir) {
    if (g_fs.node_count >= FS_MAX_NODES) {
        print_string("RAY64: No free nodes\n");
        return -1;
    }
    
    if (fs_find_in_dir(g_cwd, name) >= 0) {
        print_string("RAY64: Name already exists\n");
        return -1;
    }
    
    uint32_t new_idx = g_fs.node_count++;
    FSNode* new_node = &g_fs.nodes[new_idx];
    memset(new_node, 0, sizeof(FSNode));
    
    strncpy(new_node->name, name, FS_NAME_LEN - 1);
    new_node->name[FS_NAME_LEN - 1] = '\0';
    new_node->is_dir = is_dir;
    new_node->parent = g_cwd;
    new_node->magic = FS_MAGIC;
    new_node->data_start_lba = is_dir ? 0 : fs_next_free_lba();
    new_node->size_bytes = 0;
    
    // Initialize metadata
    RAY64NodeMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.create_time = ray64_get_timestamp();
    meta.modify_time = meta.create_time;
    meta.access_time = meta.create_time;
    meta.link_count  = is_dir ? 2 : 1;
    meta.acl_lock    = 0;

    // Propriétaire = session courante
    meta.uid = session_get_uid();

    // Permissions par défaut selon le contexte :
    //   - Répertoire home /home/<n>    → ACL_HOME_DIR (0770) posé par session.c via fs_acl_set_node
    //   - Tout autre nœud créé par un user normal → owner RWX, admin RWX, others ---  (0770)
    //   - Si le UID est système (pas encore loggué) → seulement admin accède (0070)
    if (meta.uid == UID_SYSTEM) {
        meta.permissions = ACL_ADMIN_FULL; // 0070 - nœuds système
    } else {
        meta.permissions = ACL_OWNER_FULL | ACL_ADMIN_FULL; // 0770 - fichiers/dossiers utilisateur
    }

    ray64_set_meta(new_node, &meta);
    
    // Write initial file header if file
    if (!is_dir) {
        FileHeader fh;
        memset(&fh, 0, sizeof(fh));
        fh.magic = FILE_MAGIC;
        fh.type = 0;
        fh.size = 0;
        
        uint8_t sector[512];
        memset(sector, 0, 512);
        memcpy(sector, &fh, sizeof(fh));
        ata_write(new_node->data_start_lba, sector, 1);
    }
    
    // Add to parent
    FSNode* parent = &g_fs.nodes[g_cwd];
    if (parent->child_count >= FS_MAX_CHILDREN) {
        print_string("RAY64: Parent directory full\n");
        g_fs.node_count--;
        return -1;
    }
    parent->children[parent->child_count++] = new_idx;
    
    // Update parent modify time
    RAY64NodeMeta parent_meta;
    ray64_get_meta(parent, &parent_meta);
    parent_meta.modify_time = ray64_get_timestamp();
    ray64_set_meta(parent, &parent_meta);
    
    ray64_flush_node(g_cwd);
    ray64_flush_node(new_idx);
    
    return (int)new_idx;
}

int fs_mkdir(const char* name) {
    // ACL : droit d'écriture sur le dossier courant
    if (!fs_acl_check(g_cwd, ACL_WRITE)) {
        print_string("RAY64: Acces refuse (creation de dossier ici)\n");
        return -1;
    }
    return ray64_create_node(name, 1);
}

// Create a directory inside a specific parent without changing g_cwd
int fs_mkdir_in_dir(uint32_t parent_idx, const char* name) {
    if (parent_idx >= g_fs.node_count) return -1;
    if (!g_fs.nodes[parent_idx].is_dir) return -1;

    // Check name doesn't already exist in parent
    if (fs_find_in_dir(parent_idx, name) >= 0) {
        // Directory already exists — return its index
        return fs_find_in_dir(parent_idx, name);
    }

    if (g_fs.node_count >= FS_MAX_NODES) return -1;

    uint32_t saved_cwd = g_cwd;
    g_cwd = parent_idx;
    int idx = ray64_create_node(name, 1);
    g_cwd = saved_cwd;
    return idx;
}

int fs_add(const char* name) {
    // ACL : droit d'écriture sur le dossier courant pour y créer un fichier
    if (!fs_acl_check(g_cwd, ACL_WRITE)) {
        print_string("RAY64: Acces refuse (creation dans ce dossier)\n");
        return -1;
    }
    return ray64_create_node(name, 0);
}

// Change directory
int fs_cd(const char* name) {
    if (strcmp(name, "/") == 0) {
        // ACL : vérifier qu'on peut entrer dans la racine
        if (!fs_acl_check(0, ACL_EXEC)) {
            print_string("RAY64: Acces refuse a la racine\n");
            return -1;
        }
        g_cwd = 0;
        return 0;
    }
    
    if (strcmp(name, "..") == 0) {
        if (g_cwd == 0) return 0;
        uint32_t parent = g_fs.nodes[g_cwd].parent;
        // ACL : vérifier qu'on peut entrer dans le parent
        if (!fs_acl_check(parent, ACL_EXEC)) {
            print_string("RAY64: Acces refuse au dossier parent\n");
            return -1;
        }
        g_cwd = parent;
        return 0;
    }
    
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) return -1;
    
    if (!g_fs.nodes[idx].is_dir) return -1;

    // ACL : vérifier le droit d'exécution (= entrer dans le dossier)
    if (!fs_acl_check((uint32_t)idx, ACL_EXEC)) {
        print_string("RAY64: Acces refuse au dossier '");
        print_string(name);
        print_string("'\n");
        return -1;
    }
    
    g_cwd = (uint32_t)idx;
    return 0;
}

// Print working directory
void fs_pwd(void) {
    if (g_cwd == 0) {
        print_string("/\n");
        return;
    }
    
    char path[256] = "";
    uint32_t current = g_cwd;
    
    while (current != 0 && current < g_fs.node_count) {
        FSNode* node = &g_fs.nodes[current];
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "/%s%s", node->name, path);
        strncpy(path, tmp, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        current = node->parent;
    }
    
    print_string(strlen(path) ? path : "/");
    print_string("\n");
}

// List directory
void fs_ls(void) {
    // ACL : droit de lecture sur le dossier courant
    if (!fs_acl_check(g_cwd, ACL_READ)) {
        print_string("RAY64: Acces refuse (lecture du dossier)\n");
        return;
    }

    FSNode* cwd = &g_fs.nodes[g_cwd];
    if (cwd->child_count == 0) {
        print_string("Directory empty\n");
        return;
    }
    
    print_string("Name                       Type   Perms     Owner      Size\n");
    print_string("-------------------------- ------ --------- ---------- ----------\n");
    
    for (uint32_t i = 0; i < cwd->child_count; i++) {
        uint32_t child_idx = cwd->children[i];
        if (child_idx >= g_fs.node_count) continue;
        
        FSNode* child = &g_fs.nodes[child_idx];
        RAY64NodeMeta meta;
        ray64_get_meta(child, &meta);

        // Si pas le droit de lire cette entrée, l'afficher masquée
        int can_read = fs_acl_check(child_idx, ACL_READ);
        
        // Name (padded to 26 chars)
        if (!can_read) {
            // Afficher le nom censuré mais signaler la présence
            print_string("???                        ");
        } else {
            print_string(child->name);
            int name_len = strlen(child->name);
            for (int p = 0; p < (26 - name_len); p++) print_string(" ");
        }
        
        // Type
        print_string(child->is_dir ? "[DIR] " : "[FILE]");
        print_string(" ");
        
        // Permissions (format rwxrwxrwx)
        if (can_read) {
            uint16_t p = meta.permissions;
            print_char((p & ACL_OWNER_R) ? 'r' : '-');
            print_char((p & ACL_OWNER_W) ? 'w' : '-');
            print_char((p & ACL_OWNER_X) ? 'x' : '-');
            print_char((p & ACL_ADMIN_R) ? 'r' : '-');
            print_char((p & ACL_ADMIN_W) ? 'w' : '-');
            print_char((p & ACL_ADMIN_X) ? 'x' : '-');
            print_char((p & ACL_OTHER_R) ? 'r' : '-');
            print_char((p & ACL_OTHER_W) ? 'w' : '-');
            print_char((p & ACL_OTHER_X) ? 'x' : '-');
        } else {
            print_string("---------");
        }
        print_string(" ");

        // Owner
        if (can_read) {
            const char* oname = session_get_name_by_uid(meta.uid);
            int olen = 0;
            if (oname) {
                print_string(oname);
                olen = strlen(oname);
            } else {
                print_string("system");
                olen = 6;
            }
            for (int p = 0; p < (10 - olen); p++) print_string(" ");
        } else {
            print_string("???        ");
        }
        
        // Size
        if (child->is_dir) {
            print_string("         -");
        } else if (can_read) {
            print_dec(child->size_bytes);
        } else {
            print_string("         ?");
        }
        print_string("\n");
    }
}

// Delete file/directory
int fs_delete(const char* name) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("RAY64: Not found\n");
        return -1;
    }

    // ACL : vérifier le droit d'écriture (delete = write sur le nœud)
    if (!fs_acl_check((uint32_t)idx, ACL_WRITE)) {
        print_string("RAY64: Acces refuse (suppression) sur '");
        print_string(name);
        print_string("'\n");
        return -1;
    }
    
    FSNode* node = &g_fs.nodes[idx];
    
    // Check if directory is empty
    if (node->is_dir && node->child_count > 0) {
        print_string("RAY64: Directory not empty\n");
        return -1;
    }
    
    // Remove from parent
    FSNode* parent = &g_fs.nodes[node->parent];
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == (uint32_t)idx) {
            for (uint32_t j = i; j + 1 < parent->child_count; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            break;
        }
    }
    
    // Invalidate node
    node->magic = 0;
    node->name[0] = '\0';
    
    // Update parent modify time
    RAY64NodeMeta meta;
    ray64_get_meta(parent, &meta);
    meta.modify_time = ray64_get_timestamp();
    ray64_set_meta(parent, &meta);
    
    ray64_flush_node(node->parent);
    ray64_flush_node((uint32_t)idx);
    
    return 0;
}

// Tree view (recursive)
static void ray64_print_tree_node(uint32_t idx, uint32_t depth) {
    if (idx >= g_fs.node_count) return;
    
    FSNode* node = &g_fs.nodes[idx];
    if (node->magic != FS_MAGIC) return;
    
    for (uint32_t i = 0; i < depth; i++) print_string("  ");
    print_string(node->name);
    if (node->is_dir) print_string("/");
    print_string("\n");
    
    if (node->is_dir) {
        for (uint32_t i = 0; i < node->child_count; i++) {
            ray64_print_tree_node(node->children[i], depth + 1);
        }
    }
}

void fs_tree(void) {
    ray64_print_tree_node(0, 0);
}

// List all nodes (debug)
void fs_list(void) {
    print_string("RAY64 Filesystem\n");
    print_string("Volume: ");
    print_string((char*)g_fs.volume_name);
    print_string("\n");
    print_string("Nodes: ");
    print_dec(g_fs.node_count);
    print_string("/");
    print_dec(FS_MAX_NODES);
    print_string("\n\n");
    
    for (uint32_t i = 0; i < g_fs.node_count; i++) {
        FSNode* node = &g_fs.nodes[i];
        if (node->magic != FS_MAGIC) continue;
        
        RAY64NodeMeta meta;
        ray64_get_meta(node, &meta);
        
        print_string("[");
        print_dec(i);
        print_string("] ");
        print_string(node->name);
        print_string(node->is_dir ? " [DIR]" : " [FILE]");
        
        if (!node->is_dir) {
            print_string(" size=");
            print_dec(node->size_bytes);
            print_string(" lba=");
            print_dec(node->data_start_lba);
        }
        
        print_string(" perms=");
        print_dec(meta.permissions);
        print_string(" parent=");
        print_dec(node->parent);
        print_string("\n");
    }
}