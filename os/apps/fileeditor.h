// apps/fileeditor.h — Éditeur de fichiers dynamique TetraOS
//
// Détecte automatiquement le type de fichier et ouvre le bon mode :
//
//   .txt .md .log ...    → Mode TEXTE  (éditeur enrichi, word-like)
//   .tex .c .h .s .asm  → Mode CODE   (coloration syntaxique, numéros de ligne)
//   .csv .xml .tsv       → Mode GRILLE (tableur simplifié)
//   .bin .img .raw .o    → Mode HEX    (dump hexadécimal + ASCII)
//   (autre)              → Mode TEXTE  (fallback)
//
// Lancement :
//   app_fileeditor_run("notes.txt")     → mode texte
//   app_fileeditor_run("main.c")        → mode code (C)
//   app_fileeditor_run("data.csv")      → mode grille
//   app_fileeditor_run("kernel.bin")    → mode hex
//   app_fileeditor_run("")              → nouveau fichier texte vide

#ifndef FILEEDITOR_H
#define FILEEDITOR_H

// Point d'entrée public.
// filename : chemin du fichier à ouvrir (relatif à g_cwd), ou "" pour nouveau.
// Bloque jusqu'à fermeture de la fenêtre.
void app_fileeditor_run(const char* filename);

// Point d'entrée TEX_APP (bureau)
void app_fileeditor(void);

#endif // FILEEDITOR_H
