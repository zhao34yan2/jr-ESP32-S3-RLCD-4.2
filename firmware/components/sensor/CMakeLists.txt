idf_component_register(SRCS "shtc3.cpp" "pcf85063.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer)