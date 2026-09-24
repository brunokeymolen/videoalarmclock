# Header-only request slot pool, shared by the components that post
# async work to a worker. Nothing to compile on either side.

if(ESP_PLATFORM)
    idf_component_register(
        INCLUDE_DIRS "include"
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_reqpool INTERFACE)

target_include_directories(nn20clock_reqpool
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
