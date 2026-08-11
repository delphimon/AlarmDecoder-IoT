idf_component_register(SRCS "ad2_utils.cpp" "alarmdecoder_main.cpp"
                            "ser2sock_endpoint.cpp"
                            "device_control.cpp"
                            "ad2_cli_cmd.cpp"
                            "ad2_uart_cli.cpp"
                    REQUIRES idf::esp-tls
                    REQUIRES idf::esp_wifi
                    REQUIRES idf::bt
                    REQUIRES idf::esp_eth
                    REQUIRES idf::spi_flash
                    REQUIRES idf::json
                    REQUIRES idf::fatfs
		    REQUIRES idf::stsdk
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

set(STDK_IOT_CORE_USE_DEFINED_CONFIG "y")


# Fetch SimpleIni lib and as as include path.
#  Todo: Only if not already downloaded.
include(FetchContent) # once in the project to include the module
FetchContent_Declare(simpleini GIT_REPOSITORY https://github.com/brofield/simpleini.git)
FetchContent_MakeAvailable(simpleini)
include_directories(${simpleini_SOURCE_DIR})

if(CONFIG_STDK_IOT_CORE)
        file(TO_CMAKE_PATH "$ENV{STDK_CORE_PATH}" STDK_CORE_PATH_CMAKE)
        add_subdirectory(${STDK_CORE_PATH_CMAKE} iotcore)
        target_link_libraries(${COMPONENT_LIB} PUBLIC iotcore)
        spiffs_create_partition_image(spiffs ../data-smartthings FLASH_IN_PROJECT)
endif()
