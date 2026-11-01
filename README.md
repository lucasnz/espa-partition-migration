# ESPySpa espa-v2 → default_8MB OTA migration firmware

This project contains one small firmware codebase with two PlatformIO build environments:

- `migrator`: built with the existing `espa-v2.csv` layout and installed using the existing firmware's normal `/fota` application update.
- `bridge`: built with `default_8MB.csv`. It is the same source code, staged by the migrator at the future `app1` address (`0x340000`). Once the new partition table is active, it runs as the temporary bridge and accepts the final production firmware/filesystem.

**The existing ESPySpa firmware does not need a new migration endpoint.** It only needs to be able to install an ordinary application `.bin` through its current `/fota` route and reboot into the newly selected OTA slot after a successful upload. Your supplied handler excerpt does not show the reboot, so check whether that happens elsewhere; if not, add a delayed restart after sending the successful POST response. The migrator then exposes its own endpoints.

## Important safety limitations

1. **Partition-table changes are not power-fail atomic.** The partition table is one 4 KB flash sector at `0x8000`. The migrator backs up the old table and NVS and writes the new OTA metadata before replacing the table, but a power loss while the table sector is being erased/programmed can still leave the device unable to boot. Do not migrate during an unstable power/network period. A serial recovery route is still required for that failure case.
2. **Flash encryption is deliberately rejected.** The migrator uses raw flash operations for the staged bridge and partition table. It refuses to commit when flash encryption is enabled. Secure-boot signed images should still be validated by ESP image verification; test against your exact bootloader/security configuration before deploying.
3. **The partition-table sector has an ESP-IDF dangerous-write guard.** The migrator uses the ESP-IDF internal `esp_flash_set_dangerous_write_protection()` API to disable that guard only for the single `0x8000` partition-table erase/write (and for a rollback restore), then immediately re-enables it. Do not globally set `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED`; that would remove protection for other dangerous flash addresses too.
3. The old filesystem at `0x340000` is overwritten when staging the bridge. The new filesystem is at `0x670000`. Back up any user data there separately. The firmware provides a filesystem upload endpoint for the new image.
4. The new NVS partition is only `0x5000` (20 KB). The eight settings shown in the supplied `Config.cpp` and the saved Wi-Fi station/AP configuration are exported and restored, but the actual NVS footprint must be tested on a representative device. Other Preferences namespaces not shown in that `Config.cpp` are not included in this exporter.
5. Wi-Fi station and SoftAP configuration (`wifi_config_t`) are migrated. The migrator temporarily enables AP+STA to capture both interfaces; after migration it selects AP+STA when the supplied `SoftAPAlwaysOn` preference is true, otherwise STA. This assumes that setting reflects your intended runtime mode. An application-managed static IP, custom DNS/gateway settings, country/protocol/power-save settings, or other network settings configured outside the Wi-Fi station/AP config are not automatically exported by this project.
6. **The temporary HTTP service has no authentication.** Anyone who can reach the device on the network can stage firmware, commit the migration, or install the final firmware. Use it only on a trusted, isolated LAN; disconnect or firewall access to it as soon as migration is complete.
7. The migrator is supplied as source, not a precompiled `.bin`. Build it with the same pioarduino platform release/toolchain that you use for ESPySpa. This execution environment did not have PlatformIO installed, so the source package has not been compiled here.

## Files

```text
platformio.ini
partitions/
  espa-v2.csv
  default_8MB.csv
include/
  MigrationConfig.h
src/
  main.cpp
  generated/                 # created by the build script
scripts/
  generate_partition_header.py
```

## Build configuration

The `platformio.ini` uses the platform version from your project context:

```ini
platform = https://github.com/pioarduino/platform-espressif32.git#55.03.312-1
```

Before building, edit `platformio.ini` and set:

- `MIGRATION_FALLBACK_AP_PASSWORD` (at least 8 characters)

The build script calls the `gen_esp32part.py` supplied by the installed Arduino-ESP32 framework package and embeds the vendor-generated `default_8MB` partition table into the migrator image. This prevents a hand-constructed partition-table binary from being used.

Build both images:

```bash
pio run -e migrator
pio run -e bridge
```

Expected outputs:

```text
.pio/build/migrator/firmware.bin
.pio/build/bridge/firmware.bin
```

Check the PlatformIO size report. **The migrator must fit in the old 0x180000 (1.5 MiB) application slot.** The bridge must fit in the new 0x330000 (3.1875 MiB) application slot.

## OTA procedure

Use a test ESP32-S3-DevKitC-1-N8 with the exact `espa-v2` partition table first. Keep serial logging available for the first tests.

### 1. Install the migrator using the existing firmware

Upload:

```text
.pio/build/migrator/firmware.bin
```

through your existing production firmware's `/fota` endpoint, selecting an application/firmware update. This should be an ordinary `U_FLASH` OTA update. The existing firmware's inactive old OTA slot is used, so the current running application remains intact until the migrator is selected at reboot.

After reboot, the migrator starts in temporary AP+STA mode, reuses the existing station credentials, and exposes its HTTP service on port 80. It snapshots the original station/AP configuration and the supplied Preferences values before any fallback AP can change runtime settings. The final mode is derived from `SoftAPAlwaysOn` (`true` means AP+STA; `false` means STA-only).

### 2. Stage the bridge image

Upload the bridge image to the migrator:

```bash
curl \
  -F 'firmware=@.pio/build/bridge/firmware.bin' \
  http://ESP_IP/migration/stage-bridge
```

The migrator writes the image directly to `0x340000`, one 4 KB flash sector at a time, then validates it using ESP image verification. It does not modify the partition table at this stage.

Check status:

```bash
curl http://ESP_IP/migration/status
```

Expect `"bridgeImageStaged":true`.

### 3. Commit the partition migration

Only after the bridge has staged and verified successfully:

```bash
curl -X POST \
  http://ESP_IP/migration/commit
```

The migrator:

1. Exports the `eSpa-config` Preferences values using the exact keys/types/defaults from the supplied `Config.cpp`.
2. Temporarily runs AP+STA so both stored station/AP `wifi_config_t` credentials can be read. The post-migration mode is restored from the supplied `SoftAPAlwaysOn` preference (`true` → AP+STA, `false` → STA).
3. Saves a raw copy of the old 28 KB NVS and the old 4 KB partition-table sector in the temporary 64 KB area at `0x7F0000`.
4. Verifies the backup before destructive operations.
5. Stops Wi-Fi and deinitialises NVS.
6. Erases the old NVS area, writes two new OTA-data records selecting `ota_1`, and verifies them.
7. Temporarily disables the ESP-IDF dangerous-write guard only for the partition-table sector operation, replaces the table at `0x8000` with the generated `default_8MB` table, immediately restores the guard, and reads the table back.
8. Reboots.

The new OTA metadata uses sequence 2 (`(2 - 1) % 2 == ota_1`) and `ESP_OTA_IMG_UNDEFINED`. This avoids asking rollback logic to return to an old-layout application whose address is no longer a valid app slot. The OTA-data sector format and selection logic are defined by ESP-IDF. See the references below.

### 4. Bridge restores settings and Wi-Fi

The same firmware image now runs as the bridge from `0x340000` under the new layout.

It reads the migration journal, restores these Preferences values into namespace `eSpa-config`, restores Wi-Fi station/AP configuration, verifies the writes, saves the Wi-Fi mode marker in `eSpa-migrate`, and then erases the temporary scratch area. The scratch area becomes a clean `coredump` partition again.

Check:

```bash
curl http://ESP_IP/migration/status
```

Expect `"role":"bridge"` and `"settingsRestored":true`.

If the old Wi-Fi connection cannot be restored, the bridge starts the fallback AP:

- SSID: `eSpa-Migration`
- Password: the `MIGRATION_FALLBACK_AP_PASSWORD` configured before building
- Default AP address: `192.168.4.1`

The bridge's HTTP service is also unauthenticated.

If the settings backup is still present and a restore failed, retry it with:

```bash
curl -X POST \
  http://ESP_IP/migration/restore-settings
```

### 5. Install the production filesystem and application

Build your actual ESPySpa production firmware with the supplied `default_8MB.csv` (copy it into your ESPySpa project and set `board_build.partitions` for the production environment).

If the project has a LittleFS/SPIFFS image, upload it first:

```bash
curl \
  -F 'firmware=@PATH_TO_NEW_FILESYSTEM_IMAGE.bin' \
  http://ESP_IP/migration/final-filesystem
```

This endpoint uses Arduino `Update` with `U_SPIFFS`, matching your existing update code and the `spiffs` data subtype in the supplied table. Confirm that your generated filesystem image is intended for the new `0x180000` partition.

Then upload the final production application image, built against the new partition layout:

```bash
curl \
  -F 'firmware=@PATH_TO_FINAL_ESPA_FIRMWARE.bin' \
  http://ESP_IP/migration/final-firmware
```

The bridge uses the standard Arduino `Update` API with `U_FLASH`. Because the bridge is running from `app1`, the inactive `app0` partition at `0x10000` is selected for the production firmware. The bridge sends the response and then reboots.

From then on, the production firmware can use normal OTA updates with the two new application slots.

If your bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` enabled, the final production firmware must call `esp_ota_mark_app_valid_cancel_rollback()` after its own startup/health checks succeed. Otherwise the bootloader may consider the first boot unconfirmed and roll back to the bridge. The bridge itself is selected with `ESP_OTA_IMG_UNDEFINED` because the old-layout application is not a valid rollback target after the table switch.

## Web interface

The firmware serves a built-in, self-contained migration console at `http://<device-ip>/`. It is embedded in program flash (no LittleFS assets or external CDN are required), so it is available before the production filesystem is installed. The page polls `/migration/status` every four seconds and displays the firmware role, migration phase, running application address, Wi-Fi connection/IP information, backup state, bridge staging state, settings restoration state and filesystem installation state.

The page provides file upload controls with browser-side progress reporting for the bridge image, filesystem image and final application. It also provides buttons for the commit and settings-restore POST endpoints. The destructive commit and final-firmware installation actions require browser confirmation.

The web interface is served without authentication, just like the API. Keep the migration device on a trusted, isolated network and remove access to the service when migration is complete.

## Endpoints

All endpoints are unauthenticated; keep the device on a trusted, isolated network during migration.

| Method | Endpoint | Role | Purpose |
|---|---|---|---|
| `GET` | `/` | Both | Built-in migration webpage |
| `GET` | `/migration/status` | Both | JSON status, running address, IP addresses and migration phase |
| `POST` | `/migration/stage-bridge` | Migrator | Multipart upload field `firmware`; raw-writes bridge at `0x340000` |
| `POST` | `/migration/commit` | Migrator | Back up settings/NVS, write OTA metadata and commit the new table |
| `POST` | `/migration/restore-settings` | Bridge | Retry settings restore if the migration backup remains |
| `POST` | `/migration/final-filesystem` | Bridge | Upload new filesystem image using `U_SPIFFS` |
| `POST` | `/migration/final-firmware` | Bridge | Upload production app using `U_FLASH`, then reboot |

## Recovery behaviour

- Interrupted bridge upload: the old-layout migrator remains running; upload the bridge again.
- Power loss after a valid journal is saved but before the partition table is committed: the migrator detects the journal on its next boot, restores the exact old NVS bytes, clears the temporary journal and restarts. The bridge can then be staged/committed again.
- Error returned while changing the partition table: the migrator attempts to restore the old NVS and old table from the scratch backup. The old-table restore also temporarily disables and then restores the ESP-IDF dangerous-write guard.
- Power loss during erase/program of the partition-table sector itself: no firmware-only recovery can be guaranteed. This is the inherent unsafe-update limitation for partition-table OTA. Serial recovery may be required.
- After the new layout boots, the bridge retains the settings backup until Preferences and Wi-Fi settings have both been restored and verified.

## Notes about the supplied Config.cpp

The exporter uses exactly these values:

| Namespace | Key | Type | Default |
|---|---|---|---|
| `eSpa-config` | `MqttServer` | String | `""` |
| `eSpa-config` | `MqttPort` | int | `1883` |
| `eSpa-config` | `MqttUsername` | String | `""` |
| `eSpa-config` | `MqttPassword` | String | `""` |
| `eSpa-config` | `SpaName` | String | `"eSpa"` |
| `eSpa-config` | `spaPollFreq` | int | `60` |
| `eSpa-config` | `SoftAPAlwaysOn` | bool | `true` |
| `eSpa-config` | `SoftAPPassword` | String | `"eSPA-Password"` |

Wi-Fi station/AP credentials are saved separately from Preferences using the ESP-IDF Wi-Fi configuration API. No password values are printed to serial output.

## References

- [ESP-IDF OTA documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html) — distinguishes safe app OTA from unsafe partition-table updates; documents OTA data and rollback behaviour.
- [ESP-IDF partition table documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html) — partition table format and generation.
- [ESP-IDF `esp_flash_partitions.h`](https://github.com/espressif/esp-idf/blob/master/components/bootloader_support/include/esp_flash_partitions.h) — OTA selector record and partition-table verification structures.
- [ESP-IDF `bootloader_common_loader.c`](https://sourcevu.sysprogs.com/espressif/esp-idf/files/components/bootloader_support/src/bootloader_common_loader.c) — OTA selector CRC is calculated over `ota_seq` with `esp_rom_crc32_le(UINT32_MAX, ..., 4)`.
