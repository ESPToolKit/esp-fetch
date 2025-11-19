# Host-side tests are not enabled because ESPFetch depends on ESP-IDF HTTP
# client and FreeRTOS primitives that are unavailable on the default host
# toolchain. Component/PlatformIO tests live under test/test_esp_fetch and
# should be executed on-device.
message(STATUS "ESPFetch: tests are disabled for the host build.")
