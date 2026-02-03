// session.c - Implémentation du système de sessions
#include "session.h"
#include "screen.h"
#include "input.h"
#include "fs.h"
#include "utils.h"

// Gestionnaire global de sessions
SessionManager g_session_manager;

// Nom du fichier de sauvegarde des sessions
#define SESSION_FILE "sessions.dat"

// ============================================================================
// FONCTIONS D'INITIALISATION
// ============================================================================

void session_init(void) {
    g_session_manager.session_count = 0;
    g_session_manager.logged_in = 0;
    g_session_manager.current_session = NULL;
    g_session_manager.current_session_index = -1;
    
    // Initialiser toutes les sessions comme inactives
    for (int i = 0; i < MAX_SESSIONS; i++) {
        g_session_manager.sessions[i].is_active = 0;
        memset(g_session_manager.sessions[i].name, 0, SESSION_NAME_LEN);
        memset(g_session_manager.sessions[i].password, 0, SESSION_PASSWORD_LEN);
        g_session_manager.sessions[i].is_admin = 0;
        g_session_manager.sessions[i].permissions = 0;
    }
}

void session_load(void) {
    uint8_t buffer[sizeof(Session) * MAX_SESSIONS];
    int bytes = fs_read_file(SESSION_FILE, buffer, sizeof(buffer));
    
    if (bytes > 0) {
        // Copier les données dans le gestionnaire
        int num_sessions = bytes / sizeof(Session);
        if (num_sessions > MAX_SESSIONS) num_sessions = MAX_SESSIONS;
        
        for (int i = 0; i < num_sessions; i++) {
            Session* src = (Session*)&buffer[i * sizeof(Session)];
            if (src->is_active) {
                memcpy(&g_session_manager.sessions[i], src, sizeof(Session));
                g_session_manager.session_count++;
            }
        }
    }
}

void session_save(void) {
    uint8_t buffer[sizeof(Session) * MAX_SESSIONS];
    memset(buffer, 0, sizeof(buffer));
    
    // Copier toutes les sessions actives dans le buffer
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active) {
            memcpy(&buffer[i * sizeof(Session)], 
                   &g_session_manager.sessions[i], 
                   sizeof(Session));
        }
    }
    
    fs_write_file(SESSION_FILE, buffer, sizeof(buffer));
}

// ============================================================================
// FONCTIONS DE GESTION DES SESSIONS
// ============================================================================

int session_create(const char* name, const char* password, uint8_t is_admin) {
    // Vérifier qu'on n'a pas atteint le maximum
    if (g_session_manager.session_count >= MAX_SESSIONS) {
        return -1;
    }
    
    // Vérifier que le nom n'existe pas déjà
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active &&
            strcmp(g_session_manager.sessions[i].name, name) == 0) {
            return -1; // Session existe déjà
        }
    }
    
    // Trouver un slot libre
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_session_manager.sessions[i].is_active) {
            Session* s = &g_session_manager.sessions[i];
            
            strncpy(s->name, name, SESSION_NAME_LEN - 1);
            s->name[SESSION_NAME_LEN - 1] = '\0';
            
            session_hash_password(password, s->password);
            
            s->is_active = 1;
            s->is_admin = is_admin;
            
            // Définir les permissions
            if (is_admin) {
                session_grant_all_permissions(i);
            } else {
                // Permissions de base pour utilisateur normal
                s->permissions = (1 << PERM_FS_READ) |
                                (1 << PERM_LIST_FILES) |
                                (1 << PERM_CLEAR_SCREEN);
            }
            
            g_session_manager.session_count++;
            session_save();
            return i;
        }
    }
    
    return -1;
}

int session_delete(const char* name) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active &&
            strcmp(g_session_manager.sessions[i].name, name) == 0) {
            
            g_session_manager.sessions[i].is_active = 0;
            g_session_manager.session_count--;
            session_save();
            return 0;
        }
    }
    return -1;
}

void session_list(void) {
    print_string("=== Sessions disponibles ===\n");
    int count = 0;
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active) {
            print_string("  - ");
            print_string(g_session_manager.sessions[i].name);
            if (g_session_manager.sessions[i].is_admin) {
                print_string(" [ADMIN]");
            }
            print_string("\n");
            count++;
        }
    }
    
    if (count == 0) {
        print_string("  Aucune session\n");
    }
}

// ============================================================================
// FONCTIONS D'AUTHENTIFICATION
// ============================================================================

int session_login_menu(void) {
    clear_screen();
    
    // Compter les sessions actives
    int active_sessions[MAX_SESSIONS];
    int active_count = 0;
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active) {
            active_sessions[active_count++] = i;
        }
    }
    
    if (active_count == 0) {
        return -1; // Aucune session
    }
    
    int selected = 0;
    
    while (1) {
        clear_screen();
        print_string("========================================\n");
        print_string("       TetraOS - Selection Session      \n");
        print_string("========================================\n\n");
        
        print_string("Utilisez <- et -> pour naviguer\n");
        print_string("Appuyez sur Entree pour selectionner\n\n");
        
        // Afficher les sessions avec indicateur de sélection
        for (int i = 0; i < active_count; i++) {
            Session* s = &g_session_manager.sessions[active_sessions[i]];
            
            if (i == selected) {
                print_string("  >>> ");
            } else {
                print_string("      ");
            }
            
            print_string(s->name);
            
            if (s->is_admin) {
                print_string(" [ADMIN]");
            }
            
            print_string("\n");
        }
        
        // Attendre une entrée clavier
        char c = keyboard_get_char();
        
        // Flèche gauche (codes peuvent varier)
        if (c == 'h' || c == 'H' || c == 75) { // h ou flèche gauche
            if (selected > 0) {
                selected--;
            }
        }
        // Flèche droite
        else if (c == 'l' || c == 'L' || c == 77) { // l ou flèche droite
            if (selected < active_count - 1) {
                selected++;
            }
        }
        // Entrée
        else if (c == '\n' || c == '\r') {
            return active_sessions[selected];
        }
    }
}

int session_login(int session_index, const char* password) {
    if (session_index < 0 || session_index >= MAX_SESSIONS) {
        return -1;
    }
    
    Session* s = &g_session_manager.sessions[session_index];
    
    if (!s->is_active) {
        return -1;
    }
    
    if (session_verify_password(password, s->password)) {
        g_session_manager.current_session = s;
        g_session_manager.current_session_index = session_index;
        g_session_manager.logged_in = 1;
        return 0;
    }
    
    return -1;
}

void session_logout(void) {
    g_session_manager.current_session = NULL;
    g_session_manager.current_session_index = -1;
    g_session_manager.logged_in = 0;
}

// ============================================================================
// FONCTIONS DE PERMISSIONS
// ============================================================================

int session_has_permission(Permission perm) {
    if (!g_session_manager.logged_in || !g_session_manager.current_session) {
        return 0;
    }
    
    return (g_session_manager.current_session->permissions & (1 << perm)) != 0;
}

void session_set_permission(int session_index, Permission perm, int value) {
    if (session_index < 0 || session_index >= MAX_SESSIONS) {
        return;
    }
    
    Session* s = &g_session_manager.sessions[session_index];
    
    if (!s->is_active) {
        return;
    }
    
    if (value) {
        s->permissions |= (1 << perm);
    } else {
        s->permissions &= ~(1 << perm);
    }
    
    session_save();
}

void session_grant_all_permissions(int session_index) {
    if (session_index < 0 || session_index >= MAX_SESSIONS) {
        return;
    }
    
    Session* s = &g_session_manager.sessions[session_index];
    s->permissions = 0xFFFFFFFF; // Toutes les permissions
}

// ============================================================================
// FONCTIONS UTILITAIRES
// ============================================================================

// Hash simple (XOR + rotation) - Pas sécurisé mais suffisant pour un OS éducatif
void session_hash_password(const char* password, char* hash_out) {
    uint32_t hash = 0x5A5A5A5A;
    
    for (int i = 0; password[i] != '\0' && i < 256; i++) {
        hash ^= (uint32_t)password[i];
        hash = (hash << 7) | (hash >> 25); // Rotation
        hash ^= 0x12345678;
    }
    
    // Convertir en chaîne hexadécimale
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        hash_out[i * 2] = hex[(hash >> (28 - i * 4)) & 0xF];
        hash_out[i * 2 + 1] = hex[(hash >> (24 - i * 4)) & 0xF];
    }
    hash_out[16] = '\0';
}

int session_verify_password(const char* password, const char* hash) {
    char computed_hash[SESSION_PASSWORD_LEN];
    session_hash_password(password, computed_hash);
    return strcmp(computed_hash, hash) == 0;
}

const char* session_get_current_name(void) {
    if (g_session_manager.logged_in && g_session_manager.current_session) {
        return g_session_manager.current_session->name;
    }
    return "Guest";
}

int session_is_admin(void) {
    if (g_session_manager.logged_in && g_session_manager.current_session) {
        return g_session_manager.current_session->is_admin;
    }
    return 0;
}