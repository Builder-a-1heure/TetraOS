#include "reapfs.h"
#include "screen.h"
#include "ui.h"


/* A synchroniser avec reAPFS.c*/
#define MAX_DIR_ENTRIES 32

void fs_draw_ls(void) {
    const char *cwd = fs_get_cwd();

    // On récupère le répertoire courant (celui où on est réellement)
    int ino = fs_open(cwd, 0);
    if (ino < 0) return;

    // Lire le contenu de ce répertoire
    fs_entry_t entries[MAX_DIR_ENTRIES];
    int count = fs_list_dir(entries, MAX_DIR_ENTRIES);
    if (count < 0) return;

    // Position d’affichage (coin supérieur droit)
    int screen_w = screen_get_width();
    int x = screen_w - 25;
    int y = 0;

    // Nettoyer la zone
    screen_fill_rect(x - 2, y, 25, 14, ' ');

    // En-tête
    print_xy(x, y++, "[FS]");
    print_xy(x, y++, cwd);
    print_xy(x, y++, "----------------");

    // Liste du contenu du répertoire courant
    for (int i = 0; i < count; ++i) {
        char line[40];
        snprintf(line, sizeof(line), "%s%s",
                 entries[i].name,
                 entries[i].is_dir ? "/" : "");
        print_xy(x, y++, line);
    }

    if (count == 0)
        print_xy(x, y++, "(vide)");
}
