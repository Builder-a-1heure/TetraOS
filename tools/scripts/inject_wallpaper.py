#!/usr/bin/env python3
# =============================================================================
#  inject_wallpaper.py — Injecte kernel/gfx/wallpaper.bin dans le FS RAY64
#                        d'un os.img TetraOS existant, sans toucher au reste.
#
#  Usage :
#    python3 inject_wallpaper.py [os.img] [wallpaper.bin]
#    (défauts : os.img  et  kernel/gfx/wallpaper.bin)
#
#  Ce script :
#    1. Lit la FSTable depuis FS_TABLE_LBA
#    2. Cherche si "wallpaper.bin" existe déjà → réutilise son LBA
#       Sinon crée un nouveau nœud dans la table
#    3. Écrit les données à partir du LBA alloué  (header + secteurs data)
#    4. Ré-écrit la FSTable mise à jour → SEULS les secteurs modifiés sont touchés
#    5. Ne touche PAS au kernel, au bootloader, ni aux autres fichiers
#
#  Format sur disque (RAY64) :
#    LBA FS_TABLE_LBA      : FSTable (magic + node_count + nodes[256])
#    LBA data_start_lba    : FileHeader (512 bytes, magic FILE_MAGIC)
#    LBA data_start_lba+1  : données brutes (secteurs 512 bytes)
#    ...
# =============================================================================

import sys
import struct
import os

# ── Constantes (doivent correspondre à fs.h) ─────────────────────────────────
FS_TABLE_LBA      = 2048       # ← mis à jour (était 2048, déplacé pour le kernel+wallpaper)
FS_TABLE_SECTORS  = 6144
FS_DATA_BASE_LBA  = 8192
FS_MAGIC          = 0x544F5346  # "FSOT" little-endian
FILE_MAGIC        = 0x46494C45  # "FILE"
FS_MAX_NODES      = 256
FS_NAME_LEN       = 32
FS_MAX_CHILDREN   = 16
RAY64_META_SIZE   = 32
RAY64_VERSION     = 1

WALLPAPER_FILENAME = "wallpaper.bin"

SECTOR = 512

# ── Structures (packed, identiques à fs.h) ───────────────────────────────────

# FileHeader : 512 bytes
FH_FMT  = "<III500s"       # magic, type, size, reserved[500]
FH_SIZE = struct.calcsize(FH_FMT)
assert FH_SIZE == 512, f"FileHeader size mismatch: {FH_SIZE}"

# RAY64NodeMeta : 32 bytes
META_FMT  = "<QQQHHHBx"    # create,modify,access, perms,uid,link_count,acl_lock,_pad
META_SIZE = struct.calcsize(META_FMT)
assert META_SIZE == RAY64_META_SIZE, f"Meta size mismatch: {META_SIZE}"

# FSNode : 152 bytes
# name[32] + is_dir(1) + _pad[3] + parent(4) + children[16*4] + child_count(4)
#          + data_start_lba(4) + size_bytes(4) + magic(4) + reserved[32]
NODE_FMT  = f"<{FS_NAME_LEN}sB3sI{FS_MAX_CHILDREN}IIIII{RAY64_META_SIZE}s"
NODE_SIZE = struct.calcsize(NODE_FMT)
assert NODE_SIZE == 152, f"FSNode size mismatch: {NODE_SIZE}"

# FSTable header : 132 bytes
FSTAB_HDR_FMT  = "<III64s56s"    # magic, node_count, version, volume_name, fs_reserved
FSTAB_HDR_SIZE = struct.calcsize(FSTAB_HDR_FMT)
assert FSTAB_HDR_SIZE == 132, f"FSTable header size mismatch: {FSTAB_HDR_SIZE}"

# ── Helpers ───────────────────────────────────────────────────────────────────

def read_sector(f, lba):
    f.seek(lba * SECTOR)
    return f.read(SECTOR)

def write_sector(f, lba, data):
    assert len(data) == SECTOR, f"Sector data must be exactly {SECTOR} bytes, got {len(data)}"
    f.seek(lba * SECTOR)
    f.write(data)

def read_sectors(f, lba, count):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)

def write_sectors(f, lba, data):
    count = (len(data) + SECTOR - 1) // SECTOR
    padded = data.ljust(count * SECTOR, b'\x00')
    f.seek(lba * SECTOR)
    f.write(padded)

def pad_sector(data):
    """Pad data to a multiple of SECTOR bytes."""
    r = len(data) % SECTOR
    if r: data += b'\x00' * (SECTOR - r)
    return data

# ── FSTable I/O ───────────────────────────────────────────────────────────────

def read_fstable(f):
    """Read and parse the full FSTable from disk."""
    needed = FSTAB_HDR_SIZE + FS_MAX_NODES * NODE_SIZE
    sector_count = (needed + SECTOR - 1) // SECTOR
    raw = read_sectors(f, FS_TABLE_LBA, sector_count)

    # Header
    magic, node_count, version, volume_name, fs_reserved = \
        struct.unpack_from(FSTAB_HDR_FMT, raw, 0)

    if magic != FS_MAGIC:
        raise ValueError(
            f"FS magic mismatch at LBA {FS_TABLE_LBA}: "
            f"got 0x{magic:08X}, expected 0x{FS_MAGIC:08X}\n"
            "  → Le FS n'est pas encore formaté ou l'os.img est incorrect."
        )
    if version != RAY64_VERSION:
        raise ValueError(f"FS version mismatch: got {version}, expected {RAY64_VERSION}")

    nodes = []
    offset = FSTAB_HDR_SIZE
    for _ in range(FS_MAX_NODES):
        nodes.append(raw[offset:offset + NODE_SIZE])
        offset += NODE_SIZE

    return {
        "magic":       magic,
        "node_count":  node_count,
        "version":     version,
        "volume_name": volume_name,
        "fs_reserved": fs_reserved,
        "nodes":       nodes,   # list of raw bytes[NODE_SIZE]
        "_raw":        raw,
    }

def write_fstable(f, table):
    """Serialize and write the FSTable back to disk (only sectors that changed)."""
    hdr = struct.pack(
        FSTAB_HDR_FMT,
        table["magic"],
        table["node_count"],
        table["version"],
        table["volume_name"],
        table["fs_reserved"],
    )
    blob = hdr + b''.join(table["nodes"])
    blob = pad_sector(blob)

    old = table["_raw"]

    # Write only sectors that changed
    written = 0
    for i in range(len(blob) // SECTOR):
        new_sec = blob[i*SECTOR:(i+1)*SECTOR]
        old_sec = old[i*SECTOR:(i+1)*SECTOR] if i*SECTOR < len(old) else b'\x00'*SECTOR
        if new_sec != old_sec:
            write_sector(f, FS_TABLE_LBA + i, new_sec)
            written += 1

    print(f"  FSTable : {written} secteur(s) mis à jour sur {len(blob)//SECTOR}")

# ── FSNode helpers ────────────────────────────────────────────────────────────

def parse_node(raw):
    """Parse raw bytes into a node dict."""
    name_b, is_dir, _pad, parent, *rest = struct.unpack(NODE_FMT, raw)
    children = rest[:FS_MAX_CHILDREN]
    child_count, data_start_lba, size_bytes, magic, meta_b = rest[FS_MAX_CHILDREN:]
    name = name_b.rstrip(b'\x00').decode('latin-1', errors='replace')
    return {
        "name":           name,
        "name_b":         name_b,
        "is_dir":         is_dir,
        "_pad":           _pad,
        "parent":         parent,
        "children":       list(children),
        "child_count":    child_count,
        "data_start_lba": data_start_lba,
        "size_bytes":     size_bytes,
        "magic":          magic,
        "meta_b":         meta_b,
    }

def pack_node(n):
    """Serialize a node dict back to bytes."""
    return struct.pack(
        NODE_FMT,
        n["name_b"],
        n["is_dir"],
        n["_pad"],
        n["parent"],
        *n["children"],
        n["child_count"],
        n["data_start_lba"],
        n["size_bytes"],
        n["magic"],
        n["meta_b"],
    )

def find_node(table, filename):
    """Return (index, parsed_node) for the first node matching filename, or (-1, None)."""
    for i, raw in enumerate(table["nodes"]):
        n = parse_node(raw)
        if n["magic"] == FS_MAGIC and not n["is_dir"] and n["name"] == filename:
            return i, n
    return -1, None

def find_root(table):
    """Return index of root node (parent == itself, is_dir == 1)."""
    for i, raw in enumerate(table["nodes"]):
        n = parse_node(raw)
        if n["magic"] == FS_MAGIC and n["is_dir"] and n["parent"] == 0:
            return i, n
    return -1, None

# ── LBA allocation ────────────────────────────────────────────────────────────

def next_free_lba(table):
    """Compute the next free data LBA (same logic as fs_next_free_lba in C)."""
    max_end = FS_DATA_BASE_LBA
    for raw in table["nodes"]:
        n = parse_node(raw)
        if n["magic"] == FS_MAGIC and not n["is_dir"] and n["data_start_lba"] > 0:
            sectors = (n["size_bytes"] + SECTOR - 1) // SECTOR
            end = n["data_start_lba"] + sectors + 1  # +1 for header sector
            if end > max_end:
                max_end = end
    return max_end

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    img_path = sys.argv[1] if len(sys.argv) > 1 else "os.img"
    wp_path  = sys.argv[2] if len(sys.argv) > 2 else "kernel/gfx/wallpaper.bin"

    if not os.path.exists(img_path):
        print(f"ERREUR : '{img_path}' introuvable.")
        sys.exit(1)
    if not os.path.exists(wp_path):
        print(f"ERREUR : '{wp_path}' introuvable.")
        sys.exit(1)

    with open(wp_path, "rb") as wf:
        wp_data = wf.read()
    wp_size = len(wp_data)
    wp_sectors = (wp_size + SECTOR - 1) // SECTOR

    print(f"=== inject_wallpaper.py ===")
    print(f"Image disque : {img_path}  ({os.path.getsize(img_path)//1024//1024} MB)")
    print(f"Wallpaper    : {wp_path}  ({wp_size//1024} KB, {wp_sectors} secteurs de données)")

    with open(img_path, "r+b") as f:

        # 1. Lire le FS
        print("\n[1/4] Lecture de la FSTable...")
        try:
            table = read_fstable(f)
        except ValueError as e:
            print(f"\nERREUR : {e}")
            sys.exit(1)

        print(f"  FS magic OK, {table['node_count']} nœud(s) connus")

        # 2. Chercher ou créer le nœud wallpaper.bin
        print(f"\n[2/4] Recherche du fichier '{WALLPAPER_FILENAME}'...")
        node_idx, node = find_node(table, WALLPAPER_FILENAME)

        if node_idx >= 0:
            print(f"  Trouvé : nœud #{node_idx}, LBA data = {node['data_start_lba']}")
            data_lba = node["data_start_lba"]
            if data_lba == 0:
                data_lba = next_free_lba(table)
                print(f"  LBA était 0, alloué : {data_lba}")
        else:
            print(f"  Pas trouvé → création d'un nouveau nœud")
            # Trouver le nœud root pour y rattacher le fichier
            root_idx, root_node = find_root(table)
            if root_idx < 0:
                print("ERREUR : nœud root introuvable dans le FS.")
                sys.exit(1)

            data_lba = next_free_lba(table)
            print(f"  LBA alloué : {data_lba}")

            # Créer le nœud
            name_b = WALLPAPER_FILENAME.encode('latin-1').ljust(FS_NAME_LEN, b'\x00')[:FS_NAME_LEN]
            children = [0] * FS_MAX_CHILDREN
            meta_b   = b'\x00' * RAY64_META_SIZE

            node = {
                "name":           WALLPAPER_FILENAME,
                "name_b":         name_b,
                "is_dir":         0,
                "_pad":           b'\x00\x00\x00',
                "parent":         root_idx,
                "children":       children,
                "child_count":    0,
                "data_start_lba": data_lba,
                "size_bytes":     wp_size,
                "magic":          FS_MAGIC,
                "meta_b":         meta_b,
            }

            # Ajouter dans les slots libres
            inserted = False
            for i in range(FS_MAX_NODES):
                existing = parse_node(table["nodes"][i])
                if existing["magic"] != FS_MAGIC:
                    node_idx = i
                    table["nodes"][i] = pack_node(node)
                    inserted = True
                    # Mise à jour node_count si nécessaire
                    if i >= table["node_count"]:
                        table["node_count"] = i + 1
                    break

            if not inserted:
                print("ERREUR : FS plein (256 nœuds), impossible d'ajouter wallpaper.bin")
                sys.exit(1)

            # Rattacher au root
            if root_node["child_count"] < FS_MAX_CHILDREN:
                slot = root_node["child_count"]
                root_node["children"][slot] = node_idx
                root_node["child_count"] += 1
                table["nodes"][root_idx] = pack_node(root_node)
                print(f"  Rattaché au nœud root (nœud #{root_idx}), slot enfant {slot}")
            else:
                print("AVERTISSEMENT : root a déjà 16 enfants, nœud créé mais non rattaché au root.")
                print("  Le fichier sera quand même accessible par nom depuis le répertoire courant du boot.")

        # Mettre à jour la taille dans le nœud (au cas où elle aurait changé)
        node["data_start_lba"] = data_lba
        node["size_bytes"]     = wp_size
        table["nodes"][node_idx] = pack_node(node)

        # 3. Écrire les données wallpaper sur le disque
        print(f"\n[3/4] Écriture du wallpaper (LBA {data_lba} → {data_lba + wp_sectors})...")

        # FileHeader (secteur LBA data_lba)
        fh = struct.pack(FH_FMT, FILE_MAGIC, 0, wp_size, b'\x00' * 500)
        write_sector(f, data_lba, fh)

        # Données brutes (secteurs data_lba+1 …)
        padded = wp_data + b'\x00' * (wp_sectors * SECTOR - wp_size)
        for s in range(wp_sectors):
            write_sector(f, data_lba + 1 + s, padded[s*SECTOR:(s+1)*SECTOR])

        print(f"  {wp_size} bytes écrits ({wp_sectors} secteurs de données + 1 header)")

        # 4. Réécrire la FSTable (uniquement les secteurs modifiés)
        print(f"\n[4/4] Mise à jour de la FSTable (LBA {FS_TABLE_LBA})...")
        write_fstable(f, table)

        f.flush()

    print(f"\n✅ wallpaper.bin injecté avec succès dans {img_path}")
    print(f"   Nœud #{node_idx}, LBA data = {data_lba}, taille = {wp_size} bytes")
    print(f"   Le kernel peut le lire via : fs_read_file(\"wallpaper.bin\", buf, size)")

if __name__ == "__main__":
    main()