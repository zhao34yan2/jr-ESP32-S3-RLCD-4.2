idf_component_register(SRCS "ui_main.cpp" "ui_dashboard.cpp" "ui_sysinfo.cpp" "ui_common.cpp" "fonts/custom_font_13_big.c" "fonts/custom_font_14_big.c" "fonts/custom_font_16_big.c" "fonts/font_num_28.c" "fonts/font_hud_28.c" "fonts/weather_icons.c"
                    INCLUDE_DIRS "." "fonts"
                    REQUIRES lvgl api_clients
                    PRIV_REQUIRES net_app api_clients main esp_adc esp_hw_support)