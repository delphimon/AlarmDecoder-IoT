# AlarmDecoder-IoT Project Review

Review date: 2026-08-11

Reviewed baseline: commit `17f55d5` plus the `AD2IOT-1111` workflow, packaging, and documentation changes in this working tree

Primary shipped target: `esp32-poe-iso` Web UI firmware

Deferred scope: SmartThings integration

## Executive Summary

AlarmDecoder-IoT is a capable ESP32 alarm-panel appliance with Ethernet/Wi-Fi, an AlarmDecoder parser, MQTT/Home Assistant support, browser controls, optional HTTPS/WSS, serial-over-TCP, serial and authenticated network CLIs, notification providers, SD/SPIFFS configuration, and SD firmware update support. The recent `AD2IOT-1108` through `AD2IOT-1110` work materially improved operator visibility: exact activity timestamps, readable zone cards, firmware inspection/update and restart controls, a Settings pane, bounded diagnostic history, network-CLI log access, optional rotating SD logs, and TLS resource diagnostics are now present.

The current build is usable on a trusted private network, but it is not yet safe to expose to an untrusted LAN or the internet. The two most urgent risks are unauthenticated FTP/firmware installation and unauthenticated web alarm control. Outbound TLS peer verification is globally disabled, firmware is not signed and has no rollback policy, and a factory reset can retain the higher-priority SD configuration while enabling FTP in the replacement SPIFFS configuration.

The project has no project-authored automated behavior tests. The updated CI now validates inputs, compiles the primary board on pushes and pull requests, builds SPIFFS, creates checksummed/versioned packages, and reuses the same build for releases. That is a meaningful improvement, but it proves compilation and packaging—not alarm behavior, authorization boundaries, update recovery, TLS stability, or hardware operation.

## Verification Performed

- Reviewed source, default and generated SDK configuration, partition layout, Web UI assets and API description, CLI/network CLI, FTP, SD update, logging, HTTPS/WSS resource handling, documentation, and both GitHub workflows.
- Confirmed `version.txt` is the firmware identity source and advanced it to `AD2IOT-1111` for this completed build cycle.
- Ran the repository’s new dependency-free validation, browser JavaScript syntax check, Python compile check, whitespace check, the full `esp32-poe-iso` firmware build, SPIFFS build, and a release-package smoke test. Exact final results are listed in **Validation Results**.
- No firmware was flashed during this review. Panel interaction, Ethernet/Wi-Fi failover, HTTPS/WSS under browser load, certificate renewal, SD-card failures, software restart behavior, and OTA rollback were not exercised on hardware.

## Highest-Priority TODOs

| Priority | Item | Why it is urgent | Recommended completion condition |
|---|---|---|---|
| P0 | Authenticate web controls and diagnostic APIs | Any host admitted by the Web UI ACL can arm/disarm, bypass zones, trigger alarm commands, restart, or request an SD upgrade. Diagnostics can expose operational or secret data. | Authenticated sessions, CSRF/replay protection, per-action authorization, secure defaults, and negative integration tests. |
| P0 | Secure or remove FTP firmware installation | FTP is enabled in the shipped sample configuration and after factory reset, has no wired credentials, and can upload and install firmware. | FTP disabled by default or credentials mandatory; update command independently authorized; anonymous install impossible. |
| P1 | Require outbound TLS peer verification | `CONFIG_ESP_TLS_INSECURE` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` weaken MQTT and notification HTTPS clients. | Trust bundle/time handling verified on device; insecure options disabled; failure-path tests added. |
| P1 | Sign firmware and enable recovery policy | SD images are structurally validated but not authenticated; rollback is disabled. | Signed images, board/channel/downgrade policy, rollback enabled, failed-boot recovery tested. |
| P1 | Bound Wi-Fi credentials | `strcpy` copies configuration-controlled strings into fixed ESP-IDF fields. | Length-checked copies with explicit termination and boundary tests. |
| P1 | Correct factory-reset semantics | SD configuration overrides SPIFFS, but reset removes only SPIFFS and writes an FTP-enabled replacement. | Explicit keep/remove-SD policy, warning/confirmation, safe defaults, and reboot test with SD inserted. |
| P1 | Establish automated behavior coverage | Security-sensitive parsers and controls currently depend on manual testing. | Host tests for protocol/config/redaction/path/action validation plus a small hardware smoke suite. |

These items are synchronized with the high-priority backlog at the top of `CHANGELOG.md`. SmartThings work is intentionally deferred.

## Detailed Findings

### P0 — FTP permits unauthenticated configuration and firmware replacement

- `data/ad2iot.ini:359-365` enables FTP in the shipped SPIFFS image.
- `main/device_control.cpp:343-360` writes `enable = true` for FTP during factory reset.
- The FTP class has `setCredentials()`, but `ftpd_init()` at `components/ftpd/ftpd.cpp:1865` never applies credentials.
- If no ACL is configured, the server defaults to `0.0.0.0/0` at `components/ftpd/ftpd.cpp:1883`.
- FTP supports file upload plus custom restart/update commands. The SD updater verifies ESP image structure and OTA operations, but not publisher authenticity, expected digest, board policy, or downgrade policy.

An allowed network client can replace configuration or stage and execute arbitrary ESP firmware. ACLs reduce exposure but are not authentication.

### P0 — Web controls and sensitive diagnostics have no user authentication

- The WebSocket and maintenance action handlers invoke alarm controls, restart, and SD upgrade after only IP/CIDR ACL admission (`components/webUI/webUI.cpp:418` and `:833`).
- The fallback Web UI ACL is `0.0.0.0/0` (`components/webUI/webUI.cpp:64`); the shipped sample is narrower but still subnet-wide (`data/ad2iot.ini:382-394`).
- HTTPS/WSS encrypts transport but does not authenticate an operator.
- `/api/config` applies useful key/value redaction. `/api/logs` applies the same text redactor, but generic log messages do not always use `key=value` or URL credential forms. Serial/network CLI `logs` output is raw. Alarm codes and provider credentials printed by configuration commands can therefore enter the reboot-scoped ring or optional SD log without guaranteed redaction.

The Web UI should not be internet-exposed, and subnet ACLs should be treated as a temporary containment measure.

### P1 — Outbound TLS clients do not verify peers

`sdkconfig.defaults:85-86` enables insecure ESP-TLS and skips server-certificate verification. This affects outbound HTTPS and TLS consumers such as MQTT, Twilio, SendGrid, Pushover, and the disabled network OTA path. The inbound Web UI’s custom certificate support is separate and does not correct outbound verification.

### P1 — Firmware update recovery is incomplete

The SD update path now reports installed and card-image versions, validates size and ESP image metadata, checks all OTA writes/finalization, keeps failed images, and exposes guarded Web UI upgrade/restart actions. However:

- there is no release signature or approved digest;
- there is no board, release-channel, or downgrade policy;
- `CONFIG_APP_ROLLBACK_ENABLE` is disabled in `sdkconfig.esp32-poe-iso:1703`;
- secure boot and flash encryption are disabled.

Checksummed CI packages help detect accidental download corruption but are not a device-enforced authenticity control.

### P1 — Configuration can cause memory corruption or survive reset unexpectedly

- Wi-Fi SSID/password values are copied with unbounded `strcpy` at `main/device_control.cpp:509` and `:515`.
- Configuration loading prefers SD over SPIFFS (`main/ad2_utils.cpp:337-350`), while factory reset only unlinks/recreates SPIFFS (`main/device_control.cpp:348-360`). A mounted SD card can therefore restore what the operator expected reset to remove.
- Restart calls are not fully centralized; some resources rely on shutdown handlers while older TODOs still identify cleanup gaps.

### P2 — Network CLI and SD logging are valuable but have diagnostic limits

The network CLI is fail-closed until enabled with an 8–128 character password and defaults to a loopback ACL. It is substantially easier for post-boot diagnosis than serial access, and `logs [1-64|status|sd]` now works from both CLI transports. Limitations remain:

- management traffic and the password use plain TCP/Telnet rather than TLS or SSH;
- it cannot show bootloader output, early boot failures, network initialization failures, or a crash that takes down the network stack;
- reboot/crash breaks the session, while the in-memory ring is reboot-scoped and bounded;
- it consumes a socket, task stack, and heap on an already constrained ESP32;
- raw CLI logs may contain secrets.

Optional SD logging survives reboot and is asynchronous/rotating, but adds card wear, filesystem/write-failure modes, some heap/task/queue cost, and plaintext secret retention. Serial remains the authoritative path for boot loops and network/TLS crashes; SD logs are the best unattended supplement.

### P2 — Test and warning debt remains high

- `test/` contains only an astyle installer; there are no project-authored unit or integration tests.
- Excluding vendored `{fmt}` and deferred SmartThings code, active source contains 74 `TODO`/`FIXME`/`WIP` markers and five `#if 0` blocks.
- The current build reports warnings including a left shift whose count can exceed the source type width, unused variables, deprecated C++ trimming adapters, and SimpleIni return-type qualifiers. The shift warning in `hal_ad2_reset()` deserves priority because it can produce an invalid GPIO mask.
- CI builds only `esp32-poe-iso`; `esp32dev` is declared but unverified in CI.

## Current Capability Inventory

### Core and connectivity

- AlarmDecoder input from local UART/GPIO or remote TCP ser2sock source.
- Ademco/DSC partition and zone parsing, event state, configurable alarm-code slots, virtual switches, and attached-AlarmDecoder configuration enforcement.
- Ethernet and Wi-Fi station modes with DHCP or static IPv4; optional ser2sock server/client.
- USB serial CLI plus an opt-in password/ACL-restricted network CLI exposing the shared command registry.

### Web application and observability

- Compact responsive dashboard, partition status, readable active-zone cards, primary arm/disarm/chime/exit/bypass actions, emergency controls, full keypad, and exact activity timestamps.
- Settings pane showing current redacted settings, SPIFFS/SD state and configuration, recent logs, build version and timestamp, network mode/protocol/IP, heap/socket/TLS diagnostics, and SD firmware status.
- Read-only `/api/state`, `/api/history`, `/api/system`, `/api/config`, `/api/logs`, and `/api/firmware` endpoints plus guarded maintenance actions.
- Optional HTTPS/WSS on port 443 using certificate-chain and private-key files beneath `/sdcard`; HTTP is used when HTTPS is disabled. TLS REST work is serialized around the persistent WSS session to stay within the configured two-session TLS budget.
- Static Web UI/OpenAPI assets under `contrib/webUI/flash-drive/www`, now included in release packages with the `certs` instructions.

### Integrations, storage, and update

- MQTT state/discovery and optional command topics; Pushover and Twilio/SendGrid notification paths.
- Human-readable `ad2iot.ini` loaded from SD first, then SPIFFS, with CLI configuration and FTP file access.
- Reboot-scoped activity and diagnostic rings, CLI/Web UI access to recent logs, and optional asynchronous rotating `/sdcard/ad2iot.log` persistence.
- SD firmware inspection through `versionusd` and the Web UI, plus guarded `upgradeusd` and restart actions.
- Dual OTA application partitions, SPIFFS, NVS, coredump storage, and removable FAT/SD storage.

## Build, Release, and Workflow Review

The workflows were modernized in `AD2IOT-1111`:

- `.github/workflows/build.yml` now runs on pull requests as well as pushes/manual calls and can be invoked as a reusable workflow.
- PlatformIO Core is pinned to 6.1.19 and Python to 3.13; GitHub Actions use current explicit release versions rather than floating old majors.
- Least-privilege permissions, concurrency control, timeouts, PlatformIO package caching, version/changelog/web validation, and JavaScript syntax checking were added.
- The firmware and SPIFFS builds share one job. Packaging fails if any required binary is absent, includes `version.txt` and the SD web/certificate bundle, emits `SHA256SUMS`, and uses versioned artifact names.
- `.github/workflows/release.yml` calls the same build workflow, downloads that exact artifact, creates a versioned archive, and uploads it to the published release. This removes the previous duplicated build/package implementation.
- `.github/dependabot.yml` schedules monthly GitHub Actions update checks.

Remaining CI opportunities, in priority order:

1. Add host-side tests and require them before firmware compilation.
2. Add a separate `esp32dev` compile job or formally remove the target if unsupported.
3. Run `actionlint` and a non-mutating C/C++ formatting check in CI.
4. Add firmware size budgets and fail on dangerous growth in app, static RAM, or TLS-related allocations.
5. Pin third-party actions to immutable commit SHAs and consider build provenance/SBOM generation for releases.
6. Add a hardware-in-loop smoke workflow for Ethernet, HTTPS/WSS REST concurrency, SD read/write/update failure, restart, and serial/network CLI access.

## Toolchain Currency and Upgrade Plan

| Component | Repository / workflow | Current upstream reviewed | Assessment and action |
|---|---:|---:|---|
| PlatformIO Core | 6.1.19 | 6.1.19 | Now pinned and current. |
| Python in CI | 3.13 | PlatformIO 6.1.19 also supports 3.14 | 3.13 is a conservative supported choice for the older ESP-IDF package set. |
| PlatformIO Espressif32 | 6.4.0 | 6.13.0 (ESP-IDF 5.5.3) and 7.0.0 (ESP-IDF 6.0) | High-value upgrade. Move first to 6.13/IDF 5.5.3; treat 7.0/IDF 6 as a separate breaking migration. |
| ESP-IDF | 5.1.1 | 5.5.3 supported; 6.0.x current major | 5.1.1 is old and misses years of fixes. Upgrade with warning cleanup and hardware regression testing. |
| SimpleIni | 4.19 | 4.26 | Compatibility-test 4.26; pin to a release tag or commit after the firmware build passes. |
| `{fmt}` submodule | 8.0.1 | 12.2.0 | Several majors behind. Upgrade separately because API/compile behavior changed and this runs on constrained firmware. |
| GitHub Actions | checkout 6.0.2, setup-python 7.0.0, cache 5.0.5, upload-artifact 7.0.1, download-artifact 8.0.1 | Same reviewed releases | Updated; Dependabot will keep these visible. Node 24 actions require runner 2.327.1+, which GitHub-hosted `ubuntu-latest` satisfies. |

Primary version sources reviewed: [PlatformIO Espressif32 releases](https://github.com/platformio/platform-espressif32/releases), [ESP-IDF releases](https://github.com/espressif/esp-idf/releases), [PlatformIO Core releases](https://github.com/platformio/platformio-core/releases), [SimpleIni releases](https://github.com/brofield/simpleini/releases), [{fmt} releases](https://github.com/fmtlib/fmt/releases), and the official GitHub Actions release pages.

Do not combine the framework, SimpleIni, and `{fmt}` major upgrades in one change. First make the present build warning-clean and add parser/config smoke tests; then move to Espressif32 6.13, validate on hardware, and only afterward evaluate ESP-IDF 6.

## Optimization Opportunities

- Replace repeated dynamic `std::string`/JSON construction on hot paths with bounded buffers or reserved capacities, guided by heap-low-water telemetry rather than blanket rewrites.
- Keep HTTPS requests serialized and expose peak/low-water heap per TLS operation; the recent two-session budget fix is appropriate for the ESP32 but needs sustained hardware testing.
- Add app-size and static-RAM reports to CI so toolchain/library upgrades cannot silently consume the OTA or TLS headroom.
- Consolidate restart entry points around one shutdown coordinator for Web UI, ser2sock, network CLI, FTP, MQTT, and SD logging.
- Replace deprecated `std::ptr_fun`/`std::not1`, fix the GPIO shift type, and clear warnings before enabling warnings-as-errors for project-owned code.
- Avoid synchronous SD work on request/control paths; retain the asynchronous queue and report queue drops/write failures as already implemented.
- Separate active production components from deferred/experimental integrations in documentation and build configuration. SmartThings can remain source-retained without blocking the main firmware roadmap.

## Recommended Execution Order

1. Close the two P0 network-control/update findings or keep the device isolated on a tightly controlled management VLAN.
2. Fix the Wi-Fi overflow and factory-reset behavior; these are small changes with high reliability value.
3. Add parser, configuration, redaction, path, and action-authorization tests.
4. Enable outbound certificate verification and implement firmware signature/rollback policy.
5. Clean current compiler warnings and migrate to Espressif32 6.13 / ESP-IDF 5.5.3.
6. Add hardware-in-loop checks, then evaluate library upgrades and ESP-IDF 6.

## Validation Results

This section records repository and host checks only; hardware validation is still required.

- `tools/ci/validate_project.py`: pass.
- `node --check contrib/webUI/flash-drive/www/app.js`: pass.
- `python -m compileall -q tools/ci`: pass.
- `git diff --check`: pass.
- `pio run -e esp32-poe-iso`: pass with warnings listed above; 62,628 bytes static RAM (19.1%) and 1,534,417 bytes application flash (83.6%).
- `pio run -e esp32-poe-iso -t buildfs`: pass.
- Firmware image inspection: pass; `firmware.bin` contains `AD2IOT-1111` and the `Aug 11 2026` build date.
- `tools/ci/package_release.py` smoke test: pass; required binaries, SD-card bundle, and checksum manifest verified.
- `actionlint` 1.7.12 against both workflow files: pass; the downloaded binary matched its published SHA-256 manifest.
- GitHub-hosted execution: not run from this local review. The workflow definitions were checked locally, but the first remote run remains the final confirmation of runner/action permissions and artifact transfer.
