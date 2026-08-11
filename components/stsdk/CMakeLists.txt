if(DEFINED ENV{STDK_CORE_PATH})
file(TO_CMAKE_PATH "$ENV{STDK_CORE_PATH}" STDK_CORE_PATH_CMAKE)
message(AUTHOR_WARNING "**** Registering AlarmDecoder idf component 'stsdk'.")
idf_component_register(SRCS "stsdk_main.cpp"
                            "caps_battery.c"
                            "caps_powerSource.c"
                            "caps_button.c"
                            "caps_carbonMonoxideDetector.c"
                            "caps_contactSensor.c"
                            "caps_momentary.c"
                            "caps_refresh.c"
                            "caps_securitySystem.c"
                            "caps_smokeDetector.c"
                            "caps_switch.c"
                            "caps_tamperAlert.c"
                    EMBED_FILES "onboarding_config.json"
                    REQUIRES idf::otaupdate idf::driver idf::nvs_flash idf::esp_netif
                    INCLUDE_DIRS . ../../main/ ${STDK_CORE_PATH_CMAKE}/src/include/)
project(stsdk)
else()
message(AUTHOR_WARNING "**** Not currently possible to build 'stsdk' component outside of the STSDK build.py tool.")
endif()
