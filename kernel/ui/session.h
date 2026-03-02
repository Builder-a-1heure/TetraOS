// session.h - Système de gestion des sessions utilisateur pour TetraOS
#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>

// Constantes
#define MAX_SESSIONS 8
#define SESSION_NAME_LEN 32
#define SESSION_PASSWORD_LEN 64
#define MAX_PERMISSIONS 32

// Types de permissions
typedef enum {
    PERM_FS_READ = 0,      // Lire des fichiers
    PERM_FS_WRITE,         // Écrire des fichiers
    PERM_FS_DELETE,        // Supprimer des fichiers
    PERM_TEX_EXECUTE,      // Exécuter des scripts .tex
    PERM_SESSION_CREATE,   // Créer des sessions (admin)
    PERM_SESSION_DELETE,   // Supprimer des sessions (admin)
    PERM_SESSION_EDIT,     // Modifier les permissions (admin)
    PERM_SYSTEM_SHUTDOWN,  // Éteindre le système
    PERM_CLEAR_SCREEN,     // Effacer l'écran
    PERM_LIST_FILES,       // Lister les fichiers
    PERM_CONFIG_READ,      // Lire les fichiers de config de session
    PERM_CONFIG_WRITE,     // Écrire / modifier les fichiers de config de session
} Permission;

// Structure d'une session utilisateur
typedef struct {
    char name[SESSION_NAME_LEN];           // Nom de la session
    char password[SESSION_PASSWORD_LEN];   // Mot de passe (hashé)
    uint8_t is_active;                     // 1 si la session existe
    uint8_t is_admin;                      // 1 si admin
    uint32_t permissions;                  // Bitmap des permissions
    uint32_t home_dir_node;                // Index FSNode du répertoire home de la session
} Session;

// Contexte de session actuelle
typedef struct {
    Session* current_session;              // Session active
    int current_session_index;             // Index de la session active
    Session sessions[MAX_SESSIONS];        // Toutes les sessions
    int session_count;                     // Nombre de sessions
    uint8_t logged_in;                     // 1 si connecté
} SessionManager;

// Gestionnaire global
extern SessionManager g_session_manager;

// === Fonctions d'initialisation ===

// Initialise le système de sessions
void session_init(void);

// Charge les sessions depuis le disque
void session_load(void);

// Sauvegarde les sessions sur le disque
void session_save(void);

// === Fonctions de gestion des sessions ===

// Crée une nouvelle session
// Retourne 0 si succès, -1 si erreur
int session_create(const char* name, const char* password, uint8_t is_admin);

// Supprime une session
int session_delete(const char* name);

// Liste toutes les sessions disponibles
void session_list(void);

// === Fonctions d'authentification ===

// Menu de connexion (affiché au démarrage)
// Retourne l'index de la session sélectionnée
int session_login_menu(void);

// Tente de se connecter avec un mot de passe
// Retourne 0 si succès, -1 si échec
int session_login(int session_index, const char* password);

// Déconnexion
void session_logout(void);

// === Fonctions de permissions ===

// Vérifie si la session actuelle a une permission
// Retourne 1 si oui, 0 sinon
int session_has_permission(Permission perm);

// Définit une permission pour une session
void session_set_permission(int session_index, Permission perm, int value);

// Donne toutes les permissions à une session (admin)
void session_grant_all_permissions(int session_index);

// === Fonction de login complète ===

// Gère TOUT le processus de login de A à Z (menu, saisie, vérification)
// Cette fonction est AUTONOME et NE vérifie PAS les permissions
// Retourne 0 si login réussi, -1 si erreur
int session_do_login_flow(void);

// === Fonctions de gestion du répertoire home ===

// Crée le répertoire home d'une session dans /home/<nom>
// Retourne l'index FSNode du répertoire créé, -1 si erreur
int session_create_home_dir(int session_index);

// Place le cwd sur le répertoire home de la session connectée
void session_set_cwd_to_home(void);

// Accès admin : depuis root, liste le contenu de /home/<session_name>
// Réservé aux sessions admin
void session_admin_browse_home(const char* session_name);

// === Fonctions utilitaires ===

// Hash simple d'un mot de passe
void session_hash_password(const char* password, char* hash_out);

// Compare un mot de passe avec son hash
int session_verify_password(const char* password, const char* hash);

// Obtient le nom de la session actuelle
const char* session_get_current_name(void);

// Vérifie si la session actuelle est admin
int session_is_admin(void);

// Retourne l'index (UID) de la session courante dans g_session_manager.sessions[]
// Retourne UID_SYSTEM (0xFFFF) si personne n'est connecté
uint16_t session_get_uid(void);

// Retourne l'index d'une session par son nom, -1 si introuvable
int session_get_index_by_name(const char* name);

// Retourne le nom d'une session par son uid (index), NULL si invalide
const char* session_get_name_by_uid(uint16_t uid);

#endif // SESSION_H