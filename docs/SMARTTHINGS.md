# SmartThings Direct-Connected firmware

The ESP32-POE-ISO SmartThings firmware is built separately from the PlatformIO webUI firmware. It follows the SmartThings direct-connected C SDK application pattern and does not modify SDK core code.

## Pinned environment

- SmartThings reference project: commit `d9450bb5dd4e1f6f25bc7987dc3192f4f5e17b3f`
- SmartThings Device SDK C v2.3.2: commit `e01b64f63c078cf0caca610dd35a39e63e1ea824`
- ESP-IDF 5.0.7: commit `e5617c26f7fec52b05e87c45062510122fb9ea05`

CI checks these revisions before building. The SmartThings artifact contains `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`, `spiffs.bin`, and `application.bin`. The checked-in 4 MB layout reserves STNV at `0x9000`, and each OTA slot is `0x1e0000` bytes.

## AD2IOTV10 profile

The active components are refresh, health check, firmware update, power, battery, chime, fire, alarm, auxiliary alarm, arm stay, arm away, ready, bypass, disarm, and exit. Disabled experimental security-system, button, switch, and output capabilities remain disabled. Chime, arm stay/away, disarm, and exit route to the real AlarmDecoder command functions. Fire, police/panic, and auxiliary panic retain three-press protection and must only be exercised with isolated test doubles—never against a live panel.

State is published after it changes or when refresh explicitly requests a resync. Only `ST_DEVICE_STATUS_CLOUD_CONNECTED` marks the application network-ready; onboarding and disconnected states clear readiness so the AlarmDecoder socket reconnects cleanly.

## STNV identity provisioning

Identity never belongs in `ad2iot.ini`, build logs, device-info data, or Git. The device-info JSON generated at boot contains only `firmwareVersion`. Keep the externally supplied identity JSON untracked and use:

```powershell
python tools/provision_stnv.py C:\secure\AD2IOTV10.st-identity.json --idf-path C:\esp\esp-idf --port COM7
```

The helper validates the ED25519 serial/public/private values, builds a temporary 16 KiB STNV image with ESP-IDF's NVS generator, flashes it at `0x9000`, masks the serial in its sole status line, and removes the temporary CSV/image. `smartthings cleanup` clears onboarding and cloud registration but does not erase STNV.

## BLE adoption and AlarmDecoder topology

Enable developer mode in the SmartThings app and add AD2IOTV10 from My Testing Devices. Onboarding uses BLE and 2.4 GHz Wi-Fi. The SmartThings unit identifies itself as `ad2iot-stsdk` and its bundled configuration is:

```text
netmode N
ad2source SOCK ad2iot.lan:10000
partition 1 18
smartthings enable Y
```

The separate AlarmDecoder/webUI unit exposes its AD2 stream with ser2sock on TCP port 10000. `ad2source SOCK` accepts DNS/mDNS names, IPv4 literals, and bracketed IPv6 literals such as `[2001:db8::10]:10000`; it retries every resolved address and reconnects after Wi-Fi/cloud or server interruption.

## Signed test OTA

The AlarmDecoder manifest URL, signed-image URL template, and repository public key remain the defaults. Override URLs with `CONFIG_AD2IOT_OTA_MANIFEST_URL` and `CONFIG_AD2IOT_OTA_IMAGE_URL_TEMPLATE`; pass an external verification key with CMake cache variable or environment variable `AD2IOT_OTA_PUBLIC_KEY_FILE`. The URL template must be HTTPS and contain exactly one `%s` build-flag placeholder.

Create the format expected by the updater with an external RSA-2048 private key:

```powershell
python tools/package_signed_ota.py build\application.bin --private-key C:\secure\test.ota-signing-key.pem --output hosted\signed_alarmdecoder_stsdk_esp32.bin
```

Host that image and the version manifest on HTTPS. The updater validates manifest structure, HTTPS configuration, image size, complete trailer framing, and RSA signature before selecting the OTA partition. Failure or a concurrent request does not reboot. Do not commit the private signing key or identity file.

## Hardware validation safety

Record the COM port and hosted HTTPS URLs at execution time. Safe live controls are refresh, chime, arm stay/away, disarm, and exit. Do not send fire, police/panic, or auxiliary panic commands to a live Vista panel. Verify adoption persistence, cleanup/re-adoption, hostname, state flow, independent Wi-Fi/cloud and ser2sock recovery, signature rejection, and a controlled two-version OTA before relying on the firmware.

This restoration is a development integration. WWST certification, secure boot, flash encryption, eFuse changes, and commercial-production hardening are outside its scope.
