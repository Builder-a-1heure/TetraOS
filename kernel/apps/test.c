#include "../apps/app.h"
#include "../lib/appcore.h"
#include "../drivers/vesa.h"
#include "../gfx/screen.h"

#define TE_WIN_X        30
#define TE_WIN_Y        20
#define TE_WIN_W        740
#define TE_WIN_H        520

TEX_APP("Test TEX", APPICON_GENERIC, 1, 0, APP_FLAG_DESKTOP | APP_FLAG_SYSTEM, test_run);

void test_run(){
    int app_run = 1;

    app_init();

    WinID main_win = app_new_window("Test TEX - Test Window", TE_WIN_X, TE_WIN_Y,TE_WIN_W, TE_WIN_H);
    BtnID btn_exit = app_new_button(main_win, 150, 100, 150, 100, "exit");

    while(app_run == 1) {
        app_tick();

        if (app_button_touched(btn_exit)) {
            screen_exit_ui();
            break;
        }
    }
}