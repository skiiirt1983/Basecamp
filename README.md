# SLOPEFORM Bootloader — Project Basecamp

Boot launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**
(ESP32-S3, 16 MB flash, 8 MB octal PSRAM, CO5300 466×466 QSPI AMOLED, CST9217 touch, AXP2101 PMU).
ESP-IDF **5.5**.

Every power-on shows a round touch menu with **the apps you have installed (up to three)**. Tapping one
boots it. The app's own restarts, crashes and deep-sleep wake-ups stay inside that app. The next
power-on (or a press of the RST button) returns to the menu.

The apps are not built in. Add them from a phone or laptop through the open Wi-Fi **BaseCamp-Setup**
that the menu provides (upload a `.bin`, or download it from a GitHub release), and arrange their
order on a web page. **Your own apps work too:** see [docs/ADD_YOUR_OWN_APP.md](docs/ADD_YOUR_OWN_APP.md).

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
3. Unplug and replug (or press RST). The menu says **No apps yet** and shows the Wi-Fi **BaseCamp-Setup**.
4. Add apps, either way:
   - **One tap:** press **Install default apps**. The device asks for your home Wi-Fi (join
     **BaseCamp-Setup** on your phone, pick your network on the page that opens), then downloads
     PrintSphere, Capsule Radar and LyricFlash from their GitHub releases.
   - **Your own:** join **BaseCamp-Setup** (open, no password). The page opens by itself, or browse to
     `http://192.168.4.1`. Upload a `.bin`, or install from GitHub, and put the cards in order.

To replace only the launcher and keep the apps, their order and their settings, flash
**`Basecamp-<version>-launcher-only.bin`** at **0x90000**. The same file can be flashed again to
upgrade from an earlier version: it recognises apps installed by 0.2.x and keeps their names.

`tools/flash.sh` does the same with esptool: no arguments flashes the FRESH-INSTALL image,
`--launcher-only` writes the launcher, and `--offline` also writes the pinned app bins from
`apps/` (`tools/fetch_apps.sh`, checked against `tools/apps.sha256`) for setups without Wi-Fi.

## Using it

**The menu** has one card per installed app, in the order you chose. Tap to start. A small
**Updates** button checks GitHub for newer releases of the apps that came from GitHub and installs them
(settings are kept). A grey card with **⟳ retry** is an install that failed or an image that is damaged:
tap it to reinstall. Long-press an amber **⚠ changed** card to accept its new content (see
[Known risks](#known-risks-and-caveats)).

**The web page** (join **BaseCamp-Setup**, then it opens, or `http://192.168.4.1`):
- list of the apps with name, version, size and slot; a **round preview** of the menu;
- **▲ ▼** to change the order, **✎** to rename or pick a colour, **⟳** to update from GitHub, **✕** to remove;
- **Add an app:** upload a `.bin` (checked before anything is erased), or install from a GitHub repo;
- **Wi-Fi for GitHub downloads:** pick or type your network.

Changes show on the device straight away. The setup network switches off when an app starts.

## Layout

| Path | What |
|---|---|
| `partitions.csv` | Fixed partition table (see below) |
| `bootloader_components/main/` | Custom 2nd-stage bootloader entry (`call_start_cpu0` override) |
| `common/include/basecamp_bootsel.h` | Selection-word contract shared by bootloader and apps |
| `main/main.c` | Launcher controller: busy state, install jobs with retries, Wi-Fi gate, launching |
| `main/slots.c`, `main/meta.c` | Slot scan (state, overwrite guard, legacy migration) and per-slot metadata in NVS |
| `main/swriter.c` | Slot writer shared by uploads and downloads (validates before erasing) |
| `main/installer.c` | GitHub release lookup + download |
| `main/webui.c`, `main/webui.html` | Web server and the single-page web UI |
| `main/netmgr.c` | Open setup access point, captive DNS, on-demand station connection |
| `main/ui.c` | Round LVGL UI (menu, progress, updates) |
| `main/apps_logic.c` | Pure helpers (order, names, image-header check, marker search): host-tested |
| `components/board/` | Board bring-up: AXP2101, CO5300, CST9217, LVGL port |
| `.github/workflows/firmware.yml` | CI build, tests, release images |
| `test_apps/` | `qemu_selftest` (bootloader), `swriter_selftest` (slot writer/meta/scan), `dummy_app` (by hand on a board) |
| `tools/` | `ci_build.sh`, `make_images.sh`, `flash.sh`, `host_tests/`, `qemu_*.sh`, `webui_mock.py`, `ui_preview/`, `fetch_apps.sh` |
| `docs/ADD_YOUR_OWN_APP.md` | How to add an app of your own |

### Partition table (fixed)

| Name | Type | SubType | Offset | Size | Content |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 512 KB | shared by launcher + all apps |
| otadata | data | ota | 0x89000 | 8 KB | erased; **not** used for selection |
| phy_init | data | phy | 0x8B000 | 4 KB | |
| launcher | app | factory | 0x90000 | 1 MB | this launcher (~985 KB) |
| ota_0 | app | ota_0 | 0x190000 | 4 MB | app slot 1 |
| ota_1 | app | ota_1 | 0x590000 | 4 MB | app slot 2 |
| ota_2 | app | ota_2 | 0x990000 | 4 MB | app slot 3 |
| sounds | data | 0x83 (LittleFS) | 0xD90000 | 2.4 MB | shared data partition (PrintSphere uses it) |

The table is fixed, which is why there are three app slots of 4 MB: the number of apps is "however many
slots are filled", not a list of hard-wired apps.

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

## How the launcher works

- **Apps are discovered, not configured.** At start-up each of the three slots is read: is there a valid
  app image (`esp_image_verify`), what is its `esp_app_desc_t`? Cards are drawn for what is found, in the
  order stored in NVS.
- **Metadata** lives in NVS namespace `basecamp`: `name<i>`, `col<i>` (colour), `src<i>` (GitHub source),
  `tag<i>` (installed release), `sha<i>` (overwrite-guard baseline), `ord` (display order), `ssid`/`pass`.
  A slot flashed some other way (esptool, an older launcher) is named from its `project_name`.
- **Slot writer** (`swriter.c`): the first 64 bytes of a file are checked (ESP image, ESP32-S3, app
  descriptor present) **before the slot is erased**, so a wrong file never destroys the app that is
  there. Then it streams with `esp_ota_begin/write/end` (never `esp_ota_set_boot_partition`), the image
  hash is verified by `esp_ota_end`, and a failure blanks the slot rather than leaving half an image.
- **GitHub installs:** `GET /repos/<repo>/releases/latest`, pick the asset by name rules, download through
  the API asset URL (redirects to `release-assets.githubusercontent.com`, checked against the IDF CA
  bundle), check size, the SHA-256 `digest` GitHub publishes, an optional board marker and the app's
  `project_name`. Network hiccups (5xx, timeouts) are retried automatically (4 attempts).
- **Web server:** every `POST` needs the header `X-Basecamp: 1`. Browsers only let another web page send
  a custom header after a CORS preflight, which the device never answers, so other sites can't drive it.
- **AXP2101:** makes sure DCDC1 (`VCC3V3`, the SoC rail) is on, sets ALDO1/ALDO2 to 3.3 V and on
  (ALDO1 is `A3V3`, feeding the microphone ADC), and enables the battery/VBUS ADC and fuel gauge. Other
  rails keep their defaults.
- **Display and touch:** `espressif/esp_lcd_co5300` + `waveshare/esp_lcd_touch_cst9217` + `esp_lvgl_port`;
  init table, column gap, even-pixel rounder and touch mirroring from Waveshare's BSP (v3.0.1).

### Size budget (factory partition = 1 MB)
The launcher is about **985 KB** (CI prints `idf.py size` and fails above 1 MB). To fit Wi-Fi, TLS, a web
server and LVGL, `sdkconfig.defaults` sets: `-Os`; warning-level logs; silent asserts; ROM nano printf;
client-only TLS 1.2 with the common CA bundle; no WPA3, enterprise Wi-Fi or IPv6; only the LVGL widgets
in use (label, button, arc) with RGB565/A8/ARGB8888 rendering. About 40 KB remain.

## Build and CI

GitHub Actions (`.github/workflows/firmware.yml`, `espressif/esp-idf-ci-action`, ESP-IDF v5.5.6) runs
`tools/ci_build.sh` on every push and pull request: host unit tests, launcher + custom bootloader build,
`idf.py size`, packaging, and two QEMU self-tests. It uploads:
- `Basecamp-<ver>-FRESH-INSTALL-USB.bin` (16 MB, flash at 0x0);
- `Basecamp-<ver>-launcher-only.bin` (flash at 0x90000);
- `SHA256SUMS.txt`.

On `main`, a version without a suffix (`PROJECT_VER` in `CMakeLists.txt`) also publishes the GitHub
Release `v<ver>`.

Locally (ESP-IDF v5.5.x): `idf.py build && tools/make_images.sh`, or `tools/ci_build.sh` for the whole sequence.

## Tests

| Test | Run | What it covers |
|---|---|---|
| Host unit tests | `tools/host_tests/run.sh [apps/]` | order logic, name/JSON/URL helpers, image-header check, board-marker search (also against the real app images) |
| QEMU bootloader test | `tools/qemu_selftest.sh` | the **real custom bootloader**: cold boot → launcher; each slot boots and survives restart + panic; an erased slot falls back to the launcher, not to a neighbour; a corrupt selection word is ignored |
| QEMU slot-writer test | `tools/qemu_swriter_test.sh` | real image written into a slot in many chunk sizes; non-ESP files, bootloader images, wrong-chip images rejected **without erasing**; truncated/corrupt images blanked; NVS metadata; slot scan (damaged, retry card, erase) |
| Web page vs a device mock | `NODE_PATH=<dir with playwright> node tools/webui_test.js` (or `python3 tools/webui_mock.py` and open the page) | ordering, rename, upload/replace/reject, GitHub job progress, Wi-Fi, hostile names, missing-header refusal |
| Round-screen preview | `tools/ui_preview/run.sh` | compiles LVGL + the real `ui.c` for the PC and renders every screen into the 466 px circle |

QEMU has no Wi-Fi and no emulated display, so the access point, the web server, the GitHub download and
the real screen can only be tested on the board.

`test_apps/dummy_app` (`tools/build_dummies.sh`) is a small app for checking slot selection on a board
by hand (Restart / Crash / Sleep buttons).

## ⚠️ Known risks and caveats

### Update through the launcher, not inside the apps
Each app's built-in OTA updater writes to `esp_ota_get_next_update_partition()`, which is the **next** OTA slot. It does not write over itself:

| Updating from | Overwrites |
|---|---|
| slot 1 (ota_0) | **slot 2** (ota_1) |
| slot 2 (ota_1) | **slot 3** (ota_2) |
| slot 3 (ota_2) | **slot 1** (ota_0) |

The app then sets otadata, which this bootloader ignores, and restarts into its **old version**, with a
neighbouring app destroyed. The launcher's **Updates** button replaces this: it writes each app into its
own slot and keeps its settings.

**Guard (detection).** The launcher records each slot's image SHA-256 in NVS the first time it sees it, or
right after an install.
- If a slot's image later changes, the card shows "⚠ changed" in amber and names whatever is really there now.
- Long-press the card to accept the new image as the baseline.

**Prevention options investigated and rejected:**
- **SPI-flash status-register block protection.** BP/TB/CMP ranges are chip-specific and can only cover the top or bottom of flash, so they would also lock nvs or sounds. The 2nd-stage bootloader clears them on every boot anyway, and esptool would then fail to flash.
- **Intercepting writes.** Not possible without modifying the apps.
- **Changing the partition table.** The table is fixed.

### Other caveats
- **The setup Wi-Fi is open (no password) by design.** While the menu is showing, anyone within range can
  use the page, including uploading firmware. It is off whenever an app is running. Don't leave the
  device on the menu somewhere you don't trust.
- **Three apps, 4 MB each.** The partition table is fixed.
- **A replaced app is gone.** Uploading or downloading into an occupied slot erases it as soon as the
  new file has passed its header check, and there is no spare slot to fall back to. If the transfer then
  fails, the slot is left empty; a GitHub app keeps a grey "retry" card.
- **The FRESH-INSTALL image wipes everything,** including every app's settings (Wi-Fi credentials, tokens, preferences) and the launcher's Wi-Fi. To keep settings, flash the launcher-only image at 0x90000 or update through **Updates**.
- **Shared NVS.** All apps and the launcher share one `nvs` partition. Namespaces usually differ, but IDF Wi-Fi config (`nvs.net80211`) is shared. An app that relies on stored Wi-Fi config may see another app's network.
- **Wi-Fi:** 2.4 GHz WPA/WPA2 (or open) networks only. WPA3-only networks are not supported, to keep the launcher under 1 MB. While the device is connected to your router for a GitHub download, the setup network follows the router's channel, so a phone on it may reconnect once. Unauthenticated GitHub API calls are limited to 60 per hour per IP address.
- **Crash loops.** An app that crash-loops stays in that app; this is how "restarts stay in the app" works. A power cycle or the RST button always returns to the menu.
- **Flash mode.** The launcher uses QIO like Waveshare's demos. Espressif QEMU mis-emulates the app-side QIO read. If a board ever shows every slot as damaged after an install, rebuild with `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`.

## Verification status
- ✅ **On hardware (0.1.x–0.2.x):** the menu boots each of three apps; the dummy-app restart/crash/sleep checks pass; a power cycle returns to the menu; first-boot Wi-Fi setup and GitHub installs worked (including the 504 retry fix).
- ✅ **CI on every push:** host tests, the build (launcher under 1 MB), packaging checks, both QEMU self-tests.
- ✅ **Off-device checks (0.3.0):** the web page against a device mock (25 behaviours), every launcher screen rendered with the real UI code, slot scan of the three real app images in QEMU.
- ⏳ **Not yet tested on hardware (0.3.0):** the open access point and captive portal, the web server (uploads over Wi-Fi), the dynamic menu on the real display, and the launcher-only upgrade from 0.2.x.
