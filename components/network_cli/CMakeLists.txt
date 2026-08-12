idf_component_register(SRCS "network_cli.cpp"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::espressif__cjson
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::alarmdecoder-api
                    INCLUDE_DIRS . ../../main/)
