// apps/fileman.c — Explorateur de fichiers TetraOS
//
// Application autonome déclarée sur le bureau via TEX_APP.
// Permet de naviguer dans le système de fichiers RAY64, d'ouvrir
// des fichiers dans TextEdit et de gérer des fichiers/dossiers.
//
// Architecture AppCore v2 :
//   - Fenêtre principale avec ListBox pour la liste de fichiers
//   - Barre d'adresse (Label) affichant le chemin courant
//   - Boutons d'action : [↑ Parent] [Nouveau dossier] [Supprimer]
//   - Double-clic sur dossier → navigation
//   - Double-clic sur fichier → ouverture dans TextEdit
//
// Dépendances :
//   lib/appcore.h, fs/fs.h, apps/textedit.h, lib/utils.h, apps/app.h
//
// Signature TEX :
//   TEX_APP place un TexHeader de 32 octets en .rodata.
//   desktop.c le trouve en scannant la mémoire kernel.

#include "app.h"
#include "fileman.h"
#include "textedit.h"
#include "../lib/appcore.h"
#include "../lib/utils.h"
#include "../fs/fs.h"
#include "../ui/session.h"
#include <stdint.h>

// ============================================================
// ── SIGNATURE TEX ────────────────────────────────────────────
// ============================================================
TEX_APP("FileMan", APPICON_FILEMAN, 1, 0,
        APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, app_fileman);

// ============================================================
// ── CONSTANTES ───────────────────────────────────────────────
// ============================================================
#define FM_WIN_X        60
#define FM_WIN_Y        40
#define FM_WIN_W        620
#define FM_WIN_H        480

// Layout interne (relatif au client de la fenêtre)
#define FM_PAD          8
#define FM_BAR_H        22      // hauteur barre d'adresse
#define FM_BTN_H        26
#define FM_BTN_W_SM     90
#define FM_BTN_W_DEL    80
#define FM_BTN_W_NEW   110
#define FM_BTN_W_NEWF  100   // "Nouveau fichier"
#define FM_STATUS_H     20
#define FM_ITEM_H       20

// Couleurs spécifiques
#define FM_BAR_BG       0x00060B12
#define FM_BAR_FG       0x0066AADD
#define FM_STATUS_BG    0x00050A10
#define FM_STATUS_FG    0x00445566
#define FM_STATUS_OK    0x0033AA55
#define FM_STATUS_ERR   0x00CC3333
#define FM_TITLE_FG     0x0088CCFF

// Largeur max pour chemin affiché
#define FM_PATH_MAX     256
#define FM_NAME_MAX     48

// ============================================================
// ── HELPERS CHAÎNES (bare-metal, pas de stdlib) ──────────────
// ============================================================
static int fm_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void fm_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void fm_strcat(char* dst, const char* src, int max) {
    int dlen = fm_strlen(dst);
    int i = 0;
    while (dlen + i < max - 1 && src[i]) { dst[dlen + i] = src[i]; i++; }
    dst[dlen + i] = '\0';
}

static int fm_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// Formate un entier décimal dans buf
static void fm_itoa(uint32_t val, char* buf, int bufsz) {
    if (bufsz <= 0) return;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int i = 0;
    while (val && i < 15) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0 && j < bufsz - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// Formate une taille en octets de façon lisible (B, KB, MB)
static void fm_fmt_size(uint32_t bytes, char* buf, int bufsz) {
    if (bytes < 1024) {
        fm_itoa(bytes, buf, bufsz - 2);
        fm_strcat(buf, " B", bufsz);
    } else if (bytes < 1024 * 1024) {
        fm_itoa(bytes / 1024, buf, bufsz - 3);
        fm_strcat(buf, " KB", bufsz);
    } else {
        fm_itoa(bytes / (1024 * 1024), buf, bufsz - 3);
        fm_strcat(buf, " MB", bufsz);
    }
}

// ============================================================
// ── ÉTAT GLOBAL DE L'EXPLORATEUR ────────────────────────────
// ============================================================
static struct {
    uint32_t dir_idx;       // Index FSNode du dossier courant affiché
    char     path[FM_PATH_MAX]; // Chemin affiché dans la barre d'adresse
} g_fm;

// ============================================================
// ── CALCUL DU CHEMIN ─────────────────────────────────────────
// ============================================================
// Remonte de dir_idx vers la racine et construit le chemin complet.
static void fm_build_path(uint32_t dir_idx, char* out, int max) {
    // Tableau pour stocker les segments (de feuille à racine)
    const char* parts[32];
    int depth = 0;

    uint32_t cur = dir_idx;
    while (cur != 0 && depth < 31) {
        parts[depth++] = g_fs.nodes[cur].name;
        uint32_t parent = g_fs.nodes[cur].parent;
        if (parent == cur) break; // protection boucle infinie
        cur = parent;
    }

    // Construire la chaîne de la racine vers la feuille
    out[0] = '/';
    out[1] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        fm_strcat(out, parts[i], max);
        if (i > 0) fm_strcat(out, "/", max);
    }
}

// ============================================================
// ── REMPLISSAGE DE LA LISTBOX ────────────────────────────────
// ============================================================
// Vide la listbox et la remplit avec le contenu de dir_idx.
// Affiche d'abord les dossiers, puis les fichiers.
static void fm_populate_list(LstID lst, uint32_t dir_idx) {
    app_listbox_clear(lst);

    FSNode* dir = &g_fs.nodes[dir_idx];

    // Passe 1 : dossiers
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t ci = dir->children[i];
        if (ci >= g_fs.node_count) continue;
        FSNode* child = &g_fs.nodes[ci];
        if (!child->is_dir) continue;
        if (child->magic != FS_MAGIC) continue;

        // Formater : "[Dossier] nom"
        char line[FM_NAME_MAX + 16];
        line[0] = '\0';
        fm_strcat(line, "[Dossier] ", sizeof(line));
        fm_strcat(line, child->name, sizeof(line));
        app_listbox_add(lst, line, 1); // is_dir=1 → couleur bleue
    }

    // Passe 2 : fichiers
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t ci = dir->children[i];
        if (ci >= g_fs.node_count) continue;
        FSNode* child = &g_fs.nodes[ci];
        if (child->is_dir) continue;
        if (child->magic != FS_MAGIC) continue;

        // Formater : "nom  (taille)"
        char line[FM_NAME_MAX + 20];
        line[0] = '\0';
        fm_strcat(line, child->name, sizeof(line));

        // Padding jusqu'à la colonne 28 pour aligner les tailles
        int nlen = fm_strlen(line);
        while (nlen < 28 && nlen < (int)sizeof(line) - 1) {
            line[nlen++] = ' ';
        }
        line[nlen] = '\0';

        char sizebuf[16];
        fm_fmt_size(child->size_bytes, sizebuf, sizeof(sizebuf));
        fm_strcat(line, sizebuf, sizeof(line));

        app_listbox_add(lst, line, 0); // is_dir=0 → couleur normale
    }

    // Si vide
    if (dir->child_count == 0) {
        app_listbox_add(lst, "(dossier vide)", 0);
    }
}

// ============================================================
// ── NAVIGATION ───────────────────────────────────────────────
// ============================================================
// Navigue vers un dossier, met à jour l'état et l'interface.
static void fm_navigate_to(uint32_t new_dir_idx,
                            LstID lst, LblID lbl_path, LblID lbl_status) {
    // Vérif ACL
    if (!fs_acl_check(new_dir_idx, ACL_READ)) {
        app_set_label_text(lbl_status, "Acces refuse.");
        app_set_label_color(lbl_status, FM_STATUS_ERR);
        return;
    }

    g_fm.dir_idx = new_dir_idx;
    g_cwd = new_dir_idx; // synchroniser g_cwd (permet ls, mkdir, etc.)

    fm_build_path(new_dir_idx, g_fm.path, FM_PATH_MAX);
    app_set_label_text(lbl_path, g_fm.path);
    app_set_label_color(lbl_path, FM_BAR_FG);

    fm_populate_list(lst, new_dir_idx);

    // Effacer status
    app_set_label_text(lbl_status, "");
}

// ============================================================
// ── DIALOGUE : SAISIE NOM ────────────────────────────────────
// ============================================================
// Affiche une boîte de dialogue modale pour saisir un nom.
// Retourne 1 si OK, 0 si annulé. Résultat dans out (max = out_max).
static int fm_dialog_input(const char* title, const char* prompt,
                            char* out, int out_max) {
    WinID dlg = app_new_window(title, 200, 210, 400, 130);
    app_new_label(dlg, 20, 18, prompt);

    char buf[64]; buf[0] = '\0';
    LblID lbl_input = app_new_label(dlg, 20, 44, "_");
    app_set_label_color(lbl_input, 0x00FFFFFF);

    BtnID btn_ok  = app_new_button(dlg, 250, 88, 60, 28, "OK");
    BtnID btn_ann = app_new_button(dlg, 318, 88, 70, 28, "Annuler");

    int result = 0;
    int done   = 0;

    while (app_running() && !done) {
        app_tick();
        // Touche lue par app_tick — pas de double lecture du port
        char c = app_tick_get_key();

        if (c) {
            if (c == '\n' || c == '\r') {
                result = 1; done = 1;
            } else if (c == 27) {
                result = 0; done = 1;
            } else if (c == '\b') {
                int l = fm_strlen(buf);
                if (l > 0) buf[l - 1] = '\0';
            } else if (c >= 32 && c < 127) {
                int l = fm_strlen(buf);
                if (l < out_max - 1) {
                    buf[l] = c; buf[l + 1] = '\0';
                }
            }
            // Mettre à jour le label de saisie
            char display[66];
            fm_strcpy(display, buf, sizeof(display));
            fm_strcat(display, "_", sizeof(display));
            app_set_label_text(lbl_input, display);
        }

        if (app_button_touched(btn_ok))  { result = 1; done = 1; }
        if (app_button_touched(btn_ann)) { result = 0; done = 1; }
    }

    app_close_window(dlg);

    if (result && fm_strlen(buf) > 0) {
        fm_strcpy(out, buf, out_max);
        return 1;
    }
    return 0;
}

// ============================================================
// ── DIALOGUE : CONFIRMATION ──────────────────────────────────
// ============================================================
static int fm_dialog_confirm(const char* message) {
    WinID dlg = app_new_window("Confirmation", 220, 220, 360, 110);
    app_new_label(dlg, 20, 20, message);
    BtnID btn_oui = app_new_button(dlg, 195, 62, 65, 26, "Oui");
    BtnID btn_non = app_new_button(dlg, 268, 62, 70, 26, "Non");

    int result = 0;
    int done   = 0;
    while (app_running() && !done) {
        app_tick();
        if (app_button_touched(btn_oui)) { result = 1; done = 1; }
        if (app_button_touched(btn_non)) { result = 0; done = 1; }
    }
    app_close_window(dlg);
    return result;
}

// ============================================================
// ── EXTRACTION DU NOM BRUT DEPUIS UNE LIGNE LISTBOX ──────────
// ============================================================
// La listbox stocke "[Dossier] nom" ou "nom      taille".
// Cette fonction extrait le nom réel du fichier/dossier.
static void fm_extract_name(const char* line, char* out, int max, int* out_is_dir) {
    *out_is_dir = 0;
    const char* prefix = "[Dossier] ";
    int plen = 10; // strlen("[Dossier] ")

    if (fm_strlen(line) >= plen) {
        // Vérifier si commence par "[Dossier] "
        int match = 1;
        for (int i = 0; i < plen; i++) {
            if (line[i] != prefix[i]) { match = 0; break; }
        }
        if (match) {
            *out_is_dir = 1;
            fm_strcpy(out, line + plen, max);
            return;
        }
    }

    // Fichier : copier jusqu'au premier espace de padding
    int i = 0;
    while (line[i] && line[i] != ' ' && i < max - 1) {
        out[i] = line[i]; i++;
    }
    out[i] = '\0';
}

// ============================================================
// ── POINT D'ENTRÉE PRINCIPAL ─────────────────────────────────
// ============================================================
void app_fileman_run(void) {
    app_init();

    // ── Fenêtre principale ───────────────────────────────────
    WinID win = app_new_window("Explorateur de fichiers",
                               FM_WIN_X, FM_WIN_Y, FM_WIN_W, FM_WIN_H);

    // Coordonnées relatives à la zone client (sans titlebar)
    // Appcore gère le titlebar (AC_WIN_TITLE_H = 26px) en interne,
    // les coordonnées passées à app_new_* sont relatives au coin
    // haut-gauche du client.

    int cx = FM_PAD;                        // x départ zone client
    int cy = FM_PAD;                        // y départ zone client
    int cw = FM_WIN_W - FM_PAD * 2;        // largeur disponible
    // int ch = FM_WIN_H - FM_PAD * 2;     // hauteur disponible (non utilisée directement)

    // ── Barre d'adresse ──────────────────────────────────────
    // Étiquette "Chemin :"
    app_new_label(win, cx, cy + 4, "Chemin :");
    app_set_label_color(app_new_label(win, cx, cy + 4, "Chemin :"), FM_TITLE_FG);

    // Label du chemin courant
    LblID lbl_path = app_new_label(win, cx + 70, cy + 4, "/");
    app_set_label_color(lbl_path, FM_BAR_FG);

    cy += FM_BAR_H + FM_PAD;

    // ── Boutons d'action ─────────────────────────────────────
    int btn_y = cy;
    BtnID btn_parent = app_new_button(win, cx,
                                      btn_y, FM_BTN_W_SM, FM_BTN_H,
                                      "^ Parent");
    BtnID btn_new    = app_new_button(win, cx + FM_BTN_W_SM + FM_PAD,
                                      btn_y, FM_BTN_W_NEW, FM_BTN_H,
                                      "+ Nouveau dossier");
    BtnID btn_newf   = app_new_button(win, cx + FM_BTN_W_SM + FM_PAD + FM_BTN_W_NEW + FM_PAD,
                                      btn_y, FM_BTN_W_NEWF, FM_BTN_H,
                                      "+ Nouveau fichier");
    BtnID btn_del    = app_new_button(win, cx + FM_BTN_W_SM + FM_PAD + FM_BTN_W_NEW + FM_PAD + FM_BTN_W_NEWF + FM_PAD,
                                      btn_y, FM_BTN_W_DEL, FM_BTN_H,
                                      "Supprimer");
    BtnID btn_open   = app_new_button(win, cx + FM_BTN_W_SM + FM_PAD + FM_BTN_W_NEW + FM_PAD + FM_BTN_W_NEWF + FM_PAD + FM_BTN_W_DEL + FM_PAD,
                                      btn_y, FM_BTN_W_SM, FM_BTN_H,
                                      "Ouvrir");

    cy += FM_BTN_H + FM_PAD;

    // ── ListBox de fichiers ───────────────────────────────────
    int list_h = FM_WIN_H
                 - 26                  // titlebar AppCore
                 - FM_PAD              // pad haut
                 - FM_BAR_H - FM_PAD  // barre adresse
                 - FM_BTN_H - FM_PAD  // boutons
                 - FM_STATUS_H        // status
                 - FM_PAD * 2;        // pad bas + bas liste

    LstID lst = app_new_listbox(win, cx, cy, cw, list_h, FM_ITEM_H);

    cy += list_h + FM_PAD;

    // ── Barre de statut ───────────────────────────────────────
    LblID lbl_status = app_new_label(win, cx, cy, "");
    app_set_label_color(lbl_status, FM_STATUS_FG);

    // ── Initialiser l'état ────────────────────────────────────
    g_fm.dir_idx = g_cwd;
    fm_build_path(g_cwd, g_fm.path, FM_PATH_MAX);
    app_set_label_text(lbl_path, g_fm.path);
    fm_populate_list(lst, g_fm.dir_idx);

    // ============================================================
    // ── BOUCLE PRINCIPALE ───────────────────────────────────────
    // ============================================================
    while (app_running()) {
        app_tick();

        // ── Bouton "^ Parent" ────────────────────────────────
        if (app_button_touched(btn_parent)) {
            uint32_t cur = g_fm.dir_idx;
            uint32_t parent = g_fs.nodes[cur].parent;

            if (parent == cur || cur == 0) {
                // Déjà à la racine
                app_set_label_text(lbl_status, "Deja a la racine.");
                app_set_label_color(lbl_status, FM_STATUS_FG);
            } else {
                fm_navigate_to(parent, lst, lbl_path, lbl_status);
                app_set_label_text(lbl_status, "");
            }
        }

        // ── Bouton "+ Nouveau fichier" ────────────────────────
        if (app_button_touched(btn_newf)) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                app_set_label_text(lbl_status, "Permission refusee.");
                app_set_label_color(lbl_status, FM_STATUS_ERR);
            } else {
                char filename[FM_NAME_MAX];
                filename[0] = '\0';
                if (fm_dialog_input("Nouveau fichier",
                                    "Nom du fichier :",
                                    filename, FM_NAME_MAX)) {
                    uint32_t saved_cwd = g_cwd;
                    g_cwd = g_fm.dir_idx;
                    // Créer le fichier vide s'il n'existe pas
                    if (fs_find(filename) < 0)
                        fs_write_file(filename, (const uint8_t*)"", 0);
                    // Ouvrir dans TextEdit
                    if (session_has_permission(PERM_FS_READ)) {
                        app_textedit_run(filename);
                    }
                    g_cwd = saved_cwd;
                    fm_populate_list(lst, g_fm.dir_idx);
                    app_set_label_text(lbl_status, "");
                }
            }
        }

        // ── Bouton "+ Nouveau dossier" ───────────────────────
        if (app_button_touched(btn_new)) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                app_set_label_text(lbl_status, "Permission refusee.");
                app_set_label_color(lbl_status, FM_STATUS_ERR);
            } else {
                char dirname[FM_NAME_MAX];
                dirname[0] = '\0';
                if (fm_dialog_input("Nouveau dossier",
                                    "Nom du dossier :",
                                    dirname, FM_NAME_MAX)) {
                    // Créer dans le répertoire courant de l'explorateur
                    uint32_t saved_cwd = g_cwd;
                    g_cwd = g_fm.dir_idx;
                    int r = fs_mkdir(dirname);
                    g_cwd = saved_cwd;

                    if (r >= 0) {
                        app_set_label_text(lbl_status, "Dossier cree.");
                        app_set_label_color(lbl_status, FM_STATUS_OK);
                        // Recharger la liste
                        g_cwd = g_fm.dir_idx;
                        fm_populate_list(lst, g_fm.dir_idx);
                    } else {
                        app_set_label_text(lbl_status, "Echec creation dossier.");
                        app_set_label_color(lbl_status, FM_STATUS_ERR);
                    }
                }
            }
        }

        // ── Bouton "Supprimer" ───────────────────────────────
        if (app_button_touched(btn_del)) {
            if (!session_has_permission(PERM_FS_DELETE)) {
                app_set_label_text(lbl_status, "Permission refusee.");
                app_set_label_color(lbl_status, FM_STATUS_ERR);
            } else {
                int sel = app_listbox_selected(lst);
                if (sel < 0) {
                    app_set_label_text(lbl_status, "Aucun element selectionne.");
                    app_set_label_color(lbl_status, FM_STATUS_FG);
                } else {
                    const char* line = app_listbox_selected_text(lst);
                    if (line) {
                        char name[FM_NAME_MAX]; int is_dir = 0;
                        fm_extract_name(line, name, FM_NAME_MAX, &is_dir);

                        // Vérifier que ce n'est pas le dossier "(dossier vide)"
                        if (fm_strcmp(name, "(dossier") == 0) {
                            app_set_label_text(lbl_status, "Rien a supprimer.");
                            app_set_label_color(lbl_status, FM_STATUS_FG);
                        } else {
                            // Message de confirmation
                            char confirm_msg[80];
                            confirm_msg[0] = '\0';
                            fm_strcat(confirm_msg, "Supprimer \"", sizeof(confirm_msg));
                            fm_strcat(confirm_msg, name, sizeof(confirm_msg));
                            fm_strcat(confirm_msg, "\" ?", sizeof(confirm_msg));

                            if (fm_dialog_confirm(confirm_msg)) {
                                uint32_t saved_cwd = g_cwd;
                                g_cwd = g_fm.dir_idx;
                                int r = fs_delete(name);
                                g_cwd = saved_cwd;

                                if (r == 0) {
                                    app_set_label_text(lbl_status, "Supprime.");
                                    app_set_label_color(lbl_status, FM_STATUS_OK);
                                    g_cwd = g_fm.dir_idx;
                                    fm_populate_list(lst, g_fm.dir_idx);
                                } else {
                                    app_set_label_text(lbl_status,
                                        "Echec : dossier non vide ou erreur.");
                                    app_set_label_color(lbl_status, FM_STATUS_ERR);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Bouton "Ouvrir" ──────────────────────────────────
        if (app_button_touched(btn_open)) {
            int sel = app_listbox_selected(lst);
            if (sel >= 0) {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int is_dir = 0;
                    fm_extract_name(line, name, FM_NAME_MAX, &is_dir);

                    if (fm_strcmp(name, "(dossier") != 0) {
                        if (is_dir) {
                            // Trouver l'index du nœud enfant
                            int child_idx = fs_find_in_dir(g_fm.dir_idx, name);
                            if (child_idx >= 0) {
                                fm_navigate_to((uint32_t)child_idx, lst, lbl_path, lbl_status);
                            }
                        } else {
                            // Ouvrir dans TextEdit
                            if (!session_has_permission(PERM_FS_READ)) {
                                app_set_label_text(lbl_status, "Permission refusee.");
                                app_set_label_color(lbl_status, FM_STATUS_ERR);
                            } else {
                                // TextEdit gère g_cwd lui-même pour find/read
                                uint32_t saved_cwd = g_cwd;
                                g_cwd = g_fm.dir_idx;
                                app_textedit_run(name);
                                g_cwd = saved_cwd;
                                // Recharger au retour (le fichier a pu être modifié)
                                fm_populate_list(lst, g_fm.dir_idx);
                                app_set_label_text(lbl_status, "");
                            }
                        }
                    }
                }
            } else {
                app_set_label_text(lbl_status, "Aucun element selectionne.");
                app_set_label_color(lbl_status, FM_STATUS_FG);
            }
        }

        // ── Double-clic dans la listbox ──────────────────────
        if (app_listbox_activated(lst)) {
            int sel = app_listbox_selected(lst);
            if (sel >= 0) {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int is_dir = 0;
                    fm_extract_name(line, name, FM_NAME_MAX, &is_dir);

                    if (fm_strcmp(name, "(dossier") != 0) {
                        if (is_dir) {
                            // Naviguer dans le sous-dossier
                            int child_idx = fs_find_in_dir(g_fm.dir_idx, name);
                            if (child_idx >= 0) {
                                fm_navigate_to((uint32_t)child_idx, lst, lbl_path, lbl_status);
                            } else {
                                app_set_label_text(lbl_status, "Dossier introuvable.");
                                app_set_label_color(lbl_status, FM_STATUS_ERR);
                            }
                        } else {
                            // Ouvrir fichier dans TextEdit
                            if (!session_has_permission(PERM_FS_READ)) {
                                app_set_label_text(lbl_status, "Permission refusee.");
                                app_set_label_color(lbl_status, FM_STATUS_ERR);
                            } else {
                                uint32_t saved_cwd = g_cwd;
                                g_cwd = g_fm.dir_idx;
                                app_textedit_run(name);
                                g_cwd = saved_cwd;
                                fm_populate_list(lst, g_fm.dir_idx);
                                app_set_label_text(lbl_status, "");
                            }
                        }
                    }
                }
            }
        }
    }
    // app_running() retourne 0 → l'utilisateur a fermé la fenêtre.
    // AppCore a déjà libéré les ressources via app_tick().
}

// ============================================================
// ── POINT D'ENTRÉE TEX (depuis desktop.c) ───────────────────
// ============================================================
// desktop.c appelle entry() = app_fileman().
// On délègue simplement à app_fileman_run().
void app_fileman(void) {
    app_fileman_run();
}