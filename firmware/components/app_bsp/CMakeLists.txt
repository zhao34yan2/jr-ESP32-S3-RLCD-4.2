idf_component_register(SRCS "lvgl_bsp.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl port_bsp esp_timer)