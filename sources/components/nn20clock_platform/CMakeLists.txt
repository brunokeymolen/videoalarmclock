# The ESP-IDF / host build seam. Header-only on both sides: on the target
# it forwards to esp_err.h and esp_log.h, on a desktop it shims them.

if(ESP_PLATFORM)
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES log
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_platform INTERFACE)

target_include_directories(nn20clock_platform
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
