/* reapfs.c - REAPFS hiérarchique avec offset disque
 * 
 * MODIFICATION CRITIQUE : Système de fichiers décalé à 1 MB (secteur 2048)
 * pour protéger le bootloader (secteur 0-1) et le kernel (secteurs 2-200)
 */

#include <stdint.h>
#include <stddef.h>
#include "reapfs.h"
#include "screen.h"
#include "input.h"
#include "utils.h"
#include "io.h"
#include "ata.h"

int read_file_data(reapfs_inode_t *inode, void *buf, uint32_t buf_size);
static int normalize_path_abs(const char *path_in, char *out, size_t out_sz);

/* Externs fournis par le kernel */
extern void print_string(const char *s);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *ptr, int value, size_t num);
extern int snprintf(char *str, size_t size, const char *format, ...);

/* ---------- Configuration FS avec OFFSET CRITIQUE ---------- */
#define SECTOR_SIZE 512
#define MAX_INODES 256
#define INODE_TABLE_SECTORS 64
#define MAX_FILENAME 32
#define MAX_DIR_ENTRIES 32
#define MAX_PATH 256

/* ⚠️ OFFSET DISQUE : Le FS commence à 1 MB pour protéger le kernel */
#define FS_OFFSET_SECTORS 2048

/* Positions relatives dans le FS (après l'offset) */
#define SUPERBLOCK_SECTOR 0
#define INODE_TABLE_START_SECTOR 1

/* Macro pour convertir secteur FS → secteur disque réel */
#define FS_TO_DISK_SECTOR(fs_sector) ((fs_sector) + FS_OFFSET_SECTORS)

/* ---------- In-memory state ---------- */
static reapfs_super_t g_super;
static reapfs_inode_t g_inodes[MAX_INODES];
static uint8_t g_inode_used[MAX_INODES];

/* cwd interne */
static int g_cwd_ino = 0;
static char g_cwd_path[MAX_PATH] = "/";

/* ---------- Helpers disque AVEC OFFSET ---------- */

static int disk_read_bytes(void *buf, uint64_t offset, size_t len) {
    if (len == 0) return 0;
    
    /* Appliquer l'offset de 1 MB */
    uint64_t real_offset = offset + ((uint64_t)FS_OFFSET_SECTORS * SECTOR_SIZE);
    
    uint64_t start = real_offset / SECTOR_SIZE;
    uint64_t end = (real_offset + (uint64_t)len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t count = (uint32_t)(end - start);
    uint8_t *out = (uint8_t*)buf;

    if ((real_offset % SECTOR_SIZE) == 0 && (len % SECTOR_SIZE) == 0) {
        if (ata_read((uint32_t)start, out, count) != 0) {
            print_string("ATA read failed\n");
            return -1;
        }
        return 0;
    }

    uint8_t tmp[SECTOR_SIZE];
    uint64_t cur_off = real_offset;
    size_t remaining = len;

    for (uint32_t s = 0; s < count; ++s) {
        if (ata_read((uint32_t)(start + s), tmp, 1) != 0) {
            print_string("ATA read failed\n");
            return -1;
        }
        uint64_t sector_base = (uint64_t)(start + s) * SECTOR_SIZE;
        uint64_t copy_start = (cur_off > sector_base) ? (cur_off - sector_base) : 0;
        size_t copy_len = SECTOR_SIZE - copy_start;
        if (copy_len > remaining) copy_len = remaining;
        memcpy(out, tmp + copy_start, copy_len);
        out += copy_len;
        cur_off += copy_len;
        remaining -= copy_len;
        if (remaining == 0) break;
    }
    return 0;
}

static int disk_write_bytes(const void *buf, uint64_t offset, size_t len) {
    if (len == 0) return 0;
    
    /* Appliquer l'offset de 1 MB */
    uint64_t real_offset = offset + ((uint64_t)FS_OFFSET_SECTORS * SECTOR_SIZE);
    
    uint64_t start = real_offset / SECTOR_SIZE;
    uint64_t end = (real_offset + (uint64_t)len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t count = (uint32_t)(end - start);
    const uint8_t *in = (const uint8_t*)buf;

    if ((real_offset % SECTOR_SIZE) == 0 && (len % SECTOR_SIZE) == 0) {
        if (ata_write((uint32_t)start, in, count) != 0) {
            print_string("ATA write failed\n");
            return -1;
        }
        return 0;
    }

    uint8_t tmp[SECTOR_SIZE];
    uint64_t cur_off = real_offset;
    size_t remaining = len;

    for (uint32_t s = 0; s < count; ++s) {
        if (ata_read((uint32_t)(start + s), tmp, 1) != 0) {
            print_string("ATA read before write failed\n");
            return -1;
        }
        uint64_t sector_base = (uint64_t)(start + s) * SECTOR_SIZE;
        uint64_t copy_start = (cur_off > sector_base) ? (cur_off - sector_base) : 0;
        size_t copy_len = SECTOR_SIZE - copy_start;
        if (copy_len > remaining) copy_len = remaining;
        memcpy(tmp + copy_start, in, copy_len);
        if (ata_write((uint32_t)(start + s), tmp, 1) != 0) {
            print_string("ATA write failed\n");
            return -1;
        }
        in += copy_len;
        cur_off += copy_len;
        remaining -= copy_len;
        if (remaining == 0) break;
    }
    return 0;
}

/* ---------- Super / inode persistence ---------- */

static int load_super(void) {
    print_string("FS: load_super start\n");
    uint8_t buf[SECTOR_SIZE];
    if (disk_read_bytes(buf, (uint64_t)SUPERBLOCK_SECTOR * SECTOR_SIZE, SECTOR_SIZE) != 0) {
        return -1;
    }
    memcpy(&g_super, buf, sizeof(reapfs_super_t));
    if (g_super.magic != 0x52455046) {
        return -1;
    }
    if (g_super.inode_count > MAX_INODES) {
        return -1;
    }
    size_t it_size = (size_t)g_super.inode_count * sizeof(reapfs_inode_t);
    size_t it_sectors = (it_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (disk_read_bytes((uint8_t*)g_inodes, (uint64_t)INODE_TABLE_START_SECTOR * SECTOR_SIZE, it_sectors * SECTOR_SIZE) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < g_super.inode_count && i < MAX_INODES; ++i) {
        g_inode_used[i] = g_inodes[i].used ? 1 : 0;
    }
    return 0;
}

static int save_super(void) {
    uint8_t buf[SECTOR_SIZE];
    memset(buf, 0, SECTOR_SIZE);
    memcpy(buf, &g_super, sizeof(reapfs_super_t));
    if (disk_write_bytes(buf, (uint64_t)SUPERBLOCK_SECTOR * SECTOR_SIZE, SECTOR_SIZE) != 0) {
        return -1;
    }
    size_t it_size = (size_t)g_super.inode_count * sizeof(reapfs_inode_t);
    size_t it_sectors = (it_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (disk_write_bytes((uint8_t*)g_inodes, (uint64_t)INODE_TABLE_START_SECTOR * SECTOR_SIZE, it_sectors * SECTOR_SIZE) != 0) {
        return -1;
    }
    return 0;
}

/* ---------- Inode management ---------- */

static int alloc_inode(void) {
    for (int i = 0; i < (int)g_super.inode_count && i < MAX_INODES; ++i) {
        if (!g_inode_used[i]) {
            g_inode_used[i] = 1;
            memset(&g_inodes[i], 0, sizeof(reapfs_inode_t));
            g_inodes[i].ino = (uint32_t)i;
            g_inodes[i].used = 1;
            g_inodes[i].mode = 0644;
            save_super();
            return i;
        }
    }
    return -1;
}

static void free_inode(uint32_t ino) {
    if (ino >= g_super.inode_count || ino >= MAX_INODES) return;
    g_inode_used[ino] = 0;
    memset(&g_inodes[ino], 0, sizeof(reapfs_inode_t));
    save_super();
}

/* ---------- File data IO AVEC OFFSET ---------- */

static int write_file_data(reapfs_inode_t *inode, const void *buf, uint32_t size) {
    if (!inode) return -1;
    uint32_t sectors_needed = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (sectors_needed > 12) {
        print_string("FS: file too large\n");
        return -1;
    }
    uint8_t sector_buf[SECTOR_SIZE];
    const uint8_t *in = (const uint8_t*)buf;
    for (uint32_t s = 0; s < sectors_needed; ++s) {
        /* Calcul du LBA réel AVEC offset */
        uint32_t fs_lba = g_super.data_start_sector + (inode->ino * 100) + s;
        /* Stocker LBA FS, PAS disque */
        inode->blocks[s] = fs_lba;
        /* Conversion FS → disque UNIQUEMENT ici */
        uint32_t disk_lba = FS_TO_DISK_SECTOR(fs_lba);uint32_t lba = inode->blocks[s];
        memset(sector_buf, 0, SECTOR_SIZE);
        size_t copy_len = SECTOR_SIZE;
        if (s == sectors_needed - 1) {
            uint32_t remain = size - (s * SECTOR_SIZE);
            if (remain < copy_len) copy_len = remain;
        }
        memcpy(sector_buf, in + s * SECTOR_SIZE, copy_len);
        if (ata_write(disk_lba, sector_buf, 1) != 0) {
            return -1;
        }
    }
    inode->size = size;
    save_super();
    return FS_OK;
}

void fs_debug_print(void) {
    print_string("FS: debug\n");
    for (uint32_t i = 0; i < g_super.inode_count && i < MAX_INODES; ++i) {
        if (g_inode_used[i]) {
            print_string(" ino=");
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%u", i);
            print_string(tmp);
            print_string(" name=");
            print_string(g_inodes[i].name);
            print_string(g_inodes[i].is_dir ? " (dir)\n" : "\n");
        }
    }
}

int fs_create_with_data(const char *path, const void *data, uint32_t size) {
    int ino = fs_create(path);
    if (ino < 0) return FS_ERR;
    if (fs_write(ino, data, size) < 0) {
        fs_remove(path);
        return FS_ERR;
    }
    return ino;
}

/* ---------- Fonctions de navigation ---------- */

int fs_chdir(const char *path) {
    if (!path) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || !g_inodes[ino].is_dir) return FS_ERR;
    
    g_cwd_ino = ino;
    
    if (normalize_path_abs(path, g_cwd_path, sizeof(g_cwd_path)) != 0) {
        return FS_ERR;
    }
    
    return FS_OK;
}

const char *fs_get_cwd(void) {
    return g_cwd_path;
}

int fs_is_dir(uint32_t ino) {
    if (ino >= MAX_INODES) return 0;
    return g_inodes[ino].is_dir ? 1 : 0;
}

int fs_list_dir(fs_entry_t *entries, int max_entries) {
    if (!entries || max_entries <= 0) return FS_ERR;

    int ino = g_cwd_ino;
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    if (!g_inodes[ino].is_dir) return FS_ERR;

    reapfs_dirent_t raw[MAX_DIR_ENTRIES];
    int bytes = read_file_data(&g_inodes[ino], raw, sizeof(raw));
    int count = bytes > 0 ? bytes / (int)sizeof(reapfs_dirent_t) : 0;

    int j = 0;
    for (int i = 0; i < count && j < max_entries; ++i) {
        if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0) {
            continue;
        }
        
        strncpy(entries[j].name, raw[i].name, MAX_FILENAME - 1);
        entries[j].name[MAX_FILENAME - 1] = '\0';
        entries[j].ino = raw[i].ino;
        entries[j].is_dir = g_inodes[raw[i].ino].is_dir ? 1 : 0;
        j++;
    }

    return j;
}

int read_file_data(reapfs_inode_t *inode, void *buf, uint32_t buf_size) {
    if (!inode) return -1;

    uint32_t to_read = inode->size;
    if (to_read == 0) return 0;
    if (buf_size < to_read) to_read = buf_size;

    uint32_t sectors = (to_read + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint8_t sector_buf[SECTOR_SIZE];

    for (uint32_t s = 0; s < sectors; ++s) {

        /* LBA FS stocké dans l’inode */
        uint32_t fs_lba = inode->blocks[s];

        /* Conversion FS → disque (UNE SEULE FOIS) */
        uint32_t disk_lba = FS_TO_DISK_SECTOR(fs_lba);

        /* DEBUG TEMPORAIRE (tu peux l’enlever après) */
        /*
        char tmp[32];
        print_string("FS: read disk LBA=");
        snprintf(tmp, sizeof(tmp), "%u\n", disk_lba);
        print_string(tmp);
        */

        if (ata_read(disk_lba, sector_buf, 1) != 0) {
            print_string("FS: ata_read failed\n");
            return -1;
        }

        size_t copy_len = SECTOR_SIZE;
        if (s == sectors - 1) {
            uint32_t remain = to_read - (s * SECTOR_SIZE);
            if (remain < copy_len) copy_len = remain;
        }

        memcpy((uint8_t*)buf + s * SECTOR_SIZE, sector_buf, copy_len);
    }
    return (int)to_read;
}
/* ---------- Path helpers ---------- */

static int is_valid_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    size_t len = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/') return 0;
        len++;
        if (len >= MAX_FILENAME) return 0;
    }
    return 1;
}

static int normalize_path_abs(const char *path_in, char *out, size_t out_sz) {
    if (!path_in || !out || out_sz < 2) return -1;

    char work[MAX_PATH];
    size_t work_len = 0;

    if (path_in[0] == '/') {
        work_len = 0;
        for (const char *p = path_in; *p && work_len < sizeof(work) - 1; p++) {
            work[work_len++] = *p;
        }
        work[work_len] = '\0';
    } else {
        work_len = 0;
        for (const char *p = g_cwd_path; *p && work_len < sizeof(work) - 1; p++) {
            work[work_len++] = *p;
        }
        if (work_len > 1 && work[work_len - 1] != '/' && work_len < sizeof(work) - 1) {
            work[work_len++] = '/';
        }
        for (const char *p = path_in; *p && work_len < sizeof(work) - 1; p++) {
            work[work_len++] = *p;
        }
        work[work_len] = '\0';
    }

    char components[64][MAX_FILENAME];
    int top = 0;

    const char *p = work;
    if (*p == '/') p++;

    while (*p) {
        char comp[MAX_FILENAME];
        size_t comp_len = 0;
        
        while (*p == '/') p++;
        if (!*p) break;
        
        while (*p && *p != '/' && comp_len < sizeof(comp) - 1) {
            comp[comp_len++] = *p++;
        }
        comp[comp_len] = '\0';
        
        if (comp_len == 0) continue;

        if (strcmp(comp, ".") == 0) {
            continue;
        } else if (strcmp(comp, "..") == 0) {
            if (top > 0) top--;
        } else {
            if (top >= 64) return -1;
            strncpy(components[top], comp, MAX_FILENAME - 1);
            components[top][MAX_FILENAME - 1] = '\0';
            top++;
        }
    }

    if (top == 0) {
        if (out_sz < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    for (int i = 0; i < top; i++) {
        size_t comp_len = strlen(components[i]);
        if (pos + 1 + comp_len + 1 > out_sz) return -1;
        
        out[pos++] = '/';
        memcpy(out + pos, components[i], comp_len);
        pos += comp_len;
    }
    out[pos] = '\0';
    
    return 0;
}

static int split_path(const char *path_in, char *parent_out, char *name_out) {
    if (!path_in || !parent_out || !name_out) return -1;
    
    char abs[MAX_PATH];
    if (normalize_path_abs(path_in, abs, sizeof(abs)) != 0) return -1;
    
    if (strcmp(abs, "/") == 0) return -1;
    
    const char *last_slash = strrchr(abs, '/');
    if (!last_slash) return -1;
    
    strncpy(name_out, last_slash + 1, MAX_FILENAME - 1);
    name_out[MAX_FILENAME - 1] = '\0';
    
    if (!is_valid_name(name_out)) return -1;
    
    if (last_slash == abs) {
        strcpy(parent_out, "/");
    } else {
        size_t parent_len = (size_t)(last_slash - abs);
        if (parent_len >= MAX_PATH) return -1;
        strncpy(parent_out, abs, parent_len);
        parent_out[parent_len] = '\0';
    }
    
    return 0;
}

int find_inode_by_path(const char *path) {
    if (!path) return -1;

    char abs[MAX_PATH];
    if (normalize_path_abs(path, abs, sizeof(abs)) != 0) return -1;

    if (strcmp(abs, "/") == 0) return 0;

    const char *p = abs;
    if (p[0] == '/') p++;

    int current = 0;

    while (*p) {
        size_t len = 0;
        char part[MAX_FILENAME];
        while (*p && *p != '/' && len < sizeof(part) - 1) {
            part[len++] = *p++;
        }
        part[len] = '\0';
        if (*p == '/') p++;

        if (len == 0) continue;

        if (!g_inodes[current].is_dir) return -1;

        reapfs_dirent_t entries[MAX_DIR_ENTRIES];
        int bytes = read_file_data(&g_inodes[current], entries, sizeof(entries));
        int count = bytes > 0 ? bytes / sizeof(reapfs_dirent_t) : 0;

        int found = 0;
        for (int i = 0; i < count; ++i) {
            if (strcmp(entries[i].name, part) == 0) {
                current = (int)entries[i].ino;
                found = 1;
                break;
            }
        }

        if (!found) return -1;
    }

    return current;
}

static int dir_add_entry(uint32_t parent_ino, const char *name, uint32_t child_ino) {
    if (parent_ino >= MAX_INODES) return -1;
    if (!g_inodes[parent_ino].is_dir) return -1;
    if (!is_valid_name(name)) return -1;

    reapfs_dirent_t entries[MAX_DIR_ENTRIES];
    int bytes = read_file_data(&g_inodes[parent_ino], entries, sizeof(entries));
    int count = bytes > 0 ? bytes / (int)sizeof(reapfs_dirent_t) : 0;
    
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return -1;
    }
    
    if (count >= MAX_DIR_ENTRIES) return -1;

    strncpy(entries[count].name, name, MAX_FILENAME - 1);
    entries[count].name[MAX_FILENAME - 1] = '\0';
    entries[count].ino = child_ino;
    
    return write_file_data(&g_inodes[parent_ino], entries, 
                          (uint32_t)((count + 1) * sizeof(reapfs_dirent_t)));
}

static int dir_remove_entry(uint32_t parent_ino, const char *name) {
    if (parent_ino >= MAX_INODES) return -1;
    if (!g_inodes[parent_ino].is_dir) return -1;

    reapfs_dirent_t entries[MAX_DIR_ENTRIES];
    int bytes = read_file_data(&g_inodes[parent_ino], entries, sizeof(entries));
    int count = bytes > 0 ? bytes / (int)sizeof(reapfs_dirent_t) : 0;
    
    int idx = -1;
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) return -1;
    
    for (int i = idx; i < count - 1; ++i) {
        entries[i] = entries[i + 1];
    }
    
    if (count - 1 == 0) {
        return write_file_data(&g_inodes[parent_ino], NULL, 0);
    } else {
        return write_file_data(&g_inodes[parent_ino], entries, 
                              (uint32_t)((count - 1) * sizeof(reapfs_dirent_t)));
    }
}

/* ---------- Format initial AVEC OFFSET ---------- */

static int format_super(uint32_t inode_count) {
    print_string("========================================\n");
    print_string("FS: Formatage avec offset 1MB\n");
    print_string("FS: Secteur disque reel = 2048+\n");
    print_string("========================================\n");
    
    memset(&g_super, 0, sizeof(g_super));
    g_super.magic = 0x52455046;
    g_super.version = 1;
    g_super.inode_table_sectors = INODE_TABLE_SECTORS;
    g_super.inode_count = inode_count;
    g_super.data_start_sector = INODE_TABLE_START_SECTOR + g_super.inode_table_sectors;
    
    memset(g_inodes, 0, sizeof(g_inodes));
    memset(g_inode_used, 0, sizeof(g_inode_used));

    g_inode_used[0] = 1;
    g_inodes[0].used = 1;
    g_inodes[0].is_dir = 1;
    g_inodes[0].ino = 0;
    g_inodes[0].size = 0;
    g_inodes[0].mode = 0755;
    strncpy(g_inodes[0].name, "/", MAX_FILENAME - 1);
    g_inodes[0].name[MAX_FILENAME - 1] = '\0';

    reapfs_dirent_t root_entries[2];
    strncpy(root_entries[0].name, ".", MAX_FILENAME - 1);
    root_entries[0].name[MAX_FILENAME - 1] = '\0';
    root_entries[0].ino = 0;
    strncpy(root_entries[1].name, "..", MAX_FILENAME - 1);
    root_entries[1].name[MAX_FILENAME - 1] = '\0';
    root_entries[1].ino = 0;
    
    if (write_file_data(&g_inodes[0], root_entries, (uint32_t)sizeof(root_entries)) != 0) {
        return -1;
    }

    g_cwd_ino = 0;
    strcpy(g_cwd_path, "/");

    if (save_super() != 0) return -1;
    
    print_string("FS: Formatage termine (kernel protege)\n");
    return 0;
}

/* ---------- API publique ---------- */

int fs_init(void) {
    print_string("========================================\n");
    print_string("FS: Initialisation REAPFS\n");
    print_string("FS: Offset disque = 1 MB (secteur 2048)\n");
    print_string("FS: Zone kernel protegee (0-2047)\n");
    print_string("========================================\n");
    
    memset(g_inode_used, 0, sizeof(g_inode_used));
    memset(g_inodes, 0, sizeof(g_inodes));
    
    if (load_super() == 0) {
        print_string("FS: Superblock charge depuis disque\n");
        g_cwd_ino = 0;
        strcpy(g_cwd_path, "/");
        return FS_OK;
    }
    
    print_string("FS: Aucun superblock trouve\n");
    print_string("FS: Creation nouveau systeme de fichiers...\n");
    
    if (format_super(MAX_INODES) != 0) {
        print_string("FS: ERREUR lors du formatage!\n");
        return FS_ERR;
    }
    
    return FS_OK;
}

int fs_create(const char *path) {
    if (!path) return FS_ERR;
    
    char parent[MAX_PATH];
    char name[MAX_FILENAME];
    if (split_path(path, parent, name) != 0) return FS_ERR;

    int parent_ino = find_inode_by_path(parent);
    if (parent_ino < 0 || !g_inodes[parent_ino].is_dir) return FS_ERR;

    int ino = alloc_inode();
    if (ino < 0) return FS_ERR;

    reapfs_inode_t *node = &g_inodes[ino];
    node->is_dir = 0;
    strncpy(node->name, name, MAX_FILENAME - 1);
    node->name[MAX_FILENAME - 1] = '\0';
    node->size = 0;

    if (dir_add_entry((uint32_t)parent_ino, name, (uint32_t)ino) != 0) {
        free_inode((uint32_t)ino);
        return FS_ERR;
    }

    save_super();
    return ino;
}

reapfs_fd_t fs_open(const char *path, int write) {
    (void)write;
    if (!path) return FS_ERR;
    return find_inode_by_path(path);
}

int fs_write(int fd, const void *buf, uint32_t size) {
    if (fd < 0 || (uint32_t)fd >= g_super.inode_count) return FS_ERR;
    if (!g_inode_used[fd]) return FS_ERR;
    return write_file_data(&g_inodes[fd], buf, size) == 0 ? (int)size : FS_ERR;
}

int fs_read(int fd, void *buf, uint32_t buf_size) {
    if (fd < 0 || (uint32_t)fd >= g_super.inode_count) return FS_ERR;
    if (!g_inode_used[fd]) return FS_ERR;
    return read_file_data(&g_inodes[fd], buf, buf_size);
}

void fs_close(reapfs_fd_t fd) {
    (void)fd;
}

int fs_remove(const char *path) {
    if (!path) return FS_ERR;
    
    char parent[MAX_PATH];
    char name[MAX_FILENAME];
    if (split_path(path, parent, name) != 0) return FS_ERR;
    
    int parent_ino = find_inode_by_path(parent);
    if (parent_ino < 0) return FS_ERR;

    reapfs_dirent_t entries[MAX_DIR_ENTRIES];
    int bytes = read_file_data(&g_inodes[parent_ino], entries, sizeof(entries));
    int count = bytes > 0 ? bytes / (int)sizeof(reapfs_dirent_t) : 0;
    
    int target = -1;
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            target = (int)entries[i].ino;
            break;
        }
    }
    
    if (target < 0) return FS_ERR;

    if (g_inodes[target].is_dir) {
        reapfs_dirent_t tmp[MAX_DIR_ENTRIES];
        int b = read_file_data(&g_inodes[target], tmp, sizeof(tmp));
        int c = b > 0 ? b / (int)sizeof(reapfs_dirent_t) : 0;
        if (c > 2) return FS_ERR;
    }

    if (dir_remove_entry((uint32_t)parent_ino, name) != 0) return FS_ERR;
    free_inode((uint32_t)target);
    save_super();
    return FS_OK;
}

int fs_ls(const char *path, char *out, size_t out_sz) {
    const char *list_path = path ? path : g_cwd_path;
    int ino = find_inode_by_path(list_path);
    if (ino < 0) return FS_ERR;
    if (!g_inodes[ino].is_dir) return FS_ERR;

    reapfs_dirent_t entries[MAX_DIR_ENTRIES];
    int bytes = read_file_data(&g_inodes[ino], entries, sizeof(entries));
    int count = bytes > 0 ? bytes / (int)sizeof(reapfs_dirent_t) : 0;

    size_t pos = 0;
    for (int i = 0; i < count && pos < out_sz - 64; ++i) {
        pos += snprintf(out + pos, out_sz - pos, "%s%s\n", 
                       entries[i].name,
                       g_inodes[entries[i].ino].is_dir ? "/" : "");
    }
    
    if (pos < out_sz) out[pos] = '\0';
    return FS_OK;
}

int fs_mkdir(const char *path) {
    if (!path) return FS_ERR;

    char parent[MAX_PATH];
    char name[MAX_FILENAME];
    if (split_path(path, parent, name) != 0) return FS_ERR;

    int parent_ino = find_inode_by_path(parent);
    if (parent_ino < 0 || !g_inodes[parent_ino].is_dir) return FS_ERR;

    int ino = alloc_inode();
    if (ino < 0) return FS_ERR;

    reapfs_inode_t *node = &g_inodes[ino];
    node->is_dir = 1;
    node->mode = 0755;
    node->parent = parent_ino;
    strncpy(node->name, name, MAX_FILENAME - 1);
    node->name[MAX_FILENAME - 1] = '\0';
    node->size = 0;

    reapfs_dirent_t init_entries[2];
    strncpy(init_entries[0].name, ".", MAX_FILENAME - 1);
    init_entries[0].name[MAX_FILENAME - 1] = '\0';
    init_entries[0].ino = (uint32_t)ino;
    strncpy(init_entries[1].name, "..", MAX_FILENAME - 1);
    init_entries[1].name[MAX_FILENAME - 1] = '\0';
    init_entries[1].ino = (uint32_t)parent_ino;

    if (write_file_data(node, init_entries, (uint32_t)sizeof(init_entries)) != 0) {
        free_inode((uint32_t)ino);
        return FS_ERR;
    }

    if (dir_add_entry((uint32_t)parent_ino, name, (uint32_t)ino) != 0) {
        free_inode((uint32_t)ino);
        return FS_ERR;
    }

    save_super();
    return FS_OK;
}

/* ---------- Structures additionnelles ---------- */

typedef struct {
    char name[MAX_FILENAME];
    uint32_t timestamp;
    uint8_t super_backup[sizeof(reapfs_super_t)];
    uint8_t inodes_backup[sizeof(g_inodes)];
} snapshot_t;

#define MAX_SNAPSHOTS 8
static snapshot_t g_snapshots[MAX_SNAPSHOTS];
static int g_snapshot_count = 0;

static uint8_t g_volume_encrypted = 0;
static uint32_t g_volume_key = 0;

/* ---------- Utilitaires cryptographiques ---------- */

static uint32_t derive_key(const char *password) {
    uint32_t key = 0x5A5A5A5A;
    for (const char *p = password; *p; p++) {
        key = (key << 5) ^ (key >> 27) ^ (*p);
        key *= 0x1234567;
    }
    return key;
}

static void xor_encrypt(uint8_t *data, uint32_t size, uint32_t key) {
    for (uint32_t i = 0; i < size; i++) {
        data[i] ^= ((key >> ((i & 3) * 8)) & 0xFF);
        if ((i & 3) == 3) {
            key = (key << 7) ^ (key >> 25) ^ i;
        }
    }
}

static uint32_t calculate_crc32(const uint8_t *data, uint32_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

/* ---------- Compression RLE ---------- */

static int compress_rle(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_max) {
    uint32_t out_pos = 0;
    uint32_t i = 0;
    
    while (i < in_size && out_pos < out_max - 2) {
        uint8_t current = in[i];
        uint32_t count = 1;
        
        while (i + count < in_size && in[i + count] == current && count < 255) {
            count++;
        }
        
        if (count > 2 || current == 0xFF) {
            out[out_pos++] = 0xFF;
            out[out_pos++] = (uint8_t)count;
            out[out_pos++] = current;
        } else {
            for (uint32_t j = 0; j < count && out_pos < out_max; j++) {
                out[out_pos++] = current;
            }
        }
        
        i += count;
    }
    
    return (int)out_pos;
}

/* ---------- Fonctions avancées ---------- */

int fs_encrypt_file(const char *path, const char *password) {
    if (!path || !password) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    if (g_inodes[ino].is_dir) return FS_ERR;
    
    uint8_t buffer[4096];
    int bytes = read_file_data(&g_inodes[ino], buffer, sizeof(buffer));
    if (bytes < 0) return FS_ERR;
    
    uint32_t key = derive_key(password);
    xor_encrypt(buffer, (uint32_t)bytes, key);
    
    g_inodes[ino].encrypted = 1;
    if (write_file_data(&g_inodes[ino], buffer, (uint32_t)bytes) != 0) {
        return FS_ERR;
    }
    
    save_super();
    return FS_OK;
}

int fs_decrypt_file(const char *path, const char *password) {
    if (!path || !password) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    if (!g_inodes[ino].encrypted) return FS_ERR;
    
    uint8_t buffer[4096];
    int bytes = read_file_data(&g_inodes[ino], buffer, sizeof(buffer));
    if (bytes < 0) return FS_ERR;
    
    uint32_t key = derive_key(password);
    xor_encrypt(buffer, (uint32_t)bytes, key);
    
    g_inodes[ino].encrypted = 0;
    if (write_file_data(&g_inodes[ino], buffer, (uint32_t)bytes) != 0) {
        return FS_ERR;
    }
    
    save_super();
    return FS_OK;
}

int fs_clone_file(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return FS_ERR;
    
    int src_ino = find_inode_by_path(src_path);
    if (src_ino < 0 || src_ino >= MAX_INODES) return FS_ERR;
    if (g_inodes[src_ino].is_dir) return FS_ERR;
    
    int dst_ino = fs_create(dst_path);
    if (dst_ino < 0) return FS_ERR;
    
    for (int i = 0; i < 12; i++) {
        g_inodes[dst_ino].blocks[i] = g_inodes[src_ino].blocks[i];
    }
    
    g_inodes[dst_ino].size = g_inodes[src_ino].size;
    g_inodes[dst_ino].encrypted = g_inodes[src_ino].encrypted;
    g_inodes[dst_ino].compressed = g_inodes[src_ino].compressed;
    g_inodes[dst_ino].cow_source = (uint32_t)src_ino;
    
    save_super();
    return FS_OK;
}

int fs_snapshot_create(const char *name) {
    if (!name || g_snapshot_count >= MAX_SNAPSHOTS) return FS_ERR;
    
    snapshot_t *snap = &g_snapshots[g_snapshot_count];
    
    strncpy(snap->name, name, MAX_FILENAME - 1);
    snap->name[MAX_FILENAME - 1] = '\0';
    snap->timestamp = g_snapshot_count * 1000;
    
    memcpy(snap->super_backup, &g_super, sizeof(g_super));
    memcpy(snap->inodes_backup, g_inodes, sizeof(g_inodes));
    
    g_snapshot_count++;
    return FS_OK;
}

int fs_snapshot_list(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return FS_ERR;
    
    size_t pos = 0;
    for (int i = 0; i < g_snapshot_count && pos < out_sz - 64; i++) {
        pos += snprintf(out + pos, out_sz - pos, 
                       "%d. %s (timestamp: %u)\n",
                       i + 1, g_snapshots[i].name, g_snapshots[i].timestamp);
    }
    
    if (pos < out_sz) out[pos] = '\0';
    return g_snapshot_count > 0 ? FS_OK : FS_ERR;
}

int fs_snapshot_restore(const char *name) {
    if (!name) return FS_ERR;
    
    for (int i = 0; i < g_snapshot_count; i++) {
        if (strcmp(g_snapshots[i].name, name) == 0) {
            memcpy(&g_super, g_snapshots[i].super_backup, sizeof(g_super));
            memcpy(g_inodes, g_snapshots[i].inodes_backup, sizeof(g_inodes));
            
            for (uint32_t j = 0; j < MAX_INODES; j++) {
                g_inode_used[j] = g_inodes[j].used ? 1 : 0;
            }
            
            save_super();
            return FS_OK;
        }
    }
    
    return FS_ERR;
}

int fs_snapshot_delete(const char *name) {
    if (!name) return FS_ERR;
    
    for (int i = 0; i < g_snapshot_count; i++) {
        if (strcmp(g_snapshots[i].name, name) == 0) {
            for (int j = i; j < g_snapshot_count - 1; j++) {
                g_snapshots[j] = g_snapshots[j + 1];
            }
            g_snapshot_count--;
            return FS_OK;
        }
    }
    
    return FS_ERR;
}

int fs_stat(const char *path, fs_stat_t *stat_out) {
    if (!path || !stat_out) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    
    reapfs_inode_t *node = &g_inodes[ino];
    
    memset(stat_out, 0, sizeof(fs_stat_t));
    stat_out->size = node->size;
    stat_out->is_dir = node->is_dir ? 1 : 0;
    stat_out->mode = node->mode;
    stat_out->encrypted = node->encrypted ? 1 : 0;
    stat_out->compressed = node->compressed ? 1 : 0;
    stat_out->created_time = node->created_time;
    stat_out->modified_time = node->modified_time;
    
    if (!node->is_dir && node->size > 0) {
        uint8_t buffer[4096];
        int bytes = read_file_data(node, buffer, sizeof(buffer));
        if (bytes > 0) {
            stat_out->checksum = calculate_crc32(buffer, (uint32_t)bytes);
        }
    }
    
    return FS_OK;
}

int fs_chmod(const char *path, uint32_t mode) {
    if (!path) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    
    g_inodes[ino].mode = mode & 0777;
    save_super();
    return FS_OK;
}

int fs_compress_file(const char *path) {
    if (!path) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    if (g_inodes[ino].is_dir) return FS_ERR;
    if (g_inodes[ino].compressed) return FS_OK;
    
    uint8_t input[4096];
    uint8_t output[4096];
    
    int bytes = read_file_data(&g_inodes[ino], input, sizeof(input));
    if (bytes <= 0) return FS_ERR;
    
    int compressed_size = compress_rle(input, (uint32_t)bytes, output, sizeof(output));
    if (compressed_size <= 0 || compressed_size >= bytes) {
        return FS_ERR;
    }
    
    g_inodes[ino].compressed = 1;
    g_inodes[ino].uncompressed_size = (uint32_t)bytes;
    
    if (write_file_data(&g_inodes[ino], output, (uint32_t)compressed_size) != 0) {
        return FS_ERR;
    }
    
    save_super();
    return FS_OK;
}

int fs_verify_checksum(const char *path, uint32_t *checksum_out) {
    if (!path) return FS_ERR;
    
    int ino = find_inode_by_path(path);
    if (ino < 0 || ino >= MAX_INODES) return FS_ERR;
    if (g_inodes[ino].is_dir) return FS_ERR;
    
    uint8_t buffer[4096];
    int bytes = read_file_data(&g_inodes[ino], buffer, sizeof(buffer));
    if (bytes <= 0) return FS_ERR;
    
    uint32_t calculated = calculate_crc32(buffer, (uint32_t)bytes);
    
    if (checksum_out) {
        *checksum_out = calculated;
    }
    
    if (g_inodes[ino].checksum == 0) {
        g_inodes[ino].checksum = calculated;
        save_super();
        return FS_OK;
    }
    
    return (g_inodes[ino].checksum == calculated) ? FS_OK : FS_ERR;
}

int fs_set_volume_encryption(const char *password) {
    if (!password) return FS_ERR;
    
    g_volume_key = derive_key(password);
    g_volume_encrypted = 1;
    
    g_super.volume_encrypted = 1;
    g_super.volume_key_hash = g_volume_key;
    
    save_super();
    
    print_string("FS: Volume encryption enabled\n");
    return FS_OK;
}