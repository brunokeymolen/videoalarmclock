# The project's time types. Header-only on both sides; it exists to keep
# nn20clock_timer and nn20clock_alarm from including each other.

if(ESP_PLATFORM)
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_time INTERFACE)

target_include_directories(nn20clock_time
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_time INTERFACE nn20clock_platform)
