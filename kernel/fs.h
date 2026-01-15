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

// --- File Header (stored at beginning of each file on disk) ---
typedef struct __attribute__((packed)) __FileHeader__ {
    uint32_t magic;           // FILE_MAGIC
    uint32_t type;            // reserved type/flags (0 = normal file)
    uint32_t size;            // actual data size in bytes
    uint8_t  reserved[500];   // padding to fill 512 bytes sector
} FileHeader;

// --- RAY64 Extended Metadata (stored in FSNode reserved space) ---
typedef struct __attribute__((packed)) {
    uint64_t create_time;     // Creation timestamp
    uint64_t modify_time;     // Last modification timestamp
    uint64_t access_time;     // Last access timestamp
    uint16_t permissions;     // Unix-like permissions (e.g., 0755)
    uint16_t uid;             // Owner user ID
    uint16_t gid;             // Owner group ID
    uint16_t link_count;      // Number of hard links
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

#endif // FS_H