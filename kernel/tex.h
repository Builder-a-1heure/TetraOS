// tex.h - TetraOS Executable System
// Système d'interprétation de scripts .tex
// Version modifiée : Commandes SANS préfixe #
#ifndef TEX_H
#define TEX_H

#include <stdint.h>
#include <stddef.h>

// Constantes
#define TEX_MAX_VARS 64
#define TEX_VAR_NAME_LEN 32
#define TEX_VAR_VALUE_LEN 256
#define TEX_MAX_LABELS 32
#define TEX_LABEL_NAME_LEN 32
#define TEX_MAX_LINES 512

// Types de variables
typedef enum {
    VAR_TYPE_INT,
    VAR_TYPE_FLOAT,
    VAR_TYPE_STRING
} VarType;

// Structure de variable
typedef struct {
    char name[TEX_VAR_NAME_LEN];
    VarType type;
    union {
        int int_value;
        float float_value;
        char str_value[TEX_VAR_VALUE_LEN];
    };
    int is_const;  // 1 si c'est une constante
} TexVar;

// Structure de label pour les sauts
typedef struct {
    char name[TEX_LABEL_NAME_LEN];
    int line_number;
} TexLabel;

// Structure pour les blocs if
typedef struct {
    int active;        // 1 si on est dans un bloc if
    int condition_met; // 1 si la condition est vraie
    int brace_level;   // Niveau d'imbrication des accolades
} TexIfBlock;

// Contexte d'exécution
typedef struct {
    TexVar vars[TEX_MAX_VARS];
    int var_count;
    
    TexLabel labels[TEX_MAX_LABELS];
    int label_count;
    
    char* lines[TEX_MAX_LINES];
    int line_count;
    
    TexIfBlock if_stack[16];
    int if_depth;
    
    int running;
    int current_line;
} TexContext;

// === Fonctions principales ===

// Initialise le contexte d'exécution
void tex_init(TexContext* ctx);

// Exécute un fichier .tex
int tex_execute(const char* filename);

// Exécute une ligne de commande
int tex_execute_line(TexContext* ctx, const char* line);

// Nettoie le contexte
void tex_cleanup(TexContext* ctx);

// === Gestion des variables ===

// Définit une variable
int tex_set_var(TexContext* ctx, const char* name, VarType type, void* value, int is_const);

// Récupère une variable
TexVar* tex_get_var(TexContext* ctx, const char* name);

// === Évaluation d'expressions ===

// Évalue une expression arithmétique
float tex_eval_expr(TexContext* ctx, const char* expr);

// Évalue une expression de chaîne (résout les variables)
void tex_eval_string(TexContext* ctx, const char* expr, char* out, int max_len);

// Évalue une condition booléenne
int tex_eval_condition(TexContext* ctx, const char* condition);

#endif // TEX_H