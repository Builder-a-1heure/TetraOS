// kernel/gfx/wallpaper.c
//
// Charge wallpaper.bin (RGB24 flat, 1920×1080) depuis le FS RAY64 et le blit
// directement dans le framebuffer VESA, UN SECTEUR À LA FOIS (512 bytes = 170 px).
//
// Pourquoi pas de buffer RAM global ?
//   Un buffer 1920×1080×3 = 6 220 800 bytes en BSS atterrit ~6 Mo après la fin
//   du kernel. En bare-metal i686 sur QEMU/real HW avec 64 Mo de RAM, cette zone
//   n'est pas garantie accessible (BIOS e820, holes mémoire) → le CPU freeze
//   à la première écriture. On évite le problème en lisant un secteur (512 bytes)
//   à la fois dans un petit buffer statique et en blit-ant immédiatement.
//
// Format wallpaper.bin attendu :
//   RGB24 flat, row-major : R G B R G B ... (3 bytes par pixel)
//   Taille : exactement WALLPAPER_W * WALLPAPER_H * 3 bytes sur disque.
//   Le fichier peut être paddé à un multiple de 512 — on ignore le surplus.

#include "wallpaper.h"
#include "../fs/fs.h"
#include "../drivers/ata.h"
#include "../drivers/vesa.h"
#include "../gfx/screen.h"

int g_wallpaper_loaded = 0;

// LBA de début des données wallpaper (data_start_lba + 1 pour sauter le FileHeader).
// Mis en cache au premier wallpaper_load() pour que wallpaper_blit_rect() puisse
// relire le disque sans reparcourir la FSTable à chaque fois.
static uint32_t g_wallpaper_data_lba = 0;

// Buffer statique d'un seul secteur (512 bytes).
// 512 / 3 = 170 pixels complets + 2 bytes de reste (gérés en chevauchement).
#define SECTOR_BYTES 512
static uint8_t g_sector_buf[SECTOR_BYTES];

// ============================================================
// Trouver le nœud wallpaper.bin dans la FSTable
// ============================================================
static FSNode* find_wallpaper_node(void) {
    for (uint32_t i = 0; i < g_fs.node_count; i++) {
        FSNode* n = &g_fs.nodes[i];
        if (n->magic      != FS_MAGIC) continue;
        if (n->is_dir)                  continue;
        if (n->data_start_lba == 0)     continue;

        // Comparaison du nom (max 32 chars)
        const char* a = n->name;
        const char* b = "wallpaper.bin";
        int match = 1;
        for (int k = 0; k < 32; k++) {
            if (a[k] != b[k]) { match = 0; break; }
            if (a[k] == '\0') break;
        }
        if (match) return n;
    }
    return (void*)0;
}

// ============================================================
// Blit interne : lit [sector_offset .. sector_offset+sector_count[
// depuis g_wallpaper_data_lba et blit les pixels correspondants
// dans le framebuffer VESA.
//
// Le flux RGB24 est découpé proprement même quand un pixel chevauche
// deux secteurs (= le rouge est en fin d'un secteur, vert+bleu au début
// du suivant) grâce à un registre de débordement de 2 bytes.
// ============================================================
static void blit_sectors(uint32_t sector_offset, uint32_t sector_count) {
    uint32_t* bb   = vesa_backbuf();
    uint32_t sw    = vesa_width();
    uint32_t sh    = vesa_height();

    if (!bb || !sw || !sh) return;

    // Position pixel globale correspondant au début du premier secteur lu.
    // sector_offset * 512 bytes / 3 bytes per pixel
    uint32_t global_px_start = (sector_offset * SECTOR_BYTES) / 3u;

    // Byte overflow du secteur précédent (0, 1 ou 2 bytes).
    uint8_t  overflow[2] = {0, 0};
    uint32_t overflow_cnt = (sector_offset * SECTOR_BYTES) % 3u;
    // Si on commence à mi-pixel, on a besoin de récupérer les bytes précédents.
    // On relit juste le secteur précédent pour extraire les bytes manquants.
    if (overflow_cnt > 0 && sector_offset > 0) {
        if (ata_read_single(g_wallpaper_data_lba + sector_offset - 1, g_sector_buf) == 0) {
            for (uint32_t b = 0; b < overflow_cnt; b++)
                overflow[b] = g_sector_buf[SECTOR_BYTES - overflow_cnt + b];
        }
    }

    // Index du pixel courant dans l'image 1920×1080
    uint32_t px_idx = global_px_start;
    // Bytes déjà accumulés pour le pixel courant (0, 1 ou 2)
    uint8_t  partial[3];
    uint32_t partial_cnt = 0;

    // Récupérer les bytes débordants du secteur précédent
    for (uint32_t b = 0; b < overflow_cnt; b++)
        partial[partial_cnt++] = overflow[b];
    // On recule px_idx pour que les bytes overflow appartiennent bien au bon pixel
    // (ils sont déjà pris en compte par partial_cnt, pas besoin d'ajuster px_idx)

    for (uint32_t s = 0; s < sector_count; s++) {
        if (ata_read_single(g_wallpaper_data_lba + sector_offset + s, g_sector_buf) != 0)
            break;  // erreur ATA → on arrête, le reste du fond restera noir

        for (uint32_t b = 0; b < SECTOR_BYTES; b++) {
            partial[partial_cnt++] = g_sector_buf[b];

            if (partial_cnt == 3) {
                // Pixel complet — calculer ses coordonnées dans l'image
                uint32_t img_x = px_idx % WALLPAPER_W;
                uint32_t img_y = px_idx / WALLPAPER_W;

                // On ne blit que si le pixel est dans les limites du framebuffer
                if (img_x < sw && img_y < sh
                        && img_x < VESA_BB_W && img_y < VESA_BB_H) {
                    uint32_t color = ((uint32_t)partial[0] << 16)
                                   | ((uint32_t)partial[1] <<  8)
                                   |  (uint32_t)partial[2];
                    bb[img_y * VESA_BB_W + img_x] = color;
                }

                px_idx++;
                partial_cnt = 0;

                // Arrêter si on a dépassé la fin de l'image
                if (px_idx >= WALLPAPER_W * WALLPAPER_H) return;
            }
        }
    }
}

// ============================================================
// wallpaper_load — cherche le fichier, mémorise le LBA, blit tout
// ============================================================
int wallpaper_load(void) {
    if (g_wallpaper_loaded) return 1;

    print_string("[WP] Recherche wallpaper.bin...\n");

    FSNode* node = find_wallpaper_node();
    if (!node) {
        print_string("[WP] Introuvable - fond noir\n");
        return 0;
    }

    // Vérification souple de la taille : on accepte tout fichier dont la taille
    // déclarée est entre 1 et WALLPAPER_SIZE bytes inclus.
    // Un fichier paddé à un multiple de 512 aura size_bytes == WALLPAPER_SIZE,
    // mais on tolère aussi une valeur légèrement inférieure (image partielle).
    if (node->size_bytes == 0 || node->size_bytes > WALLPAPER_SIZE) {
        print_string("[WP] Taille invalide: ");
        print_dec(node->size_bytes);
        print_string(" (max=");
        print_dec(WALLPAPER_SIZE);
        print_string(")\n");
        return 0;
    }

    // data_start_lba pointe sur le FileHeader (512 bytes).
    // Les données RGB24 commencent au LBA suivant.
    g_wallpaper_data_lba = node->data_start_lba + 1u;

    print_string("[WP] LBA data=");
    print_dec(g_wallpaper_data_lba);
    print_string(" size=");
    print_dec(node->size_bytes);
    print_string(" bytes\n");

    // Nombre de secteurs à lire pour couvrir toute l'image
    uint32_t total_sectors = (node->size_bytes + SECTOR_BYTES - 1u) / SECTOR_BYTES;

    // Bloquer screen_render() pendant les lectures pour éviter que les print_*
    // ci-dessus ne déclenchent render_vesa() par-dessus le blit en cours.
    int prev_ui = g_ui_drawing;
    g_ui_drawing = 1;

    blit_sectors(0, total_sectors);

    g_ui_drawing = prev_ui;
    g_wallpaper_loaded = 1;
    print_string("[WP] OK!\n");
    return 1;
}

// ============================================================
// wallpaper_blit_rect — reblit une zone rectangulaire depuis le disque
// Utilisé par desktop.c pour réparer ce qu'une fenêtre a couvert.
// ============================================================
void wallpaper_blit_rect(int x, int y, int w, int h) {
    if (!g_wallpaper_loaded || g_wallpaper_data_lba == 0) {
        // Pas de wallpaper → fond noir
        gfx_fill_rect(x, y, w, h, 0x00000000);
        return;
    }

    uint32_t* bb   = vesa_backbuf();
    uint32_t sw    = vesa_width();
    uint32_t sh    = vesa_height();
    if (!bb) return;

    // Clamp au framebuffer
    int x2 = x + w; if (x2 > (int)sw) x2 = (int)sw;
    int y2 = y + h; if (y2 > (int)sh) y2 = (int)sh;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x2 <= x || y2 <= y) return;

    // Pour chaque ligne de la zone, calculer le byte offset dans le flux RGB24,
    // trouver le secteur, le lire, et copier les pixels de la colonne x à x2.
    for (int py = y; py < y2; py++) {
        // Offset du premier pixel de cette ligne dans le flux RGB24
        uint32_t row_byte_start = ((uint32_t)py * WALLPAPER_W + (uint32_t)x) * 3u;
        uint32_t row_byte_end   = ((uint32_t)py * WALLPAPER_W + (uint32_t)x2) * 3u;

        if (row_byte_start >= WALLPAPER_SIZE) break;
        if (row_byte_end   >  WALLPAPER_SIZE) row_byte_end = WALLPAPER_SIZE;

        // Secteur contenant le premier byte de la ligne
        uint32_t sec_start = row_byte_start / SECTOR_BYTES;
        uint32_t sec_end   = (row_byte_end  + SECTOR_BYTES - 1u) / SECTOR_BYTES;

        // Lire secteur par secteur, extraire uniquement les pixels [x .. x2[
        uint32_t cur_byte = sec_start * SECTOR_BYTES;
        uint32_t px_idx   = cur_byte / 3u; // index pixel global au début du premier secteur
        uint8_t  partial[3];
        uint32_t partial_cnt = cur_byte % 3u;

        // Récupérer les bytes débordants éventuels du secteur précédent
        if (partial_cnt > 0 && sec_start > 0) {
            if (ata_read_single(g_wallpaper_data_lba + sec_start - 1, g_sector_buf) == 0) {
                for (uint32_t b = 0; b < partial_cnt; b++)
                    partial[b] = g_sector_buf[SECTOR_BYTES - partial_cnt + b];
            }
        }

        for (uint32_t s = sec_start; s < sec_end; s++) {
            if (ata_read_single(g_wallpaper_data_lba + s, g_sector_buf) != 0) break;

            for (uint32_t b = 0; b < SECTOR_BYTES; b++) {
                partial[partial_cnt++] = g_sector_buf[b];
                if (partial_cnt == 3) {
                    uint32_t img_x = px_idx % WALLPAPER_W;
                    uint32_t img_y = px_idx / WALLPAPER_W;

                    // N'écrire que les pixels de la colonne demandée
                    if ((int)img_y == py && (int)img_x >= x && (int)img_x < x2
                            && img_x < sw && img_y < sh
                            && img_x < VESA_BB_W && img_y < VESA_BB_H) {
                        uint32_t color = ((uint32_t)partial[0] << 16)
                                       | ((uint32_t)partial[1] <<  8)
                                       |  (uint32_t)partial[2];
                        bb[img_y * VESA_BB_W + img_x] = color;
                    }

                    px_idx++;
                    partial_cnt = 0;

                    // Sortir dès qu'on a fini la zone de cette ligne
                    if ((int)(px_idx % WALLPAPER_W) >= x2 || px_idx >= WALLPAPER_W * WALLPAPER_H)
                        goto next_line;
                }
            }
        }
        next_line:;
    }
    vesa_invalidate_rect(x, y, x2 - x, y2 - y);
}
