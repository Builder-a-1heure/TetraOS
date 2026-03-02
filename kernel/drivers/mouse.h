#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// État courant de la souris
typedef struct {
    int x;          // Position X (pixels)
    int y;          // Position Y (pixels)
    int btn_left;   // 1 si bouton gauche enfoncé
    int btn_right;  // 1 si bouton droit enfoncé
    int btn_middle; // 1 si bouton milieu enfoncé
} MouseState;

extern MouseState g_mouse;
int  mouse_in_packet(void);
void mouse_reset_cursor(void);

// Initialise la souris PS/2 via le contrôleur 8042
void mouse_init(void);

// Lit et accumule un paquet souris si des données sont disponibles (non-bloquant)
// Retourne 1 si un paquet complet a été traité, 0 sinon
int mouse_poll(void);

// Dessine le curseur à la position courante
void mouse_draw_cursor(void);

// Efface le curseur (restaure les pixels en dessous)
void mouse_erase_cursor(void);

#endif // MOUSE_H
