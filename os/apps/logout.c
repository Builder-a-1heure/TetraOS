#include "../apps/app.h"
#include "../lib/appcore.h"
#include "../drivers/vesa.h"
#include "../gfx/screen.h"
#include "../shell/shell.h"
#include "../ui/session.h"

#define TE_WIN_X        30
#define TE_WIN_Y        20
#define TE_WIN_W        420
#define TE_WIN_H        220

TEX_APP("Logout/Shutdown", APPICON_SETTINGS, 1, 0, APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, logout_app_run);

void logout_app_run(void) {
    app_init();

    WinID win      = app_new_window("Deconnexion", TE_WIN_X, TE_WIN_Y, TE_WIN_W, TE_WIN_H);
    BtnID btn_logout   = app_new_button(win, 30, 50, 160, 50, "Deconnecter");
    BtnID btn_shutdown = app_new_button(win, 30, 120, 160, 50, "Eteindre");
    BtnID btn_cancel   = app_new_button(win, 220, 50, 160, 50, "Annuler");

    while (1) {
        app_tick();

        if (app_button_touched(btn_cancel)) {
            // Fermeture propre : appcore libere la fenetre, desktop_run reprend
            app_close_window(win);
            return;
        }

        if (app_button_touched(btn_logout)) {

            dispatch_session("logout", "");

            app_close_window(win);
            // session_logout() pose logged_in=0.
            // desktop_run() sortira de sa boucle au prochain tour et
            // appellera screen_exit_ui() une seule fois — pas de double flush.
            
            return;
        }

        if (app_button_touched(btn_shutdown)) {
            dispatch_sys("shutdown", "");
            app_close_window(win);
            return;
        }
    }
}
