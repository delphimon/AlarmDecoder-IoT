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

  function renderState(state) {
    app.state = state;
    app.serverUptime = Number(state.uptime_ms) || app.serverUptime;
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
      meta.textContent = relativeTime(entry.uptime_ms);
      const partition = document.createElement("span");
      partition.textContent = "Partition " + (entry.partition || partID);
      meta.appendChild(partition);
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
      return "ws://" + wsHost.replace(/\/$/, "") + "/ad2ws";
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

  document.querySelectorAll(".tab").forEach(tab => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
      document.querySelectorAll(".view").forEach(view => view.classList.remove("active"));
      byId(tab.dataset.view + "View").classList.add("active");
    });
  });

  document.querySelectorAll("[data-command]").forEach(button => {
    button.addEventListener("click", () => sendCommand(button.dataset.command));
  });
  byId("refreshButton").addEventListener("click", () => sendRaw("!SYNC:" + partID + "," + codeID));
  byId("reloadHistory").addEventListener("click", () => sendRaw("!HISTORY:64"));

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
  connect();
}());
