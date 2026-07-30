idf_component_register(SRCS "main.cpp" "battery.cpp" "provisioning.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES app_bsp port_bsp net_app sensor api_clients ui_app lvgl esp_adc nvs_flash esp_wifi esp_netif esp_pm)