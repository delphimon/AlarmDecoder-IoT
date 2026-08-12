(function () {
  "use strict";

  const byId = id => document.getElementById(id);
  const query = new URLSearchParams(window.location.search);
  const clampSlot = (name, maximum) => {
    const value = Number.parseInt(query.get(name) || "0", 10);
    return Number.isInteger(value) && value >= 0 && value <= maximum ? value : 0;
  };

  const partID = clampSlot("partID", 8);
  const codeID = clampSlot("codeID", 128);
  const wsHost = query.get("wsHost");
  byId("partitionLabel").textContent = String(partID);

  const app = {
    socket: null,
    connected: false,
    reconnectDelay: 1000,
    reconnectTimer: null,
    state: null,
    history: [],
    system: null,
    firmware: null,
    diagnosticsLoaded: false,
    diagnosticsLoading: false,
    requestQueue: Promise.resolve(),
    serverUptime: 0,
    keypadMask: "",
    keypadClearTimer: null,
    toastTimer: null
  };

  const modeFor = state => {
    if (!state || state.last_alpha_message === "Unknown") return "unknown";
    if (state.alarm_sounding || state.fire_alarm) return "alarm";
    if (state.armed_away) return "armed-away";
    if (state.armed_stay) return "armed-stay";
    if (state.ready) return "ready";
    return "not-ready";
  };

  const modeDetails = mode => ({
    "alarm": { hero: "alarm", label: "Alarm active", glyph: "!" },
    "armed-away": { hero: "armed", label: "Armed away", glyph: "↗" },
    "armed-stay": { hero: "armed", label: "Armed stay", glyph: "⌂" },
    "ready": { hero: "ready", label: "Ready to arm", glyph: "✓" },
    "not-ready": { hero: "not-ready", label: "Not ready", glyph: "·" },
    "unknown": { hero: "unknown", label: "Waiting for panel", glyph: "?" }
  }[mode]);

  function setConnection(status) {
    const badge = byId("connectionBadge");
    badge.className = "connection " + status;
    badge.querySelector("b").textContent = status === "online" ? "Live" : status === "connecting" ? "Connecting" : "Offline";
  }

  function showToast(message, isError) {
    const toast = byId("toast");
    toast.textContent = message;
    toast.className = "toast visible" + (isError ? " error" : "");
    clearTimeout(app.toastTimer);
    app.toastTimer = setTimeout(() => { toast.className = "toast"; }, 2800);
  }

  function setIndicator(id, text, tone) {
    const value = byId(id);
    value.textContent = text;
    value.parentElement.className = "indicator" + (tone ? " " + tone : "");
  }

  function apiURL(path) {
    if (!wsHost) return path;
    const base = /^wss?:\/\//i.test(wsHost) ? wsHost.replace(/^ws/i, "http") :
      (window.location.protocol === "https:" ? "https://" : "http://") + wsHost;
    return base.replace(/\/$/, "") + path;
  }

  // HTTPS is deliberately limited to two TLS sessions: one WebSocket and one
  // HTTP request. Keep the complete request, including body consumption, in a
  // single queue so browsers cannot open competing REST connections.
  function queueRequest(operation) {
    const request = app.requestQueue.then(operation, operation);
    app.requestQueue = request.then(() => undefined, () => undefined);
    return request;
  }

  function formatBytes(value) {
    const bytes = Number(value) || 0;
    if (!bytes) return "—";
    const units = ["B", "KiB", "MiB", "GiB"];
    let amount = bytes;
    let unit = 0;
    while (amount >= 1024 && unit < units.length - 1) {
      amount /= 1024;
      unit++;
    }
    return (amount >= 10 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)) + " " + units[unit];
  }

  function formatDuration(milliseconds) {
    let seconds = Math.max(0, Math.floor((Number(milliseconds) || 0) / 1000));
    const days = Math.floor(seconds / 86400);
    seconds %= 86400;
    const hours = Math.floor(seconds / 3600);
    seconds %= 3600;
    const minutes = Math.floor(seconds / 60);
    if (days) return days + "d " + hours + "h " + minutes + "m";
    if (hours) return hours + "h " + minutes + "m";
    return minutes + "m " + (seconds % 60) + "s";
  }

  function renderSystem(system) {
    app.system = system;
    const network = system.network || {};
    const storage = system.storage || {};
    const sd = storage.sd_card || {};
    const spiffs = storage.spiffs || {};
    const memory = system.memory || {};
    const device = system.device || {};
    const built = [system.build_date, system.build_time].filter(Boolean).join(" ");

    byId("headerVersion").textContent = system.firmware_version || "Version —";
    byId("buildSummary").textContent = (system.firmware_version || "Unknown") +
      (built ? " · " + built : "");
    byId("networkModeSummary").textContent = (network.mode || "Unknown") +
      (network.web_protocol ? " · " + network.web_protocol : "");
    byId("ipSummary").textContent = network.ip_address || "Unavailable";

    byId("diagVersion").textContent = system.firmware_version || "—";
    byId("diagBuild").textContent = built || "—";
    byId("diagBuildFlags").textContent = system.build_flags || "—";
    byId("diagIdf").textContent = system.idf_version || "—";
    byId("diagUptime").textContent = formatDuration(system.uptime_ms);
    byId("diagNetworkMode").textContent = network.mode || "—";
    byId("diagIp").textContent = network.ip_address || "—";
    byId("diagWebProtocol").textContent = network.web_protocol ? network.web_protocol + " · port " + network.web_port : "—";
    byId("diagTlsSessions").textContent = network.web_protocol === "HTTPS" ?
      String(device.tls_sessions || 0) + " / " + String(device.tls_session_limit || 0) : "Not active";
    byId("diagNetworkState").textContent = network.connected ? "Connected" : "Disconnected";
    byId("diagTrustedClock").textContent = network.time_synchronized && Number(network.unix_time) ?
      new Date(Number(network.unix_time) * 1000).toLocaleString() + " · synchronized" :
      "Not synchronized · outbound TLS unavailable";
    byId("diagAd2Source").textContent = device.alarmdecoder_source || "—";
    byId("diagUuid").textContent = device.uuid || "—";
    byId("diagConfigSource").textContent = storage.active_config_source || "—";
    byId("diagSd").textContent = sd.mounted ? "Mounted · " + formatBytes(sd.free_bytes) + " free / " + formatBytes(sd.total_bytes) : "Not installed";
    byId("diagSdLogging").textContent = sd.logging_active ?
      "Active · " + String(sd.logging_dropped || 0) + " dropped · " + String(sd.logging_write_errors || 0) + " errors" :
      (sd.logging_enabled ? "Configured, unavailable" : "Disabled");
    byId("diagSpiffs").textContent = spiffs.mounted ? formatBytes(spiffs.used_bytes) + " used / " + formatBytes(spiffs.total_bytes) : "Unavailable";
    byId("diagHeap").textContent = formatBytes(memory.free_heap_bytes);
    byId("diagMinHeap").textContent = formatBytes(memory.minimum_free_heap_bytes);
    byId("diagLargestHeap").textContent = formatBytes(memory.largest_free_block_bytes);
    byId("diagResetReason").textContent = device.last_reset_reason || "—";
  }

  async function fetchSystem() {
    return queueRequest(async () => {
      const response = await fetch(apiURL("/api/system"), { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error("System status unavailable");
      const system = await response.json();
      renderSystem(system);
      return system;
    });
  }

  async function fetchConfig(source) {
    return queueRequest(async () => {
      const response = await fetch(apiURL("/api/config?source=" + encodeURIComponent(source)), {
        cache: "no-store", credentials: "same-origin"
      });
      if (response.status === 404) return "Not available on this device.";
      if (!response.ok) throw new Error("Unable to load " + source + " configuration");
      return response.text();
    });
  }

  async function fetchLogs() {
    return queueRequest(async () => {
      const response = await fetch(apiURL("/api/logs?limit=64"), { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error("Device logs unavailable");
      const payload = await response.json();
      const items = Array.isArray(payload.items) ? payload.items : [];
      if (!items.length) return "No device logs have been captured during this boot session.";
      return items.map(item => "[" + formatDuration(item.uptime_ms) + "] " + (item.text || "")).join("\n");
    });
  }

  function renderFirmware(firmware) {
    app.firmware = firmware;
    const install = byId("installFirmware");
    let status = "Unavailable";
    let detail = firmware.error || "No firmware status was returned.";
    if (!firmware.supported) {
      status = "Not supported";
    } else if (!firmware.sd_mounted) {
      status = "SD card not mounted";
    } else if (!firmware.present) {
      status = "No firmware.bin";
    } else if (firmware.same_version) {
      status = "Current version already installed";
      detail = "Same-version reinstalls are blocked. Build a new release or use USB recovery.";
    } else if (firmware.downgrade) {
      status = "Downgrade blocked";
      detail = "The SD image is older than the installed firmware and cannot be installed.";
    } else if (!firmware.valid) {
      status = "Invalid image";
    } else if (firmware.update_in_progress) {
      status = "Installation in progress";
      detail = "The device will restart after the image passes final OTA validation.";
    } else if (firmware.upgrade_available) {
      status = "Upgrade available";
      detail = "The image passed its ESP32 target, project, checksum, and SHA-256 checks.";
    } else {
      status = "No upgrade available";
      detail = "The SD image is not eligible for installation.";
    }
    byId("sdFirmwareStatus").textContent = status;
    byId("sdFirmwareVersion").textContent = firmware.version || "—";
    byId("sdFirmwareBuild").textContent = [firmware.build_date, firmware.build_time].filter(Boolean).join(" ") || "—";
    byId("sdFirmwareSize").textContent = firmware.present ? formatBytes(firmware.size_bytes) : "—";
    byId("sdFirmwareDetail").textContent = detail;
    install.disabled = !firmware.valid || firmware.update_in_progress;
  }

  async function fetchFirmware() {
    return queueRequest(async () => {
      const response = await fetch(apiURL("/api/firmware"), { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error("SD firmware status unavailable");
      const firmware = await response.json();
      renderFirmware(firmware);
      return firmware;
    });
  }

  async function loadDiagnostics(force) {
    if (app.diagnosticsLoading || (app.diagnosticsLoaded && !force)) return;
    app.diagnosticsLoading = true;
    byId("reloadDiagnostics").disabled = true;
    ["activeConfig", "spiffsConfig", "sdConfig", "deviceLogs"].forEach(id => { byId(id).textContent = "Loading…"; });
    const results = await Promise.allSettled([
      fetchSystem(), fetchConfig("active"), fetchConfig("spiffs"), fetchConfig("sd"), fetchLogs(), fetchFirmware()
    ]);
    const targets = [null, "activeConfig", "spiffsConfig", "sdConfig", "deviceLogs", null];
    results.forEach((result, index) => {
      if (!targets[index]) return;
      byId(targets[index]).textContent = result.status === "fulfilled" ? result.value : result.reason.message;
    });
    if (results.some(result => result.status === "rejected")) {
      showToast("Some diagnostics could not be loaded.", true);
    }
    app.diagnosticsLoaded = true;
    app.diagnosticsLoading = false;
    byId("reloadDiagnostics").disabled = false;
  }

  function renderState(state) {
    app.state = state;
    app.serverUptime = Math.max(app.serverUptime, Number(state.uptime_ms) || 0);
    const mode = modeFor(state);
    const details = modeDetails(mode);
    const hero = byId("hero");
    hero.className = "hero " + details.hero;
    byId("statusText").textContent = details.label;
    byId("statusGlyph").textContent = details.glyph;
    byId("alphaMessage").textContent = state.last_alpha_message || "No keypad display message received.";
    byId("keypadPanelText").textContent = state.last_alpha_message || details.label;
    document.title = details.label + " · AlarmDecoder";

    setIndicator("powerState", state.ac_power ? "AC online" : "AC outage", state.ac_power ? "good" : "danger");
    setIndicator("batteryState", state.battery_low ? "Low" : "Normal", state.battery_low ? "danger" : "good");
    setIndicator("chimeState", state.chime_on ? "On" : "Off", state.chime_on ? "good" : "");
    setIndicator("bypassState", state.zone_bypassed ? "Active" : "Clear", state.zone_bypassed ? "warn" : "good");
    setIndicator("beepsState", state.beeps ? String(state.beeps) : "Quiet", state.beeps ? "warn" : "");
    renderZones(Array.isArray(state.zone_alerts) ? state.zone_alerts : []);
  }

  function renderZones(zones) {
    const container = byId("zones");
    container.textContent = "";
    byId("zoneCount").textContent = zones.length + (zones.length === 1 ? " active" : " active");
    if (!zones.length) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "No active zone alerts.";
      container.appendChild(empty);
      return;
    }

    zones.forEach(zone => {
      const card = document.createElement("article");
      card.className = "zone";
      const number = document.createElement("span");
      number.className = "zone-number";
      number.textContent = String(zone.zone);
      const copy = document.createElement("div");
      copy.className = "zone-copy";
      const name = document.createElement("b");
      name.textContent = zone.name || "Zone " + zone.zone;
      const status = document.createElement("small");
      status.textContent = zone.state || "open";
      copy.append(name, status);
      const bypass = document.createElement("button");
      bypass.type = "button";
      bypass.textContent = "Bypass";
      bypass.addEventListener("click", () => sendCommand("BYPASS", String(zone.zone)));
      card.append(number, copy, bypass);
      container.appendChild(card);
    });
  }

  function eventLabel(event) {
    return ({
      "ARMED": "System armed",
      "DISARMED": "System disarmed",
      "POWER": "Power state changed",
      "READY": "Ready state changed",
      "ALARM": "Alarm state changed",
      "FIRE": "Fire state changed",
      "ZONE": "Zone state changed",
      "LOW BATTERY": "Battery state changed",
      "CHIME": "Chime state changed",
      "BEEPS": "Keypad beeps changed",
      "PROG. MODE": "Programming state changed",
      "ALPHA MSG.": "Keypad display updated",
      "CONTACT ID": "Panel event received",
      "PANIC": "Panic event received",
      "EXIT": "Exit state changed"
    }[event] || event || "Panel update");
  }

  function relativeTime(uptime) {
    const seconds = Math.max(0, Math.floor((app.serverUptime - Number(uptime || 0)) / 1000));
    if (seconds < 5) return "just now";
    if (seconds < 60) return seconds + "s ago";
    if (seconds < 3600) return Math.floor(seconds / 60) + "m ago";
    if (seconds < 86400) return Math.floor(seconds / 3600) + "h ago";
    return Math.floor(seconds / 86400) + "d ago";
  }

  function exactEventTime(uptime) {
    const age = Math.max(0, app.serverUptime - Number(uptime || 0));
    const date = new Date(Date.now() - age);
    return {
      date,
      label: date.toLocaleString([], {
        year: "numeric", month: "short", day: "2-digit",
        hour: "2-digit", minute: "2-digit", second: "2-digit",
        timeZoneName: "short"
      })
    };
  }

  function renderHistory() {
    const list = byId("activityList");
    list.textContent = "";
    byId("activityCount").textContent = String(app.history.length);
    if (!app.history.length) {
      const empty = document.createElement("li");
      empty.className = "empty-state";
      empty.textContent = "No activity recorded yet.";
      list.appendChild(empty);
      return;
    }

    app.history.slice(0, 64).forEach(entry => {
      const item = document.createElement("li");
      const alert = /ALARM|FIRE|PANIC|LOW BATTERY/.test(entry.event || "");
      item.className = "activity-item" + (alert ? " alert" : "");
      const dot = document.createElement("span");
      dot.className = "activity-dot";
      const body = document.createElement("div");
      body.className = "activity-body";
      const title = document.createElement("b");
      title.textContent = eventLabel(entry.event);
      const detail = document.createElement("p");
      detail.textContent = entry.alpha || (entry.zone ? "Zone " + entry.zone : "State updated");
      body.append(title, detail);
      const meta = document.createElement("div");
      meta.className = "activity-meta";
      const relative = document.createElement("b");
      relative.textContent = relativeTime(entry.uptime_ms);
      const exact = document.createElement("time");
      const timestamp = exactEventTime(entry.uptime_ms);
      exact.dateTime = timestamp.date.toISOString();
      exact.textContent = timestamp.label;
      exact.title = timestamp.date.toISOString();
      const partition = document.createElement("span");
      partition.textContent = "Partition " + (entry.partition || partID);
      meta.append(relative, exact, partition);
      item.append(dot, body, meta);
      list.appendChild(item);
    });
  }

  function rememberLiveEvent(state) {
    if (!state.event || state.event === "SYNC") return;
    app.history.unshift({
      event: state.event,
      uptime_ms: state.uptime_ms,
      partition: state.partition,
      zone: state.zone || 0,
      alpha: state.last_alpha_message || ""
    });
    app.history = app.history.slice(0, 64);
    renderHistory();
  }

  function wsURL() {
    if (wsHost) {
      if (/^wss?:\/\//i.test(wsHost)) return wsHost.replace(/\/$/, "") + "/ad2ws";
      return (window.location.protocol === "https:" ? "wss://" : "ws://") +
        wsHost.replace(/\/$/, "") + "/ad2ws";
    }
    return (window.location.protocol === "https:" ? "wss://" : "ws://") + window.location.host + "/ad2ws";
  }

  function connect() {
    clearTimeout(app.reconnectTimer);
    if (app.socket && app.socket.readyState < WebSocket.CLOSING) return;
    setConnection("connecting");
    const socket = new WebSocket(wsURL());
    app.socket = socket;

    socket.addEventListener("open", () => {
      app.connected = true;
      app.reconnectDelay = 1000;
      setConnection("online");
      sendRaw("!SYNC:" + partID + "," + codeID);
      sendRaw("!HISTORY:64");
    });

    socket.addEventListener("message", event => {
      if (typeof event.data !== "string") return;
      if (event.data.startsWith("!ERROR:")) {
        showToast(event.data.slice(7), true);
        return;
      }
      if (!event.data.startsWith("{")) return;
      try {
        const payload = JSON.parse(event.data);
        if (payload.event === "HISTORY") {
          app.serverUptime = Number(payload.uptime_ms) || app.serverUptime;
          app.history = Array.isArray(payload.items) ? payload.items : [];
          renderHistory();
        } else {
          rememberLiveEvent(payload);
          renderState(payload);
        }
      } catch (error) {
        showToast("The device returned invalid state data.", true);
      }
    });

    socket.addEventListener("close", () => {
      app.connected = false;
      if (app.socket === socket) app.socket = null;
      setConnection("offline");
      app.reconnectTimer = setTimeout(connect, app.reconnectDelay);
      app.reconnectDelay = Math.min(app.reconnectDelay * 2, 15000);
    });

    socket.addEventListener("error", () => socket.close());
  }

  function sendRaw(message) {
    if (!app.socket || app.socket.readyState !== WebSocket.OPEN) {
      showToast("Panel connection is offline.", true);
      return false;
    }
    app.socket.send(message);
    return true;
  }

  function sendCommand(command, argument) {
    const ok = sendRaw("!SEND:<" + command + ">" + (argument || ""));
    if (ok && command !== "KEYS") showToast(command.replace("_ALARM", "").replace("_", " ") + " sent");
    return ok;
  }

  function sendKey(key, button) {
    if (!/^[0-9*#]$/.test(key) || !sendCommand("KEYS", key)) return;
    app.keypadMask = (app.keypadMask + "•").slice(-12);
    byId("keypadEntry").textContent = app.keypadMask;
    clearTimeout(app.keypadClearTimer);
    app.keypadClearTimer = setTimeout(() => {
      app.keypadMask = "";
      byId("keypadEntry").innerHTML = "&nbsp;";
    }, 3000);
    if (button) {
      button.classList.add("pressed");
      setTimeout(() => button.classList.remove("pressed"), 120);
    }
  }

  async function maintenanceAction(action) {
    const isUpgrade = action === "upgradeusd";
    const confirmed = window.confirm(isUpgrade ?
      "Install the validated SD-card firmware? The device will restart automatically if installation succeeds." :
      "Restart the AlarmDecoder device now?");
    if (!confirmed) return;
    const installButton = byId("installFirmware");
    const restartButton = byId("restartDevice");
    installButton.disabled = true;
    restartButton.disabled = true;
    try {
      const result = await queueRequest(async () => {
        const response = await fetch(apiURL("/api/action"), {
          method: "POST",
          cache: "no-store",
          credentials: "same-origin",
          headers: {
            "Content-Type": "application/json",
            "X-AD2IoT-Action": action
          },
          body: JSON.stringify({ action })
        });
        if (!response.ok) {
          const message = await response.text();
          throw new Error(message || "Maintenance action was rejected");
        }
        return response.json();
      });
      showToast(result.message || "Maintenance action accepted");
      if (isUpgrade) {
        byId("sdFirmwareStatus").textContent = "Installation starting";
        byId("sdFirmwareDetail").textContent = "The device will restart after final OTA validation.";
      }
    } catch (error) {
      showToast(error.message, true);
      restartButton.disabled = false;
      installButton.disabled = !(app.firmware && app.firmware.valid);
    }
  }

  document.querySelectorAll(".tab").forEach(tab => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
      document.querySelectorAll(".view").forEach(view => view.classList.remove("active"));
      byId(tab.dataset.view + "View").classList.add("active");
      if (tab.dataset.view === "settings") loadDiagnostics(false);
    });
  });

  document.querySelectorAll("[data-command]").forEach(button => {
    button.addEventListener("click", () => sendCommand(button.dataset.command));
  });
  byId("refreshButton").addEventListener("click", () => sendRaw("!SYNC:" + partID + "," + codeID));
  byId("reloadHistory").addEventListener("click", () => sendRaw("!HISTORY:64"));
  byId("reloadDiagnostics").addEventListener("click", () => loadDiagnostics(true));
  byId("refreshFirmware").addEventListener("click", () => {
    byId("refreshFirmware").disabled = true;
    fetchFirmware().catch(error => showToast(error.message, true)).finally(() => {
      byId("refreshFirmware").disabled = false;
    });
  });
  byId("installFirmware").addEventListener("click", () => maintenanceAction("upgradeusd"));
  byId("restartDevice").addEventListener("click", () => maintenanceAction("restart"));

  document.querySelectorAll("[data-emergency]").forEach(button => {
    let taps = 0;
    let timer = null;
    button.addEventListener("click", () => {
      taps++;
      clearTimeout(timer);
      button.classList.toggle("tap-1", taps === 1);
      button.classList.toggle("tap-2", taps === 2);
      if (taps >= 3) {
        sendCommand(button.dataset.emergency);
        taps = 0;
        button.classList.remove("tap-1", "tap-2");
      }
      timer = setTimeout(() => {
        taps = 0;
        button.classList.remove("tap-1", "tap-2");
      }, 3000);
    });
  });

  document.querySelectorAll("[data-key]").forEach(button => {
    button.addEventListener("click", () => sendKey(button.dataset.key, button));
  });
  document.addEventListener("keydown", event => {
    if (!/^[0-9*#]$/.test(event.key)) return;
    const keypadView = byId("keypadView");
    if (!keypadView.classList.contains("active")) return;
    event.preventDefault();
    const button = document.querySelector('[data-key="' + event.key + '"]');
    sendKey(event.key, button);
  });

  setInterval(() => {
    if (app.connected) sendRaw("!PING:00000000");
  }, 15000);
  setInterval(() => {
    fetchSystem().catch(() => {});
  }, 30000);
  fetchSystem().catch(() => {
    byId("buildSummary").textContent = "Status unavailable";
  }).finally(connect);
}());
