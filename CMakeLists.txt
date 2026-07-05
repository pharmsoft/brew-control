# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Версия прошивки — попадает в esp_app_get_description()->version и в диагностику.
set(PROJECT_VER "0.5.0")

project(brewcontrol)
