cmake_minimum_required(VERSION 3.21)

project(phi-adapter-z2m
    VERSION 1.0.0
    DESCRIPTION "Phi Zigbee2MQTT adapter IPC sidecar"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

option(PHI_ADAPTER_Z2M_BUILD_IPC
    "Build IPC sidecar executable for z2m adapter"
    ON
)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
set(PHI_ADAPTER_SDK_QT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk-qt" CACHE PATH
    "Path to local phi-adapter-sdk-qt checkout. If not found, find_package(phi-adapter-sdk-qt) is used."
)
option(PHI_ADAPTER_Z2M_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available"
    ON
)

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

if(PHI_ADAPTER_Z2M_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_Z2M_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()
    if(NOT TARGET phi::adapter-sdk-qt)
        if(PHI_ADAPTER_Z2M_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_QT_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk-qt from: ${PHI_ADAPTER_SDK_QT_SOURCE_DIR}")
            add_subdirectory("${PHI_ADAPTER_SDK_QT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk-qt")
        else()
            message(STATUS "Using installed phi-adapter-sdk-qt via find_package")
            find_package(phi-adapter-sdk-qt CONFIG REQUIRED)
        endif()
    endif()
    if(NOT TARGET phi::adapter-sdk-qt)
        message(FATAL_ERROR "phi::adapter-sdk target phi::adapter-sdk-qt not found")
    endif()

    add_executable(phi_adapter_z2m_ipc
        src/main.cpp
        src/adapterinterface.h
        src/adapterconfig.h
        src/channel.h
        src/color.h
        src/device.h
        src/group.h
        src/room.h
        src/scene.h
        src/types.h
        src/z2m_runtime_convert.cpp
        src/z2m_runtime_convert.h
        src/z2m_schema.cpp
        src/z2m_schema.h
        src/z2m_sidecar.cpp
        src/z2m_sidecar.h
        src/z2madapter.cpp
        src/z2madapter.h
        src/mqtt/mqttclient.cpp
        src/mqtt/mqttclient.h
    )

    target_compile_features(phi_adapter_z2m_ipc PRIVATE cxx_std_20)

    target_include_directories(phi_adapter_z2m_ipc
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt
    )

        target_link_libraries(phi_adapter_z2m_ipc
            PRIVATE
                Qt6::Core
                Qt6::Network
                phi::adapter-sdk
                phi::adapter-sdk-qt
                mosquitto::mosquitto
        )

    set_target_properties(phi_adapter_z2m_ipc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    add_custom_command(TARGET phi_adapter_z2m_ipc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/z2m-config.json"
            "$<TARGET_FILE_DIR:phi_adapter_z2m_ipc>/z2m-config.json"
    )

    install(TARGETS phi_adapter_z2m_ipc
        RUNTIME DESTINATION lib/phi/plugins/adapters
    )
endif()

install(FILES z2m-config.json
    DESTINATION lib/phi/plugins/adapters
)
