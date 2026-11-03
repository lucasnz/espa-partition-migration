#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <RemoteDebug.h>
#include <cstring>
#include <cstddef>

#include "MigrationConfig.h"
#include "generated/default_8MB_partitions.h"

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_internal.h" // Narrowly toggle IDF dangerous-region protection for the partition-table commit.
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_rom_crc.h"
#include "esp_flash_encrypt.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// Remote diagnostics are available over Telnet on TCP port 23 once Wi-Fi starts.
RemoteDebug Debug;

namespace {

// Flash layout supplied by the user.
constexpr char kRemoteDebugHostname[] = "espa-migration";
constexpr uint16_t kRemoteDebugPort = 23;

constexpr uint32_t kPartitionTableOffset = 0x8000;
constexpr uint32_t kFlashSectorSize = 0x1000;
constexpr uint32_t kExpectedFlashSize = 0x800000;

constexpr uint32_t kOldNvsOffset = 0x9000;
constexpr uint32_t kOldNvsSize = 0x7000;
constexpr uint32_t kNewNvsSize = 0x5000;
constexpr uint32_t kNewOtaDataOffset = 0xE000;
constexpr uint32_t kNewOtaDataSize = 0x2000;

constexpr uint32_t kOldApp0Offset = 0x20000;
constexpr uint32_t kOldApp1Offset = 0x1A0000;
constexpr uint32_t kOldAppSize = 0x180000;

constexpr uint32_t kNewApp0Offset = 0x10000;
constexpr uint32_t kNewApp1Offset = 0x340000;
constexpr uint32_t kNewAppSize = 0x330000;
constexpr uint32_t kNewSpiffsOffset = 0x670000;
constexpr uint32_t kNewSpiffsSize = 0x180000;
constexpr uint32_t kNewCoredumpOffset = 0x7F0000;
constexpr uint32_t kNewCoredumpSize = 0x10000;

// The old table leaves the top 64 KB unused. The new table reserves it for
// coredump, so it is safe as temporary migration storage until restoration.
constexpr uint32_t kScratchBase = 0x7F0000;
constexpr uint32_t kScratchSize = 0x10000;
constexpr uint32_t kJournalOffset = kScratchBase;
constexpr uint32_t kBlobOffset = kScratchBase + 0x1000;
constexpr uint32_t kOldNvsBackupOffset = kScratchBase + 0x2000;
constexpr uint32_t kOldTableBackupOffset = kScratchBase + 0x9000;
constexpr uint32_t kBlobAreaSize = 0x1000;
constexpr uint32_t kPartitionTableVerifySize = 0xC00;

constexpr uint32_t kJournalMagic = 0x4553504D; // "ESPM"
constexpr uint32_t kBlobMagic = 0x45535042;    // "ESPB"
constexpr uint16_t kMigrationVersion = 1;
constexpr uint16_t kJournalPrepared = 1;

static_assert((kOldNvsSize % 4) == 0, "NVS backup length must be word aligned");
static_assert(sizeof(kDefault8MBPartitionTable) == kPartitionTableVerifySize,
              "Generated partition table must be 0xC00 bytes");
static_assert(sizeof(MIGRATION_FALLBACK_AP_PASSWORD) >= 9,
              "Fallback AP password must be at least eight characters");

struct MigrationBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    int32_t mqttPort;
    int32_t spaPollFrequency;
    uint8_t softAPAlwaysOn;
    uint8_t wifiMode;
    uint8_t staConfigValid;
    uint8_t apConfigValid;

    char mqttServer[128];
    char mqttUsername[128];
    char mqttPassword[128];
    char spaName[64];
    char softAPPassword[128];

    wifi_config_t staConfig;
    wifi_config_t apConfig;

    uint32_t crc;
};

struct MigrationJournal {
    uint32_t magic;
    uint16_t version;
    uint16_t state;
    uint32_t blobSize;
    uint32_t blobCrc;
    uint32_t oldNvsCrc;
    uint32_t oldTableCrc;
    uint32_t headerCrc;
};

static_assert(sizeof(MigrationBlob) <= kBlobAreaSize,
              "Migration settings blob does not fit its reserved area");
static_assert((sizeof(MigrationBlob) % 4) == 0,
              "Migration blob must be word aligned");
static_assert((sizeof(MigrationJournal) % 4) == 0,
              "Journal must be word aligned");

WebServer server(80);

const esp_partition_t *runningPartition = nullptr;
bool isBridge = false;
bool bridgeImageStaged = false;
bool settingsRestored = false;
bool settingsRestoreFailed = false;
bool migrationSettingsBackupReady = false;
bool bridgeUploadActive = false;
bool bridgeUploadOk = false;
bool finalUploadActive = false;
bool finalUploadOk = false;
bool finalFilesystemOk = false;
bool finalFilesystemInstalled = false;
bool finalFirmwareInstalled = false;
bool commitRequested = false;
bool restartRequested = false;
bool fallbackApStarted = false;
bool networkStarted = false;
bool remoteDebugStarted = false;
String uploadError;
String updateError;
uint8_t activeWifiMode = WIFI_MODE_STA;
uint32_t networkStartMillis = 0;
alignas(4) MigrationBlob migrationSettingsBackup;
// Reused by the sequential table backup/restore/verification routines. These
// are static to keep multi-kilobyte buffers off the Arduino loop task stack.
alignas(4) uint8_t partitionTableWork[kFlashSectorSize];

class RawAppWriter {
public:
    bool begin() {
        active_ = true;
        failed_ = false;
        bytesReceived_ = 0;
        address_ = kNewApp1Offset;
        buffered_ = 0;
        memset(block_, 0xFF, sizeof(block_));
        return true;
    }

    bool write(const uint8_t *data, size_t length) {
        if (!active_ || failed_ || (!data && length)) {
            return false;
        }
        if (bytesReceived_ + length > kNewAppSize) {
            failed_ = true;
            return false;
        }

        while (length > 0) {
            const size_t available = sizeof(block_) - buffered_;
            const size_t count = (length < available) ? length : available;
            memcpy(block_ + buffered_, data, count);
            buffered_ += count;
            bytesReceived_ += count;
            data += count;
            length -= count;

            if (buffered_ == sizeof(block_) && !flushBlock()) {
                failed_ = true;
                return false;
            }
        }
        return true;
    }

    bool finish() {
        if (!active_ || failed_ || bytesReceived_ == 0) {
            active_ = false;
            return false;
        }
        if (buffered_ > 0) {
            memset(block_ + buffered_, 0xFF, sizeof(block_) - buffered_);
            if (!flushBlock()) {
                active_ = false;
                return false;
            }
        }
        active_ = false;
        return true;
    }

    void abort() {
        active_ = false;
        failed_ = true;
    }

    size_t bytesReceived() const { return bytesReceived_; }

private:
    bool flushBlock() {
        esp_err_t err = esp_flash_erase_region(
            esp_flash_default_chip, address_, kFlashSectorSize);
        if (err != ESP_OK) {
            debugE("Flash erase failed at 0x%08lX: %s",
                          static_cast<unsigned long>(address_), esp_err_to_name(err));
            return false;
        }

        err = esp_flash_write(
            esp_flash_default_chip, block_, address_, sizeof(block_));
        if (err != ESP_OK) {
            debugE("Flash write failed at 0x%08lX: %s",
                          static_cast<unsigned long>(address_), esp_err_to_name(err));
            return false;
        }

        address_ += kFlashSectorSize;
        buffered_ = 0;
        memset(block_, 0xFF, sizeof(block_));
        return true;
    }

    bool active_ = false;
    bool failed_ = false;
    size_t bytesReceived_ = 0;
    uint32_t address_ = kNewApp1Offset;
    size_t buffered_ = 0;
    alignas(4) uint8_t block_[kFlashSectorSize];
};

RawAppWriter bridgeWriter;

uint32_t crc32(const void *data, size_t length) {
    return esp_rom_crc32_le(UINT32_MAX,
                             static_cast<const uint8_t *>(data),
                             static_cast<uint32_t>(length));
}

bool readFlash(uint32_t address, void *destination, size_t length) {
    return esp_flash_read(esp_flash_default_chip, destination, address, length) == ESP_OK;
}

bool writeFlash(uint32_t address, const void *source, size_t length) {
    if ((address & 3U) || (length & 3U)) return false;
    return esp_flash_write(esp_flash_default_chip, source, address, length) == ESP_OK;
}

bool eraseFlash(uint32_t address, size_t length) {
    if ((address % kFlashSectorSize) || (length % kFlashSectorSize)) {
        debugE("Unaligned flash erase request: address=0x%08lX length=0x%lX",
                      static_cast<unsigned long>(address),
                      static_cast<unsigned long>(length));
        return false;
    }

    const esp_err_t err = esp_flash_erase_region(
        esp_flash_default_chip, address, static_cast<uint32_t>(length));
    if (err != ESP_OK) {
        debugE("Flash erase failed: address=0x%08lX length=0x%lX error=%s (0x%x)",
                      static_cast<unsigned long>(address),
                      static_cast<unsigned long>(length),
                      esp_err_to_name(err), static_cast<unsigned>(err));
        return false;
    }
    return true;
}

bool copyFlashRegion(uint32_t source, uint32_t destination, size_t length) {
    alignas(4) uint8_t buffer[1024];
    for (size_t offset = 0; offset < length; offset += sizeof(buffer)) {
        const size_t count = (length - offset < sizeof(buffer))
                                 ? length - offset
                                 : sizeof(buffer);
        if (!readFlash(source + offset, buffer, count) ||
            !writeFlash(destination + offset, buffer, count)) {
            return false;
        }
    }
    return true;
}

bool crcFlashRegion(uint32_t address, size_t length, uint32_t &result) {
    alignas(4) uint8_t buffer[1024];
    uint32_t crc = UINT32_MAX;
    for (size_t offset = 0; offset < length; offset += sizeof(buffer)) {
        const size_t count = (length - offset < sizeof(buffer))
                                 ? length - offset
                                 : sizeof(buffer);
        if (!readFlash(address + offset, buffer, count)) return false;
        crc = esp_rom_crc32_le(crc, buffer, static_cast<uint32_t>(count));
    }
    result = crc;
    return true;
}

bool copyStringFromPreferences(Preferences &prefs,
                               const char *key,
                               const char *fallback,
                               char *destination,
                               size_t capacity) {
    String value = prefs.getString(key, fallback);
    if (value.length() >= capacity) {
        debugW("Preference '%s' is too long for migration buffer", key);
        return false;
    }
    memcpy(destination, value.c_str(), value.length() + 1);
    return true;
}

bool exportSettingsAndWifi(MigrationBlob &blob) {
    memset(&blob, 0, sizeof(blob));
    blob.magic = kBlobMagic;
    blob.version = kMigrationVersion;
    blob.size = sizeof(blob);

    Preferences prefs;
    if (!prefs.begin("eSpa-config", true)) {
        debugE("Cannot open Preferences namespace eSpa-config for backup");
        return false;
    }

    blob.mqttPort = prefs.getInt("MqttPort", 1883);
    blob.spaPollFrequency = prefs.getInt("spaPollFreq", 60);
    blob.softAPAlwaysOn = prefs.getBool("SoftAPAlwaysOn", true) ? 1 : 0;

    const bool stringsOk =
        copyStringFromPreferences(prefs, "MqttServer", "", blob.mqttServer, sizeof(blob.mqttServer)) &&
        copyStringFromPreferences(prefs, "MqttUsername", "", blob.mqttUsername, sizeof(blob.mqttUsername)) &&
        copyStringFromPreferences(prefs, "MqttPassword", "", blob.mqttPassword, sizeof(blob.mqttPassword)) &&
        copyStringFromPreferences(prefs, "SpaName", "eSpa", blob.spaName, sizeof(blob.spaName)) &&
        copyStringFromPreferences(prefs, "SoftAPPassword", "eSPA-Password", blob.softAPPassword, sizeof(blob.softAPPassword));

    prefs.end();
    if (!stringsOk) return false;

    // The migrator deliberately starts both interfaces so it can reach the
    // device over STA and retain the stored AP credentials. The desired mode
    // after migration is reconstructed from the application's SoftAPAlwaysOn
    // preference rather than persisting the migrator's temporary APSTA mode.
    blob.wifiMode = blob.softAPAlwaysOn ? WIFI_MODE_APSTA : WIFI_MODE_STA;

    if (esp_wifi_get_config(WIFI_IF_STA, &blob.staConfig) != ESP_OK) {
        debugE("Unable to read station Wi-Fi configuration");
        return false;
    }
    blob.staConfigValid = 1;

    if (esp_wifi_get_config(WIFI_IF_AP, &blob.apConfig) != ESP_OK) {
        debugE("Unable to read SoftAP Wi-Fi configuration");
        return false;
    }
    blob.apConfigValid = 1;

    blob.crc = crc32(&blob, offsetof(MigrationBlob, crc));
    return true;
}

bool verifyDefaultPartitionTable() {
    int count = 0;
    const esp_err_t err = esp_partition_table_verify(
        reinterpret_cast<const esp_partition_info_t *>(kDefault8MBPartitionTable),
        true,
        &count);
    if (err != ESP_OK) {
        debugE("Embedded default_8MB partition table is invalid: %s", esp_err_to_name(err));
        return false;
    }
    debugI("Embedded default_8MB partition table verified (%d entries)", count);
    return true;
}

bool verifyStagedBridgeImage(uint32_t *imageLength = nullptr) {
    esp_partition_pos_t position = {kNewApp1Offset, kNewAppSize};
    esp_image_metadata_t metadata = {};
    const esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (err != ESP_OK) {
        debugE("Staged bridge image verification failed: %s", esp_err_to_name(err));
        return false;
    }
    if (metadata.image_len == 0 || metadata.image_len > kNewAppSize) return false;
    if (imageLength) *imageLength = metadata.image_len;
    return true;
}

bool readJournal(MigrationJournal &journal, MigrationBlob *blobOut = nullptr) {
    alignas(4) MigrationJournal localJournal;
    alignas(4) MigrationBlob localBlob;
    if (!readFlash(kJournalOffset, &localJournal, sizeof(localJournal))) return false;

    if (localJournal.magic != kJournalMagic ||
        localJournal.version != kMigrationVersion ||
        localJournal.state != kJournalPrepared ||
        localJournal.blobSize != sizeof(MigrationBlob) ||
        localJournal.headerCrc != crc32(&localJournal, offsetof(MigrationJournal, headerCrc))) {
        return false;
    }

    if (!readFlash(kBlobOffset, &localBlob, sizeof(localBlob))) return false;
    if (localBlob.magic != kBlobMagic ||
        localBlob.version != kMigrationVersion ||
        localBlob.size != sizeof(MigrationBlob) ||
        localBlob.crc != crc32(&localBlob, offsetof(MigrationBlob, crc)) ||
        localJournal.blobCrc != crc32(&localBlob, sizeof(localBlob))) {
        return false;
    }

    uint32_t rawNvsCrc = 0;
    uint32_t oldTableCrc = 0;
    if (!crcFlashRegion(kOldNvsBackupOffset, kOldNvsSize, rawNvsCrc) ||
        !crcFlashRegion(kOldTableBackupOffset, kFlashSectorSize, oldTableCrc) ||
        rawNvsCrc != localJournal.oldNvsCrc ||
        oldTableCrc != localJournal.oldTableCrc) {
        return false;
    }

    journal = localJournal;
    if (blobOut) *blobOut = localBlob;
    return true;
}

bool backupOldState(const MigrationBlob &blob) {
    // Wi-Fi has already been stopped and NVS deinitialised, so this raw copy
    // cannot race a Wi-Fi/NVS write. The settings blob was exported beforehand.
    // Save the old table before modifying any critical flash region.
    if (!readFlash(kPartitionTableOffset, partitionTableWork, sizeof(partitionTableWork))) return false;

    int oldTableEntries = 0;
    if (esp_partition_table_verify(
            reinterpret_cast<const esp_partition_info_t *>(partitionTableWork),
            true,
            &oldTableEntries) != ESP_OK) {
        debugE("Current partition table failed validation; refusing migration");
        return false;
    }

    if (!eraseFlash(kScratchBase, kScratchSize)) {
        debugE("Could not erase migration scratch area");
        return false;
    }

    const uint32_t sourceTableCrc = crc32(partitionTableWork, sizeof(partitionTableWork));
    uint32_t sourceNvsCrc = 0;
    if (!crcFlashRegion(kOldNvsOffset, kOldNvsSize, sourceNvsCrc)) {
        debugE("Could not checksum old NVS before backup");
        return false;
    }

    if (!writeFlash(kBlobOffset, &blob, sizeof(blob)) ||
        !writeFlash(kOldTableBackupOffset, partitionTableWork, sizeof(partitionTableWork))) {
        debugE("Could not write settings or partition-table backup");
        return false;
    }

    if (!copyFlashRegion(kOldNvsOffset, kOldNvsBackupOffset, kOldNvsSize)) {
        debugE("Could not back up old NVS partition");
        return false;
    }

    uint32_t backupNvsCrc = 0;
    uint32_t backupTableCrc = 0;
    if (!crcFlashRegion(kOldNvsBackupOffset, kOldNvsSize, backupNvsCrc) ||
        !crcFlashRegion(kOldTableBackupOffset, kFlashSectorSize, backupTableCrc) ||
        backupNvsCrc != sourceNvsCrc || backupTableCrc != sourceTableCrc) {
        debugE("NVS or partition-table backup failed source comparison");
        return false;
    }

    MigrationJournal journal = {};
    journal.magic = kJournalMagic;
    journal.version = kMigrationVersion;
    journal.state = kJournalPrepared;
    journal.blobSize = sizeof(blob);
    journal.blobCrc = crc32(&blob, sizeof(blob));
    journal.oldNvsCrc = sourceNvsCrc;
    journal.oldTableCrc = sourceTableCrc;

    journal.headerCrc = crc32(&journal, offsetof(MigrationJournal, headerCrc));

    // The journal header is written last. Without a valid header no destructive
    // migration operation is allowed to proceed.
    if (!writeFlash(kJournalOffset, &journal, sizeof(journal))) return false;

    MigrationJournal verifyJournal;
    MigrationBlob verifyBlob;
    if (!readJournal(verifyJournal, &verifyBlob)) {
        debugE("Migration backup verification failed");
        return false;
    }

    debugI("Preferences, Wi-Fi configuration, old NVS and old partition table backed up");
    return true;
}

bool restoreOldNvsFromBackup() {
    MigrationJournal journal;
    if (!readJournal(journal)) return false;
    if (!eraseFlash(kOldNvsOffset, kOldNvsSize)) return false;
    if (!copyFlashRegion(kOldNvsBackupOffset, kOldNvsOffset, kOldNvsSize)) return false;
    uint32_t crc = 0;
    return crcFlashRegion(kOldNvsOffset, kOldNvsSize, crc) && crc == journal.oldNvsCrc;
}

bool restoreOldPartitionTableFromBackup() {
    MigrationJournal journal;
    if (!readJournal(journal)) return false;

    // The IDF SPI flash driver deliberately protects the bootloader and
    // partition-table regions. Temporarily lift that guard only while restoring
    // this one sector, then immediately reinstate it.
    esp_err_t err = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, false);
    if (err != ESP_OK) {
        debugE("Could not disable dangerous-write guard for old table restore: %s",
                      esp_err_to_name(err));
        return false;
    }

    const bool restored =
        eraseFlash(kPartitionTableOffset, kFlashSectorSize) &&
        copyFlashRegion(kOldTableBackupOffset, kPartitionTableOffset, kFlashSectorSize);

    const esp_err_t protectErr = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, true);
    if (protectErr != ESP_OK) {
        debugW("WARNING: could not re-enable dangerous-write guard after old table restore: %s",
                      esp_err_to_name(protectErr));
        return false;
    }
    if (!restored) return false;

    if (!readFlash(kPartitionTableOffset, partitionTableWork, sizeof(partitionTableWork))) return false;
    int count = 0;
    const esp_err_t verifyErr = esp_partition_table_verify(
        reinterpret_cast<const esp_partition_info_t *>(partitionTableWork), true, &count);
    if (verifyErr != ESP_OK) {
        debugE("Restored old partition table failed validation: %s",
                      esp_err_to_name(verifyErr));
        return false;
    }
    debugI("Old partition table restored and verified (%d entries)", count);
    return true;
}

void clearMigrationScratch() {
    if (!eraseFlash(kScratchBase, kScratchSize)) {
        debugW("Warning: could not clear migration scratch/coredump area");
    }
}

bool writeNewOtaData() {
    esp_ota_select_entry_t entry;
    memset(&entry, 0xFF, sizeof(entry));
    entry.ota_seq = 2; // (2 - 1) % 2 selects ota_1.
    entry.ota_state = ESP_OTA_IMG_UNDEFINED; // Do not attempt rollback to the old layout.
    entry.crc = esp_rom_crc32_le(UINT32_MAX,
                                 reinterpret_cast<const uint8_t *>(&entry.ota_seq),
                                 sizeof(entry.ota_seq));

    for (uint32_t sector = 0; sector < 2; ++sector) {
        const uint32_t address = kNewOtaDataOffset + sector * kFlashSectorSize;
        if (!eraseFlash(address, kFlashSectorSize) ||
            !writeFlash(address, &entry, sizeof(entry))) {
            debugE("Failed writing new OTA data sector %lu", static_cast<unsigned long>(sector));
            return false;
        }

        alignas(4) esp_ota_select_entry_t verify;
        if (!readFlash(address, &verify, sizeof(verify)) ||
            verify.ota_seq != entry.ota_seq ||
            verify.crc != entry.crc ||
            verify.ota_state != ESP_OTA_IMG_UNDEFINED) {
            debugE("New OTA data read-back verification failed");
            return false;
        }
    }
    return true;
}

bool writeNewPartitionTable() {
    if (!verifyDefaultPartitionTable()) return false;

    // esp_flash_erase_region()/esp_flash_write() protect the bootloader,
    // partition table and currently running app. The default IDF setting is
    // to abort() on such a write. Lift only the partition-table guard for this
    // tightly scoped erase/write pair, then restore it before read-back checks.
    esp_err_t err = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, false);
    if (err != ESP_OK) {
        debugE("Could not disable dangerous-write guard for partition-table commit: %s (0x%x)",
                      esp_err_to_name(err), static_cast<unsigned>(err));
        return false;
    }

    bool tableWritten = false;
    if (eraseFlash(kPartitionTableOffset, kFlashSectorSize)) {
        tableWritten = writeFlash(kPartitionTableOffset,
                                  kDefault8MBPartitionTable,
                                  sizeof(kDefault8MBPartitionTable));
    }

    // Always restore the guard, including when erase/write reports an error.
    const esp_err_t protectErr = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, true);
    if (protectErr != ESP_OK) {
        debugW("WARNING: could not re-enable dangerous-write guard after partition-table commit: %s (0x%x)",
                      esp_err_to_name(protectErr), static_cast<unsigned>(protectErr));
        return false;
    }
    if (!tableWritten) {
        debugE("Could not erase/write new partition table");
        return false;
    }

    // Read-back and validate after protection has been restored.
    if (!readFlash(kPartitionTableOffset, partitionTableWork, kPartitionTableVerifySize)) return false;

    int count = 0;
    err = esp_partition_table_verify(
        reinterpret_cast<const esp_partition_info_t *>(partitionTableWork), true, &count);
    if (err != ESP_OK || memcmp(partitionTableWork, kDefault8MBPartitionTable, kPartitionTableVerifySize) != 0) {
        debugE("New partition table failed read-back verification: %s",
                      esp_err_to_name(err));
        return false;
    }

    debugI("New partition table installed and verified (%d entries)", count);
    return true;
}

bool restorePreferences(const MigrationBlob &blob) {
    Preferences prefs;
    if (!prefs.begin("eSpa-config", false)) return false;

    // Preferences::putString() returns the string length, so a successfully
    // stored empty string can return zero. Verify stored values rather than
    // treating a zero-length value as a failed write.
    prefs.putString("MqttServer", blob.mqttServer);
    prefs.putInt("MqttPort", blob.mqttPort);
    prefs.putString("MqttUsername", blob.mqttUsername);
    prefs.putString("MqttPassword", blob.mqttPassword);
    prefs.putString("SpaName", blob.spaName);
    prefs.putInt("spaPollFreq", blob.spaPollFrequency);
    prefs.putBool("SoftAPAlwaysOn", blob.softAPAlwaysOn != 0);
    prefs.putString("SoftAPPassword", blob.softAPPassword);

    const bool writeMatches =
        prefs.getString("MqttServer", "") == blob.mqttServer &&
        prefs.getInt("MqttPort", 1883) == blob.mqttPort &&
        prefs.getString("MqttUsername", "") == blob.mqttUsername &&
        prefs.getString("MqttPassword", "") == blob.mqttPassword &&
        prefs.getString("SpaName", "eSpa") == blob.spaName &&
        prefs.getInt("spaPollFreq", 60) == blob.spaPollFrequency &&
        prefs.getBool("SoftAPAlwaysOn", true) == (blob.softAPAlwaysOn != 0) &&
        prefs.getString("SoftAPPassword", "eSPA-Password") == blob.softAPPassword;
    prefs.end();
    if (!writeMatches) return false;

    Preferences verify;
    if (!verify.begin("eSpa-config", true)) return false;
    const bool matches =
        verify.getString("MqttServer", "") == blob.mqttServer &&
        verify.getInt("MqttPort", 1883) == blob.mqttPort &&
        verify.getString("MqttUsername", "") == blob.mqttUsername &&
        verify.getString("MqttPassword", "") == blob.mqttPassword &&
        verify.getString("SpaName", "eSpa") == blob.spaName &&
        verify.getInt("spaPollFreq", 60) == blob.spaPollFrequency &&
        verify.getBool("SoftAPAlwaysOn", true) == (blob.softAPAlwaysOn != 0) &&
        verify.getString("SoftAPPassword", "eSPA-Password") == blob.softAPPassword;
    verify.end();
    return matches;
}

bool saveWifiModeMarker(uint8_t mode) {
    Preferences prefs;
    if (!prefs.begin("eSpa-migrate", false)) return false;
    const bool ok = prefs.putUChar("wifiMode", mode) == sizeof(uint8_t) &&
                    prefs.putBool("complete", true) == sizeof(uint8_t);
    prefs.end();
    return ok;
}

bool restoreWifiConfig(const MigrationBlob &blob) {
    const wifi_mode_t mode = static_cast<wifi_mode_t>(blob.wifiMode);
    if (mode == WIFI_MODE_NULL) return false;

    WiFi.persistent(true);
    // Temporarily enable both interfaces so both stored interface configs can
    // be restored even when the desired post-migration mode is STA-only.
    if (!WiFi.mode(WIFI_MODE_APSTA)) {
        debugE("Could not enable AP+STA while restoring Wi-Fi configuration");
        return false;
    }

    bool ok = true;
    if (blob.staConfigValid) {
        const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA,
                                                   const_cast<wifi_config_t *>(&blob.staConfig));
        if (err != ESP_OK) {
            debugE("Station Wi-Fi restore failed: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    if (blob.apConfigValid) {
        const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP,
                                                   const_cast<wifi_config_t *>(&blob.apConfig));
        if (err != ESP_OK) {
            debugE("SoftAP Wi-Fi restore failed: %s", esp_err_to_name(err));
            ok = false;
        }
    }

    // Verify both interface configurations while AP+STA is still enabled.
    if (blob.staConfigValid) {
        wifi_config_t check = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &check) != ESP_OK ||
            memcmp(check.sta.ssid, blob.staConfig.sta.ssid, sizeof(check.sta.ssid)) != 0 ||
            memcmp(check.sta.password, blob.staConfig.sta.password, sizeof(check.sta.password)) != 0) {
            ok = false;
        }
    }
    if (blob.apConfigValid) {
        wifi_config_t check = {};
        if (esp_wifi_get_config(WIFI_IF_AP, &check) != ESP_OK ||
            memcmp(check.ap.ssid, blob.apConfig.ap.ssid, sizeof(check.ap.ssid)) != 0 ||
            memcmp(check.ap.password, blob.apConfig.ap.password, sizeof(check.ap.password)) != 0) {
            ok = false;
        }
    }

    // Apply the desired operating mode only after both interface configs
    // have been restored and verified.
    if (!WiFi.mode(mode)) {
        debugE("Could not apply saved post-migration Wi-Fi mode");
        ok = false;
    }

    if ((mode & WIFI_MODE_STA) && blob.staConfigValid) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            debugE("Station connect request failed: %s", esp_err_to_name(err));
            ok = false;
        }
    }

    if (ok) {
        activeWifiMode = blob.wifiMode;
        networkStarted = true;
    }
    return ok;
}

bool importMigrationSettings() {
    MigrationJournal journal;
    alignas(4) MigrationBlob blob;
    if (!readJournal(journal, &blob)) {
        debugE("No valid migration backup found in scratch area");
        return false;
    }

    if (!restorePreferences(blob)) {
        debugE("Preferences restore/verification failed");
        return false;
    }
    if (!saveWifiModeMarker(blob.wifiMode)) {
        debugE("Could not save migrated Wi-Fi mode marker");
        return false;
    }
    if (!restoreWifiConfig(blob)) {
        debugE("Wi-Fi configuration restore/verification failed");
        return false;
    }

    // Credentials are now committed to NVS. Keep later temporary fallback
    // AP changes in RAM only, not in the restored Wi-Fi configuration.
    WiFi.persistent(false);

    settingsRestored = true;
    clearMigrationScratch();
    debugI("Preferences and Wi-Fi settings restored successfully");
    return true;
}

bool startWifiFromMigratedNvs() {
    Preferences prefs;
    if (!prefs.begin("eSpa-migrate", true)) return false;
    const bool complete = prefs.getBool("complete", false);
    const uint8_t mode = prefs.getUChar("wifiMode", WIFI_MODE_STA);
    prefs.end();
    if (!complete) return false;

    WiFi.persistent(true); // Load the migrated Wi-Fi config from NVS.
    if (!WiFi.mode(static_cast<wifi_mode_t>(mode))) return false;
    activeWifiMode = mode;
    if (mode & WIFI_MODE_STA) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return false;
    }
    WiFi.persistent(false); // Avoid unnecessary credential writes at runtime.
    networkStarted = true;
    settingsRestored = true;
    return true;
}

bool checkOldLayout() {
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    const esp_partition_t *app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t *app1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const esp_partition_t *spiffs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);

    return nvs && ota && app0 && app1 && spiffs &&
           nvs->address == kOldNvsOffset && nvs->size == kOldNvsSize &&
           ota->address == 0x10000 && ota->size == 0x2000 &&
           app0->address == kOldApp0Offset && app0->size == kOldAppSize &&
           app1->address == kOldApp1Offset && app1->size == kOldAppSize &&
           spiffs->address == 0x340000 && spiffs->size == 0x20000;
}

bool checkNewLayout() {
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    const esp_partition_t *app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t *app1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const esp_partition_t *spiffs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    const esp_partition_t *coredump = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);

    return nvs && ota && app0 && app1 && spiffs && coredump &&
           nvs->address == kOldNvsOffset && nvs->size == kNewNvsSize &&
           ota->address == kNewOtaDataOffset && ota->size == kNewOtaDataSize &&
           app0->address == kNewApp0Offset && app0->size == kNewAppSize &&
           app1->address == kNewApp1Offset && app1->size == kNewAppSize &&
           spiffs->address == kNewSpiffsOffset && spiffs->size == kNewSpiffsSize &&
           coredump->address == kNewCoredumpOffset && coredump->size == kNewCoredumpSize;
}

bool isOldLayoutRunning() {
    return runningPartition && checkOldLayout() &&
           ((runningPartition->address == kOldApp0Offset) ||
            (runningPartition->address == kOldApp1Offset));
}

void recoverInterruptedOldLayoutMigration() {
    MigrationJournal journal;
    if (!readJournal(journal)) return;

    // If the old layout still booted, the partition-table commit did not finish.
    // Restore the exact old NVS bytes before starting Wi-Fi/Preferences again.
    debugW("Found an interrupted pre-commit migration; restoring old NVS");
    if (!restoreOldNvsFromBackup()) {
        debugE("CRITICAL: old NVS recovery failed; leaving migration backup intact");
        settingsRestoreFailed = true;
        return;
    }
    clearMigrationScratch();
    debugI("Old NVS restored. Restarting before normal startup.");
    delay(250);
    ESP.restart();
}

void startFallbackAccessPoint() {
    if (fallbackApStarted) return;
    debugI("Starting fallback migration access point");
    WiFi.mode(WIFI_MODE_APSTA);
    WiFi.softAPConfig(IPAddress(169,254,1,1), IPAddress(169,254,1,1), IPAddress(255,255,255,0));
    WiFi.softAP(MIGRATION_FALLBACK_AP_SSID, MIGRATION_FALLBACK_AP_PASSWORD);
    fallbackApStarted = true;
    activeWifiMode = WIFI_MODE_APSTA;
}

const char kMigrationPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <title>eSpa partition migration</title>
  <style>
    :root{font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color-scheme:light dark;--bg:#101820;--panel:#1b2732;--panel2:#233342;--line:#354653;--text:#edf3f7;--muted:#a9bac7;--accent:#55b9a8;--warn:#f0bd61;--bad:#ef7777;--good:#71c995}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);line-height:1.5}
    header{padding:25px 18px 20px;background:linear-gradient(120deg,#142b35,#1b3945);border-bottom:1px solid var(--line)}
    main{max-width:900px;margin:0 auto;padding:20px 16px 48px}header .inner{max-width:900px;margin:auto}
    h1{font-size:clamp(1.5rem,4vw,2rem);margin:0 0 5px}h2{font-size:1.12rem;margin:0 0 12px}h3{font-size:1rem;margin:0 0 8px}
    p{margin:6px 0;color:var(--muted)}.sub{font-size:.92rem}.topline{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap}
    .pill{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--line);border-radius:999px;padding:5px 10px;font-size:.82rem;color:var(--muted);white-space:nowrap}
    .dot{width:8px;height:8px;border-radius:50%;background:var(--warn)}.dot.online{background:var(--good)}.dot.offline{background:var(--bad)}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:13px;margin:14px 0}.card{background:var(--panel);border:1px solid var(--line);border-radius:13px;padding:16px;min-width:0}
    .wide{grid-column:1/-1}.kv{display:grid;grid-template-columns:1fr auto;gap:7px 14px;font-size:.9rem}.kv span:nth-child(odd){color:var(--muted)}.kv span:nth-child(even){text-align:right;overflow-wrap:anywhere}
    .state{font-weight:650}.good{color:var(--good)}.bad{color:var(--bad)}.warn{color:var(--warn)}
    label.file{display:block;margin:12px 0 8px;font-size:.9rem;color:var(--muted)}input[type=file]{display:block;width:100%;margin-top:6px;color:var(--text);font-size:.85rem}
    button{border:0;border-radius:9px;padding:10px 14px;background:var(--accent);color:#08201d;font-weight:700;font-size:.9rem;cursor:pointer;margin:6px 6px 0 0}
    button.secondary{background:var(--panel2);color:var(--text);border:1px solid var(--line)}button.danger{background:#a84444;color:#fff}
    button:disabled{opacity:.4;cursor:not-allowed}.hint{font-size:.82rem;color:var(--muted)}.notice{border-left:3px solid var(--warn);padding:9px 12px;background:var(--panel2);border-radius:5px;font-size:.85rem;margin:12px 0;color:var(--text)}
    progress{width:100%;height:10px;margin-top:9px;accent-color:var(--accent)}.progressText{font-size:.82rem;color:var(--muted);min-height:1.3em}
    #activity{white-space:pre-wrap;overflow-wrap:anywhere;background:#0b1218;border:1px solid var(--line);border-radius:8px;padding:12px;min-height:48px;font: .84rem ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--muted)}
    details{margin-top:14px}summary{cursor:pointer;color:var(--muted)}footer{padding-top:18px;color:var(--muted);font-size:.78rem}
    @media(max-width:650px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}.card{padding:14px}.kv{grid-template-columns:1fr}.kv span:nth-child(even){text-align:left;margin-bottom:4px}}
  </style>
</head>
<body>
<header><div class="inner">
  <div class="topline"><h1>eSpa partition migration</h1><span class="pill"><i id="connectionDot" class="dot"></i><span id="connectionText">Connecting…</span></span></div>
  <p class="sub">Temporary device migration console. Keep this device on a trusted network while the service is enabled.</p>
</div></header>
<main>
  <section class="grid">
    <article class="card wide">
      <div class="topline"><h2>Device status</h2><button class="secondary" id="refreshButton" type="button">Refresh status</button></div>
      <div class="kv" aria-live="polite">
        <span>Firmware role</span><span id="role" class="state">—</span>
        <span>Migration phase</span><span id="phase" class="state">—</span>
        <span>Running application address</span><span id="runningAddress">—</span>
        <span>Wi-Fi station</span><span id="wifiState">—</span>
        <span>Station IP</span><span id="staIp">—</span>
        <span>Access point IP</span><span id="apIp">—</span>
        <span>Settings backup ready</span><span id="backupReady">—</span>
        <span>Bridge image staged</span><span id="bridgeStaged">—</span>
        <span>Settings restored</span><span id="settingsRestored">—</span>
        <span>Final filesystem installed</span><span id="filesystemInstalled">—</span>
      </div>
      <div id="statusWarning" class="notice" hidden></div>
      <p class="hint">Status refreshes automatically every 4 seconds. The connection indicator reports whether the status endpoint is reachable.</p>
    </article>

    <article class="card" id="stageCard">
      <h2>1. Stage bridge firmware</h2>
      <p>Available while running the migrator on the original <code>espa-v2</code> layout. The bridge image is written to the future <code>app1</code> address.</p>
      <label class="file">Bridge firmware (.bin)<input id="bridgeFile" type="file" accept=".bin,application/octet-stream"></label>
      <button id="stageButton" type="button">Upload bridge image</button>
      <progress id="stageProgress" value="0" max="100" hidden></progress><div id="stageProgressText" class="progressText"></div>
    </article>

    <article class="card" id="commitCard">
      <h2>2. Commit partition migration</h2>
      <p>Backs up the old state, installs the new partition table and reboots into the bridge firmware.</p>
      <div class="notice">This is the destructive migration step. Do not power off the device while it is running.</div>
      <button id="commitButton" class="danger" type="button">Commit migration</button>
    </article>

    <article class="card" id="restoreCard">
      <h2>3. Restore settings</h2>
      <p>The bridge restores the <code>eSpa-config</code> Preferences values and saved Wi-Fi station/access-point configuration.</p>
      <button id="restoreButton" type="button">Retry settings restore</button>
    </article>

    <article class="card" id="filesystemCard">
      <h2>4. Install filesystem (optional)</h2>
      <p>Upload the filesystem image built for the new <code>default_8MB</code> layout.</p>
      <label class="file">Filesystem image (.bin)<input id="filesystemFile" type="file" accept=".bin,application/octet-stream"></label>
      <button id="filesystemButton" type="button">Upload filesystem</button>
      <progress id="filesystemProgress" value="0" max="100" hidden></progress><div id="filesystemProgressText" class="progressText"></div>
    </article>

    <article class="card wide" id="firmwareCard">
      <h2>5. Install final eSpa firmware</h2>
      <p>Upload the production application compiled with <code>default_8MB.csv</code>. The bridge will install it into the inactive application slot and reboot.</p>
      <label class="file">Final application firmware (.bin)<input id="firmwareFile" type="file" accept=".bin,application/octet-stream"></label>
      <button id="firmwareButton" type="button">Upload final firmware</button>
      <progress id="firmwareProgress" value="0" max="100" hidden></progress><div id="firmwareProgressText" class="progressText"></div>
    </article>

    <article class="card wide">
      <h2>Activity</h2>
      <div id="activity" role="status" aria-live="polite">Waiting for device status…</div>
      <details><summary>Available HTTP endpoints</summary>
        <p class="hint"><code>GET /migration/status</code><br><code>POST /migration/stage-bridge</code><br><code>POST /migration/commit</code><br><code>POST /migration/restore-settings</code><br><code>POST /migration/final-filesystem</code><br><code>POST /migration/final-firmware</code></p>
      </details>
    </article>
  </section>
  <footer>Unauthenticated temporary firmware service. Anyone who can reach this device can upload firmware and trigger migration. Disconnect or firewall access when finished.</footer>
</main>
<script>
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  let current = null;
  let busy = false;
  let polling = false;
  let hostRedirectPrompted = false;

  function isIpAddress(host) {
    const value = host.replace(/^\[|\]$/g, '');
    const octets = value.split('.');
    if (octets.length === 4 && octets.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255)) return true;
    if (!value.includes(':')) return false;
    try {
      new URL(`http://[${value}]`);
      return true;
    } catch (_) {
      return false;
    }
  }
  function promptIpAddress(data) {
    if (hostRedirectPrompted || isIpAddress(window.location.hostname) || !data.staIp) return;
    hostRedirectPrompted = true;
    if (confirm(`This page was opened using a hostname. Navigate to the server IP address ${data.staIp}?`)) {
      const target = new URL(window.location.href);
      target.hostname = data.staIp;
      window.location.assign(target.href);
    }
  }

  function log(message, isError = false) {
    const stamp = new Date().toLocaleTimeString();
    $('activity').textContent = `[${stamp}] ${message}`;
    $('activity').style.color = isError ? 'var(--bad)' : 'var(--muted)';
  }
  function yesNo(value) { return value ? 'Yes' : 'No'; }
  function setText(id, value, state) {
    const el = $(id);
    el.textContent = value;
    el.className = state || '';
  }
  function setBusy(value) {
    busy = value;
    document.querySelectorAll('button').forEach(button => {
      if (button.id !== 'refreshButton') button.disabled = value;
    });
    updateControls();
  }
  function updateControls() {
    if (!current) return;
    const migrator = current.role === 'migrator';
    const bridge = current.role === 'bridge';
    $('stageCard').hidden = !migrator;
    $('commitCard').hidden = !migrator;
    $('restoreCard').hidden = !bridge;
    $('filesystemCard').hidden = !bridge;
    $('firmwareCard').hidden = !bridge;
    $('stageButton').disabled = busy || !migrator;
    $('commitButton').disabled = busy || !migrator || !current.bridgeImageStaged || !current.migrationBackupReady;
    $('restoreButton').disabled = busy || !bridge || current.settingsRestored;
    $('filesystemButton').disabled = busy || !bridge || !current.settingsRestored;
    $('firmwareButton').disabled = busy || !bridge || !current.settingsRestored;
  }
  async function refreshStatus() {
    if (polling) return;
    polling = true;
    try {
      const response = await fetch('/migration/status?_=' + Date.now(), {cache:'no-store'});
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      current = data;
    promptIpAddress(data);
      $('connectionDot').className = 'dot online';
      $('connectionText').textContent = 'Device online';
      setText('role', data.role || 'Unknown');
      setText('phase', (data.phase || 'unknown').replaceAll('_', ' '));
      setText('runningAddress', data.runningAddress || '—');
      setText('wifiState', data.wifiConnected ? 'Connected' : 'Disconnected', data.wifiConnected ? 'good' : 'warn');
      setText('staIp', data.staIp || '—');
      setText('apIp', data.apIp || '—');
      setText('backupReady', yesNo(data.migrationBackupReady), data.migrationBackupReady ? 'good' : 'warn');
      setText('bridgeStaged', yesNo(data.bridgeImageStaged), data.bridgeImageStaged ? 'good' : 'warn');
      setText('settingsRestored', data.settingsRestoreFailed ? 'Failed' : yesNo(data.settingsRestored), data.settingsRestored ? 'good' : (data.settingsRestoreFailed ? 'bad' : 'warn'));
      setText('filesystemInstalled', yesNo(data.finalFilesystemInstalled), data.finalFilesystemInstalled ? 'good' : '');
      const warning = $('statusWarning');
      if (data.settingsRestoreFailed) {
        warning.hidden = false;
        warning.textContent = 'Settings restoration has failed. The migration backup should be retained; use “Retry settings restore” and check serial logs before proceeding.';
      } else if (data.role === 'migrator' && !data.migrationBackupReady) {
        warning.hidden = false;
        warning.textContent = 'The Preferences/Wi-Fi backup is not ready. Partition migration is disabled until the device can read and snapshot its existing settings.';
      } else {
        warning.hidden = true;
      }
      updateControls();
    } catch (error) {
      $('connectionDot').className = 'dot offline';
      $('connectionText').textContent = 'Device unreachable';
      if (!current) log('Could not read migration status: ' + error.message, true);
    } finally {
      polling = false;
    }
  }
  async function postAction(path, label) {
    if (busy) return;
    setBusy(true);
    log(label + '…');
    try {
      const response = await fetch(path, {method:'POST', cache:'no-store'});
      const text = await response.text();
      if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
      log(text || (label + ' accepted.'));
      await refreshStatus();
    } catch (error) {
      log(label + ' failed: ' + error.message, true);
    } finally {
      setBusy(false);
      await refreshStatus();
    }
  }
  function upload(path, inputId, progressId, progressTextId, label, rebootExpected) {
    if (busy) return;
    const input = $(inputId);
    const file = input.files && input.files[0];
    if (!file) { log('Select a file before starting ' + label.toLowerCase() + '.', true); return; }
    const progress = $(progressId);
    const progressText = $(progressTextId);
    const xhr = new XMLHttpRequest();
    const form = new FormData();
    form.append('firmware', file, file.name);
    setBusy(true);
    progress.hidden = false;
    progress.value = 0;
    progressText.textContent = 'Starting upload…';
    log(`${label}: ${file.name} (${(file.size / 1024).toFixed(1)} KiB)`);
    xhr.open('POST', path, true);
    xhr.upload.onprogress = event => {
      if (event.lengthComputable) {
        const percent = Math.round(event.loaded * 100 / event.total);
        progress.value = percent;
        progressText.textContent = `${percent}% — ${(event.loaded / 1024).toFixed(1)} / ${(event.total / 1024).toFixed(1)} KiB`;
      }
    };
    xhr.onload = async () => {
      const message = xhr.responseText || `HTTP ${xhr.status}`;
      if (xhr.status >= 200 && xhr.status < 300) {
        progress.value = 100;
        progressText.textContent = 'Upload complete';
        log(message);
      } else {
        progressText.textContent = 'Upload failed';
        log(label + ' failed: ' + message, true);
      }
      setBusy(false);
      if (!rebootExpected) {
        await refreshStatus();
      } else if (xhr.status >= 200 && xhr.status < 300) {
        log('The final firmware was accepted. Reloading this page in 2 seconds…');
        // The bridge restarts after returning the upload response. A full page
        // reload reconnects to the device using the final application.
        window.setTimeout(() => window.location.reload(), 2000);
      } else {
        log(message + ' The device may briefly disappear while it reboots.');
      }
    };
    xhr.onerror = () => {
      progressText.textContent = 'Connection lost';
      log(label + ' connection failed. Check device status before retrying.', true);
      setBusy(false);
      refreshStatus();
    };
    xhr.onabort = () => {
      progressText.textContent = 'Upload aborted';
      log(label + ' was aborted.', true);
      setBusy(false);
    };
    xhr.send(form);
  }

  $('refreshButton').addEventListener('click', refreshStatus);
  $('stageButton').addEventListener('click', () => upload('/migration/stage-bridge', 'bridgeFile', 'stageProgress', 'stageProgressText', 'Bridge staging', false));
  $('commitButton').addEventListener('click', () => {
    if (confirm('Commit the partition-table migration now? The device will erase/replace partition metadata and reboot. Do not remove power.')) {
      postAction('/migration/commit', 'Partition migration commit');
    }
  });
  $('restoreButton').addEventListener('click', () => postAction('/migration/restore-settings', 'Settings restore'));
  $('filesystemButton').addEventListener('click', () => upload('/migration/final-filesystem', 'filesystemFile', 'filesystemProgress', 'filesystemProgressText', 'Filesystem upload', false));
  $('firmwareButton').addEventListener('click', () => {
    if (confirm('Install the final production firmware and reboot the device?')) {
      upload('/migration/final-firmware', 'firmwareFile', 'firmwareProgress', 'firmwareProgressText', 'Final firmware upload', true);
    }
  });

  refreshStatus();
  setInterval(() => { if (document.visibilityState === 'visible') refreshStatus(); }, 4000);
})();
</script>
</body>
</html>)HTML";

void sendStatus() {
    String phase;
    if (!isBridge) {
        if (commitRequested) phase = "committing";
        else if (bridgeImageStaged && migrationSettingsBackupReady) phase = "ready_to_commit";
        else if (bridgeImageStaged) phase = "bridge_staged_backup_unavailable";
        else if (migrationSettingsBackupReady) phase = "ready_to_stage_bridge";
        else phase = "initialising";
    } else {
        if (restartRequested || finalFirmwareInstalled) phase = "final_firmware_installed_rebooting";
        else if (settingsRestoreFailed) phase = "settings_restore_failed";
        else if (!settingsRestored) phase = "restoring_settings";
        else if (finalFilesystemInstalled) phase = "ready_for_final_firmware";
        else phase = "ready_for_production_uploads";
    }

    String body = "{";
    body += "\"role\":\"" + String(isBridge ? "bridge" : "migrator") + "\",";
    body += "\"phase\":\"" + phase + "\",";
    body += "\"runningAddress\":\"0x" + String(runningPartition ? runningPartition->address : 0, HEX) + "\",";
    body += "\"bridgeImageStaged\":" + String(bridgeImageStaged ? "true" : "false") + ",";
    body += "\"settingsRestored\":" + String(settingsRestored ? "true" : "false") + ",";
    body += "\"settingsRestoreFailed\":" + String(settingsRestoreFailed ? "true" : "false") + ",";
    body += "\"migrationBackupReady\":" + String(migrationSettingsBackupReady ? "true" : "false") + ",";
    body += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    body += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
    body += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
    body += "\"bridgeUploadActive\":" + String(bridgeUploadActive ? "true" : "false") + ",";
    body += "\"finalUploadActive\":" + String(finalUploadActive ? "true" : "false") + ",";
    body += "\"finalFilesystemInstalled\":" + String(finalFilesystemInstalled ? "true" : "false") + ",";
    body += "\"finalFirmwareInstalled\":" + String(finalFirmwareInstalled ? "true" : "false");
    body += "}";
    server.send(200, "application/json", body);
}

void handleRoot() {
    server.send_P(200, "text/html; charset=utf-8", kMigrationPage);
}

void handleBridgeUpload() {
    HTTPUpload &upload = server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START:
            bridgeUploadOk = false;
            uploadError = "";
            if (isBridge || !isOldLayoutRunning()) {
                uploadError = "Bridge staging is only available on the old espa-v2 layout";
                bridgeWriter.abort();
                break;
            }
            bridgeUploadActive = bridgeWriter.begin();
            if (!bridgeUploadActive) uploadError = "Could not start raw flash writer";
            break;

        case UPLOAD_FILE_WRITE:
            if (bridgeUploadActive && !bridgeWriter.write(upload.buf, upload.currentSize)) {
                uploadError = "Bridge flash write failed or image exceeded 0x330000 bytes";
                bridgeWriter.abort();
                bridgeUploadActive = false;
            }
            break;

        case UPLOAD_FILE_END:
            if (bridgeUploadActive && bridgeWriter.finish()) {
                bridgeImageStaged = verifyStagedBridgeImage();
                bridgeUploadOk = bridgeImageStaged;
                if (!bridgeImageStaged) uploadError = "Staged image failed ESP image verification";
            } else if (uploadError.isEmpty()) {
                uploadError = "Bridge upload did not complete";
            }
            bridgeUploadActive = false;
            break;

        case UPLOAD_FILE_ABORTED:
            bridgeWriter.abort();
            bridgeUploadActive = false;
            bridgeUploadOk = false;
            uploadError = "Bridge upload was aborted";
            break;

        default:
            break;
    }
}

void handleBridgeUploadPost() {
    if (bridgeUploadOk) {
        server.send(200, "text/plain", "Bridge image staged and verified at 0x340000. POST /migration/commit to continue.");
    } else {
        server.send(400, "text/plain", uploadError.isEmpty() ? "Bridge upload failed" : uploadError);
    }
}

void handleCommitPost() {
    if (isBridge || !isOldLayoutRunning()) {
        server.send(409, "text/plain", "Partition migration commit is only available on the old espa-v2 layout.");
        return;
    }
    if (!verifyStagedBridgeImage()) {
        bridgeImageStaged = false;
        server.send(409, "text/plain", "No valid bridge image is staged at 0x340000.");
        return;
    }
    if (!migrationSettingsBackupReady) {
        server.send(409, "text/plain", "Preferences/Wi-Fi backup is not ready; refusing to commit.");
        return;
    }
    server.send(202, "text/plain", "Migration commit accepted. The device will reboot into the default_8MB bridge when complete.");
    commitRequested = true;
}

void beginFinalUpdate( int command) {
    finalUploadOk = false;
    if (command == U_SPIFFS) {
        finalFilesystemOk = false;
        finalFilesystemInstalled = false;
    } else {
        finalFirmwareInstalled = false;
    }
    updateError = "";
    finalUploadActive = false;
    if (isBridge == false || !checkNewLayout()) {
        updateError = "Final application/filesystem updates are only available on the new layout";
        return;
    }
    if (!settingsRestored) {
        updateError = "Restore and verify Preferences/Wi-Fi settings before installing final firmware";
        return;
    }
    const int updateType = command;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
        updateError = "Update.begin failed, error=" + String(Update.getError());
        return;
    }
    finalUploadActive = true;
}

void handleFinalFirmwareUpload() {
    HTTPUpload &upload = server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START:
            beginFinalUpdate(U_FLASH);
            break;
        case UPLOAD_FILE_WRITE:
            if (finalUploadActive && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                updateError = "Firmware write failed, error=" + String(Update.getError());
                Update.abort();
                finalUploadActive = false;
            }
            break;
        case UPLOAD_FILE_END:
            if (finalUploadActive && Update.end(true) && Update.isFinished()) {
                finalUploadOk = true;
                finalFirmwareInstalled = true;
            } else if (updateError.isEmpty()) {
                updateError = "Firmware update finalisation failed, error=" + String(Update.getError());
            }
            finalUploadActive = false;
            break;
        case UPLOAD_FILE_ABORTED:
            if (finalUploadActive) Update.abort();
            finalUploadActive = false;
            updateError = "Firmware upload aborted";
            break;
        default:
            break;
    }
}

void handleFinalFirmwarePost() {
    if (finalUploadOk) {
        server.send(200, "text/plain", "Final firmware installed. Rebooting into app0.");
        restartRequested = true;
    } else {
        server.send(500, "text/plain", updateError.isEmpty() ? "Firmware update failed" : updateError);
    }
}

void handleFinalFilesystemUpload() {
    HTTPUpload &upload = server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START:
            beginFinalUpdate(U_SPIFFS);
            break;
        case UPLOAD_FILE_WRITE:
            if (finalUploadActive && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                updateError = "Filesystem write failed, error=" + String(Update.getError());
                Update.abort();
                finalUploadActive = false;
            }
            break;
        case UPLOAD_FILE_END:
            if (finalUploadActive && Update.end(true) && Update.isFinished()) {
                finalFilesystemOk = true;
                finalFilesystemInstalled = true;
            } else if (updateError.isEmpty()) {
                updateError = "Filesystem update finalisation failed, error=" + String(Update.getError());
            }
            finalUploadActive = false;
            break;
        case UPLOAD_FILE_ABORTED:
            if (finalUploadActive) Update.abort();
            finalUploadActive = false;
            updateError = "Filesystem upload aborted";
            break;
        default:
            break;
    }
}

void handleFinalFilesystemPost() {
    if (finalFilesystemOk) {
        server.send(200, "text/plain", "Filesystem installed. You can now upload final firmware.");
        finalFilesystemOk = false;
    } else {
        server.send(500, "text/plain", updateError.isEmpty() ? "Filesystem update failed" : updateError);
    }
}

void handleRestoreSettingsPost() {
    if (!isBridge || !checkNewLayout()) {
        server.send(409, "text/plain", "Settings restore is only available on the new-layout bridge.");
        return;
    }
    if (settingsRestored) {
        server.send(200, "text/plain", "Settings have already been restored.");
        return;
    }
    const bool ok = importMigrationSettings();
    if (ok) {
        server.send(200, "text/plain", "Preferences and Wi-Fi settings restored and verified.");
    } else {
        server.send(500, "text/plain", "Settings restore failed. Migration backup retained for retry.");
    }
}

void installRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/migration/status", HTTP_GET, sendStatus);

    server.on("/migration/stage-bridge", HTTP_POST,
              handleBridgeUploadPost, handleBridgeUpload);
    server.on("/migration/commit", HTTP_POST, handleCommitPost);
    server.on("/migration/restore-settings", HTTP_POST, handleRestoreSettingsPost);

    server.on("/migration/final-filesystem", HTTP_POST,
              handleFinalFilesystemPost, handleFinalFilesystemUpload);
    server.on("/migration/final-firmware", HTTP_POST,
              handleFinalFirmwarePost, handleFinalFirmwareUpload);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
}

bool performMigrationCommit() {
    debugI("Starting one-time partition migration commit");

    if (esp_flash_encryption_enabled()) {
        debugW("Flash encryption is enabled; raw migration is disabled for safety");
        return false;
    }

    uint32_t flashSize = 0;
    if (esp_flash_get_size(esp_flash_default_chip, &flashSize) != ESP_OK ||
        flashSize != kExpectedFlashSize) {
        debugE("Unexpected flash size: %lu (expected 8 MB)",
                      static_cast<unsigned long>(flashSize));
        return false;
    }

    if (!isOldLayoutRunning() || !verifyStagedBridgeImage() || !verifyDefaultPartitionTable()) {
        debugE("Pre-commit validation failed");
        return false;
    }

    // Use the Preferences and Wi-Fi snapshot taken immediately after startup,
    // before the fallback AP can alter the temporary runtime AP config.
    // No credentials are logged.
    if (!migrationSettingsBackupReady) {
        debugE("Preferences/Wi-Fi backup is not ready; migration cancelled");
        return false;
    }
    alignas(4) MigrationBlob settingsBackup = migrationSettingsBackup;

    // Stop Wi-Fi before taking the raw NVS snapshot because Wi-Fi itself stores
    // settings in NVS. The application settings are already held in RAM.
    // IMPORTANT: Arduino's WiFi.mode(WIFI_OFF) already calls esp_wifi_stop()
    // and esp_wifi_deinit() internally. Do not call either IDF function again.
    debugI("Stopping Wi-Fi (current mode=%d)...", static_cast<int>(WiFi.getMode()));
    if (!WiFi.mode(WIFI_OFF)) {
        debugE("WiFi.mode(WIFI_OFF) failed; aborting before flash changes");
        return false;
    }
    debugI("Wi-Fi stopped and deinitialised by Arduino WiFi layer");

    debugI("Deinitialising NVS...");
    const esp_err_t nvsErr = nvs_flash_deinit();
    if (nvsErr != ESP_OK) {
        debugE("nvs_flash_deinit failed: %s (0x%x)",
                      esp_err_to_name(nvsErr), static_cast<unsigned>(nvsErr));
        return false; // No flash has been erased or changed yet.
    }
    debugI("NVS deinitialised");

    debugI("Backing up old partition table, NVS and settings...");
    if (!backupOldState(settingsBackup)) {
        debugE("Could not create verified migration backup; old NVS/table remain intact");
        return false;
    }
    debugI("Old state backup verified");

    // The new NVS region is the first 0x5000 bytes of the old NVS partition.
    // The final 0x2000 bytes become the new OTA-data partition.
    debugI("Erasing old NVS region (0x9000, 0x7000)...");
    if (!eraseFlash(kOldNvsOffset, kOldNvsSize)) {
        debugE("Could not erase old NVS area");
        if (restoreOldNvsFromBackup()) clearMigrationScratch();
        return false;
    }
    debugI("Old NVS region erased");

    debugI("Preparing new OTA metadata at 0xE000...");
    if (!writeNewOtaData()) {
        debugE("Could not prepare new OTA selection data; restoring old NVS");
        if (restoreOldNvsFromBackup()) clearMigrationScratch();
        return false;
    }
    debugI("New OTA metadata written and verified");

    // This is the commit point. OTA data and the bridge image are already ready.
    debugI("Committing new partition table at 0x8000...");
    if (!writeNewPartitionTable()) {
        debugE("Partition-table commit failed; attempting to restore old layout");
        const bool nvsRestored = restoreOldNvsFromBackup();
        const bool tableRestored = restoreOldPartitionTableFromBackup();
        if (nvsRestored && tableRestored) clearMigrationScratch();
        return false;
    }

    debugI("Partition migration committed. Rebooting into bridge at 0x340000.");
    delay(500);
    ESP.restart();
    return true;
}

} // namespace

void setup() {
    delay(300);
    WiFi.setHostname(kRemoteDebugHostname);
    debugI("eSpa combined migrator / bridge starting");

    if (esp_flash_encryption_enabled()) {
        debugW("WARNING: flash encryption is enabled; migration commit will be refused.");
    }

    runningPartition = esp_ota_get_running_partition();
    if (!runningPartition) {
        debugE("FATAL: unable to identify running application partition");
        return;
    }

    debugI("Running partition: %s @ 0x%08lX size 0x%08lX",
                  runningPartition->label,
                  static_cast<unsigned long>(runningPartition->address),
                  static_cast<unsigned long>(runningPartition->size));

    if (runningPartition->address == kNewApp1Offset && checkNewLayout()) {
        isBridge = true;
        debugI("Detected default_8MB bridge role");

        MigrationJournal journal;
        if (readJournal(journal)) {
            // Import is idempotent. Keep the scratch backup until Preferences
            // and Wi-Fi settings have both been restored and verified.
            if (!importMigrationSettings()) {
                settingsRestoreFailed = true;
                debugE("Settings restore failed; retaining backup and starting recovery AP");
                startFallbackAccessPoint();
            }
        } else if (!startWifiFromMigratedNvs()) {
            settingsRestoreFailed = true;
            debugW("No valid migration backup/marker; starting recovery AP");
            startFallbackAccessPoint();
        }

        if (!networkStarted && !fallbackApStarted) {
            startFallbackAccessPoint();
        }
    } else if (isOldLayoutRunning()) {
        isBridge = false;
        debugI("Detected espa-v2 migrator role");
        recoverInterruptedOldLayoutMigration();
        bridgeImageStaged = verifyStagedBridgeImage();

        WiFi.persistent(true); // Load existing credentials from old NVS.
        // Keep both interfaces available while migrating. This lets an
        // operator reach the migrator via the existing STA connection or the
        // saved SoftAP, and allows both configs to be backed up. The desired
        // post-migration mode comes from SoftAPAlwaysOn in Preferences.
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            debugE("Could not start AP+STA; falling back to STA mode");
            WiFi.mode(WIFI_MODE_STA);
            activeWifiMode = WIFI_MODE_STA;
        } else {
            activeWifiMode = WIFI_MODE_APSTA;
        }
        WiFi.begin(); // Reuse existing station credentials from old NVS.
        networkStarted = true;

        // Snapshot Preferences and both Wi-Fi interface configs before any
        // fallback AP operation can change the runtime AP credentials.
        migrationSettingsBackupReady = exportSettingsAndWifi(migrationSettingsBackup);
        if (!migrationSettingsBackupReady) {
            debugE("Could not snapshot Preferences/Wi-Fi settings; commit will be disabled");
        }
        // The original Wi-Fi configuration is now cached in RAM. Keep the
        // fallback AP from persisting temporary credentials into old NVS.
        WiFi.persistent(false);
    } else {
        debugE("FATAL: running partition does not match either supported layout");
        return;
    }

    // RemoteDebug needs an active Wi-Fi interface. Start it after the migrator
    // or bridge has configured STA/AP mode. Use Telnet-only transport so this
    // small firmware does not need ESPAsyncWebServer alongside WebServer.
#ifndef DEBUG_DISABLED
    remoteDebugStarted = Debug.begin(
        kRemoteDebugHostname, kRemoteDebugPort, RemoteDebug::INFO);
    if (remoteDebugStarted) {
        Debug.showDebugLevel(true);
        Debug.showTime(true);
        Debug.showProfiler(false);
        debugI("RemoteDebug started on TCP port %u", kRemoteDebugPort);
        debugI("eSpa migration firmware ready; role=%s",
               isBridge ? "bridge" : "migrator");
        debugI("Running partition: %s @ 0x%08lX size 0x%08lX",
               runningPartition->label,
               static_cast<unsigned long>(runningPartition->address),
               static_cast<unsigned long>(runningPartition->size));
        debugI("Settings restored=%s, migration backup ready=%s",
               settingsRestored ? "yes" : "no",
               migrationSettingsBackupReady ? "yes" : "no");
    }
#endif

    installRoutes();
    server.begin();
    networkStartMillis = millis();
    debugI("Migration HTTP server listening on port 80");
    debugI("Fallback AP, if needed: %s", MIGRATION_FALLBACK_AP_SSID);
}

void loop() {
#ifndef DEBUG_DISABLED
    if (remoteDebugStarted) {
        Debug.handle();
    }
#endif

    server.handleClient();

    if (commitRequested) {
        commitRequested = false;
        server.stop();
        delay(300);
        const bool ok = performMigrationCommit();
        if (!ok) {
            debugE("Migration commit failed; restoring old NVS if necessary and rebooting");
            // If the journal exists, restore the old NVS/table. This also covers
            // errors after NVS was erased but before the partition table changed.
            MigrationJournal journal;
            if (readJournal(journal)) {
                restoreOldNvsFromBackup();
                restoreOldPartitionTableFromBackup();
                // Leave the journal until after the restart so recovery is idempotent.
            }
            delay(500);
            ESP.restart();
        }
    }

    if (restartRequested) {
        restartRequested = false;
        delay(500);
        ESP.restart();
    }

    if (networkStarted && !fallbackApStarted &&
        (activeWifiMode & WIFI_MODE_STA) &&
        WiFi.status() != WL_CONNECTED &&
        millis() - networkStartMillis > 20000) {
        startFallbackAccessPoint();
    }

    delay(2);
}
