idf_component_register(SRCS "ser2sock.cpp"
                    REQUIRES idf::esp_system
                    REQUIRES idf::lwip
                    REQUIRES idf::espressif__cjson
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::alarmdecoder-api
                    # alarmdecoder_main.h exposes the shared TLS bundle helper.
                    PRIV_REQUIRES mbedtls
                    INCLUDE_DIRS . ../../main/)
