// fs.h - RAY64 Filesystem Header
#ifndef FS_H
#define FS_H
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct __FSNode__ FSNode;
typedef struct __FSTable__ FSTable;
typedef struct __FileHeader__ FileHeader;

// Filesystem layout constants
#define FS_TABLE_LBA         2048u
#define FS_TABLE_SECTORS     6144u
#define FS_DATA_BASE_LBA     8192u
#define FS_MAGIC             0x544F5346   // "FSOT" in little-endian
#define FS_MAX_NODES         256
#define FS_NAME_LEN          32
#define FS_MAX_CHILDREN      16

// File header magic
#define FILE_MAGIC           0x46494C45  // 'F' 'I' 'L' 'E'

// RAY64 specific constants
#define RAY64_VERSION        1
#define RAY64_META_SIZE      32  // Space for extended metadata in reserved

// ============================================================
// ACL - Système de contrôle d'accès RAY64
// ============================================================
//
// Les permissions sont stockées dans un uint16_t sur 9 bits :
//
//   bits 8-6 : droits OWNER  (propriétaire du nœud)
//   bits 5-3 : droits ADMIN  (sessions is_admin == 1)
//   bits 2-0 : droits OTHER  (toutes les autres sessions)
//
//   Chaque triplet : bit2=READ  bit1=WRITE  bit0=EXEC(cd pour dirs)
//
// Exemples :
//   0b111_111_000 = 0770  → owner+admin RWX, autres rien
//   0b111_101_000 = 0750  → owner RWX, admin RX, autres rien
//   0b111_000_000 = 0700  → owner seul
//   0b111_111_111 = 0777  → tout le monde (rare)
//
// UID = index de la session propriétaire dans g_session_manager.sessions[]
// UID = 0xFFFF → propriétaire = système (nœuds créés avant les sessions)
// ============================================================

// Masques de bits ACL
#define ACL_OWNER_R   (1 << 8)   // 0400
#define ACL_OWNER_W   (1 << 7)   // 0200
#define ACL_OWNER_X   (1 << 6)   // 0100
#define ACL_ADMIN_R   (1 << 5)   // 0040
#define ACL_ADMIN_W   (1 << 4)   // 0020
#define ACL_ADMIN_X   (1 << 3)   // 0010
#define ACL_OTHER_R   (1 << 2)   // 0004
#define ACL_OTHER_W   (1 << 1)   // 0002
#define ACL_OTHER_X   (1 << 0)   // 0001

// Valeurs prédéfinies utiles
#define ACL_OWNER_FULL    (ACL_OWNER_R | ACL_OWNER_W | ACL_OWNER_X)  // 0700
#define ACL_ADMIN_FULL    (ACL_ADMIN_R | ACL_ADMIN_W | ACL_ADMIN_X)  // 0070
#define ACL_ADMIN_RX      (ACL_ADMIN_R |               ACL_ADMIN_X)  // 0050
#define ACL_HOME_DIR      (ACL_OWNER_FULL | ACL_ADMIN_FULL)          // 0770 home user
#define ACL_ROOT_DIR      (ACL_OWNER_FULL | ACL_ADMIN_FULL)          // 0770 /home root
#define ACL_PUBLIC_R      (ACL_OWNER_FULL | ACL_ADMIN_FULL | ACL_OTHER_R) // 0774

// Opérations ACL à vérifier
typedef enum {
    ACL_READ  = 0,  // Lire un fichier / entrer dans un dossier (ls)
    ACL_WRITE = 1,  // Écrire / créer / modifier
    ACL_EXEC  = 2,  // cd dans un dossier / exécuter un .tex
} AclOp;

// UID spéciaux
#define UID_SYSTEM  0xFFFF   // nœuds créés avant les sessions
#define UID_NOOWNER 0xFFFE   // nœud sans propriétaire
#define UID_ROOT    0xFFFD   // opération système root (session_root_enter)

// --- File Header (stored at beginning of each file on disk) ---
typedef struct __attribute__((packed)) __FileHeader__ {
    uint32_t magic;           // FILE_MAGIC
    uint32_t type;            // reserved type/flags (0 = normal file)
    uint32_t size;            // actual data size in bytes
    uint8_t  reserved[500];   // padding to fill 512 bytes sector
} FileHeader;

// --- RAY64 Extended Metadata (stored in FSNode reserved space) ---
// Taille totale : 32 bytes exactement (= RAY64_META_SIZE)
typedef struct __attribute__((packed)) {
    uint64_t create_time;    // 8 : timestamp création
    uint64_t modify_time;    // 8 : timestamp modification
    uint64_t access_time;    // 8 : timestamp dernier accès
    uint16_t permissions;    // 2 : ACL 9 bits (owner|admin|other) rwxrwxrwx
    uint16_t uid;            // 2 : index session propriétaire (UID_SYSTEM = nœud système)
    uint16_t link_count;     // 2 : nombre de liens
    uint8_t  acl_lock;       // 1 : 1 = seul un admin peut modifier les permissions
    uint8_t  _pad;           // 1 : alignement
} RAY64NodeMeta;

// --- Filesystem Node (packed for disk layout compatibility) ---
typedef struct __attribute__((packed)) __FSNode__ {
    char     name[FS_NAME_LEN];        // File/directory name
    uint8_t  is_dir;                   // 1 = directory, 0 = file
    uint8_t  _pad[3];                  // Padding for alignment
    uint32_t parent;                   // Parent node index
    uint32_t children[FS_MAX_CHILDREN]; // Child node indices (for directories)
    uint32_t child_count;              // Number of children
    uint32_t data_start_lba;           // Starting LBA for file data (points to header sector)
    uint32_t size_bytes;               // File size in bytes
    uint32_t magic;                    // Must equal FS_MAGIC for validation
    uint8_t  reserved[RAY64_META_SIZE]; // Extended metadata (RAY64NodeMeta)
} FSNode;

// --- Filesystem Table (main structure) ---
typedef struct __attribute__((packed)) __FSTable__ {
    uint32_t magic;           // FS_MAGIC
    uint32_t node_count;      // Current number of nodes
    uint32_t version;         // RAY64 version
    uint8_t  volume_name[64]; // Volume name
    uint8_t  fs_reserved[56]; // Reserved for future use (alignment to nice boundary)
    FSNode   nodes[FS_MAX_NODES]; // Array of all nodes
} FSTable;

// Global filesystem state
extern FSTable g_fs;
extern uint32_t g_cwd;  // Current working directory node index

// --- Core Filesystem Functions ---

// Initialize filesystem (load from disk or format if invalid)
void fs_init(void);

// Flush filesystem metadata to disk
int fs_flush(void);

// Format the filesystem (creates new empty filesystem)
void fs_format(void);

// --- Directory Operations ---

// Create a new directory
int fs_mkdir(const char* name);

// Create a directory inside a specific parent directory (does not depend on g_cwd)
// Returns the new node index or -1 on error
int fs_mkdir_in_dir(uint32_t parent_idx, const char* name);

// Change current working directory
int fs_cd(const char* name);

// Print current working directory path
void fs_pwd(void);

// List contents of current directory
void fs_ls(void);

// Display filesystem tree from root
void fs_tree(void);

// --- File Operations ---

// Create a new file (empty)
int fs_add(const char* name);

// Write data to a file
int fs_write_file(const char* name, const uint8_t* data, uint32_t size);

// Read data from a file
int fs_read_file(const char* name, uint8_t* out, uint32_t max_len);

// Delete a file or empty directory
int fs_delete(const char* name);

// --- Search and Utility Functions ---

// Find a node by name in current directory
int fs_find(const char* name);

// Find a node by name in specific directory
int fs_find_in_dir(uint32_t dir_idx, const char* name);

// Get next free LBA for data allocation
uint32_t fs_next_free_lba(void);

// List all filesystem nodes (debug function)
void fs_list(void);

// --- ACL Functions ---

// Vérifie si la session courante a le droit d'effectuer op sur le nœud node_idx
// Retourne 1 si autorisé, 0 si refusé
// bypass_admin : si 1, un admin passe toujours (comportement normal)
int fs_acl_check(uint32_t node_idx, AclOp op);

// Change les permissions d'un nœud (chmod)
// new_perms : valeur 9 bits (ex: ACL_HOME_DIR = 0770)
// Retourne 0 si succès, -1 si refus ou erreur
int fs_chmod(const char* name, uint16_t new_perms);

// Change le propriétaire d'un nœud (chown)
// new_uid : index de session (ou UID_SYSTEM)
// Seul un admin peut changer le propriétaire
int fs_chown(const char* name, uint16_t new_uid);

// Affiche les permissions ACL d'un nœud sous forme lisible
// Sortie : "rwxrwx--- owner:alice [lock]"
void fs_acl_print(uint32_t node_idx);

// Définit les permissions d'un nœud par son index (usage interne + session.c)
void fs_acl_set_node(uint32_t node_idx, uint16_t perms, uint16_t uid);

#endif // FS_H