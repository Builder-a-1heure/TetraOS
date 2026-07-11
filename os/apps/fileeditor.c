// apps/fileeditor.c — Éditeur de fichiers dynamique TetraOS v1
//
// Modes d'édition selon l'extension :
//   MODE_TEXT  : éditeur texte enrichi (word-like) — .txt .md .log et fallback
//   MODE_CODE  : éditeur code avec coloration syntaxique — .tex .c .h .s .asm
//   MODE_GRID  : tableur simplifié — .csv .xml .tsv
//   MODE_HEX   : dump hexadécimal + ASCII éditable — .bin .img .raw .o
//
// Architecture AppCore :
//   - Fenêtre principale avec DrawArea (zone centrale) + barre status
//   - Onglets en haut (mode actif affiché)
//   - Toolbar contextuelle selon le mode
//   - Clic souris → action selon le mode
//   - Clavier → édition selon le mode

#include "app.h"
#include "fileeditor.h"
#include "../lib/appcore.h"
#include "../lib/utils.h"
#include "../lib/process.h"
#include "../lib/errwin.h"
#include "../fs/fs.h"
#include "../ui/session.h"
#include "../gfx/screen.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include <stdint.h>

// Signature bureau
TEX_APP("Editeur", APPICON_TEXTEDIT, 1, 0,
        APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, app_fileeditor);

// ============================================================
// ── CONSTANTES ───────────────────────────────────────────────
// ============================================================
#define FE_WIN_X        20
#define FE_WIN_Y        15
#define FE_WIN_W        800
#define FE_WIN_H        560

#define FE_TOOLBAR_H    30
#define FE_STATUS_H     20
#define FE_FONT_W       8
#define FE_FONT_H       16
#define FE_LINENUM_W    48
#define FE_PAD          4

#define FE_MAX_CONTENT  16384
#define FE_MAX_LINES    1024
#define FE_FILENAME_MAX 63

// ── Couleurs communes ─────────────────────────────────────────
#define FE_BG           0x00080D14
#define FE_FG           0x00DDEEFF
#define FE_STATUS_BG    0x00050A10
#define FE_STATUS_FG    0x00778899
#define FE_TOOLBAR_BG   0x000C1520
#define FE_TOOLBAR_BTN  0x00152030
#define FE_TOOLBAR_HOV  0x00203040
#define FE_TOOLBAR_FG   0x0088AACC
#define FE_CURSOR_COL   0x0000CCFF
#define FE_SEL_COL      0x00203860
#define FE_LINENUM_BG   0x00060B12
#define FE_LINENUM_FG   0x00334455
#define FE_LINENUM_ACT  0x0055AAFF
#define FE_MODIFIED_COL 0x00FFAA00
#define FE_SAVED_COL    0x0033AA44

// ── Couleurs mode CODE (coloration syntaxique) ────────────────
#define SYN_KEYWORD     0x0066AAFF   // mots-clés
#define SYN_STRING      0x0099DD77   // chaînes de caractères
#define SYN_COMMENT     0x00556677   // commentaires
#define SYN_NUMBER      0x00FFCC66   // nombres
#define SYN_OPERATOR    0x00CC88FF   // opérateurs
#define SYN_INCLUDE     0x00FF8855   // directives include/@
#define SYN_NORMAL      FE_FG

// ── Couleurs mode GRILLE (tableur) ───────────────────────────
#define GRID_HEADER_BG  0x000E1A28
#define GRID_HEADER_FG  0x0066AADD
#define GRID_CELL_BG    0x00080D14
#define GRID_CELL_ALT   0x0009101A
#define GRID_CELL_SEL   0x00152540
#define GRID_BORDER     0x00162030
#define GRID_FG         FE_FG

// ── Couleurs mode HEX ─────────────────────────────────────────
#define HEX_ADDR_COL    0x00446688
#define HEX_BYTE_COL    0x0088CCEE
#define HEX_ASCII_COL   0x0077AA88
#define HEX_SEL_COL     FE_SEL_COL
#define HEX_ZERO_COL    0x00334455
#define HEX_NONPRINT    0x00445566

// ============================================================
// ── MODE D'ÉDITION ────────────────────────────────────────────
// ============================================================
typedef enum {
    MODE_TEXT = 0,
    MODE_CODE,
    MODE_GRID,
    MODE_HEX
} EditMode;

// ============================================================
// ── HELPERS CHAÎNES ──────────────────────────────────────────
// ============================================================
static int fe_strlen(const char* s) { int n=0; while(s[n]) n++; return n; }
static void fe_strcpy(char* d, const char* s, int max) {
    int i=0; while(i<max-1&&s[i]) { d[i]=s[i]; i++; } d[i]='\0';
}
static void fe_strcat(char* d, const char* s, int max) {
    int n=fe_strlen(d), i=0;
    while(n+i<max-1&&s[i]) { d[n+i]=s[i]; i++; } d[n+i]='\0';
}
static int fe_strcmp(const char* a, const char* b) {
    while(*a&&*b&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
static int fe_strncmp(const char* a, const char* b, int n) {
    for(int i=0;i<n;i++){if(a[i]!=b[i])return (unsigned char)a[i]-(unsigned char)b[i];if(!a[i])return 0;}return 0;
}
static void fe_itoa(int v, char* buf, int sz) {
    if(sz<=0)return; if(v==0){buf[0]='0';buf[1]='\0';return;}
    char t[16]; int i=0,neg=0; if(v<0){neg=1;v=-v;}
    while(v&&i<15){t[i++]='0'+(v%10);v/=10;} if(neg)t[i++]='-';
    int j=0; while(i>0&&j<sz-1)buf[j++]=t[--i]; buf[j]='\0';
}
static const char* fe_get_ext(const char* name) {
    const char* dot=0; for(const char* p=name;*p;p++) if(*p=='.') dot=p;
    return dot?dot+1:"";
}
static int fe_isdigit(char c) { return c>='0'&&c<='9'; }
static int fe_isalpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int fe_isspace(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; }

// ============================================================
// ── DÉTECTION DU MODE ─────────────────────────────────────────
// ============================================================
static EditMode fe_detect_mode(const char* filename) {
    const char* ext = fe_get_ext(filename);
    // Mode HEX
    if (fe_strcmp(ext,"bin")==0||fe_strcmp(ext,"img")==0||
        fe_strcmp(ext,"raw")==0||fe_strcmp(ext,"o")==0||
        fe_strcmp(ext,"exe")==0) return MODE_HEX;
    // Mode GRILLE
    if (fe_strcmp(ext,"csv")==0||fe_strcmp(ext,"tsv")==0||
        fe_strcmp(ext,"xml")==0) return MODE_GRID;
    // Mode CODE
    if (fe_strcmp(ext,"tex")==0||fe_strcmp(ext,"c")==0||
        fe_strcmp(ext,"h")==0||fe_strcmp(ext,"s")==0||
        fe_strcmp(ext,"asm")==0||fe_strcmp(ext,"md")==0||
        fe_strcmp(ext,"json")==0) return MODE_CODE;
    // Fallback : texte
    return MODE_TEXT;
}

// ============================================================
// ── ÉTAT GLOBAL DE L'ÉDITEUR ──────────────────────────────────
// ============================================================
typedef struct {
    // Fichier
    char      filename[FE_FILENAME_MAX+1];
    uint8_t   raw[FE_MAX_CONTENT];   // contenu brut (tous modes)
    char      text[FE_MAX_CONTENT];  // contenu texte (modes TEXT/CODE)
    int       len;
    int       modified;
    EditMode  mode;

    // Curseur texte (MODE_TEXT / MODE_CODE)
    int       cursor;       // offset dans text[]
    int       cur_line;
    int       cur_col;
    int       scroll_line;
    int       vis_rows;
    int       vis_cols;

    // Index de lignes (TEXT/CODE)
    const char* lines[FE_MAX_LINES];
    int         line_lens[FE_MAX_LINES];
    int         line_count;

    // Mode GRILLE
    int       grid_rows;
    int       grid_cols;
    int       grid_sel_row;
    int       grid_sel_col;
    int       grid_scroll_row;
    // Cellules : on stocke dans text[] délimité par \t et \n
    // (pas d'allocation dynamique — limite : FE_MAX_CONTENT)

    // Mode HEX
    int       hex_offset;   // premier octet affiché (scroll)
    int       hex_cursor;   // octet sélectionné
    int       hex_bytes_per_row;  // calculé selon largeur

    // Coordonnées zone cliente
    int       client_x, client_y, client_w, client_h;
    int       draw_x, draw_y, draw_w, draw_h; // zone dessin (sans toolbar/status)

    // Souris
    int       mouse_prev_left;
    int       mouse_drag_y;
    int       mouse_dragging;
} FeState;

static FeState g_fe;
static int g_fe_dirty = 0;
static int g_fe_close_req = 0;

// ============================================================
// ── GESTION DU BUFFER TEXTE ───────────────────────────────────
// ============================================================
static void fe_rebuild_lines(void) {
    g_fe.line_count = 0;
    g_fe.lines[0]   = g_fe.text;
    int lc = 0;
    for (int i = 0; i <= g_fe.len && lc < FE_MAX_LINES-1; i++) {
        if (g_fe.text[i]=='\n' || i==g_fe.len) {
            int start = (int)(g_fe.lines[lc] - g_fe.text);
            g_fe.line_lens[lc] = i - start;
            lc++;
            if (i < g_fe.len) g_fe.lines[lc] = g_fe.text + i + 1;
        }
    }
    g_fe.line_count = lc ? lc : 1;
}

static void fe_update_cursor(void) {
    int line=0, col=0;
    for (int i=0; i<g_fe.cursor; i++) {
        if (g_fe.text[i]=='\n') { line++; col=0; } else col++;
    }
    g_fe.cur_line = line; g_fe.cur_col = col;
}

static int fe_line_start(int line) {
    if (line<=0) return 0;
    if (line>=g_fe.line_count) return g_fe.len;
    return (int)(g_fe.lines[line] - g_fe.text);
}

static void fe_insert(char c) {
    if (g_fe.len>=FE_MAX_CONTENT-1) return;
    for (int i=g_fe.len; i>=g_fe.cursor; i--) g_fe.text[i+1]=g_fe.text[i];
    g_fe.text[g_fe.cursor]=c; g_fe.cursor++; g_fe.len++;
    g_fe.modified=1; fe_rebuild_lines(); fe_update_cursor();
}

static void fe_backspace(void) {
    if (g_fe.cursor<=0) return;
    for (int i=g_fe.cursor-1; i<g_fe.len; i++) g_fe.text[i]=g_fe.text[i+1];
    g_fe.cursor--; g_fe.len--; g_fe.modified=1;
    fe_rebuild_lines(); fe_update_cursor();
}

static void fe_ensure_scroll(void) {
    if (g_fe.cur_line < g_fe.scroll_line)
        g_fe.scroll_line = g_fe.cur_line;
    if (g_fe.cur_line >= g_fe.scroll_line + g_fe.vis_rows)
        g_fe.scroll_line = g_fe.cur_line - g_fe.vis_rows + 1;
    if (g_fe.scroll_line < 0) g_fe.scroll_line = 0;
}

// ============================================================
// ── SAUVEGARDE / CHARGEMENT ───────────────────────────────────
// ============================================================
static int fe_save(void) {
    if (!g_fe.filename[0]) return 0;

    if (!session_has_permission(PERM_FS_WRITE)) {
        errwin_error("Sauvegarde refusee", "Permission insuffisante.\nVotre session n'a pas le droit d'ecriture.");
        return 0;
    }

    int node_idx = fs_find(g_fe.filename);
    if (node_idx >= 0 && !fs_acl_check((uint32_t)node_idx, ACL_WRITE)) {
        errwin_error2("Acces refuse", "Ecriture interdite sur : ", g_fe.filename);
        return 0;
    }

    // Pour les modes texte/code, sauvegarder text[]
    // Pour hex, sauvegarder raw[]
    if (g_fe.mode == MODE_HEX) {
        int r = fs_write_file(g_fe.filename, g_fe.raw, (uint32_t)g_fe.len);
        if (r>=0) { g_fe.modified=0; return 1; }
    } else {
        int r = fs_write_file(g_fe.filename,
                              (const uint8_t*)g_fe.text, (uint32_t)g_fe.len);
        if (r>=0) { g_fe.modified=0; return 1; }
    }
    return 0;
}

static void fe_load(const char* filename) {
    if (!session_has_permission(PERM_FS_READ)) {
        errwin_error("Lecture refusee", "Permission insuffisante.\nVotre session n'a pas le droit de lecture.");
        return;
    }
    int node_idx = fs_find(filename);
    if (node_idx >= 0 && !fs_acl_check((uint32_t)node_idx, ACL_READ)) {
        errwin_error2("Acces refuse", "Lecture interdite sur : ", filename);
        return;
    }

    fe_strcpy(g_fe.filename, filename, FE_FILENAME_MAX+1);
    g_fe.len=0; g_fe.cursor=0; g_fe.scroll_line=0;
    g_fe.modified=0; g_fe.grid_scroll_row=0;
    g_fe.hex_offset=0; g_fe.hex_cursor=0;

    for (int i=0;i<FE_MAX_CONTENT;i++) { g_fe.raw[i]=0; g_fe.text[i]=0; }

    int r = fs_read_file(filename, g_fe.raw, FE_MAX_CONTENT-1);
    if (r>0) {
        g_fe.len = r;
        // Copier dans text[] (pour modes texte/code/grille)
        for (int i=0;i<r;i++) g_fe.text[i]=(char)g_fe.raw[i];
        g_fe.text[r]='\0';
    }
    fe_rebuild_lines();
    fe_update_cursor();
    // Compter lignes/colonnes pour la grille
    if (g_fe.mode==MODE_GRID) {
        g_fe.grid_rows=0; g_fe.grid_cols=0;
        for (int i=0;i<g_fe.len;i++) {
            if (g_fe.text[i]=='\n') g_fe.grid_rows++;
            else if (g_fe.text[i]=='\t'||g_fe.text[i]==',') {
                if (g_fe.grid_rows==0) g_fe.grid_cols++;
            }
        }
        g_fe.grid_rows++; g_fe.grid_cols++;
    }
}

// ============================================================
// ── COLORATION SYNTAXIQUE (MODE CODE) ─────────────────────────
// Analyse un caractère à la position `i` et retourne sa couleur.
// On maintient un état minimal (dans string, dans comment).
// ============================================================

// Mots-clés TEX
static const char* g_kw_tex[] = {
    "include","import","var","const","func","return","end","if","else",
    "while","for","to","step","label","goto","break","continue",
    "true","false","and","or","not","str","math","io","fs","gfx",
    "input","sys","session","time","mem","shell","app",0
};
// Mots-clés C
static const char* g_kw_c[] = {
    "int","char","void","float","double","long","short","unsigned","signed",
    "static","const","extern","struct","typedef","enum","union",
    "if","else","while","for","do","return","break","continue","switch","case",
    "default","sizeof","include","define","ifndef","ifdef","endif","pragma",0
};

static int fe_is_keyword(const char* word, int len, const char** kw_list) {
    for (int i=0; kw_list[i]; i++) {
        int kl=fe_strlen(kw_list[i]);
        if (kl==len && fe_strncmp(word, kw_list[i], len)==0) return 1;
    }
    return 0;
}

// Colorise une ligne complète dans la DrawArea
// px,py : position pixel de départ de la ligne
// line_str : pointeur vers le début de la ligne, line_len : longueur
static void fe_draw_code_line(int px, int py, const char* line_str,
                               int line_len, int bg_col, int is_tex) {
    const char** kw = is_tex ? g_kw_tex : g_kw_c;
    int x = px;
    int i = 0;

    // Détecter commentaire de ligne entière
    while (i < line_len) {
        char c = line_str[i];

        // Commentaire // ou ; (TEX)
        if ((c=='/'&&i+1<line_len&&line_str[i+1]=='/')||
            (is_tex&&c==';')) {
            // Dessiner le reste en couleur commentaire
            for (int j=i; j<line_len && j-i<200; j++) {
                char g[2]={line_str[j],'\0'};
                gfx_draw_text(x+(j-i)*FE_FONT_W, py, g, SYN_COMMENT, bg_col);
            }
            return;
        }

        // Chaîne "..."
        if (c=='"') {
            int end=i+1;
            while (end<line_len&&line_str[end]!='"') end++;
            for (int j=i; j<=end&&j<line_len; j++) {
                char g[2]={line_str[j],'\0'};
                gfx_draw_text(x+(j-i)*FE_FONT_W, py, g, SYN_STRING, bg_col);
            }
            int adv=(end<line_len)?(end-i+1):(line_len-i);
            x+=adv*FE_FONT_W; i+=adv; continue;
        }

        // Directive include @ ou #
        if ((c=='@'&&is_tex)||(c=='#'&&!is_tex)) {
            for (int j=i; j<line_len; j++) {
                char g[2]={line_str[j],'\0'};
                gfx_draw_text(x+(j-i)*FE_FONT_W, py, g, SYN_INCLUDE, bg_col);
            }
            return;
        }

        // Nombre
        if (fe_isdigit(c)||(c=='-'&&i+1<line_len&&fe_isdigit(line_str[i+1]))) {
            int end=i; if(c=='-')end++;
            while (end<line_len&&(fe_isdigit(line_str[end])||line_str[end]=='.'))end++;
            for (int j=i; j<end; j++) {
                char g[2]={line_str[j],'\0'};
                gfx_draw_text(x+(j-i)*FE_FONT_W, py, g, SYN_NUMBER, bg_col);
            }
            int adv=end-i; x+=adv*FE_FONT_W; i+=adv; continue;
        }

        // Opérateurs
        if (c=='+'||c=='-'||c=='*'||c=='/'||c=='='||c=='<'||c=='>'||
            c=='!'||c=='&'||c=='|'||c=='%'||c=='^') {
            char g[2]={c,'\0'};
            gfx_draw_text(x, py, g, SYN_OPERATOR, bg_col);
            x+=FE_FONT_W; i++; continue;
        }

        // Mot (potentiellement un mot-clé)
        if (fe_isalpha(c)) {
            int end=i;
            while (end<line_len&&fe_isalpha(line_str[end])) end++;
            int word_len=end-i;
            uint32_t col = fe_is_keyword(line_str+i, word_len, kw)
                           ? SYN_KEYWORD : SYN_NORMAL;
            for (int j=i; j<end; j++) {
                char g[2]={line_str[j],'\0'};
                gfx_draw_text(x+(j-i)*FE_FONT_W, py, g, col, bg_col);
            }
            x+=word_len*FE_FONT_W; i+=word_len; continue;
        }

        // Caractère normal
        char g[2]={c,'\0'};
        gfx_draw_text(x, py, g, SYN_NORMAL, bg_col);
        x+=FE_FONT_W; i++;
    }
}

// ============================================================
// ── RENDU MODE TEXTE / CODE ───────────────────────────────────
// ============================================================
static void fe_draw_text_mode(int x, int y, int w, int h) {
    int is_tex = (g_fe.mode==MODE_CODE &&
                  (fe_strcmp(fe_get_ext(g_fe.filename),"tex")==0));
    int is_code = (g_fe.mode==MODE_CODE);

    // Zone numéros de ligne
    gfx_fill_rect(x, y, FE_LINENUM_W, h, FE_LINENUM_BG);
    gfx_draw_line(x+FE_LINENUM_W-1, y, x+FE_LINENUM_W-1, y+h-1, 0x00112233);

    // Fond zone texte
    gfx_fill_rect(x+FE_LINENUM_W, y, w-FE_LINENUM_W, h, FE_BG);

    g_fe.vis_rows = h / FE_FONT_H;
    g_fe.vis_cols = (w - FE_LINENUM_W - FE_PAD*2) / FE_FONT_W;
    if (g_fe.vis_rows<1) g_fe.vis_rows=1;
    if (g_fe.vis_cols<1) g_fe.vis_cols=1;

    fe_ensure_scroll();

    for (int row=0; row<g_fe.vis_rows; row++) {
        int li = g_fe.scroll_line + row;
        int py = y + row*FE_FONT_H;

        // Numéro de ligne
        if (li < g_fe.line_count) {
            char lbuf[8]; fe_itoa(li+1, lbuf, sizeof(lbuf));
            int ll=fe_strlen(lbuf);
            // Aligner à droite dans 4 chars
            char lpad[6]="     "; lpad[4]='\0';
            for (int k=0;k<ll&&k<4;k++) lpad[3-(ll-1-k)]=lbuf[k];
            uint32_t lf=(li==g_fe.cur_line)?FE_LINENUM_ACT:FE_LINENUM_FG;
            gfx_draw_text(x+2, py, lpad, lf, FE_LINENUM_BG);
        }

        // Surligneur ligne courante
        uint32_t line_bg = (li==g_fe.cur_line) ? 0x00091420 : FE_BG;
        if (li==g_fe.cur_line)
            gfx_fill_rect(x+FE_LINENUM_W, py, w-FE_LINENUM_W, FE_FONT_H, line_bg);

        // Texte
        if (li < g_fe.line_count) {
            const char* ls = g_fe.lines[li];
            int ll2 = g_fe.line_lens[li];
            int tx = x + FE_LINENUM_W + FE_PAD;

            if (is_code) {
                // Coloration syntaxique
                fe_draw_code_line(tx, py, ls, ll2, line_bg, is_tex);
            } else {
                // Texte brut
                for (int col=0; col<g_fe.vis_cols&&col<ll2; col++) {
                    char g[2]={ls[col],'\0'};
                    gfx_draw_text(tx+col*FE_FONT_W, py, g, FE_FG, line_bg);
                }
            }
        }
    }

    // Curseur
    int col_d = g_fe.cur_col < g_fe.vis_cols ? g_fe.cur_col : g_fe.vis_cols-1;
    int screen_row = g_fe.cur_line - g_fe.scroll_line;
    if (screen_row>=0 && screen_row<g_fe.vis_rows) {
        int cpx = x + FE_LINENUM_W + FE_PAD + col_d*FE_FONT_W;
        int cpy = y + screen_row*FE_FONT_H;
        gfx_fill_rect(cpx, cpy, 2, FE_FONT_H, FE_CURSOR_COL);
    }
}

// ============================================================
// ── RENDU MODE GRILLE (CSV/XML) ───────────────────────────────
// Analyse le texte comme un tableau séparé par virgules/tabs/newlines.
// Affiche une grille avec en-têtes colonnes (A,B,C...) et numéros de lignes.
// ============================================================
#define GRID_COL_W      100
#define GRID_ROW_H      FE_FONT_H
#define GRID_HEADER_H   (FE_FONT_H+4)
#define GRID_ROWNUM_W   40

// Extrait la valeur de la cellule (row,col) depuis text[].
// Copie dans buf (max bufsz).
static void grid_get_cell(int row, int col, char* buf, int bufsz) {
    buf[0]='\0';
    // Parcourir le texte en comptant lignes et colonnes
    int cur_row=0, cur_col=0, bi=0;
    int in_target = 0;
    for (int i=0; i<=g_fe.len; i++) {
        char c = (i<g_fe.len) ? g_fe.text[i] : '\n';
        if (cur_row==row && cur_col==col) {
            in_target=1;
        }
        if (c=='\n') {
            if (in_target) { buf[bi]='\0'; return; }
            cur_row++; cur_col=0; in_target=0;
        } else if (c==','||c=='\t') {
            if (in_target) { buf[bi]='\0'; return; }
            cur_col++;
        } else if (in_target && bi<bufsz-1) {
            buf[bi++]=c;
        }
    }
    buf[bi]='\0';
}

// Modifie la valeur d'une cellule (row,col) dans text[].
static void grid_set_cell(int row, int col, const char* val) {
    // Stratégie : reconstruire le texte entier avec la cellule modifiée
    // On scanne pour trouver la position de début et fin de la cellule
    int cur_row=0, cur_col=0;
    int cell_start=-1, cell_end=-1;
    for (int i=0; i<=g_fe.len; i++) {
        char c=(i<g_fe.len)?g_fe.text[i]:'\n';
        if (cur_row==row && cur_col==col && cell_start<0) cell_start=i;
        if (cur_row==row && cur_col==col &&
            (c==','||c=='\t'||c=='\n'||i==g_fe.len)) {
            cell_end=i; break;
        }
        if (c=='\n')  { cur_row++; cur_col=0; }
        else if (c==','||c=='\t') cur_col++;
    }
    if (cell_start<0||cell_end<0) return;

    int vlen=fe_strlen(val);
    int old_len=cell_end-cell_start;
    int delta=vlen-old_len;

    if (g_fe.len+delta>=FE_MAX_CONTENT) return;

    // Décaler le reste du texte
    if (delta>0) {
        for (int i=g_fe.len; i>=cell_end; i--) g_fe.text[i+delta]=g_fe.text[i];
    } else if (delta<0) {
        for (int i=cell_start+vlen; i<=g_fe.len+delta; i++) g_fe.text[i]=g_fe.text[i-delta];
    }
    // Insérer la valeur
    for (int i=0;i<vlen;i++) g_fe.text[cell_start+i]=val[i];
    g_fe.len+=delta; g_fe.modified=1;
}

static void fe_draw_grid_mode(int x, int y, int w, int h) {
    int vis_cols = (w - GRID_ROWNUM_W) / GRID_COL_W;
    int vis_rows = (h - GRID_HEADER_H) / GRID_ROW_H;
    if (vis_cols<1) vis_cols=1;
    if (vis_rows<1) vis_rows=1;

    // Fond
    gfx_fill_rect(x, y, w, h, GRID_CELL_BG);

    // En-tête des colonnes (A,B,C...)
    gfx_fill_rect(x, y, w, GRID_HEADER_H, GRID_HEADER_BG);
    // Coin vide en haut-gauche
    gfx_fill_rect(x, y, GRID_ROWNUM_W, GRID_HEADER_H, GRID_HEADER_BG);
    gfx_draw_line(x+GRID_ROWNUM_W-1, y, x+GRID_ROWNUM_W-1, y+h-1, GRID_BORDER);
    gfx_draw_line(x, y+GRID_HEADER_H-1, x+w-1, y+GRID_HEADER_H-1, GRID_BORDER);

    for (int c=0; c<vis_cols; c++) {
        int cx = x + GRID_ROWNUM_W + c*GRID_COL_W;
        char hdr[4]; hdr[0]='A'+c; hdr[1]='\0';
        if (c>25) { hdr[0]='A'+(c/26-1); hdr[1]='A'+(c%26); hdr[2]='\0'; }
        gfx_draw_text(cx+4, y+2, hdr, GRID_HEADER_FG, GRID_HEADER_BG);
        gfx_draw_line(cx+GRID_COL_W-1, y, cx+GRID_COL_W-1, y+h-1, GRID_BORDER);
    }

    // Lignes
    for (int r=0; r<vis_rows; r++) {
        int ry = y + GRID_HEADER_H + r*GRID_ROW_H;
        int data_row = g_fe.grid_scroll_row + r;

        // Fond alterné
        uint32_t rbg=(r%2==0)?GRID_CELL_BG:GRID_CELL_ALT;
        gfx_fill_rect(x+GRID_ROWNUM_W, ry, w-GRID_ROWNUM_W, GRID_ROW_H, rbg);

        // Numéro de ligne
        char rnum[8]; fe_itoa(data_row+1, rnum, sizeof(rnum));
        gfx_draw_text(x+2, ry, rnum, GRID_HEADER_FG, GRID_HEADER_BG);

        // Cellule sélectionnée
        if (data_row==g_fe.grid_sel_row) {
            int sc_x=x+GRID_ROWNUM_W+g_fe.grid_sel_col*GRID_COL_W;
            gfx_fill_rect(sc_x, ry, GRID_COL_W, GRID_ROW_H, GRID_CELL_SEL);
        }

        // Cellules
        for (int c=0; c<vis_cols; c++) {
            int cx=x+GRID_ROWNUM_W+c*GRID_COL_W;
            char val[24];
            grid_get_cell(data_row, c, val, sizeof(val));
            // Tronquer si trop long
            if (fe_strlen(val)>11) { val[10]='.'; val[11]='.'; val[12]='\0'; }
            uint32_t fg=(data_row==g_fe.grid_sel_row&&c==g_fe.grid_sel_col)
                        ?0x00FFFFFF:GRID_FG;
            gfx_draw_text(cx+3, ry, val, fg, rbg);
        }
        gfx_draw_line(x, ry+GRID_ROW_H-1, x+w-1, ry+GRID_ROW_H-1, GRID_BORDER);
    }
}

// ============================================================
// ── RENDU MODE HEX ────────────────────────────────────────────
// 16 octets par ligne : ADDR | hex×16 | ASCII×16
// Offsets décimaux à gauche, hex au centre, ASCII à droite.
// ============================================================
static const char hex_chars[]="0123456789ABCDEF";

static void fe_draw_hex_mode(int x, int y, int w, int h) {
    // Calculer combien d'octets par ligne selon la largeur
    // Format : "0x00000000  " (12) + "XX "(3×N) + " | " (3) + N + 2
    // On cible 16 octets par ligne par défaut
    int bpr=16;
    g_fe.hex_bytes_per_row=bpr;

    int addr_w = 12*FE_FONT_W;   // "0x00000000  "
    int hex_w  = (bpr*3)*FE_FONT_W; // "XX " × bpr
    int sep_w  = 3*FE_FONT_W;    // " | "
    int asc_w  = bpr*FE_FONT_W;  // ASCII

    int total_w = addr_w + hex_w + sep_w + asc_w;
    int margin  = (total_w < w) ? (w - total_w) / 2 : 0;

    int rows = (h - FE_FONT_H) / FE_FONT_H; // -1 pour l'en-tête
    if (rows<1) rows=1;

    // Fond
    gfx_fill_rect(x, y, w, h, FE_BG);

    // En-tête
    gfx_fill_rect(x, y, w, FE_FONT_H, FE_LINENUM_BG);
    {
        char hdr[64]; hdr[0]='\0';
        fe_strcat(hdr,"Adresse      ", sizeof(hdr));
        fe_strcat(hdr,"00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F", sizeof(hdr));
        fe_strcat(hdr,"  ASCII", sizeof(hdr));
        gfx_draw_text(x+margin, y, hdr, HEX_ADDR_COL, FE_LINENUM_BG);
    }

    for (int row=0; row<rows; row++) {
        int offset = g_fe.hex_offset + row*bpr;
        if (offset >= g_fe.len) break;

        int py = y + FE_FONT_H + row*FE_FONT_H;
        int rx = x + margin;

        // Fond ligne sélectionnée
        int is_sel_row = (g_fe.hex_cursor/bpr == (g_fe.hex_offset/bpr+row));
        if (is_sel_row) gfx_fill_rect(x, py, w, FE_FONT_H, 0x000C1824);

        // Adresse
        char addr[12];
        addr[0]='0'; addr[1]='x';
        for (int k=7;k>=2;k--) { addr[k]=hex_chars[offset&0xF]; offset>>=4; }
        addr[8]=' '; addr[9]=' '; addr[10]='\0';
        gfx_draw_text(rx, py, addr, HEX_ADDR_COL, is_sel_row?0x000C1824:FE_BG);
        rx += addr_w;
        offset = g_fe.hex_offset + row*bpr;

        // Octets hex
        for (int b=0; b<bpr; b++) {
            int abs_off = offset+b;
            int is_cur = (abs_off==g_fe.hex_cursor);

            if (abs_off < g_fe.len) {
                uint8_t byte = g_fe.raw[abs_off];
                char hx[4];
                hx[0]=hex_chars[byte>>4]; hx[1]=hex_chars[byte&0xF]; hx[2]=' '; hx[3]='\0';
                uint32_t col=(byte==0)?HEX_ZERO_COL:HEX_BYTE_COL;
                uint32_t bg=is_cur?HEX_SEL_COL:(is_sel_row?0x000C1824:FE_BG);
                if (is_cur) gfx_fill_rect(rx, py, FE_FONT_W*2, FE_FONT_H, HEX_SEL_COL);
                gfx_draw_text(rx, py, hx, is_cur?0x00FFFFFF:col, bg);
            } else {
                gfx_draw_text(rx, py, "   ", FE_BG, FE_BG);
            }
            rx += FE_FONT_W*3;
            // Espace au milieu
            if (b==7) { gfx_draw_text(rx-FE_FONT_W, py, " ", FE_BG, FE_BG); }
        }

        // Séparateur
        gfx_draw_text(rx, py, " | ", GRID_BORDER, FE_BG);
        rx += sep_w;

        // ASCII
        offset = g_fe.hex_offset + row*bpr;
        for (int b=0; b<bpr && offset+b<g_fe.len; b++) {
            uint8_t byte=g_fe.raw[offset+b];
            char ac[2]; ac[0]=(byte>=32&&byte<127)?(char)byte:'.'; ac[1]='\0';
            uint32_t col=(byte>=32&&byte<127)?HEX_ASCII_COL:HEX_NONPRINT;
            gfx_draw_text(rx+b*FE_FONT_W, py, ac, col, FE_BG);
        }
    }
}

// ============================================================
// ── BARRE D'OUTILS (commune) ──────────────────────────────────
// Dessinée en haut de la zone cliente.
// ============================================================
static void fe_draw_toolbar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, FE_TOOLBAR_H, FE_TOOLBAR_BG);
    gfx_draw_line(x, y+FE_TOOLBAR_H-1, x+w-1, y+FE_TOOLBAR_H-1, 0x00112233);

    // Onglet mode actif
    const char* mode_names[]={"TEXTE","CODE","GRILLE","HEX"};
    const uint32_t mode_cols[]={0x002244AA,0x00224422,0x00442222,0x00332244};
    for (int m=0; m<4; m++) {
        int tx=x+4+m*90;
        uint32_t bg=(m==g_fe.mode)?mode_cols[m]:FE_TOOLBAR_BTN;
        gfx_fill_rect(tx, y+3, 86, FE_TOOLBAR_H-6, bg);
        gfx_draw_text(tx+4, y+7, mode_names[m], FE_TOOLBAR_FG, bg);
    }

    // Hints clavier à droite
    int hx = x + w - 340;
    gfx_draw_text(hx, y+7,
                  "Ctrl+S: Sauv  Ctrl+N: Nouveau  ESC: Quitter",
                  0x00334455, FE_TOOLBAR_BG);
}

// ── Barre de statut ───────────────────────────────────────────
static void fe_draw_status(int x, int y, int w, int is_modified) {
    gfx_fill_rect(x, y, w, FE_STATUS_H, FE_STATUS_BG);
    gfx_draw_line(x, y, x+w-1, y, 0x00112233);

    char info[128]; info[0]='\0';
    const char* fname = g_fe.filename[0] ? g_fe.filename : "(nouveau)";
    fe_strcat(info, " ", sizeof(info));
    fe_strcat(info, fname, sizeof(info));

    if (g_fe.mode==MODE_TEXT||g_fe.mode==MODE_CODE) {
        char pos[32]; pos[0]=' '; pos[1]='|'; pos[2]=' ';
        fe_itoa(g_fe.cur_line+1, pos+3, 20);
        fe_strcat(pos,":", sizeof(pos));
        int pl=fe_strlen(pos);
        fe_itoa(g_fe.cur_col+1, pos+pl, 20);
        fe_strcat(info, pos, sizeof(info));
    } else if (g_fe.mode==MODE_GRID) {
        char pos[32]; pos[0]=' '; pos[1]='|'; pos[2]=' ';
        fe_itoa(g_fe.grid_sel_row+1, pos+3, 20);
        fe_strcat(pos,":", sizeof(pos));
        int pl=fe_strlen(pos);
        char col_name[4]; col_name[0]='A'+g_fe.grid_sel_col; col_name[1]='\0';
        fe_strcpy(pos+pl, col_name, 4);
        fe_strcat(info, pos, sizeof(info));
    } else {
        char pos[32]; pos[0]=' '; pos[1]='|'; pos[2]=' ';
        fe_itoa(g_fe.hex_cursor, pos+3, 20);
        fe_strcat(pos," (0x", sizeof(pos)); int pl=fe_strlen(pos);
        char hxb[12];
        hxb[0]=hex_chars[(g_fe.hex_cursor>>28)&0xF];
        hxb[1]=hex_chars[(g_fe.hex_cursor>>24)&0xF];
        hxb[2]=hex_chars[(g_fe.hex_cursor>>20)&0xF];
        hxb[3]=hex_chars[(g_fe.hex_cursor>>16)&0xF];
        hxb[4]=hex_chars[(g_fe.hex_cursor>>12)&0xF];
        hxb[5]=hex_chars[(g_fe.hex_cursor>>8)&0xF];
        hxb[6]=hex_chars[(g_fe.hex_cursor>>4)&0xF];
        hxb[7]=hex_chars[g_fe.hex_cursor&0xF];
        hxb[8]=')'; hxb[9]='\0';
        fe_strcpy(pos+pl, hxb, 12);
        fe_strcat(info, pos, sizeof(info));
    }

    gfx_draw_text(x+2, y+2, info, FE_STATUS_FG, FE_STATUS_BG);

    // Indicateur modifié
    const char* mod_str = is_modified ? "[modifie]" : "[sauvegarde]";
    uint32_t mc = is_modified ? FE_MODIFIED_COL : FE_SAVED_COL;
    gfx_draw_text(x+w-100, y+2, mod_str, mc, FE_STATUS_BG);
}

// ============================================================
// ── RENDU COMPLET ─────────────────────────────────────────────
// ============================================================
static void fe_draw_all(void) {
    int x=g_fe.client_x, y=g_fe.client_y;
    int w=g_fe.client_w, h=g_fe.client_h;

    // Toolbar
    fe_draw_toolbar(x, y, w);
    // Zone d'édition
    int edit_y = y + FE_TOOLBAR_H;
    int edit_h = h - FE_TOOLBAR_H - FE_STATUS_H;

    switch (g_fe.mode) {
        case MODE_TEXT:
        case MODE_CODE:
            fe_draw_text_mode(x, edit_y, w, edit_h);
            break;
        case MODE_GRID:
            fe_draw_grid_mode(x, edit_y, w, edit_h);
            break;
        case MODE_HEX:
            fe_draw_hex_mode(x, edit_y, w, edit_h);
            break;
    }

    // Status bar
    fe_draw_status(x, y+h-FE_STATUS_H, w, g_fe.modified);
    vesa_invalidate_all();
}

// ============================================================
// ── GESTION CLAVIER ───────────────────────────────────────────
// ============================================================

// Retourne 1=quitter, 2=nouveau, 0=continuer, set do_save
static int fe_handle_key_text(char c, int* do_save) {
    *do_save=0;
    if (c==19) { *do_save=1; return 0; }      // Ctrl+S
    if (c==29) return 2;                        // Ctrl+N
    if (c==27) return 1;                        // ESC
    if (c=='\n'||c=='\r') { fe_insert('\n'); return 0; }
    if (c=='\b')           { fe_backspace();   return 0; }
    // Flèches
    if (c==16) { // haut
        if (g_fe.cur_line>0) {
            int ps=fe_line_start(g_fe.cur_line-1);
            int pl=g_fe.line_lens[g_fe.cur_line-1];
            g_fe.cursor=ps+(g_fe.cur_col<pl?g_fe.cur_col:pl);
            fe_update_cursor();
        }
        return 0;
    }
    if (c==14) { // bas
        if (g_fe.cur_line<g_fe.line_count-1) {
            int ns=fe_line_start(g_fe.cur_line+1);
            int nl=g_fe.line_lens[g_fe.cur_line+1];
            g_fe.cursor=ns+(g_fe.cur_col<nl?g_fe.cur_col:nl);
            fe_update_cursor();
        }
        return 0;
    }
    if (c==17) { if(g_fe.cursor>0){g_fe.cursor--;fe_update_cursor();} return 0; }
    if (c==18) { if(g_fe.cursor<g_fe.len){g_fe.cursor++;fe_update_cursor();} return 0; }
    if ((unsigned char)c>=32&&(unsigned char)c<127) { fe_insert(c); return 0; }
    return 0;
}

static void fe_handle_key_grid(char c, int* do_save) {
    *do_save=0;
    if (c==19) { *do_save=1; return; }
    if (c==16&&g_fe.grid_sel_row>0) { g_fe.grid_sel_row--; g_fe_dirty=1; }
    if (c==14) { g_fe.grid_sel_row++; g_fe_dirty=1; }
    if (c==17&&g_fe.grid_sel_col>0) { g_fe.grid_sel_col--; g_fe_dirty=1; }
    if (c==18) { g_fe.grid_sel_col++; g_fe_dirty=1; }
    // TODO: édition inline de cellule (F2)
}

static void fe_handle_key_hex(char c, int* do_save) {
    *do_save=0;
    if (c==19) { *do_save=1; return; }
    int bpr=g_fe.hex_bytes_per_row;
    if (c==16&&g_fe.hex_cursor>=bpr)   { g_fe.hex_cursor-=bpr; g_fe_dirty=1; }
    if (c==14&&g_fe.hex_cursor+bpr<g_fe.len) { g_fe.hex_cursor+=bpr; g_fe_dirty=1; }
    if (c==17&&g_fe.hex_cursor>0)      { g_fe.hex_cursor--; g_fe_dirty=1; }
    if (c==18&&g_fe.hex_cursor<g_fe.len-1) { g_fe.hex_cursor++; g_fe_dirty=1; }
    // Scroll si curseur sort de la vue
    int cur_row=g_fe.hex_cursor/bpr;
    int vis_rows=(g_fe.draw_h)/FE_FONT_H;
    if (cur_row<g_fe.hex_offset/bpr)
        g_fe.hex_offset=cur_row*bpr;
    if (cur_row>=(g_fe.hex_offset/bpr+vis_rows))
        g_fe.hex_offset=(cur_row-vis_rows+1)*bpr;
}

// ============================================================
// ── GESTION SOURIS ────────────────────────────────────────────
// ============================================================
static void fe_handle_mouse(void) {
    int mx=g_mouse.x, my=g_mouse.y;
    int left=g_mouse.btn_left;
    int left_down=left&&!g_fe.mouse_prev_left;
    int left_up=!left&&g_fe.mouse_prev_left;

    int ex=g_fe.client_x, ey=g_fe.client_y+FE_TOOLBAR_H;
    int eh=g_fe.client_h-FE_TOOLBAR_H-FE_STATUS_H;

    if (g_fe.mode==MODE_TEXT||g_fe.mode==MODE_CODE) {
        int tx=ex+FE_LINENUM_W+FE_PAD;
        int ty=ey;

        // Clic → positionner curseur
        if (left_down && mx>=ex+FE_LINENUM_W && my>=ty && my<ty+eh) {
            int row=(my-ty)/FE_FONT_H;
            int col=(mx-tx)/FE_FONT_W;
            int li=g_fe.scroll_line+row;
            if (li>=g_fe.line_count) li=g_fe.line_count-1;
            int ll=g_fe.line_lens[li];
            if (col>ll) col=ll; if (col<0) col=0;
            g_fe.cursor=fe_line_start(li)+col;
            fe_update_cursor(); g_fe_dirty=1;
            g_fe.mouse_dragging=1; g_fe.mouse_drag_y=my;
        }
        // Drag → scroll
        if (left&&g_fe.mouse_dragging) {
            int dy=my-g_fe.mouse_drag_y;
            if (dy>=FE_FONT_H) {
                int s=dy/FE_FONT_H;
                g_fe.scroll_line-=s;
                if(g_fe.scroll_line<0) g_fe.scroll_line=0;
                g_fe.mouse_drag_y+=s*FE_FONT_H; g_fe_dirty=1;
            } else if (dy<=-FE_FONT_H) {
                int s=(-dy)/FE_FONT_H;
                int mx2=g_fe.line_count-g_fe.vis_rows;
                if(mx2<0)mx2=0;
                g_fe.scroll_line+=s;
                if(g_fe.scroll_line>mx2) g_fe.scroll_line=mx2;
                g_fe.mouse_drag_y-=s*FE_FONT_H; g_fe_dirty=1;
            }
        }
    } else if (g_fe.mode==MODE_GRID) {
        // Clic sur cellule
        if (left_down) {
            int gy=ey+GRID_HEADER_H;
            if (my>=gy && mx>=ex+GRID_ROWNUM_W) {
                int row=(my-gy)/GRID_ROW_H;
                int col=(mx-ex-GRID_ROWNUM_W)/GRID_COL_W;
                g_fe.grid_sel_row=g_fe.grid_scroll_row+row;
                g_fe.grid_sel_col=col;
                g_fe_dirty=1;
            }
        }
    } else if (g_fe.mode==MODE_HEX) {
        // Clic sur octet
        if (left_down&&my>ey+FE_FONT_H) {
            int bpr=g_fe.hex_bytes_per_row;
            int addr_w=12*FE_FONT_W;
            int row=(my-ey-FE_FONT_H)/FE_FONT_H;
            int col=(mx-ex-addr_w)/(FE_FONT_W*3);
            if(col>=0&&col<bpr) {
                g_fe.hex_cursor=g_fe.hex_offset+row*bpr+col;
                if(g_fe.hex_cursor<0) g_fe.hex_cursor=0;
                if(g_fe.hex_cursor>=g_fe.len) g_fe.hex_cursor=g_fe.len-1;
                g_fe_dirty=1;
            }
        }
    }

    if (left_up) g_fe.mouse_dragging=0;
    g_fe.mouse_prev_left=left;
    (void)left_up;
}

// ============================================================
// ── DIALOGUE CONFIRMATION ─────────────────────────────────────
// ============================================================
static int g_fe_dlg_close=0;
static void fe_dlg_close_cb(WinID w __attribute__((unused))) { g_fe_dlg_close=1; }

static int fe_confirm(const char* msg) {
    WinID dlg=app_new_window("Confirmation",230,215,360,110);
    if(dlg==APPCORE_INVALID) return 0;
    g_fe_dlg_close=0;
    app_on_close(dlg,fe_dlg_close_cb);
    app_new_label(dlg,20,18,msg);
    BtnID bo=app_new_button(dlg,200,60,70,28,"Oui");
    BtnID bn=app_new_button(dlg,278,60,70,28,"Non");
    int r=0,done=0;
    while(!done){
        app_tick();
        if(app_button_touched(bo)){r=1;done=1;}
        if(app_button_touched(bn)){r=0;done=1;}
        if(g_fe_dlg_close){r=0;done=1;}
    }
    app_close_window(dlg); return r;
}

static int fe_ask_filename(char* out) {
    WinID dlg=app_new_window("Nom du fichier",195,190,420,140);
    if(dlg==APPCORE_INVALID) return 0;
    g_fe_dlg_close=0;
    app_on_close(dlg,fe_dlg_close_cb);
    app_new_label(dlg,20,16,"Nom du fichier :");
    char buf[FE_FILENAME_MAX+1]; buf[0]='\0'; int bl=0;
    LblID li=app_new_label(dlg,20,40,"");
    app_set_label_color(li,0x00FFFFFF);
    BtnID bo=app_new_button(dlg,274,94,60,30,"OK");
    BtnID ba=app_new_button(dlg,342,94,66,30,"Annuler");
    int r=0,done=0;
    while(!done){
        app_tick(); char c=app_tick_get_key();
        if(c){
            if(c=='\n'||c=='\r'){if(bl>0){r=1;done=1;}}
            else if(c=='\b'){if(bl>0)buf[--bl]='\0';app_set_label_text(li,buf);}
            else if(c==27){r=0;done=1;}
            else if((unsigned char)c>=32&&(unsigned char)c<127&&bl<FE_FILENAME_MAX){
                buf[bl++]=c;buf[bl]='\0';app_set_label_text(li,buf);
            }
        }
        if(app_button_touched(bo)&&bl>0){r=1;done=1;}
        if(app_button_touched(ba)){r=0;done=1;}
        if(g_fe_dlg_close){r=0;done=1;}
    }
    if(r) fe_strcpy(out,buf,FE_FILENAME_MAX+1);
    app_close_window(dlg); return r;
}

// ============================================================
// ── CALLBACK FERMETURE FENÊTRE ────────────────────────────────
// ============================================================
static void fe_main_close_cb(WinID w __attribute__((unused))) {
    g_fe_close_req=1;
}

// ============================================================
// ── MISE À JOUR TITRE ─────────────────────────────────────────
// ============================================================
static void fe_update_title(WinID win) {
    char t[80]; t[0]='\0';
    const char* mn[]={"[Texte]","[Code]","[Grille]","[Hex]"};
    fe_strcat(t,"Editeur ", sizeof(t));
    fe_strcat(t,mn[g_fe.mode],sizeof(t));
    fe_strcat(t," — ",sizeof(t));
    fe_strcat(t,g_fe.filename[0]?g_fe.filename:"(nouveau)",sizeof(t));
    if(g_fe.modified) fe_strcat(t," *",sizeof(t));
    app_set_title(win,t);
}

// ============================================================
// ── BOUCLE PRINCIPALE ─────────────────────────────────────────
// ============================================================
static void fe_editor_loop(void) {
    WinID win=app_new_window("Editeur — TetraOS",
                              FE_WIN_X,FE_WIN_Y,FE_WIN_W,FE_WIN_H);
    if(win==APPCORE_INVALID) return;

    g_fe_close_req=0;
    app_on_close(win,fe_main_close_cb);
    fe_update_title(win);

    // Coordonnées client initiales
    g_fe.client_x=FE_WIN_X;
    g_fe.client_y=FE_WIN_Y+AC_WIN_TITLE_H;
    g_fe.client_w=FE_WIN_W;
    g_fe.client_h=FE_WIN_H;
    g_fe.draw_h  =FE_WIN_H-FE_TOOLBAR_H-FE_STATUS_H;
    g_fe.hex_bytes_per_row=16;

    g_fe_dirty=1;
    app_tick();
    mouse_erase_cursor();
    fe_draw_all();
    mouse_draw_cursor();
    g_fe_dirty=0;

    int running=1;
    while(running) {
        app_tick();
        char c=app_tick_get_key();

        // Mise à jour coordonnées si fenêtre déplacée
        if(app_was_redrawn()) {
            int nx,ny; app_get_win_pos(win,&nx,&ny);
            g_fe.client_x=nx; g_fe.client_y=ny+AC_WIN_TITLE_H;
            g_fe_dirty=1;
        }

        // Souris
        fe_handle_mouse();

        // Fermeture [X]
        if(g_fe_close_req) {
            g_fe_close_req=0;
            if(g_fe.modified) {
                if(!fe_confirm("Quitter sans sauvegarder ?")) {
                    g_fe_dirty=1; continue;
                }
            }
            running=0; break;
        }

        // Clavier
        if(c!=0) {
            int do_save=0; int action=0;
            if(g_fe.mode==MODE_TEXT||g_fe.mode==MODE_CODE) {
                action=fe_handle_key_text(c,&do_save);
            } else if(g_fe.mode==MODE_GRID) {
                fe_handle_key_grid(c,&do_save);
            } else {
                fe_handle_key_hex(c,&do_save);
                if(c==27) action=1;
            }
            g_fe_dirty=1;

            if(do_save) {
                if(!g_fe.filename[0]) {
                    char nn[FE_FILENAME_MAX+1]={0};
                    if(fe_ask_filename(nn)) {
                        fe_strcpy(g_fe.filename,nn,FE_FILENAME_MAX+1);
                        fe_save(); fe_update_title(win);
                    }
                } else { fe_save(); fe_update_title(win); }
                g_fe_dirty=1;
            }

            if(action==1) { // quitter
                if(g_fe.modified) {
                    if(!fe_confirm("Quitter sans sauvegarder ?")) {
                        g_fe_dirty=1; continue;
                    }
                }
                running=0; break;
            }
            if(action==2) { // nouveau
                if(g_fe.modified) {
                    if(!fe_confirm("Abandonner les modifications ?")) {
                        g_fe_dirty=1; continue;
                    }
                }
                for(int i=0;i<FE_MAX_CONTENT;i++){g_fe.raw[i]=0;g_fe.text[i]=0;}
                g_fe.filename[0]='\0'; g_fe.len=0; g_fe.cursor=0;
                g_fe.scroll_line=0; g_fe.modified=0;
                g_fe.mode=MODE_TEXT;
                fe_rebuild_lines(); fe_update_cursor(); fe_update_title(win);
                g_fe_dirty=1;
            }
        }

        // Repaint
        if(g_fe_dirty) {
            mouse_erase_cursor();
            fe_draw_all();
            mouse_draw_cursor();
            g_fe_dirty=0;
        }
    }
    app_close_window(win);
}

// ============================================================
// ── POINTS D'ENTRÉE PUBLICS ───────────────────────────────────
// ============================================================
void app_fileeditor_run(const char* filename) {
    // Reset état
    for(int i=0;i<(int)sizeof(FeState);i++) ((char*)&g_fe)[i]=0;
    g_fe_dirty=0; g_fe_close_req=0;

    g_fe.mode=fe_detect_mode(filename?filename:"");

    if(filename && filename[0]!='\0') {
        if(fs_find(filename)<0)
            fs_write_file(filename,(const uint8_t*)"",0);
        fe_load(filename);
    } else {
        g_fe.mode=MODE_TEXT;
        fe_rebuild_lines();
    }

    app_init();
    fe_editor_loop();
}

void app_fileeditor(void) {
    // Lancé depuis le bureau : demander un fichier à ouvrir ou nouveau
    app_fileeditor_run("");
}
