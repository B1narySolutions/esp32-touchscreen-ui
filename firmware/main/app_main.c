#include "board_init.h"
#include "nvs_flash.h"
#include "diag.h"
#include "ui_main.h"
#include "seed_link.h"

void app_main(void) {
    // NVS holds the saved rig and settings (ui_main.c). A layout change from an IDF upgrade or
    // a full partition just means starting fresh.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lv_display_t *disp = board_init();

    if (board_lvgl_lock()) {
        diag_init(disp);
        ui_main_init();
        board_lvgl_unlock();
    }

    // After ui_main_init(): rig_state now holds the restored rig, which the first snapshot carries.
    seed_link_init();
    diag_start_serial_stream();
}
