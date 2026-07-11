// kernel/lib/errwin.c — Fenêtres d'erreur TetraOS

#include "errwin.h"
#include "appcore.h"

// ============================================================
// Constantes internes
// ============================================================
#define EW_WIN_W       420
#define EW_LINE_H       18
#define EW_PAD          16
#define EW_ICON_W       36
#define EW_BTN_W        80
#define EW_BTN_H        26
#define EW_MAX_LINES     6
#define EW_LINE_LEN     80

// Couleurs par niveau
#define EW_COLOR_INFO   0x004488CC   // bleu
#define EW_COLOR_WARN   0x00CC9922   // jaune/orange
#define EW_COLOR_ERROR  0x00CC2222   // rouge

// Label icône par niveau
static const char* ew_icon_str(ErrwinLevel level) {
    if (level == ERRWIN_ERROR) return "[ERR]";
    if (level == ERRWIN_WARN)  return "[!]";
    return "[i]";
}

static uint32_t ew_color(ErrwinLevel level) {
    if (level == ERRWIN_ERROR) return EW_COLOR_ERROR;
    if (level == ERRWIN_WARN)  return EW_COLOR_WARN;
    return EW_COLOR_INFO;
}

// ============================================================
// Helpers chaînes (bare-metal)
// ============================================================
static int ew_strlen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void ew_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void ew_strcat(char* dst, const char* src, int max) {
    int d = ew_strlen(dst), i = 0;
    while (d + i < max - 1 && src && src[i]) { dst[d+i] = src[i]; i++; }
    dst[d+i] = '\0';
}

// ============================================================
// Découpage du message en lignes sur les '\n'
// ============================================================
static int ew_split_lines(const char* msg,
                           char lines[EW_MAX_LINES][EW_LINE_LEN]) {
    int line = 0, col = 0;
    for (int i = 0; msg[i] && line < EW_MAX_LINES; i++) {
        if (msg[i] == '\n') {
            lines[line][col] = '\0';
            line++; col = 0;
        } else {
            if (col < EW_LINE_LEN - 1) {
                lines[line][col++] = msg[i];
            }
        }
    }
    lines[line][col] = '\0';
    if (col > 0 || line == 0) line++;
    return line;  // nombre de lignes
}

// ============================================================
// Implémentation principale
// ============================================================
void errwin_show(ErrwinLevel level, const char* title, const char* msg) {
    if (!msg) msg = "(aucun message)";
    if (!title) title = "Erreur";

    char lines[EW_MAX_LINES][EW_LINE_LEN];
    int nlines = ew_split_lines(msg, lines);

    // Hauteur de la fenêtre selon le nombre de lignes
    int content_h = EW_PAD                         // marge haute
                  + EW_LINE_H                       // ligne icône + titre interne
                  + EW_PAD / 2
                  + nlines * EW_LINE_H              // lignes message
                  + EW_PAD
                  + EW_BTN_H                        // bouton OK
                  + EW_PAD;                         // marge basse

    // Centrer sur l'écran (800×600 VESA standard TetraOS)
    int wx = (800 - EW_WIN_W) / 2;
    int wy = (600 - content_h) / 2 - 26;  // -26 = titlebar AppCore
    if (wx < 20) wx = 20;
    if (wy < 20) wy = 20;

    WinID win = app_new_window(title, wx, wy, EW_WIN_W, content_h);

    int cy = EW_PAD;

    // Icône (label coloré à gauche)
    LblID lbl_icon = app_new_label(win, EW_PAD, cy, ew_icon_str(level));
    app_set_label_color(lbl_icon, ew_color(level));

    cy += EW_LINE_H + EW_PAD / 2;

    // Lignes du message
    for (int i = 0; i < nlines; i++) {
        LblID lbl = app_new_label(win, EW_PAD, cy, lines[i]);
        app_set_label_color(lbl, 0x00DDEEFF);
        cy += EW_LINE_H;
    }

    cy += EW_PAD;

    // Bouton OK centré
    int btn_x = (EW_WIN_W - EW_BTN_W) / 2;
    BtnID btn_ok = app_new_button(win, btn_x, cy, EW_BTN_W, EW_BTN_H, "OK");

    // Boucle modale — bloque jusqu'à OK ou Entrée/Echap
    while (app_running()) {
        app_tick();
        if (app_button_touched(btn_ok)) break;
        char k = app_tick_get_key();
        if (k == '\n' || k == '\r' || k == 27) break;
    }

    app_close_window(win);
}

// ============================================================
// Variante deux chaînes
// ============================================================
void errwin_show2(ErrwinLevel level, const char* title,
                  const char* msg1, const char* msg2) {
    if (!msg1) msg1 = "";
    if (!msg2) msg2 = "";
    char buf[EW_LINE_LEN * EW_MAX_LINES];
    buf[0] = '\0';
    ew_strcat(buf, msg1, sizeof(buf));
    ew_strcat(buf, msg2, sizeof(buf));
    errwin_show(level, title, buf);
}
