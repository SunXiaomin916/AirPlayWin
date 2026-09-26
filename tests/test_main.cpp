#include <exception>
#include <iostream>
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
void TestLoopbackLatency();
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
void TestWindowsAudioStreamIntegration();
void TestWindowsPlatform();
void TestWindowsPtpTimingService();
void TestWindowsRecovery();
void TestWaveFileReader();

namespace {

using TestFunction = void (*)();

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

int main() {
    int failed = 0;
    failed += Run("AdaptiveJitterBuffer", &TestAdaptiveJitterBuffer) ? 0 : 1;
    failed += Run("AntiPopStress", &TestAntiPopStress) ? 0 : 1;
    failed += Run("AudioEpoch", &TestAudioEpoch) ? 0 : 1;
    failed += Run("AudioRingBuffer", &TestAudioRingBuffer) ? 0 : 1;
    failed += Run("AudioTransitionGuard", &TestAudioTransitionGuard) ? 0 : 1;
    failed += Run("BufferedTiming", &TestBufferedTiming) ? 0 : 1;
    failed += Run("ClickPopDetector", &TestClickPopDetector) ? 0 : 1;
    failed += Run("ClockServo", &TestClockServo) ? 0 : 1;
    failed += Run("EndpointLatencyModel", &TestEndpointLatencyModel) ? 0 : 1;
    failed += Run("LoopbackLatency", &TestLoopbackLatency) ? 0 : 1;
    failed += Run("TestSignalGenerator", &TestSignalGenerator) ? 0 : 1;
    failed += Run("AudioEngine", &TestAudioEngine) ? 0 : 1;
    failed += Run("PcmL16Decoder", &TestPcmL16Decoder) ? 0 : 1;
    failed += Run("PtpTiming", &TestPtpTiming) ? 0 : 1;
    failed += Run("Recovery", &TestRecovery) ? 0 : 1;
    failed += Run("RtpPacket", &TestRtpPacket) ? 0 : 1;
    failed += Run("RtpJitterBuffer", &TestRtpJitterBuffer) ? 0 : 1;
    failed += Run("RtpAudioStream", &TestRtpAudioStream) ? 0 : 1;
    failed += Run("RtpTransportIntegration", &TestRtpTransportIntegration) ? 0 : 1;
    failed += Run("RtspParser", &TestRtspParser) ? 0 : 1;
    failed += Run("SdpAndTransport", &TestSdpAndTransport) ? 0 : 1;
    failed += Run("SessionManager", &TestSessionManager) ? 0 : 1;
    failed += Run("AirPlayControlService", &TestAirPlayControlService) ? 0 : 1;
    failed += Run("IocpTcpServer", &TestIocpTcpServer) ? 0 : 1;
    failed += Run("IocpUdpReceiver", &TestIocpUdpReceiver) ? 0 : 1;
    failed += Run("Discovery", &TestDiscovery) ? 0 : 1;
    failed += Run("DriftResampler", &TestDriftResampler) ? 0 : 1;
    failed += Run("WindowsDiscovery", &TestWindowsDiscovery) ? 0 : 1;
    failed += Run("WindowsAudioStreamIntegration", &TestWindowsAudioStreamIntegration) ? 0 : 1;
    failed += Run("WindowsPlatform", &TestWindowsPlatform) ? 0 : 1;
    failed += Run("WindowsPtpTimingService", &TestWindowsPtpTimingService) ? 0 : 1;
    failed += Run("WindowsRecovery", &TestWindowsRecovery) ? 0 : 1;
    failed += Run("WaveFileReader", &TestWaveFileReader) ? 0 : 1;
    return failed == 0 ? 0 : 1;
}
