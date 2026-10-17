"use strict";

/* ============================================================
 * AirPlayWin 前端
 * 与原生端通过 window.chrome.webview.postMessage 通信：
 *   发送: {"id":N,"command":"...","payload":{...}}
 *   接收: {"type":"response","id":N,"ok":bool,"result":...}
 *         {"type":"state","payload":{...}}      每秒推送
 *         {"type":"receiver","payload":{...}}   接收器状态事件
 * 在普通浏览器中打开时自动使用 mock 数据，便于界面开发。
 * ============================================================ */

const hasNativeBridge = typeof window.chrome !== "undefined" &&
                        typeof window.chrome.webview !== "undefined";

let requestCounter = 0;
const pending = new Map();
let state = null;

/* ---------- 桥接 ---------- */

function sendCommand(command, payload) {
  return new Promise((resolve) => {
    const id = ++requestCounter;
    if (!hasNativeBridge) {
      setTimeout(() => resolve(mockResponse(command, payload)), 60);
      return;
    }
    pending.set(id, resolve);
    window.chrome.webview.postMessage(JSON.stringify({ id, command, payload: payload || {} }));
  });
}

function handleNativeMessage(event) {
  let message;
  try { message = typeof event.data === "string" ? JSON.parse(event.data) : event.data; }
  catch { return; }
  if (!message || typeof message.type !== "string") return;
  if (message.type === "response") {
    const resolve = pending.get(message.id);
    if (resolve) { pending.delete(message.id); resolve(message); }
  } else if (message.type === "state") {
    onState(message.payload);
  } else if (message.type === "receiver") {
    onReceiverEvent(message.payload);
  }
}

// WebView2 delivers native messages through chrome.webview, not the DOM
// window message event.
if (hasNativeBridge) {
  window.chrome.webview.addEventListener("message", handleNativeMessage);
}

/* ---------- 模拟数据（浏览器预览用） ---------- */

const mock = {
  running: false,
  settings: {
    deviceName: "书房电脑", outputDeviceId: "", followDefaultDevice: true,
    lowLatency: false, exclusiveAudio: false, strictExclusive: false,
    raopPort: 5000, airplayPort: 7000, classicRaop: true,
    includeVirtualInterfaces: false, experimentalBufferedTiming: false,
    bufferedTimingMilliseconds: 120, experimentalPtpTiming: false,
    ptpInterfaceAddress: "0.0.0.0", endpointCalibrationOffsetMicroseconds: 0,
    volume: 1.0, minimizeToTray: true, diagnosticsIntervalSeconds: 1
  },
  receiver: {
    advertisedName: "书房电脑", sessionCount: 0, receivedRequests: 0,
    rejectedRequests: 0, parseErrors: 0, pairingRequests: 0,
    hasSession: false, senderName: "—", senderAddress: "—", sessionState: "—",
    volumeDb: 0, recording: false, packets: 0, retransmitted: 0, resendRequests: 0,
    lost: 0, late: 0, duplicate: 0, reordered: 0, decoded: 0, concealed: 0,
    decoderErrors: 0, jitterUs: 0, jitterP95Us: 0, targetPackets: 0,
    adaptiveEnabled: false, protocolLatencyUs: 0, receiverLatencyUs: 0,
    servoState: "未启用", driftPpm: 0, rateCorrection: 1,
    underruns: 0, renderedFrames: 0, queueFrames: 0, latencyUs: 0,
    endpointFormat: "Float 32-bit", periodFrames: 0, outputMode: "共享模式",
    lowLatencyActive: false, outputFallback: false, sessionVolumeActive: true,
    sessionVolumeScalar: 1.0,
    transitionState: "已停止", clickPopEvents: 0, clickPopMaxStep: 0,
    invalidNumeric: 0, clipped: 0, dcEvents: 0, deviceSwitches: 0,
    deviceRecoveries: 0, deviceRecoveryFailures: 0, hardResyncs: 0,
    underrunTransitions: 0, safeMutes: 0,
    discoveryInterfaces: 0, registrations: 0, nameConflicts: 0,
    networkChanges: 0, registrationFailures: 0,
    suspendEvents: 0, resumeEvents: 0, recoveryAttempts: 0, recoveryFailures: 0,
    raopActiveConnections: 0, raopAccepted: 0, raopTimeouts: 0, raopErrors: 0
  },
  version: "v1.1.0-preview"
};

let mockTick = 0;

function mockResponse(command, payload) {
  const ok = { ok: true };
  switch (command) {
    case "getState":
      return { ok: true, result: { version: mock.version, running: mock.running, lastError: 0, settings: mock.settings, receiver: mock.receiver } };
    case "start":
      mock.running = true;
      pushMockState();
      return ok;
    case "stop":
      mock.running = false;
      pushMockState();
      return ok;
    case "setConfig":
      Object.assign(mock.settings, payload);
      mock.receiver.advertisedName = mock.settings.deviceName;
      return ok;
    case "listDevices":
      return { ok: true, result: [
        { id: "{realtek}", name: "扬声器 (Realtek(R) Audio)", isDefault: true, state: 1 },
        { id: "{hdmi}", name: "MPG321UX OLED (NVIDIA High Definition Audio)", isDefault: false, state: 1 },
        { id: "{usb}", name: "USB DAC (示例)", isDefault: false, state: 4 }
      ] };
    case "listInterfaces":
      return { ok: true, result: [
        { index: 11, ipv4: "192.168.1.14", name: "以太网 2", description: "Realtek Gaming 2.5GbE", virtual: false }
      ] };
    case "firewallStatus":
      return { ok: true, detail: "模拟状态" };
    case "playTestTone":
      setTimeout(() => toast("测试音播放完成", "ok"), 1500);
      return ok;
    case "getCalibration":
      return { ok: true, offsetUs: 0 };
    case "getAutostart":
      return { enabled: false };
    case "setAutostart":
    case "setVolume":
    case "saveCalibration":
    case "clearCalibration":
    case "openLogsFolder":
      return ok;
    default:
      return ok;
  }
}

function pushMockState() {
  if (mock.running && mockTick % 5 === 0) {
    mock.receiver.packets += 42 + Math.floor(Math.random() * 8);
    mock.receiver.decoded += 42;
    mock.receiver.renderedFrames += 36864;
    mock.receiver.queueFrames = 400 + Math.floor(Math.random() * 60);
    mock.receiver.latencyUs = 23000 + Math.floor(Math.random() * 2000);
    mock.receiver.receiverLatencyUs = 27000 + Math.floor(Math.random() * 2000);
    mock.receiver.jitterUs = 1200 + Math.floor(Math.random() * 400);
    mock.receiver.jitterP95Us = 2400 + Math.floor(Math.random() * 600);
    mock.receiver.transitionState = "播放中";
  } else if (!mock.running) {
    mock.receiver.transitionState = "已停止";
  }
  mockTick++;
  onState({ version: mock.version, running: mock.running, lastError: 0, settings: mock.settings, receiver: mock.receiver });
}

/* ---------- 状态接收 ---------- */

function onState(payload) {
  state = payload;
  if (payload.settings) renderSettings(payload.settings);
  if (payload.receiver) renderReceiver(payload.receiver);
  if (payload.running !== undefined) renderRunning(payload.running);
}

function onReceiverEvent(payload) {
  if (payload.message) {
    toast(payload.message, payload.running ? "ok" : payload.error ? "error" : "warn");
  }
  if (payload.running !== undefined) renderRunning(payload.running);
}

/* ---------- 渲染 ---------- */

const $ = (id) => document.getElementById(id);

function fmtUs(us) { return us >= 1000 ? (us / 1000).toFixed(1) + " ms" : us + " µs"; }
function fmtPct(x) { return (x * 100).toFixed(0) + "%"; }

function renderRunning(running) {
  const btn = $("btn-power");
  const text = $("btn-power-text");
  const pill = $("sidebar-status");
  btn.classList.toggle("on", running);
  text.textContent = running ? "停止接收器" : "启动接收器";
  pill.classList.toggle("running", running);
  pill.innerHTML = running ? '<span class="dot"></span>接收中' : '<span class="dot"></span>未运行';
  // The hero card button mirrors the state: it starts the receiver when
  // stopped and stops it when running.
  const homeBtn = $("home-start");
  homeBtn.textContent = running ? "立即停止" : "立即启动";
  homeBtn.classList.toggle("btn-danger", running);
  homeBtn.classList.toggle("btn-primary", !running);
  const hero = $("hero-icon");
  const title = $("hero-title");
  const sub = $("hero-sub");
  hero.classList.toggle("live", running);
  if (running) {
    hero.textContent = "●";
    title.textContent = "接收器运行中";
    sub.textContent = "在 iPhone / iPad / Mac 的 AirPlay 列表中选择 \"" +
      (state?.receiver?.advertisedName || "此设备") + "\" 即可播放";
  } else {
    hero.textContent = "◌";
    title.textContent = "接收器未运行";
    sub.textContent = "启动后，iPhone / iPad / Mac 即可发现这台电脑";
  }
}

function renderSettings(settings) {
  $("brand-version").textContent = state?.version || "v1.0.2";
  $("about-version").textContent = state?.version || "v1.0.2";
  $("topbar-device").textContent = "设备名：" + settings.deviceName;
  if (document.activeElement !== $("net-name")) $("net-name").value = settings.deviceName;
  $("net-raop-port").value = settings.raopPort;
  $("net-airplay-port").value = settings.airplayPort;
  $("audio-follow-default").checked = settings.followDefaultDevice;
  $("audio-low-latency").checked = settings.lowLatency;
  $("audio-exclusive").checked = settings.exclusiveAudio;
  $("audio-strict-exclusive").checked = settings.strictExclusive;
  $("net-include-virtual").checked = settings.includeVirtualInterfaces;
  $("adv-buffered").checked = settings.experimentalBufferedTiming;
  $("adv-buffered-ms").value = settings.bufferedTimingMilliseconds;
  $("adv-ptp").checked = settings.experimentalPtpTiming;
  $("adv-ptp-if").value = settings.ptpInterfaceAddress;
  $("adv-minimize-tray").checked = settings.minimizeToTray;
  $("adv-diag-interval").value = settings.diagnosticsIntervalSeconds;
  // The volume slider is driven by renderReceiver: it follows the sender's
  // volume while a session is active and the live Windows session volume
  // otherwise, so it must not be overwritten here from the saved setting.
  selectDevice(settings.outputDeviceId, settings.followDefaultDevice);
}

function renderReceiver(r) {
  $("st-sender").textContent = r.senderName;
  $("st-address").textContent = r.senderAddress;
  $("st-state").textContent = r.sessionState;
  $("st-latency").textContent = r.latencyUs ? fmtUs(r.latencyUs) : "—";
  $("st-recv-latency").textContent = r.receiverLatencyUs ? fmtUs(r.receiverLatencyUs) : "—";
  $("st-adv-name").textContent = r.advertisedName || "—";
  $("st-interfaces").textContent = r.discoveryInterfaces || "—";
  $("st-registrations").textContent = r.registrations || "—";
  $("st-conflicts").textContent = r.nameConflicts || "—";

  $("hs-packets").textContent = r.packets.toLocaleString();
  $("hs-lost").textContent = r.lost;
  $("hs-underrun").textContent = r.underruns;
  $("hs-clicks").textContent = r.clickPopEvents;

  $("d-control").textContent = `${r.sessionCount} 会话 · ${r.receivedRequests} 请求`;
  $("d-packets").textContent = `${r.packets}（重传 ${r.retransmitted} / 请求 ${r.resendRequests}）`;
  $("d-loss").textContent = `${r.lost} / ${r.late} / ${r.duplicate} / ${r.reordered}`;
  $("d-decode").textContent = `${r.decoded} / ${r.concealed} / ${r.decoderErrors}`;
  $("d-jitter").textContent = r.jitterP95Us ? fmtUs(r.jitterP95Us) : "—";
  $("d-latency").textContent = `${r.protocolLatencyUs ? fmtUs(r.protocolLatencyUs) : "—"} / ${r.receiverLatencyUs ? fmtUs(r.receiverLatencyUs) : "—"}`;
  $("d-servo").textContent = `${r.servoState} · ${r.driftPpm.toFixed(1)} ppm · ×${r.rateCorrection.toFixed(6)}`;
  $("d-tcp").textContent = `${r.raopActiveConnections}（累计 ${r.raopAccepted}）`;

  $("d-output").textContent = `${r.outputMode} · ${r.endpointFormat}${r.outputFallback ? "（已回退）" : ""}`;
  $("d-queue").textContent = `${r.queueFrames} 帧 · 周期 ${r.periodFrames}`;
  $("d-underrun").textContent = `${r.underruns} · ${r.renderedFrames.toLocaleString()}`;
  $("d-transition").textContent = r.transitionState;
  $("d-clickpop").textContent = `${r.clickPopEvents} · 最大阶跃 ${r.clickPopMaxStep.toFixed(3)}`;
  $("d-numeric").textContent = `${r.invalidNumeric} / ${r.clipped} / ${r.dcEvents}`;
  $("d-device").textContent = `${r.deviceSwitches} · 失败 ${r.deviceRecoveryFailures}`;
  $("d-resync").textContent = `${r.hardResyncs} / ${r.safeMutes}`;

  $("d-power").textContent = `${r.suspendEvents} / ${r.resumeEvents}`;
  $("d-recovery").textContent = `${r.recoveryAttempts} / ${r.recoveryFailures}`;
  $("d-discovery").textContent = `${r.nameConflicts} / ${r.registrationFailures}`;
  $("d-parse").textContent = `${r.parseErrors} / ${r.rejectedRequests}`;

  // Volume slider follows the live value while the receiver runs: the
  // sender's volume during a session, the Windows per-app session volume
  // otherwise. When the receiver is stopped the slider keeps the saved
  // setting instead of being driven by the empty snapshot.
  if (!volumeDragging && state && state.running) {
    let targetGain = null;
    if (r.hasSession && typeof r.volumeDb === "number" && r.volumeDb > -144) {
      targetGain = Math.pow(10, r.volumeDb / 20);
    } else if (typeof r.sessionVolumeScalar === "number") {
      targetGain = r.sessionVolumeScalar;
    }
    if (targetGain !== null) {
      const slider = $("volume-slider");
      const targetPercent = Math.round(targetGain * 100);
      if (Math.abs(targetPercent - Number(slider.value)) >= 1) {
        slider.value = targetPercent;
        $("volume-value").textContent = targetPercent + "%";
      }
    }
  }
}

/* ---------- 设备下拉 ---------- */

let deviceList = [];
let volumeDragging = false;

function selectDevice(id, followDefault) {
  const select = $("audio-device");
  const value = followDefault ? "__default" : id;
  if (select.value !== value) select.value = value;
}

async function refreshDevices() {
  const response = await sendCommand("listDevices", {});
  if (!response.ok) return;
  deviceList = response.result;
  const select = $("audio-device");
  const current = select.value;
  select.innerHTML = "";
  const defaultOption = document.createElement("option");
  defaultOption.value = "__default";
  defaultOption.textContent = "跟随系统默认设备";
  select.appendChild(defaultOption);
  for (const device of deviceList) {
    const option = document.createElement("option");
    option.value = device.id;
    option.textContent = (device.isDefault ? "[默认] " : "") + device.name +
      (device.state === 1 ? "" : "（未连接）");
    select.appendChild(option);
  }
  if (current) select.value = current;
}

/* ---------- 配置应用 ---------- */

let restartPending = false;

function applySettings(patch, live) {
  return sendCommand("setConfig", patch).then(() => {
    if (!live) {
      restartPending = true;
      $("restart-banner").hidden = false;
    }
    if (patch.deviceName) {
      $("topbar-device").textContent = "设备名：" + patch.deviceName;
    }
  });
}

function restartReceiver() {
  return sendCommand("stop", {}).then(() => sendCommand("start", {})).then(() => {
    restartPending = false;
    $("restart-banner").hidden = true;
  });
}

/* ---------- Toast ---------- */

function toast(message, kind) {
  const container = $("toast-container");
  const element = document.createElement("div");
  element.className = "toast " + (kind || "");
  element.textContent = message;
  container.appendChild(element);
  setTimeout(() => {
    element.classList.add("out");
    setTimeout(() => element.remove(), 350);
  }, 4200);
}

/* ---------- 页面导航 ---------- */

const pageTitles = {
  home: "首页", audio: "音频", network: "网络",
  advanced: "高级", diagnostics: "诊断", about: "关于"
};

document.querySelectorAll(".nav-item").forEach((item) => {
  item.addEventListener("click", () => {
    document.querySelectorAll(".nav-item").forEach((i) => i.classList.remove("active"));
    document.querySelectorAll(".page").forEach((p) => p.classList.remove("active"));
    item.classList.add("active");
    $("page-" + item.dataset.page).classList.add("active");
    $("page-title").textContent = pageTitles[item.dataset.page];
  });
});

/* ---------- 事件绑定 ---------- */

$("btn-power").addEventListener("click", () => {
  if (!state) return;
  if (state.running) {
    sendCommand("stop", {});
  } else {
    sendCommand("start", {}).then(() => sendCommand("getState", {}));
  }
});
$("home-start").addEventListener("click", () => {
  if (!state) return;
  if (state.running) {
    sendCommand("stop", {});
  } else {
    sendCommand("start", {}).then(() => sendCommand("getState", {}));
  }
});
$("btn-tray").addEventListener("click", () => {
  sendCommand("hideToTray", {});
  toast("已最小化到系统托盘，接收器继续在后台运行。双击托盘图标可重新打开。", "ok");
});

$("home-tone").addEventListener("click", () => {
  document.querySelector('.nav-item[data-page="audio"]').click();
});

$("volume-slider").addEventListener("pointerdown", () => { volumeDragging = true; });
$("volume-slider").addEventListener("pointerup", () => { volumeDragging = false; });
$("volume-slider").addEventListener("input", (event) => {
  const gain = Number(event.target.value) / 100;
  $("volume-value").textContent = event.target.value + "%";
  sendCommand("setVolume", { gain });
});

$("net-apply-name").addEventListener("click", () => {
  const name = $("net-name").value.trim();
  if (!name) { toast("设备名不能为空", "error"); return; }
  applySettings({ deviceName: name }, true).then(() => toast("设备名已更新，AirPlay 列表将立即刷新", "ok"));
});

$("net-raop-port").addEventListener("change", () => {
  applySettings({ raopPort: Number($("net-raop-port").value) }, false);
});
$("net-airplay-port").addEventListener("change", () => {
  applySettings({ airplayPort: Number($("net-airplay-port").value) }, false);
});

$("audio-device").addEventListener("change", (event) => {
  const value = event.target.value;
  if (value === "__default") {
    applySettings({ outputDeviceId: "", followDefaultDevice: true }, false);
  } else {
    applySettings({ outputDeviceId: value, followDefaultDevice: false }, false);
  }
});
$("audio-refresh").addEventListener("click", refreshDevices);
$("audio-follow-default").addEventListener("change", (event) => {
  if (event.target.checked) {
    applySettings({ outputDeviceId: "", followDefaultDevice: true }, false);
  } else {
    const selected = $("audio-device").value;
    if (selected !== "__default") {
      applySettings({ outputDeviceId: selected, followDefaultDevice: false }, false);
    }
  }
});
$("audio-low-latency").addEventListener("change", (e) => applySettings({ lowLatency: e.target.checked }, false));
$("audio-exclusive").addEventListener("change", (e) => {
  applySettings({ exclusiveAudio: e.target.checked }, false);
  if (!e.target.checked) $("audio-strict-exclusive").checked = false;
});
$("audio-strict-exclusive").addEventListener("change", (e) => applySettings({ strictExclusive: e.target.checked }, false));

$("tone-play").addEventListener("click", async () => {
  const button = $("tone-play");
  button.disabled = true;
  button.textContent = "播放中…";
  const response = await sendCommand("playTestTone", {
    signal: $("tone-signal").value,
    seconds: Number($("tone-seconds").value)
  });
  button.disabled = false;
  button.textContent = "播放";
  if (response.ok) {
    const result = response.result || {};
    $("tone-result").textContent = "✓ " + (result.detail || "播放完成") +
      " 渲染 " + (result.renderedFrames || 0).toLocaleString() + " 帧，缓冲不足 " + (result.underruns || 0) + " 次。";
  } else {
    $("tone-result").textContent = "✗ " + (response.detail || "播放失败");
  }
});

$("fw-status").addEventListener("click", async () => {
  const response = await sendCommand("firewallStatus", {});
  $("fw-result").textContent = (response.ok ? "✓ 规则状态正常。" : "⚠ ") + (response.detail || "");
});
$("fw-install").addEventListener("click", () => {
  sendCommand("installFirewall", {});
  toast("已请求安装防火墙规则。请在系统弹窗中允许。", "warn");
});
$("fw-remove").addEventListener("click", () => {
  sendCommand("removeFirewall", {});
  toast("已请求移除防火墙规则。请在系统弹窗中允许。", "warn");
});

$("net-include-virtual").addEventListener("change", (e) => applySettings({ includeVirtualInterfaces: e.target.checked }, false));

$("adv-buffered").addEventListener("change", (e) => applySettings({ experimentalBufferedTiming: e.target.checked }, false));
$("adv-buffered-ms").addEventListener("change", () => {
  applySettings({ bufferedTimingMilliseconds: Number($("adv-buffered-ms").value) }, false);
});
$("adv-ptp").addEventListener("change", (e) => applySettings({ experimentalPtpTiming: e.target.checked }, false));
$("adv-ptp-if").addEventListener("change", () => {
  applySettings({ ptpInterfaceAddress: $("adv-ptp-if").value.trim() || "0.0.0.0" }, false);
});

$("adv-cal-save").addEventListener("click", async () => {
  const device = state?.settings?.outputDeviceId;
  if (!device) { toast("请先在“音频”页选择一个固定输出设备", "warn"); return; }
  const response = await sendCommand("saveCalibration", {
    deviceId: device, offsetUs: Number($("adv-offset-us").value)
  });
  toast(response.ok ? "✓ 已保存该设备的延迟偏移。" : "✗ " + (response.detail || "保存失败"), response.ok ? "ok" : "error");
});
$("adv-cal-load").addEventListener("click", async () => {
  const device = state?.settings?.outputDeviceId;
  if (!device) { toast("请先在“音频”页选择一个固定输出设备", "warn"); return; }
  const response = await sendCommand("getCalibration", { deviceId: device });
  if (response.ok && response.offsetUs !== undefined) {
    $("adv-offset-us").value = response.offsetUs;
    $("cal-result").textContent = "✓ 当前设备已保存的偏移为 " + response.offsetUs + " µs。";
  } else {
    $("cal-result").textContent = "该设备还没有保存过偏移。";
  }
});
$("adv-cal-clear").addEventListener("click", async () => {
  const device = state?.settings?.outputDeviceId;
  if (!device) { toast("请先在“音频”页选择一个固定输出设备", "warn"); return; }
  const response = await sendCommand("clearCalibration", { deviceId: device });
  toast(response.ok ? "✓ 已清除。" : "✗ 清除失败", response.ok ? "ok" : "error");
});

$("adv-autostart").addEventListener("change", async (e) => {
  const response = await sendCommand("setAutostart", { enabled: e.target.checked });
  toast(response.ok && response.result && response.result.ok
    ? "✓ 开机自启已更新。" : "✗ 无法更新开机自启（可能需要手动配置）",
    response.ok ? "ok" : "error");
});
$("adv-minimize-tray").addEventListener("change", (e) => applySettings({ minimizeToTray: e.target.checked }, true));
$("adv-diag-interval").addEventListener("change", () => {
  applySettings({ diagnosticsIntervalSeconds: Number($("adv-diag-interval").value) }, false);
});

$("restart-now").addEventListener("click", () => {
  restartReceiver().then(() => toast("接收器已重启，新设置生效", "ok"));
});

$("diag-open-logs").addEventListener("click", () => sendCommand("openLogsFolder", {}));
$("about-logs").addEventListener("click", () => sendCommand("openLogsFolder", {}));
$("about-github").addEventListener("click", () => {
  if (hasNativeBridge) {
    window.chrome.webview.postMessage(JSON.stringify({ id: ++requestCounter, command: "openUrl", payload: { url: "https://github.com/SunXiaomin916/AirPlayWin" } }));
  } else {
    window.open("https://github.com/SunXiaomin916/AirPlayWin", "_blank");
  }
});

/* ---------- 自动化桥测试（--test-bridge） ---------- */

window.__runBridgeTest = async function () {
  const results = [];
  try {
    const started = await sendCommand("start", {});
    results.push(["start", !!started.ok]);
    await new Promise((resolve) => setTimeout(resolve, 1500));
    const stateResult = await sendCommand("getState", {});
    results.push(["getState.running",
      !!(stateResult.ok && stateResult.result && stateResult.result.running === true)]);
    results.push(["getState.settings",
      !!(stateResult.ok && stateResult.result && stateResult.result.settings &&
         typeof stateResult.result.settings.deviceName === "string")]);
    const config = await sendCommand("setConfig", { deviceName: "测试设备" });
    results.push(["setConfig", !!config.ok]);
    const volume = await sendCommand("setVolume", { gain: 0.5 });
    results.push(["setVolume", !!volume.ok]);
    await new Promise((resolve) => setTimeout(resolve, 1500));
    const stateAfterVolume = await sendCommand("getState", {});
    const actualVolume = stateAfterVolume.ok && stateAfterVolume.result &&
        stateAfterVolume.result.receiver
        ? stateAfterVolume.result.receiver.sessionVolumeScalar
        : null;
    results.push(["getState.sessionVolume",
      actualVolume !== null && Math.abs(actualVolume - 0.5) < 0.05,
      "actual=" + actualVolume]);
    const devices = await sendCommand("listDevices", {});
    results.push(["listDevices", !!(devices.ok && Array.isArray(devices.result))]);
    const interfaces = await sendCommand("listInterfaces", {});
    results.push(["listInterfaces", !!(interfaces.ok && Array.isArray(interfaces.result))]);
    const calibration = await sendCommand("getCalibration", { deviceId: "test" });
    results.push(["getCalibration", !!calibration.ok]);
    const autostart = await sendCommand("getAutostart", {});
    results.push(["getAutostart", !!autostart.ok]);
    const firewall = await sendCommand("firewallStatus", {});
    results.push(["firewallStatus", typeof firewall.ok === "boolean"]);
    const stopped = await sendCommand("stop", {});
    results.push(["stop", !!stopped.ok]);
    const afterStop = await sendCommand("getState", {});
    results.push(["getState.stopped",
      !!(afterStop.ok && afterStop.result && afterStop.result.running === false)]);
    window.chrome.webview.postMessage(JSON.stringify({
      id: 0, command: "testResult", payload: { results }
    }));
  } catch (error) {
    window.chrome.webview.postMessage(JSON.stringify({
      id: 0, command: "testResult", payload: { error: String(error), results }
    }));
  }
};

/* ---------- 启动 ---------- */

async function boot() {
  refreshDevices();
  if (hasNativeBridge) {
    sendCommand("getAutostart", {}).then((response) => {
      if (response.ok) $("adv-autostart").checked = !!response.result?.enabled;
    });
    sendCommand("getState", {}).then((response) => {
      if (response.ok && response.result) onState(response.result);
    });
  } else {
    onState(mockResponse("getState", {}).result);
    setInterval(pushMockState, 1000);
  }
}

boot();
