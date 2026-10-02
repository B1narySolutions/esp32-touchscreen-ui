#pragma once
#include <stdbool.h>

// The microSD card (FAT32, MBR) at SD_MOUNT_POINT, over SDSPI on GPIO11 MOSI / GPIO12 SCK /
// GPIO13 MISO, with chip select on the IO expander's EXIO4 (held low while mounted: the card is
// the only device on that SPI bus). Never formats a card; a card that won't mount is reported.
#define SD_MOUNT_POINT "/sdcard"

// Mounts the card. Returns false (and logs why) if there is no card or it isn't FAT. Blocking:
// call from a worker task, never the LVGL task.
bool sd_mount(void);
bool sd_is_mounted(void);
// Why the last mount failed, for the UI ("no card", "not FAT32 (format it as FAT32/MBR)", ...).
const char *sd_status(void);
