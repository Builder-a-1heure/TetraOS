// apps/textedit.h — Éditeur de fichiers .txt pour TetraOS
// Interface publique — une seule fonction à appeler depuis le shell.
//
// Usage depuis shell_dispatch :
//   app_textedit_run("")              → nouveau fichier vide
//   app_textedit_run("notes.txt")    → ouvre notes.txt (ou le crée)

#ifndef TEXTEDIT_H
#define TEXTEDIT_H

// Point d'entrée public.
// filename : nom du fichier à ouvrir, ou chaîne vide pour un nouveau fichier.
// Bloque jusqu'à fermeture de la fenêtre éditeur.
void app_textedit_run(const char* filename);

#endif // TEXTEDIT_H