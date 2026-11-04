idf_component_register(
    SRCS "wardrive_bridge.c" "display.c" "lcd.c" "lcd_draw.c" "apcount.c" "sdrun.c" "netlog.c" "gesture.c" "rtc_pcf.c"
         "memdiag.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_wifi esp_event esp_netif nvs_flash esp_timer driver esp_driver_uart esp_driver_usb_serial_jtag esp_driver_tsens
             esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_driver_spi esp_driver_ledc esp_lcd
             app_update spi_flash esp_system heap fatfs sdmmc esp_driver_sdspi esp_pm
)
# The watch's 410x502 pages and their fonts (lcd_draw.c) only go into ESP32-S3 images: no other chip runs that screen.
if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LCD_WATCH=1)
    # Painting is CPU-bound (docs/memory-soak-results.md, rendering): at -O2 the watch's alert frames paint in 117 ms
    # instead of 162 (-Og, the project default). Pure C with no IDF calls; its renders are byte-identical at both levels.
    set_source_files_properties(lcd_draw.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
# Bench diagnostics: per-function stack frames (.su files next to the objects) for the stack audit.
if(CONFIG_WARDRIVEESP_MEMDIAG)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
endif()
# Record the cause of a crash in RTC memory before the stock panic handler runs (reported as WD:CRASH next boot).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
