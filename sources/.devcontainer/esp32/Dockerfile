# ESP-IDF toolchain for NN20Clock (ESP32-P4 target).
#
# The container owns the toolchain; the host only needs Docker and access
# to the board's USB serial device. Driven from the CLI by tools/idf.sh.
FROM espressif/idf:release-v5.5

# net-tools for network debugging (FTP server, Wi-Fi bring-up later on).
RUN apt-get update && \
    apt-get install -y net-tools && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
