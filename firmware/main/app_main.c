#include "board_init.h"
#include "diag.h"
#include "ui_main.h"

void app_main(void) {
    lv_display_t *disp = board_init();

    if (board_lvgl_lock()) {
        diag_init(disp);
        ui_main_init();
        board_lvgl_unlock();
    }

    diag_start_serial_stream();
}
