mbedTLS certificates
***
ESP-IDF's certificate-bundle API links trusted roots into the firmware for outbound HTTPS, MQTTS, and WSS server authentication. The common Mozilla-derived bundle is enabled by default. https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_crt_bundle.html

Private outbound CA roots are opt-in build inputs: place only the required PEM root certificates in this directory, enable `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`, and rebuild. Do not place private keys here. The former self-signed OTA placeholder is deliberately not trusted by production builds because a CA root can authenticate every outbound hostname unless it has explicit name constraints.

This directory is not used for the Web UI's inbound HTTPS identity. The Web UI loads its Let's Encrypt-compatible certificate chain and private key from `/sdcard/certs/fullchain.pem` and `/sdcard/certs/privkey.pem` (or the configured `sslcert`/`sslkey` paths), so those can be renewed without rebuilding firmware.

Certificate validity checks require the device clock. Configure `timeserver` in `ad2iot.ini` and allow DNS plus outbound NTP/UDP port 123. Secure clients fail closed while time is unavailable.

   Example using openssl command on Linux.
```console
openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null
```
