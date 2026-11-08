# SLOPEFORM Bootloader — Project Basecamp

Boot launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**
(ESP32-S3, 16 MB flash, 8 MB octal PSRAM, CO5300 466×466 QSPI AMOLED, CST9217 touch, AXP2101 PMU).
ESP-IDF **5.5**.

Every power-on shows a round touch menu with three apps. Tapping one boots it.
The app's own restarts, crashes and deep-sleep wake-ups stay inside that app.
The next power-on (or a press of the RST button) returns to the menu.

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

## Layout

| Path | What |
|---|---|
| `partitions.csv` | Fixed partition table (see below) |
| `bootloader_components/main/` | Custom 2nd-stage bootloader entry (`call_start_cpu0` override) |
| `common/include/basecamp_bootsel.h` | Selection-word contract shared by bootloader and apps |
| `main/` | Factory launcher app (LVGL UI, slot scan, overwrite guard) |
| `components/board/` | Board bring-up: AXP2101, CO5300, CST9217, LVGL port |
| `test_apps/dummy_app/` | Dummy OTA app with a UI, for testing slot selection on hardware |
| `test_apps/qemu_selftest/` | Headless automated test of the bootloader logic (QEMU or board) |
| `tools/` | `fetch_apps.sh`, `flash.sh`, `package.sh`, `build_dummies.sh`, `qemu_selftest.sh` |

### Partition table (fixed)

| Name | Type | SubType | Offset | Size | Content |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 512 KB | shared by launcher + all apps |
| otadata | data | ota | 0x89000 | 8 KB | erased; **not** used for selection |
| phy_init | data | phy | 0x8B000 | 4 KB | |
| launcher | app | factory | 0x90000 | 1 MB | this launcher (~570 KB) |
| ota_0 | app | ota_0 | 0x190000 | 4 MB | PrintSphere v1.6.2 |
| ota_1 | app | ota_1 | 0x590000 | 4 MB | Capsule Radar v1.6.0 (Arduino) |
| ota_2 | app | ota_2 | 0x990000 | 4 MB | LyricFlash v1.0.0 |
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

- LVGL 9.4 on a black AMOLED screen: three stacked pill buttons that fit inside the circle, a title at the top, and USB/battery status at the bottom.
- Each card shows the app name and version, taken from that slot's `esp_app_desc_t`, plus the slot label.
  - Names come from `project_name` (`printsphere_idf` → PrintSphere, `lyricflash` → LyricFlash, anything unknown is shown as-is). The menu therefore shows what is *actually* in each slot.
  - Capsule Radar is an Arduino build. Its descriptor only says `arduino-lib-builder` / `37d69ca`, so the card shows the configured name and version label instead, marked with `*`. This is set in `menuconfig → Basecamp launcher`.
- Empty or invalid slots (no descriptor, or a failed SHA-256 image check) are greyed out and can't be tapped.
- A tap shows "Starting <app>", blanks the panel, writes STORE0 and restarts.
- **AXP2101**: makes sure DCDC1 (`VCC3V3`, the SoC rail) is on, sets ALDO1/ALDO2 (`VL1_3.3V`/`VL2_3.3V` on the schematic) to 3.3 V and on, and enables the battery/VBUS ADC and fuel gauge for the footer. All other rails keep their OTP defaults. Waveshare's own examples leave every rail at OTP defaults.
- **Display and touch**: `espressif/esp_lcd_co5300` + `waveshare/esp_lcd_touch_cst9217` + `esp_lvgl_port`. The init table, 6-px column gap, even-pixel rounder and touch mirroring are taken from Waveshare's BSP (v3.0.1). The full BSP isn't used because it pulls in audio codecs, USB and an LVGL adapter.

## Build

```bash
. $IDF_PATH/export.sh          # ESP-IDF v5.5.x
idf.py set-target esp32s3      # first time only
idf.py build                   # builds launcher + custom bootloader
tools/package.sh               # copies bootloader/partition table/launcher to dist/
```

## Stage 1 — prove slot selection with dummy apps

```bash
tools/build_dummies.sh                     # dist/dummy_{a,b,c}.bin
tools/flash.sh --dummy -p /dev/ttyACM0
```

On the device:
1. After flashing (USB reset = cold) → **menu** shows `basecamp_dummy_a/b/c`, version `dummy-x-1.0`.
2. Tap a card → "DUMMY A/B/C" screen shows `ota_N @ 0x…` matching the card, `bootsel: ota_N`.
3. **Restart**, **Crash** and **Sleep 2s** → the same dummy comes back, and its boot counter goes up.
4. Unplug/replug (or RST button, or PWR off/on) → **menu**.

Automated, no hardware (needs `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`):

```bash
tools/qemu_selftest.sh
```

This boots the **real custom bootloader** in Espressif QEMU, with one test binary in factory and all three OTA slots. It checks:
- a cold boot goes to the launcher;
- each slot boots when selected and stays put through `esp_restart()` and a panic;
- clearing the word goes back to the launcher;
- an erased ota_2 falls back to the **launcher, not ota_1**;
- a corrupt word goes to the launcher.

QEMU doesn't emulate deep-sleep wake-up, so that step only runs when the test is built with `-DSELFTEST_DEEP_SLEEP=1` (e.g. on a board).

## Stage 2 — real apps

```bash
tools/fetch_apps.sh                 # downloads + sha256-verifies into apps/
tools/flash.sh -p /dev/ttyACM0      # add --print to only show the command
```

`flash.sh` runs **one esptool command**. It writes the bootloader, partition table, launcher and the three app images unmodified at their offsets (esptool only patches the header of the image at 0x0). It also writes 0xFF images over nvs, otadata and sounds, which erases them, and generates those blank images itself:

```bash
esptool.py --chip esp32s3 -p PORT -b 921600 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 dist/bootloader.bin  0x8000 dist/partition-table.bin \
  0x9000 dist/blank/nvs.bin  0x89000 dist/blank/otadata.bin \
  0x90000 dist/launcher.bin \
  0x190000 apps/printsphere_ota-v1.6.2.bin \
  0x590000 apps/CapsuleRadar-ota.bin \
  0x990000 apps/lyricflash-ota.bin \
  0xD90000 dist/blank/sounds.bin
```

| Image | sha256 |
|---|---|
| printsphere_ota-v1.6.2.bin | `6be314d4b337779bf50ad500e72bdd38271871b4c5ec292782c5dcb1410ad2ea` |
| CapsuleRadar-ota.bin | `084d399ee11798d07548b0eccfa60a83b9411f590eca9e0df86b6016b283ccf8` |
| lyricflash-ota.bin | `7fa1a27537209157f418ae44707509489b454540d05d0bd85dfec0bdddda5610` |

## ⚠️ Known risks and caveats

### Don't update from inside the apps
Each app's built-in OTA updater writes to `esp_ota_get_next_update_partition()`, which is the **next** OTA slot. It does not write over itself:

| Updating from | Overwrites |
|---|---|
| PrintSphere (ota_0) | **Capsule Radar** (ota_1) |
| Capsule Radar (ota_1) | **LyricFlash** (ota_2) |
| LyricFlash (ota_2) | **PrintSphere** (ota_0) |

The app then sets otadata, which this bootloader ignores, and restarts. Because STORE0 still points at the old slot, the **old version boots again**. The update appears not to apply, and a neighbouring app is destroyed. To update an app, download its new OTA bin, update `tools/apps.sha256`, and re-flash that slot over USB:

```bash
esptool.py --chip esp32s3 write_flash 0x590000 new.bin
```

**Guard (detection).** On first boot after flashing, the launcher records each slot's image SHA-256 in NVS (namespace `basecamp`).
- If a slot's image later changes, the card shows "⚠ changed" in amber. Because names come from the descriptor, the card also names whatever is really there now.
- Long-press the card to accept the new image as the baseline.
- A full `flash.sh` erases NVS, which resets all baselines.

**Prevention options investigated and rejected:**
- **SPI-flash status-register block protection.** BP/TB/CMP ranges are chip-specific and can only cover the top or bottom of flash, so they would also lock nvs or sounds. The 2nd-stage bootloader clears them on every boot anyway, and esptool would then fail to flash.
- **Intercepting writes.** Not possible without modifying the apps.
- **Changing the partition table.** The table is fixed.

### Other caveats
- **Shared NVS.** All three apps and the launcher share one `nvs` partition. Namespaces usually differ, but IDF Wi-Fi config (`nvs.net80211`) is shared. An app that relies on stored Wi-Fi config may see another app's network.
- **Crash loops.** An app that crash-loops stays in that app; this is how "restarts stay in the app" works. A power cycle or the RST button always returns to the menu.
- **`sounds` is used by PrintSphere, not LyricFlash.** Strings in the binaries show PrintSphere mounting LittleFS `sounds` (`/sounds/snd_%u.pcm`). LyricFlash only contains the BSP's generic `storage`/SPIFFS string, and no partition by that label exists here. Either way, `sounds` starts empty.
- **Capsule Radar** logs `No core dump partition found!` at boot. This is harmless, because the table has no coredump partition.
- **Flash mode.** The launcher uses QIO like Waveshare's demos. Espressif QEMU mis-emulates the app-side QIO read (bytes shifted). On real hardware QIO is the normal setting, but if the launcher shows every slot as "empty", rebuild with `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`.

## Verification status
- ✅ Launcher, custom bootloader and dummy apps build with ESP-IDF v5.5.6. Launcher is ~570 KB of 1 MB; bootloader is 21.7 KB.
- ✅ `tools/qemu_selftest.sh` passes: all bootloader decisions as listed above.
- ✅ In QEMU, the bootloader verifies and loads all three real images from their own offsets. Their crash-reboot loops under emulation stay in the same slot. These aborts come from QEMU missing octal PSRAM and the display, so they say nothing about how the apps behave on the board.
- ✅ In QEMU, the launcher slot scan reads all three real images correctly (PrintSphere v1.6.2, Capsule Radar v1.6.0\*, LyricFlash 1.0.0). It flags a corrupted image as invalid and flags a neighbour overwrite as changed.
- ✅ **Confirmed on the physical board:** stage 1 (dummy apps) and stage 2 (PrintSphere, Capsule Radar, LyricFlash) flashed as single full images at 0x0, and the menu boots each app.
  Single-image flashing: `esptool.py --chip esp32s3 -b 921600 write_flash 0x0 basecamp-stage2-apps.bin` (or Espressif Flash Download Tool, address 0x0, SPI mode DIO, 80 MHz, DoNotChgBin).
  To build those images: `esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o out.bin --flash_mode dio --flash_freq 80m --flash_size 16MB` followed by the same offset/file pairs as `tools/flash.sh`.
