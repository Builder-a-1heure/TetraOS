// tex.c - TetraOS Executable System Implementation
// Version modifiée : Commandes SANS préfixe #
#include "tex.h"
#include "../gfx/screen.h"
#include "../lib/utils.h"
#include "../fs/fs.h"
#include "../drivers/input.h"

// ============================================================================
// FONCTIONS UTILITAIRES INTERNES
// ============================================================================

// Implémentation de atoi (string to int)
static int tex_atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') str++;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

// Implémentation de int_to_str (int to string)
static void tex_int_to_str(int num, char* str, int max_len) {
    int i = 0;
    int is_negative = 0;
    
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    
    char temp[16];
    int temp_i = 0;
    while (num > 0 && temp_i < 15) {
        temp[temp_i++] = '0' + (num % 10);
        num /= 10;
    }
    
    if (is_negative && i < max_len - 1) {
        str[i++] = '-';
    }
    
    while (temp_i > 0 && i < max_len - 1) {
        str[i++] = temp[--temp_i];
    }
    
    str[i] = '\0';
}

static const char* skip_spaces(const char* str) {
    while (*str == ' ' || *str == '\t') str++;
    return str;
}

static int is_varname_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || c == '_';
}

static void copy_varname(const char** src, char* dest, int max_len) {
    int i = 0;
    while (is_varname_char(**src) && i < max_len - 1) {
        dest[i++] = *(*src)++;
    }
    dest[i] = '\0';
}

void tex_init(TexContext* ctx) {
    ctx->var_count = 0;
    ctx->label_count = 0;
    ctx->line_count = 0;
    ctx->if_depth = 0;
    ctx->running = 1;
    ctx->current_line = 0;
    
    for (int i = 0; i < TEX_MAX_VARS; i++) {
        memset(ctx->vars[i].name, 0, TEX_VAR_NAME_LEN);
        ctx->vars[i].type = VAR_TYPE_INT;
        ctx->vars[i].int_value = 0;
        ctx->vars[i].is_const = 0;
    }
    
    for (int i = 0; i < TEX_MAX_LABELS; i++) {
        memset(ctx->labels[i].name, 0, TEX_LABEL_NAME_LEN);
        ctx->labels[i].line_number = -1;
    }
    
    for (int i = 0; i < TEX_MAX_LINES; i++) {
        ctx->lines[i] = NULL;
    }
}

void tex_cleanup(TexContext* ctx) {
    for (int i = 0; i < ctx->line_count; i++) {
        if (ctx->lines[i] != NULL) {
            ctx->lines[i] = NULL;
        }
    }
}

int tex_set_var(TexContext* ctx, const char* name, VarType type, void* value, int is_const) {
    TexVar* existing = tex_get_var(ctx, name);
    
    if (existing != NULL) {
        if (existing->is_const) {
            print_string("TEX Erreur: Impossible de modifier la constante '");
            print_string(name);
            print_string("'\n");
            return -1;
        }
        
        existing->type = type;
        if (type == VAR_TYPE_INT) {
            existing->int_value = *(int*)value;
        } else if (type == VAR_TYPE_FLOAT) {
            existing->float_value = *(float*)value;
        } else {
            strncpy(existing->str_value, (char*)value, TEX_VAR_VALUE_LEN - 1);
            existing->str_value[TEX_VAR_VALUE_LEN - 1] = '\0';
        }
        return 0;
    }
    
    if (ctx->var_count >= TEX_MAX_VARS) {
        print_string("TEX Erreur: Trop de variables\n");
        return -1;
    }
    
    TexVar* var = &ctx->vars[ctx->var_count];
    strncpy(var->name, name, TEX_VAR_NAME_LEN - 1);
    var->name[TEX_VAR_NAME_LEN - 1] = '\0';
    var->type = type;
    var->is_const = is_const;
    
    if (type == VAR_TYPE_INT) {
        var->int_value = *(int*)value;
    } else if (type == VAR_TYPE_FLOAT) {
        var->float_value = *(float*)value;
    } else {
        strncpy(var->str_value, (char*)value, TEX_VAR_VALUE_LEN - 1);
        var->str_value[TEX_VAR_VALUE_LEN - 1] = '\0';
    }
    
    ctx->var_count++;
    return 0;
}

TexVar* tex_get_var(TexContext* ctx, const char* name) {
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0) {
            return &ctx->vars[i];
        }
    }
    return NULL;
}

float tex_eval_expr(TexContext* ctx, const char* expr) {
    expr = skip_spaces(expr);
    
    // Parser l'opérande gauche
    float left_val = 0.0f;
    
    if ((*expr >= '0' && *expr <= '9') || *expr == '-') {
        left_val = (float)tex_atoi(expr);
        // Avancer jusqu'au prochain opérateur ou fin
        while (*expr && (*expr == '-' || (*expr >= '0' && *expr <= '9'))) expr++;
    } else if (is_varname_char(*expr)) {
        char varname[TEX_VAR_NAME_LEN];
        copy_varname(&expr, varname, TEX_VAR_NAME_LEN);
        
        TexVar* var = tex_get_var(ctx, varname);
        if (var) {
            if (var->type == VAR_TYPE_INT) left_val = (float)var->int_value;
            if (var->type == VAR_TYPE_FLOAT) left_val = var->float_value;
        }
    }
    
    // Chercher un opérateur
    expr = skip_spaces(expr);
    if (*expr == '\0' || *expr == ')' || *expr == '{') {
        return left_val;
    }
    
    char op = *expr;
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        return left_val;
    }
    expr++;
    
    // Parser l'opérande droit
    expr = skip_spaces(expr);
    float right_val = 0.0f;
    
    if ((*expr >= '0' && *expr <= '9') || *expr == '-') {
        right_val = (float)tex_atoi(expr);
    } else if (is_varname_char(*expr)) {
        char varname[TEX_VAR_NAME_LEN];
        copy_varname(&expr, varname, TEX_VAR_NAME_LEN);
        
        TexVar* var = tex_get_var(ctx, varname);
        if (var) {
            if (var->type == VAR_TYPE_INT) right_val = (float)var->int_value;
            if (var->type == VAR_TYPE_FLOAT) right_val = var->float_value;
        }
    }
    
    // Effectuer l'opération
    switch (op) {
        case '+': return left_val + right_val;
        case '-': return left_val - right_val;
        case '*': return left_val * right_val;
        case '/': return (right_val != 0.0f) ? (left_val / right_val) : 0.0f;
        default: return left_val;
    }
}

void tex_eval_string(TexContext* ctx, const char* expr, char* out, int max_len) {
    int out_idx = 0;
    expr = skip_spaces(expr);
    
    if (*expr == '"') expr++;
    
    while (*expr && out_idx < max_len - 1) {
        if (*expr == '"') {
            break;
        } else if (is_varname_char(*expr)) {
            const char* start = expr;
            char varname[TEX_VAR_NAME_LEN];
            copy_varname(&expr, varname, TEX_VAR_NAME_LEN);
            
            TexVar* var = tex_get_var(ctx, varname);
            if (var) {
                if (var->type == VAR_TYPE_STRING) {
                    for (int j = 0; var->str_value[j] && out_idx < max_len - 1; j++) {
                        out[out_idx++] = var->str_value[j];
                    }
                } else if (var->type == VAR_TYPE_INT) {
                    char num[16];
                    tex_int_to_str(var->int_value, num, sizeof(num));
                    for (int j = 0; num[j] && out_idx < max_len - 1; j++) {
                        out[out_idx++] = num[j];
                    }
                }
            } else {
                while (start < expr && out_idx < max_len - 1) {
                    out[out_idx++] = *start++;
                }
            }
        } else if (*expr == '\\' && *(expr + 1) == 'n') {
            out[out_idx++] = '\n';
            expr += 2;
        } else {
            out[out_idx++] = *expr++;
        }
    }
    
    out[out_idx] = '\0';
}

int tex_eval_condition(TexContext* ctx, const char* condition) {
    char left[128], op[4], right[128];
    int i = 0;
    
    condition = skip_spaces(condition);
    if (*condition == '(') condition++;
    
    while (*condition && *condition != '=' && *condition != '!' && 
           *condition != '<' && *condition != '>' && *condition != ' ' && i < 127) {
        left[i++] = *condition++;
    }
    left[i] = '\0';
    
    condition = skip_spaces(condition);
    
    i = 0;
    while (*condition && (*condition == '=' || *condition == '!' || 
           *condition == '<' || *condition == '>') && i < 3) {
        op[i++] = *condition++;
    }
    op[i] = '\0';
    
    condition = skip_spaces(condition);
    
    i = 0;
    while (*condition && *condition != ')' && *condition != '{' && i < 127) {
        right[i++] = *condition++;
    }
    right[i] = '\0';
    
    i = strlen(right) - 1;
    while (i >= 0 && (right[i] == ' ' || right[i] == '\t')) right[i--] = '\0';
    
    float left_val = tex_eval_expr(ctx, left);
    float right_val = tex_eval_expr(ctx, right);
    
    if (strcmp(op, "==") == 0) return (int)(left_val == right_val);
    if (strcmp(op, "!=") == 0) return (int)(left_val != right_val);
    if (strcmp(op, "<") == 0) return (int)(left_val < right_val);
    if (strcmp(op, ">") == 0) return (int)(left_val > right_val);
    if (strcmp(op, "<=") == 0) return (int)(left_val <= right_val);
    if (strcmp(op, ">=") == 0) return (int)(left_val >= right_val);
    
    return 0;
}

// ============================================================================
// EXÉCUTION DE LIGNES
// ============================================================================

int tex_execute_line(TexContext* ctx, const char* line) {
    line = skip_spaces(line);
    
    // Ignorer les lignes vides et les commentaires
    if (*line == '\0' || *line == ';') {
        return 0;
    }
    
    // Ignorer les commentaires // (vérifier 2 caractères)
    if (*line == '/' && *(line + 1) == '/') {
        return 0;
    }
    
    // Gestion des accolades pour les blocs if
    if (strcmp(line, "}") == 0) {
        if (ctx->if_depth > 0) {
            ctx->if_depth--;
        }
        return 0;
    }
    
    // Vérifier si on doit exécuter la ligne (si on n'est pas dans un bloc if faux)
    int should_execute = 1;
    for (int i = 0; i < ctx->if_depth; i++) {
        if (ctx->if_stack[i].active && !ctx->if_stack[i].condition_met) {
            should_execute = 0;
            break;
        }
    }
    
    // Si on est dans un bloc if faux, ignorer tout sauf les accolades et les if
    if (!should_execute && strncmp(line, "if ", 3) != 0 && strncmp(line, "if(", 3) != 0) {
        return 0;
    }
    
    // === COMMANDES SANS PRÉFIXE # ===
    
    // var <nom> = <valeur>
    if (strncmp(line, "var ", 4) == 0) {
        line += 4;
        line = skip_spaces(line);
        
        char varname[TEX_VAR_NAME_LEN];
        copy_varname(&line, varname, TEX_VAR_NAME_LEN);
        
        line = skip_spaces(line);
        if (*line == '=') {
            line++;
            line = skip_spaces(line);
            
            // Vérifier si c'est une chaîne
            if (*line == '"') {
                char value[TEX_VAR_VALUE_LEN];
                tex_eval_string(ctx, line, value, TEX_VAR_VALUE_LEN);
                tex_set_var(ctx, varname, VAR_TYPE_STRING, value, 0);
            } else {
                int value = (int)tex_eval_expr(ctx, line);
                tex_set_var(ctx, varname, VAR_TYPE_INT, &value, 0);
            }
        }
        return 0;
    }
    
    // const <nom> = <valeur>
    if (strncmp(line, "const ", 6) == 0) {
        line += 6;
        line = skip_spaces(line);
        
        char varname[TEX_VAR_NAME_LEN];
        copy_varname(&line, varname, TEX_VAR_NAME_LEN);
        
        line = skip_spaces(line);
        if (*line == '=') {
            line++;
            line = skip_spaces(line);
            
            if (*line == '"') {
                char value[TEX_VAR_VALUE_LEN];
                tex_eval_string(ctx, line, value, TEX_VAR_VALUE_LEN);
                tex_set_var(ctx, varname, VAR_TYPE_STRING, value, 1);
            } else {
                int value = (int)tex_eval_expr(ctx, line);
                tex_set_var(ctx, varname, VAR_TYPE_INT, &value, 1);
            }
        }
        return 0;
    }
    
    // io.print(<expr>)
    if (strncmp(line, "io.print(", 9) == 0) {
        line += 9;
        
        char arg[256];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(i > 0 && *line == ')' && !in_quotes) && i < 255) {
            if (*line == '"') in_quotes = !in_quotes;
            arg[i++] = *line++;
        }
        arg[i] = '\0';
        
        char output[256];
        tex_eval_string(ctx, arg, output, sizeof(output));
        print_string(output);
        
        return 0;
    }
    
    // io.println(<expr>)
    if (strncmp(line, "io.println(", 11) == 0) {
        line += 11;
        
        char arg[256];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(*line == ')' && !in_quotes) && i < 255) {
            if (*line == '"') in_quotes = !in_quotes;
            arg[i++] = *line++;
        }
        arg[i] = '\0';
        
        char output[256];
        tex_eval_string(ctx, arg, output, sizeof(output));
        print_string(output);
        print_char('\n');
        
        return 0;
    }
    
    // io.input(<prompt>, <var>)
    if (strncmp(line, "io.input(", 9) == 0) {
        line += 9;
        
        char prompt[256];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(*line == ',' && !in_quotes) && i < 255) {
            if (*line == '"') in_quotes = !in_quotes;
            if (*line != '"' || in_quotes) {
                prompt[i++] = *line;
            }
            line++;
        }
        prompt[i] = '\0';
        
        if (*line == ',') {
            line++;
            line = skip_spaces(line);
            
            char varname[TEX_VAR_NAME_LEN];
            copy_varname(&line, varname, TEX_VAR_NAME_LEN);
            
            char prompt_eval[256];
            tex_eval_string(ctx, prompt, prompt_eval, sizeof(prompt_eval));
            print_string(prompt_eval);
            
            char input_buffer[256];
            int idx = 0;
            while (1) {
                char c = keyboard_get_char();
                if (c == '\r' || c == '\n') {
                    input_buffer[idx] = '\0';
                    print_char('\n');
                    break;
                } else if ((c == '\b' || c == 127) && idx > 0) {
                    idx--;
                    print_string("\b \b");
                } else if (c >= 32 && c <= 126 && idx < 255) {
                    input_buffer[idx++] = c;
                    print_char(c);
                }
            }
            
            tex_set_var(ctx, varname, VAR_TYPE_STRING, input_buffer, 0);
        }
        
        return 0;
    }
    
    // fs.write(<filename>, <content>)
    if (strncmp(line, "fs.write(", 9) == 0) {
        line += 9;
        
        char filename[64];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(*line == ',' && !in_quotes) && i < 63) {
            if (*line == '"') {
                in_quotes = !in_quotes;
            } else {
                filename[i++] = *line;
            }
            line++;
        }
        filename[i] = '\0';
        
        if (*line == ',') {
            line++;
            line = skip_spaces(line);
            
            char content[512];
            tex_eval_string(ctx, line, content, sizeof(content));
            
            fs_write_file(filename, (uint8_t*)content, strlen(content));
        }
        
        return 0;
    }
    
    // fs.read(<filename>, <var>)
    if (strncmp(line, "fs.read(", 8) == 0) {
        line += 8;
        
        char filename[64];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(*line == ',' && !in_quotes) && i < 63) {
            if (*line == '"') {
                in_quotes = !in_quotes;
            } else {
                filename[i++] = *line;
            }
            line++;
        }
        filename[i] = '\0';
        
        if (*line == ',') {
            line++;
            line = skip_spaces(line);
            
            char varname[TEX_VAR_NAME_LEN];
            copy_varname(&line, varname, TEX_VAR_NAME_LEN);
            
            uint8_t buffer[512];
            int bytes = fs_read_file(filename, buffer, sizeof(buffer) - 1);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                tex_set_var(ctx, varname, VAR_TYPE_STRING, buffer, 0);
            }
        }
        
        return 0;
    }
    
    // fs.exists(<filename>, <var>)
    if (strncmp(line, "fs.exists(", 10) == 0) {
        line += 10;
        
        char filename[64];
        int i = 0;
        int in_quotes = 0;
        while (*line && !(*line == ',' && !in_quotes) && i < 63) {
            if (*line == '"') {
                in_quotes = !in_quotes;
            } else {
                filename[i++] = *line;
            }
            line++;
        }
        filename[i] = '\0';
        
        if (*line == ',') {
            line++;
            line = skip_spaces(line);
            
            char varname[TEX_VAR_NAME_LEN];
            copy_varname(&line, varname, TEX_VAR_NAME_LEN);
            
            int exists = (fs_find(filename) >= 0) ? 1 : 0;
            tex_set_var(ctx, varname, VAR_TYPE_INT, &exists, 0);
        }
        
        return 0;
    }
    
    // fs.delete(<filename>)
    if (strncmp(line, "fs.delete(", 10) == 0) {
        line += 10;
        
        char filename[64];
        tex_eval_string(ctx, line, filename, sizeof(filename));
        fs_delete(filename);
        
        return 0;
    }
    
    // if <condition> {
    if (strncmp(line, "if ", 3) == 0 || strncmp(line, "if(", 3) == 0) {
        line += 3;  // Corriger: avancer de 3 caractères pour "if " ou "if("
        line = skip_spaces(line);
        
        int condition_met = tex_eval_condition(ctx, line);
        
        if (ctx->if_depth < 15) {
            ctx->if_stack[ctx->if_depth].active = 1;
            ctx->if_stack[ctx->if_depth].condition_met = condition_met;
            ctx->if_depth++;
        }
        
        return 0;
    }
    
    // clear / clear()
    if (strcmp(line, "clear") == 0 || strcmp(line, "clear()") == 0) {
        clear_screen();
        return 0;
    }
    
    // exit / exit()
    if (strcmp(line, "exit") == 0 || strcmp(line, "exit()") == 0) {
        ctx->running = 0;
        return 0;
    }
    
    // Assignation simple: varname = expression
    // (doit être vérifié en dernier pour éviter les faux positifs)
    const char* equals_pos = line;
    while (*equals_pos && *equals_pos != '=' && *equals_pos != '(' && *equals_pos != '.') {
        equals_pos++;
    }
    
    if (*equals_pos == '=' && *(equals_pos + 1) != '=') {
        // C'est une assignation simple
        char varname[TEX_VAR_NAME_LEN];
        int i = 0;
        const char* ptr = line;
        
        // Extraire le nom de la variable
        while (is_varname_char(*ptr) && i < TEX_VAR_NAME_LEN - 1) {
            varname[i++] = *ptr++;
        }
        varname[i] = '\0';
        
        // Vérifier que la variable existe déjà
        TexVar* existing = tex_get_var(ctx, varname);
        if (existing == NULL) {
            print_string("TEX Erreur: Variable '");
            print_string(varname);
            print_string("' non declaree\n");
            return -1;
        }
        
        if (existing->is_const) {
            print_string("TEX Erreur: Impossible de modifier la constante '");
            print_string(varname);
            print_string("'\n");
            return -1;
        }
        
        // Sauter jusqu'au signe =
        while (*ptr && *ptr != '=') ptr++;
        if (*ptr == '=') ptr++;
        ptr = skip_spaces(ptr);
        
        // Évaluer l'expression à droite du =
        if (*ptr == '"') {
            char value[TEX_VAR_VALUE_LEN];
            tex_eval_string(ctx, ptr, value, TEX_VAR_VALUE_LEN);
            tex_set_var(ctx, varname, VAR_TYPE_STRING, value, 0);
        } else {
            int value = (int)tex_eval_expr(ctx, ptr);
            tex_set_var(ctx, varname, VAR_TYPE_INT, &value, 0);
        }
        
        return 0;
    }
    return 0;
}

int tex_execute(const char* filename) {
    uint8_t buffer[4096];
    int bytes = fs_read_file(filename, buffer, sizeof(buffer) - 1);
    
    if (bytes <= 0) {
        print_string("TEX: Impossible de lire le fichier ");
        print_string(filename);
        print_string("\n");
        return -1;
    }
    
    buffer[bytes] = '\0';
    
    TexContext ctx;
    tex_init(&ctx);
    
    char* line_start = (char*)buffer;
    ctx.line_count = 0;
    
    // Parser les lignes
    for (int i = 0; i < bytes && ctx.line_count < TEX_MAX_LINES; i++) {
        if (buffer[i] == '\n' || buffer[i] == '\0') {
            buffer[i] = '\0';
            ctx.lines[ctx.line_count++] = line_start;
            line_start = (char*)&buffer[i + 1];
        }
    }
    
    // Ajouter la dernière ligne si elle n'a pas de \n final
    if (line_start < (char*)buffer + bytes && ctx.line_count < TEX_MAX_LINES) {
        ctx.lines[ctx.line_count++] = line_start;
    }
    
    for (ctx.current_line = 0; ctx.current_line < ctx.line_count && ctx.running; ctx.current_line++) {
        tex_execute_line(&ctx, ctx.lines[ctx.current_line]);
    }
    
    tex_cleanup(&ctx);
    return 0;
}