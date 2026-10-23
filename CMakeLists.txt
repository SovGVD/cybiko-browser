# cybiko-browser: an ESP-IDF component when built by ESP-IDF (put this repo,
# or a link to it, in a components folder as "cy_browser"), and a plain
# library otherwise (add_subdirectory; the PC test tool is built by the
# Makefile).
set(CYB_SOURCES
    src/cy_browser.c src/cyb_font.c src/cyb_html.c src/cyb_image.c
    src/cyb_layout.c src/cyb_text.c src/cyb_url.c third_party/tjpgd/tjpgd.c
    gate/cy_deflate.c gate/cy_gate.c gate/cy_page.c gate/cy_wml.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${CYB_SOURCES} port/esp32/cyb_net_esp.c
        INCLUDE_DIRS include gate port/esp32
        PRIV_INCLUDE_DIRS src third_party/tjpgd
        PRIV_REQUIRES esp_http_client esp_wifi esp_netif esp_event nvs_flash mbedtls)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(cybiko_browser C)
add_library(cy_browser STATIC ${CYB_SOURCES})
target_include_directories(cy_browser PUBLIC include gate PRIVATE src third_party/tjpgd)
target_compile_definitions(cy_browser PRIVATE _GNU_SOURCE)
set_target_properties(cy_browser PROPERTIES C_STANDARD 11)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cy_browser PRIVATE -Wno-format-truncation)
endif()
if(NOT WIN32)
    add_library(cy_browser_curl STATIC host/cyb_net_curl.c)
    target_include_directories(cy_browser_curl PUBLIC host)
    target_link_libraries(cy_browser_curl PUBLIC cy_browser)
endif()
