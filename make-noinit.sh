#!/bin/bash
# ============================================================
#  TetraOS — Script de build automatique (macOS / Apple Silicon)
#
#  Détecte automatiquement tous les .c dans kernel/.
#  Seule contrainte fixe : main.c doit être linkée EN PREMIER.
#
#  Prérequis :
#    brew install i686-elf-gcc i686-elf-binutils nasm qemu
#
#  Usage :
#    chmod +x make.sh
#    ./make.sh             → compile + lance QEMU
#    ./make.sh --no-run    → compile seulement
#    ./make.sh --verbose   → affiche chaque fichier trouvé
# ============================================================

set -e

# ============================================================
# CONFIGURATION
# ============================================================
GCC="i686-elf-gcc"
LD="i686-elf-ld"
OBJCOPY="i686-elf-objcopy"
NASM="nasm"
QEMU="qemu-system-i386"

KERNEL_DIR="kernel"
OBJ="kernel/compilation"
ENTRY_SRC="kernel/shell/main.c"   # doit être linkée EN PREMIER

# Fichier de liste transmis au linker (évite les problèmes de tableaux shell)
OBJ_LIST="$OBJ/link_objects.txt"

CFLAGS="-ffreestanding -Wall -Wextra -nostdlib -g \
    -Ikernel/drivers \
    -Ikernel/gfx \
    -Ikernel/mem \
    -Ikernel/fs \
    -Ikernel/ui \
    -Ikernel/shell \
    -Ikernel/lib \
    -Ikernel/apps \
    -Ikernel"

# Dossiers exclus de la détection automatique
EXCLUDE_DIRS="$KERNEL_DIR/boot $KERNEL_DIR/compilation"

# Options
VERBOSE=0; NO_RUN=0
for arg in "$@"; do
    [[ "$arg" == "--verbose" ]] && VERBOSE=1
    [[ "$arg" == "--no-run"  ]] && NO_RUN=1
done

# ============================================================
# COULEURS
# ============================================================
RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[0;33m'
BLU='\033[0;34m'; CYN='\033[0;36m'; RST='\033[0m'
info() { echo -e "${BLU}$*${RST}"; }
ok()   { echo -e "${GRN}$*${RST}"; }
warn() { echo -e "${YEL}$*${RST}"; }
err()  { echo -e "${RED}$*${RST}"; }

# ============================================================
# [0] VÉRIFICATION DES OUTILS
# ============================================================
info "==> Vérification des outils..."
missing=0
for tool in "$GCC" "$LD" "$OBJCOPY" "$NASM" "$QEMU" python3; do
    if ! command -v "$tool" &>/dev/null; then
        err "  ERREUR : '$tool' introuvable."; missing=1
    fi
done
if [[ $missing -eq 1 ]]; then
    warn "  Installe : brew install i686-elf-gcc i686-elf-binutils nasm qemu"
    exit 1
fi
ok "    Tous les outils sont disponibles."

# ============================================================
# [1] NETTOYAGE
# ============================================================
echo ""
info "[1/7] Nettoyage..."
rm -rf "$OBJ"
rm -f bootloader.bin stage2.bin kernel.bin kernel.elf kernel.map os.img

# ============================================================
# [2] CRÉATION DU DOSSIER COMPILATION
# ============================================================
info "[2/7] Création du dossier $OBJ..."
mkdir -p "$OBJ"

# ============================================================
# [3] ASSEMBLAGE BOOTLOADER
# ============================================================
info "[3/7] Assemblage bootloader (Stage 1)..."
$NASM -f bin "$KERNEL_DIR/boot/bootloader.asm" -o bootloader.bin
info "[3/7] Assemblage stage2..."
$NASM -f bin "$KERNEL_DIR/boot/stage2.asm" -o stage2.bin

# ============================================================
# [4] DÉTECTION ET COMPILATION AUTOMATIQUE DES .c
# ============================================================
info "[4/7] Détection des sources C dans $KERNEL_DIR/..."

# Arguments -prune pour exclure les dossiers indésirables
PRUNE_ARGS=()
for excl in $EXCLUDE_DIRS; do
    PRUNE_ARGS+=(-path "$excl" -prune -o)
done

# Collecter tous les .c (triés pour reproductibilité)
ALL_C=()
while IFS= read -r f; do
    ALL_C+=("$f")
done < <(find "$KERNEL_DIR" "${PRUNE_ARGS[@]}" -name "*.c" -print | sort)

# Le fichier de liste pour le linker :
# - première ligne = ENTRY_OBJ (main.o)
# - lignes suivantes = les autres .o
# On l'écrit en deux passes pour garantir l'ordre.

ENTRY_OBJ=""
: > "$OBJ_LIST.rest"   # fichier temporaire pour les autres .o
compiled_count=0

echo ""
echo "  Sources trouvées :"

for src in "${ALL_C[@]}"; do
    # Nom .o plat : kernel/foo/bar.c → kernel_foo_bar.o
    base="${src//\//_}"
    base="${base%.c}.o"
    obj="$OBJ/$base"

    if [[ $VERBOSE -eq 1 ]]; then
        echo -e "  ${CYN}→${RST} $src"
    else
        echo "  $src"
    fi

    # Compilation (stderr → terminal, stdout ignorée)
    $GCC $CFLAGS -c "$src" -o "$obj" 2>&1 | cat
    compiled_count=$((compiled_count + 1))

    if [[ "$src" == "$ENTRY_SRC" ]]; then
        ENTRY_OBJ="$obj"
    else
        echo "$obj" >> "$OBJ_LIST.rest"
    fi
done

echo ""
ok "  → $compiled_count fichier(s) compilé(s)."

if [[ -z "$ENTRY_OBJ" ]]; then
    err "ERREUR : point d'entrée '$ENTRY_SRC' introuvable !"
    exit 1
fi

# Construire le fichier de liste final : entry en premier, puis le reste
echo "$ENTRY_OBJ"  >  "$OBJ_LIST"
cat "$OBJ_LIST.rest" >> "$OBJ_LIST"
rm -f "$OBJ_LIST.rest"

if [[ $VERBOSE -eq 1 ]]; then
    echo ""
    echo "  Ordre de linkage :"
    cat "$OBJ_LIST" | while read -r line; do echo "    $line"; done
fi

# ============================================================
# [5] LINKAGE
# ============================================================
info "[5/7] Linkage du kernel..."

# Lire le fichier de liste ligne par ligne → tableau propre sans pollution
OBJ_ARGS=()
while IFS= read -r line; do
    [[ -n "$line" ]] && OBJ_ARGS+=("$line")
done < "$OBJ_LIST"

$LD -T "$KERNEL_DIR/boot/linker.ld" -o kernel.elf -Map kernel.map \
    "${OBJ_ARGS[@]}"

info "[5/7] Extraction du binaire kernel..."
$OBJCOPY -O binary kernel.elf kernel.bin

# ============================================================
# [6] CRÉATION IMAGE DISQUE
# ============================================================
info "[6/7] Création de os.img (16 Mo)..."
dd if=/dev/zero of=os.img bs=1048576 count=16 2>/dev/null

info "[6/7] Ecriture bootloader  (LBA 0)..."
python3 write_lba.py os.img bootloader.bin 0
info "[6/7] Ecriture stage2      (LBA 1)..."
python3 write_lba.py os.img stage2.bin 1
info "[6/7] Ecriture kernel      (LBA 3)..."
python3 write_lba.py os.img kernel.bin 3

echo ""; ok "Build OK !"