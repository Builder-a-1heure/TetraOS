// vesa_anim.c - Animation de chargement circulaire style Windows
#include "../gfx/vesaanim.h"
#include "../drivers/vesa.h"
#include "../drivers/vesa_font.h"
#include <stdint.h>

// ============================================================
// Couleurs — même palette que l'UI session
// ============================================================
#define ANIM_BG_TOP    0x00000D1A
#define ANIM_BG_BOT    0x00000000
#define ANIM_ACCENT    0x000066CC
#define ANIM_ACCENT2   0x000033AA
#define ANIM_WHITE     0x00FFFFFF
#define ANIM_GRAY      0x00555566
#define ANIM_DOT_ON    0x000088FF   // point actif : bleu vif
#define ANIM_DOT_TRAIL 0x00224488   // trainée : bleu sombre
#define ANIM_DOT_OFF   0x00111122   // point éteint

// ============================================================
// Math entier — pas de FPU en bare-metal
// ============================================================

// sin/cos approximés en virgule fixe (précision 1/1024)
// Lookup table 64 entrées pour 0..2π
static const int16_t sin64[64] = {
       0,  101,  200,  297,  390,  478,  561,  637,
     706,  768,  822,  868,  904,  931,  949,  957,
     956,  945,  925,  896,  858,  813,  760,  700,
     634,  563,  488,  409,  328,  245,  161,   77,
      -8, -101, -200, -297, -390, -478, -561, -637,
    -706, -768, -822, -868, -904, -931, -949, -957,
    -956, -945, -925, -896, -858, -813, -760, -700,
    -634, -563, -488, -409, -328, -245, -161,  -77
};

// sin(i * 2π/64) * 1024, cos décalé de 16 positions
static inline int isin(int i) { return (int)sin64[((i % 64) + 64) % 64]; }
static inline int icos(int i) { return (int)sin64[((i + 16) % 64 + 64) % 64]; }

// ============================================================
// Primitives pixel directes (bypass dirty buffer — c'est voulu,
// on est en mode graphique pur pendant l'animation)
// ============================================================

static inline void px(int x, int y, uint32_t color) {
    if ((uint32_t)x >= vesa_width() || (uint32_t)y >= vesa_height()) return;
    *(uint32_t*)((uint8_t*)vesa_fb_addr()
        + (uint32_t)y * vesa_pitch()
        + (uint32_t)x * 4) = color;
}

// Disque plein (cercle rempli) — pour dessiner les points du spinner
static void fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r)
                px(cx + dx, cy + dy, color);
        }
    }
}

// Cercle vide (anneau fin) — pour le rail de fond du spinner
static void draw_ring(int cx, int cy, int r, int thickness, uint32_t color) {
    int r_out = r;
    int r_in  = r - thickness;
    for (int dy = -r_out; dy <= r_out; dy++) {
        for (int dx = -r_out; dx <= r_out; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= r_out*r_out && d2 >= r_in*r_in)
                px(cx + dx, cy + dy, color);
        }
    }
}

// Dégradé vertical simple
static void gradient_bg(void) {
    uint32_t sw = vesa_width();
    uint32_t sh = vesa_height();

    uint8_t tr = (ANIM_BG_TOP >> 16) & 0xFF;
    uint8_t tg = (ANIM_BG_TOP >>  8) & 0xFF;
    uint8_t tb =  ANIM_BG_TOP        & 0xFF;
    uint8_t br = (ANIM_BG_BOT >> 16) & 0xFF;
    uint8_t bg = (ANIM_BG_BOT >>  8) & 0xFF;
    uint8_t bb =  ANIM_BG_BOT        & 0xFF;

    for (uint32_t y = 0; y < sh; y++) {
        uint32_t* line = (uint32_t*)((uint8_t*)vesa_fb_addr() + y * vesa_pitch());
        uint8_t r = tr + (uint8_t)(((int)(br - tr) * (int)y) / (int)sh);
        uint8_t g = tg + (uint8_t)(((int)(bg - tg) * (int)y) / (int)sh);
        uint8_t b = tb + (uint8_t)(((int)(bb - tb) * (int)y) / (int)sh);
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (uint32_t x = 0; x < sw; x++) line[x] = c;
    }

    // Barres décoratives
    for (uint32_t x = 0; x < sw; x++) {
        for (int t = 0; t < 3; t++) {
            *(uint32_t*)((uint8_t*)vesa_fb_addr() + (uint32_t)t * vesa_pitch() + x*4) = ANIM_ACCENT;
            *(uint32_t*)((uint8_t*)vesa_fb_addr() + (sh-1-(uint32_t)t) * vesa_pitch() + x*4) = ANIM_ACCENT2;
        }
    }
}

// Texte centré minimaliste (réutilise draw_glyph_raw via vesa_put_pixel —
// on appelle directement vesa_draw_glyph + flush serait lourd ici,
// donc on dessine pixel par pixel avec vga_font)
extern const uint8_t vga_font[256][16];

static void draw_text_centered_px(int cx, int y, const char* str,
                                   uint32_t fg, uint32_t bg) {
    int len = 0;
    while (str[len]) len++;
    int start_x = cx - (len * FONT_W) / 2;

    for (int i = 0; i < len; i++) {
        const uint8_t* glyph = vga_font[(uint8_t)str[i]];
        int gx = start_x + i * FONT_W;
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                px(gx + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
            }
        }
    }
}

// ============================================================
// Timer busy-wait (calibration approximative)
// En bare-metal on n'a pas usleep — on boucle dans le vide.
// Ajuste LOOP_PER_MS selon la vitesse de ta machine.
// ============================================================
#define LOOPS_PER_MS 50000

static void delay_ms(int ms) {
    volatile int n = ms * LOOPS_PER_MS;
    while (n-- > 0) __asm__ volatile("nop");
}

// ============================================================
// vesa_boot_anim — animation principale
//
// Spinner style Windows :
//   - 8 points disposés en cercle
//   - Un arc de 4 points "actifs" tourne en continu
//   - Trainée dégradée derrière l'arc
//   - Texte "TetraOS" + sous-titre en dessous
// ============================================================
#define N_DOTS      6       // nombre total de points
#define ARC_LEN     4       // longueur de l'arc lumineux
#define SPIN_R      36      // rayon du spinner (px)
#define DOT_R       4       // rayon de chaque point (px)
#define SPIN_FRAMES 600      // frames par tour complet
#define N_LOOPS     5       // nombre de tours avant de finir

void vesa_boot_anim(void) {
    if (!vesa_active()) return;

    uint32_t sw = vesa_width();
    uint32_t sh = vesa_height();
    int cx = (int)sw / 2;
    int cy = (int)sh / 2 + 40;  // légèrement sous le centre

    // --- Fond fixe (dessiné une seule fois) ---
    gradient_bg();

    // Logo "TetraOS"
    draw_text_centered_px(cx, (int)sh/2 - 80,
                          "TetraOS", ANIM_WHITE, 0x00000000);
    draw_text_centered_px(cx, (int)sh/2 - 56,
                          "v1.0",    ANIM_GRAY,  0x00000000);

    // Rail de fond du spinner (cercle gris fixe)
    draw_ring(cx, cy, SPIN_R, DOT_R + 1, 0x00111122);

    // Sous-titre
    draw_text_centered_px(cx, cy + SPIN_R + 20,
                          "Initialisation...", ANIM_GRAY, 0x00000000);

    // --- Boucle d'animation ---
    int total_frames = SPIN_FRAMES * N_LOOPS;

    for (int frame = 0; frame <= total_frames; frame++) {
        // Position angulaire de la tête de l'arc (en 64ièmes de tour)
        int head = (frame * 64) / SPIN_FRAMES;

        // Dessiner les N_DOTS points
        for (int d = 0; d < N_DOTS; d++) {
            // Angle du point d dans la grille de 64
            int angle = (head - d * (64 / N_DOTS) + 128) % 64;

            // Position pixel du centre du point
            int px_x = cx + (icos(angle) * SPIN_R) / 1024;
            int px_y = cy - (isin(angle) * SPIN_R) / 1024;

            // Distance par rapport à la tête → couleur
            uint32_t color;
            if (d == 0) {
                color = ANIM_DOT_ON;     // tête : bleu vif
            } else if (d < ARC_LEN) {
                // Trainée : interpolation linéaire vers sombre
                int t = d * 255 / ARC_LEN;
                uint8_t r = (uint8_t)(((ANIM_DOT_ON  >> 16) & 0xFF) * (255-t) / 255
                                    + ((ANIM_DOT_TRAIL >> 16) & 0xFF) * t / 255);
                uint8_t g = (uint8_t)(((ANIM_DOT_ON  >>  8) & 0xFF) * (255-t) / 255
                                    + ((ANIM_DOT_TRAIL >>  8) & 0xFF) * t / 255);
                uint8_t b = (uint8_t)(( ANIM_DOT_ON        & 0xFF) * (255-t) / 255
                                    + ( ANIM_DOT_TRAIL      & 0xFF) * t / 255);
                color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                color = ANIM_DOT_OFF;    // éteint
            }

            fill_circle(px_x, px_y, DOT_R, color);
        }

        delay_ms(18);  // ~55 fps

        // Effacer les points pour la prochaine frame
        // (on repeint juste les disques en couleur de fond locale)
        for (int d = 0; d < N_DOTS; d++) {
            int angle = (head - d * (64 / N_DOTS) + 128) % 64;
            int px_x  = cx + (icos(angle) * SPIN_R) / 1024;
            int px_y  = cy - (isin(angle) * SPIN_R) / 1024;

            // Recalculer la couleur de fond à cet endroit (dégradé)
            uint8_t tr2 = (ANIM_BG_TOP >> 16) & 0xFF;
            uint8_t tg2 = (ANIM_BG_TOP >>  8) & 0xFF;
            uint8_t tb2 =  ANIM_BG_TOP        & 0xFF;
            uint8_t br2 = (ANIM_BG_BOT >> 16) & 0xFF;
            uint8_t bg2 = (ANIM_BG_BOT >>  8) & 0xFF;
            uint8_t bb2 =  ANIM_BG_BOT        & 0xFF;
            int yy = px_y;
            if (yy < 0) yy = 0;
            uint8_t lr = tr2 + (uint8_t)(((int)(br2-tr2) * yy) / (int)sh);
            uint8_t lg = tg2 + (uint8_t)(((int)(bg2-tg2) * yy) / (int)sh);
            uint8_t lb = tb2 + (uint8_t)(((int)(bb2-tb2) * yy) / (int)sh);
            uint32_t local_bg = ((uint32_t)lr << 16) | ((uint32_t)lg << 8) | lb;

            fill_circle(px_x, px_y, DOT_R, local_bg);
        }
    }

    // --- Fondu final vers noir ---
    // On assombrit progressivement l'écran entier
    for (int step = 0; step < 16; step++) {
        uint32_t* fb = (uint32_t*)vesa_fb_addr();
        uint32_t  total_pixels = vesa_pitch() / 4 * vesa_height();
        for (uint32_t i = 0; i < total_pixels; i++) {
            uint32_t c = fb[i];
            uint8_t r = ((c >> 16) & 0xFF) * 3 / 4;
            uint8_t g = ((c >>  8) & 0xFF) * 3 / 4;
            uint8_t b = ( c        & 0xFF) * 3 / 4;
            fb[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        delay_ms(30);
    }

    // Écran noir — prêt pour le login
    uint32_t* fb = (uint32_t*)vesa_fb_addr();
    uint32_t  total_pixels = vesa_pitch() / 4 * vesa_height();
    for (uint32_t i = 0; i < total_pixels; i++) fb[i] = 0;

    // Invalider le dirty cache : l'animation a écrit directement dans
    // le framebuffer sans passer par vesa_draw_glyph. Sans ça, le cache
    // croit que les cellules sont déjà à jour et bloque le rendu du login.
    vesa_invalidate_all();
}