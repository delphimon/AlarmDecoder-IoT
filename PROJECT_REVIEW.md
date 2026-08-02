# AlarmDecoder-IoT Project Review

Review date: 2026-08-01

## Scope

This review is based on the repository contents at commit `31f0fb0` (`AD2IOT-1107`), including the four implementation commits after the original review. It covers README and changelog claims, CI and release workflows, build/default configuration, source-level feature wiring, browser assets, and local static checks. Hardware behavior was not tested.

## Verification Status

- The origin fork's GitHub Actions [`CI build`](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/30734725706) completed successfully for exact commit `31f0fb0` on 2026-08-01 PDT. The workflow builds `esp32-poe-iso`, builds SPIFFS, and packages the webUI firmware. Upstream `nutechsoftware` CI remains at the older `a6dd08c` commit.
- A local Apple Silicon build still cannot start compilation: PlatformIO Espressif32 6.4.0 invokes an Intel-only CMake 3.16 binary and exits with `Bad CPU type in executable`. This is a host-tool compatibility problem, but it leaves current firmware compilation unverified.
- The declared `fmt` submodule is initialized. The updated browser JavaScript, Python load-test script, shell helper, and checked-in JSON files passed syntax checks.
- `git diff --check` passed for this review document.
- No firmware was flashed. Alarm panel behavior, HTTPS certificates, WebSocket controls, diagnostics, network CLI, ser2sock reconnects, notifications, SmartThings, and update recovery were not exercised on hardware.

## Executive Summary

- The actively shipped product is an ESP32 AlarmDecoder network appliance with Ethernet/Wi-Fi, MQTT/Home Assistant integration, a responsive browser keypad/dashboard, read-only diagnostics APIs, optional HTTPS, serial-over-TCP, an opt-in network CLI, notification providers, local configuration, and firmware update support.
- The source still contains a substantial SmartThings Direct Connected implementation, but that build was removed from CI and release packaging in March 2026 because it was broken. It should not be presented as an available production build.
- Three important weaknesses from the original review were addressed: WebSocket input is bounded and validated, static file requests reject traversal, and the uSD updater now checks file/OTA failures and retains failed images. Version identity and ser2sock shutdown handling also improved.
- The current security posture is still not appropriate for an untrusted network. Unauthenticated FTP can upload an unsigned firmware image and trigger installation; the web UI permits alarm control without user authentication; and the new diagnostic log can disclose alarm codes or integration credentials previously printed by CLI commands.
- Reliability is difficult to establish because the repository has no project-authored automated tests. CI builds only the `esp32-poe-iso` target and does not run style, static analysis, parser tests, protocol tests, or security checks.
- The project is feature-rich but still mixes active, disabled, obsolete, experimental, and unfinished capabilities in one default configuration and one README.

## Priority Findings

### P0 - Unauthenticated FTP clients can still install arbitrary firmware

- `factory-reset` creates a configuration with Ethernet DHCP and FTP enabled, but it does not set FTP credentials or an ACL.
- FTP authentication support exists in the class but `ftpd_init()` never calls `setCredentials()`.
- If no ACL is stored, FTP explicitly defaults to `0.0.0.0/0`.
- FTP supports arbitrary file upload plus the custom `UPGD` command.
- The uSD updater now handles allocation, size, read, OTA write/finalization, metadata, and boot-selection failures correctly, but it still accepts any structurally valid ESP application image without a release signature, board/build policy, or downgrade check.

Impact: any host allowed to reach enabled FTP can replace the appliance firmware, persist arbitrary code, read alarm codes/API tokens, alter configuration, or reboot the device. This is especially serious because a factory reset creates this state.

Evidence: `main/device_control.cpp:343`, `components/ftpd/ftpd.cpp:982`, `components/ftpd/ftpd.cpp:1622`, `components/ftpd/ftpd.cpp:1865`, `components/usdupdate/usdupdate.cpp:56`.

### P0 - The web UI exposes alarm control and can leak secrets without authentication

- WebSocket commands directly invoke disarm, arm, exit, bypass, auxiliary alarm, panic alarm, and fire alarm operations.
- Access control remains IP/CIDR-only; there is no user, password, session token, request signature, or per-command authorization. HTTPS protects transport when explicitly configured but does not identify or authorize a user.
- The default ACL is `0.0.0.0/0`, and plain HTTP remains the default.
- The new `/api/config` endpoint redacts common secret fields, but `/api/logs` exposes a generic reboot-scoped capture of `ad2_printf_host()` output. Existing CLI handlers print alarm codes, Pushover keys, and Twilio credentials in quoted prose formats the log redactor does not recognize.
- Static file traversal is no longer part of this finding: requests containing `..` or backslashes are now rejected before filesystem access.

Impact: any host admitted by the ACL can operate the alarm. It may also retrieve secrets if a sensitive CLI command was used during the current boot, including commands entered through the authenticated network CLI.

Evidence: `components/webUI/webUI.cpp:330`, `components/webUI/webUI.cpp:489`, `components/webUI/webUI.cpp:698`, `components/webUI/webUI.cpp:902`, `main/ad2_utils.cpp:1678`, `main/ad2_cli_cmd.cpp:95`, `components/pushover/pushover.cpp:284`, `components/twilio/twilio.cpp:640`.

### P1 - TLS peer verification is globally disabled

- `CONFIG_ESP_TLS_INSECURE=y` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` are enabled.
- This weakens MQTT-over-TLS, Pushover, Twilio, SendGrid, and the disabled HTTPS OTA path.
- Broker credentials, alarm states, codes in command messages, and notification provider credentials may be exposed to an active network attacker; MQTT control messages could also be injected when commands are enabled.

Evidence: `sdkconfig.defaults:85`.

### P1 - Factory reset does not reliably return the device to factory state

- Configuration loading prefers `/sdcard/ad2iot.ini` over internal SPIFFS.
- `factory-reset` erases NVS and rewrites only `/spiffs/ad2iot.ini`; it leaves the uSD configuration untouched.
- On reboot, a present uSD card therefore restores the prior configuration.
- The generated internal configuration also enables insecure FTP as described above.

Evidence: `main/ad2_utils.cpp:152`, `main/device_control.cpp:343`.

### P1 - Unbounded Wi-Fi configuration copies can corrupt memory

- SSID and password values are loaded from the INI/CLI into `std::string` and copied with `strcpy` into fixed-size ESP-IDF fields.
- No maximum length is checked before either copy.

Impact: an oversized setting can overwrite stack memory and crash the device; combined with remote FTP configuration writes, this is remotely triggerable after restart.

Evidence: `main/device_control.cpp:503`.

### P1 - Firmware update still lacks authenticity and rollback policy

- The active uSD path now validates image size, read completion, ESP application structure, and all OTA operations, and it retains the image on failure.
- It still has no cryptographic signature, expected hash, board/build compatibility, downgrade, or release-channel check.
- Bootloader rollback remains disabled, so a structurally valid but boot-failing image has no automatic application-level recovery path.

Evidence: `components/usdupdate/usdupdate.cpp:56`; `sdkconfig.esp32-poe-iso:260`.

### P2 - The network CLI protects access but not transport

- The new TCP CLI is disabled until an eight-character password is configured and defaults its ACL to loopback, which is appropriately fail-closed.
- Once enabled, password authentication and all administrative commands travel over plain TCP/Telnet. The password is also stored in plaintext in the active INI file.
- The CLI exposes the full command surface, including alarm codes, network and integration credentials, restart/factory reset, and update triggers.

The README warns users to restrict this feature to trusted private networks or a VPN. TLS or an SSH-based management channel is still required before this can be considered a generally safe remote-administration feature.

Evidence: `components/network_cli/network_cli.cpp:230`, `components/network_cli/network_cli.cpp:293`, `README.md:105`.

### P2 - SmartThings is retained as a product capability but is not shippable

- The SmartThings build was removed from CI/release packaging as broken in commit `ec9982d`.
- The component cannot register in normal PlatformIO builds unless `STDK_CORE_PATH` is externally provided.
- Adoption was not successful in the latest release notes.
- Major capability blocks remain under `#if 0`, including `securitySystem`, main action buttons, and output switches.

Evidence: `.github/workflows/build.yml`, `components/stsdk/CMakeLists.txt:1`, `components/stsdk/stsdk_main.cpp:1093`, `CHANGELOG.md:15`, `CHANGELOG.md:62`.

### P2 - Verification coverage is too small for the system's risk

- There are no project-authored unit, integration, protocol-parser, or hardware-in-loop tests; `test/` contains only an astyle installer.
- CI builds one board target and a SPIFFS image. It does not build `esp32dev`, run the simulator, validate configuration migrations, inspect warnings, or test update/control paths.
- The project-authored C/C++ source currently contains 93 TODO/FIXME/WIP markers and 19 `#if 0` blocks, excluding vendored `fmt` sources.

Evidence: `.github/workflows/build.yml`, `test/install_astyle.sh`.

## Resolved or Materially Improved Since the Original Review

- **Malformed WebSocket handling:** frames are limited to 256 bytes, explicitly terminated, restricted to text, and validated for session state, argument count, numeric conversion, partition/code/zone bounds, and command allowlisting.
- **Web static-file traversal:** requests containing `..` or backslashes are rejected before filesystem resolution. This should still receive encoded-path regression coverage.
- **uSD update transaction handling:** allocation, image size, file reads, OTA writes, final validation, metadata, boot partition selection, cleanup, and image retention now have explicit success/failure handling.
- **Firmware version drift in code:** the removed `FIRMWARE_VERSION` macro has been replaced by ESP application metadata sourced from `version.txt`; runtime consumers now call `ad2_firmware_version()`.
- **ser2sock restart behavior:** the daemon now closes listening/client sockets through an ESP shutdown handler and handles partial sends, queue overflow, and shared connection state more defensively.
- **Read-only observability:** state, activity history, system diagnostics, configuration snapshots, and recent-log APIs plus a Settings UI are now implemented. Log confidentiality remains an open issue described above.

## Complete Capabilities

### Firmware and Build System

- ESP32 firmware for an AlarmDecoder IoT network appliance using ESP-IDF through PlatformIO.
- Supported board targets in `platformio.ini`: `esp32dev` and `esp32-poe-iso`.
- Custom 4 MB partition table with dual OTA app slots, NVS, encrypted NVS-key storage, SPIFFS config storage, and flash coredump storage; external FAT/uSD storage is mounted separately.
- Two product build modes remain in source and documentation:
  - `webui`: the active normal network appliance build produced by CI and releases.
  - `stsdk`: a SmartThings direct-connected build retained in source but no longer built or shipped.
- Component-level feature flags exist for TOP, FTPD, MQTT, webUI, ser2sock, Twilio/SendGrid, Pushover, OTA, uSD firmware update, Wi-Fi, Ethernet, and SmartThings.
- `version.txt` is now the single firmware version input used by ESP-IDF application metadata and all runtime version reporting; the current value is `AD2IOT-1107`.
- A generated `sdkconfig.esp32-poe-iso` is checked in alongside `sdkconfig.defaults`, making the current board configuration inspectable but also creating a second configuration artifact that must be kept synchronized.
- GitHub Actions release-package build support is documented in the 1.1.0 P2 changelog.

### AlarmDecoder Core

- Reads AlarmDecoder protocol data from either:
  - local UART/COM GPIO pins, for an attached AD2pHAT or AD2 device;
  - TCP socket, for a remote ser2sock AlarmDecoder source.
- Parses and tracks partition state, zone state, LRR events, alarm state, ready state, arm/disarm state, chime, beeps, fire, power, low battery, bypass, exit, config, and version events.
- Supports Ademco and DSC configuration patterns through `ad2config`.
- Can enforce selected AD2 device firmware settings by comparing local config with AD2-reported config and sending a `C...` config update once per boot.
- Maintains up to:
  - 8 partitions;
  - 255 zones;
  - 128 alarm code slots;
  - 255 virtual switches.
- Supports zone descriptions and types stored as JSON strings in `[zone N]`.
- Provides global virtual switches driven by message type filters, pre-filters, and open/close/trouble regexes.
- Has a shared HTTP request queue for outbound HTTPS integrations.

### Configuration and CLI

- USB serial CLI at 115200 baud.
- Human-readable INI configuration loaded from `/sdcard/ad2iot.ini` first, then `/spiffs/ad2iot.ini`, then defaults.
- CLI commands include:
  - `help`;
  - `restart`;
  - `factory-reset`;
  - `logmode`;
  - `top`;
  - `ad2source`;
  - `ad2config`;
  - `ad2term` pass-through terminal, with optional attached-AD2 reset;
  - `netmode`;
  - `partition`;
  - `zone`;
  - `code`;
  - `switch`;
  - component commands for enabled integrations.
- An optional network CLI exposes the same command registry over one authenticated TCP/Telnet session:
  - configurable enable flag and TCP port, defaulting to 2323;
  - configurable IP/CIDR ACL, defaulting to `127.0.0.1`;
  - required password of 8-128 characters;
  - bounded line/password input, constant-work comparison, three authentication attempts, Telnet negotiation, and `exit`/`quit` commands;
  - disabled at runtime unless both enablement and a valid password are present.
- `restart` persists running configuration before reboot.
- `factory-reset` erases NVS, replaces the internal SPIFFS configuration, and reboots, but does not remove a higher-priority uSD configuration.
- `top` provides FreeRTOS task, stack, uptime, memory, and CPU-busy diagnostics when enabled.

### Networking

- Supports disabled networking, Wi-Fi station mode, and Ethernet mode.
- Supports DHCP and static IPv4 configuration for Wi-Fi/Ethernet.
- Has IPv6 conditional code paths where LWIP IPv6 is enabled.
- Provides ACL parsing/checking for exposed services such as webUI and FTPD.
- The network CLI adds a separately configurable ACL and authenticated management listener.
- Ethernet Kconfig supports internal ESP32 EMAC and a DM9051 SPI Ethernet option.

### MQTT

- MQTT client can be enabled/disabled independently.
- Configurable broker URL, topic prefix, and Home Assistant-style discovery prefix.
- Publishes partition, zone, LRR/Contact ID, version/config, and virtual switch state messages.
- Publishes retained availability/status/LWT-style state.
- Supports Home Assistant discovery payloads for partitions, zones, and virtual switches.
- Optional command subscription can receive disarm, arm stay/away, exit, chime, auxiliary/panic/fire alarm, bypass, raw-send, AD2 config/update, and AD2IoT update verbs; the CLI warns that this is unsafe on public brokers.
- Virtual switch notifications can produce separate `open`, `close`, and `trouble` MQTT messages.

### Web UI and Read-Only APIs

- Responsive browser application with dashboard, partition state, active-zone cards, arm stay/away, disarm, chime, exit, zone bypass, emergency controls, full keypad, activity history, and settings/diagnostics views.
- WebSocket endpoint streams alarm state and reboot-scoped history and accepts bounded, validated control commands.
- Read-only HTTP APIs provide:
  - `/api/state` for current partition state and active zones;
  - `/api/history` for up to 64 reboot-scoped alarm events;
  - `/api/system` for firmware/build, network, storage, heap, UUID, and AlarmDecoder-source status;
  - `/api/config` for active, SPIFFS, or uSD configuration snapshots with field redaction;
  - `/api/logs` for up to 64 reboot-scoped device log lines.
- Embedded HTTP server serves static and pre-compressed files from `/sdcard/www`; traversal tokens and backslashes are rejected.
- Optional HTTPS listener on port 443 loads a PEM certificate chain and private key from configurable paths beneath `/sdcard`. Plain HTTP remains available when HTTPS is disabled; there is no dual listener or redirect.
- Static app assets are provided under `contrib/webUI/flash-drive/www`, including an OpenAPI description and Swagger UI page.
- Template support exists for `.tpl` files using a bundled tiny template engine.
- webUI can be enabled/disabled and protected with an IP/CIDR ACL.
- README documents uSD web content and Let's Encrypt certificate setup.

### FTP Daemon

- FTP server for remote access to mounted files, including `/spiffs` and `/sdcard` virtual paths.
- CLI configuration for enable/disable and ACL.
- Supports common FTP operations sufficient for editing/uploading configuration and web assets.
- Adds a custom `REST` command path for restart without saving running config, intended for FileZilla workflows.
- Adds custom `UPGD` and `VERS` commands for uSD firmware installation and version reporting.
- Allows only one client connection at a time.

### ser2sock

- TCP server exposes the AlarmDecoder protocol stream to network clients.
- Forwards raw RX data from the parser to connected ser2sock clients.
- Can also act as a socket client when `ad2source` is configured as a remote socket source.
- Configurable enable flag and server/client settings through the component CLI.
- The server now uses mutex-protected connection/FIFO state, handles partial nonblocking sends, disconnects slow clients when queues overflow, configures TCP keepalive, and registers an ESP shutdown handler to close listening/client sockets before software restart.

### Notifications

- Pushover integration:
  - configurable app token and user key;
  - virtual-switch-driven notification messages;
  - open/close/trouble message mapping.
- Twilio/SendGrid integration:
  - configurable account SID/token/from/to/type/format;
  - Twilio SMS;
  - Twilio voice calls via TwiML;
  - SendGrid email;
  - many notification slots;
  - per-slot disable setting;
  - virtual-switch-driven open/close/trouble messages.

### Firmware Update

- uSD firmware update component is enabled by default.
- uSD update commands register as `versionusd` and `upgradeusd`.
- The changelog documents a FileZilla workflow using `/sdcard/firmware.bin`.
- The uSD updater validates image size against the target slot, handles allocation/read/write/finalization/metadata/boot-selection errors, aborts incomplete writes, retains failed images, and only removes the image/restarts after ESP-IDF validates and selects it.
- OTA update code exists behind `CONFIG_AD2IOT_OTAUPDATE`, including CLI commands, but it is disabled in defaults.

### SmartThings (Source Present, Not Shippable)

- SmartThings Direct Connected SDK integration is present behind `CONFIG_STDK_IOT_CORE`.
- CLI supports SmartThings enable flag and security credential fields.
- Initializes SmartThings connection from generated `deviceInfo` JSON.
- Registers capabilities for:
  - refresh;
  - health check;
  - firmware update;
  - power source;
  - battery;
  - chime state and chime momentary action;
  - fire/smoke state and fire panic action;
  - alarm bell state and panic action;
  - auxiliary alarm action;
  - arm stay/arm away contact indicators and actions;
  - bypass, ready, and exit indicators;
  - exit and disarm momentary actions.
- Updates SmartThings capabilities from AlarmDecoder callbacks for arm, disarm, chime, fire, power, low battery, alarm, bypass, ready, and exit.

### Contrib and Hardware Assets

- Flashing instructions and firmware-load testing notes.
- ESP32 upload-tool screenshot.
- AD2pHAT/ESP32 breadboard image.
- Board/carrier PDFs and adapter PDF.
- AlarmDecoder simulator script and sample log.
- Example webUI screenshots and complete flash-drive web content.
- Example/default `ad2iot.ini`.
- Hardware abstraction for status LED patterns, optional button counting, relay/output state, attached-AD2 reset, persistent storage, and network interfaces; several optional controls are not wired in current board definitions.

## Parts That Might Be Obsolete

- **SmartThings product build:** likely obsolete unless there is a committed plan to restore it. It was removed from CI/releases as broken, adoption failed, and current normal builds cannot register the component. Disabled/generated capability files are candidates for removal with it.
- **HTTPS OTA product path:** disabled in defaults and replaced operationally by uSD updates, while the README still presents internet OTA as normal. Either restore and test signed OTA or remove/archive the code and correct the docs.
- **Legacy ESP-IDF GNU Make entry point:** the root `Makefile` uses `make/project.mk`, an old pre-CMake ESP-IDF build path that does not match the current ESP-IDF 5.1/PlatformIO workflow.
- **Old SmartThings LAN8720 patch:** `contrib/esp_eth_phy_lan8720.c.patch` targets an old `st-device-sdk-c-ref` tree and is not referenced by the active build.
- **FTP administration:** functional but obsolete as the preferred management channel for a security appliance. It has no transport security and is not wired to authentication.
- **ESP-IDF-era compatibility blocks:** disabled Ethernet pin-mode code and changelog references to auditing ESP-IDF 3.2 APIs predate the pinned ESP-IDF 5.1 toolchain and should be reviewed or removed.
- **Build comments/configuration:** `platformio.ini` says “Force 4.3 branch” while selecting PlatformIO Espressif32 6.4.x/ESP-IDF 5.1. `sdkconfig.defaults` also carries SmartThings SDK settings even though normal PlatformIO builds cannot register that component.
- **Stale release examples:** runtime version identity is now correctly centralized in `version.txt`, but README command examples still report `AD2IOT-1103` and `AD2IOT-1104` while current source is `AD2IOT-1107`. Git tags `1.1.0p1` and `1.1.1` point to the same 2022 commit, so tag/release naming still needs clarification.
- **Static example output:** README boot logs and some setup guidance date from 2021 and include components/commands that no longer match the active package.

## Parts Not Yet Fully Implemented

- SmartThings adoption is not proven complete. The changelog has `STSDK: TODO: Successful adopting test`, and the release notes mention a failed adoption test.
- SmartThings cannot currently be built in the PlatformIO path according to the changelog open issue.
- SmartThings `securitySystem` capability is disabled under `#if 0` because it is marked broken in the SmartThings cloud.
- SmartThings main arm/disarm/exit button capability is disabled under `#if 0`.
- SmartThings Output A and Output B switch capabilities are disabled under `#if 0`.
- SmartThings zone devices are listed as TODO and are not implemented as separate SmartThings devices.
- SmartThings carbon monoxide alarm support is listed as TODO and depends on LRR processing.
- SmartThings partition selection is not configurable in-app; refresh uses the default partition slot with a FIXME.
- Virtual Switch A/B and Button A/B are listed as unfinished in the changelog.
- Physical button events are detected and counted, but the main task does not act on them beyond a SmartThings FIXME.
- AD2 device firmware update (`ad2_fw_update()`) is listed as a needed feature, and `ad2_fw_update()` currently logs a TODO rather than performing an update.
- AD2IoT config-update command handling has a TODO placeholder in `ad2_utils.cpp`.
- Exit countdown tracking for DSC/Ademco is listed as an API TODO.
- Virtual switch reset timing is only partially implemented: comments say it is currently TRUE/FALSE rather than real time tracking.
- Some zone parsing has unresolved larger-panel issues: code comments question whether larger panels have three-digit zones and need detection.
- MQTT publish queue/backpressure is not fully handled; a TODO notes that queued MQTT calls can stack up and should be addressed.
- MQTT and other switch CLI paths do not dump all switch settings when the requested switch ID is invalid; TODO comments remain.
- Twilio call formatting only supports a single message argument; splitting multiple args is TODO.
- Pushover/Twilio virtual switch partition qualification is incomplete. The changelog says Twilio is hard-coded to the default virtual partition and Pushover sends regardless of partition as long as the match passes.
- WebUI read-only state, history, system, config, and log APIs are implemented. Authenticated REST control/configuration remains a TODO, and the existing WebSocket control path still has no authentication model.
- WebUI diagnostic log redaction is incomplete: generic CLI output can retain alarm codes and integration credentials even though the Settings documentation states those values are redacted.
- HTTPS certificate provisioning and renewal are manual, and enabling HTTPS replaces rather than redirects the HTTP listener. There is no authenticated session or client-certificate mode.
- The network CLI is intentionally plain TCP/Telnet. Password and administrative traffic protection depends on an external trusted network or VPN.
- WebUI has socket-close and template-refactor FIXMEs.
- FTPD lacks relative path syntax support (`..`, `~`) and has IPv6 FIXME notes.
- Centralized HUP/restart cleanup is not implemented. ser2sock now closes its sockets through an ESP shutdown handler, but other sockets, queues, files, and integration state are not coordinated through one shutdown path.
- Static-IP Ethernet hostname setting is unresolved.
- Shared mbedTLS/HTTPS request concurrency is suspect; changelog notes a coredump when an oil-change check and Twilio request both perform web requests.
- The active uSD update path is transactionally safer but still lacks release signature, compatibility, downgrade, and rollback enforcement.
- No astyle CI check exists yet.
- No substantial automated test suite is present in the repository beyond style-install helper and CI/build references.

## Prioritized Recommendations

### Immediate Security Fixes

1. Disable FTP in generated/factory configuration. Make ACL parsing fail closed, require authentication if FTP remains, remove cleartext credentials from command output, and bind management services only to an explicitly selected interface.
2. Disable `UPGD` until the uSD updater verifies a release signature with the embedded public key. Preserve the new error handling, then add chip/board/build/version policy, downgrade controls, and tested boot rollback.
3. Add authentication and authorization before all web control operations. Use a per-device secret or provisioned user credential, authenticated sessions, command-specific authorization, rate limiting, and an explicit opt-in for panic operations.
4. Stop logging secrets at their source. Never print alarm codes, passwords, tokens, or credential-bearing URLs; make log entries typed/sensitivity-aware; deny `/api/logs` until it can guarantee redaction; and add regression fixtures for every credential format.
5. Turn on certificate verification and configure the ESP-IDF certificate bundle or pinned trust roots for every outbound TLS integration. Require `mqtts://` when MQTT commands or credentials are configured.
6. Replace unbounded Wi-Fi `strcpy` calls with bounded copies and reject values exceeding ESP-IDF limits.
7. Keep the network CLI disabled by default. Add TLS or replace it with an SSH-capable management channel before recommending use beyond a separately secured management network.
8. Preserve the new WebSocket and static-path checks and add tests for malformed frames plus plain, encoded, repeated, and mixed-separator traversal so those fixes do not regress.

### Reliability and Verification

1. Add host-side tests for AlarmDecoder parsing, partition/zone state transitions, virtual-switch regex/reset behavior, INI precedence/migration, ACL parsing, path confinement, redaction, WebSocket/MQTT command validation, and firmware-image validation.
2. Add simulator-driven integration tests for UART/socket input through MQTT and web outputs. Add hardware-in-loop smoke tests for Ethernet, Wi-Fi, attached AD2 UART, factory reset, and both successful and interrupted upgrades.
3. Restore current-commit CI first, then expand it to build both declared boards, build SPIFFS, run formatting/static analysis/tests, fail on new compiler warnings, and verify artifact sizes against partition limits.
4. Remove the duplicate SimpleIni acquisition paths: PlatformIO declares `lib_deps`, while `main/CMakeLists.txt` independently runs `FetchContent`. Pin one dependency source by immutable revision and support offline/reproducible builds.
5. Make `factory-reset` behavior explicit for removable media: refuse while an overriding uSD config exists, remove it only after explicit confirmation, or boot once ignoring it. Never enable a network administration service during reset.
6. Add queue limits/backpressure and failure metrics for MQTT and the shared outbound HTTP worker. Reproduce and fix the documented concurrent mbedTLS coredump.
7. Centralize restart/shutdown so all sockets, queues, files, and update state are closed before reboot. Retain the new ser2sock shutdown handler and add reconnect, slow-client, partial-send, and queue-overflow regression tests.

### Product and Cleanup Decisions

1. Decide whether SmartThings is supported. If yes, create a separate tested build profile and complete adoption/capabilities. If no, remove it from README/defaults/releases and move or delete its source/generated files.
2. Decide whether HTTPS OTA or local signed update is the long-term update channel. Maintain one secure primary workflow rather than a disabled signed path beside an active unsigned path.
3. Replace FTP with a small authenticated configuration/update service or require USB/uSD administration. Do not add write-capable REST endpoints until the authentication model is complete.
4. Split `sdkconfig.defaults` into explicit `webui` and optional integration profiles, and either generate `sdkconfig.esp32-poe-iso` reproducibly in CI or document it as the authoritative locked configuration.
5. Remove the legacy Makefile, obsolete SmartThings Ethernet patch, dead `#if 0` blocks, stale generated capability files, and contradictory docs after product decisions are recorded.
6. Introduce a supported-version manifest and release checklist covering firmware version, git tag, board, build profile, partition layout, migration requirements, signature, checksum, and hardware validation.

### Feature Additions After Stabilization

1. Implement signed attached-AlarmDecoder firmware updates (`ad2_fw_update`) with recovery and progress reporting.
2. Complete exit countdown tracking and three-digit/larger-panel zone parsing with captured protocol fixtures.
3. Add partition qualification to Pushover/Twilio virtual-switch notifications.
4. Implement real time-based virtual-switch auto-reset semantics and MQTT queue/backpressure reporting.
5. Finish or remove physical button, relay Output A/B, and virtual Button/Switch A/B behavior.
6. Add secure certificate enrollment/renewal and an authenticated configuration workflow only after the web security foundation is tested.

## Evidence Pointers

- Default enabled/disabled features and insecure TLS defaults: `sdkconfig.defaults`.
- Open TODOs and release notes: `CHANGELOG.md`.
- Runtime component registration and startup: `main/alarmdecoder_main.cpp`.
- Core CLI commands: `main/ad2_cli_cmd.cpp`.
- Core limits and firmware build flags: `main/ad2_settings.h`; runtime version source: `version.txt` and `main/ad2_utils.cpp`.
- Network CLI transport and authentication: `components/network_cli/network_cli.cpp`.
- MQTT feature implementation: `components/ad2mqtt/ad2mqtt.cpp`.
- webUI server and WebSocket implementation: `components/webUI/webUI.cpp`.
- Browser UI and API client: `contrib/webUI/flash-drive/www/app.html`, `app.js`, and `app.css`.
- FTP daemon implementation: `components/ftpd/ftpd.cpp`.
- ser2sock shutdown and queue handling: `components/ser2sock/ser2sock.cpp`.
- SmartThings implementation and disabled blocks: `components/stsdk/stsdk_main.cpp`.
- Twilio/SendGrid implementation: `components/twilio/twilio.cpp`.
- Pushover implementation: `components/pushover/pushover.cpp`.
- uSD update implementation: `components/usdupdate/usdupdate.cpp`.
- OTA implementation behind disabled Kconfig: `components/otaupdate/ota_util.cpp`.
