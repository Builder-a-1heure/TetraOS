/* reapfs.h - REAPFS Advanced 1.0 (inspiré APFS)
 * 
 * Système de fichiers hiérarchique avec fonctionnalités avancées :
 * - Chiffrement de fichiers et volumes
 * - Snapshots du système de fichiers
 * - Copy-on-Write (COW)
 * - Clones de fichiers instantanés
 * - Métadonnées étendues
 * - Compression RLE
 * - Checksums d'intégrité CRC32
 * - Permissions Unix-like
 * 
 * MODIFICATION CRITIQUE v1.1 :
 * - Système de fichiers décalé à 1 MB (secteur 2048) pour protéger le kernel
 */

#ifndef REAPFS_H
#define REAPFS_H

#include <stdint.h>
#include <stddef.h>

/* ========== Configuration ========== */
#define REAPFS_MAX_INODES       256
#define REAPFS_MAX_FILENAME     32
#define REAPFS_MAX_PATH         256
#define REAPFS_MAX_SNAPSHOTS    16
#define REAPFS_SECTOR_SIZE      512
#define MAX_FILENAME            REAPFS_MAX_FILENAME
#define MAX_DIR_ENTRIES         32

/* ========== Codes de retour ========== */
#define FS_OK   0
#define FS_ERR  -1

/* ========== Types de base ========== */
typedef int reapfs_fd_t;

/* ========== Flags de fichier (style APFS) ========== */
#define REAPFS_FLAG_ENCRYPTED   (1 << 0)  /* Fichier chiffré */
#define REAPFS_FLAG_COMPRESSED  (1 << 1)  /* Fichier compressé */
#define REAPFS_FLAG_COW         (1 << 2)  /* Copy-on-Write actif */
#define REAPFS_FLAG_IMMUTABLE   (1 << 3)  /* Fichier en lecture seule */
#define REAPFS_FLAG_SYSTEM      (1 << 4)  /* Fichier système */

/* ========== Structure inode étendue ========== */
typedef struct {
    /* Champs de base */
    uint32_t ino;                       /* Numéro d'inode */
    uint32_t size;                      /* Taille du fichier */
    uint32_t blocks[12];                /* Pointeurs vers blocs de données */
    uint8_t used;                       /* 1 si inode utilisé */
    uint8_t is_dir;                     /* 1 si répertoire */
    char name[REAPFS_MAX_FILENAME];     /* Nom du fichier/répertoire */
    uint32_t parent;                    /* Inode du répertoire parent */
    
    /* Métadonnées avancées APFS-like */
    uint32_t flags;                     /* REAPFS_FLAG_* combinés */
    uint32_t mode;                      /* Permissions Unix (0644, 0755, etc.) */
    uint32_t created_time;              /* Timestamp création */
    uint32_t modified_time;             /* Timestamp dernière modification */
    uint32_t accessed_time;             /* Timestamp dernier accès */
    uint32_t checksum;                  /* CRC32 du contenu pour intégrité */
    
    /* Copy-on-Write */
    uint32_t cow_refcount;              /* Nombre de références COW */
    uint32_t cow_original;              /* Inode original si clone COW */
    uint32_t cow_source;                /* Alias pour compatibilité */
    
    /* Chiffrement */
    uint8_t encrypted;                  /* 1 si fichier chiffré */
    uint8_t encryption_key[32];         /* Clé de chiffrement dérivée */
    uint8_t encryption_iv[16];          /* IV pour AES (réservé) */
    
    /* Compression */
    uint8_t compressed;                 /* 1 si fichier compressé */
    uint32_t uncompressed_size;         /* Taille avant compression */
    
    /* Propriétaires (Unix-like) */
    uint32_t owner_uid;                 /* User ID propriétaire */
    uint32_t group_gid;                 /* Group ID */
    
    /* Padding pour alignement futur */
    uint8_t _reserved[32];
} reapfs_inode_t;

/* ========== Structure superblock étendue ========== */
typedef struct {
    /* Champs de base */
    uint32_t magic;                     /* 0x52455046 ("REPF") */
    uint32_t version;                   /* Version du FS */
    uint32_t inode_table_sectors;       /* Taille table inodes en secteurs */
    uint32_t inode_count;               /* Nombre total d'inodes */
    uint32_t data_start_sector;         /* Secteur de début des données */
    
    /* Métadonnées volume */
    uint32_t volume_flags;              /* Flags du volume */
    uint8_t volume_encrypted;           /* 1 si volume chiffré globalement */
    uint8_t volume_key[32];             /* Clé maître du volume */
    uint32_t volume_key_hash;           /* Hash de la clé pour vérification */
    
    /* Snapshots */
    uint32_t snapshot_count;            /* Nombre de snapshots actifs */
    uint32_t last_snapshot_id;          /* ID du dernier snapshot */
    
    /* Statistiques */
    uint32_t mount_count;               /* Nombre de montages */
    uint32_t last_check;                /* Timestamp dernier fsck */
    
    /* Réservé pour extensions futures */
    uint8_t reserved[REAPFS_SECTOR_SIZE - 128];
} reapfs_super_t;

/* ========== Structure snapshot ========== */
typedef struct {
    char name[64];                              /* Nom du snapshot */
    uint32_t timestamp;                         /* Timestamp de création */
    uint32_t inode_count;                       /* Nombre d'inodes sauvegardés */
    reapfs_inode_t inodes[REAPFS_MAX_INODES];   /* Copie des inodes */
    uint8_t valid;                              /* 1 si snapshot valide */
} reapfs_snapshot_t;

/* ========== Structure dirent (entrée de répertoire) ========== */
typedef struct {
    char name[REAPFS_MAX_FILENAME];     /* Nom du fichier/répertoire */
    uint32_t ino;                       /* Numéro d'inode */
} reapfs_dirent_t;

/* ========== Structure entry (pour fs_list_dir) ========== */
typedef struct {
    char name[REAPFS_MAX_FILENAME];     /* Nom */
    uint32_t ino;                       /* Inode */
    int is_dir;                         /* 1 si répertoire, 0 si fichier */
} fs_entry_t;

/* ========== Structure stat (métadonnées pour cmd_stat) ========== */
typedef struct {
    uint32_t size;                      /* Taille du fichier */
    int is_dir;                         /* 1 si répertoire */
    uint32_t mode;                      /* Permissions Unix */
    int encrypted;                      /* 1 si chiffré */
    int compressed;                     /* 1 si compressé */
    uint32_t created_time;              /* Timestamp création */
    uint32_t modified_time;             /* Timestamp modification */
    uint32_t accessed_time;             /* Timestamp accès */
    uint32_t checksum;                  /* CRC32 */
    uint32_t cow_refcount;              /* Compteur COW */
} fs_stat_t;

/* ========================================================================
 * API PUBLIQUE DU SYSTÈME DE FICHIERS
 * ======================================================================== */

/* ========== Initialisation ========== */
int fs_init(void);                      /* Initialise ou charge le FS */

/* ========== Opérations de base sur fichiers ========== */
int fs_create(const char *path);                                    /* Crée un fichier vide */
int fs_create_with_data(const char *path, const void *data, uint32_t size);  /* Crée avec contenu */
reapfs_fd_t fs_open(const char *path, int write);                   /* Ouvre un fichier */
int fs_read(int fd, void *buf, uint32_t buf_size);                  /* Lit depuis un fichier */
int fs_write(int fd, const void *buf, uint32_t size);               /* Écrit dans un fichier */
void fs_close(reapfs_fd_t fd);                                       /* Ferme un fichier */
int fs_remove(const char *path);                                    /* Supprime un fichier/répertoire */

/* ========== Opérations sur répertoires ========== */
int fs_mkdir(const char *path);                                     /* Crée un répertoire */
int fs_ls(const char *path, char *out, size_t out_sz);              /* Liste le contenu (format texte) */
int fs_list_dir(fs_entry_t *entries, int max_entries);              /* Liste le cwd (format structuré) */

/* ========== Navigation ========== */
int fs_chdir(const char *path);                                     /* Change de répertoire courant */
const char *fs_get_cwd(void);                                        /* Retourne le chemin courant */
int fs_is_dir(uint32_t ino);                                        /* Vérifie si inode est un répertoire */

/* ========== Utilitaires ========== */
void fs_debug_print(void);                                           /* Affiche l'état du FS (debug) */
int find_inode_by_path(const char *path);                           /* Trouve l'inode d'un chemin */

/* ========== Chiffrement ========== */
int fs_encrypt_file(const char *path, const char *password);        /* Chiffre un fichier */
int fs_decrypt_file(const char *path, const char *password);        /* Déchiffre un fichier */
int fs_set_volume_encryption(const char *password);                 /* Active chiffrement du volume */

/* ========== Snapshots ========== */
int fs_snapshot_create(const char *name);                           /* Crée un snapshot */
int fs_snapshot_list(char *out, size_t out_sz);                     /* Liste les snapshots */
int fs_snapshot_restore(const char *name);                          /* Restaure un snapshot */
int fs_snapshot_delete(const char *name);                           /* Supprime un snapshot */

/* ========== Copy-on-Write ========== */
int fs_clone_file(const char *src_path, const char *dst_path);      /* Clone instantané COW */

/* ========== Métadonnées et permissions ========== */
int fs_stat(const char *path, fs_stat_t *stat_out);                 /* Récupère les métadonnées */
int fs_chmod(const char *path, uint32_t mode);                      /* Change les permissions */
int fs_touch(const char *path);                                     /* Met à jour les timestamps */

/* ========== Compression ========== */
int fs_compress_file(const char *path);                             /* Compresse un fichier (RLE) */
int fs_decompress_file(const char *path);                           /* Décompresse un fichier */

/* ========== Intégrité ========== */
int fs_verify_checksum(const char *path, uint32_t *checksum_out);   /* Vérifie le checksum */
int fs_update_checksum(const char *path);                           /* Met à jour le checksum */

#endif /* REAPFS_H */