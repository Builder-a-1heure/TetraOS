#ifndef EDITOR_H
#define EDITOR_H

// ============================================================
// editor.h — Éditeur de texte fenêtré TetraOS
//
// Ouvre un fichier du FS RAY64 dans un éditeur VGA/texte simple.
// Navigation : flèches, Backspace, Entrée.
// Sauvegarder : ESC   |   Annuler : Ctrl+C
// ============================================================

// Ouvre l'éditeur sur le fichier `filename` (dans le FS courant).
// Crée le fichier s'il n'existe pas. Bloquant jusqu'à ESC ou Ctrl+C.
void editor_open(const char* filename);

#endif // EDITOR_H
