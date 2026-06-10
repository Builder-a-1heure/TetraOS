# TetraOS

OS bare-metal x86 (i686) développé from scratch en C & ASM.

---

## Structure du projet

```
TetraOS/
├── kernel/          ← Couches bas niveau
│   ├── boot/        ← Bootloader (stage1 ASM, stage2 ASM, linker script)
│   ├── drivers/     ← Pilotes matériels (VGA, VESA, ATA, mouse, input)
│   ├── mem/         ← Gestion mémoire (boot allocator, PFA)
│   ├── fs/          ← Système de fichiers custom
│   ├── gfx/         ← Couche graphique bas niveau (screen, VESA anim, wallpaper)
│   └── lib/         ← Runtime partagé (utils, io, appcore, boot_info, global.h)
│
├── os/              ← Couches hautes
│   ├── shell/       ← Shell interactif + moteur TeX custom
│   ├── ui/          ← Session manager + Desktop (fenêtres, curseur)
│   └── apps/        ← Applications (fileeditor, fileman, textedit, terminal…)
│
├── tools/           ← Outillage
│   ├── scripts/     ← Scripts de build et d'init (make.sh, make.bat, init*.sh…)
│   │                   + scripts Python (generate_wallpaper.py, inject_wallpaper.py, write_lba.py)
│   ├── build/       ← Binaires de compilation (nasm, ndisasm, i686-elf-gcc)
│   └── assets/      ← Images source pour le wallpaper
│
├── os.img           ← Image disque principale (64 Mo)
├── os.img.fs_ready  ← Flag : FS déjà formaté par le kernel
└── TetraOSv1.6.img  ← Snapshot de release
```

---

## Build & Run

> Tous les scripts se lancent depuis la **racine** du projet.

### Prérequis (macOS)

```bash
brew install i686-elf-gcc i686-elf-binutils nasm qemu
```

### Commandes

```bash
# Compiler et lancer dans QEMU
bash tools/scripts/make.sh

# Compiler seulement (sans lancer QEMU)
bash tools/scripts/make.sh --no-run

# Forcer la recréation de os.img (efface le FS)
bash tools/scripts/make.sh --fresh

# Afficher le détail des fichiers compilés
bash tools/scripts/make.sh --verbose
```

### Première utilisation

1. `bash tools/scripts/make.sh`  → crée os.img et lance QEMU
2. Laisser le kernel démarrer une fois (il formate le FS)
3. Quitter QEMU, puis `bash tools/scripts/make.sh --no-run` → injecte le wallpaper

---

## Wallpaper

```bash
# Générer wallpaper.bin depuis une image JPEG/PNG
python3 tools/scripts/generate_wallpaper.py tools/assets/background.jpg

# L'injecter manuellement dans os.img
python3 tools/scripts/inject_wallpaper.py os.img kernel/gfx/wallpaper.bin
```
