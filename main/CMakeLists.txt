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
    list(APPEND MAIN_SRCS "wifi_manager.c")
    set(C6_PRIV_REQUIRES esp_wifi esp_netif esp_event)
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
    WHOLE_ARCHIVE
)
