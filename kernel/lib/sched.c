// kernel/lib/sched.c — Scheduler round-robin (coopératif + préemptif)
//
// ============================================================
// PRINCIPE DU CONTEXT SWITCH
// ============================================================
// Le noyau tourne toujours en ring 0, segments plats (voir idt.h), sans
// pagination. Chaque thread est donc juste "une pile qui ressemble à une
// suite d'appels C suspendue". On exploite ça avec context_switch() :
// une fonction "naked" qui sauvegarde les 4 registres callee-saved de
// l'ABI cdecl i386 (ebx, esi, edi, ebp) sur la pile courante, mémorise
// %esp dans la tâche sortante, charge %esp de la tâche entrante, restaure
// ses 4 registres, puis fait un simple `ret`.
//
// Ce `ret` dépile l'adresse de retour laissée sur LA PILE DE LA TÂCHE
// ENTRANTE — qui pointe soit :
//   - vers task_trampoline() si le thread n'a encore jamais tourné
//     (pile construite à la main par thread_create(), voir plus bas) ;
//   - vers l'instruction juste après le précédent appel à
//     context_switch() dans CE thread, s'il avait été suspendu par un
//     thread_yield() ou par une préemption timer.
//
// Le point clé qui rend ça correct même en présence de préemption par
// IRQ : quand la tâche préemptée reprend la main, elle "remonte"
// naturellement toute la pile d'appels qu'elle avait au moment de
// l'interruption (scheduler_tick -> timer_irq_handler -> interrupt_handler
// -> épilogue isr_common_stub -> `iret`), donc son EFLAGS/CS/EIP d'origine
// est restauré correctement par le CPU lui-même, sans rien de spécial à
// coder ici.
//
// ATTENTION EOI : pour que ça marche, l'accusé de réception au PIC
// (pic_send_eoi) doit être envoyé AVANT d'appeler le handler d'IRQ, pas
// après — sinon, quand context_switch() "abandonne" la pile de la tâche
// préemptée pour sauter sur une autre, l'EOI de ce tick ne serait jamais
// émis avant qu'un futur tick ne revienne dessus, et le PIC bloquerait
// alors IRQ0 (et toutes les IRQ de priorité inférieure) indéfiniment
// après la toute première commutation. Voir le correctif dans isr.c
// (interrupt_handler): EOI déplacé avant l'appel du handler.
// ============================================================

#include "sched.h"
#include "idt.h"
#include "timer.h"
#include "utils.h"

static task_t   g_tasks[MAX_TASKS];
static uint8_t  g_stacks[MAX_TASKS][TASK_STACK_SIZE] __attribute__((aligned(16)));
static int      g_current          = 0;
static int      g_preempt_enabled  = 0;
static uint32_t g_tick_count       = 0;

// Quantum de préemption : toutes les TIME_SLICE_TICKS interruptions timer
// (100Hz, voir main.c: timer_init(100)) -> ~50Hz de commutation max.
#define TIME_SLICE_TICKS 2

static void scheduler_tick(void); // forward decl — utilisée par sched_init()

// ============================================================
// context_switch(&old->esp, new->esp)
// Sauvegarde ebx/esi/edi/ebp + esp courant dans *old_esp, puis restaure
// depuis new_esp. Ne touche jamais eax/ecx/edx (caller-saved, donc pas
// besoin) ni EFLAGS (IF est un état CPU global, chaque point d'appel
// (thread_yield / scheduler_tick) est responsable de le régler lui-même
// autour de l'appel — voir plus bas).
// ============================================================
__attribute__((naked)) static void context_switch(uint32_t* old_esp, uint32_t new_esp) {
    (void)old_esp; (void)new_esp; // paramètres lus manuellement via la pile en asm
    __asm__ volatile (
        "push %ebp\n"
        "push %ebx\n"
        "push %esi\n"
        "push %edi\n"
        "mov  20(%esp), %eax\n"   // eax = old_esp (arg1) : ret@+esp0 +4*4 pushes
        "mov  %esp, (%eax)\n"     // *old_esp = esp courant (après les 4 push)
        "mov  24(%esp), %eax\n"   // eax = new_esp (arg2)
        "mov  %eax, %esp\n"       // *** bascule de pile ***
        "pop  %edi\n"
        "pop  %esi\n"
        "pop  %ebx\n"
        "pop  %ebp\n"
        "ret\n"
    );
}

// Point d'entrée réel de tout thread fraîchement créé (jamais utilisé
// par la tâche 0/main, qui tourne déjà au moment de sched_init()).
static void task_trampoline(task_t* t) {
    // Un thread neuf démarre avec les interruptions actives : c'est ce
    // qui permet au timer de venir le préempter à son tour.
    interrupts_enable();
    if (t->entry) t->entry(t->arg);
    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); } // jamais atteint
}

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) g_tasks[i].state = TASK_UNUSED;

    g_tasks[0].state      = TASK_RUNNING;
    g_tasks[0].id         = 0;
    g_tasks[0].stack_base = 0;   // pile de boot (0x3000000), pas gérée ici
    g_tasks[0].entry      = 0;
    g_tasks[0].arg        = 0;
    strncpy(g_tasks[0].name, "main", sizeof(g_tasks[0].name));

    g_current         = 0;
    g_tick_count      = 0;
    g_preempt_enabled = 1;

    // Branche la préemption sur le tick PIT existant (100Hz, voir
    // timer.c). N'a d'effet réel qu'une fois timer_init() appelé côté
    // kmain() — sched_init() doit donc être placé APRES timer_init()
    // dans la séquence de boot.
    timer_set_tick_callback(scheduler_tick);
}

int thread_create(const char* name, task_entry_t entry, void* arg) {
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = i; break; }
    }
    if (slot < 0) return -1;

    task_t* t     = &g_tasks[slot];
    t->stack_base = g_stacks[slot];
    t->entry      = entry;
    t->arg        = arg;
    t->id         = slot;
    strncpy(t->name, name ? name : "thread", sizeof(t->name));
    t->name[sizeof(t->name) - 1] = '\0';

    // Construction de la pile initiale — voir le commentaire en tête de
    // fichier : on empile "à la main" exactement ce que context_switch()
    // s'attend à trouver, dans l'ordre inverse de la disposition finale.
    uint8_t* top = t->stack_base + TASK_STACK_SIZE;
    uint32_t* sp = (uint32_t*)top;

    *(--sp) = (uint32_t)t;               // argument de task_trampoline(t)
    *(--sp) = (uint32_t)task_trampoline; // "adresse de retour" du futur ret
    *(--sp) = 0;                         // ebp (valeur initiale sans importance)
    *(--sp) = 0;                         // ebx
    *(--sp) = 0;                         // esi
    *(--sp) = 0;                         // edi

    t->esp   = (uint32_t)sp;
    t->state = TASK_READY;
    return slot;
}

int thread_current_id(void) { return g_current; }

int thread_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (g_tasks[i].state == TASK_READY || g_tasks[i].state == TASK_RUNNING) n++;
    return n;
}

// Choisit le prochain thread READY après g_current, en round-robin.
// Retourne -1 si aucun autre thread n'est prêt (system mono-tâche ou
// tous les autres sont bloqués/zombies).
static int pick_next(void) {
    for (int i = 1; i <= MAX_TASKS; i++) {
        int idx = (g_current + i) % MAX_TASKS;
        if (g_tasks[idx].state == TASK_READY) return idx;
    }
    return -1;
}

// Coeur du scheduler — appelable aussi bien depuis un contexte normal
// (thread_yield, avec IF déjà coupé par l'appelant) que depuis le
// contexte IRQ du timer (scheduler_tick, IF déjà à 0 par construction
// du chemin d'interruption). Ne fait RIEN si aucun autre thread n'est
// prêt : comportement transparent quand il n'y a qu'un thread.
static void schedule(void) {
    int next = pick_next();
    if (next < 0) return; // rien d'autre à faire tourner

    task_t* cur = &g_tasks[g_current];
    task_t* nxt = &g_tasks[next];

    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    nxt->state = TASK_RUNNING;

    int prev = g_current;
    g_current = next;

    // Recycle les slots des threads terminés au passage (petit ménage,
    // pas indispensable mais évite d'accumuler des ZOMBIE pour rien).
    for (int i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_ZOMBIE) g_tasks[i].state = TASK_UNUSED;
    }

    context_switch(&g_tasks[prev].esp, nxt->esp);
    // On ne "revient" ICI que lorsque CE thread (prev) est réélu plus
    // tard par une autre commutation — voir le commentaire en tête de
    // fichier. Rien à faire de plus : on retourne simplement à l'appelant
    // (thread_yield ou scheduler_tick), qui sait chacun gérer IF derrière.
}

void thread_yield(void) {
    interrupts_disable();
    schedule();
    interrupts_enable();
}

void thread_exit(void) {
    interrupts_disable();
    g_tasks[g_current].state = TASK_ZOMBIE;
    schedule();
    // Si schedule() n'a rien trouvé d'autre à élire (dernier thread),
    // on ne devrait jamais arriver ici pour de vrai puisqu'il reste
    // toujours au moins la tâche 0 ; par sécurité, on boucle plutôt que
    // de "revenir" dans une tâche zombie sans pile valide.
    for (;;) { __asm__ volatile ("sti; hlt"); }
}

void sched_set_preemption(int enabled) { g_preempt_enabled = enabled; }

// Callback appelé à CHAQUE tick timer (100Hz), donc en contexte IRQ,
// IF=0. Doit rester court. L'EOI du tick en cours a déjà été envoyé par
// interrupt_handler AVANT cet appel (voir isr.c) — c'est ce qui rend
// sûr le fait de potentiellement "sauter" vers une autre pile ici sans
// jamais revenir dérouler la fin normale de cette invocation.
static void scheduler_tick(void) {
    if (!g_preempt_enabled) return;
    if (++g_tick_count < TIME_SLICE_TICKS) return;
    g_tick_count = 0;
    schedule();
}
