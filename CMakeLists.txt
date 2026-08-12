# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Reconfigure the ESP application descriptor whenever the release identity
# changes. Without this dependency, an incremental PlatformIO build can retain
# the previous version even after version.txt is bumped.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Problem with IPv6 being disabled and these components.
set(EXCLUDE_COMPONENTS "coap" "asio")

project(alarmdecoder_ad2iot_esp32)

