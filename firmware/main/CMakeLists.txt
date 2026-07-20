idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES app_bsp port_bsp net_app sensor api_clients ui_app lvgl esp_adc wifi_provisioning)