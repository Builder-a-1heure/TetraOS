#ifndef SHELL_H
#define SHELL_H

// ============================================================
// shell.h — Shell namespaced TetraOS
//
// Parse et dispatche les commandes "module.commande [args]".
// Modules disponibles : ray64, session, sys, tex
//
// Utilisation :
//   char mod[32], cmd[64], args[192];
//   if (shell_parse(input, mod, sizeof(mod),
//                            cmd, sizeof(cmd),
//                            args, sizeof(args))) {
//       shell_dispatch(mod, cmd, args);
//   }
// ============================================================

// Parse "module.commande args" → mod, cmd, args séparés.
// Retourne 1 si le format "mod.cmd" est respecté, 0 sinon.
int shell_parse(const char* input,
                char* mod_out,  int mod_max,
                char* cmd_out,  int cmd_max,
                char* args_out, int args_max);

// Dispatche vers le bon handler selon le module.
// Affiche un message d'erreur si le module est inconnu.
void shell_dispatch(const char* mod, const char* cmd, const char* args);

// Dispatchers individuels (accessibles pour extensions futures)
void dispatch_ray64(const char* cmd, const char* args);
void dispatch_session(const char* cmd, const char* args);
void dispatch_sys(const char* cmd, const char* args);
void dispatch_tex(const char* cmd, const char* args);

// Easter egg
void cmd_sl(void);

void textedit_run(const char* filename);

#endif // SHELL_H
