#include "sd_storage.h"

#include <stdio.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io_expander.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define PIN_MOSI GPIO_NUM_11
#define PIN_SCK  GPIO_NUM_12
#define PIN_MISO GPIO_NUM_13

static sdmmc_card_t *s_card;
static const char *s_status = "not mounted yet";

static esp_err_t try_mount(sdmmc_host_t *host) {
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host->slot;
    slot.gpio_cs = SDSPI_SLOT_NO_CS; // CS is EXIO4 on the IO expander, held low by sd_mount()
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, // never erase someone's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, host, &slot, &mount, &s_card);
}

bool sd_mount(void) {
    if (s_card) return true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_status = "SPI bus init failed";
        ESP_LOGE(TAG, "%s: %s", s_status, esp_err_to_name(err));
        return false;
    }

    // Select the card for the whole time it is mounted. If the first attempt fails, give the
    // card a deselected pause (some cards want CS high before entering SPI mode) and retry once.
    io_expander_write(EXIO_SD_CS, 0);
    err = try_mount(&host);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        io_expander_write(EXIO_SD_CS, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        io_expander_write(EXIO_SD_CS, 0);
        err = try_mount(&host);
    }
    if (err != ESP_OK) {
        io_expander_write(EXIO_SD_CS, 1);
        spi_bus_free(host.slot); // no card: give the bus's memory back
        s_status = err == ESP_FAIL ? "card not FAT32 (format it as FAT32 with an MBR partition table)"
                                   : "no card, or the card doesn't answer";
        ESP_LOGW(TAG, "mount failed (%s): %s", esp_err_to_name(err), s_status);
        return false;
    }
    s_status = "mounted";
    ESP_LOGI(TAG, "mounted %s: %s, %llu MB", SD_MOUNT_POINT, s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return true;
}

bool sd_is_mounted(void) { return s_card != NULL; }
const char *sd_status(void) { return s_status; }
