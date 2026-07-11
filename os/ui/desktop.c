// ui/desktop.c — Bureau graphique TetraOS
//
// Responsabilités UNIQUEMENT :
//   - Fond dégradé + taskbar
//   - Icônes dynamiques (scanner TEX)
//   - Hover + clic → launch_app()
//   - Souris + curseur
//
// Tout ce qui concerne le terminal (buffer, chrome fenêtre, titlebar,
// boucle shell) est dans apps/terminal.c.
//
// Découverte des apps :
//   tex_scan() parcourt [_kernel_start, _kernel_end[ à la recherche
//   du magic 0x54455800. Chaque TexHeader valide avec APP_FLAG_DESKTOP
//   devient une icône. desktop.c ne connaît aucune app à la compilation.

#include "desktop.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../ui/session.h"
#include "../lib/utils.h"
#include "../lib/appcore.h"
#include "../lib/process.h"
#include "../apps/app.h"
#include "../gfx/wallpaper.h"
#include <stdint.h>
#include "../fs/fs.h"

#include "../shell/tex.h"

// ============================================================
// Symboles linker — bornes de la mémoire kernel
// ============================================================
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

// ============================================================
// Palette bureau (taskbar, icônes — le fond est maintenant une image)
// ============================================================
#define DT_TASKBAR_BG   0x00050510
#define DT_TASKBAR_LINE 0x00002244
#define DT_ICON_BG      0x00001133
#define DT_ICON_BORDER  0x000055CC
#define DT_ICON_HOVER   0x00003366
#define DT_ICON_TEXT    0x00FFFFFF
#define DT_CLOCK_FG     0x00AAAACC
#define DT_WHITE        0x00FFFFFF
#define DT_ACCENT       0x0000AAFF

// Couleurs graphismes icônes
#define DT_TERM_PROMPT  0x0033CCFF
#define DT_TERM_FG      0x00CCDDFF
#define DT_PAGE_BG      0x00D8E8F8
#define DT_PAGE_LN      0x003366AA
#define DT_FOLD_TOP     0x00CC9900
#define DT_FOLD_BG      0x00FFCC33
#define DT_GEAR_COL     0x00AAAACC

// ============================================================
// Icônes bureau
// ============================================================
#define ICON_W            72
#define ICON_H            72
#define ICON_LABEL_H      14
#define ICON_STRIDE       (ICON_H + ICON_LABEL_H + 16)
#define ICON_COL_X        80
#define ICON_START_Y      60
#define DESKTOP_MAX_ICONS 8

typedef struct {
    int        x, y;
    int        hovered;
    TexHeader* hdr;             // pointeur vers le TexHeader natif (apps C compilées)
    int        is_tex_script;   // 1 = script .tex du FS, 0 = app C native
    char       tex_filename[64];// chemin du script si is_tex_script == 1
    TexHeader  tex_hdr_copy;    // copie synthétique du header pour les scripts
} IconState;

// ============================================================
// État global
// ============================================================
static IconState g_icons[DESKTOP_MAX_ICONS];
static int       g_icon_count = 0;
static int       g_prev_left  = 0;

// ============================================================
// Scanner TEX
// ============================================================
static void tex_scan(void) {
    g_icon_count = 0;

    // Scanner uniquement sur des adresses alignées à 4 octets.
    // Le magic 0x54455800 peut apparaître par hasard dans le code ou
    // les données — l'alignement + les validations ci-dessous éliminent
    // les faux positifs qui faisaient planter le bureau au démarrage.
    uintptr_t start = (uintptr_t)_kernel_start;
    if (start & 3) start = (start + 4) & ~(uintptr_t)3;
    uintptr_t end = (uintptr_t)_kernel_end;
    if (end > sizeof(TexHeader)) end -= sizeof(TexHeader);

    for (uintptr_t addr = start; addr < end && g_icon_count < DESKTOP_MAX_ICONS; addr += 4) {
        if (*(uint32_t*)addr != TEX_MAGIC) continue;

        TexHeader* hdr = (TexHeader*)addr;

        // Nom ASCII valide et non vide
        if (hdr->name[0] < 32 || hdr->name[0] >= 127) continue;

        // entry dans les bornes kernel
        uint8_t* eptr = (uint8_t*)(uintptr_t)hdr->entry;
        if (eptr < _kernel_start || eptr >= _kernel_end) continue;

        // reserved doit être 0 (non encore utilisé)
        if (hdr->reserved != 0) continue;

        if (hdr->flags & APP_FLAG_DESKTOP) {
            g_icons[g_icon_count].hdr     = hdr;
            g_icons[g_icon_count].x       = ICON_COL_X;
            g_icons[g_icon_count].y       = ICON_START_Y + g_icon_count * ICON_STRIDE;
            g_icons[g_icon_count].hovered = 0;
            g_icon_count++;
        }
    }
}

// ============================================================
// Scanner les scripts .tex du FS qui contiennent @TEX_APP
// ============================================================
//
// Format de la signature dans le script (n'importe où dans les 8 premières lignes) :
//
//   @TEX_APP(nom, icone, version)
//
//   nom     : affiché sous l'icône (max 15 chars)
//   icone   : terminal | textedit | fileman | settings | generic
//   version : ex. "1.0"
//
// Exemple dans un script .tex :
//   // @TEX_APP(MonApp, generic, 1.0)
//   import io
//   io.println("Hello")
//
// Le commentaire // est optionnel — la ligne peut commencer directement par @TEX_APP.

static AppIconType parse_icon_type(const char* s) {
    // Comparer manuellement (pas de strcmp disponible facilement ici)
    if (s[0]=='t' && s[1]=='e' && s[2]=='r') return APPICON_TERMINAL;
    if (s[0]=='t' && s[1]=='e' && s[2]=='x') return APPICON_TEXTEDIT;
    if (s[0]=='f' && s[1]=='i')              return APPICON_FILEMAN;
    if (s[0]=='s' && s[1]=='e')              return APPICON_SETTINGS;
    return APPICON_GENERIC;
}

static void tex_scan_scripts(void) {
    // Lister les nœuds du répertoire courant (g_cwd) et en chercher en .tex
    // On passe par fs_list_nodes via l'accès direct à g_fs
    extern FSTable g_fs;
    extern uint32_t g_cwd;

    static uint8_t script_buf[512]; // buffer de lecture partiel (header seulement)

    for (uint32_t ni = 0; ni < g_fs.node_count && g_icon_count < DESKTOP_MAX_ICONS; ni++) {
        FSNode* node = &g_fs.nodes[ni];

        // Ignorer dossiers et nœuds vides
        if (node->is_dir) continue;
        if (node->name[0] == '\0') continue;

        // Vérifier extension .tex
        int nlen = 0;
        while (node->name[nlen] && nlen < FS_NAME_LEN) nlen++;
        if (nlen < 5) continue; // trop court pour "x.tex"
        if (node->name[nlen-4] != '.' ||
            node->name[nlen-3] != 't' ||
            node->name[nlen-2] != 'e' ||
            node->name[nlen-1] != 'x') continue;

        // Lire les 512 premiers octets du script
        int bytes = fs_read_file(node->name, script_buf, sizeof(script_buf) - 1);
        if (bytes <= 0) continue;
        script_buf[bytes] = '\0';

        // Chercher @TEX_APP dans les 8 premières lignes
        char* sig_pos = 0;
        int   lines_checked = 0;
        char* p = (char*)script_buf;
        while (*p && lines_checked < 8) {
            // Sauter espaces/commentaires en début de ligne
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '/' && p[1] == '/') p += 2; // sauter //
            while (*p == ' ' || *p == '\t') p++;
            // Chercher @TEX_APP
            if (p[0] == '@' && p[1] == 'T' && p[2] == 'E' && p[3] == 'X' &&
                p[4] == '_' && p[5] == 'A' && p[6] == 'P' && p[7] == 'P') {
                sig_pos = p;
                break;
            }
            // Aller à la ligne suivante
            while (*p && *p != '\n') p++;
            if (*p == '\n') { p++; lines_checked++; }
        }
        if (!sig_pos) continue;

        // Parser @TEX_APP(nom, icone, version)
        char app_name[16]; app_name[0] = '\0';
        char app_icon[16]; app_icon[0] = '\0';

        p = sig_pos + 8; // après @TEX_APP
        while (*p == ' ') p++;
        if (*p == '(') p++;

        // Argument 1 : nom
        int i = 0;
        while (*p && *p != ',' && *p != ')' && i < 15)
            app_name[i++] = *p++;
        app_name[i] = '\0';
        // Trim espaces droite
        while (i > 0 && (app_name[i-1] == ' ' || app_name[i-1] == '\t')) app_name[--i] = '\0';
        if (*p == ',') p++;

        // Argument 2 : icone
        while (*p == ' ') p++;
        i = 0;
        while (*p && *p != ',' && *p != ')' && i < 15)
            app_icon[i++] = *p++;
        app_icon[i] = '\0';
        while (i > 0 && (app_icon[i-1] == ' ' || app_icon[i-1] == '\t')) app_icon[--i] = '\0';

        if (app_name[0] == '\0') continue; // pas de nom = pas d'icône

        // Construire l'IconState pour ce script
        IconState* st = &g_icons[g_icon_count];
        st->is_tex_script = 1;
        st->hovered = 0;
        st->x = ICON_COL_X;
        st->y = ICON_START_Y + g_icon_count * ICON_STRIDE;

        // Copier le nom du fichier
        for (int k = 0; k < 63 && node->name[k]; k++) st->tex_filename[k] = node->name[k];
        st->tex_filename[63] = '\0';

        // Remplir le header synthétique
        st->tex_hdr_copy.magic     = TEX_MAGIC;
        st->tex_hdr_copy.icon_type = (uint8_t)parse_icon_type(app_icon);
        st->tex_hdr_copy.ver_major = 1;
        st->tex_hdr_copy.ver_minor = 0;
        st->tex_hdr_copy.flags     = APP_FLAG_DESKTOP;
        st->tex_hdr_copy.entry     = 0; // pas de fonction C — on passe par tex_execute
        st->tex_hdr_copy.reserved  = 0;
        for (int k = 0; k < 15; k++) st->tex_hdr_copy.name[k] = app_name[k];
        st->tex_hdr_copy.name[15]  = '\0';

        // Pointer hdr sur la copie locale
        st->hdr = &st->tex_hdr_copy;

        g_icon_count++;
    }
}

// ============================================================
// Hit-test icône
// ============================================================
static int icon_hit(int idx, int mx, int my) {
    int cx = g_icons[idx].x, cy = g_icons[idx].y;
    return mx >= cx - ICON_W/2 && mx <= cx + ICON_W/2 &&
           my >= cy - ICON_H/2 && my <= cy + ICON_H/2;
}

// ============================================================
// Dessin graphisme icône selon AppIconType
// ============================================================
static void draw_icon_gfx(int ix, int iy, AppIconType type) {
    int x = ix + 6, y = iy + 8;
    int w = ICON_W - 12, h = ICON_H - 22;

    switch (type) {

        case APPICON_TERMINAL:
            gfx_fill_rect(x, y, w, h, 0x00000000);
            gfx_draw_rect(x, y, w, h, DT_ICON_BORDER);
            gfx_fill_rect(x+1, y+1, w-2, 5, DT_ICON_BORDER);
            gfx_draw_text(x+3, y+8,  ">", DT_TERM_PROMPT, 0x00000000);
            gfx_draw_text(x+3, y+18, "_", DT_TERM_FG,     0x00000000);
            break;

        case APPICON_TEXTEDIT: {
            int pw = w-4, ph = h-2, px = x+2, py = y;
            gfx_fill_rect(px, py, pw, ph, DT_PAGE_BG);
            gfx_draw_rect(px, py, pw, ph, DT_PAGE_LN);
            int fold = 8;
            for (int fi = 0; fi < fold; fi++)
                gfx_fill_rect(px+pw-fold+fi, py+fi, fold-fi, 1, 0x00FFFFFF);
            gfx_fill_rect(px+pw-fold, py, fold, fold, 0x00AABBCC);
            for (int li = 0; li < 3; li++)
                gfx_fill_rect(px+4, py+8+li*8, (li==2)?pw/2:pw-8, 2, DT_PAGE_LN);
            break;
        }

        case APPICON_FILEMAN: {
            int bx = x+2, by = y+6, bw = w-4, bh = h-8;
            gfx_fill_rect(bx, by,   bw/2, 6,    DT_FOLD_TOP);
            gfx_fill_rect(bx, by+6, bw,   bh-6, DT_FOLD_BG);
            gfx_draw_rect(bx, by+6, bw,   bh-6, DT_FOLD_TOP);
            break;
        }

        case APPICON_SETTINGS: {
            int cx = x+w/2, cy = y+h/2, r = h/2-4, t = 4;
            gfx_fill_rect(cx-r/2, cy-r/2, r,   r,   DT_GEAR_COL);
            gfx_fill_rect(cx-t/2, y+1,    t,   r/2, DT_GEAR_COL);
            gfx_fill_rect(cx-t/2, y+h-r/2,t,   r/2, DT_GEAR_COL);
            gfx_fill_rect(x+1,    cy-t/2, r/2, t,   DT_GEAR_COL);
            gfx_fill_rect(x+w-r/2,cy-t/2, r/2, t,   DT_GEAR_COL);
            break;
        }

        default:
        case APPICON_GENERIC:
            gfx_fill_rect(x, y, w, h, 0x00001A2A);
            gfx_draw_rect(x, y, w, h, DT_ICON_BORDER);
            gfx_draw_text(x+w/2-4, y+h/2-FONT_H/2, "?", DT_WHITE, 0x00001A2A);
            break;
    }
}

// ============================================================
// Dessin d'une icône (fond + graphisme + label)
// ============================================================
static void draw_icon(int idx) {
    IconState* st  = &g_icons[idx];
    TexHeader* hdr = st->hdr;
    int x   = st->x - ICON_W/2;
    int y   = st->y - ICON_H/2;
    int hov = st->hovered;

    gfx_fill_rect(x, y, ICON_W, ICON_H, hov ? DT_ICON_HOVER : DT_ICON_BG);
    gfx_draw_rect(x, y, ICON_W, ICON_H, hov ? DT_ACCENT     : DT_ICON_BORDER);
    draw_icon_gfx(x, y, (AppIconType)hdr->icon_type);

    // Label — fond échantillonné depuis le framebuffer
    int ly = y + ICON_H + 3;
    int cx = st->x, cy = ly + FONT_H/2;
    uint32_t sw = vesa_width(), sh = vesa_height();
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
    if ((uint32_t)cx >= sw) cx = (int)sw-1;
    if ((uint32_t)cy >= sh) cy = (int)sh-1;
    gfx_draw_text_centered(x, ly, ICON_W, hdr->name, DT_ICON_TEXT,
                           vesa_get_pixel(cx, cy));
}

static void draw_all_icons(void) {
    for (int i = 0; i < g_icon_count; i++) draw_icon(i);
}

// ============================================================
// Background + taskbar
// ============================================================

// ============================================================
// Fond d'écran — chargé depuis le FS au démarrage, blit nearest-neighbour.
//
// Si "wallpaper.bin" est absent du FS, wallpaper_blit_rect() affiche du noir.
// ============================================================

// Repeint une zone rectangulaire du wallpaper (ou noir si non chargé).
// Délègue entièrement à wallpaper_blit_rect() qui lit le disque secteur
// par secteur — aucun buffer RAM géant, pas de freeze au démarrage.
static void redraw_background_rect(int x, int y, int w, int h) {
    wallpaper_blit_rect(x, y, w, h);
    vesa_invalidate_rect(x, y, w, h);
}

// Repeint tout le fond au démarrage du bureau.
static void draw_background(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    redraw_background_rect(0, 0, (int)sw, (int)sh);
    // NE PAS appeler vesa_invalidate_all() ici : ça forcerait render_vesa()
    // à repeindre toutes les cellules texte (fond noir) par-dessus le wallpaper.
    // On marque tout comme "propre" pour que render_vesa() ne retouche rien.
    vesa_invalidate_none();
}

static void draw_taskbar(void) {
    uint32_t sw = vesa_width(), sh = vesa_height();
    int tbh = 32, tby = (int)sh - tbh;
    gfx_fill_rect(0, tby, (int)sw, tbh, DT_TASKBAR_BG);
    gfx_fill_rect(0, tby, (int)sw, 1,   DT_TASKBAR_LINE);
    gfx_draw_text(10, tby + (tbh-FONT_H)/2,
                  session_get_current_name(), DT_CLOCK_FG, DT_TASKBAR_BG);
    gfx_draw_text_centered(0, tby + (tbh-FONT_H)/2, (int)sw,
                           "TetraOS Desktop", DT_WHITE, DT_TASKBAR_BG);
}

// ============================================================
// Redraw bureau complet
// ============================================================
static void desktop_redraw(void) {
    screen_begin_ui();
    draw_background();
    draw_taskbar();
    for (int i = 0; i < g_icon_count; i++)
        g_icons[i].hovered = icon_hit(i, g_mouse.x, g_mouse.y);
    draw_all_icons();
    screen_end_ui();
    vesa_flip();
}

// ============================================================
// Handler de paquet souris pour la boucle desktop
// ============================================================
// On NE dessine PAS le curseur ici (desktop_run gère lui-même
// erase/redraw en fonction du hover des icônes). On se contente
// de signaler qu'un mouvement/clic a eu lieu.
static volatile int s_desktop_mouse_pending = 0;
static void desktop_on_mouse_packet(void) {
    s_desktop_mouse_pending = 1;
}

// ============================================================
// Lancement d'une app
// ============================================================
static void launch_app(int idx) {
    // Snapshot du contexte session → ProcessContext avant d'entrer dans l'app.
    // Toutes les vérifications ACL/permissions de l'app liront g_current_process
    // plutôt que g_session_manager directement.
    process_begin();
    mouse_erase_cursor();
    g_icons[idx].hdr->entry();
    process_end();
    // Restaurer le handler souris du bureau : l'app quittée (terminal, etc.)
    // a pu enregistrer le sien via input_set_mouse_packet_handler() et ne
    // le désenregistre pas forcément à la sortie.
    input_set_mouse_packet_handler(desktop_on_mouse_packet);
    // Retour ici après fermeture de l'app.
    // Si l'app a déclenché un logout (ex: terminal → session.logout),
    // logged_in=0 : ne pas redessiner le bureau — desktop_run va sortir
    // de sa boucle et appeler screen_exit_ui() proprement.
    if (!g_session_manager.logged_in) return;
    mouse_erase_cursor();
    desktop_redraw();
    mouse_draw_cursor();
}

// ============================================================
// Loop du Desktop
// ============================================================
void desktop_run(void) {
    tex_scan();

    // Pré-charger le wallpaper ICI, avant tout bloc screen_begin_ui/end_ui.
    // Si on le charge dans draw_background(), ata_read_single() peut émettre
    // des print_string() qui déclenchent screen_render() → render_vesa() qui
    // écrase le framebuffer avec le fond texte EN PLEIN MILIEU du blit.
    wallpaper_load();

    // Enregistrer le callback de fond pour appcore.
    // Sans ça, erase_rect() utilise son fallback (fond noir uni)
    // et laisse des zones noires quand on déplace une fenêtre.
    app_set_bg_callback(redraw_background_rect);

    g_prev_left = 0;
    desktop_redraw();
    mouse_draw_cursor();

    // IMPORTANT : on route tout par input_poll_char() (dispatcher centralisé
    // de input.c), jamais par mouse_poll() en direct. mouse_poll() lit
    // aveuglément le premier octet disponible sur le port 0x60 — s'il
    // s'agit en fait d'un scancode clavier, ça désynchronise le paquet
    // souris 3-octets et fait dérailler le curseur (bug historique).
    input_set_mouse_packet_handler(desktop_on_mouse_packet);

    while (g_session_manager.logged_in) {

        int mouse_moved = 0;
        s_desktop_mouse_pending = 0;
        while (input_has_pending_byte()) {
            input_poll_char(); // route vers clavier ou mouse_poll() selon le status byte
        }
        if (s_desktop_mouse_pending) mouse_moved = 1;

        int cur_left  = g_mouse.btn_left;
        int prev_left = g_prev_left;

        if (mouse_moved || cur_left != prev_left) {

            // Détecter quelles icônes changent d'état hover
            int any_changed = 0;
            for (int i = 0; i < g_icon_count; i++) {
                int hov = icon_hit(i, g_mouse.x, g_mouse.y);
                if (hov != g_icons[i].hovered) {
                    any_changed = 1;
                    break;
                }
            }

            // On efface le curseur une seule fois pour les deux cas
            mouse_erase_cursor();

            if (any_changed) {
                // Redessiner UNIQUEMENT les icônes dont le hover a changé.
                // On ne touche pas au fond global ni à la taskbar — aucun flash.
                for (int i = 0; i < g_icon_count; i++) {
                    int hov = icon_hit(i, g_mouse.x, g_mouse.y);
                    if (hov != g_icons[i].hovered) {
                        g_icons[i].hovered = hov;
                        // Zone de l'icône + son label (marge basse ICON_LABEL_H + 6)
                        int ix = g_icons[i].x - ICON_W/2;
                        int iy = g_icons[i].y - ICON_H/2;
                        int iw = ICON_W;
                        int ih = ICON_H + ICON_LABEL_H + 6;
                        // Repeindre le fond uniquement sur cette zone
                        redraw_background_rect(ix, iy, iw, ih);
                        // Redessiner l'icône avec son nouvel état hover
                        draw_icon(i);
                    }
                }
            }

            // Redessiner le curseur à sa nouvelle position (qu'il ait bougé ou non)
            mouse_draw_cursor();

            if (cur_left && !prev_left) {
                for (int i = 0; i < g_icon_count; i++) {
                    if (icon_hit(i, g_mouse.x, g_mouse.y)) {
                        launch_app(i);
                        if (!g_session_manager.logged_in) goto desktop_exit;
                        break;
                    }
                }
            }
        }

        g_prev_left = cur_left;
    }

desktop_exit:
    // Retour au handler par défaut (efface/redessine le curseur) pour
    // le prochain écran de login — sinon il resterait branché sur
    // desktop_on_mouse_packet() qui ne dessine plus rien.
    input_set_mouse_packet_handler(NULL);
    // Fermer toutes les fenêtres appcore avant de rendre la main à main.c.
    // Sans ça, les widgets de l'ancienne session restent alloués et peuvent
    // déclencher des redraws parasites au premier app_tick() de la session suivante.
    app_reset();
    // Un seul screen_exit_ui() ici — logout_app_run() ne l'appelle plus lui-même.
    screen_exit_ui();
}