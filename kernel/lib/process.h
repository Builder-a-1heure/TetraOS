// kernel/lib/process.h — Contexte de processus TetraOS
//
// Couche basse entre session.h et AppCore/apps.
// Peuplé par desktop.c au moment du launch_app(), avant l'appel au
// point d'entrée de l'app. Toutes les vérifications ACL et permissions
// lisent ce contexte plutôt que g_session_manager directement.
//
// Modèle actuel : mono-tâche (une seule app active à la fois).
// Évolution future : remplacer g_current_process par une table
// g_processes[MAX_PROC] avec un index courant — l'API ne change pas.

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

// ============================================================
// Structure du contexte de processus
// ============================================================
typedef struct {
    uint16_t uid;           // UID de la session propriétaire (session_get_uid() au launch)
    uint32_t permissions;   // Bitmap copié depuis Session.permissions au launch
    uint32_t cwd;           // Répertoire de travail du processus (g_cwd au launch)
    uint8_t  active;        // 1 si un processus est en cours d'exécution
    uint8_t  is_admin;      // Copié depuis Session.is_admin au launch
} ProcessContext;

// Contexte du processus courant (mono-tâche)
extern ProcessContext g_current_process;

// ============================================================
// API
// ============================================================

// Appelé par desktop.c AVANT chaque launch_app().
// Prend un snapshot de la session courante dans g_current_process.
void process_begin(void);

// Appelé par desktop.c APRÈS retour de l'app (optionnel, remet active=0).
void process_end(void);

// Retourne l'UID du processus courant.
// Si aucun processus actif, retourne UID_SYSTEM (0xFFFF).
uint16_t process_get_uid(void);

// Retourne 1 si le processus courant est admin.
int process_is_admin(void);

// Retourne 1 si le processus courant a la permission donnée.
// perm : index de bit (Permission enum de session.h)
int process_has_permission(int perm);

#endif // PROCESS_H
