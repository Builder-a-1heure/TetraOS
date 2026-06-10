#!/usr/bin/env python3
# =============================================================================
#  generate_wallpaper.py — Convertit n'importe quelle image en wallpaper.bin
#
#  Format de sortie : RGB24 flat brut, 1920×1080, row-major
#    R G B R G B ... (3 bytes par pixel, pas de header, pas de padding)
#    Taille exacte : 1920 * 1080 * 3 = 6 220 800 bytes
#
#  Usage :
#    python3 tools/scripts/generate_wallpaper.py <image.jpg|png|...>
#    python3 tools/scripts/generate_wallpaper.py <image> <sortie.bin>
#
#  Ensuite :
#    bash tools/scripts/make.sh --no-run    ← injecte dans os.img via inject_wallpaper.py
# =============================================================================

import sys, os

try:
    from PIL import Image
except ImportError:
    print("Pillow manquant — installe : pip3 install Pillow")
    sys.exit(1)

# Résolution cible — DOIT correspondre à WALLPAPER_W / WALLPAPER_H dans wallpaper.h
TARGET_W = 1920
TARGET_H = 1080
EXPECTED_SIZE = TARGET_W * TARGET_H * 3  # 6 220 800 bytes

def convert(src_path, out_path="kernel/gfx/wallpaper.bin"):
    if not os.path.exists(src_path):
        print(f"ERREUR : '{src_path}' introuvable.")
        sys.exit(1)

    img = Image.open(src_path)
    orig_mode = img.mode
    orig_size = img.size
    print(f"Source : {src_path}  ({orig_size[0]}×{orig_size[1]}, {orig_mode})")

    # ── Normalisation du mode en RGB ──────────────────────────────────────────
    # Il faut absolument passer en RGB AVANT tobytes(), sinon :
    #   CMYK  → 4 bytes/pixel  → binaire corrompu côté kernel
    #   RGBA  → 4 bytes/pixel
    #   P     → 1 byte/pixel (palette)
    #   YCbCr → channel Y seulement sans conversion
    if img.mode == 'CMYK':
        img = img.convert('RGB')
    elif img.mode == 'P':
        img = img.convert('RGBA').convert('RGB')
    elif img.mode in ('RGBA', 'LA', 'PA'):
        # Composer sur fond noir pour les images avec canal alpha
        bg = Image.new('RGB', img.size, (0, 0, 0))
        if img.mode != 'RGBA':
            img = img.convert('RGBA')
        bg.paste(img, mask=img.split()[3])
        img = bg
    elif img.mode != 'RGB':
        img = img.convert('RGB')

    assert img.mode == 'RGB', f"Conversion RGB échouée : mode={img.mode}"

    # ── Resize + crop centré pour remplir exactement TARGET_W × TARGET_H ─────
    orig_w, orig_h = img.size
    ratio = max(TARGET_W / orig_w, TARGET_H / orig_h)
    new_w = max(int(orig_w * ratio), TARGET_W)
    new_h = max(int(orig_h * ratio), TARGET_H)

    img  = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - TARGET_W) // 2
    top  = (new_h - TARGET_H) // 2
    img  = img.crop((left, top, left + TARGET_W, top + TARGET_H))

    assert img.size == (TARGET_W, TARGET_H), \
        f"Taille incorrecte après crop : {img.size}"
    assert img.mode == 'RGB'

    # ── Export RGB24 brut (3 bytes/pixel, aucun header) ───────────────────────
    raw = img.tobytes()   # garantit R G B R G B ... en mode RGB
    assert len(raw) == EXPECTED_SIZE, \
        f"Taille binaire incorrecte : {len(raw)} (attendu {EXPECTED_SIZE})"

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(raw)

    print(f"Écrit  : {out_path}")
    print(f"  Taille  : {len(raw)} bytes  ({TARGET_W}×{TARGET_H} RGB24 flat)")
    print(f"  Mode    : {orig_mode} → RGB")
    print(f"  Ratio   : ×{ratio:.4f}, crop ({left},{top})")
    print()
    print("Étape suivante :")
    print("  bash tools/scripts/make.sh --no-run    ← injecte dans os.img")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage : python3 {sys.argv[0]} <image.jpg|png|...> [sortie.bin]")
        print(f"  Résolution de sortie : {TARGET_W}×{TARGET_H} RGB24 flat ({EXPECTED_SIZE} bytes)")
        sys.exit(1)
    out = sys.argv[2] if len(sys.argv) > 2 else "kernel/gfx/wallpaper.bin"
    convert(sys.argv[1], out)
