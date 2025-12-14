set(app_sources
    "main.cpp"
    "rollershutter.cpp"
    "rollershutter_driver.cpp"
    "web_ui_handler.cpp"
    "shelly_ble_manager.cpp"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."

    REQUIRES
        # Core & System
        nvs_flash
        esp_matter
        json

        # Networking
        esp_wifi
        esp_netif
        lwip
        esp_http_server

        # Bluetooth (h2zero/NimBLE statt ESP-IDF's bt)
        bt

        # Crypto (für BTHome Decryption)
        mbedtls
        
    PRIV_REQUIRES
        # h2zero/NimBLE-Komponente
        esp-nimble-cpp
        
    WHOLE_ARCHIVE
        esp_netif
        app_update
)

# CRITICAL: Exclude ESP Matter Console (has OpenThread dependencies)
idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_ESP_MATTER_CONSOLE_ENABLE=0" APPEND)
