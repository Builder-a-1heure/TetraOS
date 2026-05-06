#include "../apps/app.h"
#include "../lib/appcore.h"
#include "../drivers/vesa.h"
#include "../gfx/screen.h"
#include "../shell/shell.h"

#define TE_WIN_X        30
#define TE_WIN_Y        20
#define TE_WIN_W        740
#define TE_WIN_H        520

TEX_APP("Logout/Shutdown", APPICON_SETTINGS, 1, 0, APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, logout_app_run);

void logout_app_run(){
    int app_run = 1;

    app_init();

    WinID main_win = app_new_window("Logout/Shutdown - Screen", TE_WIN_X, TE_WIN_Y,TE_WIN_W, TE_WIN_H);
    BtnID btn_logout = app_new_button(main_win, 150, 100, 150, 100, "Logout");
    BtnID btn_Shutdown = app_new_button(main_win, 150, 200, 150, 100, "Shutdown");
    BtnID btn_Exit = app_new_button(main_win, 150, 300, 150, 100, "Cancel");


    while(app_run == 1) {
        app_tick();

        if (app_button_touched(btn_logout)) {
            dispatch_session("logout","");
            screen_exit_ui();
            break;
        }

        else if (app_button_touched(btn_Shutdown)) {
            dispatch_sys("shutdown","");
            screen_exit_ui();
            break;
        }

        else if (app_button_touched(btn_Exit)) {
            screen_exit_ui();
            break;
        }
    }
}