/* CoCIS Smart Control and Monitoring System - single ESP32, ESP-IDF + FreeRTOS.
 * Three logical terminals run as tasks on one chip: Entrance, Lecture Room 1, Lecture Room 2.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include "app.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/task.h"

sys_state_t G;

static void time_init(void)
{
    setenv("TZ", "EAT-3", 1);                        /* East Africa Time (UTC+3) */
    tzset();
    struct tm t = { .tm_year = 2026 - 1900, .tm_mon = 9, .tm_mday = 28, .tm_hour = 9, .tm_min = 0, .tm_isdst = -1 };
    struct timeval tv = { .tv_sec = mktime(&t) };
    settimeofday(&tv, NULL);                         /* demo start: 28 Oct 2026 09:00 */
}

static void default_config(void)
{
    G.cfg.open_min = 7 * 60;
    G.cfg.close_min = 21 * 60;
    G.cfg.idle_s = 30;
    /* Default administrator: user "admin", PIN 1234 (change with 'passwd'). */
    sha256_hex("cocis:admin:1234", G.cfg.admin_hash);
}

void app_main(void)
{
    memset(&G, 0, sizeof G);
    G.mtx = xSemaphoreCreateMutex();
    default_config();
    time_init();
    io_init();

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO, .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));   /* shared by SD + 2 RFID readers */

    rfid_init();
    storage_init();
    display_init();                                                                 /* LCD at the entrance */

    xTaskCreate(entrance_task, "entrance", 4096, NULL, 5, NULL);                  
    xTaskCreate(room_task, "room1", 3072, (void *)(intptr_t)0, 5, NULL);           
    xTaskCreate(room_task, "room2", 3072, (void *)(intptr_t)1, 5, NULL);           
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);
}
