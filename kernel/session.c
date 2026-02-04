// session.c - Implémentation du système de sessions (VERSION CORRIGÉE - BUG FS)
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
        
        // Réinitialiser le compteur
        g_session_manager.session_count = 0;
        
        for (int i = 0; i < num_sessions; i++) {
            Session* src = (Session*)&buffer[i * sizeof(Session)];
            if (src->is_active) {
                memcpy(&g_session_manager.sessions[i], src, sizeof(Session));
                g_session_manager.session_count++;
            }
        }
        
        print_string("Sessions chargees: ");
        char count_str[4];
        count_str[0] = '0' + g_session_manager.session_count;
        count_str[1] = '\0';
        print_string(count_str);
        print_string("\n");
    } else {
        print_string("Aucune session sauvegardee trouvee\n");
    }
}

// CORRECTION BUG CRITIQUE : Création du fichier sessions.dat s'il n'existe pas
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
    
    // CORRECTION : Vérifier si le fichier existe, sinon le créer
    int file_idx = fs_find(SESSION_FILE);
    if (file_idx < 0) {
        // Le fichier n'existe pas, le créer
        print_string("Creation du fichier sessions.dat...\n");
        file_idx = fs_add(SESSION_FILE);
        if (file_idx < 0) {
            print_string("ERREUR: Impossible de creer sessions.dat\n");
            return;
        }
    }
    
    // Écrire les données
    int result = fs_write_file(SESSION_FILE, buffer, sizeof(buffer));
    if (result < 0) {
        print_string("ERREUR: Impossible d'ecrire sessions.dat\n");
    } else {
        print_string("Sessions sauvegardees avec succes\n");
    }
}

// ============================================================================
// FONCTIONS DE GESTION DES SESSIONS
// ============================================================================

int session_create(const char* name, const char* password, uint8_t is_admin) {
    // Vérifier qu'on n'a pas atteint le maximum
    if (g_session_manager.session_count >= MAX_SESSIONS) {
        print_string("ERREUR: Nombre maximum de sessions atteint\n");
        return -1;
    }
    
    // Vérifier que le nom n'existe pas déjà
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active &&
            strcmp(g_session_manager.sessions[i].name, name) == 0) {
            print_string("ERREUR: Session existe deja\n");
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
            
            print_string("Session creee, sauvegarde en cours...\n");
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
    
    print_string("Total: ");
    char count_str[4];
    count_str[0] = '0' + count;
    count_str[1] = '\0';
    print_string(count_str);
    print_string(" session(s)\n");
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
        
        print_string("Utilisez les fleches <- et -> pour naviguer\n");
        print_string("Ou utilisez 'h' (gauche) et 'l' (droite)\n");
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
        
        // Flèche gauche : code 17 (ou 'h'/'H')
        if (c == 17 || c == 'h' || c == 'H') {
            if (selected > 0) {
                selected--;
            }
        }
        // Flèche droite : code 18 (ou 'l'/'L')
        else if (c == 18 || c == 'l' || c == 'L') {
            if (selected < active_count - 1) {
                selected++;
            }
        }
        // Flèche haut : code 16
        else if (c == 16) {
            if (selected > 0) {
                selected--;
            }
        }
        // Flèche bas : code 14
        else if (c == 14) {
            if (selected < active_count - 1) {
                selected++;
            }
        }
        // Entrée
        else if (c == '\n' || c == '\r') {
            return active_sessions[selected];
        }
        // ESC pour quitter
        else if (c == 27) {
            return -1;
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
    // Si aucune session n'existe encore, autoriser tout (premier démarrage)
    if (g_session_manager.session_count == 0) {
        return 1;
    }
    
    // Si personne n'est connecté mais des sessions existent, refuser
    if (!g_session_manager.logged_in || !g_session_manager.current_session) {
        return 0;
    }
    
    // Vérifier les permissions de la session
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

// ============================================================================
// FONCTION DE LOGIN COMPLÈTE - AUTONOME
// ============================================================================

// Fonction utilitaire locale pour lire une ligne SANS vérification de permissions
static void read_line_raw(char* buffer, int max_len, int hide_password) {
    int idx = 0;
    extern char keyboard_get_char(void);
    extern void print_char(char c);
    extern void print_string(const char* str);
    
    while (1) {
        char c = keyboard_get_char();
        
        if (c == '\n' || c == '\r') {
            buffer[idx] = '\0';
            print_char('\n');
            break;
        }
        else if ((c == '\b' || c == 127) && idx > 0) {
            idx--;
            print_string("\b \b");
        }
        else if (c >= 32 && c <= 126 && idx < max_len - 1) {
            buffer[idx++] = c;
            if (hide_password) {
                print_char('*');
            } else {
                print_char(c);
            }
        }
    }
}

int session_do_login_flow(void) {
    extern void clear_screen(void);
    extern void print_string(const char* str);
    extern char keyboard_get_char(void);
    
    // Vérifier s'il y a des sessions
    int has_sessions = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active) {
            has_sessions = 1;
            break;
        }
    }
    
    // Si aucune session n'existe, créer un admin
    if (!has_sessions) {
        clear_screen();
        print_string("========================================\n");
        print_string("    PREMIER DEMARRAGE - TetraOS        \n");
        print_string("========================================\n\n");
        print_string("Aucune session trouvee.\n");
        print_string("Creation d'une session administrateur...\n\n");
        
        char admin_name[SESSION_NAME_LEN];
        char admin_password[SESSION_PASSWORD_LEN];
        
        print_string("Nom de l'administrateur: ");
        read_line_raw(admin_name, SESSION_NAME_LEN, 0);
        
        print_string("Mot de passe: ");
        read_line_raw(admin_password, SESSION_PASSWORD_LEN, 1);
        
        print_string("\n--- DEBUG: Creation de la session ---\n");
        print_string("Nom: ");
        print_string(admin_name);
        print_string("\n");
        
        // Créer la session admin
        int result = session_create(admin_name, admin_password, 1);
        
        if (result < 0) {
            print_string("\nERREUR CRITIQUE: Impossible de creer la session!\n");
            print_string("Verifiez le systeme de fichiers.\n");
            print_string("Appuyez sur une touche...\n");
            keyboard_get_char();
            return -1;
        }
        
        // Se connecter automatiquement
        g_session_manager.current_session = &g_session_manager.sessions[result];
        g_session_manager.current_session_index = result;
        g_session_manager.logged_in = 1;
        
        print_string("\nSession admin creee avec succes!\n");
        print_string("Index: ");
        char idx_str[4];
        idx_str[0] = '0' + result;
        idx_str[1] = '\0';
        print_string(idx_str);
        print_string("\n");
        print_string("Connexion automatique...\n");
        print_string("Appuyez sur une touche pour continuer...\n");
        keyboard_get_char();
        
        return 0;
    }
    
    // Boucle de tentatives
    int max_attempts = 3;
    
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        // Menu de sélection
        int selected_session = session_login_menu();
        
        if (selected_session < 0) {
            print_string("Annulation du login ou aucune session disponible\n");
            return -1;
        }
        
        // Écran de login
        clear_screen();
        print_string("========================================\n");
        print_string("          TetraOS - Connexion           \n");
        print_string("========================================\n\n");
        
        print_string("Session: ");
        print_string(g_session_manager.sessions[selected_session].name);
        print_string("\n\n");
        
        char password[SESSION_PASSWORD_LEN];
        print_string("Mot de passe: ");
        read_line_raw(password, SESSION_PASSWORD_LEN, 1);
        
        // Tentative de connexion
        if (session_login(selected_session, password) == 0) {
            print_string("\nConnexion reussie!\n");
            print_string("Bienvenue ");
            print_string(session_get_current_name());
            print_string("!\n\n");
            print_string("Appuyez sur une touche pour continuer...\n");
            keyboard_get_char();
            return 0;
        } else {
            print_string("\nMot de passe incorrect!\n");
            
            if (attempt < max_attempts - 1) {
                print_string("Tentatives restantes: ");
                char num[2];
                num[0] = '0' + (max_attempts - attempt - 1);
                num[1] = '\0';
                print_string(num);
                print_string("\n");
                print_string("Appuyez sur une touche pour reessayer...\n");
                keyboard_get_char();
            }
        }
    }
    
    // Toutes les tentatives échouées
    print_string("\nNombre maximum de tentatives atteint.\n");
    print_string("Le systeme va redemarrer.\n");
    print_string("Appuyez sur une touche...\n");
    keyboard_get_char();
    
    return -1;
}