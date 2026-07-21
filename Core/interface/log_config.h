#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

/*
 * Module log switches:
 *   1 = compile this module's diagnostic logs
 *   0 = remove log calls and format strings at preprocessing time
 *
 * Each value may also be overridden from the compiler command line.
 * Shell command responses are functional output and are not controlled here.
 */
/* main.c: startup, LittleFS mount, shell/OTA entry selection */
#ifndef LOG_MAIN_ENABLE
#define LOG_MAIN_ENABLE                 1
#endif

/* ota_state_machine.c: state transitions and OTA/revert decisions */
#ifndef LOG_OTA_STATE_MACHINE_ENABLE
#define LOG_OTA_STATE_MACHINE_ENABLE    1
#endif

/* data_storage.c: LittleFS package write callback */
#ifndef LOG_DATA_STORAGE_ENABLE
#define LOG_DATA_STORAGE_ENABLE         1
#endif

/* xmodem.c: protocol detection, packet errors and transfer progress */
#ifndef LOG_XMODEM_ENABLE
#define LOG_XMODEM_ENABLE               1
#endif

/* w25qxx.c: device detection and erase/read/write progress */
#ifndef LOG_W25QXX_ENABLE
#define LOG_W25QXX_ENABLE               0
#endif

/* esp8266.c: WiFi/MQTT status, errors, ThingsBoard OTA progress and ESP-AT trace */
#ifndef LOG_WIFI_ENABLE
#define LOG_WIFI_ENABLE                 1
#endif

/* flash_bootloader.c: erase, ELF load, Flash write and App jump */
#ifndef LOG_BOOTLOADER_ENABLE
#define LOG_BOOTLOADER_ENABLE           1
#endif

/* app_verify.c: SHA-256 and ECDSA package verification */
#ifndef LOG_APP_VERIFY_ENABLE
#define LOG_APP_VERIFY_ENABLE           0
#endif

/* elf_loader.c: ELF validation, relocation and section details */
#ifndef LOG_ELF_LOADER_ENABLE
#define LOG_ELF_LOADER_ENABLE           0
#endif

/* lfs.c: LittleFS debug, warning and error messages (trace remains off) */
#ifndef LOG_LITTLEFS_ENABLE
#define LOG_LITTLEFS_ENABLE             1
#endif

#endif /* LOG_CONFIG_H */
