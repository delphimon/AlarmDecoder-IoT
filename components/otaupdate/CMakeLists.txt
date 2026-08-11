if(DEFINED AD2IOT_OTA_PUBLIC_KEY_FILE)
    set(OTA_PUBLIC_KEY_FILE "${AD2IOT_OTA_PUBLIC_KEY_FILE}")
elseif(DEFINED ENV{AD2IOT_OTA_PUBLIC_KEY_FILE})
    set(OTA_PUBLIC_KEY_FILE "$ENV{AD2IOT_OTA_PUBLIC_KEY_FILE}")
else()
    set(OTA_PUBLIC_KEY_FILE "${CMAKE_CURRENT_LIST_DIR}/firmware_signature_public_key.pem")
endif()

if(NOT EXISTS "${OTA_PUBLIC_KEY_FILE}")
    message(FATAL_ERROR "AD2IOT OTA public key does not exist: ${OTA_PUBLIC_KEY_FILE}")
endif()

# Keep the embedded symbol stable even when a caller supplies an external key.
configure_file("${OTA_PUBLIC_KEY_FILE}"
               "${CMAKE_CURRENT_BINARY_DIR}/firmware_signature_public_key.pem"
               COPYONLY)

idf_component_register(SRCS "ota_util.cpp"
                    REQUIRES idf::nvs_flash
                    REQUIRES idf::alarmdecoder-api
                    REQUIRES idf::app_update
                    REQUIRES idf::esp-tls
                    REQUIRES idf::esp_https_ota
                    REQUIRES idf::json
                    INCLUDE_DIRS . ../../main/
                    EMBED_TXTFILES "${CMAKE_CURRENT_BINARY_DIR}/firmware_signature_public_key.pem" ${EXTRA_EMBED})
project(otaupdate)
