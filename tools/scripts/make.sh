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
#    ./make.sh --fresh     → force la recréation de os.img (efface le FS)
#
#  Comportement sur os.img :
#    - Si os.img EXISTE et --fresh n'est pas passé :
#        → seuls bootloader, stage2 et kernel.bin sont réécrits (LBA 0-1, 3+)
#        → le FS (LBA 5120+) est CONSERVÉ intact
#        → le wallpaper est re-injecté dans le FS via inject_wallpaper.py
#    - Si os.img N'EXISTE PAS ou --fresh est passé :
#        → création d'un nouvel os.img vierge de 64 Mo
#        → le FS sera formaté au premier boot par le kernel
#        → wallpaper injecté automatiquement au build suivant (après le boot)
# ============================================================

set -e

# ============================================================
# RACINE DU PROJET
# Se déplacer dans la racine du projet (deux niveaux au-dessus
# de tools/scripts/) quel que soit le répertoire courant.
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"
echo "==> Racine projet : $PROJECT_ROOT"

# ============================================================
# CONFIGURATION
# ============================================================
GCC="i686-elf-gcc"
LD="i686-elf-ld"
OBJCOPY="i686-elf-objcopy"
NASM="nasm"
QEMU="qemu-system-i386"

KERNEL_DIR="kernel"
OS_DIR="os"
OBJ="tools/build/compilation"
ENTRY_SRC="os/shell/main.c"   # doit être linkée EN PREMIER

# Sous-dossiers à compiler — LISTE EXPLICITE.
# Le find ne sort jamais de ces dossiers, donc les vieux fichiers
# qui traîneraient ailleurs (ex: kernel/apps/, kernel/shell/ d'une
# ancienne arborescence) ne sont jamais compilés deux fois.
COMPILE_DIRS=(
    "kernel/drivers"
    "kernel/fs"
    "kernel/gfx"
    "kernel/lib"
    "kernel/mem"
    "os/shell"
    "os/ui"
    "os/apps"
)

# Secteurs kernel — le wallpaper est maintenant dans le FS, pas dans le kernel.
# ~500 KB de marge suffisent amplement pour le code seul.
KERNEL_SECTORS=1200

# Fichier de liste transmis au linker
OBJ_LIST="$OBJ/link_objects.txt"

CFLAGS="-ffreestanding -O2 -Wall -Wextra -nostdlib -g \
    -Ikernel/drivers \
    -Ikernel/gfx \
    -Ikernel/mem \
    -Ikernel/fs \
    -Ios/ui \
    -Ios/shell \
    -Ikernel/lib \
    -Ios/apps \
    -Ikernel"

VERBOSE=0; NO_RUN=0; FRESH=0
for arg in "$@"; do
    [[ "$arg" == "--verbose" ]] && VERBOSE=1
    [[ "$arg" == "--no-run"  ]] && NO_RUN=1
    [[ "$arg" == "--fresh"   ]] && FRESH=1
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
# [1] NETTOYAGE (compilation seulement — os.img préservé)
# ============================================================
echo ""
info "[1/8] Nettoyage des fichiers de compilation..."
rm -rf "$OBJ"
rm -f bootloader.bin stage2.bin kernel.bin kernel.elf kernel.map

# ============================================================
# [2] CRÉATION DU DOSSIER COMPILATION
# ============================================================
info "[2/8] Création du dossier $OBJ..."
mkdir -p "$OBJ"

# ============================================================
# [3] ASSEMBLAGE BOOTLOADER
# ============================================================
info "[3/8] Assemblage bootloader (Stage 1)..."
$NASM -f bin "$KERNEL_DIR/boot/bootloader.asm" -o bootloader.bin
info "[3/8] Assemblage stage2 (KERNEL_SECTORS=$KERNEL_SECTORS)..."
$NASM -f bin -D KERNEL_SECTORS=$KERNEL_SECTORS "$KERNEL_DIR/boot/stage2.asm" -o stage2.bin

# ============================================================
# [4] DÉTECTION ET COMPILATION AUTOMATIQUE DES .c
# ============================================================
info "[4/8] Détection des sources C dans $KERNEL_DIR/..."

# Collecter les .c uniquement dans les dossiers listés dans COMPILE_DIRS.
# Chaque dossier est scanné récursivement mais indépendamment → pas de
# risque de ramasser des fichiers hors périmètre (ancienne arborescence,
# dossiers temporaires, etc.)
ALL_C=()
for dir in "${COMPILE_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        while IFS= read -r f; do
            ALL_C+=("$f")
        done < <(find "$dir" -name "*.c" -print | sort)
    else
        warn "  ATTENTION : dossier '$dir' introuvable, ignoré."
    fi
done

ENTRY_OBJ=""
: > "$OBJ_LIST.rest"
compiled_count=0

echo ""
echo "  Sources trouvées :"

for src in "${ALL_C[@]}"; do
    base="${src//\//_}"
    base="${base%.c}.o"
    obj="$OBJ/$base"

    if [[ $VERBOSE -eq 1 ]]; then
        echo -e "  ${CYN}→${RST} $src"
    else
        echo "  $src"
    fi

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
info "[5/8] Linkage du kernel..."

OBJ_ARGS=()
while IFS= read -r line; do
    [[ -n "$line" ]] && OBJ_ARGS+=("$line")
done < "$OBJ_LIST"

$LD -T "$KERNEL_DIR/boot/linker.ld" -o kernel.elf -Map kernel.map \
    "${OBJ_ARGS[@]}"

info "[5/8] Extraction du binaire kernel..."
$OBJCOPY -O binary kernel.elf kernel.bin

KERNEL_SIZE=$(wc -c < kernel.bin)
KERNEL_SECTORS_USED=$(( (KERNEL_SIZE + 511) / 512 ))
ok "  kernel.bin : $((KERNEL_SIZE / 1024)) KB — $KERNEL_SECTORS_USED secteurs / $KERNEL_SECTORS alloués"

if [[ $KERNEL_SECTORS_USED -gt $KERNEL_SECTORS ]]; then
    err "ERREUR : kernel.bin ($KERNEL_SECTORS_USED secteurs) dépasse KERNEL_SECTORS ($KERNEL_SECTORS) !"
    err "  Augmente KERNEL_SECTORS dans make.sh."
    exit 1
fi

# ============================================================
# [6] CRÉATION ou MISE À JOUR de os.img
# ============================================================
echo ""
if [[ -f "os.img" && $FRESH -eq 0 ]]; then
    info "[6/8] os.img existant détecté → mise à jour partielle"
    warn "  Bootloader (LBA 0), stage2 (LBA 1) et kernel (LBA 3+) réécrits."
    warn "  FS (LBA 5120+) conservé intact."

    # Agrandir si nécessaire (ancienne image trop petite)
    CURRENT_SIZE=$(wc -c < os.img)
    NEEDED_SIZE=$((64 * 1024 * 1024))
    if [[ $CURRENT_SIZE -lt $NEEDED_SIZE ]]; then
        warn "  os.img trop petite ($((CURRENT_SIZE/1024/1024)) Mo) → extension à 64 Mo..."
        dd if=/dev/zero bs=1 count=1 seek=$((NEEDED_SIZE - 1)) \
           of=os.img conv=notrunc 2>/dev/null
        ok "  os.img étendu à 64 Mo."
    fi
else
    if [[ $FRESH -eq 1 ]]; then
        info "[6/8] --fresh : recréation complète de os.img (64 Mo, FS effacé)..."
        rm -f os.img.fs_ready   # forcer re-détection après reformatage
    else
        info "[6/8] Aucun os.img existant → création (64 Mo)..."
    fi
    dd if=/dev/zero of=os.img bs=1048576 count=64 2>/dev/null
fi

info "[6/8] Écriture bootloader  (LBA 0)..."
python3 tools/scripts/write_lba.py os.img bootloader.bin 0
info "[6/8] Écriture stage2      (LBA 1)..."
python3 tools/scripts/write_lba.py os.img stage2.bin 1
info "[6/8] Écriture kernel      (LBA 3)..."
python3 tools/scripts/write_lba.py os.img kernel.bin 3

# ============================================================
# [7] INJECTION DU WALLPAPER DANS LE FS
# ============================================================
echo ""
WALLPAPER_BIN="$KERNEL_DIR/gfx/wallpaper.bin"
INJECT_SCRIPT="tools/scripts/inject_wallpaper.py"

if [[ ! -f "$WALLPAPER_BIN" ]]; then
    warn "[7/8] $WALLPAPER_BIN introuvable — wallpaper ignoré."
    warn "  Lance : python3 generate_wallpaper.py <image.jpg>"
elif [[ ! -f "$INJECT_SCRIPT" ]]; then
    warn "[7/8] inject_wallpaper.py introuvable — wallpaper non injecté."
elif [[ ! -f "os.img.fs_ready" ]]; then
    # Première fois (os.img tout neuf ou après --fresh) :
    # le FS n'est pas encore formaté par le kernel → on ne peut pas injecter.
    info "[7/8] Premier boot nécessaire pour formater le FS."
    warn "  Lance QEMU maintenant, laisse le kernel démarrer une fois,"
    warn "  puis relance './make.sh --no-run' pour injecter le wallpaper."
    warn "  (Ce message n'apparaîtra plus ensuite.)"
else
    info "[7/8] Injection du wallpaper dans le FS de os.img..."
    if python3 "$INJECT_SCRIPT" os.img "$WALLPAPER_BIN"; then
        ok "  wallpaper.bin injecté avec succès dans le FS."
    else
        warn "  Injection échouée — le FS n'est peut-être pas encore formaté."
        warn "  Relance './make.sh --no-run' après un premier boot complet."
        rm -f os.img.fs_ready
    fi
fi

# ============================================================
# [8] LANCEMENT QEMU
# ============================================================

# Marquer le FS comme prêt AVANT de lancer QEMU (ou avant de sortir en --no-run)
# afin que le step [7] puisse injecter le wallpaper au prochain build.
touch os.img.fs_ready

if [[ $NO_RUN -eq 1 ]]; then
    echo ""; ok "Build OK ! (QEMU non lancé)"; exit 0
fi

info "[8/8] Lancement de QEMU..."

$QEMU \
    -drive format=raw,file=os.img \
    -m 64M \
    -display cocoa,zoom-to-fit=on \
    -vga std \
    -global VGA.vgamem_mb=32 \
    -d guest_errors

echo ""; ok "Build OK !"