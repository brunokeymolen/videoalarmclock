# A small FTP server (design 12). FIRMWARE ONLY - it is a socket server.
#
# Deliberately knows nothing about clocks, alarms or SD cards: design 12
# asks for a component reusable in other projects, so it is given a set
# of callbacks and never touches storage itself.
#
# It does depend on nn20clock_media, for the path rules alone. Those are
# pure logic with no filesystem in them, and canonicalising a client's
# path is this server's security boundary - a second copy of that code
# so the component could claim to stand alone is exactly the trade that
# gets directory traversal bugs written.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_ftp.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_media
    PRIV_REQUIRES lwip
)
