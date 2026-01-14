#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

// Déclaration anticipée si nécessaire
struct FSNode;
struct FSTable;

#define FS_TABLE_LBA         2048u
#define FS_TABLE_SECTORS     6144u
#define FS_DATA_BASE_LBA     8192u
#define FS_MAGIC             0x544F5346   // "FSOT" en little-endian
#define FS_MAX_NODES         256
#define FS_NAME_LEN          32
#define FS_MAX_CHILDREN      16

// --- header placé au début de chaque fichier sur disque
#define FILE_MAGIC           0x46494C45  // 'F' 'I' 'L' 'E'
typedef struct __attribute__((packed)) FileHeader {
    uint32_t magic;     // FILE_MAGIC
    uint32_t type;      // reserved type/flags (0 = normal file)
    uint32_t size;      // taille utile en octets
    uint8_t  reserved[500]; // padding pour occuper 512 bytes total si tu veux le stocker directement dans un secteur
} FileHeader;

// --- structures du FS (table / nodes) : packed pour correspondre au layout disque
typedef struct __attribute__((packed)) FSNode {
    char     name[FS_NAME_LEN];
    uint8_t  is_dir;
    uint8_t  _pad[3];
    uint32_t parent;
    uint32_t children[FS_MAX_CHILDREN];
    uint32_t child_count;
    uint32_t data_start_lba;  // adresse (LBA) du fichier sur disque (pointant au secteur header)
    uint32_t size_bytes;      // taille en octets des données (cohérent avec FileHeader.size)
    uint32_t magic;           // doit valoir FS_MAGIC
} FSNode;

typedef struct __attribute__((packed)) FSTable {
    uint32_t magic;
    uint32_t node_count;
    FSNode   nodes[FS_MAX_NODES];
} FSTable;

extern FSTable g_fs;
extern uint32_t g_cwd;

// Prototypes des fonctions
void fs_init(void);
int fs_flush(void);
void fs_format(void);
int fs_mkdir(const char* name);
int fs_add(const char* name);
int fs_cd(const char* name);
void fs_pwd(void);
void fs_ls(void);
int fs_write_file(const char* name, const uint8_t* data, uint32_t size);
int fs_read_file(const char* name, uint8_t* out, uint32_t max_len);
int fs_delete(const char* name);
void fs_list(void);
int fs_find_in_dir(uint32_t dir_idx, const char* name);
uint32_t fs_next_free_lba(void);
int fs_find(const char* name);
void fs_tree(void);

#endif
