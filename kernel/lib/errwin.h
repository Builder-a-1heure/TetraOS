// kernel/lib/errwin.h — Fenêtres d'erreur TetraOS
//
// Affiche une fenêtre modale AppCore dès qu'une erreur survient.
// Fonctionne depuis n'importe quelle app (AppCore doit être initialisé).
//
// Usage :
//   errwin_error("Titre", "Message d'erreur.");
//   errwin_warn ("Titre", "Avertissement.");
//   errwin_info ("Titre", "Information.");
//
// Format multi-ligne — utiliser \n dans le message :
//   errwin_error("Acces refuse", "Permission insuffisante.\nFichier : notes.txt");
//
// La fenêtre bloque jusqu'à ce que l'utilisateur clique OK ou appuie sur Entrée/Echap.

#ifndef ERRWIN_H
#define ERRWIN_H

// ============================================================
// Niveaux de sévérité
// ============================================================
typedef enum {
    ERRWIN_INFO    = 0,   // bleu — information neutre
    ERRWIN_WARN    = 1,   // jaune — attention
    ERRWIN_ERROR   = 2,   // rouge — erreur
} ErrwinLevel;

// ============================================================
// API principale
// ============================================================

// Affiche une fenêtre modale avec titre + message + niveau.
// msg peut contenir des '\n' pour sauter des lignes (max 6 lignes).
// Bloque jusqu'au clic OK ou touche Entrée/Echap.
void errwin_show(ErrwinLevel level, const char* title, const char* msg);

// Raccourcis
#define errwin_error(title, msg)  errwin_show(ERRWIN_ERROR, title, msg)
#define errwin_warn(title, msg)   errwin_show(ERRWIN_WARN,  title, msg)
#define errwin_info(title, msg)   errwin_show(ERRWIN_INFO,  title, msg)

// ============================================================
// Variante avec deux chaînes concaténées (évite de formater soi-même)
// Utile pour afficher un message + un nom de fichier sans buffer intermédiaire.
// ex: errwin_error2("Erreur", "Impossible d'ouvrir : ", filename)
// ============================================================
void errwin_show2(ErrwinLevel level, const char* title,
                  const char* msg1, const char* msg2);

#define errwin_error2(title, m1, m2)  errwin_show2(ERRWIN_ERROR, title, m1, m2)
#define errwin_warn2(title, m1, m2)   errwin_show2(ERRWIN_WARN,  title, m1, m2)
#define errwin_info2(title, m1, m2)   errwin_show2(ERRWIN_INFO,  title, m1, m2)

#endif // ERRWIN_H
