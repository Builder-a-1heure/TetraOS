// apps/fileman.c — Explorateur de fichiers TetraOS v2
//
// Nouvelles fonctionnalités v2 :
//   - Opérations RAY64 complètes : renommer, copier/coller, chmod, chown
//   - Panneau d'informations latéral : permissions, propriétaire, taille, extension
//   - Dialog chmod : interface graphique pour modifier les ACL (owner/admin/other rwx)
//   - Association d'app par extension : .tex → exécuter, .txt/.* → TextEdit, .bin → hex (futur)
//   - Tri : dossiers d'abord, fichiers ensuite
//
// Architecture AppCore v2 :
//   - ListBox à gauche + DrawArea panneau infos à droite
//   - Deux barres de boutons : actions FS + actions fichier
//   - Double-clic : naviguer (dossier) ou ouvrir l'app associée (fichier)

#include "app.h"
#include "fileman.h"
#include "textedit.h"
#include "fileeditor.h"
#include "../lib/appcore.h"
#include "../lib/utils.h"
#include "../lib/errwin.h"
#include "../fs/fs.h"
#include "../ui/session.h"
#include "../drivers/vesa.h"
#include <stdint.h>

// ── Signature TEX ────────────────────────────────────────────
TEX_APP("FileMan", APPICON_FILEMAN, 2, 0,
        APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, app_fileman);


// ============================================================
// ── CONSTANTES DE LAYOUT ─────────────────────────────────────
// ============================================================
#define FM_WIN_X        40
#define FM_WIN_Y        30
#define FM_WIN_W        780
#define FM_WIN_H        520

#define FM_PAD          8
#define FM_BAR_H        22
#define FM_BTN_H        26
#define FM_BTN_W_SM     86
#define FM_BTN_W_MD    100
#define FM_BTN_W_LG    112
#define FM_STATUS_H     20
#define FM_ITEM_H       20

// Panneau latéral droit
#define FM_PANEL_W      196
#define FM_PANEL_X      (FM_WIN_W - FM_PAD - FM_PANEL_W)
#define FM_LIST_W       (FM_WIN_W - FM_PAD * 3 - FM_PANEL_W)

// ── Couleurs ─────────────────────────────────────────────────
#define FM_BAR_FG       0x0066AADD
#define FM_STATUS_FG    0x00445566
#define FM_STATUS_OK    0x0033AA55
#define FM_STATUS_ERR   0x00CC3333
#define FM_STATUS_WARN  0x00DDAA22
#define FM_TITLE_FG     0x0088CCFF
#define FM_PANEL_HDR    0x0044AAFF
#define FM_PANEL_TXT    0x0088AABB
#define FM_PANEL_VAL    0x00DDEEFF

// ── Limites ──────────────────────────────────────────────────
#define FM_PATH_MAX     256
#define FM_NAME_MAX      48
#define FM_COPY_BUF    8192

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
    int d = fm_strlen(dst), i = 0;
    while (d + i < max - 1 && src[i]) { dst[d+i] = src[i]; i++; }
    dst[d+i] = '\0';
}
static int fm_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int fm_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void fm_itoa(uint32_t val, char* buf, int bufsz) {
    if (bufsz <= 0) return;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int i = 0;
    while (val && i < 15) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0 && j < bufsz - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}
static void fm_fmt_size(uint32_t bytes, char* buf, int bufsz) {
    if (bytes < 1024)             { fm_itoa(bytes,               buf, bufsz-2); fm_strcat(buf, " B",  bufsz); }
    else if (bytes < 1024*1024)   { fm_itoa(bytes/1024,          buf, bufsz-3); fm_strcat(buf, " Ko", bufsz); }
    else                          { fm_itoa(bytes/(1024*1024),    buf, bufsz-3); fm_strcat(buf, " Mo", bufsz); }
}
// Helper pour accéder aux métadonnées RAY64 d'un nœud
static void fm_get_meta(FSNode* node, RAY64NodeMeta* meta) {
    uint8_t* src = node->reserved;
    uint8_t* dst = (uint8_t*)meta;
    for (int i = 0; i < (int)sizeof(RAY64NodeMeta); i++) dst[i] = src[i];
}

static const char* fm_get_ext(const char* name) {
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    return dot ? dot + 1 : "";
}

// ============================================================
// ── ÉTAT GLOBAL ──────────────────────────────────────────────
// ============================================================
static struct {
    uint32_t dir_idx;
    char     path[FM_PATH_MAX];
    int      sel_node_idx;          // -1 si aucun sélectionné
    char     clipboard_name[FM_NAME_MAX];
    uint32_t clipboard_dir;
} g_fm;

// Buffer statique pour copier/coller
static uint8_t g_fm_copy_buf[FM_COPY_BUF];

// ============================================================
// ── CONSTRUCTION DU CHEMIN ───────────────────────────────────
// ============================================================
static void fm_build_path(uint32_t dir_idx, char* out, int max) {
    const char* parts[32]; int depth = 0;
    uint32_t cur = dir_idx;
    while (cur != 0 && depth < 31) {
        parts[depth++] = g_fs.nodes[cur].name;
        uint32_t parent = g_fs.nodes[cur].parent;
        if (parent == cur) break;
        cur = parent;
    }
    out[0] = '/'; out[1] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        fm_strcat(out, parts[i], max);
        if (i > 0) fm_strcat(out, "/", max);
    }
}

// ============================================================
// ── ASSOCIATION APP PAR EXTENSION ───────────────────────────
// ============================================================
typedef enum { FMAPP_TEXTEDIT=0, FMAPP_TEX_RUN, FMAPP_HEX } FmAppType;

static FmAppType fm_app_for(const char* name) {
    const char* ext = fm_get_ext(name);
    if (fm_strcmp(ext, "tex") == 0) return FMAPP_TEX_RUN;
    if (fm_strcmp(ext, "bin") == 0 || fm_strcmp(ext, "img") == 0) return FMAPP_HEX;
    return FMAPP_TEXTEDIT;
}

// Ouvre le fichier avec l'app appropriée (g_cwd doit être positionné)
static void fm_open_file(const char* name, LblID lbl_status) {
    FmAppType t = fm_app_for(name);
    if (t == FMAPP_TEX_RUN) {
        if (!session_has_permission(PERM_TEX_EXECUTE)) {
            errwin_error("Permission refusee", "Execution de scripts .tex\nnon autorisee pour cette session.");
        } else {
            extern int tex_execute(const char*);
            tex_execute(name);
            app_set_label_text(lbl_status, "Script .tex execute.");
            app_set_label_color(lbl_status, FM_STATUS_OK);
        }
    } else if (t == FMAPP_HEX) {
        app_set_label_text(lbl_status, "Editeur hexadecimal : bientot disponible.");
        app_set_label_color(lbl_status, FM_STATUS_WARN);
    } else {
        if (!session_has_permission(PERM_FS_READ)) {
            errwin_error("Permission refusee", "Lecture de fichiers\nnon autorisee pour cette session.");
        } else {
            app_fileeditor_run(name);
        }
    }
}

// ============================================================
// ── REMPLISSAGE DE LA LISTBOX ────────────────────────────────
// ============================================================
static void fm_populate_list(LstID lst, uint32_t dir_idx) {
    app_listbox_clear(lst);
    g_fm.sel_node_idx = -1;

    FSNode* dir = &g_fs.nodes[dir_idx];
    int has_item = 0;

    // Passe 1 : dossiers
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t ci = dir->children[i];
        if (ci >= g_fs.node_count) continue;
        FSNode* child = &g_fs.nodes[ci];
        if (!child->is_dir || child->magic != FS_MAGIC) continue;
        char line[FM_NAME_MAX + 16]; line[0] = '\0';
        fm_strcat(line, "[Dir]  ", sizeof(line));
        fm_strcat(line, child->name, sizeof(line));
        app_listbox_add(lst, line, 1);
        has_item = 1;
    }

    // Passe 2 : fichiers (avec taille alignée)
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t ci = dir->children[i];
        if (ci >= g_fs.node_count) continue;
        FSNode* child = &g_fs.nodes[ci];
        if (child->is_dir || child->magic != FS_MAGIC) continue;
        char line[FM_NAME_MAX + 24]; line[0] = '\0';
        fm_strcat(line, "       ", sizeof(line));   // indentation
        fm_strcat(line, child->name, sizeof(line));
        int nlen = fm_strlen(line);
        while (nlen < 36 && nlen < (int)sizeof(line)-1) line[nlen++] = ' ';
        line[nlen] = '\0';
        char sz[12]; fm_fmt_size(child->size_bytes, sz, sizeof(sz));
        fm_strcat(line, sz, sizeof(line));
        app_listbox_add(lst, line, 0);
        has_item = 1;
    }

    if (!has_item) app_listbox_add(lst, "(dossier vide)", 0);
}

// ============================================================
// ── EXTRACTION NOM DEPUIS LIGNE LISTBOX ─────────────────────
// ============================================================
static void fm_extract_name(const char* line, char* out, int max, int* out_is_dir) {
    *out_is_dir = 0;
    if (fm_strncmp(line, "[Dir]  ", 7) == 0) {
        *out_is_dir = 1;
        fm_strcpy(out, line + 7, max);
        return;
    }
    const char* p = line;
    while (*p == ' ') p++;
    int i = 0;
    while (*p && *p != ' ' && i < max-1) { out[i++] = *p++; }
    out[i] = '\0';
}

// Cherche l'index FS d'un enfant par nom dans g_fm.dir_idx
static int fm_find_child(const char* name) {
    FSNode* dir = &g_fs.nodes[g_fm.dir_idx];
    for (uint32_t i = 0; i < dir->child_count; i++) {
        uint32_t ci = dir->children[i];
        if (ci < g_fs.node_count && fm_strcmp(g_fs.nodes[ci].name, name) == 0)
            return (int)ci;
    }
    return -1;
}

// ============================================================
// ── NAVIGATION ───────────────────────────────────────────────
// ============================================================
static void fm_navigate_to(uint32_t new_dir, LstID lst,
                            LblID lbl_path, LblID lbl_status) {
    // Positionner g_cwd sur la cible AVANT la vérification ACL.
    uint32_t prev_dir = g_fm.dir_idx;
    g_fm.dir_idx = new_dir;
    g_cwd = new_dir;

    // Bypass ACL pour :
    //  1. Le dossier home de l'utilisateur courant : il en est le propriétaire
    //     par définition même si l'uid stocké sur disque ne correspond plus
    //     exactement à session_get_uid() après rechargement de session.
    //  2. Les nœuds UID_SYSTEM (racine, /home, wallpaper…) : fs_acl_check
    //     autorise déjà la lecture, mais on court-circuite pour éviter les
    //     faux-refus liés à un g_session_manager dans un état intermédiaire.
    int bypass = 0;
    if (g_session_manager.logged_in && g_session_manager.current_session) {
        if (new_dir == g_session_manager.current_session->home_dir_node)
            bypass = 1;
    }
    if (!bypass && new_dir < g_fs.node_count) {
        uint16_t node_uid = fs_get_node_uid(new_dir);
        if (node_uid == UID_SYSTEM || node_uid == UID_NOOWNER)
            bypass = 1;  // nœud système : lecture toujours ok
    }

    if (!bypass && !fs_acl_check(new_dir, ACL_READ)) {
        // Refus réel : restaurer l'état précédent
        g_fm.dir_idx = prev_dir;
        g_cwd = prev_dir;
        errwin_error("Acces refuse", "Permissions insuffisantes\npour naviguer dans ce dossier.");
        return;
    }
    fm_build_path(new_dir, g_fm.path, FM_PATH_MAX);
    app_set_label_text(lbl_path, g_fm.path);
    app_set_label_color(lbl_path, FM_BAR_FG);
    fm_populate_list(lst, new_dir);
    app_set_label_text(lbl_status, "");
}

// ============================================================
// ── DIALOGS ──────────────────────────────────────────────────
// ============================================================
static int fm_dialog_input(const char* title, const char* prompt,
                            char* out, int out_max) {
    WinID dlg = app_new_window(title, 210, 205, 420, 140);
    app_new_label(dlg, 20, 18, prompt);
    char buf[64]; buf[0] = '\0';
    LblID lbl_inp = app_new_label(dlg, 20, 46, "_");
    app_set_label_color(lbl_inp, 0x00FFFFFF);
    BtnID btn_ok  = app_new_button(dlg, 260, 98, 66, 26, "OK");
    BtnID btn_ann = app_new_button(dlg, 334, 98, 74, 26, "Annuler");
    int result = 0, done = 0;
    while (app_running() && !done) {
        app_tick();
        char c = app_tick_get_key();
        if (c) {
            if      (c == '\n' || c == '\r') { result = 1; done = 1; }
            else if (c == 27)                { result = 0; done = 1; }
            else if (c == '\b') { int l = fm_strlen(buf); if (l>0) buf[l-1]='\0'; }
            else if (c >= 32 && c < 127) {
                int l = fm_strlen(buf);
                if (l < out_max-1) { buf[l]=c; buf[l+1]='\0'; }
            }
            char disp[66]; fm_strcpy(disp, buf, sizeof(disp));
            fm_strcat(disp, "_", sizeof(disp));
            app_set_label_text(lbl_inp, disp);
        }
        if (app_button_touched(btn_ok))  { result = 1; done = 1; }
        if (app_button_touched(btn_ann)) { result = 0; done = 1; }
    }
    app_close_window(dlg);
    if (result && fm_strlen(buf) > 0) { fm_strcpy(out, buf, out_max); return 1; }
    return 0;
}

static int fm_dialog_confirm(const char* msg) {
    WinID dlg = app_new_window("Confirmation", 225, 218, 380, 112);
    app_new_label(dlg, 20, 22, msg);
    BtnID btn_oui = app_new_button(dlg, 200, 66, 66, 26, "Oui");
    BtnID btn_non = app_new_button(dlg, 274, 66, 66, 26, "Non");
    int result = 0, done = 0;
    while (app_running() && !done) {
        app_tick();
        if (app_button_touched(btn_oui)) { result = 1; done = 1; }
        if (app_button_touched(btn_non)) { result = 0; done = 1; }
    }
    app_close_window(dlg);
    return result;
}

// ============================================================
// ── DIALOG CHMOD ─────────────────────────────────────────────
// Affiche 9 boutons toggle (rwx × owner/admin/other).
// Seul l'admin peut modifier si acl_lock=1.
// ============================================================
static void fm_dialog_chmod(const char* name, int node_idx) {
    FSNode* node = &g_fs.nodes[node_idx];
    RAY64NodeMeta meta_chk; fm_get_meta(node, &meta_chk);
    int is_admin = session_is_admin();
    int is_owner = (meta_chk.uid == session_get_uid());

    // Infos seulement si pas les droits
    if (!is_admin && (!is_owner || meta_chk.acl_lock)) {
        WinID dlg = app_new_window("Permissions (lecture seule)", 185, 185, 410, 120);
        char pstr[10];
        uint16_t p = meta_chk.permissions;
        pstr[0]=(p&ACL_OWNER_R)?'r':'-'; pstr[1]=(p&ACL_OWNER_W)?'w':'-'; pstr[2]=(p&ACL_OWNER_X)?'x':'-';
        pstr[3]=(p&ACL_ADMIN_R)?'r':'-'; pstr[4]=(p&ACL_ADMIN_W)?'w':'-'; pstr[5]=(p&ACL_ADMIN_X)?'x':'-';
        pstr[6]=(p&ACL_OTHER_R)?'r':'-'; pstr[7]=(p&ACL_OTHER_W)?'w':'-'; pstr[8]=(p&ACL_OTHER_X)?'x':'-';
        pstr[9]='\0';
        char info[64]; info[0]='\0';
        fm_strcat(info, "Permissions actuelles : ", sizeof(info));
        fm_strcat(info, pstr, sizeof(info));
        app_new_label(dlg, 20, 20, info);
        app_new_label(dlg, 20, 44, "(Admin requis pour modifier)");
        BtnID bf = app_new_button(dlg, 316, 78, 74, 26, "Fermer");
        while (app_running()) { app_tick(); if (app_button_touched(bf)) break; }
        app_close_window(dlg);
        return;
    }

    WinID dlg = app_new_window("Modifier les permissions", 165, 155, 490, 250);

    // En-têtes colonnes
    app_new_label(dlg, 162, 14, "Lecture (r)");
    app_new_label(dlg, 252, 14, "Ecriture (w)");
    app_new_label(dlg, 342, 14, "Execute (x)");

    // En-têtes lignes
    app_new_label(dlg, 16, 48, "Proprietaire");
    app_new_label(dlg, 16, 94, "Admin");
    app_new_label(dlg, 16, 140, "Autres");

    // Séparateur : affichage "verrou ACL"
    if (meta_chk.acl_lock && is_admin) { app_new_label(dlg, 16, 186, "[ACL verrouille - admin seulement]");
    }

    uint16_t perms = meta_chk.permissions;

    // 9 boutons toggle (labels mis à jour à chaque clic via rétro-affichage)
    // On génère les labels initiaux selon l'état courant
    uint16_t bits[9] = {
        ACL_OWNER_R, ACL_OWNER_W, ACL_OWNER_X,
        ACL_ADMIN_R, ACL_ADMIN_W, ACL_ADMIN_X,
        ACL_OTHER_R, ACL_OTHER_W, ACL_OTHER_X
    };
    int btn_x[9] = {160,250,340, 160,250,340, 160,250,340};
    int btn_y[9] = { 38, 38, 38,  84, 84, 84, 130,130,130};
    BtnID btn[9];
    for (int i = 0; i < 9; i++) {
        btn[i] = app_new_button(dlg, btn_x[i], btn_y[i], 66, 28,
                                 (perms & bits[i]) ? " ON " : "OFF");
    }

    BtnID btn_ok  = app_new_button(dlg, 298, 200, 88, 28, "Appliquer");
    BtnID btn_ann = app_new_button(dlg, 392, 200, 82, 28, "Annuler");

    // Label récapitulatif rwxrwxrwx
    char pdisp[10];
    pdisp[0]=(perms&ACL_OWNER_R)?'r':'-'; pdisp[1]=(perms&ACL_OWNER_W)?'w':'-'; pdisp[2]=(perms&ACL_OWNER_X)?'x':'-';
    pdisp[3]=(perms&ACL_ADMIN_R)?'r':'-'; pdisp[4]=(perms&ACL_ADMIN_W)?'w':'-'; pdisp[5]=(perms&ACL_ADMIN_X)?'x':'-';
    pdisp[6]=(perms&ACL_OTHER_R)?'r':'-'; pdisp[7]=(perms&ACL_OTHER_W)?'w':'-'; pdisp[8]=(perms&ACL_OTHER_X)?'x':'-';
    pdisp[9]='\0';
    char recap[32]; recap[0]='\0';
    fm_strcat(recap, "Actuellement : ", sizeof(recap));
    fm_strcat(recap, pdisp, sizeof(recap));
    LblID lbl_recap = app_new_label(dlg, 16, 200, recap);
    app_set_label_color(lbl_recap, FM_PANEL_HDR);

    int done = 0;
    while (app_running() && !done) {
        app_tick();
        for (int i = 0; i < 9; i++) {
            if (app_button_touched(btn[i])) {
                perms ^= bits[i];
                // Mettre à jour le récapitulatif
                pdisp[0]=(perms&ACL_OWNER_R)?'r':'-'; pdisp[1]=(perms&ACL_OWNER_W)?'w':'-'; pdisp[2]=(perms&ACL_OWNER_X)?'x':'-';
                pdisp[3]=(perms&ACL_ADMIN_R)?'r':'-'; pdisp[4]=(perms&ACL_ADMIN_W)?'w':'-'; pdisp[5]=(perms&ACL_ADMIN_X)?'x':'-';
                pdisp[6]=(perms&ACL_OTHER_R)?'r':'-'; pdisp[7]=(perms&ACL_OTHER_W)?'w':'-'; pdisp[8]=(perms&ACL_OTHER_X)?'x':'-';
                recap[0]='\0';
                fm_strcat(recap, "Actuellement : ", sizeof(recap));
                fm_strcat(recap, pdisp, sizeof(recap));
                app_set_label_text(lbl_recap, recap);
            }
        }
        if (app_button_touched(btn_ok)) {
            uint32_t saved_cwd = g_cwd;
            g_cwd = g_fm.dir_idx;
            fs_chmod(name, perms);
            g_cwd = saved_cwd;
            done = 1;
        }
        if (app_button_touched(btn_ann)) done = 1;
    }
    app_close_window(dlg);
}

// ============================================================
// ── DIALOG CHOWN ─────────────────────────────────────────────
// ============================================================
static void fm_dialog_chown(const char* name, LblID lbl_status) {
    if (!session_is_admin()) {
        app_set_label_text(lbl_status, "Chown : admin requis.");
        app_set_label_color(lbl_status, FM_STATUS_ERR);
        return;
    }
    char uid_str[16]; uid_str[0] = '\0';
    if (!fm_dialog_input("Changer proprietaire",
                          "Nouveau UID (entier, 65535=SYSTEM) :",
                          uid_str, 8)) return;
    uint16_t new_uid = 0;
    for (int i = 0; uid_str[i] >= '0' && uid_str[i] <= '9'; i++)
        new_uid = (uint16_t)(new_uid * 10 + (uid_str[i] - '0'));
    uint32_t saved_cwd = g_cwd;
    g_cwd = g_fm.dir_idx;
    int r = fs_chown(name, new_uid);
    g_cwd = saved_cwd;
    if (r == 0) {
        app_set_label_text(lbl_status, "Proprietaire modifie.");
        app_set_label_color(lbl_status, FM_STATUS_OK);
    } else {
        app_set_label_text(lbl_status, "Echec chown (admin requis ?).");
        app_set_label_color(lbl_status, FM_STATUS_ERR);
    }
}

// ============================================================
// ── OPÉRATION RENOMMER ───────────────────────────────────────
// ============================================================
static void fm_do_rename(const char* old_name, LstID lst, LblID lbl_status) {
    if (!session_has_permission(PERM_FS_WRITE)) {
        errwin_error("Permission refusee", "Renommage non autorise\npour cette session.");
        return;
    }
    char new_name[FM_NAME_MAX]; new_name[0] = '\0';
    char prompt[80]; prompt[0] = '\0';
    fm_strcat(prompt, "Nouveau nom pour \"", sizeof(prompt));
    fm_strcat(prompt, old_name, sizeof(prompt));
    fm_strcat(prompt, "\" :", sizeof(prompt));
    if (!fm_dialog_input("Renommer", prompt, new_name, FM_NAME_MAX)) return;
    if (fm_strlen(new_name) == 0) return;

    uint32_t saved_cwd = g_cwd;
    g_cwd = g_fm.dir_idx;

    if (fs_find(new_name) >= 0) {
        app_set_label_text(lbl_status, "Ce nom existe deja."); app_set_label_color(lbl_status, FM_STATUS_ERR);
        g_cwd = saved_cwd; return;
    }
    int ni = fs_find(old_name);
    if (ni >= 0 && (uint32_t)ni < g_fs.node_count) {
        if (!fs_acl_check((uint32_t)ni, ACL_WRITE)) {
            errwin_error2("Acces refuse", "Ecriture interdite sur : ", old_name);
            g_cwd = saved_cwd; return;
        }
        // Modifier le nom directement dans la structure FS
        int j = 0;
        while (j < FS_NAME_LEN-1 && new_name[j]) { g_fs.nodes[ni].name[j] = new_name[j]; j++; }
        g_fs.nodes[ni].name[j] = '\0';
        fs_flush();
        app_set_label_text(lbl_status, "Renomme.");
        app_set_label_color(lbl_status, FM_STATUS_OK);
        fm_populate_list(lst, g_fm.dir_idx);
    } else {
        app_set_label_text(lbl_status, "Element introuvable."); app_set_label_color(lbl_status, FM_STATUS_ERR);
    }
    g_cwd = saved_cwd;
}

// ============================================================
// ── COPIER / COLLER ──────────────────────────────────────────
// ============================================================
static void fm_do_copy(const char* name, LblID lbl_status) {
    if (!session_has_permission(PERM_FS_READ)) {
        errwin_error("Permission refusee", "Lecture non autorisee\npour cette session.");
        return;
    }
    int ni = fm_find_child(name);
    if (ni < 0 || g_fs.nodes[ni].is_dir) {
        app_set_label_text(lbl_status, "Copie de dossiers non supportee."); app_set_label_color(lbl_status, FM_STATUS_WARN); return;
    }
    fm_strcpy(g_fm.clipboard_name, name, FM_NAME_MAX);
    g_fm.clipboard_dir = g_fm.dir_idx;
    app_set_label_text(lbl_status, "Copie dans le presse-papier.");
    app_set_label_color(lbl_status, FM_STATUS_OK);
}

static void fm_do_paste(LstID lst, LblID lbl_status) {
    if (fm_strlen(g_fm.clipboard_name) == 0) {
        app_set_label_text(lbl_status, "Presse-papier vide."); app_set_label_color(lbl_status, FM_STATUS_FG); return;
    }
    if (!session_has_permission(PERM_FS_WRITE)) {
        errwin_error("Permission refusee", "Ecriture non autorisee\npour cette session.");
        return;
    }
    uint32_t saved_cwd = g_cwd;
    g_cwd = g_fm.clipboard_dir;
    int bytes = fs_read_file(g_fm.clipboard_name, g_fm_copy_buf, FM_COPY_BUF-1);
    if (bytes < 0) bytes = 0;

    g_cwd = g_fm.dir_idx;
    char dest[FM_NAME_MAX];
    fm_strcpy(dest, g_fm.clipboard_name, FM_NAME_MAX);
    if (fs_find(dest) >= 0) {
        char tmp[FM_NAME_MAX]; tmp[0] = '\0';
        fm_strcat(tmp, "copie_", sizeof(tmp));
        fm_strcat(tmp, dest, sizeof(tmp));
        fm_strcpy(dest, tmp, FM_NAME_MAX);
    }
    fs_write_file(dest, g_fm_copy_buf, (uint32_t)bytes);
    g_cwd = saved_cwd;
    fm_populate_list(lst, g_fm.dir_idx);
    app_set_label_text(lbl_status, "Colle avec succes.");
    app_set_label_color(lbl_status, FM_STATUS_OK);
}

// ============================================================
// ── ANIMATION LIQUID GLASS ───────────────────────────────────
// Bulle circulaire avec "respiration" et satellites orbitaux.
// Utilise Bresenham pour tracer les arcs sans multiplication.
// ============================================================




// ============================================================
// ── CALLBACK DRAWAREA — PANNEAU LATÉRAL ─────────────────────
// ============================================================


// ============================================================
// ── POINT D'ENTRÉE PRINCIPAL ─────────────────────────────────

// Met à jour les labels du panneau d'informations selon la sélection courante.
static void fm_update_info_panel(LblID lbl_name,  LblID lbl_type,
                                  LblID lbl_size,  LblID lbl_perms,
                                  LblID lbl_uid,   LblID lbl_app) {
    if (g_fm.sel_node_idx < 0 ||
        (uint32_t)g_fm.sel_node_idx >= g_fs.node_count) {
        app_set_label_text(lbl_name,  "");
        app_set_label_text(lbl_type,  "(rien de selectionne)");
        app_set_label_text(lbl_size,  "");
        app_set_label_text(lbl_perms, "");
        app_set_label_text(lbl_uid,   "");
        app_set_label_text(lbl_app,   "");
        return;
    }

    FSNode* node = &g_fs.nodes[g_fm.sel_node_idx];
    RAY64NodeMeta meta; fm_get_meta(node, &meta);

    // Nom
    app_set_label_text(lbl_name, node->name);

    // Type
    app_set_label_text(lbl_type, node->is_dir ? "Type : Dossier" : "Type : Fichier");

    // Taille (fichiers seulement)
    if (!node->is_dir) {
        char szl[24]; szl[0] = '\0';
        fm_strcat(szl, "Taille: ", sizeof(szl));
        fm_fmt_size(node->size_bytes, szl + fm_strlen(szl),
                    (int)(sizeof(szl) - fm_strlen(szl)));
        app_set_label_text(lbl_size, szl);
        // App associée
        FmAppType at = fm_app_for(node->name);
        app_set_label_text(lbl_app,
            at == FMAPP_TEX_RUN ? "App : TEX Run"
          : at == FMAPP_HEX    ? "App : Hex Edit"
                                : "App : Editeur");
    } else {
        app_set_label_text(lbl_size, "");
        app_set_label_text(lbl_app,  "");
    }

    // Permissions rwxrwxrwx
    uint16_t p = meta.permissions;
    char pstr[10];
    pstr[0]=(p&ACL_OWNER_R)?'r':'-'; pstr[1]=(p&ACL_OWNER_W)?'w':'-'; pstr[2]=(p&ACL_OWNER_X)?'x':'-';
    pstr[3]=(p&ACL_ADMIN_R)?'r':'-'; pstr[4]=(p&ACL_ADMIN_W)?'w':'-'; pstr[5]=(p&ACL_ADMIN_X)?'x':'-';
    pstr[6]=(p&ACL_OTHER_R)?'r':'-'; pstr[7]=(p&ACL_OTHER_W)?'w':'-'; pstr[8]=(p&ACL_OTHER_X)?'x':'-';
    pstr[9] = '\0';
    char perml[20]; perml[0] = '\0';
    fm_strcat(perml, "Perms: ", sizeof(perml));
    fm_strcat(perml, pstr, sizeof(perml));
    app_set_label_text(lbl_perms, perml);

    // UID
    char uidl[20]; uidl[0] = '\0';
    fm_strcat(uidl, "UID: ", sizeof(uidl));
    if (meta.uid == UID_SYSTEM) {
        fm_strcat(uidl, "SYSTEM", sizeof(uidl));
    } else {
        char ub[8]; fm_itoa(meta.uid, ub, sizeof(ub));
        fm_strcat(uidl, ub, sizeof(uidl));
    }
    if (meta.acl_lock) fm_strcat(uidl, " [LOCK]", sizeof(uidl));
    app_set_label_text(lbl_uid, uidl);
}
// ============================================================
void app_fileman_run(void) {
  int need_restart = 0;
  do {
    need_restart = 0;
    app_init();

    WinID win = app_new_window("Explorateur — TetraOS",
                               FM_WIN_X, FM_WIN_Y, FM_WIN_W, FM_WIN_H);
    int cx = FM_PAD, cy = FM_PAD;

    // ── Barre d'adresse ──────────────────────────────────────
    LblID lbl_path_t = app_new_label(win, cx, cy+4, "Chemin :");
    app_set_label_color(lbl_path_t, FM_TITLE_FG);
    LblID lbl_path = app_new_label(win, cx+74, cy+4, "/");
    app_set_label_color(lbl_path, FM_BAR_FG);
    cy += FM_BAR_H + FM_PAD;

    // ── Barre d'action principale (opérations FS) ─────────────
    int bx = cx;
    BtnID btn_parent  = app_new_button(win, bx, cy, FM_BTN_W_SM, FM_BTN_H, "^ Parent");  bx += FM_BTN_W_SM + 4;
    BtnID btn_new_dir = app_new_button(win, bx, cy, FM_BTN_W_LG, FM_BTN_H, "+ Dossier"); bx += FM_BTN_W_LG + 4;
    BtnID btn_new_fil = app_new_button(win, bx, cy, FM_BTN_W_LG, FM_BTN_H, "+ Fichier"); bx += FM_BTN_W_LG + 4;
    BtnID btn_rename  = app_new_button(win, bx, cy, FM_BTN_W_MD, FM_BTN_H, "Renommer");  bx += FM_BTN_W_MD + 4;
    BtnID btn_copy    = app_new_button(win, bx, cy, FM_BTN_W_SM, FM_BTN_H, "Copier");    bx += FM_BTN_W_SM + 4;
    BtnID btn_paste   = app_new_button(win, bx, cy, FM_BTN_W_SM, FM_BTN_H, "Coller");    bx += FM_BTN_W_SM + 4;
    BtnID btn_del     = app_new_button(win, bx, cy, FM_BTN_W_SM, FM_BTN_H, "Supprimer");
    cy += FM_BTN_H + 4;

    // ── Barre d'action secondaire (fichier + permissions) ─────
    bx = cx;
    BtnID btn_open    = app_new_button(win, bx, cy, FM_BTN_W_MD, FM_BTN_H, "Ouvrir/Editer");  bx += FM_BTN_W_MD + 4;
    BtnID btn_exec    = app_new_button(win, bx, cy, FM_BTN_W_LG, FM_BTN_H, "Executer (.tex)"); bx += FM_BTN_W_LG + 4;
    BtnID btn_chmod   = app_new_button(win, bx, cy, FM_BTN_W_MD, FM_BTN_H, "Permissions");    bx += FM_BTN_W_MD + 4;
    BtnID btn_chown   = app_new_button(win, bx, cy, FM_BTN_W_MD, FM_BTN_H, "Proprietaire");
    cy += FM_BTN_H + FM_PAD;

    // ── ListBox principale ────────────────────────────────────
    int list_h = FM_WIN_H
                 - 26                    // titlebar AppCore
                 - FM_PAD
                 - FM_BAR_H - FM_PAD
                 - (FM_BTN_H + 4) * 2
                 - FM_STATUS_H
                 - FM_PAD * 2;

    LstID lst = app_new_listbox(win, cx, cy, FM_LIST_W, list_h, FM_ITEM_H);

    // ── Panneau latéral droit — labels d'information (statiques) ──
    // Plus de DrawArea ni d'animation : juste des labels mis à jour
    // à chaque sélection. Zéro bug, zéro overhead.
    LblID lbl_panel_title = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 4, "[ Informations ]");
    app_set_label_color(lbl_panel_title, FM_PANEL_HDR);

    LblID lbl_info_name  = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 24, "");
    LblID lbl_info_type  = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 42, "");
    LblID lbl_info_size  = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 58, "");
    LblID lbl_info_perms = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 74, "");
    LblID lbl_info_uid   = app_new_label(win, FM_PANEL_X, cy - FM_PAD + 90, "");
    LblID lbl_info_app   = app_new_label(win, FM_PANEL_X, cy - FM_PAD +106, "");
    app_set_label_color(lbl_info_name,  0x00DDEEFF);
    app_set_label_color(lbl_info_type,  FM_PANEL_TXT);
    app_set_label_color(lbl_info_size,  FM_PANEL_TXT);
    app_set_label_color(lbl_info_perms, FM_PANEL_TXT);
    app_set_label_color(lbl_info_uid,   FM_PANEL_TXT);
    app_set_label_color(lbl_info_app,   FM_PANEL_TXT);

    cy += list_h + FM_PAD;

    // ── Barre de statut ───────────────────────────────────────
    LblID lbl_status = app_new_label(win, cx, cy, "");
    app_set_label_color(lbl_status, FM_STATUS_FG);

    // ── Initialisation ────────────────────────────────────────
    // Démarrer dans le home de l'utilisateur pour éviter les refus ACL
    // sur la racine (UID_SYSTEM, write admin seulement).
    uint32_t start_dir = g_cwd;
    if (g_session_manager.logged_in && g_session_manager.current_session) {
        uint32_t home = g_session_manager.current_session->home_dir_node;
        if (home > 0 && home < g_fs.node_count) start_dir = home;
    }
    g_fm.dir_idx = start_dir;
    g_cwd = start_dir;               // synchroniser g_cwd pour que fs_acl_check
                                     // utilise le bon contexte dès le départ
    g_fm.sel_node_idx    = -1;
    g_fm.clipboard_name[0] = '\0';
    g_fm.clipboard_dir   = 0;

    fm_build_path(g_fm.dir_idx, g_fm.path, FM_PATH_MAX);  // utiliser dir_idx, pas g_cwd
    app_set_label_text(lbl_path, g_fm.path);
    fm_populate_list(lst, g_fm.dir_idx);

    // ============================================================
    // ── BOUCLE PRINCIPALE ───────────────────────────────────────
    // ============================================================
    while (app_running()) {
        app_tick();

        // ── Clic sur un item de la listbox ───────────────────
        if (app_listbox_clicked(lst)) {
            const char* line = app_listbox_selected_text(lst);
            if (line) {
                char name[FM_NAME_MAX]; int is_dir = 0;
                fm_extract_name(line, name, FM_NAME_MAX, &is_dir);
                if (fm_strcmp(name, "(dossier") != 0) {
                    g_fm.sel_node_idx = is_dir
                        ? fs_find_in_dir(g_fm.dir_idx, name)
                        : fs_find_in_dir(g_fm.dir_idx, name);
                } else {
                    g_fm.sel_node_idx = -1;
                }
                // Mettre à jour le panneau d'infos
                fm_update_info_panel(lbl_info_name, lbl_info_type,
                                     lbl_info_size, lbl_info_perms,
                                     lbl_info_uid,  lbl_info_app);
            }
        }

        // ── Double-clic : naviguer ou ouvrir ─────────────────
        if (app_listbox_activated(lst)) {
            const char* line = app_listbox_selected_text(lst);
            if (line) {
                char name[FM_NAME_MAX]; int is_dir = 0;
                fm_extract_name(line, name, FM_NAME_MAX, &is_dir);
                if (fm_strcmp(name, "(dossier") != 0) {
                    if (is_dir) {
                        int ci = fs_find_in_dir(g_fm.dir_idx, name);
                        if (ci >= 0) fm_navigate_to((uint32_t)ci, lst, lbl_path, lbl_status);
                        else { app_set_label_text(lbl_status, "Dossier introuvable."); app_set_label_color(lbl_status, FM_STATUS_ERR); }
                    } else {
                        uint32_t saved_dir = g_fm.dir_idx;
                        g_cwd = g_fm.dir_idx;
                        fm_open_file(name, lbl_status);
                        // app_fileeditor_run a appelé app_init() → widgets détruits.
                        // On sort de la boucle et on reconstruit fileman (pas de récursion).
                        g_cwd = saved_dir;
                        g_fm.dir_idx = saved_dir;
                        need_restart = 1; break;
                    }
                }
            }
        }

        // ── ^ Parent ────────────────────────────────────────
        if (app_button_touched(btn_parent)) {
            uint32_t parent = g_fs.nodes[g_fm.dir_idx].parent;
            if (parent == g_fm.dir_idx || g_fm.dir_idx == 0) {
                app_set_label_text(lbl_status, "Deja a la racine."); app_set_label_color(lbl_status, FM_STATUS_FG);
            } else {
                fm_navigate_to(parent, lst, lbl_path, lbl_status);
            }
        }

        // ── + Dossier ────────────────────────────────────────
        if (app_button_touched(btn_new_dir)) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                errwin_error("Permission refusee", "Creation de dossier non autorisee\npour cette session.");
            } else if (!fs_acl_check(g_fm.dir_idx, ACL_WRITE)) {
                errwin_error("Acces refuse", "Ecriture interdite dans ce dossier.\n(Dossier systeme ou permissions insuffisantes)");
            } else {
                char dname[FM_NAME_MAX]; dname[0] = '\0';
                if (fm_dialog_input("Nouveau dossier", "Nom du dossier :", dname, FM_NAME_MAX)) {
                    uint32_t sv = g_cwd; g_cwd = g_fm.dir_idx;
                    int r = fs_mkdir(dname);
                    g_cwd = sv;
                    if (r >= 0) { app_set_label_text(lbl_status, "Dossier cree."); app_set_label_color(lbl_status, FM_STATUS_OK); fm_populate_list(lst, g_fm.dir_idx); }
                    else        { app_set_label_text(lbl_status, "Echec creation."); app_set_label_color(lbl_status, FM_STATUS_ERR); }
                }
            }
        }

        // ── + Fichier ────────────────────────────────────────
        if (app_button_touched(btn_new_fil)) {
            if (!session_has_permission(PERM_FS_WRITE)) {
                errwin_error("Permission refusee", "Creation de fichier non autorisee\npour cette session.");
            } else if (!fs_acl_check(g_fm.dir_idx, ACL_WRITE)) {
                errwin_error("Acces refuse", "Ecriture interdite dans ce dossier.\n(Dossier systeme ou permissions insuffisantes)");
            } else {
                char fname[FM_NAME_MAX]; fname[0] = '\0';
                if (fm_dialog_input("Nouveau fichier", "Nom du fichier :", fname, FM_NAME_MAX)) {
                    uint32_t sv = g_cwd; g_cwd = g_fm.dir_idx;
                    int already = fs_find(fname);
                    if (already < 0) {
                        int r = fs_write_file(fname, (const uint8_t*)"", 0);
                        if (r >= 0) {
                            app_set_label_text(lbl_status, "Fichier cree."); app_set_label_color(lbl_status, FM_STATUS_OK);
                        } else {
                            app_set_label_text(lbl_status, "Echec creation."); app_set_label_color(lbl_status, FM_STATUS_ERR);
                        }
                    } else {
                        app_set_label_text(lbl_status, "Fichier deja existant."); app_set_label_color(lbl_status, FM_STATUS_WARN);
                    }
                    g_cwd = sv;
                    fm_populate_list(lst, g_fm.dir_idx);
                }
            }
        }

        // ── Renommer ─────────────────────────────────────────
        if (app_button_touched(btn_rename)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0)
                        fm_do_rename(name, lst, lbl_status);
                }
            }
        }

        // ── Copier ───────────────────────────────────────────
        if (app_button_touched(btn_copy)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0) fm_do_copy(name, lbl_status);
                }
            }
        }

        // ── Coller ───────────────────────────────────────────
        if (app_button_touched(btn_paste)) fm_do_paste(lst, lbl_status);

        // ── Supprimer ────────────────────────────────────────
        if (app_button_touched(btn_del)) {
            if (!session_has_permission(PERM_FS_DELETE)) {
                errwin_error("Permission refusee", "Suppression non autorisee\npour cette session.");
            } else if (g_fm.sel_node_idx < 0) {
                app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG);
            } else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0) {
                        char msg[80]; msg[0]='\0';
                        fm_strcat(msg, "Supprimer \"", sizeof(msg)); fm_strcat(msg, name, sizeof(msg)); fm_strcat(msg, "\" ?", sizeof(msg));
                        if (fm_dialog_confirm(msg)) {
                            uint32_t sv = g_cwd; g_cwd = g_fm.dir_idx;
                            int r = fs_delete(name);
                            g_cwd = sv;
                            if (r == 0) {
                                app_set_label_text(lbl_status, "Supprime."); app_set_label_color(lbl_status, FM_STATUS_OK);
                                g_fm.sel_node_idx = -1;
                                fm_populate_list(lst, g_fm.dir_idx);
                            } else {
                                errwin_error2("Suppression impossible", "Dossier non vide ou acces refuse : ", name);
                            }
                        }
                    }
                }
            }
        }

        // ── Ouvrir/Editer ─────────────────────────────────────
        if (app_button_touched(btn_open)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0) {
                        if (isd) {
                            int ci = fs_find_in_dir(g_fm.dir_idx, name);
                            if (ci >= 0) fm_navigate_to((uint32_t)ci, lst, lbl_path, lbl_status);
                        } else {
                            if (!session_has_permission(PERM_FS_READ)) {
                                errwin_error("Permission refusee", "Lecture non autorisee\npour cette session.");
                            } else {
                                uint32_t saved_dir = g_fm.dir_idx;
                                g_cwd = g_fm.dir_idx;
                                app_fileeditor_run(name);
                                g_cwd = saved_dir;
                                g_fm.dir_idx = saved_dir;
                                need_restart = 1; break;
                            }
                        }
                    }
                }
            }
        }

        // ── Executer (.tex) ───────────────────────────────────
        if (app_button_touched(btn_exec)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un .tex."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (!isd && fm_strcmp(fm_get_ext(name), "tex") == 0) {
                        if (!session_has_permission(PERM_TEX_EXECUTE)) {
                            errwin_error("Permission refusee", "Execution de scripts .tex\nnon autorisee pour cette session.");
                        } else {
                            extern int tex_execute(const char*);
                            uint32_t sv = g_cwd; g_cwd = g_fm.dir_idx;
                            tex_execute(name);
                            g_cwd = sv;
                            app_set_label_text(lbl_status, "Script execute."); app_set_label_color(lbl_status, FM_STATUS_OK);
                        }
                    } else {
                        app_set_label_text(lbl_status, "Fichier .tex requis."); app_set_label_color(lbl_status, FM_STATUS_WARN);
                    }
                }
            }
        }

        // ── Permissions (chmod) ───────────────────────────────
        if (app_button_touched(btn_chmod)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0) {
                        fm_dialog_chmod(name, g_fm.sel_node_idx);
                        app_set_label_text(lbl_status, "Permissions mises a jour."); app_set_label_color(lbl_status, FM_STATUS_OK);
                    }
                }
            }
        }

        // ── Proprietaire (chown) ──────────────────────────────
        if (app_button_touched(btn_chown)) {
            if (g_fm.sel_node_idx < 0) { app_set_label_text(lbl_status, "Selectionner un element."); app_set_label_color(lbl_status, FM_STATUS_FG); }
            else {
                const char* line = app_listbox_selected_text(lst);
                if (line) {
                    char name[FM_NAME_MAX]; int isd=0;
                    fm_extract_name(line, name, FM_NAME_MAX, &isd);
                    if (fm_strcmp(name, "(dossier") != 0) {
                        fm_dialog_chown(name, lbl_status);
                    }
                }
            }
        }
    }
  } while (need_restart);
}

// ── Point d'entrée TEX ───────────────────────────────────────
void app_fileman(void) { app_fileman_run(); }