// apps/fileman.h — Explorateur de fichiers TetraOS
// Interface publique — lancé depuis le bureau ou le shell.
//
// Usage depuis shell_dispatch :
//   app_fileman_run()   → ouvre l'explorateur dans le répertoire courant
//
// L'explorateur permet :
//   - Naviguer dans l'arborescence RAY64 (double-clic sur dossier)
//   - Voir les fichiers et répertoires avec leur taille
//   - Ouvrir un fichier texte dans TextEdit (double-clic sur fichier)
//   - Créer un nouveau dossier
//   - Supprimer un fichier ou dossier vide
//   - Afficher le chemin courant dans la barre d'adresse

#ifndef FILEMAN_H
#define FILEMAN_H

// Point d'entrée public.
// Bloque jusqu'à fermeture de la fenêtre.
void app_fileman_run(void);

#endif // FILEMAN_H