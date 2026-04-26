set(MAIN_SRCS
    "main.cpp"
    "config_api.c"
    "coordinator_fallback.c"
    "ld2450_cli.c"
    "nvs_config.c"
    "zigbee_init.c"
    "zigbee_attr_handler.c"
    "sensor_bridge.c"
    "zigbee_signal_handlers.c"
)

if("${IDF_TARGET}" STREQUAL "esp32c6")
    list(APPEND MAIN_SRCS "web_server.c")
    set(C6_PRIV_REQUIRES web_server_base wifi_manager ota_check)
else()
    set(C6_PRIV_REQUIRES "")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        json
        ${C6_PRIV_REQUIRES}
    REQUIRES
        ld2450
        espressif__esp-zigbee-lib
        espressif__esp-zboss-lib
        led_strip
        zigbee_ota
    WHOLE_ARCHIVE
)

if("${IDF_TARGET}" STREQUAL "esp32c6")
    # Web assets embedded in firmware binary. On boot, sync_web_assets() in
    # web_server_base checks the NVS version key and copies to the www SPIFFS
    # partition if the firmware version has changed, then serves from SPIFFS.
    target_add_binary_data(${COMPONENT_LIB} "../web/index.html" TEXT)
    target_add_binary_data(${COMPONENT_LIB} "../web/app.js"     TEXT)
    target_add_binary_data(${COMPONENT_LIB} "../web/style.css"  TEXT)
endif()
