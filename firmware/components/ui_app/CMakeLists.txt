idf_component_register(SRCS "ui_main.cpp" "fonts/custom_font_13_big.c" "fonts/custom_font_14_big.c"
                    INCLUDE_DIRS "." "fonts"
                    REQUIRES lvgl api_clients
                    PRIV_REQUIRES net_app api_clients main esp_adc)