cmake_minimum_required(VERSION 3.26)

project(AirPlayWin VERSION 0.3.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AirPlayWin requires Windows 11 and Windows native APIs")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

option(AIRPLAYWIN_BUILD_TESTS "Build AirPlayWin unit tests" ON)

add_library(airplaywin_core STATIC
    src/core/audio/AudioEngine.cpp
    src/core/audio/AudioEpoch.cpp
    src/core/audio/AudioRingBuffer.cpp
    src/core/audio/AudioTransitionGuard.cpp
    src/core/audio/TestSignalGenerator.cpp
    src/core/crypto/OpenSessionAuthenticator.cpp
    src/core/discovery/AirPlayServiceRecords.cpp
    src/core/discovery/NetworkInterfacePolicy.cpp
    src/core/protocol/AirPlayControlService.cpp
    src/core/protocol/IncrementalRtspParser.cpp
    src/core/protocol/RtspResponseBuilder.cpp
    src/core/protocol/RtspTypes.cpp
    src/core/session/SessionManager.cpp
)

target_include_directories(airplaywin_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(airplaywin_core PUBLIC cxx_std_23)
airplaywin_set_warnings(airplaywin_core)

add_library(airplaywin_windows STATIC
    src/platform/windows/audio/WasapiAudioOutput.cpp
    src/platform/windows/audio/WindowsAudioEngine.cpp
    src/platform/windows/audio/WindowsAudioDeviceEnumerator.cpp
    src/platform/windows/network/WindowsDiscoveryService.cpp
    src/platform/windows/network/IocpTcpServer.cpp
    src/platform/windows/network/WindowsNetworkInterfaceEnumerator.cpp
    src/platform/windows/timing/QpcClock.cpp
)

target_include_directories(airplaywin_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(airplaywin_windows PUBLIC cxx_std_23)
target_compile_definitions(airplaywin_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(airplaywin_windows
    PUBLIC airplaywin_core
    PRIVATE advapi32 avrt dnsapi iphlpapi ole32 propsys uuid ws2_32
)
airplaywin_set_warnings(airplaywin_windows)

add_executable(AirPlayWin src/app/main.cpp)
target_compile_features(AirPlayWin PRIVATE cxx_std_23)
target_compile_definitions(AirPlayWin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(AirPlayWin PRIVATE airplaywin_core airplaywin_windows)
airplaywin_set_warnings(AirPlayWin)

if(AIRPLAYWIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
