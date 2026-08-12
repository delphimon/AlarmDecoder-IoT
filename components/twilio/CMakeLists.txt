idf_component_register(SRCS "twilio.cpp" "lib/fmt/src/format.cc"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::json
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::alarmdecoder-api
                    INCLUDE_DIRS . ../../main/ lib/fmt/include)
