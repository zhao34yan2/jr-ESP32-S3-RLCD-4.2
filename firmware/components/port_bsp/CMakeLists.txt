idf_component_register(SRCS "display_bsp.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES driver spi_flash esp_lcd)