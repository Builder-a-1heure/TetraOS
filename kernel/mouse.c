// mouse.c — Driver souris PS/2 minimal pour TetraOS
// Le contrôleur 8042 gère à la fois le clavier (IRQ1) et la souris (IRQ12).
// On n'utilise pas d'interruptions : polling pur via le status byte du port 0x64.
// Un paquet souris = 3 octets : [flags, dx, dy]

#include "mouse.h"
#include "vesa.h"
#include "screen.h"

// ============================================================
// Ports I/O
// ============================================================
#define PORT_DATA   0x60
#define PORT_STATUS 0x64
#define PORT_CMD    0x64

// Bits du status register
#define STATUS_OUTPUT_FULL  (1 << 0)  // Données dispo en lecture
#define STATUS_INPUT_FULL   (1 << 1)  // Contrôleur occupé
#define STATUS_MOUSE_DATA   (1 << 5)  // Les données viennent de la souris

static inline void io_wait(void) {
    // Petite attente pour laisser le contrôleur traiter
    __asm__ __volatile__("outb %%al, $0x80" : : "a"(0));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Attendre que le contrôleur soit prêt à recevoir une commande
static void wait_write(void) {
    int timeout = 100000;
    while (timeout-- && (inb(PORT_STATUS) & STATUS_INPUT_FULL));
}

// Attendre qu'une donnée soit disponible en lecture
static void wait_read(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(PORT_STATUS) & STATUS_OUTPUT_FULL));
}

// Envoyer une commande au contrôleur 8042
static void ctrl_cmd(uint8_t cmd) {
    wait_write();
    outb(PORT_CMD, cmd);
}

// Envoyer une commande directement à la souris
static void mouse_write(uint8_t data) {
    ctrl_cmd(0xD4);       // "Écriture vers le port auxiliaire (souris)"
    wait_write();
    outb(PORT_DATA, data);
}

// Lire un octet du port de données
static uint8_t mouse_read(void) {
    wait_read();
    return inb(PORT_DATA);
}

// ============================================================
// État global
// ============================================================
MouseState g_mouse = {0, 0, 0, 0, 0};

// Sauvegarde des pixels sous le curseur pour le restaurer
static uint32_t g_cursor_bg[16 * 16];
static int      g_cursor_saved = 0;
static int      g_cursor_saved_x = -1;
static int      g_cursor_saved_y = -1;

// Accumulateur de paquet (3 octets)
static uint8_t  g_pkt[3];
static int      g_pkt_idx = 0;

// ============================================================
// Initialisation
// ============================================================
void mouse_init(void) {
    uint8_t status;

    // 1. Activer le port auxiliaire
    ctrl_cmd(0xA8);
    io_wait();

    // 2. Lire la config courante du contrôleur
    ctrl_cmd(0x20);
    wait_read();
    status = inb(PORT_DATA);

    // 3. Polling pur — on N'active PAS les IRQ souris (bit 1 = 0)
    //    Sans IDT configurée, activer IRQ12 bloque le contrôleur.
    //    On lit les données directement depuis la boucle principale.
    status &= ~(1 << 1);  // IRQ12 DÉSACTIVÉ — polling pur
    status &= ~(1 << 5);  // Clock souris active (port auxiliaire ON)
    ctrl_cmd(0x60);
    wait_write();
    outb(PORT_DATA, status);
    io_wait();

    // 4. Reset souris
    mouse_write(0xFF);
    mouse_read(); // ACK (0xFA)
    mouse_read(); // Self-test passed (0xAA)
    mouse_read(); // Device ID (0x00)

    // 5. Activer le streaming de données
    mouse_write(0xF4);
    mouse_read(); // ACK

    // 6. Résolution par défaut (4 counts/mm) et sample rate 100
    mouse_write(0xE8); mouse_read(); // Set resolution
    mouse_write(0x02); mouse_read(); // 4 counts/mm
    mouse_write(0xF3); mouse_read(); // Set sample rate
    mouse_write(100);  mouse_read(); // 100 samples/sec

    // Position initiale au centre de l'écran
    g_mouse.x = (int)vesa_width()  / 2;
    g_mouse.y = (int)vesa_height() / 2;
    g_pkt_idx = 0;
}

// ============================================================
// Polling non-bloquant — lit UN octet si disponible
// Retourne 1 quand un paquet de 3 octets est complet et décodé
// ============================================================
int mouse_poll(void) {
    uint8_t st = inb(PORT_STATUS);

    // Aucune donnée disponible
    if (!(st & STATUS_OUTPUT_FULL)) return 0;

    // Lire l'octet — on accepte qu'il vienne du clavier ou de la souris
    // car en polling pur sans IRQ, le bit 5 peut être peu fiable
    uint8_t byte = inb(PORT_DATA);

    // Si c'est clairement une donnée clavier (bit 5 = 0 dans le status
    // qu'on a lu), on la remet "dans le clavier" en ne la traitant pas
    // comme souris — mais on ne peut pas la remettre dans le buffer.
    // Solution : on ne lit que si bit 5 est set OU si on est en milieu de paquet
    if (g_pkt_idx == 0 && !(st & STATUS_MOUSE_DATA)) {
        // Premier octet et bit 5 pas set → probablement clavier, ignorer
        return 0;
    }

    g_pkt[g_pkt_idx++] = byte;

    // Resynchronisation : le premier octet doit avoir le bit 3 à 1
    if (g_pkt_idx == 1 && !(byte & (1 << 3))) {
        g_pkt_idx = 0;
        return 0;
    }

    if (g_pkt_idx < 3) return 0; // Paquet incomplet, attendre les suivants
    g_pkt_idx = 0;

    // --- Décoder le paquet complet ---
    uint8_t flags = g_pkt[0];
    int8_t  dx    = (int8_t)g_pkt[1];
    int8_t  dy    = (int8_t)g_pkt[2];

    // Overflow → ignorer le mouvement mais mettre à jour les boutons
    if (!(flags & (1 << 6)) && !(flags & (1 << 7))) {
        // Signe étendu via les bits 4 et 5 du flags
        int mdx = (int)dx;
        int mdy = (int)dy;
        if (flags & (1 << 4)) mdx |= ~0xFF;
        if (flags & (1 << 5)) mdy |= ~0xFF;

        g_mouse.x += mdx;
        g_mouse.y -= mdy; // Y inversé PS/2

        // Clamp
        int sw = (int)vesa_width();
        int sh = (int)vesa_height();
        if (g_mouse.x < 0)   g_mouse.x = 0;
        if (g_mouse.y < 0)   g_mouse.y = 0;
        if (g_mouse.x >= sw) g_mouse.x = sw - 1;
        if (g_mouse.y >= sh) g_mouse.y = sh - 1;
    }

    // Boutons (toujours mis à jour)
    g_mouse.btn_left   = (flags & (1 << 0)) ? 1 : 0;
    g_mouse.btn_right  = (flags & (1 << 1)) ? 1 : 0;
    g_mouse.btn_middle = (flags & (1 << 2)) ? 1 : 0;

    return 1;
}

// ============================================================
// Curseur graphique (flèche 12×19 pixels)
// ============================================================
// Bitmap 1bpp, 12 colonnes × 19 lignes
// '1' = pixel visible, '0' = transparent
#define CW 12
#define CH 19
static const uint8_t cursor_shape[CH][CW] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,2,1,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0},
};

#define CURSOR_COLOR_OUTLINE  0x00000000  // Noir
#define CURSOR_COLOR_FILL     0x00FFFFFF  // Blanc

void mouse_erase_cursor(void) {
    if (!g_cursor_saved || !vesa_active()) return;
    int sw = (int)vesa_width();
    int sh = (int)vesa_height();
    for (int row = 0; row < CH; row++) {
        for (int col = 0; col < CW; col++) {
            int px = g_cursor_saved_x + col;
            int py = g_cursor_saved_y + row;
            if (px >= 0 && px < sw && py >= 0 && py < sh)
                vesa_put_pixel(px, py, g_cursor_bg[row * CW + col]);
        }
    }
    g_cursor_saved = 0;
}

void mouse_draw_cursor(void) {
    if (!vesa_active()) return;
    int sw = (int)vesa_width();
    int sh = (int)vesa_height();

    // Sauvegarder le fond
    for (int row = 0; row < CH; row++) {
        for (int col = 0; col < CW; col++) {
            int px = g_mouse.x + col;
            int py = g_mouse.y + row;
            uint32_t px_color = 0;
            if (px >= 0 && px < sw && py >= 0 && py < sh) {
                // Lire le pixel courant du framebuffer
                extern uint32_t vesa_get_pixel(int x, int y);
                px_color = vesa_get_pixel(px, py);
            }
            g_cursor_bg[row * CW + col] = px_color;
        }
    }
    g_cursor_saved_x = g_mouse.x;
    g_cursor_saved_y = g_mouse.y;
    g_cursor_saved   = 1;

    // Dessiner la flèche
    for (int row = 0; row < CH; row++) {
        for (int col = 0; col < CW; col++) {
            int px = g_mouse.x + col;
            int py = g_mouse.y + row;
            if (px < 0 || px >= sw || py < 0 || py >= sh) continue;
            uint8_t v = cursor_shape[row][col];
            if (v == 1) vesa_put_pixel(px, py, CURSOR_COLOR_OUTLINE);
            else if (v == 2) vesa_put_pixel(px, py, CURSOR_COLOR_FILL);
            // 0 = transparent, on ne dessine rien
        }
    }
}
