#!/bin/bash
# ============================================================
#  TetraOS — Script de build (macOS / Apple Silicon M-series)
#  Converti depuis build.bat
#
#  Prérequis (installer via Homebrew) :
#    brew install i686-elf-gcc i686-elf-binutils nasm qemu
#
#  Usage :
#    chmod +x build.sh
#    ./build.sh          → compile + lance QEMU
#    ./build.sh --no-run → compile seulement
# ============================================================

set -e  # Arrêt immédiat en cas d'erreur

# === CONFIGURATION ===
GCC="i686-elf-gcc"
LD="i686-elf-ld"
OBJCOPY="i686-elf-objcopy"
NASM="nasm"
QEMU="qemu-system-i386"

OBJ="kernel/compilation"

CFLAGS="-ffreestanding -Wall -Wextra -nostdlib -g \
    -Ikernel/drivers \
    -Ikernel/gfx \
    -Ikernel/mem \
    -Ikernel/fs \
    -Ikernel/ui \
    -Ikernel/shell \
    -Ikernel/lib \
    -Ikernel"

# === VÉRIFICATION DES OUTILS ===
echo "==> Vérification des outils..."
for tool in "$GCC" "$LD" "$OBJCOPY" "$NASM" "$QEMU" python3; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERREUR : '$tool' introuvable."
        echo ""
        echo "  Installe les outils manquants :"
        echo "    brew install i686-elf-gcc i686-elf-binutils nasm qemu"
        exit 1
    fi
done
echo "    Tous les outils sont disponibles."

# === [1/7] NETTOYAGE ===
echo ""
echo "[1/7] Nettoyage..."
rm -rf "$OBJ"
rm -f bootloader.bin stage2.bin kernel.bin kernel.elf kernel.map os.img

# === [2/7] CRÉATION DU DOSSIER COMPILATION ===
echo "[2/7] Création du dossier $OBJ..."
mkdir -p "$OBJ"

# === [3/7] ASSEMBLAGE BOOTLOADER (Stage 1 — MBR) ===
echo "[3/7] Assemblage bootloader (Stage 1)..."
$NASM -f bin kernel/boot/bootloader.asm -o bootloader.bin

echo "[3/7] Assemblage stage2..."
$NASM -f bin kernel/boot/stage2.asm -o stage2.bin

# === [4/7] COMPILATION DU KERNEL ===
echo "[4/7] Compilation des sources C..."

compile() {
    echo "  $1"
    $GCC $CFLAGS -c "$1" -o "$2"
}

# shell/
compile kernel/shell/main.c      "$OBJ/main.o"
compile kernel/shell/shell.c     "$OBJ/shell.o"
compile kernel/shell/terminal.c  "$OBJ/terminal.o"
compile kernel/shell/editor.c    "$OBJ/editor.o"
compile kernel/shell/tex.c       "$OBJ/tex.o"

# ui/
compile kernel/ui/desktop.c      "$OBJ/desktop.o"
compile kernel/ui/session.c      "$OBJ/session.o"

# fs/
compile kernel/fs/fs.c           "$OBJ/fs.o"

# gfx/
compile kernel/gfx/screen.c      "$OBJ/screen.o"
compile kernel/gfx/vesaanim.c    "$OBJ/vesaanim.o"

# drivers/
compile kernel/drivers/vesa.c    "$OBJ/vesa.o"
compile kernel/drivers/mouse.c   "$OBJ/mouse.o"
compile kernel/drivers/ata.c     "$OBJ/ata.o"
compile kernel/drivers/input.c   "$OBJ/input.o"

# mem/
compile kernel/mem/mem_boot.c    "$OBJ/mem_boot.o"
compile kernel/mem/pfa.c         "$OBJ/pfa.o"

# lib/
compile kernel/lib/utils.c       "$OBJ/utils.o"
compile kernel/lib/boot_info.c   "$OBJ/boot_info.o"

# === [5/7] LINKAGE ===
echo "[5/7] Linkage du kernel..."
$LD -T kernel/boot/linker.ld -o kernel.elf -Map kernel.map \
    "$OBJ/main.o"      \
    "$OBJ/shell.o"     \
    "$OBJ/terminal.o"  \
    "$OBJ/editor.o"    \
    "$OBJ/tex.o"       \
    "$OBJ/desktop.o"   \
    "$OBJ/session.o"   \
    "$OBJ/fs.o"        \
    "$OBJ/screen.o"    \
    "$OBJ/vesaanim.o"  \
    "$OBJ/vesa.o"      \
    "$OBJ/mouse.o"     \
    "$OBJ/ata.o"       \
    "$OBJ/input.o"     \
    "$OBJ/mem_boot.o"  \
    "$OBJ/pfa.o"       \
    "$OBJ/utils.o"     \
    "$OBJ/boot_info.o"

echo "[5/7] Extraction du binaire kernel..."
$OBJCOPY -O binary kernel.elf kernel.bin

# === [6/7] CRÉATION IMAGE DISQUE ===
echo "[6/7] Création de os.img (16 Mo)..."
# Utilise dd sur macOS (fsutil n'existe pas)
dd if=/dev/zero of=os.img bs=1048576 count=16 2>/dev/null

echo "[6/7] Ecriture bootloader  (LBA 0)..."
python3 write_lba.py os.img bootloader.bin 0

echo "[6/7] Ecriture stage2      (LBA 1)..."
python3 write_lba.py os.img stage2.bin 1

echo "[6/7] Ecriture kernel      (LBA 3)..."
python3 write_lba.py os.img kernel.bin 3

# === [7/7] LANCEMENT QEMU ===
if [[ "$1" == "--no-run" ]]; then
    echo ""
    echo "Build OK ! (QEMU non lancé)"
    exit 0
fi

echo "[7/7] Lancement de QEMU..."
$QEMU \
    -drive format=raw,file=os.img \
    -m 64M \
    -display cocoa \
    -vga std \
    -global VGA.vgamem_mb=32 \
    -d guest_errors

echo ""
echo "Build OK !"