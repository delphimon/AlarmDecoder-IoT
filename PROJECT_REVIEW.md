# AlarmDecoder-IoT Project Review

Review date: 2026-08-11

Reviewed baseline: `AD2IOT-1121`, including the hardware-validated ESP-IDF 5.5 toolchain migration, authenticated outbound TLS, managed trusted time, current library pins, authenticated Web/API access, bounded TLS diagnostics, and strict SD update policy

Primary shipped target: `esp32-poe-iso` Web UI firmware

Deferred scope: SmartThings integration

## Executive Summary

AlarmDecoder-IoT is a capable ESP32 alarm-panel appliance with Ethernet/Wi-Fi, an AlarmDecoder parser, MQTT/Home Assistant support, browser controls, optional HTTPS/WSS, serial-over-TCP, serial and authenticated network CLIs, notification providers, SD/SPIFFS configuration, and SD firmware update support. The recent `AD2IOT-1108` through `AD2IOT-1121` work materially improved operator visibility, reliability, and maintainability: exact activity timestamps, readable zone cards, firmware inspection/update and restart controls, a Settings pane, bounded diagnostic history, network-CLI log access, optional rotating SD logs, TLS resource diagnostics, authenticated Web/API access, bounded configuration streaming, strict SD release ordering, an ESP-IDF 5.5/GCC 14 migration, and verified outbound TLS with trusted boot time are now present.

The Web UI's former P0 exposure is addressed in `AD2IOT-1113` through `AD2IOT-1118`: the service is disabled by default, ships without credentials, refuses invalid startup configuration, and authenticates static files, diagnostic APIs, maintenance actions, and WebSocket commands. HTTP Basic credentials establish a random reboot-scoped browser session, and browser command paths enforce same-origin and custom-header controls. Large configuration views are redacted and streamed in bounded chunks so they fit beside the persistent WSS session on the constrained ESP32. FTP is also disabled by default and fails closed on invalid credentials or access controls. `AD2IOT-1120` removes the global outbound TLS verification bypass, attaches the ESP certificate bundle to active secure clients, and establishes certificate-validity time through managed SNTP; `AD2IOT-1121` removes a legacy self-signed OTA CA from the default trust store. The device still should not be internet-exposed: Basic authentication over plain HTTP is observable, FTP/network CLI are plaintext management services, and firmware is not signed or protected by boot rollback. SD updates require a structurally valid, strictly newer numeric release, but this is an accidental-installation control rather than publisher authentication.

The dependency-free host regression suite has grown from six to forty-seven checks covering externally reachable service defaults, Web UI authorization guard placement, WSS session state/history allocation, origin/action guards, persistent cookie storage, browser credential behavior, bounded configuration streaming, traversal rejection, SD update policy, outbound TLS/time policy, hardware-smoke helpers, firmware size/identity enforcement, release-package checksums, and exact toolchain/library pins. CI validates those tests and repository inputs, compiles the primary board on pushes and pull requests, enforces RAM/flash budgets and embedded version identity, builds SPIFFS, creates checksummed/versioned packages, and reuses the same build for releases. The read-only hardware smoke tool covers verified HTTPS/WSS authentication, trusted device time, REST/configuration stability, secret redaction, version/uptime, heap retention, and the browser-equivalent two-session workload. Panel actions, trust/time failure injection, certificate renewal, Wi-Fi failover, rollback, and sustained hardware testing remain incomplete.

## Verification Performed

- Reviewed source, default and generated SDK configuration, partition layout, Web UI assets and API description, CLI/network CLI, FTP, SD update, logging, HTTPS/WSS resource handling, documentation, and both GitHub workflows.
- Confirmed `version.txt` is the firmware identity source and advanced it for every complete hardware candidate through `AD2IOT-1121`; CMake observes the file so incremental builds cannot retain the prior identity.
- Ran dependency-free repository and host regression tests, browser JavaScript syntax checking, Python compile checking, whitespace checking, the full `esp32-poe-iso` firmware and SPIFFS builds, and release-package verification. Exact final results are listed in **Validation Results**.
- Installed `AD2IOT-1115` through `AD2IOT-1121` through the validated SD updater on the Ethernet device. Exercised a public Let's Encrypt HTTPS certificate, managed SNTP time, Basic/cookie authentication, clean unauthenticated rejection, WSS sync/ping/history, the Settings APIs, network-CLI logs, repeated REST/configuration traffic, and update policy. No alarm-panel control or emergency action was sent. Wi-Fi failover, certificate renewal, invalid/expired outbound trust, NTP denial, SD failure injection, abrupt-power recovery, and rollback were not exercised.

## Highest-Priority TODOs

| Priority | Item | Why it is urgent | Recommended completion condition |
|---|---|---|---|
| P1 | Authenticate firmware installation independently | FTP now requires credentials and a valid ACL, but authenticated FTP can still stage and trigger an unsigned firmware image. | Device-enforced signing, release-channel policy, and separate update authorization; chip/project/version/downgrade checks are already enforced. |
| P1 | Enable boot recovery policy | SD images are structurally validated and constrained to newer releases, but rollback is disabled. | Rollback enabled, application-valid marking defined, and failed-boot recovery tested. |
| P1 | Establish automated behavior coverage | Security-sensitive parsers and controls currently depend on manual testing. | Host tests for protocol/config/redaction/path/action validation plus a small hardware smoke suite. |

These items are synchronized with the high-priority backlog at the top of `CHANGELOG.md`. SmartThings work is intentionally deferred.

## Detailed Findings

### P1 — FTP access is hardened, but firmware authenticity remains unresolved

- The shipped configuration and factory-reset configuration now disable FTP.
- Startup requires a username, an 8-64 character password, and a syntactically valid ACL; missing ACLs default to loopback and malformed ACL updates no longer erase the active restriction.
- FTP still uses cleartext credentials and file transfer, so it belongs only on a trusted management network.
- Authenticated FTP supports file upload plus custom restart/update commands. The SD updater verifies ESP image structure, chip/project identity, OTA operations, and strictly increasing numeric releases, but not publisher authenticity or an independently approved digest/channel manifest.

The former anonymous installation path is closed, but a compromised credential or management host can still stage and execute arbitrary ESP firmware. Device-enforced signing remains required.

### Resolved through AD2IOT-1118 — Web control and diagnostic authentication

- The sample Web UI is disabled and ships without a username or password. Missing ACLs default to loopback, and the server refuses to start with missing/invalid credentials or a malformed ACL.
- A valid HTTP Basic login establishes a random 128-bit, reboot-scoped cookie with `HttpOnly`, `SameSite=Strict`, and `Secure` under HTTPS. Every static file and REST API requires either that cookie or valid Basic credentials.
- The WebSocket upgrade authenticates once and records authorization in socket session state; commands remain unavailable until the authenticated socket also completes its partition/code synchronization.
- Browser WebSocket and maintenance requests reject mismatched origins. Restart/upgrade additionally require a custom header matching the bounded JSON action body. Static content sends CSP, anti-framing, and MIME-sniffing defenses.
- The browser explicitly uses same-origin credentials, while the existing two-connection HTTPS queue remains intact.

Important limits remain. This is a single appliance-wide operator account with no role separation or logout endpoint; changing the password and restarting revokes sessions. Basic authentication is not confidential over HTTP, so HTTPS is strongly recommended. Hardware testing verified Basic and cookie access, a clean REST 401, and authenticated WSS sync/ping/history, but did not exercise hostile origins, malformed frames, repeated failed logins, or maintenance actions. `/api/config` redacts password-like keys and reused configured secret values, and `/api/logs` applies the same text redactor, but generic/raw CLI and SD logs still cannot guarantee that every secret-shaped message is removed.

### Resolved in AD2IOT-1116 — Configuration diagnostics under TLS pressure

- `AD2IOT-1113` reproduced a software panic when the active configuration was requested while HTTPS/WSS occupied the two-session TLS budget. It also exposed a configured secret reused beneath a generic key.
- `AD2IOT-1114` removed duplicate redaction buffers and fixed HTTP error-response returns, but reserving the in-memory SimpleIni snapshot still did not fit reliably.
- `AD2IOT-1115` replaced live SimpleIni serialization with the persisted active SD/SPIFFS file, but a full 20–21 KiB response buffer could still panic.
- `AD2IOT-1116` makes two bounded passes over the selected file: the first collects sensitive values and the second redacts and streams lines as HTTP chunks. Lines are capped at 1 KiB and files at 64 KiB.

The Settings “active” view now deliberately represents the persisted boot source. Unsaved CLI changes are not visible there until `restart` persists them. On hardware, nine full configuration reads plus 25 smaller API calls completed with zero errors and no panic/OOM/watchdog log entries.

The repeatable two-session smoke test subsequently exposed a 3,548-byte heap low-water mark on `AD2IOT-1116`: WSS history held the cJSON tree, printed JSON, and a duplicate `std::string` while the REST TLS session remained live. `AD2IOT-1117` sends the printed buffer directly. The identical test then passed with a 23,608-byte minimum, no retained heap loss, and no reboot; `AD2IOT-1118` retained that stability with a 21,828-byte minimum during its final hardware run.

### Resolved in AD2IOT-1120/1121 — Outbound TLS peer verification and trusted time

- The global `CONFIG_ESP_TLS_INSECURE` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` test options are disabled.
- The common Mozilla-derived bundle is attached to queued Twilio/SendGrid/Pushover HTTPS, OTA HTTPS, MQTTS, and secure MQTT WebSocket clients. Hostname verification remains enabled. Private outbound roots remain a deliberate build-time opt-in; `AD2IOT-1121` removed a legacy unconstrained self-signed OTA CA from the production default.
- ESP-IDF 5.5's managed `esp_netif_sntp` service uses the configurable `timeserver` setting. A persistent event bit releases all secure waiters once the clock is valid; queued HTTPS and OTA fail closed after a bounded wait, while secure MQTT continues its normal reconnect cycle until time becomes valid.
- `/api/system` and the Settings pane report synchronization and current Unix time; the hardware smoke test rejects missing/stale device time.

The hardware device synchronized to within one second of the test host and retained stable inbound HTTPS/WSS behavior. Remaining failure-path coverage should deny NTP, provide an invalid/expired chain, and test a private outbound CA. The SD-loaded Let's Encrypt Web UI certificate/key are an inbound server identity and remain intentionally separate from build-time outbound trust roots.

### P1 — Firmware update recovery is incomplete

The SD update path now reports installed and card-image versions, validates size and ESP image metadata, checks all OTA writes/finalization, keeps failed images, rejects malformed/same/older releases, and exposes guarded Web UI upgrade/restart actions. Hardware tests confirmed both CLI and HTTPS report a valid older image as `downgrade=true, valid=false` and the running image as `same_version=true, valid=false`. However:

- there is no release signature or approved digest;
- chip ID, project identity, and numeric upgrade direction are checked, but there is no independently authenticated board/release-channel manifest;
- `CONFIG_APP_ROLLBACK_ENABLE` is disabled in `sdkconfig.esp32-poe-iso:2134`;
- secure boot and flash encryption are disabled.

Checksummed CI packages help detect accidental download corruption but are not a device-enforced authenticity control.

### Resolved in AD2IOT-1112 — Wi-Fi bounds and factory-reset semantics

- Wi-Fi SSID/password values are length-checked before bounded copies into zero-initialized ESP-IDF fields.
- Factory reset refuses to proceed while `/sdcard/ad2iot.ini` would override the reset unless the operator explicitly uses `factory-reset ERASE-SD`; replacement settings keep FTP disabled with a loopback ACL.
- Restart calls are not fully centralized; some resources rely on shutdown handlers while older TODOs still identify cleanup gaps.

### P2 — Network CLI and SD logging are valuable but have diagnostic limits

The network CLI is fail-closed until enabled with an 8–128 character password and defaults to a loopback ACL. It is substantially easier for post-boot diagnosis than serial access, and `logs [1-64|status|sd]` now works from both CLI transports. Limitations remain:

- management traffic and the password use plain TCP/Telnet rather than TLS or SSH;
- it cannot show bootloader output, early boot failures, network initialization failures, or a crash that takes down the network stack;
- reboot/crash breaks the session, while the in-memory ring is reboot-scoped and bounded;
- it consumes a socket, task stack, and heap on an already constrained ESP32;
- raw CLI logs may contain secrets.

Optional SD logging survives reboot and is asynchronous/rotating, but adds card wear, filesystem/write-failure modes, some heap/task/queue cost, and plaintext secret retention. Serial remains the authoritative path for boot loops and network/TLS crashes; SD logs are the best unattended supplement.

### P2 — Test and warning debt remains

- `tools/ci/tests` now has forty-seven checks covering shipped service defaults, Web UI authorization structure, browser/session/action/path protections, WSS history allocation, bounded configuration streaming, SD update policy/order/UI behavior, outbound TLS/time policy, incremental version dependencies, hardware-smoke helpers, firmware size/identity enforcement, and release-package contents/checksums. Functional parser/redaction unit tests, broader live negative authorization tests, update failure paths, and automated hardware scheduling remain.
- Excluding vendored `{fmt}` and deferred SmartThings code, active source contains 74 `TODO`/`FIXME`/`WIP` markers and five `#if 0` blocks.
- Project-owned GPIO-mask shifts now use 64-bit operands and deprecated C++ trimming adapters were replaced. Vendored SimpleIni qualifier warnings and other project warning debt remain.
- CI builds only `esp32-poe-iso`; `esp32dev` is declared but unverified in CI.

## Current Capability Inventory

### Core and connectivity

- AlarmDecoder input from local UART/GPIO or remote TCP ser2sock source.
- Ademco/DSC partition and zone parsing, event state, configurable alarm-code slots, virtual switches, and attached-AlarmDecoder configuration enforcement.
- Ethernet and Wi-Fi station modes with DHCP or static IPv4; optional ser2sock server/client.
- USB serial CLI plus an opt-in password/ACL-restricted network CLI exposing the shared command registry.

### Web application and observability

- Compact responsive dashboard, partition status, readable active-zone cards, primary arm/disarm/chime/exit/bypass actions, emergency controls, full keypad, and exact activity timestamps.
- Settings pane showing the persisted active boot-source settings, SPIFFS/SD state and redacted configuration, recent logs, build version and timestamp, network mode/protocol/IP, heap/socket/TLS diagnostics, and SD firmware status.
- Authenticated `/api/state`, `/api/history`, `/api/system`, `/api/config`, `/api/logs`, and `/api/firmware` endpoints plus authenticated, origin-checked maintenance actions.
- Optional HTTPS/WSS on port 443 using certificate-chain and private-key files beneath `/sdcard`; HTTP is used when HTTPS is disabled. TLS REST work is serialized around the persistent WSS session to stay within the configured two-session TLS budget.
- Static Web UI/OpenAPI assets under `contrib/webUI/flash-drive/www`, now included in release packages with the `certs` instructions.

### Integrations, storage, and update

- MQTT state/discovery and optional command topics; Pushover and Twilio/SendGrid notification paths.
- Human-readable `ad2iot.ini` loaded from SD first, then SPIFFS, with CLI configuration and FTP file access.
- Reboot-scoped activity and diagnostic rings, CLI/Web UI access to recent logs, and optional asynchronous rotating `/sdcard/ad2iot.log` persistence.
- SD firmware inspection through `versionusd` and the Web UI, plus guarded `upgradeusd` and restart actions.
- Dual OTA application partitions, SPIFFS, NVS, coredump storage, and removable FAT/SD storage.

## Build, Release, and Workflow Review

The workflows were modernized in `AD2IOT-1111` and made clean-runner compatible in `AD2IOT-1112`:

- `.github/workflows/build.yml` now runs on pull requests as well as pushes/manual calls and can be invoked as a reusable workflow.
- PlatformIO Core is pinned to 6.1.19 and Python to 3.12. The ESP-IDF 5.5 migration removed the former `pkg_resources`/setuptools compatibility shim. `IntelHex` 2.3.0 is pinned and import-checked because PlatformIO's esptool 4.11 package can otherwise omit that runtime on a clean runner. PlatformIO caches are versioned by complete toolchain input and no longer restore broadly across incompatible package sets. GitHub Actions use explicit release versions rather than floating old majors.
- Least-privilege permissions, concurrency control, timeouts, PlatformIO package caching, version/changelog/web validation, host regression tests, and JavaScript syntax checking are enforced.
- The build log is checked against explicit 98,304-byte static-RAM and 1,650,000-byte application-flash budgets before packaging.
- The ESP application descriptor is parsed after compilation, preventing a stale binary with an embedded version that differs from `version.txt` from reaching a package or release.
- The firmware and SPIFFS builds share one job. Packaging fails if any required binary is absent, includes `version.txt` and the SD web/certificate bundle, emits `SHA256SUMS`, and uses versioned artifact names.
- `.github/workflows/release.yml` calls the same build workflow, downloads that exact artifact, creates a versioned archive, and uploads it to the published release. This removes the previous duplicated build/package implementation.
- `.github/dependabot.yml` schedules monthly GitHub Actions update checks.

Remaining CI opportunities, in priority order:

1. Add functional protocol-parser and redaction tests, then run live negative authorization tests against hardware.
2. Add a separate `esp32dev` compile job or formally remove the target if unsupported.
3. Run `actionlint` and a non-mutating C/C++ formatting check in CI.
4. Extend the new static RAM/application flash budgets with runtime TLS heap-low-water thresholds from hardware tests.
5. Pin third-party actions to immutable commit SHAs and consider build provenance/SBOM generation for releases.
6. Add a hardware-in-loop smoke workflow for Ethernet, HTTPS/WSS REST concurrency, SD read/write/update failure, restart, and serial/network CLI access.

## Toolchain Currency and Upgrade Plan

| Component | Repository / workflow | Current upstream reviewed | Assessment and action |
|---|---:|---:|---|
| PlatformIO Core | 6.1.19 | 6.1.19 | Now pinned and current; CI also pins the required IntelHex 2.3.0 esptool runtime. |
| Python in CI | 3.12 | Newer Python versions are available | Supported and reproducible; no framework-specific setuptools shim remains. |
| PlatformIO Espressif32 | 6.13.0 | 6.13.0 (ESP-IDF 5.5.3) and 7.0.x (ESP-IDF 6.0) | Upgraded. Treat 7.x/IDF 6 as a separate breaking migration. |
| ESP-IDF | 5.5.3 | 5.5.x supported; 6.0.x current major | Upgraded to the current PlatformIO 6.x line; generated configuration and hardware behavior require regression validation before IDF 6. |
| SimpleIni | 4.26, commit `877f735` | 4.26 | Upgraded and linked through its canonical header-only CMake target from one exact dependency source. |
| `{fmt}` submodule | 12.2.0, commit `1be298e` | 12.2.0 | Upgraded; `fmt::runtime` behavior retained and GCC 14 compatibility restored. |
| GitHub Actions | checkout 6.0.2, setup-python 7.0.0, cache 5.0.5, upload-artifact 7.0.1, download-artifact 8.0.1 | Same reviewed releases | Updated; Dependabot will keep these visible. Node 24 actions require runner 2.327.1+, which GitHub-hosted `ubuntu-latest` satisfies. |

Primary version sources reviewed: [PlatformIO Espressif32 releases](https://github.com/platformio/platform-espressif32/releases), [ESP-IDF releases](https://github.com/espressif/esp-idf/releases), [PlatformIO Core releases](https://github.com/platformio/platformio-core/releases), [SimpleIni releases](https://github.com/brofield/simpleini/releases), [{fmt} releases](https://github.com/fmtlib/fmt/releases), and the official GitHub Actions release pages.

`AD2IOT-1119` combines the closely coupled framework/compiler compatibility fixes and library pins in one hardware candidate because GCC 14 could not compile the old `{fmt}` release and SimpleIni 4.26 changed to a canonical header-only CMake integration. ESP-IDF 6 remains intentionally separate: ESP-MQTT moves to a managed component, additional legacy compatibility is removed, and its resource impact needs a new baseline.

### API migration findings

- GCC 14/ESP-IDF 5.5 identified the removed compatibility path first: `eth_esp32_emac_config_t::smi_mdc_gpio_num` and `smi_mdio_gpio_num` were replaced with `smi_gpio.mdc_num` and `smi_gpio.mdio_num`.
- Project-owned code does not directly include the deprecated ADC, DAC, I2C, I2S, MCPWM, PCNT, RMT, or legacy timer headers. Their `driver/deprecated` objects appearing in a verbose link are ESP-IDF compatibility components, not evidence that AlarmDecoder-IoT calls those APIs.
- Millisecond delays and UART read timeouts now use `pdMS_TO_TICKS`; the remaining `portTICK_PERIOD_MS` use converts an already tick-based socket timeout into `timeval` units and is intentional.
- `esp_http_client`, the IDF 5.5 nested `esp_mqtt_client_config_t` layout, `esp_vfs_fat_sdmmc_mount`, and the streaming `mbedtls_sha256_*` functions used by image validation are supported in the selected framework and produced no deprecation diagnostics. PSA Crypto could replace direct SHA contexts, but that is not currently a functional or resource win for sequential firmware hashing.
- Component `CMakeLists.txt` files no longer invoke nested `project()` commands, dead pre-IDF-4.1 certificate embedding is removed, UART configuration is stack allocated, and inactive SmartThings Kconfig/CMake dependencies no longer participate in the primary build.
- ESP-IDF log v2, binary logging, managed SNTP, certificate-bundle attachment, and newer driver lifecycle APIs are available. Managed SNTP and bundle attachment are now active; Log v1 is deliberately retained until size/runtime measurements exist.

## Optimization Opportunities

- Continue replacing large dynamic `std::string`/JSON construction on hot paths with bounded or chunked processing, guided by heap-low-water telemetry. Configuration delivery is now chunked; other large diagnostics should be profiled before changing them.
- Keep HTTPS requests serialized and expose peak/low-water heap per TLS operation. The two-session design now passes the repeatable hardware smoke threshold but still needs sustained and failure-injection testing.
- Add app-size and static-RAM reports to CI so toolchain/library upgrades cannot silently consume the OTA or TLS headroom.
- Consolidate restart entry points around one shutdown coordinator for Web UI, ser2sock, network CLI, FTP, MQTT, and SD logging.
- Keep project-owned code warning-clean under GCC 14 before enabling warnings-as-errors globally; deferred SmartThings and third-party source still need their own policy.
- Add automated trust/time failure-path coverage: deny NTP, present invalid/expired certificates, and verify a build-time private outbound CA without weakening hostname checks.
- Evaluate ESP-IDF log v2 and binary logging only with measured flash/RAM/runtime cost; the migration deliberately retains log v1 for compatibility and constrained-device stability.
- Before ESP-IDF 6, move ESP-MQTT to a pinned managed component and audit network/storage teardown against the newer driver lifecycle APIs.
- Avoid synchronous SD work on request/control paths; retain the asynchronous queue and report queue drops/write failures as already implemented.
- Separate active production components from deferred/experimental integrations in documentation and build configuration. SmartThings can remain source-retained without blocking the main firmware roadmap.

## Recommended Execution Order

1. Add device-enforced firmware signing, independent update authorization, a signed release-channel manifest, and boot rollback.
2. Add functional parser/redaction tests and hardware negative tests for the new HTTP/WSS authorization boundary.
3. Centralize restart cleanup and exercise update/restart recovery on hardware.
4. Expand hardware-in-loop coverage, trust/time failure injection, and runtime size/heap gates.
5. Move ESP-MQTT to a pinned managed component, then evaluate ESP-IDF 6 as a separate migration.

## Validation Results

This section records the final repository, build, and hardware checks for `AD2IOT-1121`.

- `tools/ci/validate_project.py`: pass.
- `python -m unittest discover -s tools/ci/tests -p "test_*.py"`: pass; forty-seven security-default, Web authorization/session, WSS history allocation, bounded-config, SD version-policy/order/UI, authenticated outbound TLS/time, incremental-version dependency, hardware-helper, firmware size/identity, bounded-copy, factory-reset, packaging, and exact toolchain/dependency regression tests.
- `node --check contrib/webUI/flash-drive/www/app.js`: pass.
- `python -m compileall -q tools/ci tools/hil`: pass.
- `git diff --check`: pass.
- `pio run -e esp32-poe-iso`: pass on PlatformIO Espressif32 6.13.0, ESP-IDF 5.5.3, GCC 14.2, CMake 3.30.2, and esptool 4.11 without project-source compiler warnings. The build used 63,120 bytes static RAM (19.3%) and 1,601,473 bytes application flash (87.3%), below the 98,304/1,650,000-byte CI budgets. Authenticated TLS/time adds 112 bytes RAM and approximately 24.1 KiB linked flash versus the documented AD2IOT-1119 baseline; removing the legacy CA saves 132 bytes versus AD2IOT-1120 and leaves 48,527 bytes of the conservative application-flash budget.
- `pio run -e esp32-poe-iso -t buildfs`: pass.
- Firmware image inspection: pass; `firmware.bin` contains `AD2IOT-1121` and the `Aug 11 2026 22:59:06` build timestamp; the validated image is 1,601,872 bytes.
- `tools/ci/package_release.py` smoke test: pass; required binaries, SD-card bundle, and checksum manifest verified.
- `actionlint` 1.7.12 against both workflow files: pass; the downloaded binary matched its published SHA-256 manifest.
- GitHub-hosted execution: the Python 3.12/setuptools compatibility hotfix passed in [workflow run 31519749844](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/31519749844). The tagged `AD2IOT-1112` release build, tests, SPIFFS image, package, and Actions artifact passed in [workflow run 31527943625](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/31527943625). Its initial release-asset step exposed missing repository context in `gh release upload`; commit `9d01f02` supplies `--repo`, adds tag/version validation, and passed the subsequent [master build](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/31529380757). The verified 41-file Actions artifact was attached to the release and its public download hash was rechecked.
- GitHub-hosted execution: the first ESP-IDF 5.5 run exposed PlatformIO esptool 4.11's missing `IntelHex` runtime on the clean Ubuntu runner. Commit `c995f33` pins/import-checks IntelHex and isolates the cache from incompatible package restores; all validation, 40 host tests, clean firmware/SPIFFS builds, identity/size budgets, packaging, checksums, and artifact upload then passed in [workflow run 31565446903](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/31565446903).
- Latest GitHub-hosted execution: commit `848e422` passed all 47 host tests, the clean AD2IOT-1121 firmware/SPIFFS builds, embedded identity and resource budgets, 41-file packaging/checksums, and upload of the `AD2IOT-1121-Release-Package` artifact in [workflow run 31569058870](https://github.com/delphimon/AlarmDecoder-IoT/actions/runs/31569058870).
- Hardware update: the AD2IOT-1120 device's `versionusd` validated AD2IOT-1121's identity, project, build timestamp, and 1,601,872-byte size before `upgradeusd`; the device returned on AD2IOT-1121 and removed `firmware.bin` after success.
- Hardware HTTPS/WSS/time: the public Let's Encrypt certificate was accepted; authenticated WSS opened in 1,531 ms and returned JSON plus pong with no protocol error. The device reported synchronized UTC with zero-second test-host skew. Twenty-seven full configuration reads completed with 1,141 ms median/1,886 ms p95, and 25 smaller API calls completed with 63 ms median/79 ms p95.
- Hardware configuration: active, SPIFFS, and SD views returned 20,439–21,249 byte redacted responses. The apparent SPIFFS secret match was confirmed to be the configured word appearing only in documentation/key text; its actual assignment was `[redacted]` and no configured assigned value leaked.
- Hardware workload: the dependency-free smoke tool kept authenticated WSS open across all 52 REST requests with zero failures; post-WSS free heap changed by -244 bytes and minimum free heap was 17,920 bytes. A separate authenticated network-CLI scan found no panic, watchdog, Guru Meditation, OOM, assertion, abort, certificate-verification, or time-synchronization-failure signature.
