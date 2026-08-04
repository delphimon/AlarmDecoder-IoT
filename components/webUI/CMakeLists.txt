idf_component_register(SRCS "webUI.cpp" "TinyTemplateEngine.cpp" "TinyTemplateEngineFileReader.cpp"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::json
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::alarmdecoder-api
                    REQUIRES idf::esp_http_server
                    REQUIRES idf::esp_https_server
                    REQUIRES idf::esp_app_format
                    REQUIRES idf::fatfs
                    REQUIRES idf::spiffs
                    REQUIRES idf::usdupdate
                    INCLUDE_DIRS . ../../main/)
project(webUI)
