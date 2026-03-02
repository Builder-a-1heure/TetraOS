#ifndef VESA_ANIM_H
#define VESA_ANIM_H

// Lance l'animation de chargement circulaire (style Windows).
// À appeler à la fin de vesa_init(), juste avant session_do_login_flow().
// Durée : environ 2-3 secondes (N_LOOPS tours × SPIN_FRAMES frames × 18ms).
// Gère elle-même vesa_clear_back() — pas besoin de l'appeler avant.
void vesa_boot_anim(void);

#endif // VESA_ANIM_H