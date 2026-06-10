// apps/app.h — Format TEX (TetraEXecutable)
//
// Une app se déclare sur le bureau en UNE SEULE LIGNE dans son .c :
//
//   TEX_APP("TextEdit", APPICON_TEXTEDIT, 1, 0, APP_FLAG_DESKTOP, app_textedit);
//   void app_textedit(void) { ... }
//
// Le kernel scanne [_kernel_start, _kernel_end[, cherche 0x54455800,
// lit chaque TexHeader valide et construit la liste d'icônes.
// desktop.c ne connaît aucune app à la compilation.
//
// ── Format binaire (32 octets, packed) ───────────────────────────────────
//
//   Offset  Taille  Champ
//   0x00    4       magic      — 0x54455800 ("TEX\0")
//   0x04    16      name       — affiché sous l'icône, '\0'-terminé
//   0x14    1       icon_type  — AppIconType
//   0x15    1       ver_major
//   0x16    1       ver_minor
//   0x17    1       flags      — APP_FLAG_*
//   0x18    4       entry      — pointeur vers void fn(void)
//   0x1C    4       reserved   — futur : hash / signature crypto
//
// ── Crypto (futur) ───────────────────────────────────────────────────────
//
//   Le champ reserved accueillera plus tard un hash SHA-256 tronqué
//   ou une signature ECDSA. Les apps APP_FLAG_SYSTEM pourront exiger
//   une vérification avant lancement. La fonction d'entrée (entry)
//   sera le site canonique de la signature — son adresse est connue
//   statiquement au link, ce qui la rend signable indépendamment.

#ifndef APP_H
#define APP_H

#include <stdint.h>

// ============================================================
// Magic — "TEX\0"
// ============================================================
#define TEX_MAGIC  0x54455800u

// ============================================================
// Flags
// ============================================================
#define APP_FLAG_DESKTOP  (1 << 0)  // icône visible sur le bureau
#define APP_FLAG_SYSTEM   (1 << 1)  // app système (future vérif. signature)
#define APP_FLAG_HIDDEN   (1 << 2)  // shell uniquement, pas d'icône

// ============================================================
// Types d'icônes
// ============================================================
typedef enum {
    APPICON_TERMINAL = 0,
    APPICON_TEXTEDIT,
    APPICON_FILEMAN,
    APPICON_SETTINGS,
    APPICON_FILEEDITOR,
    APPICON_GENERIC
} AppIconType;

// ============================================================
// Structure TexHeader — 32 octets
// ============================================================
typedef void (*TexEntryFn)(void);

typedef struct __attribute__((packed)) {
    uint32_t   magic;       // TEX_MAGIC
    char       name[16];    // nom affiché
    uint8_t    icon_type;   // AppIconType
    uint8_t    ver_major;
    uint8_t    ver_minor;
    uint8_t    flags;       // APP_FLAG_*
    TexEntryFn entry;       // point d'entrée — résolu au link
    uint32_t   reserved;    // futur : hash / signature
} TexHeader;                // sizeof == 32

// ============================================================
// Macro TEX_APP
// ============================================================
// Génère un TexHeader statique en .rodata (dans [_kernel_start, _kernel_end[).
// Le forward-declare de fn_ permet de placer la macro AVANT la définition
// de la fonction dans le .c, ce qui garantit que le header apparaît en
// mémoire AVANT le corps de la fonction — pratique pour le scan.
//
// __used__   : empêche GCC d'éliminer la variable (personne n'y fait référence).
// aligned(4) : le scanner lit des uint32_t — alignement requis.

#define TEX_APP(name_, icon_, vmaj_, vmin_, flags_, fn_)            \
    void fn_(void);                                                 \
    static const TexHeader __tex_##fn_                              \
    __attribute__((section(".rodata"), used, aligned(4))) = {       \
        .magic     = TEX_MAGIC,                                     \
        .name      = name_,                                         \
        .icon_type = (uint8_t)(icon_),                              \
        .ver_major = (uint8_t)(vmaj_),                              \
        .ver_minor = (uint8_t)(vmin_),                              \
        .flags     = (uint8_t)(flags_),                             \
        .entry     = fn_,                                           \
        .reserved  = 0                                              \
    }

#endif // APP_H