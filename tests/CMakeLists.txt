add_executable(airplaywin_tests
    test_main.cpp
    TestAudioEngine.cpp
    TestAudioEpoch.cpp
    TestAudioRingBuffer.cpp
    TestAudioTransitionGuard.cpp
    TestAirPlayControlService.cpp
    TestDiscovery.cpp
    TestIocpTcpServer.cpp
    TestIocpUdpReceiver.cpp
    TestPcmL16Decoder.cpp
    TestRtpAudioStream.cpp
    TestRtpJitterBuffer.cpp
    TestRtpPacket.cpp
    TestRtpTransportIntegration.cpp
    TestRtspParser.cpp
    TestSdpAndTransport.cpp
    TestSessionManager.cpp
    TestSignalGenerator.cpp
    TestWindowsDiscovery.cpp
    TestWindowsPlatform.cpp
)

target_compile_features(airplaywin_tests PRIVATE cxx_std_23)
target_compile_definitions(airplaywin_tests PRIVATE
    AIRPLAYWIN_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(airplaywin_tests PRIVATE airplaywin_core airplaywin_windows)
airplaywin_set_warnings(airplaywin_tests)

add_test(NAME airplaywin_unit_tests COMMAND airplaywin_tests)
