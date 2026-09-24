# The application's four worker threads (design 4). Dual-mode: an IDF
# component on the target, a static library on the host.
#
# worker.h is a public dependency here, unlike in the other components:
# the accessors hand out nn20_worker_ctx pointers, so every consumer of
# this header sees that type.

set(WORKERS_SRCS
    src/nn20clock_workers.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${WORKERS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_worker
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_workers STATIC ${WORKERS_SRCS})

target_include_directories(nn20clock_workers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_workers
    PUBLIC nn20clock_platform nn20clock_worker
)

target_compile_options(nn20clock_workers PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
