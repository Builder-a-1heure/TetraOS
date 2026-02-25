// session.c - Implémentation du système de sessions
#include "session.h"
#include "screen.h"
#include "vesa.h"
#include "input.h"
#include "fs.h"
#include "utils.h"

// Gestionnaire global de sessions
SessionManager g_session_manager;

// Nom du fichier de sauvegarde des sessions
#define SESSION_FILE "sessions.dat"
// Nom du répertoire racine des homes utilisateurs
#define HOME_ROOT_NAME "home"

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
        g_session_manager.sessions[i].home_dir_node = 0;
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
// FONCTIONS DE RÉPERTOIRE HOME
// ============================================================================

// Obtient ou crée /home dans la racine FS (node index 0 = root)
static int session_get_or_create_home_root(void) {
    // Chercher /home dans la racine
    int home_idx = fs_find_in_dir(0, HOME_ROOT_NAME);
    if (home_idx >= 0) return home_idx;

    // Créer /home
    print_string("Creation du dossier /home...\n");
    int result = fs_mkdir_in_dir(0, HOME_ROOT_NAME);
    if (result < 0) {
        print_string("ERREUR: Impossible de creer /home\n");
    }
    return result;
}

int session_create_home_dir(int session_index) {
    if (session_index < 0 || session_index >= MAX_SESSIONS) return -1;
    Session* s = &g_session_manager.sessions[session_index];
    if (!s->is_active) return -1;

    // S'assurer que /home existe
    int home_root = session_get_or_create_home_root();
    if (home_root < 0) return -1;

    // ACL de /home : admin seul peut y entrer et lister
    fs_acl_set_node((uint32_t)home_root, ACL_ADMIN_FULL, UID_SYSTEM);

    // Creer /home/<n>
    int user_home = fs_mkdir_in_dir((uint32_t)home_root, s->name);
    if (user_home < 0) {
        print_string("ERREUR: Impossible de creer /home/");
        print_string(s->name);
        print_string("\n");
        return -1;
    }

    // ACL home : owner rwx, admin rwx, others ---
    fs_acl_set_node((uint32_t)user_home,
                    ACL_OWNER_FULL | ACL_ADMIN_FULL,
                    (uint16_t)session_index);

    s->home_dir_node = (uint32_t)user_home;
    print_string("Repertoire home cree: /home/");
    print_string(s->name);
    print_string(" [rwxrwx---]\n");
    return user_home;
}

void session_set_cwd_to_home(void) {
    if (!g_session_manager.logged_in || !g_session_manager.current_session) return;

    uint32_t home = g_session_manager.current_session->home_dir_node;

    // Si home_dir_node non initialisé (session ancienne chargée depuis disque)
    if (home == 0) {
        int idx = g_session_manager.current_session_index;
        int created = session_create_home_dir(idx);
        if (created >= 0) {
            home = g_session_manager.current_session->home_dir_node;
            session_save(); // Persister le home_dir_node
        } else {
            // Fallback sur la racine
            g_cwd = 0;
            return;
        }
    }

    g_cwd = home;
}

// Admin : se déplacer dans le home d'une session spécifique
void session_admin_browse_home(const char* session_name) {
    if (!session_is_admin()) {
        print_string("ERREUR: Acces refuse. Administrateur requis.\n");
        return;
    }

    // Chercher /home dans la racine
    int home_root = fs_find_in_dir(0, HOME_ROOT_NAME);
    if (home_root < 0) {
        print_string("ERREUR: Repertoire /home introuvable.\n");
        return;
    }

    // Chercher /home/<session_name>
    int user_home = fs_find_in_dir((uint32_t)home_root, session_name);
    if (user_home < 0) {
        print_string("ERREUR: Aucun home pour la session '");
        print_string(session_name);
        print_string("'.\n");
        return;
    }

    g_cwd = (uint32_t)user_home;
    print_string("Admin: navigation vers /home/");
    print_string(session_name);
    print_string("\n");
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
            s->home_dir_node = 0; // Sera initialisé par session_create_home_dir
            
            // Définir les permissions
            if (is_admin) {
                session_grant_all_permissions(i);
            } else {
                // Permissions de base pour utilisateur normal
                // Inclut lecture ET écriture des fichiers de config
                // de sa propre session (sessions.dat + home config)
                s->permissions = (1 << PERM_FS_READ)      |
                                 (1 << PERM_FS_WRITE)     |
                                 (1 << PERM_FS_DELETE)    |
                                 (1 << PERM_LIST_FILES)   |
                                 (1 << PERM_CLEAR_SCREEN) |
                                 (1 << PERM_CONFIG_READ)  |
                                 (1 << PERM_CONFIG_WRITE);
            }
            
            g_session_manager.session_count++;
            
            // Créer le répertoire home /home/<nom>
            session_create_home_dir(i);
            
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

// ============================================================
// DESSIN UI LOGIN — helpers internes
// ============================================================

// Couleurs UI
#define UI_BG_TOP    0x00000D1A   // bleu nuit profond
#define UI_BG_BOT    0x00000000   // noir
#define UI_ACCENT    0x000066CC   // bleu vif
#define UI_ACCENT2   0x000033AA   // bleu sombre
#define UI_WHITE     0x00FFFFFF
#define UI_GRAY      0x00AAAAAA
#define UI_DARK      0x00222222
#define UI_SELECTED  0x000055BB
#define UI_ADMIN     0x00CC6600
#define UI_GREEN     0x0022CC66
#define UI_RED       0x00CC2222

static void ui_draw_background(void) {
    uint32_t sw = vesa_width();
    uint32_t sh = vesa_height();
    // Dégradé vertical bleu nuit → noir
    gfx_gradient_v(0, 0, (int)sw, (int)sh, UI_BG_TOP, UI_BG_BOT);
    // Ligne décorative en haut
    gfx_fill_rect(0, 0, (int)sw, 3, UI_ACCENT);
    // Ligne décorative en bas
    gfx_fill_rect(0, (int)sh - 3, (int)sw, 3, UI_ACCENT2);
}

static void ui_draw_logo(int cx, int y) {
    // "TetraOS" en grand avec cercle décoratif
    gfx_draw_circle(cx, y + 28, 38, UI_ACCENT);
    gfx_draw_circle(cx, y + 28, 34, UI_ACCENT2);
    gfx_draw_text_centered(cx - 120, y + 20, 240, "TetraOS", UI_WHITE,  0x00000000);
    gfx_draw_text_centered(cx - 120, y + 38, 240, "v1.0",    UI_GRAY,   0x00000000);
}

static void ui_draw_panel(int x, int y, int w, int h) {
    // Fond semi-sombre du panel
    gfx_fill_rect(x, y, w, h, UI_DARK);
    // Bordure accent
    gfx_draw_rect(x, y, w, h, UI_ACCENT);
    // Ligne de titre
    gfx_fill_rect(x + 1, y + 1, w - 2, 2, UI_ACCENT);
}

static void ui_draw_button(int x, int y, int w, int h,
                           const char* label, int active) {
    uint32_t bg = active ? UI_SELECTED : UI_DARK;
    uint32_t bd = active ? UI_WHITE    : UI_ACCENT2;
    uint32_t fg = active ? UI_WHITE    : UI_GRAY;
    gfx_fill_rect(x, y, w, h, bg);
    gfx_draw_rect(x, y, w, h, bd);
    if (active) {
        // Petit triangle indicateur à gauche
        for (int t = 0; t < 5; t++)
            gfx_fill_rect(x + 6 + t, y + h/2 - t, 1, 2*t + 1, UI_WHITE);
    }
    gfx_draw_text_centered(x, y + (h - FONT_H) / 2, w, label, fg, bg);
}

int session_login_menu(void) {
    // Compter les sessions actives
    int active_sessions[MAX_SESSIONS];
    int active_count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active)
            active_sessions[active_count++] = i;
    }
    if (active_count == 0) return -1;

    int selected = 0;
    int prev_selected = -1; // Forcer le premier dessin complet

    while (1) {
        uint32_t sw = vesa_width();
        uint32_t sh = vesa_height();
        int cx = (int)sw / 2;

        int panel_w = 420;
        int panel_h = 60 + active_count * 56 + 40;
        int panel_x = cx - panel_w / 2;
        int panel_y = (int)sh / 2 - 60;

        // Dessin complet uniquement au premier passage
        if (prev_selected < 0) {
            screen_begin_ui();
            ui_draw_background();
            ui_draw_logo(cx, (int)sh / 2 - 180);
            ui_draw_panel(panel_x, panel_y, panel_w, panel_h);
            gfx_draw_text_centered(panel_x, panel_y + 14, panel_w,
                                   "Selectionnez un utilisateur",
                                   UI_WHITE, UI_DARK);
            // Hint navigation
            gfx_draw_text_centered(panel_x, panel_y + panel_h - 26, panel_w,
                                   "^ v  Naviguer    Entree  Valider",
                                   UI_GRAY, UI_DARK);
            // Tous les boutons
            for (int i = 0; i < active_count; i++) {
                Session* s = &g_session_manager.sessions[active_sessions[i]];
                int btn_y = panel_y + 46 + i * 56;
                ui_draw_button(panel_x + 20, btn_y, panel_w - 40, 40,
                               s->name, i == selected);
                if (s->is_admin) {
                    gfx_fill_rect(panel_x + panel_w - 80, btn_y + 8, 54, 22, UI_ADMIN);
                    gfx_draw_text(panel_x + panel_w - 76, btn_y + 11,
                                  "ADMIN", UI_WHITE, UI_ADMIN);
                }
            }
            screen_end_ui();
        } else if (selected != prev_selected) {
            // Mise à jour partielle : redessiner seulement les deux boutons modifiés
            screen_begin_ui();
            // Ancien bouton sélectionné → déselectionné
            {
                Session* s = &g_session_manager.sessions[active_sessions[prev_selected]];
                int btn_y = panel_y + 46 + prev_selected * 56;
                ui_draw_button(panel_x + 20, btn_y, panel_w - 40, 40, s->name, 0);
                if (s->is_admin) {
                    gfx_fill_rect(panel_x + panel_w - 80, btn_y + 8, 54, 22, UI_ADMIN);
                    gfx_draw_text(panel_x + panel_w - 76, btn_y + 11,
                                  "ADMIN", UI_WHITE, UI_ADMIN);
                }
            }
            // Nouveau bouton sélectionné → actif
            {
                Session* s = &g_session_manager.sessions[active_sessions[selected]];
                int btn_y = panel_y + 46 + selected * 56;
                ui_draw_button(panel_x + 20, btn_y, panel_w - 40, 40, s->name, 1);
                if (s->is_admin) {
                    gfx_fill_rect(panel_x + panel_w - 80, btn_y + 8, 54, 22, UI_ADMIN);
                    gfx_draw_text(panel_x + panel_w - 76, btn_y + 11,
                                  "ADMIN", UI_WHITE, UI_ADMIN);
                }
            }
            screen_end_ui();
        }
        prev_selected = selected;

        char c = keyboard_get_char();

        if (c == 16 || c == 17 || c == 'h' || c == 'H') {
            if (selected > 0) selected--;
        } else if (c == 14 || c == 18 || c == 'l' || c == 'L') {
            if (selected < active_count - 1) selected++;
        } else if (c == '\n' || c == '\r') {
            return active_sessions[selected];
        } else if (c == 27) {
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
        // Placer le cwd sur le répertoire home de la session
        session_set_cwd_to_home();
        return 0;
    }
    
    return -1;
}

void session_logout(void) {
    g_session_manager.current_session = NULL;
    g_session_manager.current_session_index = -1;
    g_session_manager.logged_in = 0;
    // Retour à la racine du filesystem
    g_cwd = 0;
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

uint16_t session_get_uid(void) {
    if (g_session_manager.logged_in && g_session_manager.current_session_index >= 0) {
        return (uint16_t)g_session_manager.current_session_index;
    }
    return 0xFFFF; // UID_SYSTEM
}

int session_get_index_by_name(const char* name) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_manager.sessions[i].is_active &&
            strcmp(g_session_manager.sessions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const char* session_get_name_by_uid(uint16_t uid) {
    if (uid == 0xFFFF || uid == 0xFFFE) return "system";
    if (uid >= MAX_SESSIONS) return NULL;
    if (!g_session_manager.sessions[uid].is_active) return NULL;
    return g_session_manager.sessions[uid].name;
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
    extern void screen_clear_visible(void);
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
    
    // --- Premier démarrage ---
    if (!has_sessions) {
        uint32_t sw = vesa_width();
        uint32_t sh = vesa_height();
        int cx = (int)sw / 2;

        // Fond statique — un seul flush
        screen_begin_ui();
        ui_draw_background();
        ui_draw_logo(cx, (int)sh / 2 - 220);

        // Panel de setup
        int panel_w = 440;
        int panel_h = 260;
        int panel_x = cx - panel_w / 2;
        int panel_y = (int)sh / 2 - 100;
        ui_draw_panel(panel_x, panel_y, panel_w, panel_h);

        gfx_draw_text_centered(panel_x, panel_y + 14, panel_w,
                               "Premier demarrage", UI_WHITE, UI_DARK);
        gfx_draw_text_centered(panel_x, panel_y + 40, panel_w,
                               "Creez votre compte administrateur",
                               UI_GRAY, UI_DARK);

        // Zone nom — fond fixe
        int nw = panel_w - 40;
        int nx = panel_x + 20, ny = panel_y + 92;
        gfx_draw_text(panel_x + 20, panel_y + 72,
                      "Nom :", UI_GRAY, UI_DARK);
        gfx_fill_rect(nx, ny, nw, 28, 0x00111111);
        gfx_draw_rect(nx, ny, nw, 28, UI_ACCENT);

        // Zone password — fond fixe
        int px2 = panel_x + 20, py2 = panel_y + 152;
        gfx_draw_text(panel_x + 20, panel_y + 132,
                      "Mot de passe :", UI_GRAY, UI_DARK);
        gfx_fill_rect(px2, py2, nw, 28, 0x00111111);
        gfx_draw_rect(px2, py2, nw, 28, UI_ACCENT);
        screen_end_ui(); // Flush fond complet

        // Saisie nom — ne redessine que le curseur/texte
        char admin_name[SESSION_NAME_LEN];
        int ni = 0;
        while (1) {
            screen_begin_ui();
            gfx_fill_rect(nx+1, ny+1, nw-2, 26, 0x00111111);
            for (int k = 0; k < ni; k++) {
                char tmp[2]; tmp[0] = admin_name[k]; tmp[1] = '\0';
                gfx_draw_text(nx + 6 + k*FONT_W, ny + 6, tmp, UI_WHITE, 0x00111111);
            }
            gfx_fill_rect(nx + 6 + ni*FONT_W, ny+6, 2, FONT_H-4, UI_ACCENT);
            screen_end_ui();
            char c = keyboard_get_char();
            if (c == '\n' || c == '\r') { admin_name[ni] = '\0'; break; }
            else if ((c == '\b' || c == 127) && ni > 0) ni--;
            else if (c >= 32 && c <= 126 && ni < SESSION_NAME_LEN-1)
                admin_name[ni++] = c;
        }

        // Saisie mot de passe — idem
        char admin_password[SESSION_PASSWORD_LEN];
        int pi = 0;
        while (1) {
            screen_begin_ui();
            gfx_fill_rect(px2+1, py2+1, nw-2, 26, 0x00111111);
            for (int k = 0; k < pi; k++)
                gfx_draw_text(px2 + 6 + k*FONT_W, py2+6, "*", UI_WHITE, 0x00111111);
            gfx_fill_rect(px2 + 6 + pi*FONT_W, py2+6, 2, FONT_H-4, UI_ACCENT);
            screen_end_ui();
            char c = keyboard_get_char();
            if (c == '\n' || c == '\r') { admin_password[pi] = '\0'; break; }
            else if ((c == '\b' || c == 127) && pi > 0) pi--;
            else if (c >= 32 && c <= 126 && pi < SESSION_PASSWORD_LEN-1)
                admin_password[pi++] = c;
        }

        int result = session_create(admin_name, admin_password, 1);
        if (result < 0) {
            screen_begin_ui();
            gfx_draw_text_centered(panel_x, panel_y + panel_h - 30, panel_w,
                                   "ERREUR: creation impossible", UI_RED, UI_DARK);
            screen_end_ui();
            keyboard_get_char();
            screen_exit_ui();
            return -1;
        }

        g_session_manager.current_session = &g_session_manager.sessions[result];
        g_session_manager.current_session_index = result;
        g_session_manager.logged_in = 1;
        session_set_cwd_to_home();

        // Confirmation
        screen_begin_ui();
        ui_draw_background();
        int cw = 420, ch2 = 110;
        ui_draw_panel(cx - cw/2, (int)sh/2 - ch2/2, cw, ch2);
        gfx_draw_text_centered(cx - cw/2, (int)sh/2 - 36, cw,
                               "Compte cree !", UI_GREEN, UI_DARK);
        gfx_draw_text_centered(cx - cw/2, (int)sh/2 - 10, cw,
                               admin_name, UI_WHITE, UI_DARK);
        gfx_draw_text_centered(cx - cw/2, (int)sh/2 + 18, cw,
                               "Appuyez sur une touche...", UI_GRAY, UI_DARK);
        screen_end_ui();
        keyboard_get_char();
        screen_exit_ui();
        return 0;
    }
    
    // Boucle de tentatives
    int max_attempts = 3;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int selected_session = session_login_menu();
        if (selected_session < 0) return -1;

        // --- Écran de saisie du mot de passe ---
        uint32_t sw = vesa_width();
        uint32_t sh = vesa_height();
        int cx = (int)sw / 2;

        // Dessin statique de l'écran mot de passe — un seul flush
        screen_begin_ui();
        ui_draw_background();
        ui_draw_logo(cx, (int)sh / 2 - 200);

        // Panel mot de passe
        int panel_w = 380;
        int panel_h = 180;
        int panel_x = cx - panel_w / 2;
        int panel_y = (int)sh / 2 - 80;
        ui_draw_panel(panel_x, panel_y, panel_w, panel_h);

        // Titre
        gfx_draw_text_centered(panel_x, panel_y + 14, panel_w,
                               "Connexion", UI_WHITE, UI_DARK);

        // Nom de session
        Session* s = &g_session_manager.sessions[selected_session];
        gfx_draw_text_centered(panel_x, panel_y + 46, panel_w,
                               s->name, UI_ACCENT, UI_DARK);

        // Label "Mot de passe"
        gfx_draw_text(panel_x + 20, panel_y + 78,
                      "Mot de passe :", UI_GRAY, UI_DARK);

        // Zone de saisie
        int input_x = panel_x + 20;
        int input_y = panel_y + 100;
        int input_w = panel_w - 40;
        gfx_fill_rect(input_x, input_y, input_w, 28, 0x00111111);
        gfx_draw_rect(input_x, input_y, input_w, 28, UI_ACCENT);

        // Message d'erreur si tentative précédente échouée
        if (attempt > 0) {
            char msg[40];
            msg[0]='T'; msg[1]='e'; msg[2]='n'; msg[3]='t';
            msg[4]='a'; msg[5]='t'; msg[6]='i'; msg[7]='v';
            msg[8]='e'; msg[9]='s'; msg[10]=' '; msg[11]='r';
            msg[12]='e'; msg[13]='s'; msg[14]='t'; msg[15]='a';
            msg[16]='n'; msg[17]='t'; msg[18]='e'; msg[19]='s';
            msg[20]=' '; msg[21]=':'; msg[22]=' ';
            msg[23]='0' + (max_attempts - attempt);
            msg[24]='\0';
            gfx_draw_text_centered(panel_x, panel_y + 140, panel_w,
                                   msg, UI_RED, UI_DARK);
        }
        screen_end_ui(); // Flush unique pour tout le fond statique

        // Boucle de saisie — seule la zone input est mise à jour
        char password[SESSION_PASSWORD_LEN];
        int idx = 0;
        while (1) {
            // Redessiner uniquement la zone de saisie (pas le fond entier !)
            screen_begin_ui();
            gfx_fill_rect(input_x + 1, input_y + 1, input_w - 2, 26, 0x00111111);
            for (int k = 0; k < idx; k++)
                gfx_draw_text(input_x + 6 + k * FONT_W, input_y + 6,
                              "*", UI_WHITE, 0x00111111);
            // Curseur clignotant
            gfx_fill_rect(input_x + 6 + idx * FONT_W, input_y + 6,
                          2, FONT_H - 4, UI_ACCENT);
            screen_end_ui();

            char c = keyboard_get_char();
            if (c == '\n' || c == '\r') {
                password[idx] = '\0';
                break;
            } else if ((c == '\b' || c == 127) && idx > 0) {
                idx--;
            } else if (c >= 32 && c <= 126 && idx < SESSION_PASSWORD_LEN - 1) {
                password[idx++] = c;
            }
        }

        if (session_login(selected_session, password) == 0) {
            // Écran de bienvenue
            screen_begin_ui();
            ui_draw_background();
            int bw = 500, bh = 120;
            int bx = cx - bw/2, by = (int)sh/2 - bh/2;
            ui_draw_panel(bx, by, bw, bh);
            gfx_draw_text_centered(bx, by + 20, bw,
                                   "Connexion reussie !", UI_GREEN, UI_DARK);
            gfx_draw_text_centered(bx, by + 50, bw,
                                   "Bienvenue,", UI_GRAY, UI_DARK);
            gfx_draw_text_centered(bx, by + 74, bw,
                                   s->name, UI_WHITE, UI_DARK);
            screen_end_ui();
            keyboard_get_char();
            screen_exit_ui();
            return 0;
        }
    }

    // Trop de tentatives
    uint32_t sw2 = vesa_width();
    uint32_t sh2 = vesa_height();
    int cx2 = (int)sw2 / 2;
    screen_begin_ui();
    ui_draw_background();
    int fw = 480, fh = 100;
    ui_draw_panel(cx2 - fw/2, (int)sh2/2 - fh/2, fw, fh);
    gfx_draw_text_centered(cx2 - fw/2, (int)sh2/2 - 30, fw,
                           "Acces refuse", UI_RED, UI_DARK);
    gfx_draw_text_centered(cx2 - fw/2, (int)sh2/2,     fw,
                           "Trop de tentatives", UI_GRAY, UI_DARK);
    screen_end_ui();
    keyboard_get_char();
    screen_exit_ui();
    return -1;
}