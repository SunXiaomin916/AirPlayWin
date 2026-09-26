cmake_minimum_required(VERSION 3.26)

project(AirPlayWin VERSION 0.12.0 LANGUAGES CXX)

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
    src/core/audio/ClickPopDetector.cpp
    src/core/audio/DriftResampler.cpp
    src/core/audio/LoopbackLatencyAnalyzer.cpp
    src/core/audio/PcmL16Decoder.cpp
    src/core/audio/TestSignalGenerator.cpp
    src/core/crypto/OpenSessionAuthenticator.cpp
    src/core/discovery/AirPlayServiceRecords.cpp
    src/core/discovery/NetworkInterfacePolicy.cpp
    src/core/group/EndpointCalibrationStore.cpp
    src/core/group/GroupCoordinator.cpp
    src/core/group/GroupMemberAudioGate.cpp
    src/core/group/GroupSyncAnalyzer.cpp
    src/core/group/GroupTypes.cpp
    src/core/lifecycle/RecoveryCoordinator.cpp
    src/core/protocol/AirPlayControlService.cpp
    src/core/protocol/IncrementalRtspParser.cpp
    src/core/protocol/RtspResponseBuilder.cpp
    src/core/protocol/RtpInfo.cpp
    src/core/protocol/RtspTransport.cpp
    src/core/protocol/RtspTypes.cpp
    src/core/protocol/SdpAudioParser.cpp
    src/core/session/SessionManager.cpp
    src/core/timing/BufferedRtpTimingEngine.cpp
    src/core/timing/ClockServo.cpp
    src/core/timing/DisciplinedRtpTimingEngine.cpp
    src/core/timing/PtpClockDomain.cpp
    src/core/timing/PtpPacket.cpp
    src/core/timing/RtpPtpPhaseTimeline.cpp
    src/core/transport/RtpAudioStream.cpp
    src/core/transport/RtpJitterBuffer.cpp
    src/core/transport/NetworkFaultInjector.cpp
    src/core/transport/RtpPacket.cpp
)

target_include_directories(airplaywin_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(airplaywin_core PUBLIC cxx_std_23)
airplaywin_set_warnings(airplaywin_core)

add_library(airplaywin_windows STATIC
    src/platform/windows/audio/EndpointLatencyModel.cpp
    src/platform/windows/audio/WasapiAudioOutput.cpp
    src/platform/windows/audio/WindowsAudioEngine.cpp
    src/platform/windows/audio/WindowsAudioStreamSink.cpp
    src/platform/windows/audio/WindowsAudioDeviceEnumerator.cpp
    src/platform/windows/audio/WindowsEndpointCalibrationStore.cpp
    src/platform/windows/network/WindowsDiscoveryService.cpp
    src/platform/windows/network/IocpTcpServer.cpp
    src/platform/windows/network/IocpUdpReceiver.cpp
    src/platform/windows/network/WindowsNetworkInterfaceEnumerator.cpp
    src/platform/windows/network/WindowsRtpTransportController.cpp
    src/platform/windows/system/SingleInstanceGuard.cpp
    src/platform/windows/system/WindowsFirewallManager.cpp
    src/platform/windows/system/WindowsPowerEventMonitor.cpp
    src/platform/windows/timing/QpcClock.cpp
    src/platform/windows/timing/WindowsPtpTimingService.cpp
)

target_include_directories(airplaywin_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(airplaywin_windows PUBLIC cxx_std_23)
target_compile_definitions(airplaywin_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(airplaywin_windows
    PUBLIC airplaywin_core
    PRIVATE advapi32 avrt dnsapi iphlpapi ole32 oleaut32 powrprof propsys uuid ws2_32
)
airplaywin_set_warnings(airplaywin_windows)

add_executable(AirPlayWin
    src/app/main.cpp
    src/app/WaveFileReader.cpp
)
target_compile_features(AirPlayWin PRIVATE cxx_std_23)
target_compile_definitions(AirPlayWin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(AirPlayWin PRIVATE airplaywin_core airplaywin_windows)
airplaywin_set_warnings(AirPlayWin)

if(AIRPLAYWIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    install(TARGETS airplaywin_tests RUNTIME DESTINATION tools)
endif()

install(TARGETS AirPlayWin RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION .)
install(DIRECTORY installer/ DESTINATION installer)
install(FILES
    tools/Run-S12Regression.ps1
    DESTINATION tools
)
install(FILES
    tools/packet_replay/rtp_l16_replay.ps1
    tools/packet_replay/README.md
    DESTINATION tools/packet_replay
)
install(FILES docs/phase-12.md DESTINATION docs)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "AirPlayWin")
set(CPACK_PACKAGE_VENDOR "AirPlayWin")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "AirPlayWin-${PROJECT_VERSION}-windows-x64")
include(CPack)
