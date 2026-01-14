// fs.c - complet, compatible avec le fs.h fourni
#include "fs.h"
#include "utils.h"
#include "screen.h"
#include "ata.h"
#include <stddef.h>
#include <stdint.h>

FSTable g_fs;
uint32_t g_cwd = 0;

// buffer temporaire pour opérations (réutilisé)
// ATTENTION: taille = FS_TABLE_SECTORS * 512 ; si mémoire limitée réduire usage de gros buffers.
static uint8_t fs_temp_buffer[FS_TABLE_SECTORS * 512];

#define MAX_READ_ATTEMPTS 3
#define SECTORS_PER_BATCH 256   // 256 * 512 = 131072 bytes per batch
#define MOVE_CHUNK_SECTORS 128  // pour déplacer fichiers par morceaux

// --- helpers: read_with_retry (par chunks) ---
static int read_with_retry(uint32_t lba, uint8_t* buf, uint32_t count) {
    const uint32_t CHUNK = SECTORS_PER_BATCH;
    for (int attempt = 0; attempt < MAX_READ_ATTEMPTS; attempt++) {
        print_string("FS: Read attempt ");
        print_dec(attempt + 1);
        print_string("/");
        print_dec(MAX_READ_ATTEMPTS);
        print_string("\n");

        int success = 1;
        uint32_t sectors_read = 0;

        for (uint32_t i = 0; i < count; i += CHUNK) {
            uint32_t blocks_to_read = (count - i) > CHUNK ? CHUNK : (count - i);
            int r = ata_read(lba + i, buf + i * 512, blocks_to_read);
            if (r != 0) {
                print_string("FS: Failed block read at LBA ");
                print_dec(lba + i);
                print_string(" (attempt ");
                print_dec(attempt + 1);
                print_string(")\n");
                success = 0;
                break;
            }
            sectors_read += blocks_to_read;
            if ((sectors_read % 1024) == 0 || sectors_read == count) {
                print_string("FS: Read ");
                print_dec(sectors_read);
                print_string("/");
                print_dec(count);
                print_string(" sectors\n");
            }
        }

        if (success) {
            print_string("FS: Read successful\n");
            return 0;
        }
    }
    print_string("FS: Read failed after ");
    print_dec(MAX_READ_ATTEMPTS);
    print_string(" attempts\n");
    return -1;
}


// --- helpers: write_sectors (par chunks) ---
static int write_sectors(uint32_t lba, const uint8_t* buf, uint32_t count) {
    const uint32_t CHUNK = SECTORS_PER_BATCH;
    print_string("FS: write_sectors LBA=");
    print_dec(lba);
    print_string(" count=");
    print_dec(count);
    print_string("\n");

    for (uint32_t i = 0; i < count; i += CHUNK) {
        uint32_t blocks_to_write = (count - i) > CHUNK ? CHUNK : (count - i);
        if ((i % (CHUNK * 4)) == 0) {
            print_string("FS: Writing chunk at LBA ");
            print_dec(lba + i);
            print_string(" blocks=");
            print_dec(blocks_to_write);
            print_string("\n");
        }
        int r = ata_write(lba + i, buf + i * 512, blocks_to_write);
        if (r != 0) {
            print_string("FS: Write error at LBA ");
            print_dec(lba + i);
            print_string(" (res=");
            print_dec(r);
            print_string(")\n");
            return -1;
        }
    }
    print_string("FS: write_sectors OK\n");
    return 0;
}

// --- read header (1 sector) ---
typedef struct {
    uint32_t magic;
    uint32_t node_count;
    uint8_t reserved[504];
} __attribute__((packed)) FSHeader;

static FSHeader fs_header_tmp;

static int fs_read_header(void) {
    uint8_t sec[512];
    int r = ata_read(FS_TABLE_LBA, sec, 1);
    if (r != 0) return r;
    memcpy(&fs_header_tmp, sec, sizeof(FSHeader));
    return 0;
}

// --- flush minimal (taille de FSTable) ---
int fs_flush(void) {
    print_string("FS: Flushing table (minimal)\n");

    uint32_t needed_sectors = (sizeof(FSTable) + 511) / 512;
    if (needed_sectors > FS_TABLE_SECTORS) {
        print_string("FS: needed_sectors > FS_TABLE_SECTORS\n");
        return -1;
    }

    uint32_t bytes = needed_sectors * 512;
    memset(fs_temp_buffer, 0, bytes);
    uint32_t copy_len = sizeof(FSTable);
    if (copy_len > bytes) copy_len = bytes;
    memcpy(fs_temp_buffer, (uint8_t*)&g_fs, copy_len);

    int r = write_sectors(FS_TABLE_LBA, fs_temp_buffer, needed_sectors);
    if (r != 0) {
        print_string("FS: fs_flush failed\n");
        return -1;
    }
    print_string("FS: fs_flush success\n");
    return 0;
}

// --- flush node (écrit uniquement les secteurs contenant le node) ---
static int fs_flush_node(uint32_t node_idx) {
    if (node_idx >= g_fs.node_count) {
        print_string("FS: fs_flush_node invalid idx\n");
        return -1;
    }

    uint8_t* base = (uint8_t*)&g_fs;
    uint8_t* nodeptr = (uint8_t*)&g_fs.nodes[node_idx];
    ptrdiff_t offset = nodeptr - base;
    if (offset < 0) { print_string("FS: unexpected node offset\n"); return -1; }

    size_t node_size = sizeof(g_fs.nodes[0]);
    uint32_t start_sector = (uint32_t)(offset / 512);
    uint32_t end_sector = (uint32_t)((offset + node_size + 511) / 512);
    uint32_t sectors = end_sector - start_sector;
    if (sectors == 0) return 0;
    if (start_sector + sectors > FS_TABLE_SECTORS) {
        print_string("FS: fs_flush_node out of bounds\n");
        return -1;
    }

    uint32_t bytes = sectors * 512;
    memset(fs_temp_buffer, 0, bytes);
    memcpy(fs_temp_buffer, base + start_sector * 512, bytes);

    print_string("FS: fs_flush_node idx=");
    print_dec(node_idx);
    print_string(" start_sector=");
    print_dec(start_sector);
    print_string(" sectors=");
    print_dec(sectors);
    print_string("\n");

    int r = write_sectors(FS_TABLE_LBA + start_sector, fs_temp_buffer, sectors);
    if (r != 0) {
        print_string("FS: fs_flush_node write failed\n");
        return -1;
    }
    return 0;
}

// --- format (init en mémoire puis écriture progressive) ---
void fs_format(void) {
    print_string("FS: Formatting new filesystem\n");

    memset(&g_fs, 0, sizeof(FSTable));
    g_fs.magic = FS_MAGIC;
    g_fs.node_count = 1;

    FSNode* root = &g_fs.nodes[0];
    memset(root, 0, sizeof(FSNode));
    strncpy(root->name, "/", FS_NAME_LEN - 1);
    root->is_dir = 1;
    root->parent = 0;
    root->magic = FS_MAGIC;
    root->data_start_lba = FS_DATA_BASE_LBA;

    // Ecrire 1 secteur test
    uint8_t header_sector[512];
    memset(header_sector, 0, 512);
    uint32_t copy_len = sizeof(FSTable) < 512 ? sizeof(FSTable) : 512;
    memcpy(header_sector, (uint8_t*)&g_fs, copy_len);

    print_string("FS: Writing header test sector... ");
    if (ata_write(FS_TABLE_LBA, header_sector, 1) != 0) {
        print_string("FAILED\n");
        print_string("FS: Format failed: cannot write header sector\n");
        return;
    }
    print_string("OK\n");

    // Ecriture complète de la zone (par batches) pour zero-ing and reserving space
    print_string("FS: Writing full table area (this may take a while)...\n");
    uint32_t total = FS_TABLE_SECTORS;
    uint32_t written = 0;
    uint32_t batch = SECTORS_PER_BATCH;
    if (batch == 0) batch = 256;

    while (written < total) {
        uint32_t to_write = (total - written) > batch ? batch : (total - written);
        memset(fs_temp_buffer, 0, to_write * 512);
        if (written == 0) {
            uint32_t c = sizeof(FSTable);
            if (c > to_write * 512) c = to_write * 512;
            memcpy(fs_temp_buffer, (uint8_t*)&g_fs, c);
        }
        int r = ata_write(FS_TABLE_LBA + written, fs_temp_buffer, to_write);
        if (r != 0) {
            print_string("FS: Failed writing table area at LBA ");
            print_dec(FS_TABLE_LBA + written);
            print_string("\n");
            print_string("FS: Aborting format, memory-only fallback\n");
            return;
        }
        written += to_write;
        if ((written % (batch * 4)) == 0 || written == total) {
            print_string("FS: Written ");
            print_dec(written);
            print_string("/");
            print_dec(total);
            print_string(" sectors\n");
        }
    }

    print_string("FS: Format completed successfully\n");
}

// --- init (header + conditional read minimal) ---
void fs_init(void) {
    print_string("FS: Initializing (fast header check)\n");

    if (fs_read_header() != 0) {
        print_string("FS: Header read failed -> formatting\n");
        fs_format();
        return;
    }

    if (fs_header_tmp.magic != FS_MAGIC ||
        fs_header_tmp.node_count == 0 ||
        fs_header_tmp.node_count > FS_MAX_NODES) {
        print_string("FS: No valid table found -> formatting\n");
        fs_format();
        return;
    }

    uint32_t needed_sectors = (sizeof(FSTable) + 511) / 512;
    if (needed_sectors > FS_TABLE_SECTORS) {
        print_string("FS: Table too large -> formatting\n");
        fs_format();
        return;
    }

    print_string("FS: Loading full table, sectors=");
    print_dec(needed_sectors);
    print_string("\n");

    if (read_with_retry(FS_TABLE_LBA, fs_temp_buffer, needed_sectors) != 0) {
        print_string("FS: Failed to read full table -> formatting\n");
        fs_format();
        return;
    }

    uint32_t copy_len = sizeof(FSTable);
    if (copy_len > needed_sectors * 512) copy_len = needed_sectors * 512;
    memcpy(&g_fs, fs_temp_buffer, copy_len);

    if (g_fs.magic != FS_MAGIC) {
        print_string("FS: Magic mismatch after load -> formatting\n");
        fs_format();
    } else {
        print_string("FS: Loaded successfully\n");
    }
}

// --- helper find in dir ---
int fs_find_in_dir(uint32_t dir_idx, const char* name) {
    if (dir_idx >= g_fs.node_count || !g_fs.nodes[dir_idx].is_dir) return -1;

    FSNode* dir = &g_fs.nodes[dir_idx];
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t child_idx = dir->children[i];
        if (child_idx >= g_fs.node_count) continue;

        FSNode* child = &g_fs.nodes[child_idx];

        // Sécurité : s'assurer que le nom est bien terminé
        child->name[FS_NAME_LEN - 1] = '\0';

        // Comparaison insensible à la casse
        if (strcasecmp(child->name, name) == 0) {
            return (int)child_idx;
        }
    }
    return -1;
}

int fs_find(const char* name) {
    return fs_find_in_dir(g_cwd, name);
}

// --- next free LBA for data (append-only allocation) ---
uint32_t fs_next_free_lba(void) {
    uint32_t max_end = FS_DATA_BASE_LBA;
    for (uint32_t i = 0; i < g_fs.node_count; i++) {
        if (g_fs.nodes[i].magic == FS_MAGIC && !g_fs.nodes[i].is_dir) {
            uint32_t sectors = (g_fs.nodes[i].size_bytes + 511) / 512;
            uint32_t end = g_fs.nodes[i].data_start_lba + sectors;
            if (end > max_end) max_end = end;
        }
    }
    return max_end;
}

// --- read file (contiguous) ---
int fs_read_file(const char* name, uint8_t* out, uint32_t max_len) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("FS: fs_read_file: file not found\n");
        return -1;
    }
    FSNode* file = &g_fs.nodes[idx];
    if (file->is_dir) {
        print_string("FS: fs_read_file: cannot read a directory\n");
        return -1;
    }

    if (file->data_start_lba == 0) {
        print_string("FS: fs_read_file: node has no data_start_lba\n");
        return -1;
    }

    // Lire header
    FileHeader fh;
    uint8_t sector[512];
    if (ata_read(file->data_start_lba, sector, 1) != 0) {
        print_string("FS: fs_read_file: failed to read header\n");
        return -1;
    }
    memcpy(&fh, sector, sizeof(FileHeader));

    if (fh.magic != FILE_MAGIC) {
        print_string("FS: fs_read_file: invalid file header\n");
        return -1;
    }

    uint32_t total_to_read = fh.size;
    if (total_to_read > max_len) total_to_read = max_len;

    uint32_t sectors = (total_to_read + 511) / 512;
    uint32_t readn = 0;

    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_read(file->data_start_lba + 1 + s, sector, 1) != 0) {
            print_string("FS: fs_read_file: ata_read data failed\n");
            return -1;
        }
        for (uint32_t k = 0; k < 512 && readn < total_to_read; k++, readn++) {
            out[readn] = sector[k];
        }
    }

    return (int)total_to_read;
}



// --- move a node's data to new location (chunked) and update node metadata and flush it ---
// returns 0 on success, -1 on failure
static int fs_move_node_data(uint32_t node_idx, uint32_t new_start_lba) {
    if (node_idx >= g_fs.node_count) return -1;
    FSNode* n = &g_fs.nodes[node_idx];
    if (n->is_dir) return 0; // nothing to move for directories (size likely 0)

    uint32_t sectors = (n->size_bytes + 511) / 512;
    if (sectors == 0) {
        n->data_start_lba = new_start_lba;
        if (fs_flush_node(node_idx) != 0) {
            print_string("FS: fs_move_node_data: flush node failed\n");
            return -1;
        }
        return 0;
    }

    // buffer chunk
    static uint8_t move_buf[MOVE_CHUNK_SECTORS * 512];
    uint32_t moved = 0;
    while (moved < sectors) {
        uint32_t to_move = (sectors - moved) > MOVE_CHUNK_SECTORS ? MOVE_CHUNK_SECTORS : (sectors - moved);
        // read from old location
        if (ata_read(n->data_start_lba + moved, move_buf, to_move) != 0) {
            print_string("FS: move_node_data read failed\n");
            return -1;
        }
        // write to new location
        if (ata_write(new_start_lba + moved, move_buf, to_move) != 0) {
            print_string("FS: move_node_data write failed\n");
            return -1;
        }
        moved += to_move;
    }

    // update metadata and flush node
    n->data_start_lba = new_start_lba;
    if (fs_flush_node(node_idx) != 0) {
        print_string("FS: move_node_data flush_node failed\n");
        return -1;
    }
    return 0;
}

// --- write file with neighbor-move logic ---
int fs_write_file(const char* name, const uint8_t* data, uint32_t size) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) {
        print_string("FS: fs_write_file: file not found\n");
        return -1;
    }

    FSNode* file = &g_fs.nodes[idx];
    if (file->is_dir) {
        print_string("FS: fs_write_file: target is a directory\n");
        return -1;
    }

    // Allouer une adresse si jamais = 0
    if (file->data_start_lba == 0) {
        file->data_start_lba = fs_next_free_lba();
    }

    uint32_t new_sectors = (size + 511) / 512;

    // écriture : on réserve le secteur 0 comme header ; les données commencent à data_start_lba + 1
    uint32_t start = file->data_start_lba + 1;
    uint32_t written = 0;
    uint8_t tmpbuf[512];

    for (uint32_t s = 0; s < new_sectors; s++) {
        memset(tmpbuf, 0, sizeof(tmpbuf));
        uint32_t to_copy = (size - written) > 512 ? 512 : (size - written);
        if (to_copy > 0) memcpy(tmpbuf, data + written, to_copy);

        if (ata_write(start + s, tmpbuf, 1) != 0) {
            print_string("FS: fs_write_file ata_write failed\n");
            return -1;
        }
        written += to_copy;
    }

    // Mettre à jour header -> size
    FileHeader fh;
    fh.magic = FILE_MAGIC;
    fh.type  = 0;
    fh.size  = size;
    uint8_t header_sector[512];
    memset(header_sector, 0, sizeof(header_sector));
    memcpy(header_sector, &fh, sizeof(FileHeader));
    if (ata_write(file->data_start_lba, header_sector, 1) != 0) {
        print_string("FS: fs_write_file failed to write header\n");
        // on continue : les données sont écrites mais header non à jour
    }

    // Mettre à jour la table
    file->size_bytes = size;
    if (fs_flush_node((uint32_t)idx) != 0) {
        print_string("FS: fs_write_file flush node failed after write\n");
    }

    print_string("FS: fs_write_file completed\n");
    return 0;
}


// --- create node helper ---
static int fs_create_node(const char* name, uint8_t is_dir) {
    if (g_fs.node_count >= FS_MAX_NODES) {
        print_string("FS: No free nodes\n");
        return -1;
    }
    if (fs_find_in_dir(g_cwd, name) >= 0) {
        print_string("FS: Name already exists\n");
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

    // Allouer dynamiquement une adresse libre pour le fichier (header à cette LBA)
    new_node->data_start_lba = fs_next_free_lba();
    new_node->size_bytes = 0;

    // Si c'est un fichier (pas un dir), écrire un header initial sur le LBA attribué
    if (!is_dir) {
        uint8_t sector[512];
        memset(sector, 0, sizeof(sector));
        FileHeader fh;
        fh.magic = FILE_MAGIC;
        fh.type  = 0;
        fh.size  = 0;
        // memcpy du header au début du secteur
        memcpy(sector, &fh, sizeof(FileHeader));
        if (ata_write(new_node->data_start_lba, sector, 1) != 0) {
            print_string("FS: failed to write initial file header\n");
            // on continue quand même : node créé, mais signale l'erreur
        }
    }

    FSNode* cwd = &g_fs.nodes[g_cwd];
    if (cwd->child_count < FS_MAX_CHILDREN) {
        cwd->children[cwd->child_count++] = new_idx;
    } else {
        print_string("FS: parent has too many children\n");
        // rollback node_count?
        g_fs.node_count--;
        return -1;
    }

    // flush parent and new node metadata
    if (fs_flush_node(g_cwd) != 0) print_string("FS: failed to flush parent\n");
    if (fs_flush_node(new_idx) != 0) print_string("FS: failed to flush new node\n");
    return (int)new_idx;
}


int fs_mkdir(const char* name) { return fs_create_node(name, 1); }
int fs_add(const char* name) { return fs_create_node(name, 0); }

// --- cd, pwd, ls, delete, list ---
int fs_cd(const char* name) {
    if (strcmp(name, "/") == 0) { g_cwd = 0; return 0; }
    if (strcmp(name, "..") == 0) {
        if (g_cwd == 0) return 0;
        FSNode* cur = &g_fs.nodes[g_cwd];
        g_cwd = cur->parent;
        return 0;
    }
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) return -1;
    if (!g_fs.nodes[idx].is_dir) return -1;
    g_cwd = (uint32_t)idx;
    return 0;
}

void fs_pwd(void) {
    if (g_cwd == 0) { print_string("/\n"); return; }
    char path[256] = "";
    uint32_t current = g_cwd;
    while (current != 0 && current < g_fs.node_count) {
        char tmp[256];
        // build backwards
        snprintf(tmp, sizeof(tmp), "/%s%s", g_fs.nodes[current].name, path);
        strncpy(path, tmp, sizeof(path)-1);
        current = g_fs.nodes[current].parent;
    }
    if (strlen(path) == 0) print_string("/\n");
    else { print_string(path); print_string("\n"); }
}

void fs_ls(void) {
    FSNode* cwd = &g_fs.nodes[g_cwd];
    if (cwd->child_count == 0) { print_string("Directory empty\n"); return; }

    print_string("Name                       Type   Addr     Size\n");
    print_string("-------------------------- ------ -------- ----\n");
    for (uint32_t i = 0; i < cwd->child_count; i++) {
        uint32_t child_idx = cwd->children[i];
        if (child_idx >= g_fs.node_count) continue;
        FSNode* child = &g_fs.nodes[child_idx];

        // Nom aligné (max FS_NAME_LEN)
        print_string(child->name);
        // padding pour aligner (simple)
        int name_len = 0;
        for (int p = 0; p < FS_NAME_LEN && child->name[p]; p++) name_len++;
        for (int s = 0; s < (26 - name_len); s++) print_string(" ");

        if (child->is_dir) {
            print_string("[DIR]  ");
            print_string("         ");
            print_string("    -\n");
        } else {
            print_string("[FILE] ");
            print_string(" ");
            print_dec(child->data_start_lba);
            print_string(" ");
            print_dec(child->size_bytes);
            print_string("\n");
        }
    }
}


int fs_delete(const char* name) {
    int idx = fs_find_in_dir(g_cwd, name);
    if (idx < 0) return -1;
    FSNode* node = &g_fs.nodes[idx];
    FSNode* parent = &g_fs.nodes[node->parent];
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == (uint32_t)idx) {
            for (uint32_t j = i; j + 1 < parent->child_count; j++) parent->children[j] = parent->children[j+1];
            parent->child_count--;
            break;
        }
    }
    node->magic = 0;
    node->name[0] = '\0';
    if (fs_flush_node(node->parent) != 0) print_string("FS: fs_delete flush parent failed\n");
    if (fs_flush_node((uint32_t)idx) != 0) print_string("FS: fs_delete flush node failed\n");
    return 0;
}


static void fs_print_indent(uint32_t depth) {
    for (uint32_t i = 0; i < depth; i++) {
        print_string("  "); // 2 spaces
    }
}

static void fs_print_tree_node(uint32_t idx, uint32_t depth) {
    if (idx >= g_fs.node_count) return;
    FSNode* n = &g_fs.nodes[idx];
    fs_print_indent(depth);
    print_string(n->name);
    if (n->is_dir) print_string("/");
    print_string("\n");
    if (n->is_dir) {
        for (uint32_t i = 0; i < n->child_count; i++) {
            fs_print_tree_node(n->children[i], depth + 1);
        }
    }
}

void fs_tree(void) {
    // Assume root is index 0
    fs_print_tree_node(0, 0);
}
void fs_list(void) {
    print_string("FS: nodes = ");
    print_dec(g_fs.node_count);
    print_string("\n");
    for (uint32_t i = 0; i < g_fs.node_count; i++) {
        FSNode* n = &g_fs.nodes[i];
        print_string(" - ");
        print_dec(i);
        print_string(": ");
        print_string(n->name);
        print_string(n->is_dir ? " [DIR]" : " [FILE]");
        if (!n->is_dir) {
            print_string(" size=");
            print_dec(n->size_bytes);
            print_string(" lba=");
            print_dec(n->data_start_lba);
        }
        print_string("\n");
    }
}
