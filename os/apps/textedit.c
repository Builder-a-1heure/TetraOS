// apps/textedit.c — Éditeur de fichiers .txt pour TetraOS
// Utilise AppCore v2 (lib/appcore.h) pour l'interface graphique.
//
// Lancement depuis le shell :
//   app_textedit_run("")              → nouveau fichier vide
//   app_textedit_run("notes.txt")    → ouvre notes.txt (ou le crée)
//
// Contrôles dans l'éditeur :
//   Flèches         — déplacer le curseur
//   Entrée          — nouvelle ligne
//   Backspace       — effacer caractère avant curseur
//   Ctrl+S          — sauvegarder
//   Ctrl+N          — nouveau fichier (demande confirmation si modifié)
//   ESC             — quitter (demande confirmation si modifié)
//
// Architecture AppCore v2 :
//   - La fenêtre principale est gérée via app_new_window / app_tick.
//   - Le rendu du contenu texte est injecté dans le framebuffer directement
//     via les primitives gfx_* entre deux appels à app_tick.
//   - Les boîtes de dialogue (confirmation, saisie nom) ouvrent une seconde
//     fenêtre AppCore et tournent dans leur propre mini-boucle imbriquée,
//     bloquant l'éditeur le temps de la réponse utilisateur.
//   - La lecture clavier non-bloquante est dupliquée depuis appcore.c
//     (même port 0x64) : on l'appelle AVANT app_tick pour ne pas consommer
//     deux fois le même octet.
// ============================================================

#include "textedit.h"
#include "../lib/appcore.h"
#include "../fs/fs.h"
#include "../lib/utils.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../apps/app.h"
#include <stdint.h>

// Globaux de input.c — partagés avec appcore.c
extern int           shift_pressed;
extern int           ctrl_pressed;
extern unsigned char keyboard_map[256];
extern unsigned char keyboard_map_shift[256];


// ============================================================
// Constantes
// ============================================================
#define TE_MAX_CONTENT  8192
#define TE_MAX_LINES    512
#define TE_FILENAME_MAX 63

// Couleurs zone texte
#define TE_BG           0x00080D14
#define TE_FG           0x00DDEEFF
#define TE_CURSOR_COL   0x0000CCFF
#define TE_LINENUM_FG   0x00445566
#define TE_LINENUM_BG   0x00060B12
#define TE_STATUS_BG    0x00001133
#define TE_STATUS_FG    0x00AABBCC
#define TE_STATUS_MOD   0x00FFAA00

// Dimensions fenêtre principale
#define TE_WIN_X        30
#define TE_WIN_Y        20
#define TE_WIN_W        740
#define TE_WIN_H        520

// Layout zone texte
#define TE_LINENUM_W    40
#define TE_TEXT_PAD     6
#define TE_FONT_W       8
#define TE_FONT_H       16

// ============================================================
// Helpers chaînes (pas de stdlib dans TetraOS)
// ============================================================
static int te_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void te_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// snprintf minimal : %d et %s uniquement
static void te_fmt(char* buf, int sz, const char* fmt, ...) {
    // Implémentation directe sans va_list pour garder la portabilité bare-metal.
    // On parse fmt caractère par caractère.
    // NOTE : n'utiliser qu'avec %d et %s comme dans ce fichier.
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int pos = 0;
    for (int i = 0; fmt[i] && pos < sz - 1; i++) {
        if (fmt[i] != '%') { buf[pos++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == 'd') {
            int val = __builtin_va_arg(ap, int);
            char tmp[16]; int tl = 0, neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            if (val == 0) { tmp[tl++] = '0'; }
            while (val > 0) { tmp[tl++] = '0' + (val % 10); val /= 10; }
            if (neg) tmp[tl++] = '-';
            for (int k = tl - 1; k >= 0 && pos < sz - 1; k--)
                buf[pos++] = tmp[k];
        } else if (fmt[i] == 's') {
            const char* s = __builtin_va_arg(ap, const char*);
            while (*s && pos < sz - 1) buf[pos++] = *s++;
        } else {
            buf[pos++] = '%';
            if (pos < sz - 1) buf[pos++] = fmt[i];
        }
    }
    buf[pos] = '\0';
    __builtin_va_end(ap);
}

// ============================================================
// État de l'éditeur
// ============================================================
typedef struct {
    char filename[TE_FILENAME_MAX + 1];
    char content[TE_MAX_CONTENT];
    int  len;
    int  cursor;
    int  scroll_line;
    int  modified;

    const char* lines[TE_MAX_LINES];
    int         line_lens[TE_MAX_LINES];
    int         line_count;

    int cur_line;
    int cur_col;
    int vis_cols;
    int vis_rows;
} TeState;

static TeState g_te;

// ============================================================
// Gestion du buffer texte
// ============================================================

static void te_rebuild_lines(void) {
    g_te.line_count = 0;
    g_te.lines[0]   = g_te.content;
    int lc = 0;
    for (int i = 0; i <= g_te.len && lc < TE_MAX_LINES - 1; i++) {
        if (g_te.content[i] == '\n' || i == g_te.len) {
            int start = (int)(g_te.lines[lc] - g_te.content);
            g_te.line_lens[lc] = i - start;
            lc++;
            if (i < g_te.len)
                g_te.lines[lc] = g_te.content + i + 1;
        }
    }
    g_te.line_count = lc ? lc : 1;
}

static void te_update_cursor_pos(void) {
    int line = 0, col = 0;
    for (int i = 0; i < g_te.cursor; i++) {
        if (g_te.content[i] == '\n') { line++; col = 0; }
        else col++;
    }
    g_te.cur_line = line;
    g_te.cur_col  = col;
}

static int te_line_start(int line) {
    if (line <= 0) return 0;
    if (line >= g_te.line_count) return g_te.len;
    return (int)(g_te.lines[line] - g_te.content);
}

static void te_insert(char c) {
    if (g_te.len >= TE_MAX_CONTENT - 1) return;
    for (int i = g_te.len; i >= g_te.cursor; i--)
        g_te.content[i + 1] = g_te.content[i];
    g_te.content[g_te.cursor] = c;
    g_te.cursor++;
    g_te.len++;
    g_te.modified = 1;
    te_rebuild_lines();
    te_update_cursor_pos();
}

static void te_backspace(void) {
    if (g_te.cursor <= 0) return;
    for (int i = g_te.cursor - 1; i < g_te.len; i++)
        g_te.content[i] = g_te.content[i + 1];
    g_te.cursor--;
    g_te.len--;
    g_te.modified = 1;
    te_rebuild_lines();
    te_update_cursor_pos();
}

static void te_ensure_scroll(void) {
    if (g_te.cur_line < g_te.scroll_line)
        g_te.scroll_line = g_te.cur_line;
    if (g_te.cur_line >= g_te.scroll_line + g_te.vis_rows)
        g_te.scroll_line = g_te.cur_line - g_te.vis_rows + 1;
    if (g_te.scroll_line < 0) g_te.scroll_line = 0;
}

// ============================================================
// Fichier
// ============================================================

static int te_save(void) {
    if (g_te.filename[0] == '\0') return 0;
    int r = fs_write_file(g_te.filename,
                          (const uint8_t*)g_te.content,
                          (uint32_t)g_te.len);
    if (r >= 0) { g_te.modified = 0; return 1; }
    return 0;
}

static void te_load(const char* filename) {
    te_strcpy(g_te.filename, filename, TE_FILENAME_MAX + 1);
    g_te.len = 0;
    g_te.cursor = 0;
    g_te.scroll_line = 0;
    g_te.modified = 0;
    for (int i = 0; i < TE_MAX_CONTENT; i++) g_te.content[i] = 0;

    uint8_t tmp[TE_MAX_CONTENT];
    int r = fs_read_file(filename, tmp, TE_MAX_CONTENT - 1);
    if (r > 0) {
        tmp[r] = '\0';
        for (int i = 0; i <= r; i++) g_te.content[i] = (char)tmp[i];
        g_te.len    = r;
        g_te.cursor = r;
    }
    te_rebuild_lines();
    te_update_cursor_pos();
}


// ============================================================
// Rendu du contenu texte (dessin direct dans le framebuffer)
// Appelé après app_tick / redraw_all de AppCore.
// win_x, win_y : coin haut-gauche de la fenêtre AppCore
// win_w, win_h : dimensions de la zone CLIENT (sans la titlebar)
// ============================================================
static void te_draw_content(int win_x, int win_y, int win_w, int win_h) {
    int cx = win_x;
    int cy = win_y;   // win_y est déjà la coordonnée zone client (titlebar exclue)
    int cw = win_w;
    int ch = win_h;

    // --- Barre de statut ---
    int status_y = cy + ch - TE_FONT_H - 4;
    gfx_fill_rect(cx, status_y, cw, TE_FONT_H + 4, TE_STATUS_BG);

    char status[128];
    const char* fname = g_te.filename[0] ? g_te.filename : "(nouveau)";
    te_fmt(status, sizeof(status), " %s  —  L%d:C%d",
           fname, g_te.cur_line + 1, g_te.cur_col + 1);
    gfx_draw_text(cx + 4, status_y + 2, status, TE_STATUS_FG, TE_STATUS_BG);

    // Raccourcis clavier centrés dans la barre
    gfx_draw_text(cx + cw / 2 - 140, status_y + 2,
                  "Ctrl+S: Sauvegarder  |  Ctrl+N: Nouveau  |  ESC: Quitter",
                  0x00334466, TE_STATUS_BG);

    // Indicateur modifié / sauvegardé (à droite)
    if (g_te.modified) {
        gfx_draw_text(cx + cw - 80, status_y + 2,
                      "[modifie]", TE_STATUS_MOD, TE_STATUS_BG);
    } else {
        gfx_draw_text(cx + cw - 88, status_y + 2,
                      "[sauvegarde]", 0x0033AA44, TE_STATUS_BG);
    }

    // --- Zone texte ---
    int text_area_h = status_y - cy;

    // Colonne numéros de ligne
    gfx_fill_rect(cx, cy, TE_LINENUM_W, text_area_h, TE_LINENUM_BG);
    gfx_draw_line(cx + TE_LINENUM_W - 1, cy,
                  cx + TE_LINENUM_W - 1, cy + text_area_h - 1,
                  0x00002244);

    // Fond zone texte
    gfx_fill_rect(cx + TE_LINENUM_W, cy,
                  cw - TE_LINENUM_W, text_area_h, TE_BG);

    // Nombre de lignes / colonnes visibles
    g_te.vis_rows = text_area_h / TE_FONT_H;
    g_te.vis_cols = (cw - TE_LINENUM_W - TE_TEXT_PAD * 2) / TE_FONT_W;
    if (g_te.vis_rows < 1) g_te.vis_rows = 1;
    if (g_te.vis_cols < 1) g_te.vis_cols = 1;

    te_ensure_scroll();

    // Rendu ligne par ligne
    for (int row = 0; row < g_te.vis_rows; row++) {
        int line_idx = g_te.scroll_line + row;
        int py = cy + row * TE_FONT_H;

        // Numéro de ligne
        if (line_idx < g_te.line_count) {
            char lnum[8];
            te_fmt(lnum, sizeof(lnum), "%d", line_idx + 1);
            // Aligner à droite dans 3 caractères
            int ll = te_strlen(lnum);
            char lnum_pad[5] = "   ";
            for (int k = 0; k < ll && k < 3; k++)
                lnum_pad[2 - (ll - 1 - k)] = lnum[k];
            lnum_pad[3] = '\0';
            uint32_t lnum_fg = (line_idx == g_te.cur_line)
                               ? 0x0055AAFF : TE_LINENUM_FG;
            gfx_draw_text(cx + 2, py, lnum_pad, lnum_fg, TE_LINENUM_BG);
        }

        // Surligneur ligne courante
        if (line_idx == g_te.cur_line) {
            gfx_fill_rect(cx + TE_LINENUM_W, py,
                          cw - TE_LINENUM_W, TE_FONT_H,
                          0x0009141E);
        }

        // Texte de la ligne
        if (line_idx < g_te.line_count) {
            const char* ls = g_te.lines[line_idx];
            int          ll = g_te.line_lens[line_idx];
            int tx = cx + TE_LINENUM_W + TE_TEXT_PAD;
            uint32_t line_bg = (line_idx == g_te.cur_line) ? 0x0009141E : TE_BG;

            for (int col = 0; col < g_te.vis_cols && col < ll; col++) {
                char glyph[2] = { ls[col], '\0' };
                gfx_draw_text(tx + col * TE_FONT_W, py, glyph, TE_FG, line_bg);
            }
        }
    }

    // --- Curseur (barre verticale 2px) ---
    {
        int col_disp  = g_te.cur_col;
        if (col_disp > g_te.vis_cols - 1) col_disp = g_te.vis_cols - 1;
        int screen_row = g_te.cur_line - g_te.scroll_line;
        if (screen_row >= 0 && screen_row < g_te.vis_rows) {
            int cur_px = cx + TE_LINENUM_W + TE_TEXT_PAD + col_disp * TE_FONT_W;
            int cur_py = cy + screen_row * TE_FONT_H;
            gfx_fill_rect(cur_px, cur_py, 2, TE_FONT_H, TE_CURSOR_COL);
        }
    }

    vesa_invalidate_all();
}

// ============================================================
// Redraw conditionnel : on ne repaint que si nécessaire.
// AppCore pose dirty=1 sur la fenêtre quand il la redessine
// (drag, focus, hover). On suit la même logique avec te_dirty.
// ============================================================
static int g_te_dirty;   // 1 = contenu texte doit être repeint

// Position et taille de la zone cliente de la fenêtre principale.
static int g_main_client_x;
static int g_main_client_y;
static int g_main_client_w;
static int g_main_client_h;

// Appelé à chaque cycle après app_tick.
// - Si force=1 ou g_te_dirty=1 : repaint complet (efface curseur souris,
//   redessine le texte, réaffiche le curseur souris).
// - Sinon : no-op.
// Note : app_tick peut avoir redessiné le chrome (drag) → notre texte
// est perdu. On le détecte via g_te_dirty qu'on lève après chaque frappe,
// et via le flag g_chrome_redrawn qu'on lève nous-mêmes avant app_tick
// quand on sait que le chrome va changer (premier cycle, post-dialogue…).
// Pour les drags, AppCore redessine ET met dirty=1 en interne ; comme
// te_draw_content est peu coûteux (fills + text directs), on l'appelle
// à chaque cycle sans les opérations curseur souris — seule la partie
// erase/draw curseur est conditionnée à un vrai changement.
static void te_full_redraw(int force) {
    if (!force && !g_te_dirty) return;
    mouse_erase_cursor();
    te_draw_content(g_main_client_x, g_main_client_y,
                    g_main_client_w, g_main_client_h);
    mouse_draw_cursor();
    g_te_dirty = 0;
}

// ============================================================
// Gestion des touches
// Retourne : 0 = continuer, 1 = quitter, 2 = nouveau fichier,
//            et positionne *do_save si Ctrl+S.
// ============================================================
static int te_handle_key(char c, int* do_save) {
    *do_save = 0;

    if (c == 19) { *do_save = 1; return 0; }     // Ctrl+S
    if (c == 29) return 2;                        // Ctrl+N → nouveau fichier

    if (c == 27) return 1;                        // ESC → quitter

    if (c == '\n' || c == '\r') { te_insert('\n'); return 0; }
    if (c == '\b')               { te_backspace();  return 0; }

    // Flèche haut (16)
    if (c == 16) {
        if (g_te.cur_line > 0) {
            int ps = te_line_start(g_te.cur_line - 1);
            int pl = g_te.line_lens[g_te.cur_line - 1];
            int tc = g_te.cur_col < pl ? g_te.cur_col : pl;
            g_te.cursor = ps + tc;
            te_update_cursor_pos();
        }
        return 0;
    }

    // Flèche bas (14)
    if (c == 14) {
        if (g_te.cur_line < g_te.line_count - 1) {
            int ns = te_line_start(g_te.cur_line + 1);
            int nl = g_te.line_lens[g_te.cur_line + 1];
            int tc = g_te.cur_col < nl ? g_te.cur_col : nl;
            g_te.cursor = ns + tc;
            te_update_cursor_pos();
        }
        return 0;
    }

    // Flèche gauche (17)
    if (c == 17) {
        if (g_te.cursor > 0) { g_te.cursor--; te_update_cursor_pos(); }
        return 0;
    }

    // Flèche droite (18)
    if (c == 18) {
        if (g_te.cursor < g_te.len) { g_te.cursor++; te_update_cursor_pos(); }
        return 0;
    }

    // Caractère imprimable
    if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        te_insert(c); return 0;
    }

    return 0;
}

// ============================================================
// Callback fermeture boîte de dialogue (évite fermeture auto par AppCore)
// ============================================================
static int g_dlg_close_flag;

static void te_dlg_close_cb(WinID wid __attribute__((unused))) {
    g_dlg_close_flag = 1;
}

// ============================================================
// Boîte de dialogue "Confirmer" (Oui / Non)
// Ouvre une fenêtre AppCore secondaire, tourne dans sa propre
// mini-boucle, puis se ferme. Retourne 1 si "Oui".
// ============================================================
static int te_confirm(const char* message) {
    WinID dlg = app_new_window("Confirmation", 220, 200, 360, 110);
    if (dlg == APPCORE_INVALID) return 0;

    g_dlg_close_flag = 0;
    app_on_close(dlg, te_dlg_close_cb);   // intercepte [X] sans fermeture auto

    app_new_label(dlg, 20, 18, message);

    BtnID btn_oui = app_new_button(dlg, 200, 58, 70, 28, "Oui");
    BtnID btn_non = app_new_button(dlg, 278, 58, 70, 28, "Non");

    int result = 0;
    int done   = 0;

    while (!done) {
        app_tick();
        if (app_button_touched(btn_oui)) { result = 1; done = 1; }
        if (app_button_touched(btn_non)) { result = 0; done = 1; }
        if (g_dlg_close_flag)            { result = 0; done = 1; }
    }

    app_close_window(dlg);
    return result;
}

// ============================================================
// Boîte de dialogue "Enregistrer sous" (saisie nom de fichier)
//
// AppCore v2 ne propose pas de widget champ de saisie texte.
// On simule une saisie via le clavier : on ouvre une fenêtre
// avec un label qui affiche la frappe en cours, et un bouton OK.
// ============================================================
static int te_ask_filename(char* filename_out) {
    WinID dlg = app_new_window("Nom du fichier", 190, 185, 420, 140);
    if (dlg == APPCORE_INVALID) return 0;

    g_dlg_close_flag = 0;
    app_on_close(dlg, te_dlg_close_cb);

    app_new_label(dlg, 20, 16, "Nom du fichier (ex: notes.txt) :");
    char  input_buf[TE_FILENAME_MAX + 1];
    input_buf[0] = '\0';
    int   input_len = 0;

    // On affiche un label qui se met à jour à chaque frappe
    LblID lbl_input = app_new_label(dlg, 20, 40, "");
    app_set_label_color(lbl_input, 0x00FFFFFF);

    BtnID btn_ok  = app_new_button(dlg, 270, 90, 60, 30, "OK");
    BtnID btn_ann = app_new_button(dlg, 338, 90, 70, 30, "Annuler");

    int result = 0;
    int done   = 0;

    while (!done) {
        app_tick();
        // Récupère la touche lue par app_tick — pas de double lecture du port
        char c = app_tick_get_key();

        // Gestion saisie texte dans la boîte de dialogue
        if (c != 0) {
            if (c == '\n' || c == '\r') {
                // Valider
                if (input_len > 0) { result = 1; done = 1; }
            } else if (c == '\b') {
                if (input_len > 0) {
                    input_buf[--input_len] = '\0';
                    app_set_label_text(lbl_input, input_buf);
                }
            } else if (c == 27) {
                result = 0; done = 1;
            } else if ((unsigned char)c >= 32 && (unsigned char)c < 127
                       && input_len < TE_FILENAME_MAX) {
                input_buf[input_len++] = c;
                input_buf[input_len]   = '\0';
                app_set_label_text(lbl_input, input_buf);
            }
        }

        if (app_button_touched(btn_ok) && input_len > 0) {
            result = 1; done = 1;
        }
        if (app_button_touched(btn_ann)) {
            result = 0; done = 1;
        }
        if (g_dlg_close_flag) {
            result = 0; done = 1;
        }
    }

    if (result)
        te_strcpy(filename_out, input_buf, TE_FILENAME_MAX + 1);

    app_close_window(dlg);
    return result;
}

// ============================================================
// Mise à jour du titre de la fenêtre principale
// ============================================================
static void te_update_title(WinID win) {
    char title[80];
    const char* fname = g_te.filename[0] ? g_te.filename : "Nouveau fichier";
    if (g_te.modified)
        te_fmt(title, sizeof(title), "TextEdit - %s *", fname);
    else
        te_fmt(title, sizeof(title), "TextEdit - %s", fname);
    app_set_title(win, title);
}

// ============================================================
// Callback fermeture fenêtre principale
// ============================================================
static int g_main_close_requested;

static void te_main_close_cb(WinID wid __attribute__((unused))) {
    // On intercepte [X] : on gèrera la confirmation dans la boucle.
    g_main_close_requested = 1;
}

// ============================================================
// Boucle principale de l'éditeur
// ============================================================
static void te_editor_loop(void) {
    g_main_client_x = TE_WIN_X;
    g_main_client_y = TE_WIN_Y + AC_WIN_TITLE_H;
    g_main_client_w = TE_WIN_W;
    g_main_client_h = TE_WIN_H;

    g_te.vis_rows = g_main_client_h / TE_FONT_H;
    g_te.vis_cols = (g_main_client_w - TE_LINENUM_W - TE_TEXT_PAD * 2) / TE_FONT_W;

    WinID main_win = app_new_window("TextEdit - Nouveau fichier",
                                    TE_WIN_X, TE_WIN_Y,
                                    TE_WIN_W, TE_WIN_H);
    if (main_win == APPCORE_INVALID) return;

    g_main_close_requested = 0;
    app_on_close(main_win, te_main_close_cb);
    te_update_title(main_win);

    // Premier rendu forcé
    g_te_dirty = 1;
    app_tick();
    te_full_redraw(1);

    int running = 1;

    while (running) {
        // 1. AppCore gère souris / drag / hover / redraw chrome + lit le clavier
        app_tick();

        // 2. Récupère la touche lue par app_tick — une seule lecture du port
        char c = app_tick_get_key();

        // 3. Si AppCore a redessiné le chrome ce tick (drag, focus, hover),
        //    notre texte est écrasé → forcer le repaint.
        //    On met aussi à jour les coordonnées client si la fenêtre a bougé.
        if (app_was_redrawn()) {
            int nx, ny;
            app_get_win_pos(main_win, &nx, &ny);
            g_main_client_x = nx;
            g_main_client_y = ny + AC_WIN_TITLE_H;
            g_te_dirty = 1;
        }

        // 4. Fermeture [X]
        if (g_main_close_requested) {
            g_main_close_requested = 0;
            if (g_te.modified) {
                if (!te_confirm("Quitter sans sauvegarder ?")) {
                    g_te_dirty = 1;
                    te_full_redraw(1);
                    continue;
                }
            }
            running = 0;
            break;
        }

        // 5. Traitement clavier
        if (c != 0) {
            int do_save = 0;
            int action  = te_handle_key(c, &do_save);
            g_te_dirty = 1;   // toute frappe → redraw

            if (do_save) {
                if (g_te.filename[0] == '\0') {
                    char newname[TE_FILENAME_MAX + 1] = {0};
                    if (te_ask_filename(newname)) {
                        te_strcpy(g_te.filename, newname, TE_FILENAME_MAX + 1);
                        te_save();
                        te_update_title(main_win);
                    }
                } else {
                    te_save();
                    te_update_title(main_win);
                }
                g_te_dirty = 1;
            }

            if (action == 1) {   // ESC → quitter
                if (g_te.modified) {
                    if (!te_confirm("Quitter sans sauvegarder ?")) {
                        g_te_dirty = 1;
                        te_full_redraw(1);
                        continue;
                    }
                }
                running = 0;
                break;
            }

            if (action == 2) {   // Ctrl+N → nouveau fichier
                if (g_te.modified) {
                    if (!te_confirm("Abandonner les modifications ?")) {
                        g_te_dirty = 1;
                        te_full_redraw(1);
                        continue;
                    }
                }
                for (int i = 0; i < TE_MAX_CONTENT; i++) g_te.content[i] = 0;
                g_te.filename[0] = '\0';
                g_te.len         = 0;
                g_te.cursor      = 0;
                g_te.scroll_line = 0;
                g_te.modified    = 0;
                te_rebuild_lines();
                te_update_cursor_pos();
                te_update_title(main_win);
                g_te_dirty = 1;
            }
        }

        // 6. Repaint texte si nécessaire
        te_full_redraw(0);
    }

    app_close_window(main_win);
}

// ============================================================
// Point d'entrée public
// ============================================================
void app_textedit_run(const char* filename) {
    // Remise à zéro complète de l'état
    for (int i = 0; i < (int)sizeof(TeState); i++)
        ((char*)&g_te)[i] = 0;

    if (filename && filename[0] != '\0') {
        // Créer le fichier s'il n'existe pas
        if (fs_find(filename) < 0)
            fs_write_file(filename, (const uint8_t*)"", 0);
        te_load(filename);
    } else {
        te_rebuild_lines();
    }

    // Initialiser AppCore (réinitialise toutes les fenêtres/widgets)
    app_init();

    te_editor_loop();
}