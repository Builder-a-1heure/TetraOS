// apps/fileman.h — Explorateur de fichiers TetraOS v2
//
// Interface publique.
//
// === Utilisation ===
//   Depuis le bureau (TEX_APP) ou le shell :
//     app_fileman_run()   → ouvre l'explorateur dans le répertoire courant (g_cwd)
//
// === Fonctionnalités v2 ===
//   Navigation         : dossiers + fichiers RAY64, chemin complet en barre d'adresse
//   Opérations FS      : créer dossier/fichier, renommer, copier/coller, supprimer
//   Permissions (RAY64): dialog chmod graphique (owner/admin/other × rwx)
//   Propriétaire       : dialog chown (admin requis)
//   App par extension  : .tex → exécuter, .bin/.img → hex (futur), reste → TextEdit
//   Panneau latéral    : infos du nœud sélectionné (permissions, uid, taille, ext)
//   Liquid Glass       : animations bulles dans le panneau (désactivables)
//
// === Option animations ===
//   extern int g_fm_anim_enabled;   // 1 = activé (défaut), 0 = désactivé
//   Réglable depuis les paramètres système (settings.c) ou le shell.

#ifndef FILEMAN_H
#define FILEMAN_H

// Animations Liquid Glass — 1 = activé, 0 = désactivé
// Modifiable à chaud depuis settings.c ou le shell.
extern int g_fm_anim_enabled;

// Point d'entrée public.
// Bloque jusqu'à fermeture de la fenêtre AppCore.
void app_fileman_run(void);

#endif // FILEMAN_H
