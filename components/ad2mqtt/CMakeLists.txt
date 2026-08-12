idf_component_register(SRCS "ad2mqtt.cpp"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::espressif__cjson
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::alarmdecoder-api
                    REQUIRES idf::espressif__mqtt
                    INCLUDE_DIRS . ../../main/)
