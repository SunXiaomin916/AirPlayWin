add_executable(airplaywin_tests
    ../src/app/WaveFileReader.cpp
    test_main.cpp
    TestAntiPopStress.cpp
    TestAdaptiveJitterBuffer.cpp
    TestAudioEngine.cpp
    TestAudioEpoch.cpp
    TestAudioRingBuffer.cpp
    TestAudioTransitionGuard.cpp
    TestBufferedTiming.cpp
    TestClickPopDetector.cpp
    TestClockServo.cpp
    TestEndpointLatencyModel.cpp
    TestEndpointCalibrationStore.cpp
    TestGroupCoordinator.cpp
    TestGroupMemberAudioGate.cpp
    TestGroupSyncAnalyzer.cpp
    TestGroupSoakRegression.cpp
    TestGroupTimeline.cpp
    TestLoopbackLatency.cpp
    TestNetworkFaultInjector.cpp
    TestAirPlayControlService.cpp
    TestDiscovery.cpp
    TestDriftResampler.cpp
    TestIocpTcpServer.cpp
    TestIocpUdpReceiver.cpp
    TestPcmL16Decoder.cpp
    TestPtpTiming.cpp
    TestRecovery.cpp
    TestRtpAudioStream.cpp
    TestRtpJitterBuffer.cpp
    TestRtpPacket.cpp
    TestRtpTransportIntegration.cpp
    TestRtspParser.cpp
    TestSdpAndTransport.cpp
    TestSessionManager.cpp
    TestSignalGenerator.cpp
    TestWindowsDiscovery.cpp
    TestWindowsEndpointCalibrationStore.cpp
    TestWindowsAudioStreamIntegration.cpp
    TestWindowsPlatform.cpp
    TestWindowsPtpTimingService.cpp
    TestWindowsRecovery.cpp
    TestWaveFileReader.cpp
)

target_compile_features(airplaywin_tests PRIVATE cxx_std_23)
target_compile_definitions(airplaywin_tests PRIVATE
    AIRPLAYWIN_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(airplaywin_tests PRIVATE airplaywin_core airplaywin_windows)
airplaywin_set_warnings(airplaywin_tests)

add_test(NAME airplaywin_unit_tests COMMAND airplaywin_tests)
add_test(NAME airplaywin_sync_regression COMMAND airplaywin_tests --only GroupSoakRegression)
add_test(NAME airplaywin_antipop_regression COMMAND airplaywin_tests --only AntiPopStress)
add_test(NAME airplaywin_latency_regression COMMAND airplaywin_tests --only LoopbackLatency)
set_tests_properties(airplaywin_sync_regression PROPERTIES LABELS "ci;sync;soak")
set_tests_properties(airplaywin_antipop_regression PROPERTIES LABELS "ci;anti-pop")
set_tests_properties(airplaywin_latency_regression PROPERTIES LABELS "ci;latency")
