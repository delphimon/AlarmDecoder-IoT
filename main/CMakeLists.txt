idf_component_register(SRCS "ad2_utils.cpp" "alarmdecoder_main.cpp"
                            "device_control.cpp"
                            "ad2_cli_cmd.cpp"
                            "ad2_uart_cli.cpp"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::esp_wifi
                    REQUIRES idf::esp_eth
                    REQUIRES idf::spi_flash
                    REQUIRES idf::espressif__cjson
                    REQUIRES idf::fatfs
                    REQUIRES idf::esp_http_client
                    REQUIRES idf::spiffs
                    REQUIRES idf::alarmdecoder-api
                    REQUIRES idf::otaupdate
                    REQUIRES idf::usdupdate
                    REQUIRES idf::pushover
                    REQUIRES idf::twilio
                    REQUIRES idf::ser2sock
                    REQUIRES idf::network_cli
                    REQUIRES idf::webUI
                    REQUIRES idf::ad2mqtt
                    REQUIRES idf::ftpd
                    )

# SimpleIni 4.26 is header-only and publishes a CMake interface target. Pin the
# exact release commit so native ESP-IDF and PlatformIO builds use one source.
include(FetchContent)
FetchContent_Declare(simpleini
        GIT_REPOSITORY https://github.com/brofield/simpleini.git
        GIT_TAG 877f7357d1fa4232f1f3352e5028f99899210b27
        GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(simpleini)
target_link_libraries(${COMPONENT_LIB} PUBLIC SimpleIni::SimpleIni)
