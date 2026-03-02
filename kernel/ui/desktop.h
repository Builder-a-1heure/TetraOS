#ifndef DESKTOP_H
#define DESKTOP_H

// Point d'entrée principal du bureau graphique.
// Appelé après le login, remplace tetra_shell() dans la boucle kmain.
// Ne retourne que sur logout (session.logout).
void desktop_run(void);

#endif // DESKTOP_H
