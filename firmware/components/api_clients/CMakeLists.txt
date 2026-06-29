idf_component_register(SRCS "api_clients.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_client json net_app esp-tls
                    PRIV_REQUIRES main)