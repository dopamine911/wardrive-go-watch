idf_component_register(
    SRCS "wardrive_bridge.c" "display.c" "lcd.c" "lcd_draw.c" "apcount.c" "sdrun.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_wifi esp_event esp_netif nvs_flash esp_timer driver esp_driver_uart esp_driver_usb_serial_jtag esp_driver_tsens
             esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_driver_spi esp_driver_ledc
             app_update spi_flash esp_system heap fatfs sdmmc esp_driver_sdspi
)
# Record the cause of a crash in RTC memory before the stock panic handler runs (reported as WD:CRASH next boot).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
