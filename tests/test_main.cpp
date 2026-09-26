#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

void TestAudioEngine();
void TestAdaptiveJitterBuffer();
void TestAntiPopStress();
void TestAirPlayControlService();
void TestAudioEpoch();
void TestAudioRingBuffer();
void TestAudioTransitionGuard();
void TestBufferedTiming();
void TestClickPopDetector();
void TestClockServo();
void TestEndpointLatencyModel();
void TestEndpointCalibrationStore();
void TestGroupCoordinator();
void TestGroupMemberAudioGate();
void TestGroupSyncAnalyzer();
void TestGroupSoakRegression();
void TestGroupTimeline();
void TestLoopbackLatency();
void TestNetworkFaultInjector();
void TestDiscovery();
void TestDriftResampler();
void TestIocpTcpServer();
void TestIocpUdpReceiver();
void TestPcmL16Decoder();
void TestPtpTiming();
void TestRecovery();
void TestRtpAudioStream();
void TestRtpJitterBuffer();
void TestRtpPacket();
void TestRtpTransportIntegration();
void TestRtspParser();
void TestSdpAndTransport();
void TestSessionManager();
void TestSignalGenerator();
void TestWindowsDiscovery();
void TestWindowsEndpointCalibrationStore();
void TestWindowsAudioStreamIntegration();
void TestWindowsPlatform();
void TestWindowsPtpTimingService();
void TestWindowsRecovery();
void TestWaveFileReader();

namespace {

using TestFunction = void (*)();

struct TestCase final {
    std::string_view name{};
    TestFunction function{nullptr};
};

constexpr std::array<TestCase, 41U> kTests{{
    {"AdaptiveJitterBuffer", &TestAdaptiveJitterBuffer},
    {"AntiPopStress", &TestAntiPopStress},
    {"AudioEpoch", &TestAudioEpoch},
    {"AudioRingBuffer", &TestAudioRingBuffer},
    {"AudioTransitionGuard", &TestAudioTransitionGuard},
    {"BufferedTiming", &TestBufferedTiming},
    {"ClickPopDetector", &TestClickPopDetector},
    {"ClockServo", &TestClockServo},
    {"EndpointLatencyModel", &TestEndpointLatencyModel},
    {"EndpointCalibrationStore", &TestEndpointCalibrationStore},
    {"GroupCoordinator", &TestGroupCoordinator},
    {"GroupMemberAudioGate", &TestGroupMemberAudioGate},
    {"GroupSyncAnalyzer", &TestGroupSyncAnalyzer},
    {"GroupSoakRegression", &TestGroupSoakRegression},
    {"GroupTimeline", &TestGroupTimeline},
    {"LoopbackLatency", &TestLoopbackLatency},
    {"TestSignalGenerator", &TestSignalGenerator},
    {"AudioEngine", &TestAudioEngine},
    {"PcmL16Decoder", &TestPcmL16Decoder},
    {"PtpTiming", &TestPtpTiming},
    {"Recovery", &TestRecovery},
    {"RtpPacket", &TestRtpPacket},
    {"RtpJitterBuffer", &TestRtpJitterBuffer},
    {"RtpAudioStream", &TestRtpAudioStream},
    {"RtpTransportIntegration", &TestRtpTransportIntegration},
    {"RtspParser", &TestRtspParser},
    {"SdpAndTransport", &TestSdpAndTransport},
    {"SessionManager", &TestSessionManager},
    {"AirPlayControlService", &TestAirPlayControlService},
    {"IocpTcpServer", &TestIocpTcpServer},
    {"IocpUdpReceiver", &TestIocpUdpReceiver},
    {"Discovery", &TestDiscovery},
    {"DriftResampler", &TestDriftResampler},
    {"NetworkFaultInjector", &TestNetworkFaultInjector},
    {"WindowsDiscovery", &TestWindowsDiscovery},
    {"WindowsEndpointCalibrationStore", &TestWindowsEndpointCalibrationStore},
    {"WindowsAudioStreamIntegration", &TestWindowsAudioStreamIntegration},
    {"WindowsPlatform", &TestWindowsPlatform},
    {"WindowsPtpTimingService", &TestWindowsPtpTimingService},
    {"WindowsRecovery", &TestWindowsRecovery},
    {"WaveFileReader", &TestWaveFileReader},
}};

[[nodiscard]] bool Run(const std::string_view name, const TestFunction function) {
    try {
        function();
        std::cout << "[PASS] " << name << '\n';
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        return false;
    } catch (...) {
        std::cerr << "[FAIL] " << name << ": unknown exception\n";
        return false;
    }
}

}  // namespace

int main(const int argc, char* argv[]) {
    if (argc == 3 && std::string_view{argv[1]} == "--only") {
        const std::string_view requested{argv[2]};
        for (const auto& test : kTests) {
            if (test.name == requested) {
                return Run(test.name, test.function) ? 0 : 1;
            }
        }
        std::cerr << "Unknown test module: " << requested << '\n';
        return 2;
    }
    if (argc != 1) {
        std::cerr << "Usage: airplaywin_tests [--only <module>]\n";
        return 2;
    }
    int failed = 0;
    for (const auto& test : kTests) {
        failed += Run(test.name, test.function) ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}
