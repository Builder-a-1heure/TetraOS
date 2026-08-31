// kernel/lib/sched.h — Scheduler noyau TetraOS (threads + préemption)
//
// Modèle : threads noyau "verts" en round-robin, tous en ring 0, sur le
// même espace mémoire plat (pas de pagination/process séparé — voir
// process.h pour l'isolation logique par ACL/UID qui reste, elle,
// mono-tâche pour l'instant). Chaque thread a sa propre pile statique.
//
// Fonctionne en s'appuyant sur l'infra existante :
//   - timer.c (PIT à 100Hz) fournit le tick qui déclenche la préemption.
//   - idt.h/isr.c fournissent interrupts_enable/disable et le chemin
//     d'interruption dont le context switch réutilise l'épilogue (voir
//     sched.c pour le détail du mécanisme).
//
// Zéro thread créé == comportement strictement identique à avant : la
// tâche 0 ("main") est la continuation de kmain(), rien ne change tant
// que thread_create() n'est pas appelé.
#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define MAX_TASKS       8
#define TASK_STACK_SIZE 16384   // 16 Ko par thread — largement suffisant,
                                 // pas d'allocateur dynamique dans ce noyau

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_ZOMBIE          // terminé, en attente de recyclage du slot
} task_state_t;

typedef void (*task_entry_t)(void*);

typedef struct task {
    uint32_t      esp;         // Pile sauvegardée — valide seulement
                                // quand state != TASK_RUNNING
    uint8_t*      stack_base;  // Base de la pile allouée (0 pour la tâche 0)
    task_state_t  state;
    task_entry_t  entry;
    void*         arg;
    int           id;
    char          name[16];
} task_t;

// Initialise le scheduler : la tâche 0 ("main") devient la continuation
// de l'appelant (typiquement kmain(), juste après idt_init()+timer_init()).
// Active la préemption par défaut (voir sched_set_preemption()).
// À appeler une seule fois.
void sched_init(void);

// Crée un thread noyau prêt à tourner (état READY, pas encore exécuté).
// `name` est tronqué à 15 caractères, uniquement pour le debug.
// Retourne l'id (0..MAX_TASKS-1) ou -1 si plus de slot libre.
int thread_create(const char* name, task_entry_t entry, void* arg);

// Cède volontairement le CPU à un autre thread READY (round-robin).
// No-op silencieux si aucun autre thread n'est prêt — sûr à appeler
// même si aucun thread_create() n'a jamais été fait.
void thread_yield(void);

// Termine le thread courant et libère son slot. Ne retourne jamais.
// Appelé automatiquement si la fonction d'entrée d'un thread retourne.
void thread_exit(void);

// Active/coupe la préemption automatique (tick timer). Activée par
// défaut dans sched_init(). La couper revient à du coopératif pur
// (seuls les thread_yield() explicites font commuter les threads) —
// utile pour debug ou pour protéger une section critique longue sans
// avoir à jouer avec interrupts_disable() (qui couperait aussi le
// timer lui-même).
void sched_set_preemption(int enabled);

// Id du thread actuellement élu.
int thread_current_id(void);

// Nombre de threads actifs (READY + RUNNING), tâche 0 incluse.
int thread_count(void);

#endif // SCHED_H
