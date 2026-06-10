// tex.h — TetraExecutable v3
// Langage de script natif de TetraOS
//
// Syntaxe inspirée Python/JS, modules inspirés Python.
//
// === Import de modules bas niveau (accès direct libs C de l'OS) ===
//   import <module>
//   Modules disponibles :
//     io       — entrées/sorties texte
//     fs       — système de fichiers
//     sys      — informations système
//     math     — calculs mathématiques
//     str      — manipulation de chaînes
//     gfx      — dessin VESA (pixels, rectangles, texte)
//     input    — clavier et souris en temps réel
//     session  — informations session courante
//     time     — temporisation
//     mem      — mémoire (alloc/free)
//     shell    — exécution de commandes shell
//     app      — gestion d'apps et fenêtres (bas niveau)
//
// === Packages haut niveau (regroupent plusieurs modules) ===
//   include @<package>.<NomPackage>
//   Packages disponibles :
//     @lib.fs          — alias import fs
//     @lib.ui          — alias import gfx + input
//     @lib.io          — alias import io
//     @lib.math        — alias import math
//     @lib.str         — alias import str
//     @lib.mem         — alias import mem
//     @lib.shell       — alias import shell
//     @lib.time        — alias import time
//     @lib.session     — alias import session
//     @package.AppCore      — ui + app + gfx + input (framework appli bureau)
//     @package.OsExt        — fs + mem + shell + sys (extensions OS)
//     @package.SystemModifier — sys + mem + fs + session (modification système)
//     @package.NetCore      — réservé futur (Internet)
//
// === Mode framework ===
//   Un .tex peut se déclarer framework dans son en-tête :
//     #framework <nom>
//   Il doit être placé à la racine du fs et déclarer ses exports dans un .frame.
//   D'autres scripts peuvent alors faire : include lib <nom>.tex
//
// === Débogueur ===
//   Toute erreur d'exécution affiche : "[TEX DBG] Ligne <N>: <message>"
//   En mode debug activé (#debug), chaque ligne exécutée est tracée.

#ifndef TEX_H
#define TEX_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Limites
// ============================================================
#define TEX_MAX_VARS        128
#define TEX_VAR_NAME_LEN     32
#define TEX_VAR_VALUE_LEN   256
#define TEX_MAX_LABELS       64
#define TEX_LABEL_NAME_LEN   32
#define TEX_MAX_LINES       2048
#define TEX_MAX_CALL_DEPTH    16
#define TEX_MAX_FUNCTIONS     64
#define TEX_FUNC_NAME_LEN     32
#define TEX_MAX_PARAMS         8
#define TEX_MAX_MODULES       32
#define TEX_MODULE_NAME_LEN   32
#define TEX_SCRIPT_BUF      16384

// Packages
#define TEX_MAX_PACKAGES      16
#define TEX_PACKAGE_NAME_LEN  32

// Frameworks externes
#define TEX_MAX_FRAMEWORKS    8
#define TEX_FRAMEWORK_NAME_LEN 64

// ============================================================
// Types de variables
// ============================================================
typedef enum {
    VAR_INT = 0,
    VAR_FLOAT,
    VAR_STRING,
    VAR_BOOL
} VarType;

// ============================================================
// Variable
// ============================================================
typedef struct {
    char    name[TEX_VAR_NAME_LEN];
    VarType type;
    union {
        int   ival;
        float fval;
        char  sval[TEX_VAR_VALUE_LEN];
    };
    int is_const;
    int scope;
} TexVar;

// ============================================================
// Label
// ============================================================
typedef struct {
    char name[TEX_LABEL_NAME_LEN];
    int  line;
} TexLabel;

// ============================================================
// Fonction utilisateur
// ============================================================
typedef struct {
    char name[TEX_FUNC_NAME_LEN];
    char params[TEX_MAX_PARAMS][TEX_VAR_NAME_LEN];
    int  param_count;
    int  start_line;
    int  end_line;
} TexFunc;

// ============================================================
// Frame d'appel
// ============================================================
typedef struct {
    int return_line;
    int scope;
} TexCallFrame;

// ============================================================
// Module importé
// ============================================================
typedef struct {
    char name[TEX_MODULE_NAME_LEN];
} TexModule;

// ============================================================
// Package importé (include @<package>.<Nom>)
// ============================================================
typedef struct {
    char name[TEX_PACKAGE_NAME_LEN];  // ex: "AppCore", "OsExt"
    char type;                         // 'p' = package, 'l' = lib
} TexPackage;

// ============================================================
// Framework externe chargé (include lib <nom>.tex)
// ============================================================
typedef struct {
    char name[TEX_FRAMEWORK_NAME_LEN]; // chemin ou nom du .tex framework
    int  loaded;
} TexFramework;

// ============================================================
// Bloc de contrôle
// ============================================================
typedef enum { BLOCK_IF = 0, BLOCK_WHILE, BLOCK_FOR } BlockType;

typedef struct {
    BlockType type;
    int  condition_met;
    int  loop_start_line;
    char condition[128];
    char for_var[TEX_VAR_NAME_LEN];
    int  for_end;
    int  for_step;
    int  skip;
} TexBlock;

// ============================================================
// Débogueur
// ============================================================
typedef struct {
    int  enabled;               // 1 si #debug présent dans le script
    int  last_error_line;       // dernière ligne en erreur (-1 = aucune)
    char last_error_msg[128];   // dernier message d'erreur
    int  error_count;           // nombre d'erreurs rencontrées
} TexDebugger;

// ============================================================
// Mode framework
// ============================================================
typedef struct {
    int  is_framework;                          // 1 si ce .tex est un framework
    char framework_name[TEX_PACKAGE_NAME_LEN];  // nom déclaré (#framework <nom>)
} TexFrameworkInfo;

// ============================================================
// Contexte d'exécution
// ============================================================
typedef struct {
    TexVar       vars[TEX_MAX_VARS];
    int          var_count;

    TexLabel     labels[TEX_MAX_LABELS];
    int          label_count;

    TexFunc      funcs[TEX_MAX_FUNCTIONS];
    int          func_count;

    TexCallFrame call_stack[TEX_MAX_CALL_DEPTH];
    int          call_depth;
    int          scope;

    TexModule    modules[TEX_MAX_MODULES];
    int          module_count;

    TexPackage   packages[TEX_MAX_PACKAGES];
    int          package_count;

    TexFramework frameworks[TEX_MAX_FRAMEWORKS];
    int          framework_count;

    TexBlock     block_stack[32];
    int          block_depth;

    char*        lines[TEX_MAX_LINES];
    int          line_count;

    int          running;
    int          current_line;
    int          jump_to;
    int          returning;
    char         return_val[TEX_VAR_VALUE_LEN];
    VarType      return_type;

    // Débogueur intégré
    TexDebugger  dbg;

    // Infos framework (si ce script est un framework)
    TexFrameworkInfo fw;

} TexContext;

// ============================================================
// API publique
// ============================================================
void    tex_init(TexContext* ctx);
int     tex_execute(const char* filename);
int     tex_execute_string(const char* src);
int     tex_execute_line(TexContext* ctx, const char* line);
void    tex_cleanup(TexContext* ctx);

int     tex_set_var(TexContext* ctx, const char* name, VarType type, const void* value, int is_const);
TexVar* tex_get_var(TexContext* ctx, const char* name);
void    tex_del_scope_vars(TexContext* ctx, int scope);

float   tex_eval_expr(TexContext* ctx, const char* expr);
void    tex_eval_string(TexContext* ctx, const char* expr, char* out, int max_len);
int     tex_eval_condition(TexContext* ctx, const char* cond);
int     tex_has_module(TexContext* ctx, const char* name);
int     tex_has_package(TexContext* ctx, const char* name);

// Débogueur
void    tex_dbg_error(TexContext* ctx, const char* msg);
void    tex_dbg_trace(TexContext* ctx, const char* line_content);

// Packages
void    tex_load_package(TexContext* ctx, const char* pkg_type, const char* pkg_name);
void    tex_load_lib(TexContext* ctx, const char* lib_name);

#endif // TEX_H
