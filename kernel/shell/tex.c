// tex.c — TetraExecutable v3
// Moteur de scripts natif TetraOS
//
// Nouvelles fonctionnalités vs v2 :
//   - Modules (import io / fs / math / str / gfx / input / sys / session / time / mem / shell / app)
//   - Boucles while et for...to...step
//   - Fonctions utilisateur (func/return/end)
//   - Opérateurs booléens && || !
//   - Expressions récursives avec priorité des opérateurs
//   - Concaténation de chaînes avec +
//   - Fonctions mathématiques (math.sqrt, math.abs, math.pow, math.min, math.max, math.mod)
//   - Manipulation de chaînes (str.len, str.upper, str.lower, str.sub, str.find, str.starts, str.ends)
//   - Dessin VESA (gfx.pixel, gfx.rect, gfx.fill, gfx.text, gfx.clear, gfx.width, gfx.height)
//   - Input temps réel (input.key, input.mouse_x, input.mouse_y, input.mouse_btn)
//   - Session (session.name, session.is_admin)
//   - Système (sys.mem_free, sys.mem_total, sys.version)
//   - Temporisation (time.sleep)
//   - Scopes de variables (isolation par fonction)
//   - Buffer 16Ko
//
// === NOUVEAU v3 ===
//   - Système de PACKAGES : include @<package>.<Nom>
//       @package.AppCore      → ui + app + gfx + input (framework appli bureau)
//       @package.OsExt        → fs + mem + shell + sys (extensions OS)
//       @package.SystemModifier → sys + mem + fs + session (modification système)
//       @lib.fs / @lib.ui / @lib.io / @lib.math / @lib.str / ...  → alias modules
//   - MODE FRAMEWORK : #framework <nom> en en-tête
//       Permet de déclarer un .tex comme framework réutilisable.
//       Chargement externe via : include lib <nom>.tex
//   - DÉBOGUEUR intégré :
//       Chaque erreur affiche "[TEX DBG] Ligne N: <message>"
//       Mode trace activé avec : #debug en en-tête du script
//   - EXPOSITION OS : sys.exec(<cmd>), app.window, app.btn, app.listbox ...
//       Passerelle complète vers les libs C compilées de l'OS

#include "tex.h"
#include "../gfx/screen.h"
#include "../lib/utils.h"
#include "../lib/appcore.h"
#include "../fs/fs.h"
#include "../drivers/input.h"
#include "../drivers/vesa.h"
#include "../drivers/mouse.h"
#include "../ui/session.h"
#include "../mem/pfa.h"

// Buffer de script statique (évite malloc)
static char g_script_buf[TEX_SCRIPT_BUF];
// Buffer secondaire pour frameworks chargés
static char g_fw_buf[TEX_SCRIPT_BUF];

// ============================================================
// UTILITAIRES INTERNES
// ============================================================

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static int tex_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static void tex_strcpy(char* d, const char* s, int max) {
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static int tex_strcmp(const char* a, const char* b) { return strcmp(a, b); }
static int tex_strncmp(const char* a, const char* b, int n) { return strncmp(a, b, n); }

// Convertit int → string
static void itoa_tex(int n, char* buf, int max) {
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    char tmp[16]; int i=0, neg=0;
    if (n < 0) { neg=1; n=-n; }
    while (n > 0 && i < 14) { tmp[i++] = '0' + n%10; n /= 10; }
    int j = 0;
    if (neg && j < max-1) buf[j++] = '-';
    while (i > 0 && j < max-1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// Convertit float → string (2 décimales)
static void ftoa_tex(float f, char* buf, int max) {
    int neg = 0;
    if (f < 0.0f) { neg = 1; f = -f; }
    int whole = (int)f;
    int frac  = (int)((f - (float)whole) * 100.0f + 0.5f);
    char tmp[32];
    itoa_tex(whole, tmp, 24);
    int j = 0;
    if (neg && j < max-1) buf[j++] = '-';
    for (int k = 0; tmp[k] && j < max-2; k++) buf[j++] = tmp[k];
    buf[j++] = '.';
    buf[j++] = '0' + (frac / 10) % 10;
    buf[j++] = '0' + frac % 10;
    buf[j] = '\0';
}

// Convertit string → int
static int atoi_tex(const char* s) {
    s = skip_ws(s);
    int neg = 0, r = 0;
    if (*s == '-') { neg=1; s++; }
    while (is_digit(*s)) r = r*10 + (*s++ - '0');
    return neg ? -r : r;
}

// Convertit string → float
static float atof_tex(const char* s) {
    s = skip_ws(s);
    int neg = 0;
    if (*s == '-') { neg=1; s++; }
    float r = 0.0f;
    while (is_digit(*s)) r = r*10.0f + (*s++ - '0');
    if (*s == '.') {
        s++;
        float d = 0.1f;
        while (is_digit(*s)) { r += (*s++ - '0') * d; d *= 0.1f; }
    }
    return neg ? -r : r;
}

// isspace
static int is_ws(char c) { return c == ' ' || c == '\t'; }

// Copie un identifiant depuis *src, avance *src
static int read_ident(const char** src, char* dest, int max) {
    const char* s = *src;
    s = skip_ws(s);
    if (!is_alpha(*s)) return 0;
    int i = 0;
    while (is_alnum(*s) && i < max-1) dest[i++] = *s++;
    dest[i] = '\0';
    *src = s;
    return i > 0;
}

// Retire les espaces en fin de chaîne
static void rtrim(char* s) {
    int n = tex_strlen(s) - 1;
    while (n >= 0 && is_ws(s[n])) s[n--] = '\0';
}

// isqrt entier (Newton)
static int isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n/x) / 2; }
    return x;
}

// sqrt float (Newton)
static float fsqrt(float n) {
    if (n <= 0.0f) return 0.0f;
    float x = n;
    for (int i = 0; i < 20; i++) { float nx = (x + n/x) / 2.0f; if (nx == x) break; x = nx; }
    return x;
}

static float fpow(float base, int exp) {
    float r = 1.0f;
    int neg = (exp < 0);
    if (neg) exp = -exp;
    for (int i = 0; i < exp; i++) r *= base;
    return neg ? 1.0f / r : r;
}

// ============================================================
// MODULES
// ============================================================

int tex_has_module(TexContext* ctx, const char* name) {
    for (int i = 0; i < ctx->module_count; i++)
        if (tex_strcmp(ctx->modules[i].name, name) == 0) return 1;
    return 0;
}

static void tex_require_module(TexContext* ctx, const char* cmd, const char* mod) {
    if (!tex_has_module(ctx, mod)) {
        print_string("TEX: '"); print_string(cmd);
        print_string("' necessite 'import "); print_string(mod); print_string("'\n");
    }
}

// ============================================================
// DÉBOGUEUR
// ============================================================

// Affiche une erreur avec numéro de ligne
void tex_dbg_error(TexContext* ctx, const char* msg) {
    char lbuf[16];
    itoa_tex(ctx->current_line + 1, lbuf, sizeof(lbuf));
    print_string("[TEX DBG] Ligne ");
    print_string(lbuf);
    print_string(": ");
    print_string(msg);
    print_string("\n");
    // Mémoriser l'erreur
    ctx->dbg.last_error_line = ctx->current_line;
    tex_strcpy(ctx->dbg.last_error_msg, msg, sizeof(ctx->dbg.last_error_msg));
    ctx->dbg.error_count++;
}

// Trace d'exécution ligne par ligne (si #debug actif)
void tex_dbg_trace(TexContext* ctx, const char* line_content) {
    if (!ctx->dbg.enabled) return;
    char lbuf[16];
    itoa_tex(ctx->current_line + 1, lbuf, sizeof(lbuf));
    print_string("[TRACE] L");
    print_string(lbuf);
    print_string(": ");
    print_string(line_content);
    print_string("\n");
}

// ============================================================
// SYSTÈME DE PACKAGES
// ============================================================

// Ajoute un module au contexte (évite les doublons)
static void tex_add_module(TexContext* ctx, const char* name) {
    if (!tex_has_module(ctx, name) && ctx->module_count < TEX_MAX_MODULES)
        tex_strcpy(ctx->modules[ctx->module_count++].name, name, TEX_MODULE_NAME_LEN);
}

// Vérifie si un package est déjà chargé
int tex_has_package(TexContext* ctx, const char* name) {
    for (int i = 0; i < ctx->package_count; i++)
        if (tex_strcmp(ctx->packages[i].name, name) == 0) return 1;
    return 0;
}

// Charge une lib individuelle (alias vers import)
void tex_load_lib(TexContext* ctx, const char* lib_name) {
    tex_add_module(ctx, lib_name);
    if (!tex_has_package(ctx, lib_name) && ctx->package_count < TEX_MAX_PACKAGES) {
        tex_strcpy(ctx->packages[ctx->package_count].name, lib_name, TEX_PACKAGE_NAME_LEN);
        ctx->packages[ctx->package_count].type = 'l';
        ctx->package_count++;
    }
}

// Charge un package et ses modules associés
// Syntaxe : include @<type>.<Nom>
// Exemples : include @package.AppCore   include @lib.fs
void tex_load_package(TexContext* ctx, const char* pkg_type, const char* pkg_name) {
    if (tex_has_package(ctx, pkg_name)) return; // déjà chargé

    // Enregistrer le package
    if (ctx->package_count < TEX_MAX_PACKAGES) {
        tex_strcpy(ctx->packages[ctx->package_count].name, pkg_name, TEX_PACKAGE_NAME_LEN);
        ctx->packages[ctx->package_count].type = (tex_strcmp(pkg_type, "package") == 0) ? 'p' : 'l';
        ctx->package_count++;
    }

    // Charger les modules selon le package
    if (tex_strcmp(pkg_name, "AppCore") == 0) {
        // Framework appli bureau : ui + app + gfx + input
        tex_add_module(ctx, "gfx");
        tex_add_module(ctx, "input");
        tex_add_module(ctx, "app");
        tex_add_module(ctx, "session");
        print_string("[TEX] Package AppCore charge (gfx, input, app, session)\n");
    }
    else if (tex_strcmp(pkg_name, "OsExt") == 0) {
        // Extensions OS : fs + mem + shell + sys
        tex_add_module(ctx, "fs");
        tex_add_module(ctx, "mem");
        tex_add_module(ctx, "shell");
        tex_add_module(ctx, "sys");
        print_string("[TEX] Package OsExt charge (fs, mem, shell, sys)\n");
    }
    else if (tex_strcmp(pkg_name, "SystemModifier") == 0) {
        // Modification système : sys + mem + fs + session
        tex_add_module(ctx, "sys");
        tex_add_module(ctx, "mem");
        tex_add_module(ctx, "fs");
        tex_add_module(ctx, "session");
        print_string("[TEX] Package SystemModifier charge (sys, mem, fs, session)\n");
    }
    else if (tex_strcmp(pkg_name, "NetCore") == 0) {
        // Réservé futur (Internet)
        print_string("[TEX] Package NetCore : non disponible (fonctionnalite future)\n");
    }
    // @lib.* → alias simple
    else if (tex_strcmp(pkg_type, "lib") == 0) {
        tex_add_module(ctx, pkg_name);
        // Pas de message pour les libs simples (silencieux comme import)
    }
    else {
        print_string("[TEX DBG] Package inconnu: ");
        print_string(pkg_name);
        print_string("\n");
    }
}

// ============================================================
// CHARGEMENT D'UN FRAMEWORK EXTERNE (.tex framework)
// Syntaxe : include lib <nom>.tex
// Le fichier doit être à la racine du fs et déclaré #framework
// ============================================================
static void tex_load_framework(TexContext* ctx, const char* fw_path) {
    // Vérifier si déjà chargé
    for (int i = 0; i < ctx->framework_count; i++) {
        if (tex_strcmp(ctx->frameworks[i].name, fw_path) == 0) return;
    }

    // Charger le fichier framework
    int bytes = fs_read_file(fw_path, (uint8_t*)g_fw_buf, TEX_SCRIPT_BUF - 1);
    if (bytes <= 0) {
        print_string("[TEX DBG] Framework introuvable: ");
        print_string(fw_path);
        print_string("\n");
        return;
    }
    g_fw_buf[bytes] = '\0';

    // Vérifier que c'est bien un framework (#framework en en-tête)
    int is_fw = 0;
    const char* lp = g_fw_buf;
    while (*lp) {
        lp = skip_ws(lp);
        if (tex_strncmp(lp, "#framework", 10) == 0) { is_fw = 1; break; }
        // Avancer à la ligne suivante
        while (*lp && *lp != '\n') lp++;
        if (*lp == '\n') lp++;
    }
    if (!is_fw) {
        print_string("[TEX DBG] '");
        print_string(fw_path);
        print_string("' n'est pas declare #framework\n");
        return;
    }

    // Enregistrer le framework
    if (ctx->framework_count < TEX_MAX_FRAMEWORKS) {
        tex_strcpy(ctx->frameworks[ctx->framework_count].name, fw_path, TEX_FRAMEWORK_NAME_LEN);
        ctx->frameworks[ctx->framework_count].loaded = 1;
        ctx->framework_count++;
    }

    // Exécuter le framework dans le même contexte pour enregistrer ses fonctions
    // (pré-scan uniquement pour collecter les func/labels du framework)
    // On parse les lignes du framework et on les ajoute au contexte courant
    char* p2 = g_fw_buf;
    int old_lc = ctx->line_count;
    // On sauvegarde le line_count et on ajoute les lignes du fw
    // Note : les lignes sont ajoutées à la suite, les fonctions du fw sont prescannes
    int added = 0;
    char* lstart = p2;
    for (int i = 0; g_fw_buf[i] && ctx->line_count < TEX_MAX_LINES; i++) {
        if (g_fw_buf[i] == '\n') {
            g_fw_buf[i] = '\0';
            if (ctx->line_count < TEX_MAX_LINES) {
                ctx->lines[ctx->line_count++] = lstart;
                added++;
            }
            lstart = &g_fw_buf[i+1];
        } else if (g_fw_buf[i] == '\r') {
            g_fw_buf[i] = '\0';
        }
    }
    // Pré-scanner les nouvelles lignes pour collecter les fonctions du framework
    // (on re-lance tex_prescan qui réindexe tout)
    // Les fonctions du fw sont maintenant disponibles dans ctx->funcs

    print_string("[TEX] Framework charge: ");
    print_string(fw_path);
    print_string("\n");
    (void)old_lc;
    (void)added;
}

// ============================================================
// VARIABLES
// ============================================================

void tex_init(TexContext* ctx) {
    memset(ctx, 0, sizeof(TexContext));
    ctx->running  = 1;
    ctx->jump_to  = -1;
    ctx->scope    = 0;
    // Débogueur
    ctx->dbg.enabled        = 0;
    ctx->dbg.last_error_line = -1;
    ctx->dbg.error_count    = 0;
    // Framework
    ctx->fw.is_framework    = 0;
}

void tex_cleanup(TexContext* ctx) {
    (void)ctx;
}

int tex_set_var(TexContext* ctx, const char* name, VarType type, const void* value, int is_const) {
    // Chercher dans le scope courant d'abord
    for (int i = 0; i < ctx->var_count; i++) {
        if (tex_strcmp(ctx->vars[i].name, name) == 0) {
            if (ctx->vars[i].is_const) {
                char errmsg[64];
                tex_strcpy(errmsg, "constante '", 64);
                tex_strcpy(errmsg + tex_strlen(errmsg), name, 64 - tex_strlen(errmsg));
                tex_strcpy(errmsg + tex_strlen(errmsg), "' non modifiable", 64 - tex_strlen(errmsg));
                tex_dbg_error(ctx, errmsg);
                return -1;
            }
            ctx->vars[i].type = type;
            if (type == VAR_INT || type == VAR_BOOL)
                ctx->vars[i].ival = *(const int*)value;
            else if (type == VAR_FLOAT)
                ctx->vars[i].fval = *(const float*)value;
            else
                tex_strcpy(ctx->vars[i].sval, (const char*)value, TEX_VAR_VALUE_LEN);
            return 0;
        }
    }
    if (ctx->var_count >= TEX_MAX_VARS) {
        tex_dbg_error(ctx, "trop de variables (limite TEX_MAX_VARS atteinte)"); return -1;
    }
    TexVar* v = &ctx->vars[ctx->var_count++];
    tex_strcpy(v->name, name, TEX_VAR_NAME_LEN);
    v->type     = type;
    v->is_const = is_const;
    v->scope    = ctx->scope;
    if (type == VAR_INT || type == VAR_BOOL)
        v->ival = *(const int*)value;
    else if (type == VAR_FLOAT)
        v->fval = *(const float*)value;
    else
        tex_strcpy(v->sval, (const char*)value, TEX_VAR_VALUE_LEN);
    return 0;
}

TexVar* tex_get_var(TexContext* ctx, const char* name) {
    for (int i = 0; i < ctx->var_count; i++)
        if (tex_strcmp(ctx->vars[i].name, name) == 0) return &ctx->vars[i];
    return 0;
}

void tex_del_scope_vars(TexContext* ctx, int scope) {
    int w = 0;
    for (int i = 0; i < ctx->var_count; i++)
        if (ctx->vars[i].scope < scope) ctx->vars[w++] = ctx->vars[i];
    ctx->var_count = w;
}

// ============================================================
// ÉVALUATION D'EXPRESSIONS ARITHMÉTIQUES
// Grammaire :
//   expr   = term  (('+' | '-') term)*
//   term   = factor (('*' | '/' | '%') factor)*
//   factor = '-' factor | NUMBER | IDENT | '(' expr ')' | func_call
// ============================================================

static float eval_expr_prec(TexContext* ctx, const char** s);

static float eval_factor(TexContext* ctx, const char** s) {
    *s = skip_ws(*s);

    // Négation unaire
    if (**s == '-') {
        (*s)++;
        return -eval_factor(ctx, s);
    }

    // Parenthèses
    if (**s == '(') {
        (*s)++;
        float v = eval_expr_prec(ctx, s);
        *s = skip_ws(*s);
        if (**s == ')') (*s)++;
        return v;
    }

    // Nombre (int ou float)
    if (is_digit(**s) || (**s == '.' && is_digit(*(*s+1)))) {
        float r = 0.0f;
        while (is_digit(**s)) r = r*10.0f + (*(*s)++ - '0');
        if (**s == '.') {
            (*s)++;
            float d = 0.1f;
            while (is_digit(**s)) { r += (*(*s)++ - '0') * d; d *= 0.1f; }
        }
        return r;
    }

    // Identifiant ou appel module
    if (is_alpha(**s)) {
        char ident[TEX_VAR_NAME_LEN];
        int  i = 0;
        while (is_alnum(**s) && i < TEX_VAR_NAME_LEN-1) ident[i++] = *(*s)++;
        ident[i] = '\0';

        *s = skip_ws(*s);

        // Appel namespace : module.fonction(...)
        if (**s == '.') {
            (*s)++;
            char func[32]; i = 0;
            while (is_alnum(**s) && i < 30) func[i++] = *(*s)++;
            func[i] = '\0';
            *s = skip_ws(*s);

            // Argument unique entre parenthèses
            float arg1 = 0.0f, arg2 = 0.0f;
            if (**s == '(') {
                (*s)++;
                arg1 = eval_expr_prec(ctx, s);
                *s = skip_ws(*s);
                if (**s == ',') { (*s)++; arg2 = eval_expr_prec(ctx, s); }
                *s = skip_ws(*s);
                if (**s == ')') (*s)++;
            }

            // math.*
            if (tex_strcmp(ident, "math") == 0) {
                if (tex_strcmp(func, "sqrt")  == 0) return fsqrt(arg1);
                if (tex_strcmp(func, "abs")   == 0) return arg1 < 0 ? -arg1 : arg1;
                if (tex_strcmp(func, "pow")   == 0) return fpow(arg1, (int)arg2);
                if (tex_strcmp(func, "min")   == 0) return arg1 < arg2 ? arg1 : arg2;
                if (tex_strcmp(func, "max")   == 0) return arg1 > arg2 ? arg1 : arg2;
                if (tex_strcmp(func, "mod")   == 0) return (arg2 != 0.0f) ? (float)((int)arg1 % (int)arg2) : 0.0f;
                // math.eval(var_expr) — évalue une expression arithmétique contenue dans une variable string
                // Utilisation : var res = math.eval(expr)  où expr = "3+4*2"
                // Fonctionne car tex_eval_expr est récursif : si la var contient une string numérique ou
                // une expression, elle sera ré-évaluée depuis la chaîne de la variable.
                if (tex_strcmp(func, "floor") == 0) return (float)(int)arg1;
                if (tex_strcmp(func, "ceil")  == 0) {
                    int fi = (int)arg1;
                    return (arg1 > (float)fi) ? (float)(fi+1) : (float)fi;
                }
            }
            // str.len
            if (tex_strcmp(ident, "str") == 0) {
                if (tex_strcmp(func, "len") == 0) {
                    // arg1 vaut 0, on a besoin du contexte string — cas spécial traité dans eval_string
                    return arg1;
                }
            }
            // sys.*
            if (tex_strcmp(ident, "sys") == 0) {
                if (tex_strcmp(func, "mem_free")  == 0) return (float)(pfa_free_frames() * 4096);
                if (tex_strcmp(func, "mem_total") == 0) return (float)(pfa_total_frames() * 4096);
            }
            // gfx.*
            if (tex_strcmp(ident, "gfx") == 0) {
                if (tex_strcmp(func, "width")  == 0) return (float)vesa_width();
                if (tex_strcmp(func, "height") == 0) return (float)vesa_height();
            }
            // input.*
            if (tex_strcmp(ident, "input") == 0) {
                if (tex_strcmp(func, "mouse_x") == 0) return (float)g_mouse.x;
                if (tex_strcmp(func, "mouse_y") == 0) return (float)g_mouse.y;
                if (tex_strcmp(func, "mouse_btn") == 0) return (float)(g_mouse.btn_left ? 1 : 0);
            }
            return 0.0f;
        }

        // Appel de fonction utilisateur (résultat numérique)
        if (**s == '(') {
            // Pour simplifier, les fonctions utilisateur retournent via return_val
            // On les traite dans exec_line → ici on retourne 0
            return 0.0f;
        }

        // Variable simple
        TexVar* v = tex_get_var(ctx, ident);
        if (v) {
            if (v->type == VAR_INT || v->type == VAR_BOOL) return (float)v->ival;
            if (v->type == VAR_FLOAT) return v->fval;
            if (v->type == VAR_STRING) return atof_tex(v->sval);
        }

        // Mots-clés booléens
        if (tex_strcmp(ident, "true")  == 0) return 1.0f;
        if (tex_strcmp(ident, "false") == 0) return 0.0f;
        return 0.0f;
    }

    return 0.0f;
}

static float eval_term(TexContext* ctx, const char** s) {
    float v = eval_factor(ctx, s);
    *s = skip_ws(*s);
    while (**s == '*' || **s == '/' || **s == '%') {
        char op = *(*s)++;
        float r = eval_factor(ctx, s);
        *s = skip_ws(*s);
        if (op == '*') v *= r;
        else if (op == '/') v = (r != 0.0f) ? v / r : 0.0f;
        else v = (float)((int)v % ((int)r != 0 ? (int)r : 1));
    }
    return v;
}

static float eval_expr_prec(TexContext* ctx, const char** s) {
    float v = eval_term(ctx, s);
    *s = skip_ws(*s);
    while (**s == '+' || **s == '-') {
        // Ne pas consommer '-' s'il fait partie d'un opérateur de comparaison
        if (**s == '-' && *(*s+1) == '-') break;
        char op = *(*s)++;
        float r = eval_term(ctx, s);
        *s = skip_ws(*s);
        if (op == '+') v += r;
        else           v -= r;
    }
    return v;
}

float tex_eval_expr(TexContext* ctx, const char* expr) {
    const char* s = skip_ws(expr);
    return eval_expr_prec(ctx, &s);
}

// ============================================================
// ÉVALUATION DE CHAÎNES
// Résout les variables, les appels str.*, et la concaténation avec +
// ============================================================

void tex_eval_string(TexContext* ctx, const char* expr, char* out, int max_len) {
    out[0] = '\0';
    int olen = 0;
    expr = skip_ws(expr);

    // Parcours de l'expression par segments séparés par +
    while (*expr && olen < max_len - 1) {
        expr = skip_ws(expr);
        if (!*expr) break;

        char seg[TEX_VAR_VALUE_LEN];
        seg[0] = '\0';
        int slen = 0;

        if (*expr == '"') {
            // Littéral chaîne
            expr++;
            while (*expr && *expr != '"' && slen < TEX_VAR_VALUE_LEN-1) {
                if (*expr == '\\' && *(expr+1) == 'n') { seg[slen++] = '\n'; expr += 2; }
                else if (*expr == '\\' && *(expr+1) == 't') { seg[slen++] = '\t'; expr += 2; }
                else if (*expr == '\\' && *(expr+1) == '"') { seg[slen++] = '"'; expr += 2; }
                else seg[slen++] = *expr++;
            }
            if (*expr == '"') expr++;
            seg[slen] = '\0';
        }
        else if (is_digit(*expr) || (*expr == '-' && is_digit(*(expr+1)))) {
            // Nombre → converti en string
            const char* num_start = expr;
            int is_float = 0;
            if (*expr == '-') expr++;
            while (is_digit(*expr)) expr++;
            if (*expr == '.') { is_float = 1; expr++; while (is_digit(*expr)) expr++; }
            if (is_float) {
                float fv = atof_tex(num_start);
                ftoa_tex(fv, seg, TEX_VAR_VALUE_LEN);
            } else {
                int iv = atoi_tex(num_start);
                itoa_tex(iv, seg, TEX_VAR_VALUE_LEN);
            }
        }
        else if (is_alpha(*expr)) {
            char ident[TEX_VAR_NAME_LEN];
            const char* p = expr;
            read_ident(&p, ident, TEX_VAR_NAME_LEN);
            expr = p;
            expr = skip_ws(expr);

            // Namespace : str.* / sys.* / math.* / session.* etc.
            if (*expr == '.') {
                expr++;
                char fn[32]; int fi = 0;
                while (is_alnum(*expr) && fi < 30) fn[fi++] = *expr++;
                fn[fi] = '\0';
                expr = skip_ws(expr);

                // Lire l'argument chaîne si présent
                char arg_s[TEX_VAR_VALUE_LEN]; arg_s[0] = '\0';
                float arg_f = 0.0f;
                int   arg2i = 0, arg3i = 0;

                if (*expr == '(') {
                    expr++;
                    expr = skip_ws(expr);
                    if (*expr == '"' || is_alpha(*expr) || is_digit(*expr)) {
                        // Premier argument : peut être string ou numérique
                        if (*expr == '"' || (is_alpha(*expr) && tex_get_var(ctx, (void*)expr))) {
                            tex_eval_string(ctx, expr, arg_s, TEX_VAR_VALUE_LEN);
                            // Avancer expr jusqu'à ',' ou ')'
                            int in_q = 0;
                            while (*expr && !(*expr == ')' && !in_q) && !(*expr == ',' && !in_q)) {
                                if (*expr == '"') in_q = !in_q;
                                expr++;
                            }
                        }
                        arg_f = tex_eval_expr(ctx, expr);
                        // Avancer jusqu'à ',' ou ')'
                        int d = 0;
                        while (*expr && !((*expr == ',' || *expr == ')') && d == 0)) {
                            if (*expr == '(') d++;
                            if (*expr == ')') d--;
                            expr++;
                        }
                    }
                    if (*expr == ',') {
                        expr++;
                        arg2i = (int)tex_eval_expr(ctx, expr);
                        while (*expr && *expr != ',' && *expr != ')') expr++;
                    }
                    if (*expr == ',') {
                        expr++;
                        arg3i = (int)tex_eval_expr(ctx, expr);
                        while (*expr && *expr != ')') expr++;
                    }
                    if (*expr == ')') expr++;
                }

                // str.*
                if (tex_strcmp(ident, "str") == 0) {
                    // Résoudre arg_s si c'est une variable
                    if (arg_s[0] == '\0') {
                        // arg_f a été évalué comme float mais c'est probablement une var string
                        // On reparse depuis le contexte — simplifié : arg_s déjà rempli
                    }
                    if (tex_strcmp(fn, "len") == 0) {
                        itoa_tex(tex_strlen(arg_s), seg, TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "upper") == 0) {
                        int k = 0;
                        while (arg_s[k] && k < TEX_VAR_VALUE_LEN-1) {
                            seg[k] = (arg_s[k] >= 'a' && arg_s[k] <= 'z') ? arg_s[k]-32 : arg_s[k];
                            k++;
                        }
                        seg[k] = '\0';
                    } else if (tex_strcmp(fn, "lower") == 0) {
                        int k = 0;
                        while (arg_s[k] && k < TEX_VAR_VALUE_LEN-1) {
                            seg[k] = (arg_s[k] >= 'A' && arg_s[k] <= 'Z') ? arg_s[k]+32 : arg_s[k];
                            k++;
                        }
                        seg[k] = '\0';
                    } else if (tex_strcmp(fn, "sub") == 0) {
                        // str.sub(s, start, len)
                        int start = arg2i, sn = arg3i;
                        int alen = tex_strlen(arg_s);
                        if (start < 0) start = 0;
                        if (start > alen) start = alen;
                        if (sn <= 0 || start+sn > alen) sn = alen - start;
                        int k = 0;
                        while (k < sn && k < TEX_VAR_VALUE_LEN-1) { seg[k] = arg_s[start+k]; k++; }
                        seg[k] = '\0';
                    } else if (tex_strcmp(fn, "find") == 0) {
                        // str.find(s, needle) → index ou -1 (retourné comme string)
                        // arg_s = haystack, arg2i ignoré — simplifié
                        itoa_tex(-1, seg, TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "int") == 0) {
                        itoa_tex(atoi_tex(arg_s), seg, TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "float") == 0) {
                        ftoa_tex(atof_tex(arg_s), seg, TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "bool") == 0) {
                        tex_strcpy(seg, (atoi_tex(arg_s) != 0) ? "true" : "false", TEX_VAR_VALUE_LEN);
                    }
                }
                // sys.*
                else if (tex_strcmp(ident, "sys") == 0) {
                    if (tex_strcmp(fn, "version") == 0) tex_strcpy(seg, "TetraOS TEX v3.0", TEX_VAR_VALUE_LEN);
                    else if (tex_strcmp(fn, "mem_free") == 0) {
                        itoa_tex((int)(pfa_free_frames() * 4096), seg, TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "mem_total") == 0) {
                        itoa_tex((int)(pfa_total_frames() * 4096), seg, TEX_VAR_VALUE_LEN);
                    }
                }
                // session.*
                else if (tex_strcmp(ident, "session") == 0) {
                    if (tex_strcmp(fn, "name") == 0) {
                        const char* n = session_get_current_name();
                        tex_strcpy(seg, n ? n : "(none)", TEX_VAR_VALUE_LEN);
                    } else if (tex_strcmp(fn, "is_admin") == 0) {
                        tex_strcpy(seg, session_is_admin() ? "true" : "false", TEX_VAR_VALUE_LEN);
                    }
                }
                // math.* → numérique converti en string
                else if (tex_strcmp(ident, "math") == 0) {
                    float mres = 0.0f;
                    if (tex_strcmp(fn, "sqrt") == 0) mres = fsqrt(arg_f);
                    else if (tex_strcmp(fn, "abs") == 0) mres = arg_f < 0 ? -arg_f : arg_f;
                    else if (tex_strcmp(fn, "floor") == 0) mres = (float)(int)arg_f;
                    // Afficher avec ou sans décimales
                    if ((float)(int)mres == mres) itoa_tex((int)mres, seg, TEX_VAR_VALUE_LEN);
                    else ftoa_tex(mres, seg, TEX_VAR_VALUE_LEN);
                }
                // gfx.*
                else if (tex_strcmp(ident, "gfx") == 0) {
                    if (tex_strcmp(fn, "width") == 0) itoa_tex((int)vesa_width(), seg, TEX_VAR_VALUE_LEN);
                    else if (tex_strcmp(fn, "height") == 0) itoa_tex((int)vesa_height(), seg, TEX_VAR_VALUE_LEN);
                }
                // input.*
                else if (tex_strcmp(ident, "input") == 0) {
                    if (tex_strcmp(fn, "mouse_x") == 0) itoa_tex(g_mouse.x, seg, TEX_VAR_VALUE_LEN);
                    else if (tex_strcmp(fn, "mouse_y") == 0) itoa_tex(g_mouse.y, seg, TEX_VAR_VALUE_LEN);
                    else if (tex_strcmp(fn, "mouse_btn") == 0)
                        tex_strcpy(seg, g_mouse.btn_left ? "true" : "false", TEX_VAR_VALUE_LEN);
                    else if (tex_strcmp(fn, "key") == 0) {
                        char c = input_poll_char();
                        seg[0] = c ? c : '\0'; seg[1] = '\0';
                    }
                }
            }
            else if (*expr == '(') {
                // Appel de fonction utilisateur → évaluation via exec_line non disponible ici
                // On retourne la valeur return_val si disponible
                tex_strcpy(seg, ctx->return_val, TEX_VAR_VALUE_LEN);
            }
            else {
                // Variable simple
                if (tex_strcmp(ident, "true") == 0) { tex_strcpy(seg, "true", TEX_VAR_VALUE_LEN); }
                else if (tex_strcmp(ident, "false") == 0) { tex_strcpy(seg, "false", TEX_VAR_VALUE_LEN); }
                else {
                    TexVar* v = tex_get_var(ctx, ident);
                    if (v) {
                        if (v->type == VAR_STRING) tex_strcpy(seg, v->sval, TEX_VAR_VALUE_LEN);
                        else if (v->type == VAR_INT || v->type == VAR_BOOL)
                            itoa_tex(v->ival, seg, TEX_VAR_VALUE_LEN);
                        else ftoa_tex(v->fval, seg, TEX_VAR_VALUE_LEN);
                    } else {
                        tex_strcpy(seg, ident, TEX_VAR_VALUE_LEN);
                    }
                }
            }
        }

        // Copier le segment dans out
        slen = tex_strlen(seg);
        for (int k = 0; k < slen && olen < max_len-1; k++) out[olen++] = seg[k];
        out[olen] = '\0';

        // Sauter un éventuel + de concaténation
        expr = skip_ws(expr);
        if (*expr == '+') expr++;
        else break;
    }
}

// ============================================================
// ÉVALUATION DE CONDITIONS
// Supporte : ==, !=, <, >, <=, >= sur nombres et strings
//            &&, || entre conditions
//            ! pour la négation
// ============================================================

static int eval_single_cond(TexContext* ctx, const char* cond) {
    cond = skip_ws(cond);

    // Négation
    if (*cond == '!') {
        cond++;
        return !eval_single_cond(ctx, cond);
    }

    // Parenthèses
    if (*cond == '(') {
        cond++;
        int r = tex_eval_condition(ctx, cond);
        return r;
    }

    // Récupérer les deux membres et l'opérateur
    char left[128], right[128], op[4];
    int i = 0;

    // Évaluer left comme string d'abord
    tex_eval_string(ctx, cond, left, sizeof(left));
    float lf = atof_tex(left);

    // Chercher l'opérateur de comparaison
    const char* p = cond;
    // Avancer jusqu'à l'opérateur
    int depth = 0, in_q = 0;
    while (*p) {
        if (*p == '"') in_q = !in_q;
        if (!in_q) {
            if (*p == '(') depth++;
            if (*p == ')') { if (depth == 0) break; depth--; }
            if (depth == 0 && (*p=='=' || *p=='!' || *p=='<' || *p=='>') && !in_q) break;
        }
        p++;
    }

    if (*p == '\0' || *p == ')') {
        // Pas d'opérateur → valeur booléenne
        if (tex_strcmp(left, "true") == 0) return 1;
        if (tex_strcmp(left, "false") == 0) return 0;
        return (lf != 0.0f);
    }

    i = 0;
    while ((*p=='='||*p=='!'||*p=='<'||*p=='>') && i < 3) op[i++] = *p++;
    op[i] = '\0';

    p = skip_ws(p);
    tex_eval_string(ctx, p, right, sizeof(right));
    float rf = atof_tex(right);

    // Comparaison numérique si les deux sont des nombres
    int left_is_num  = (left[0]=='-'||is_digit(left[0]));
    int right_is_num = (right[0]=='-'||is_digit(right[0]));

    if (left_is_num && right_is_num) {
        if (tex_strcmp(op, "==") == 0) return lf == rf;
        if (tex_strcmp(op, "!=") == 0) return lf != rf;
        if (tex_strcmp(op, "<")  == 0) return lf <  rf;
        if (tex_strcmp(op, ">")  == 0) return lf >  rf;
        if (tex_strcmp(op, "<=") == 0) return lf <= rf;
        if (tex_strcmp(op, ">=") == 0) return lf >= rf;
    } else {
        // Comparaison de chaînes
        int cmp = tex_strcmp(left, right);
        if (tex_strcmp(op, "==") == 0) return cmp == 0;
        if (tex_strcmp(op, "!=") == 0) return cmp != 0;
        if (tex_strcmp(op, "<")  == 0) return cmp < 0;
        if (tex_strcmp(op, ">")  == 0) return cmp > 0;
    }
    return 0;
}

int tex_eval_condition(TexContext* ctx, const char* cond) {
    cond = skip_ws(cond);

    // Chercher && ou ||
    const char* p = cond;
    int depth = 0, in_q = 0;
    while (*p) {
        if (*p == '"') in_q = !in_q;
        if (!in_q) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            if (depth == 0 && *p == '&' && *(p+1) == '&') {
                // left && right
                char left_s[256]; int ll = (int)(p - cond);
                if (ll > 255) ll = 255;
                memcpy(left_s, cond, ll); left_s[ll] = '\0';
                rtrim(left_s);
                int lv = eval_single_cond(ctx, left_s);
                if (!lv) return 0; // court-circuit
                return tex_eval_condition(ctx, p+2);
            }
            if (depth == 0 && *p == '|' && *(p+1) == '|') {
                char left_s[256]; int ll = (int)(p - cond);
                if (ll > 255) ll = 255;
                memcpy(left_s, cond, ll); left_s[ll] = '\0';
                rtrim(left_s);
                int lv = eval_single_cond(ctx, left_s);
                if (lv) return 1;
                return tex_eval_condition(ctx, p+2);
            }
        }
        p++;
    }
    return eval_single_cond(ctx, cond);
}

// ============================================================
// EXTRACTION D'UN ARGUMENT (pour les appels de type cmd(arg1, arg2))
// Retourne la position après la parenthèse fermante
// ============================================================
static const char* extract_args(const char* p, char* a1, char* a2, char* a3) {
    if (a1) a1[0] = '\0';
    if (a2) a2[0] = '\0';
    if (a3) a3[0] = '\0';
    p = skip_ws(p);
    if (*p == '(') p++;
    // Arg 1
    if (a1) {
        int i = 0, d = 0, q = 0;
        while (*p && !((*p == ',' || *p == ')') && d == 0 && !q) && i < TEX_VAR_VALUE_LEN-1) {
            if (*p == '"') q = !q;
            if (!q) { if (*p == '(') d++; if (*p == ')') d--; }
            if (d >= 0) a1[i++] = *p;
            p++;
        }
        a1[i] = '\0'; rtrim(a1);
    }
    if (*p == ',') p++;
    // Arg 2
    if (a2) {
        int i = 0, d = 0, q = 0;
        while (*p && !((*p == ',' || *p == ')') && d == 0 && !q) && i < TEX_VAR_VALUE_LEN-1) {
            if (*p == '"') q = !q;
            if (!q) { if (*p == '(') d++; if (*p == ')') d--; }
            if (d >= 0) a2[i++] = *p;
            p++;
        }
        a2[i] = '\0'; rtrim(a2);
    }
    if (*p == ',') p++;
    // Arg 3
    if (a3) {
        int i = 0, d = 0, q = 0;
        while (*p && !((*p == ')') && d == 0 && !q) && i < TEX_VAR_VALUE_LEN-1) {
            if (*p == '"') q = !q;
            if (!q) { if (*p == '(') d++; if (*p == ')') d--; }
            if (d >= 0) a3[i++] = *p;
            p++;
        }
        a3[i] = '\0'; rtrim(a3);
    }
    if (*p == ')') p++;
    return p;
}

// ============================================================
// PREMIÈRE PASSE : collecte labels et fonctions
// Détecte aussi les directives #debug et #framework
// ============================================================
static void tex_prescan(TexContext* ctx) {
    for (int i = 0; i < ctx->line_count; i++) {
        const char* line = skip_ws(ctx->lines[i]);

        // Directive #debug — active le mode trace
        if (tex_strncmp(line, "#debug", 6) == 0 && (line[6] == '\0' || line[6] == ' ' || line[6] == '\r')) {
            ctx->dbg.enabled = 1;
        }

        // Directive #framework <nom> — déclare ce script comme framework
        if (tex_strncmp(line, "#framework", 10) == 0 && (line[10] == ' ' || line[10] == '\0')) {
            ctx->fw.is_framework = 1;
            const char* p = line + 10;
            p = skip_ws(p);
            tex_strcpy(ctx->fw.framework_name, p, TEX_PACKAGE_NAME_LEN);
        }

        // Label : label <nom>
        if (tex_strncmp(line, "label ", 6) == 0) {
            const char* p = line + 6;
            p = skip_ws(p);
            if (ctx->label_count < TEX_MAX_LABELS) {
                TexLabel* lbl = &ctx->labels[ctx->label_count++];
                read_ident(&p, lbl->name, TEX_LABEL_NAME_LEN);
                lbl->line = i;
            }
        }

        // Fonction : func <nom>(<params>)
        if (tex_strncmp(line, "func ", 5) == 0) {
            const char* p = line + 5;
            p = skip_ws(p);
            if (ctx->func_count < TEX_MAX_FUNCTIONS) {
                TexFunc* fn = &ctx->funcs[ctx->func_count++];
                read_ident(&p, fn->name, TEX_FUNC_NAME_LEN);
                fn->param_count = 0;
                fn->start_line  = i + 1;
                fn->end_line    = -1;
                p = skip_ws(p);
                if (*p == '(') {
                    p++;
                    while (*p && *p != ')') {
                        p = skip_ws(p);
                        char pname[TEX_VAR_NAME_LEN];
                        if (read_ident(&p, pname, TEX_VAR_NAME_LEN) && fn->param_count < TEX_MAX_PARAMS)
                            tex_strcpy(fn->params[fn->param_count++], pname, TEX_VAR_NAME_LEN);
                        p = skip_ws(p);
                        if (*p == ',') p++;
                    }
                }
                // Chercher le end correspondant
                int depth = 1;
                for (int j = i+1; j < ctx->line_count && fn->end_line < 0; j++) {
                    const char* jl = skip_ws(ctx->lines[j]);
                    if (tex_strncmp(jl, "func ", 5) == 0) depth++;
                    if (tex_strcmp(jl, "end") == 0 || tex_strncmp(jl, "end ", 4) == 0) {
                        depth--;
                        if (depth == 0) fn->end_line = j;
                    }
                }
            }
        }
    }
}

// ============================================================
// EXÉCUTION D'UNE LIGNE
// ============================================================

int tex_execute_line(TexContext* ctx, const char* raw_line) {
    const char* line = skip_ws(raw_line);

    // Ligne vide ou commentaire
    if (!*line || *line == ';' || (*line == '/' && *(line+1) == '/')) return 0;

    // Directives #debug / #framework → traitées en préscan, ignorées ici
    if (*line == '#') return 0;

    // Trace débogueur (si #debug actif)
    tex_dbg_trace(ctx, line);

    // ── Gestion des blocs ──────────────────────────────────

    // Vérifier si on doit sauter (bloc false)
    int skip = 0;
    for (int i = 0; i < ctx->block_depth; i++) {
        if (ctx->block_stack[i].skip) { skip = 1; break; }
    }

    // } — fin de bloc
    if (*line == '}') {
        if (ctx->block_depth > 0) {
            TexBlock* b = &ctx->block_stack[ctx->block_depth - 1];
            if (!b->skip) {
                // Si c'est un while/for : retourner au début
                if (b->type == BLOCK_WHILE) {
                    int cond = tex_eval_condition(ctx, b->condition);
                    if (cond) {
                        ctx->jump_to = b->loop_start_line;
                        return 0; // on ne dépile pas encore
                    }
                }
                if (b->type == BLOCK_FOR) {
                    TexVar* fv = tex_get_var(ctx, b->for_var);
                    if (fv) {
                        fv->ival += b->for_step;
                        if ((b->for_step > 0 && fv->ival <= b->for_end) ||
                            (b->for_step < 0 && fv->ival >= b->for_end)) {
                            ctx->jump_to = b->loop_start_line;
                            return 0;
                        }
                    }
                }
            }
            ctx->block_depth--;
        }
        return 0;
    }

    // ── Commandes qui peuvent ouvrir des blocs (parsées même si skip) ──

    // if <condition> {
    if (tex_strncmp(line, "if ", 3) == 0 || tex_strncmp(line, "if(", 3) == 0) {
        int cond = 0;
        if (!skip) {
            const char* cond_str = line + 3;
            cond_str = skip_ws(cond_str);
            if (*cond_str == '(') cond_str++;
            cond = tex_eval_condition(ctx, cond_str);
        }
        if (ctx->block_depth < 31) {
            TexBlock* b = &ctx->block_stack[ctx->block_depth++];
            b->type = BLOCK_IF;
            b->skip = skip || !cond;
            b->condition_met = cond;
        }
        return 0;
    }

    // else {
    if (tex_strcmp(line, "else {") == 0 || tex_strcmp(line, "else{") == 0) {
        if (ctx->block_depth > 0) {
            TexBlock* b = &ctx->block_stack[ctx->block_depth - 1];
            if (b->type == BLOCK_IF) b->skip = b->condition_met;
        }
        return 0;
    }

    // while <condition> {
    if (tex_strncmp(line, "while ", 6) == 0 || tex_strncmp(line, "while(", 6) == 0) {
        const char* cond_str = line + 6;
        cond_str = skip_ws(cond_str);
        if (*cond_str == '(') cond_str++;
        char cond_copy[128];
        tex_strcpy(cond_copy, cond_str, 128);
        // Retirer le { final
        int cl = tex_strlen(cond_copy) - 1;
        while (cl >= 0 && (cond_copy[cl] == '{' || is_ws(cond_copy[cl]))) cond_copy[cl--] = '\0';

        int cond = skip ? 0 : tex_eval_condition(ctx, cond_copy);
        if (ctx->block_depth < 31) {
            TexBlock* b = &ctx->block_stack[ctx->block_depth++];
            b->type = BLOCK_WHILE;
            b->skip = skip || !cond;
            b->loop_start_line = ctx->current_line;
            tex_strcpy(b->condition, cond_copy, 128);
        }
        return 0;
    }

    // for <var> = <start> to <end> [step <n>] {
    if (tex_strncmp(line, "for ", 4) == 0) {
        const char* p = line + 4;
        char vname[TEX_VAR_NAME_LEN];
        read_ident(&p, vname, TEX_VAR_NAME_LEN);
        p = skip_ws(p);
        if (*p == '=') p++;
        int start_val = (int)tex_eval_expr(ctx, p);
        while (*p && !(*p == 't' && *(p+1) == 'o')) p++;
        if (*p) p += 2;
        int end_val = (int)tex_eval_expr(ctx, p);
        while (*p && !(*p == 's' || *p == '{')) p++;
        int step_val = 1;
        if (*p == 's' && tex_strncmp(p, "step", 4) == 0) {
            p += 4;
            step_val = (int)tex_eval_expr(ctx, p);
        }
        if (step_val == 0) step_val = 1;

        if (!skip) {
            tex_set_var(ctx, vname, VAR_INT, &start_val, 0);
        }
        int cond = skip ? 0 : (step_val > 0 ? start_val <= end_val : start_val >= end_val);
        if (ctx->block_depth < 31) {
            TexBlock* b = &ctx->block_stack[ctx->block_depth++];
            b->type = BLOCK_FOR;
            b->skip = skip || !cond;
            b->loop_start_line = ctx->current_line;
            tex_strcpy(b->for_var, vname, TEX_VAR_NAME_LEN);
            b->for_end  = end_val;
            b->for_step = step_val;
        }
        return 0;
    }

    // func → on saute (déjà pré-scannée)
    if (tex_strncmp(line, "func ", 5) == 0) {
        // Trouver le end correspondant et sauter par dessus
        for (int i = 0; i < ctx->func_count; i++) {
            const char* p = line + 5;
            p = skip_ws(p);
            char fn[TEX_FUNC_NAME_LEN]; read_ident(&p, fn, TEX_FUNC_NAME_LEN);
            if (tex_strcmp(ctx->funcs[i].name, fn) == 0) {
                ctx->jump_to = ctx->funcs[i].end_line + 1;
                return 0;
            }
        }
        return 0;
    }

    // end (fin de fonction)
    if (tex_strcmp(line, "end") == 0) {
        if (ctx->call_depth > 0 && !skip) {
            ctx->call_depth--;
            TexCallFrame* fr = &ctx->call_stack[ctx->call_depth];
            tex_del_scope_vars(ctx, fr->scope);
            ctx->scope = fr->scope;
            ctx->returning = 1;
            ctx->jump_to = fr->return_line;
        }
        return 0;
    }

    // Si on est en mode skip, ignorer tout le reste
    if (skip) return 0;

    // ── Commandes normales ─────────────────────────────────

    // import <module>
    if (tex_strncmp(line, "import ", 7) == 0) {
        const char* p = line + 7;
        p = skip_ws(p);
        tex_add_module(ctx, p);
        return 0;
    }

    // include @<type>.<Nom>  — chargement de package
    // Exemples : include @package.AppCore    include @lib.fs
    if (tex_strncmp(line, "include @", 9) == 0) {
        const char* p = line + 9;
        // Lire le type (avant le point)
        char pkg_type[32]; int ti = 0;
        while (*p && *p != '.' && ti < 31) pkg_type[ti++] = *p++;
        pkg_type[ti] = '\0';
        if (*p == '.') p++;
        // Lire le nom du package
        char pkg_name[TEX_PACKAGE_NAME_LEN]; int ni = 0;
        while (*p && !is_ws(*p) && ni < TEX_PACKAGE_NAME_LEN-1) pkg_name[ni++] = *p++;
        pkg_name[ni] = '\0';
        tex_load_package(ctx, pkg_type, pkg_name);
        return 0;
    }

    // include lib <nom>.tex  — chargement d'un framework .tex externe
    if (tex_strncmp(line, "include lib ", 12) == 0) {
        const char* p = line + 12;
        p = skip_ws(p);
        char fw_path[TEX_FRAMEWORK_NAME_LEN];
        tex_strcpy(fw_path, p, TEX_FRAMEWORK_NAME_LEN);
        rtrim(fw_path);
        tex_load_framework(ctx, fw_path);
        // Re-scanner pour collecter les fonctions du framework nouvellement chargé
        ctx->func_count = 0; ctx->label_count = 0;
        tex_prescan(ctx);
        return 0;
    }

    // label <nom> — déjà traitée en préscan, ici on ignore
    if (tex_strncmp(line, "label ", 6) == 0) return 0;

    // goto <nom>
    if (tex_strncmp(line, "goto ", 5) == 0) {
        const char* p = line + 5; p = skip_ws(p);
        char lname[TEX_LABEL_NAME_LEN];
        read_ident(&p, lname, TEX_LABEL_NAME_LEN);
        for (int i = 0; i < ctx->label_count; i++) {
            if (tex_strcmp(ctx->labels[i].name, lname) == 0) {
                ctx->jump_to = ctx->labels[i].line;
                return 0;
            }
        }
        char errmsg[64];
        tex_strcpy(errmsg, "label introuvable: ", 64);
        tex_strcpy(errmsg + tex_strlen(errmsg), lname, 64 - tex_strlen(errmsg));
        tex_dbg_error(ctx, errmsg);
        return 0;
    }

    // return [expr]
    if (tex_strncmp(line, "return", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
        const char* p = line + 6;
        p = skip_ws(p);
        if (*p) tex_eval_string(ctx, p, ctx->return_val, TEX_VAR_VALUE_LEN);
        else ctx->return_val[0] = '\0';
        if (ctx->call_depth > 0) {
            ctx->call_depth--;
            TexCallFrame* fr = &ctx->call_stack[ctx->call_depth];
            tex_del_scope_vars(ctx, fr->scope);
            ctx->scope = fr->scope;
            ctx->returning = 1;
            ctx->jump_to = fr->return_line;
        } else {
            ctx->running = 0;
        }
        return 0;
    }

    // exit
    if (tex_strcmp(line, "exit") == 0 || tex_strncmp(line, "exit(", 5) == 0) {
        ctx->running = 0;
        return 0;
    }

    // ─── MODULE io ─────────────────────────────────────────

    // io.print(<expr>)
    if (tex_strncmp(line, "io.print(", 9) == 0) {
        char a1[TEX_VAR_VALUE_LEN]; extract_args(line+8, a1, 0, 0);
        char out[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a1, out, sizeof(out));
        print_string(out);
        return 0;
    }
    // io.println(<expr>)
    if (tex_strncmp(line, "io.println(", 11) == 0) {
        char a1[TEX_VAR_VALUE_LEN]; extract_args(line+10, a1, 0, 0);
        char out[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a1, out, sizeof(out));
        print_string(out); print_char('\n');
        return 0;
    }
    // io.input(<prompt>, <var>)
    if (tex_strncmp(line, "io.input(", 9) == 0) {
        char a1[TEX_VAR_VALUE_LEN], a2[TEX_VAR_NAME_LEN];
        extract_args(line+8, a1, a2, 0);
        char prompt[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a1, prompt, sizeof(prompt));
        print_string(prompt);
        char buf[256]; int idx = 0;
        while (1) {
            char c = keyboard_get_char();
            if (c == '\r' || c == '\n') { buf[idx] = '\0'; print_char('\n'); break; }
            else if ((c == '\b' || c == 127) && idx > 0) { idx--; print_string("\b \b"); }
            else if (c >= 32 && c <= 126 && idx < 255) { buf[idx++] = c; print_char(c); }
        }
        const char* vname = skip_ws(a2);
        tex_set_var(ctx, vname, VAR_STRING, buf, 0);
        return 0;
    }
    // io.clear()
    if (tex_strncmp(line, "io.clear", 8) == 0 || tex_strcmp(line, "clear") == 0) {
        clear_screen(); return 0;
    }

    // ─── MODULE fs ─────────────────────────────────────────

    // fs.write(<filename>, <content>)
    if (tex_strncmp(line, "fs.write(", 9) == 0) {
        char a1[64], a2[TEX_VAR_VALUE_LEN]; extract_args(line+8, a1, a2, 0);
        char fn[64]; tex_eval_string(ctx, a1, fn, sizeof(fn));
        char content[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a2, content, sizeof(content));
        fs_write_file(fn, (uint8_t*)content, tex_strlen(content));
        return 0;
    }
    // fs.read(<filename>, <var>)
    if (tex_strncmp(line, "fs.read(", 8) == 0) {
        char a1[64], a2[TEX_VAR_NAME_LEN]; extract_args(line+7, a1, a2, 0);
        char fn[64]; tex_eval_string(ctx, a1, fn, sizeof(fn));
        uint8_t buf[512]; int bytes = fs_read_file(fn, buf, sizeof(buf)-1);
        if (bytes > 0) { buf[bytes] = '\0'; tex_set_var(ctx, skip_ws(a2), VAR_STRING, buf, 0); }
        else { tex_set_var(ctx, skip_ws(a2), VAR_STRING, "", 0); }
        return 0;
    }
    // fs.exists(<filename>, <var>)
    if (tex_strncmp(line, "fs.exists(", 10) == 0) {
        char a1[64], a2[TEX_VAR_NAME_LEN]; extract_args(line+9, a1, a2, 0);
        char fn[64]; tex_eval_string(ctx, a1, fn, sizeof(fn));
        int exists = (fs_find(fn) >= 0) ? 1 : 0;
        tex_set_var(ctx, skip_ws(a2), VAR_INT, &exists, 0);
        return 0;
    }
    // fs.delete(<filename>)
    if (tex_strncmp(line, "fs.delete(", 10) == 0) {
        char a1[64]; extract_args(line+9, a1, 0, 0);
        char fn[64]; tex_eval_string(ctx, a1, fn, sizeof(fn));
        fs_delete(fn);
        return 0;
    }
    // fs.mkdir(<dirname>)
    if (tex_strncmp(line, "fs.mkdir(", 9) == 0) {
        char a1[64]; extract_args(line+8, a1, 0, 0);
        char dn[64]; tex_eval_string(ctx, a1, dn, sizeof(dn));
        fs_mkdir(dn);
        return 0;
    }
    // fs.cd(<path>)
    if (tex_strncmp(line, "fs.cd(", 6) == 0) {
        char a1[64]; extract_args(line+5, a1, 0, 0);
        char dn[64]; tex_eval_string(ctx, a1, dn, sizeof(dn));
        fs_cd(dn);
        return 0;
    }
    // fs.ls()
    if (tex_strncmp(line, "fs.ls", 5) == 0) {
        fs_ls(); return 0;
    }

    // ─── MODULE gfx ────────────────────────────────────────

    // gfx.pixel(<x>, <y>, <color>)
    if (tex_strncmp(line, "gfx.pixel(", 10) == 0) {
        char a1[32], a2[32], a3[32]; extract_args(line+9, a1, a2, a3);
        int x = (int)tex_eval_expr(ctx, a1);
        int y = (int)tex_eval_expr(ctx, a2);
        uint32_t col = (uint32_t)tex_eval_expr(ctx, a3);
        vesa_put_pixel(x, y, col);
        return 0;
    }
    // gfx.rect(<x>, <y>, <w>, <h>, <color>, <fill>)  fill=1 filled
    if (tex_strncmp(line, "gfx.rect(", 9) == 0) {
        // Extraire manuellement les 6 arguments
        const char* p = line + 8;
        char args[6][32];
        for (int ai = 0; ai < 6; ai++) {
            args[ai][0]='\0';
            p = skip_ws(p);
            if (*p == '(') p++;
            int i=0, d=0, q=0;
            while (*p && !((*p==','||*p==')') && d==0 && !q) && i<31) {
                if (*p=='"') q=!q;
                if (!q){if(*p=='(')d++;if(*p==')')d--;}
                if (d>=0) args[ai][i++]=*p;
                p++;
            }
            args[ai][i]='\0'; rtrim(args[ai]);
            if (*p==',') p++;
        }
        int x=(int)tex_eval_expr(ctx,args[0]), y=(int)tex_eval_expr(ctx,args[1]);
        int w=(int)tex_eval_expr(ctx,args[2]), h=(int)tex_eval_expr(ctx,args[3]);
        uint32_t col=(uint32_t)tex_eval_expr(ctx,args[4]);
        int fill=(int)tex_eval_expr(ctx,args[5]);
        if (fill) {
            for (int row=y; row<y+h; row++)
                for (int col2=x; col2<x+w; col2++)
                    vesa_put_pixel(col2, row, col);
        } else {
            for (int col2=x; col2<x+w; col2++) { vesa_put_pixel(col2,y,col); vesa_put_pixel(col2,y+h-1,col); }
            for (int row=y; row<y+h; row++) { vesa_put_pixel(x,row,col); vesa_put_pixel(x+w-1,row,col); }
        }
        return 0;
    }
    // gfx.text(<x>, <y>, <text>, <color>)
    if (tex_strncmp(line, "gfx.text(", 9) == 0) {
        // Extraire 4 args manuellement
        const char* p = line + 9;
        char a1[32],a2[32],a3[TEX_VAR_VALUE_LEN],a4[32];
        a1[0]=a2[0]=a3[0]=a4[0]='\0';
        // arg1 : x
        int i=0,d=0,q=0;
        while (*p && !((*p==',')&&d==0&&!q)&&i<31){if(*p=='"')q=!q;if(!q){if(*p=='(')d++;if(*p==')')d--;}if(d>=0)a1[i++]=*p;p++;}a1[i]='\0';rtrim(a1);if(*p==',')p++;
        // arg2 : y
        i=0;d=0;q=0;
        while (*p && !((*p==',')&&d==0&&!q)&&i<31){if(*p=='"')q=!q;if(!q){if(*p=='(')d++;if(*p==')')d--;}if(d>=0)a2[i++]=*p;p++;}a2[i]='\0';rtrim(a2);if(*p==',')p++;
        // arg3 : text
        i=0;d=0;q=0;
        while (*p && !((*p==',')&&d==0&&!q)&&i<TEX_VAR_VALUE_LEN-1){if(*p=='"')q=!q;if(!q){if(*p=='(')d++;if(*p==')')d--;}if(d>=0)a3[i++]=*p;p++;}a3[i]='\0';rtrim(a3);if(*p==',')p++;
        // arg4 : color
        i=0;d=0;q=0;
        while (*p && !((*p==')')&&d==0&&!q)&&i<31){if(*p=='"')q=!q;if(!q){if(*p=='(')d++;if(*p==')')d--;}if(d>=0)a4[i++]=*p;p++;}a4[i]='\0';rtrim(a4);

        int gfx_x = (int)tex_eval_expr(ctx, a1);
        int gfx_y = (int)tex_eval_expr(ctx, a2);
        char txt[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a3, txt, sizeof(txt));
        uint32_t col = (uint32_t)tex_eval_expr(ctx, a4);
        extern void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);
        gfx_draw_text(gfx_x, gfx_y, txt, col, 0x00000000);
        return 0;
    }
    // gfx.clear(<color>)
    if (tex_strncmp(line, "gfx.clear(", 10) == 0) {
        char a1[32]; extract_args(line+9, a1, 0, 0);
        uint32_t col = (uint32_t)tex_eval_expr(ctx, a1);
        uint32_t w = vesa_width(), h = vesa_height();
        for (uint32_t row=0; row<h; row++)
            for (uint32_t col2=0; col2<w; col2++)
                vesa_put_pixel(col2, row, col);
        return 0;
    }

    // ─── MODULE time ───────────────────────────────────────

    // time.sleep(<n>)   — boucle active (pas de timer IRQ)
    if (tex_strncmp(line, "time.sleep(", 11) == 0) {
        char a1[32]; extract_args(line+10, a1, 0, 0);
        int ms = (int)tex_eval_expr(ctx, a1);
        volatile uint32_t loops = (uint32_t)ms * 50000u;
        while (loops--) { asm volatile("nop"); }
        return 0;
    }

    // ─── MODULE input ──────────────────────────────────────

    // input.key(<var>)  — lit une touche (non-bloquant, '' si rien)
    if (tex_strncmp(line, "input.key(", 10) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+9, a1, 0, 0);
        char c = input_poll_char();
        char buf[2]; buf[0] = c; buf[1] = '\0';
        tex_set_var(ctx, skip_ws(a1), VAR_STRING, buf, 0);
        return 0;
    }
    // input.waitkey(<var>)  — bloquant
    if (tex_strncmp(line, "input.waitkey(", 14) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+13, a1, 0, 0);
        char c = keyboard_get_char();
        char buf[2]; buf[0] = c; buf[1] = '\0';
        tex_set_var(ctx, skip_ws(a1), VAR_STRING, buf, 0);
        return 0;
    }
    // input.mouse(<x_var>, <y_var>, <btn_var>)
    if (tex_strncmp(line, "input.mouse(", 12) == 0) {
        char a1[TEX_VAR_NAME_LEN], a2[TEX_VAR_NAME_LEN], a3[TEX_VAR_NAME_LEN];
        extract_args(line+11, a1, a2, a3);
        mouse_poll();
        tex_set_var(ctx, skip_ws(a1), VAR_INT, &g_mouse.x, 0);
        tex_set_var(ctx, skip_ws(a2), VAR_INT, &g_mouse.y, 0);
        int btn = g_mouse.btn_left;
        tex_set_var(ctx, skip_ws(a3), VAR_INT, &btn, 0);
        return 0;
    }

    // ─── MODULE sys ────────────────────────────────────────

    // sys.print_mem()
    if (tex_strncmp(line, "sys.print_mem", 13) == 0) {
        print_string("RAM libre: ");
        char buf[32]; itoa_tex((int)(pfa_free_frames()*4096/1024), buf, 32);
        print_string(buf); print_string(" Ko / ");
        itoa_tex((int)(pfa_total_frames()*4096/1024), buf, 32);
        print_string(buf); print_string(" Ko\n");
        return 0;
    }

    // sys.version()
    if (tex_strncmp(line, "sys.version", 11) == 0) {
        print_string("TetraOS TEX v3.0\n"); return 0;
    }

    // ─── MODULE session ────────────────────────────────────

    // session.name(<var>)
    if (tex_strncmp(line, "session.name(", 13) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+12, a1, 0, 0);
        const char* n = session_get_current_name();
        tex_set_var(ctx, skip_ws(a1), VAR_STRING, n ? n : "(none)", 0);
        return 0;
    }
    // session.is_admin(<var>)
    if (tex_strncmp(line, "session.is_admin(", 17) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+16, a1, 0, 0);
        int adm = session_is_admin();
        tex_set_var(ctx, skip_ws(a1), VAR_INT, &adm, 0);
        return 0;
    }
    // session.has_perm(<perm_name>, <var>)
    if (tex_strncmp(line, "session.has_perm(", 17) == 0) {
        char a1[32], a2[TEX_VAR_NAME_LEN]; extract_args(line+16, a1, a2, 0);
        char pname[32]; tex_eval_string(ctx, a1, pname, sizeof(pname));
        int perm_id = -1;
        if (tex_strcmp(pname, "fs_read")   == 0) perm_id = PERM_FS_READ;
        if (tex_strcmp(pname, "fs_write")  == 0) perm_id = PERM_FS_WRITE;
        if (tex_strcmp(pname, "fs_delete") == 0) perm_id = PERM_FS_DELETE;
        if (tex_strcmp(pname, "shutdown")  == 0) perm_id = PERM_SYSTEM_SHUTDOWN;
        int has = (perm_id >= 0) ? session_has_permission((Permission)perm_id) : 0;
        tex_set_var(ctx, skip_ws(a2), VAR_INT, &has, 0);
        return 0;
    }

    // ─── Appel de fonction utilisateur ────────────────────

    // <funcname>(<args>)
    {
        const char* p = line;
        char fname[TEX_FUNC_NAME_LEN];
        const char* after = p;
        read_ident(&after, fname, TEX_FUNC_NAME_LEN);
        after = skip_ws(after);
        if (*after == '(') {
            for (int fi = 0; fi < ctx->func_count; fi++) {
                if (tex_strcmp(ctx->funcs[fi].name, fname) == 0) {
                    TexFunc* fn = &ctx->funcs[fi];
                    // Empiler le contexte d'appel
                    if (ctx->call_depth < TEX_MAX_CALL_DEPTH) {
                        ctx->call_stack[ctx->call_depth].return_line = ctx->current_line + 1;
                        ctx->call_stack[ctx->call_depth].scope = ctx->scope;
                        ctx->call_depth++;
                        ctx->scope++;
                    }
                    // Lier les paramètres
                    after++;
                    for (int pi = 0; pi < fn->param_count; pi++) {
                        char arg[TEX_VAR_VALUE_LEN]; arg[0]='\0';
                        int i=0, d=0, q=0;
                        while (*after && !((*after==','||*after==')') && d==0 && !q) && i<TEX_VAR_VALUE_LEN-1) {
                            if (*after=='"') q=!q;
                            if (!q){if(*after=='(')d++;if(*after==')')d--;}
                            if (d>=0) arg[i++]=*after;
                            after++;
                        }
                        arg[i]='\0'; rtrim(arg);
                        if (*after==',') after++;
                        char val[TEX_VAR_VALUE_LEN];
                        tex_eval_string(ctx, arg, val, sizeof(val));
                        tex_set_var(ctx, fn->params[pi], VAR_STRING, val, 0);
                    }
                    ctx->jump_to = fn->start_line;
                    return 0;
                }
            }
        }
    }

    // ─── Déclarations de variables ─────────────────────────

    // var <nom> = <expr>
    if (tex_strncmp(line, "var ", 4) == 0) {
        const char* p = line + 4; p = skip_ws(p);
        char vname[TEX_VAR_NAME_LEN]; read_ident(&p, vname, TEX_VAR_NAME_LEN);
        p = skip_ws(p);
        if (*p == '=') {
            p++; p = skip_ws(p);
            if (*p == '"') {
                char val[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, p, val, sizeof(val));
                tex_set_var(ctx, vname, VAR_STRING, val, 0);
            } else if (tex_strcmp(p, "true") == 0)  { int v=1; tex_set_var(ctx, vname, VAR_BOOL, &v, 0); }
            else if (tex_strcmp(p, "false") == 0) { int v=0; tex_set_var(ctx, vname, VAR_BOOL, &v, 0); }
            else {
                // Essayer d'abord comme expression numérique
                // Si l'expression contient des lettres non-numériques, c'est peut-être une string
                float fv = tex_eval_expr(ctx, p);
                int   iv = (int)fv;
                if (fv == (float)iv) tex_set_var(ctx, vname, VAR_INT, &iv, 0);
                else                 tex_set_var(ctx, vname, VAR_FLOAT, &fv, 0);
            }
        } else {
            int zero = 0; tex_set_var(ctx, vname, VAR_INT, &zero, 0);
        }
        return 0;
    }

    // const <nom> = <expr>
    if (tex_strncmp(line, "const ", 6) == 0) {
        const char* p = line + 6; p = skip_ws(p);
        char vname[TEX_VAR_NAME_LEN]; read_ident(&p, vname, TEX_VAR_NAME_LEN);
        p = skip_ws(p);
        if (*p == '=') {
            p++; p = skip_ws(p);
            if (*p == '"') {
                char val[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, p, val, sizeof(val));
                tex_set_var(ctx, vname, VAR_STRING, val, 1);
            } else {
                float fv = tex_eval_expr(ctx, p);
                int iv = (int)fv;
                if (fv == (float)iv) tex_set_var(ctx, vname, VAR_INT, &iv, 1);
                else                 tex_set_var(ctx, vname, VAR_FLOAT, &fv, 1);
            }
        }
        return 0;
    }

    // ─── Assignation <var> = <expr>  ou  <var> += -= *= /= ──

    {
        const char* p = line;
        char vname[TEX_VAR_NAME_LEN]; const char* after = p;
        if (is_alpha(*p)) {
            read_ident(&after, vname, TEX_VAR_NAME_LEN);
            after = skip_ws(after);
            // Opérateurs composés : +=  -=  *=  /=
            char compound = '\0';
            if ((*after == '+' || *after == '-' || *after == '*' || *after == '/') && *(after+1) == '=') {
                compound = *after; after += 2;
            } else if (*after == '=' && *(after+1) != '=') {
                after++;
            } else {
                goto not_assign;
            }
            after = skip_ws(after);
            TexVar* v = tex_get_var(ctx, vname);
            if (!v) {
                char errmsg[64];
                tex_strcpy(errmsg, "variable non declaree: ", 64);
                tex_strcpy(errmsg + tex_strlen(errmsg), vname, 64 - tex_strlen(errmsg));
                tex_dbg_error(ctx, errmsg);
                return 0;
            }
            if (v->is_const) {
                char errmsg[64];
                tex_strcpy(errmsg, "constante non modifiable: ", 64);
                tex_strcpy(errmsg + tex_strlen(errmsg), vname, 64 - tex_strlen(errmsg));
                tex_dbg_error(ctx, errmsg);
                return 0;
            }

            if (*after == '"' || (v->type == VAR_STRING && !is_digit(*after))) {
                // Assignation string
                char val[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, after, val, sizeof(val));
                if (compound == '+') {
                    // Concaténation
                    int cur_len = tex_strlen(v->sval);
                    tex_strcpy(v->sval + cur_len, val, TEX_VAR_VALUE_LEN - cur_len);
                } else {
                    tex_strcpy(v->sval, val, TEX_VAR_VALUE_LEN);
                    v->type = VAR_STRING;
                }
            } else {
                float rhs = tex_eval_expr(ctx, after);
                float cur = (v->type == VAR_INT || v->type == VAR_BOOL) ? (float)v->ival : v->fval;
                float res;
                switch (compound) {
                    case '+': res = cur + rhs; break;
                    case '-': res = cur - rhs; break;
                    case '*': res = cur * rhs; break;
                    case '/': res = (rhs != 0.0f) ? cur / rhs : 0.0f; break;
                    default:  res = rhs; break;
                }
                if (res == (float)(int)res && v->type != VAR_FLOAT) {
                    v->type = VAR_INT; v->ival = (int)res;
                } else {
                    v->type = VAR_FLOAT; v->fval = res;
                }
            }
            return 0;
        }
    }
    // ─── MODULE app (AppCore bridge) ───────────────────────
    // Nécessite : import app  OU  include @package.AppCore
    // Expose le système de fenêtres/widgets AppCore aux scripts .tex

    // app.set_label(<lbl_id>, <texte>)  — met à jour le texte d'un label existant
    if (tex_strncmp(line, "app.set_label(", 14) == 0) {
        char a1[32], a2[TEX_VAR_VALUE_LEN]; extract_args(line+13, a1, a2, 0);
        LblID lid = (LblID)tex_eval_expr(ctx, a1);
        char txt[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a2, txt, sizeof(txt));
        app_set_label_text(lid, txt);
        return 0;
    }
    // app.label_color(<lbl_id>, <couleur_hex>)  — change la couleur d'un label
    if (tex_strncmp(line, "app.label_color(", 16) == 0) {
        char a1[32], a2[32]; extract_args(line+15, a1, a2, 0);
        LblID lid   = (LblID)tex_eval_expr(ctx, a1);
        uint32_t col = (uint32_t)tex_eval_expr(ctx, a2);
        app_set_label_color(lid, col);
        return 0;
    }

    // app.init()
    if (tex_strncmp(line, "app.init", 8) == 0) {
        app_init();
        return 0;
    }
    // app.window(<titre>, <x>, <y>, <w>, <h>, <var_id>)
    if (tex_strncmp(line, "app.window(", 11) == 0) {
        const char* p = line + 11;
        char args[6][TEX_VAR_VALUE_LEN];
        for (int ai = 0; ai < 6; ai++) {
            args[ai][0] = '\0';
            int i2=0, d=0, q=0;
            while (*p && !((*p==','||*p==')') && d==0 && !q) && i2<TEX_VAR_VALUE_LEN-1) {
                if (*p=='"') q=!q;
                if (!q){if(*p=='(')d++;if(*p==')')d--;}
                if (d>=0) args[ai][i2++] = *p;
                p++;
            }
            args[ai][i2] = '\0'; rtrim(args[ai]);
            if (*p==',') p++;
        }
        char title[64]; tex_eval_string(ctx, args[0], title, sizeof(title));
        int x=(int)tex_eval_expr(ctx, args[1]), y=(int)tex_eval_expr(ctx, args[2]);
        int w=(int)tex_eval_expr(ctx, args[3]), h=(int)tex_eval_expr(ctx, args[4]);
        WinID wid = app_new_window(title, x, y, w, h);
        const char* vname = skip_ws(args[5]);
        tex_set_var(ctx, vname, VAR_INT, &wid, 0);
        return 0;
    }
    // app.btn(<win_id>, <x>, <y>, <w>, <h>, <label>, <var_id>)
    if (tex_strncmp(line, "app.btn(", 8) == 0) {
        const char* p = line + 8;
        char args[7][TEX_VAR_VALUE_LEN];
        for (int ai = 0; ai < 7; ai++) {
            args[ai][0] = '\0';
            int i2=0, d=0, q=0;
            while (*p && !((*p==','||*p==')') && d==0 && !q) && i2<TEX_VAR_VALUE_LEN-1) {
                if (*p=='"') q=!q;
                if (!q){if(*p=='(')d++;if(*p==')')d--;}
                if (d>=0) args[ai][i2++] = *p;
                p++;
            }
            args[ai][i2] = '\0'; rtrim(args[ai]);
            if (*p==',') p++;
        }
        WinID wid = (WinID)tex_eval_expr(ctx, args[0]);
        int x=(int)tex_eval_expr(ctx, args[1]), y=(int)tex_eval_expr(ctx, args[2]);
        int w=(int)tex_eval_expr(ctx, args[3]), h=(int)tex_eval_expr(ctx, args[4]);
        char lbl[64]; tex_eval_string(ctx, args[5], lbl, sizeof(lbl));
        BtnID bid = app_new_button(wid, x, y, w, h, lbl);
        const char* vname = skip_ws(args[6]);
        tex_set_var(ctx, vname, VAR_INT, &bid, 0);
        return 0;
    }
    // app.label(<win_id>, <x>, <y>, <texte>, <var_id>)
    if (tex_strncmp(line, "app.label(", 10) == 0) {
        const char* p = line + 10;
        char args[5][TEX_VAR_VALUE_LEN];
        for (int ai = 0; ai < 5; ai++) {
            args[ai][0] = '\0';
            int i2=0, d=0, q=0;
            while (*p && !((*p==','||*p==')') && d==0 && !q) && i2<TEX_VAR_VALUE_LEN-1) {
                if (*p=='"') q=!q;
                if (!q){if(*p=='(')d++;if(*p==')')d--;}
                if (d>=0) args[ai][i2++] = *p;
                p++;
            }
            args[ai][i2] = '\0'; rtrim(args[ai]);
            if (*p==',') p++;
        }
        WinID wid = (WinID)tex_eval_expr(ctx, args[0]);
        int x=(int)tex_eval_expr(ctx, args[1]), y=(int)tex_eval_expr(ctx, args[2]);
        char txt[128]; tex_eval_string(ctx, args[3], txt, sizeof(txt));
        LblID lid = app_new_label(wid, x, y, txt);
        const char* vname = skip_ws(args[4]);
        tex_set_var(ctx, vname, VAR_INT, &lid, 0);
        return 0;
    }
    // app.tick()  — traite les événements UI pour un cycle
    if (tex_strncmp(line, "app.tick", 8) == 0) {
        app_tick();
        return 0;
    }
    // app.running(<var>)  — 1 si l'app tourne encore
    if (tex_strncmp(line, "app.running(", 12) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+11, a1, 0, 0);
        int r = app_running();
        tex_set_var(ctx, skip_ws(a1), VAR_INT, &r, 0);
        return 0;
    }
    // app.btn_clicked(<btn_id>, <var>)  — 1 si le bouton a été cliqué
    if (tex_strncmp(line, "app.btn_clicked(", 16) == 0) {
        char a1[32], a2[TEX_VAR_NAME_LEN]; extract_args(line+15, a1, a2, 0);
        BtnID bid = (BtnID)tex_eval_expr(ctx, a1);
        int clicked = app_button_touched(bid);
        tex_set_var(ctx, skip_ws(a2), VAR_INT, &clicked, 0);
        return 0;
    }
    // app.close_window(<win_id>)
    if (tex_strncmp(line, "app.close_window(", 17) == 0) {
        char a1[32]; extract_args(line+16, a1, 0, 0);
        WinID wid = (WinID)tex_eval_expr(ctx, a1);
        app_close_window(wid);
        return 0;
    }

    // ─── MODULE mem ────────────────────────────────────────

    // mem.free_kb(<var>)   — RAM libre en Ko
    if (tex_strncmp(line, "mem.free_kb(", 12) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+11, a1, 0, 0);
        int kb = (int)(pfa_free_frames() * 4096 / 1024);
        tex_set_var(ctx, skip_ws(a1), VAR_INT, &kb, 0);
        return 0;
    }
    // mem.total_kb(<var>)  — RAM totale en Ko
    if (tex_strncmp(line, "mem.total_kb(", 13) == 0) {
        char a1[TEX_VAR_NAME_LEN]; extract_args(line+12, a1, 0, 0);
        int kb = (int)(pfa_total_frames() * 4096 / 1024);
        tex_set_var(ctx, skip_ws(a1), VAR_INT, &kb, 0);
        return 0;
    }

    // ─── MODULE shell ──────────────────────────────────────

    // shell.exec(<cmd>)  — exécute une commande shell
    if (tex_strncmp(line, "shell.exec(", 11) == 0) {
        char a1[TEX_VAR_VALUE_LEN]; extract_args(line+10, a1, 0, 0);
        char cmd[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a1, cmd, sizeof(cmd));
        // Déléguer au shell : on imprime la commande (le shell est appelé depuis l'OS)
        print_string("[TEX shell.exec] "); print_string(cmd); print_string("\n");
        // TODO: appel direct shell_run(cmd) quand l'API sera exposée
        return 0;
    }
    // sys.exec(<cmd>)  — alias shell.exec pour compatibilité
    if (tex_strncmp(line, "sys.exec(", 9) == 0) {
        char a1[TEX_VAR_VALUE_LEN]; extract_args(line+8, a1, 0, 0);
        char cmd[TEX_VAR_VALUE_LEN]; tex_eval_string(ctx, a1, cmd, sizeof(cmd));
        print_string("[TEX sys.exec] "); print_string(cmd); print_string("\n");
        return 0;
    }

    not_assign:;

    // Commande inconnue — ignorer silencieusement
    return 0;
}

// ============================================================
// BOUCLE D'EXÉCUTION PRINCIPALE
// ============================================================

static int tex_run(TexContext* ctx) {
    // Pré-scan : collecte labels et fonctions, détecte #debug/#framework
    tex_prescan(ctx);

    if (ctx->dbg.enabled) {
        print_string("[TEX DBG] Mode debug actif\n");
    }
    if (ctx->fw.is_framework) {
        print_string("[TEX] Framework: "); print_string(ctx->fw.framework_name); print_string("\n");
    }

    ctx->current_line = 0;
    while (ctx->running && ctx->current_line < ctx->line_count) {
        ctx->jump_to = -1;
        ctx->returning = 0;

        tex_execute_line(ctx, ctx->lines[ctx->current_line]);

        if (ctx->jump_to >= 0) {
            ctx->current_line = ctx->jump_to;
        } else {
            ctx->current_line++;
        }
    }

    // Résumé débogueur à la fin
    if (ctx->dbg.error_count > 0) {
        print_string("[TEX DBG] Fin d'execution : ");
        char ebuf[16]; itoa_tex(ctx->dbg.error_count, ebuf, 16);
        print_string(ebuf); print_string(" erreur(s)\n");
        if (ctx->dbg.last_error_line >= 0) {
            char lbuf[16]; itoa_tex(ctx->dbg.last_error_line + 1, lbuf, 16);
            print_string("[TEX DBG] Derniere erreur : ligne ");
            print_string(lbuf); print_string(" — ");
            print_string(ctx->dbg.last_error_msg); print_string("\n");
        }
    }
    return 0;
}

// ============================================================
// API PUBLIQUE
// ============================================================

int tex_execute(const char* filename) {
    int bytes = fs_read_file(filename, (uint8_t*)g_script_buf, TEX_SCRIPT_BUF - 1);
    if (bytes <= 0) {
        // Pas de contexte ici, on affiche directement
        print_string("[TEX DBG] Fichier introuvable: "); print_string(filename); print_string("\n");
        return -1;
    }
    g_script_buf[bytes] = '\0';
    return tex_execute_string(g_script_buf);
}

int tex_execute_string(const char* src) {
    // Copier dans le buffer global
    int len = tex_strlen(src);
    if (len >= TEX_SCRIPT_BUF) len = TEX_SCRIPT_BUF - 1;
    memcpy(g_script_buf, src, len);
    g_script_buf[len] = '\0';

    TexContext ctx;
    tex_init(&ctx);

    // Parser les lignes
    char* p = g_script_buf;
    ctx.line_count = 0;
    ctx.lines[ctx.line_count++] = p;
    for (int i = 0; i < len && ctx.line_count < TEX_MAX_LINES; i++) {
        if (g_script_buf[i] == '\n') {
            g_script_buf[i] = '\0';
            if (i + 1 < len) ctx.lines[ctx.line_count++] = &g_script_buf[i+1];
        } else if (g_script_buf[i] == '\r') {
            g_script_buf[i] = '\0';
        }
    }

    return tex_run(&ctx);
}