cmake_minimum_required(VERSION 3.21)

project(phi-adapter-z2m
    VERSION 1.0.0
    DESCRIPTION "Phi Zigbee2MQTT adapter plugin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

set(PHI_ADAPTER_API_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-api" CACHE PATH
    "Path to local phi-adapter-api checkout. If not found, find_package(phi-adapter-api) is used."
)

if(EXISTS "${PHI_ADAPTER_API_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local phi-adapter-api from: ${PHI_ADAPTER_API_SOURCE_DIR}")
    add_subdirectory("${PHI_ADAPTER_API_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-api")
    set(PHI_ADAPTER_API_INCLUDE_DIR "${PHI_ADAPTER_API_SOURCE_DIR}/include/phi/adapter/api")
else()
    message(STATUS "Using installed phi-adapter-api via find_package")
    find_package(phi-adapter-api CONFIG REQUIRED)
    find_path(PHI_ADAPTER_API_INCLUDE_DIR
        NAMES adapterinterface.h
        PATH_SUFFIXES phi/adapter/api
    )
endif()

if(NOT TARGET phi::adapter-api)
    message(FATAL_ERROR "phi-adapter-api target phi::adapter-api not found")
endif()

if(NOT PHI_ADAPTER_API_INCLUDE_DIR)
    message(FATAL_ERROR "Could not resolve phi-adapter-api header include directory")
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO IMPORTED_TARGET libmosquitto)
endif()

if(TARGET PkgConfig::MOSQUITTO)
    add_library(mosquitto::mosquitto ALIAS PkgConfig::MOSQUITTO)
else()
    find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
    find_library(MOSQUITTO_LIBRARY mosquitto)
    if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
        message(FATAL_ERROR "libmosquitto not found. Install libmosquitto-dev (Debian) or mosquitto (Homebrew).")
    endif()
    add_library(mosquitto::mosquitto UNKNOWN IMPORTED)
    set_target_properties(mosquitto::mosquitto PROPERTIES
        IMPORTED_LOCATION "${MOSQUITTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MOSQUITTO_INCLUDE_DIR}"
    )
endif()

add_library(phi_adapter_z2m MODULE
    src/z2madapter.cpp
    src/z2madapter.h
    src/z2madapterfactory.cpp
    src/z2madapterfactory.h
    src/mqtt/mqttclient.cpp
    src/mqtt/mqttclient.h
    ${PHI_ADAPTER_API_INCLUDE_DIR}/adapterinterface.h
    ${PHI_ADAPTER_API_INCLUDE_DIR}/adapterfactory.h
)

target_include_directories(phi_adapter_z2m
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt
)

target_link_libraries(phi_adapter_z2m
    PRIVATE
        Qt6::Core
        Qt6::Network
        phi::adapter-api
        mosquitto::mosquitto
)

install(TARGETS phi_adapter_z2m
    LIBRARY DESTINATION lib/phi/plugins/adapters
)
