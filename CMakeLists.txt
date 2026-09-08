cmake_minimum_required(VERSION 3.21)

project(phi-adapter-z2m
    VERSION 1.0.0
    DESCRIPTION "Phi Zigbee2MQTT adapter IPC sidecar"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No Qt. The loop, the timers and the strings come from phi-runtime through the
# SDK; MQTT from libmosquitto driven by that loop; JSON from nlohmann, which is
# what phi-core uses.
find_package(nlohmann_json 3.11 REQUIRED)

option(PHI_ADAPTER_Z2M_BUILD_IPC "Build IPC sidecar executable for z2m adapter" ON)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
option(PHI_ADAPTER_Z2M_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available" ON)

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
        message(FATAL_ERROR "libmosquitto not found. Install libmosquitto-dev.")
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
            set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()

    # One concern per translation unit; everything but the client and the
    # instance is pure enough to test without a broker.
    add_library(phi_adapter_z2m_core STATIC
        src/z2m_actions.cpp
        src/z2m_bridge.cpp
        src/z2m_exposes.cpp
        src/z2m_instance.cpp
        src/z2m_json.cpp
        src/z2m_mqtt_client.cpp
        src/z2m_probe.cpp
        src/z2m_schema.cpp
        src/z2m_state.cpp
        src/z2m_topics.cpp
    )
    target_compile_features(phi_adapter_z2m_core PUBLIC cxx_std_20)
    set_target_properties(phi_adapter_z2m_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(phi_adapter_z2m_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(phi_adapter_z2m_core
        PUBLIC
            phi::adapter-sdk
            nlohmann_json::nlohmann_json
            mosquitto::mosquitto
    )

    add_executable(phi_adapter_z2m_ipc src/main.cpp)
    target_link_libraries(phi_adapter_z2m_ipc PRIVATE phi_adapter_z2m_core)

    enable_testing()

    # A device description and the channels it turns into; the schema.
    add_executable(z2m_convert_tests tests/z2m_convert_tests.cpp)
    target_link_libraries(z2m_convert_tests PRIVATE phi_adapter_z2m_core)
    add_test(NAME z2m_convert_tests COMMAND z2m_convert_tests)

    # Topics, the action vocabulary, the press machine, the device table and
    # the health rule.
    add_executable(z2m_behaviour_tests tests/z2m_behaviour_tests.cpp)
    target_link_libraries(z2m_behaviour_tests PRIVATE phi_adapter_z2m_core)
    add_test(NAME z2m_behaviour_tests COMMAND z2m_behaviour_tests)

    # The MQTT client against a broker that behaves and one that does not.
    find_package(Threads REQUIRED)
    add_executable(z2m_mqtt_client_tests tests/z2m_mqtt_client_tests.cpp)
    target_link_libraries(z2m_mqtt_client_tests PRIVATE phi_adapter_z2m_core Threads::Threads)
    add_test(NAME z2m_mqtt_client_tests COMMAND z2m_mqtt_client_tests)

    set_tests_properties(z2m_convert_tests z2m_behaviour_tests z2m_mqtt_client_tests
        PROPERTIES TIMEOUT 120)

    set_target_properties(phi_adapter_z2m_ipc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    add_custom_command(TARGET phi_adapter_z2m_ipc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/z2m-config.json"
            "$<TARGET_FILE_DIR:phi_adapter_z2m_ipc>/z2m-config.json"
    )

    install(TARGETS phi_adapter_z2m_ipc
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
    )
endif()

install(FILES z2m-config.json
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
)
