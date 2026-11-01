idf_component_register(
    SRCS "main.c" "io.c" "storage.c" "access.c" "lighting.c" "rfid.c" "console.c" "sha256.c" "display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart esp_driver_ledc esp_driver_sdspi esp_driver_i2c
                  esp_timer fatfs sdmmc vfs esp_rom)
