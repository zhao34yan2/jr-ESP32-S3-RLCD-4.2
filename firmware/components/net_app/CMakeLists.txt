idf_component_register(SRCS "wifi_app.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif lwip)