#ifndef INPUT_H
#define INPUT_H

void handle_input();
int keyboard_read_scancode();
void process_command(const char* input);

char keyboard_get_char(void);
char get_input_char(void);

// Dispatcher centralisé 8042 :
// Lit le status register et route automatiquement vers souris ou clavier.
// - Si donnée souris  → mouse_poll() + redraw curseur, retourne 0
// - Si donnée clavier → décode et retourne le caractère (>0)
// - Si rien           → retourne 0
// À appeler en boucle ; bloque jusqu'à un vrai caractère clavier.
char input_dispatch_char(void);

// Callback optionnel quand un paquet souris complet est reçu (terminal graphique).
// NULL = comportement par défaut (efface/redessine le curseur).
void input_set_mouse_packet_handler(void (*fn)(void));

#endif