(function (root, factory) {
  "use strict";
  const activity = factory();
  if (typeof module === "object" && module.exports) module.exports = activity;
  else root.AD2Activity = activity;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const MERGE_WINDOW_MS = 2500;
  const PRIORITY = {
    "ALARM": 100, "FIRE": 100, "PANIC": 100,
    "ARMED": 90, "DISARMED": 90,
    "ZONE": 80,
    "POWER": 70, "LOW BATTERY": 70,
    "CONTACT ID": 60, "CHIME": 60, "EXIT": 60, "PROG. MODE": 60,
    "READY": 30, "BEEPS": 20, "ALPHA MSG.": 10
  };

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

  function titleCase(value) {
    return String(value || "").toLowerCase().replace(/(^|[\s/-])([a-z])/g,
      (_match, prefix, letter) => prefix + letter.toUpperCase());
  }

  function describe(entry) {
    const alpha = String(entry.alpha || "").trim();
    const fault = alpha.match(/^FAULT\s+(\d+)\s*(.*)$/i);
    if (fault) {
      return {
        title: "Zone " + fault[1] + " faulted",
        detail: fault[2] ? titleCase(fault[2]) : "Zone requires attention"
      };
    }

    if (/READY TO ARM/i.test(alpha)) {
      const states = [];
      if (/DISARMED/i.test(alpha)) states.push("Disarmed");
      if (/BYPASS/i.test(alpha)) states.push("Bypass active");
      return { title: "System ready to arm", detail: states.join(" · ") || "All zones are ready" };
    }

    if (/ZONES FAULTED/i.test(alpha)) {
      const states = [];
      if (/DISARMED/i.test(alpha)) states.push("Disarmed");
      if (/BYPASS/i.test(alpha)) states.push("Bypass active");
      return { title: "Zones faulted", detail: states.join(" · ") || "One or more zones require attention" };
    }

    return {
      title: eventLabel(entry.event),
      detail: alpha || (entry.zone ? "Zone " + entry.zone : "State updated")
    };
  }

  function summarizeHistory(history, showTechnical) {
    const entries = Array.isArray(history) ? history : [];
    if (showTechnical) {
      return entries.map(entry => Object.assign({}, entry, {
        events: [entry.event || ""],
        update_count: 1,
        technical: true
      }));
    }

    const summaries = [];
    entries.forEach(entry => {
      const alpha = String(entry.alpha || "").trim();
      const previous = summaries[summaries.length - 1];
      const sameBurst = previous && alpha && previous.alpha === alpha &&
        Number(previous.partition || 0) === Number(entry.partition || 0) &&
        Math.abs(Number(previous.merge_uptime_ms || 0) - Number(entry.uptime_ms || 0)) <= MERGE_WINDOW_MS;

      if (!sameBurst) {
        summaries.push(Object.assign({}, entry, {
          events: [entry.event || ""],
          update_count: 1,
          technical: false,
          merge_uptime_ms: entry.uptime_ms
        }));
        return;
      }

      previous.update_count++;
      previous.merge_uptime_ms = entry.uptime_ms;
      if (entry.event && !previous.events.includes(entry.event)) previous.events.push(entry.event);
      if (!previous.zone && entry.zone) previous.zone = entry.zone;
      if ((PRIORITY[entry.event] || 0) > (PRIORITY[previous.event] || 0)) {
        previous.event = entry.event;
      }
    });
    return summaries.map(summary => {
      delete summary.merge_uptime_ms;
      return summary;
    });
  }

  return { MERGE_WINDOW_MS, describe, eventLabel, summarizeHistory };
}));
