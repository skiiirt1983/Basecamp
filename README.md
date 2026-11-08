# SLOPEFORM Bootloader — Project Basecamp

Boot launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**
(ESP32-S3, 16 MB flash, 8 MB octal PSRAM, CO5300 466×466 QSPI AMOLED, CST9217 touch, AXP2101 PMU).
ESP-IDF **5.5**.

Every power-on shows a round touch menu with three apps. Tapping one boots it.
The app's own restarts, crashes and deep-sleep wake-ups stay inside that app.
The next power-on (or a press of the RST button) returns to the menu.

The USB image contains only the bootloader, partition table and launcher. On first boot the
launcher sets up Wi-Fi and downloads the three apps from their GitHub releases; later it
updates them through its **Updates** button.

```
power-on ─► custom bootloader ─► cold reset? ──yes──► launcher (factory) ──tap──► STORE0 = slot, esp_restart()
                                     │no                                                 │
                                     ▼                                                   ▼
                        valid word in RTC STORE0?  ──no──► launcher           custom bootloader
                                     │yes                                                │
                        slot image verifies?       ──no──► launcher            boots ota_<slot>
                                     │yes                                    (restarts stay here)
                                     ▼
                               boot ota_<slot>
```

## Install (fresh device)

1. Download **`Basecamp-<version>-FRESH-INSTALL-USB.bin`** from the latest
   [release](https://github.com/skiiirt1983/project-basecamp/releases/latest).
2. Flash it at **0x0**. It is a full 16 MB image, so it **erases everything**, including all app
   settings.
   - esptool: `esptool.py --chip esp32s3 -b 921600 write_flash 0x0 Basecamp-<version>-FRESH-INSTALL-USB.bin`
   - Espressif Flash Download Tool: ESP32-S3, one row with the file at `0x0`, SPI mode DIO,
     80 MHz, DoNotChgBin ticked, START.
   - Browser: https://espressif.github.io/esptool-js/ (Chrome/Edge), Flash Address `0x0`, Program.
3. Unplug and replug (or press RST). All slots are empty, so setup starts by itself:
   - **Wi-Fi:** the screen says to join **`Basecamp-Setup`** on a phone. The setup page opens by
     itself (or browse to `192.168.4.1`). Pick your 2.4 GHz network, enter the password, tap
     Connect. The credentials are kept in NVS (namespace `basecamp`).
   - **Download:** one progress ring per app. Each app comes from its GitHub release's newest
     stable version and is written straight into its slot. Afterwards the launcher restarts into the menu.

To replace only the launcher and keep the apps and their settings, flash
**`Basecamp-<version>-launcher-only.bin`** at **0x90000**.

`tools/flash.sh` does the same with esptool: no arguments flashes the FRESH-INSTALL image,
`--launcher-only` writes the launcher, and `--offline` also writes the pinned app bins from
`apps/` (`tools/fetch_apps.sh`, checked against `tools/apps.sha256`) for setups without Wi-Fi.

## Layout

| Path | What |
|---|---|
| `partitions.csv` | Fixed partition table (see below) |
| `bootloader_components/main/` | Custom 2nd-stage bootloader entry (`call_start_cpu0` override) |
| `common/include/basecamp_bootsel.h` | Selection-word contract shared by bootloader and apps |
| `main/` | Factory launcher: UI, slot scan + overwrite guard (`slots.c`), Wi-Fi + setup portal (`netmgr.c`), GitHub installer (`installer.c`) |
| `components/board/` | Board bring-up: AXP2101, CO5300, CST9217, LVGL port |
| `.github/workflows/firmware.yml` | CI build, QEMU self-test, release images |
| `test_apps/dummy_app/` | Dummy OTA app with a UI, for testing slot selection on hardware |
| `test_apps/qemu_selftest/` | Headless automated test of the bootloader logic (QEMU or board) |
| `tools/` | `ci_build.sh`, `make_images.sh`, `flash.sh`, `qemu_selftest.sh`, `fetch_apps.sh`, `build_dummies.sh` |

### Partition table (fixed)

| Name | Type | SubType | Offset | Size | Content |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 512 KB | shared by launcher + all apps |
| otadata | data | ota | 0x89000 | 8 KB | erased; **not** used for selection |
| phy_init | data | phy | 0x8B000 | 4 KB | |
| launcher | app | factory | 0x90000 | 1 MB | this launcher (~960 KB) |
| ota_0 | app | ota_0 | 0x190000 | 4 MB | PrintSphere (cptkirki/PrintSphere) |
| ota_1 | app | ota_1 | 0x590000 | 4 MB | Capsule Radar (socquique/capsule-radar, Arduino) |
| ota_2 | app | ota_2 | 0x990000 | 4 MB | LyricFlash (skiiirt1983/LyricFlash) |
| sounds | data | 0x83 (LittleFS) | 0xD90000 | 2.4 MB | erased; LittleFS formats on first mount |

## Boot logic

The selection is a 32-bit word in **`RTC_CNTL_STORE0_REG`**:
`[31:16] 0xBA5E magic | [15:8] ~slot | [7:0] slot`.

Why STORE0:
- In the ESP32-S3 ROM header (`esp_rom/esp32s3/include/esp32s3/rom/rtc.h`), STORE1–7 are taken (slow-clock calibration, boot time, XTAL frequency, wake stub, reset cause, RTC-memory CRC). STORE0 is listed as *Reserved*.
- In ESP-IDF v5.5.6, nothing writes STORE0 outside the ULP unit tests (checked with grep). The Arduino core doesn't use it either.
- It sits in the RTC power domain, so it survives software resets, panic and watchdog resets, and deep sleep. A power-on or chip reset clears it.
- RTC *memory* was rejected. The prebuilt apps were not built with `CONFIG_BOOTLOADER_RESERVE_RTC_MEM`, so they are free to use that area themselves (RTC_DATA variables, RTC-fast heap) and could overwrite the word.

Bootloader decisions (`bootloader_components/main/bootloader_start.c`):

| Reset reason | Selection word | Result |
|---|---|---|
| Power-on / EN (RST button), brownout, RTC WDT, glitch, eFuse CRC | any | word cleared → **launcher** |
| USB-Serial-JTAG chip reset (esptool, `idf.py monitor`) | any | word cleared → **launcher** (`CONFIG_BASECAMP_USB_RESET_IS_COLD`, default y) |
| SW reset, panic, task/int WDT, deep-sleep wake | valid, slot image verifies | **that slot** (the word is kept) |
| same | missing / corrupt | **launcher** |
| same | valid, but the slot is empty or corrupt | word cleared → **launcher** |

The stock `bootloader_utility_load_boot_image()` falls back to *lower OTA slots* before trying factory when an image fails to load. For example, a broken ota_2 would boot ota_1. So the custom bootloader runs `esp_image_verify()` on the chosen slot first and goes straight to the launcher if that fails. With a factory partition present, the IDF loader never writes otadata, and otadata is never read for selection.

## Launcher

- **Menu:** LVGL 9.4 on a black AMOLED screen. Three stacked pill cards fit inside the circle, with the title and USB/battery status at the top and a small **Updates** button at the bottom.
- **Cards** show the app name and version from that slot's `esp_app_desc_t`, plus the slot label.
  - Names come from `project_name` (`printsphere_idf` → PrintSphere, `lyricflash` → LyricFlash; anything unknown is shown as-is), so the menu shows what is *actually* in each slot.
  - Capsule Radar is an Arduino build whose descriptor only says `arduino-lib-builder`. Its card shows the release tag the launcher installed (or the configured label, marked `*`).
- **Tap an installed card:** "Starting <app>", the panel blanks, STORE0 is written and the board restarts into the app.
- **Tap a grey card (empty or failed slot):** that app is installed from GitHub. Wi-Fi setup runs first if needed.
- **Updates:** checks the three latest releases and lists `current -> available` per app. **Install** downloads the newer ones. Updating a slot doesn't touch NVS, so the app's settings survive.

### Installing from GitHub (`main/installer.c`)
- `GET https://api.github.com/repos/<repo>/releases/latest` (User-Agent set), then pick the asset by name:

  | Slot | Repo | Asset | `project_name` must contain |
  |---|---|---|---|
  | ota_0 | cptkirki/PrintSphere | `printsphere_ota*.bin` | `printsphere` |
  | ota_1 | socquique/capsule-radar | `CapsuleRadar-ota.bin` | `arduino-lib-builder` (Arduino) |
  | ota_2 | skiiirt1983/LyricFlash | `lyricflash-ota.bin` | `lyricflash` |

  All of these are Kconfig options (`menuconfig → Basecamp launcher → App sources`).
- **Checks:**
  - Assets larger than the 4 MB slot are rejected.
  - The download follows the redirect to `release-assets.githubusercontent.com`, verified against the IDF CA bundle.
  - It is streamed into the slot with `esp_ota_begin/write/end` on that partition. **`esp_ota_set_boot_partition` is never called.**
  - The SHA-256 is checked against the asset's `digest` when GitHub provides one. `esp_ota_end` verifies the image, and `project_name` is checked afterwards.
- **On success:** the release tag is saved in NVS (`tag0`..`tag2`) and the overwrite guard is re-baselined.
- **On any failure:** the slot is blanked, so it shows as empty and can be retried. A failed *update* therefore also removes the old version of that app until a retry succeeds.

### Hardware bring-up
- **AXP2101:** makes sure DCDC1 (`VCC3V3`, the SoC rail) is on, sets ALDO1/ALDO2 (`VL1_3.3V`/`VL2_3.3V`; ALDO1 is `A3V3`, which feeds the ES7210 mic ADC) to 3.3 V and on, and enables the battery/VBUS ADC and fuel gauge. All other rails keep their OTP defaults.
- **Display and touch:** `espressif/esp_lcd_co5300` + `waveshare/esp_lcd_touch_cst9217` + `esp_lvgl_port`. The init table, 6-px column gap, even-pixel rounder and touch mirroring come from Waveshare's BSP (v3.0.1).

### Size budget (factory partition = 1 MB)
The launcher is about **960 KB** (CI prints `idf.py size` and fails above 1 MB). To fit Wi-Fi, TLS and LVGL, `sdkconfig.defaults` sets:
- `-Os`, warning-level logs, silent asserts, ROM nano printf;
- client-only TLS 1.2 with the common CA bundle and only the ciphers and curves GitHub needs;
- no WPA3, enterprise Wi-Fi or IPv6;
- only the LVGL widgets in use (label, button, arc), with RGB565/A8/ARGB8888 rendering.

## Build and CI

GitHub Actions (`.github/workflows/firmware.yml`, `espressif/esp-idf-ci-action`, ESP-IDF v5.5.6) runs `tools/ci_build.sh` on every push and pull request. It builds the launcher and custom bootloader, prints `idf.py size`, runs the QEMU bootloader self-test, and packages:
- `Basecamp-<ver>-FRESH-INSTALL-USB.bin` (16 MB, flash at 0x0);
- `Basecamp-<ver>-launcher-only.bin` (flash at 0x90000);
- `SHA256SUMS.txt`.

Every run uploads these as artifacts. On `main`, a version without a suffix (`PROJECT_VER` in `CMakeLists.txt`) also publishes the GitHub Release `v<ver>`.

Locally (ESP-IDF v5.5.x): `idf.py build && tools/make_images.sh`, or `tools/ci_build.sh` for the whole CI sequence.

## Tests

`tools/qemu_selftest.sh` boots the **real custom bootloader** in Espressif QEMU, with one test binary in factory and all three OTA slots. It checks:
- a cold boot goes to the launcher;
- each slot boots when selected and stays put through `esp_restart()` and a panic;
- clearing the word goes back to the launcher;
- an erased ota_2 falls back to the **launcher, not ota_1**;
- a corrupt word goes to the launcher.

QEMU doesn't emulate deep-sleep wake-up, so that step only runs with `-DSELFTEST_DEEP_SLEEP=1` (on a board). QEMU has no Wi-Fi either, so the setup portal and the GitHub installer can only be tested on hardware.

`test_apps/dummy_app` (`tools/build_dummies.sh`) is a small app for checking slot selection on a board by hand (Restart / Crash / Sleep buttons).

## ⚠️ Known risks and caveats

### Update through the launcher's Updates button, not inside the apps
Each app's built-in OTA updater writes to `esp_ota_get_next_update_partition()`, which is the **next** OTA slot. It does not write over itself:

| Updating from | Overwrites |
|---|---|
| PrintSphere (ota_0) | **Capsule Radar** (ota_1) |
| Capsule Radar (ota_1) | **LyricFlash** (ota_2) |
| LyricFlash (ota_2) | **PrintSphere** (ota_0) |

The app then sets otadata, which this bootloader ignores, and restarts into its **old version**, with a neighbouring app destroyed. The launcher's **Updates** button replaces this: it writes each app into its own slot and keeps its settings.

**Guard (detection).** The launcher records each slot's image SHA-256 in NVS (namespace `basecamp`) after installing it, or the first time it sees it.
- If a slot's image later changes, the card shows "⚠ changed" in amber and names whatever is really there now.
- Long-press the card to accept the new image as the baseline.

**Prevention options investigated and rejected:**
- **SPI-flash status-register block protection.** BP/TB/CMP ranges are chip-specific and can only cover the top or bottom of flash, so they would also lock nvs or sounds. The 2nd-stage bootloader clears them on every boot anyway, and esptool would then fail to flash.
- **Intercepting writes.** Not possible without modifying the apps.
- **Changing the partition table.** The table is fixed.

### Other caveats
- **The FRESH-INSTALL image wipes everything,** including every app's settings (Wi-Fi credentials, tokens, preferences) and the launcher's Wi-Fi. To keep settings, flash the launcher-only image at 0x90000 or update apps through **Updates**.
- **Shared NVS.** All three apps and the launcher share one `nvs` partition. Namespaces usually differ, but IDF Wi-Fi config (`nvs.net80211`) is shared. An app that relies on stored Wi-Fi config may see another app's network.
- **Wi-Fi:** 2.4 GHz WPA/WPA2 (or open) networks only. WPA3-only networks are not supported, to keep the launcher under 1 MB. Unauthenticated GitHub API calls are limited to 60 per hour per IP address.
- **Crash loops.** An app that crash-loops stays in that app; this is how "restarts stay in the app" works. A power cycle or the RST button always returns to the menu.
- **`sounds` is used by PrintSphere,** not LyricFlash (PrintSphere mounts LittleFS `sounds`, `/sounds/snd_%u.pcm`). It starts empty.
- **Capsule Radar** logs `No core dump partition found!` at boot. This is harmless.
- **Flash mode.** The launcher uses QIO like Waveshare's demos. Espressif QEMU mis-emulates the app-side QIO read. If a board ever shows every slot as "empty" after an install, rebuild with `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`.

## Verification status
- ✅ **On hardware (0.1.x):** the menu boots each of the three apps; the dummy-app restart/crash/sleep checks pass; a power cycle returns to the menu.
- ✅ **QEMU (every CI run):** the bootloader decisions listed above.
- ✅ **CI/local build:** the launcher fits (about 960 KB of 1 MB). The FRESH-INSTALL image has the launcher in place and nvs, otadata, ota_0–2 and sounds erased (checked by `make_images.sh`).
- ✅ **GitHub API response format** (asset names, `digest`, redirect to `release-assets.githubusercontent.com`) checked against LyricFlash's live release.
- ⏳ **Not yet tested on hardware (0.2.0):** the Wi-Fi setup portal, downloading and installing from GitHub, the Updates screen, and the new menu layout.
