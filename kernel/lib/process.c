// kernel/lib/process.c — Contexte de processus TetraOS

#include "process.h"
#include "../ui/session.h"
#include "../fs/fs.h"

// UID_SYSTEM défini dans fs.h — on l'inclut via session.h qui l'utilise déjà,
// mais on redéfinit la constante localement pour éviter la dépendance circulaire.
#define _UID_SYSTEM 0xFFFF

ProcessContext g_current_process = {
    .uid         = 0xFFFF,  // UID_SYSTEM
    .permissions = 0,
    .cwd         = 0,
    .active      = 0,
    .is_admin    = 0,
};

void process_begin(void) {
    if (!g_session_manager.logged_in || !g_session_manager.current_session) {
        // Pas de session — contexte vide (refus par défaut)
        g_current_process.uid         = (uint16_t)_UID_SYSTEM;
        g_current_process.permissions = 0;
        g_current_process.cwd         = 0;
        g_current_process.active      = 0;
        g_current_process.is_admin    = 0;
        return;
    }

    g_current_process.uid         = session_get_uid();
    g_current_process.permissions = g_session_manager.current_session->permissions;
    g_current_process.cwd         = g_cwd;
    g_current_process.active      = 1;
    g_current_process.is_admin    = g_session_manager.current_session->is_admin;
}

void process_end(void) {
    g_current_process.active = 0;
}

uint16_t process_get_uid(void) {
    if (!g_current_process.active) return (uint16_t)_UID_SYSTEM;
    return g_current_process.uid;
}

int process_is_admin(void) {
    if (!g_current_process.active) return 0;
    return g_current_process.is_admin;
}

int process_has_permission(int perm) {
    if (!g_current_process.active) return 0;
    // Même logique que session_has_permission() mais depuis le contexte process
    return (g_current_process.permissions & (1 << perm)) != 0;
}
