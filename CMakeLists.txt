# English: Main build file - controls everything
cmake_minimum_required(VERSION 3.16.0)

# English: Initialize ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# English: Project name
project(matter_shutter_control)

# Exclude OpenThread and Matter Console components
set(EXCLUDE_COMPONENTS "openthread" "esp_matter_console")

# English: C++17 is required by Matter and Arduino
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)

# English: Fix format warnings for RISC-V
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
