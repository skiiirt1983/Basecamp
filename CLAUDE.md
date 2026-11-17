# CLAUDE.md — SLOPEFORM Basecamp: rules, hand-over and reference

> **KEEP THIS FILE CURRENT.** Any change to code, config, docs, tests, CI, behaviour, known issues or
> decisions must update this file **in the same commit** (status snapshot, changelog, open issues,
> whatever the change touches). A model that reads only this file and README.md should be able to
> continue the work. If you learn something non-obvious, write it down in "Lessons learned".
> Docs-only commits do not trigger CI (see "CI and releases").

Last updated for: **0.3.4** — released (CI run #10 green, release `v0.3.4`). Check the Releases page for anything newer.

---

## 1. Working rules (from the owner, Paul — GitHub `skiiirt1983`)

- **Never ask Paul to build, compile or run scripts.** You build, test and package everything yourself.
  He flashes with the Espressif Flash Download Tool, esptool or esptool-js and reports what he sees.
- **Builds run in GitHub Actions** (`.github/workflows/firmware.yml`, `espressif/esp-idf-ci-action`,
  ESP-IDF v5.5.6, target esp32s3). Locally ESP-IDF is also usable (section 8). Watch CI with the GitHub MCP
  tools (`actions_list`, `actions_get`, `get_job_logs`) and fix failures yourself until green.
- **Every successful build uploads artifacts.** A `PROJECT_VER` (in `CMakeLists.txt`) without a suffix
  also publishes the GitHub Release `v<ver>`; a suffixed one (`0.3.5-rc1`) only produces artifacts.
- **When a build is ready, tell Paul:** release link, exact file name, flash address and how to flash.
  (`…-FRESH-INSTALL-USB.bin` at `0x0`, erases everything; `…-launcher-only.bin` at `0x90000`, keeps apps and
  settings. Flash Download Tool: ESP32-S3, SPI mode DIO, 80 MHz, DoNotChgBin ticked.)
- **The partition table is fixed.** Never change offsets or sizes in `partitions.csv`.
- **The launcher must stay under 1 MB** (factory partition, 0x100000). CI prints `idf.py size` and fails above.
  `basecamp_launcher.bin` is currently ~1,013,000 of 1,048,576 bytes: **only ~36 KB free**. Check after every change.
- **Say clearly what was tested on hardware and what was not.** Green CI / QEMU / mock tests are not
  hardware proof. Paul tests on the real board and reports back.
- **Paul prefers concise updates.** Lead with the result; keep caveats short and concrete.
- Work on `main`. Commit trailer: `Co-Authored-By: Claude <noreply@anthropic.com>` and the session link
  given by the harness. Never put a model name in code, comments or docs other than that trailer.
- Be honest about uncertainty: when a diagnosis is a hypothesis, say so and say what would confirm it.

---

## 2. What this is

A boot launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**. Every power-on shows a round touch menu
with the apps installed in three flash slots; tapping one boots it. The app's own restarts / crashes /
deep-sleep wake-ups stay in the app; **a power cycle or RST always returns to the menu**. The apps are not
built in: they are added over Wi-Fi from a web page (upload a `.bin`, or install from a GitHub release),
ordered visually, renamed, updated and removed there. Anyone can add their own app
(`docs/ADD_YOUR_OWN_APP.md`).

Hardware: ESP32-S3, 16 MB flash, 8 MB octal PSRAM, CO5300 466×466 QSPI AMOLED, CST9217 touch (I2C 0x5A),
AXP2101 PMU (I2C 0x34), TCA9554 expander (0x20), ES8311/ES7210 audio, PCF85063 RTC, QMI8658 IMU, microSD
slot (SDMMC pins 1/2/3; the launcher never touches it). Pins used by the launcher: I2C SCL 14 / SDA 15;
LCD CS 12, PCLK 38, D0–D3 = 4–7, RST 39; touch RST 40, INT 11. Schematic and the vendor examples:
`github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75`.

---

## 3. Architecture in one page

**Partition table (fixed)**

| Name | Offset | Size | Use |
|---|---|---|---|
| nvs | 0x9000 | 512 KB | shared by launcher and all apps |
| otadata | 0x89000 | 8 KB | unused (selection does **not** use it) |
| phy_init | 0x8B000 | 4 KB | |
| launcher (factory) | 0x90000 | 1 MB | this project's app |
| ota_0 / ota_1 / ota_2 | 0x190000 / 0x590000 / 0x990000 | 4 MB each | the three app slots |
| sounds (data, 0x83 LittleFS) | 0xD90000 | 2.4 MB | shared data partition (PrintSphere uses it) |

**Boot selection** (`bootloader_components/main/bootloader_start.c`, overrides the stock `main`):
- The launcher writes a word into **`RTC_CNTL_STORE0_REG`** (`common/include/basecamp_bootsel.h`):
  `[31:16]=0xBA5E`, `[15:8]=~slot`, `[7:0]=slot`, then `esp_restart()`.
- Bootloader: cold reset (power-on, EN/RST, brown-out, RTC WDT, glitch, eFuse CRC, and by default
  USB-Serial-JTAG resets — `CONFIG_BASECAMP_USB_RESET_IS_COLD`) → clear the word, boot the launcher.
  Other resets (SW restart, panic, WDT, deep-sleep wake) with a valid word → `esp_image_verify()` the slot
  and boot it (word kept). Missing/corrupt word, or slot invalid → launcher. **Never falls back to another
  OTA slot** (the stock loader would). otadata is never read.
- STORE0 was chosen because it is "Reserved" in the S3 ROM header and unused by IDF 5.5; RTC *memory* was
  rejected (apps may overwrite it).
- An app returns to the menu with `REG_WRITE(RTC_CNTL_STORE0_REG, 0); esp_restart();`.

**Launcher** (`main/`, ESP-IDF 5.5, LVGL 9.4 via `esp_lvgl_port`, CO5300 via `espressif/esp_lcd_co5300`,
touch via `waveshare/esp_lcd_touch_cst9217`, own AXP2101 driver):
- `main.c` — controller: single *busy* state (`ctl_begin/ctl_end`), worker task + queue for jobs
  (install/update with 4 retries on 5xx/timeouts, updates check, launch), Wi-Fi mode logic
  (`apply_wifi_mode`, `wifi_kick`), start-reason record, footer (battery/USB, `RESTARTED:` marker).
- `slots.c/h` — scans the three slots (valid? damaged? name/version/colour/source), overwrite guard
  (SHA-256 baseline in NVS → "⚠ changed"), legacy migration from 0.2.x, `slots_erase`.
- `meta.c/h` — per-slot metadata in NVS; `inst_source_t` (GitHub source).
- `swriter.c/h` — **the slot writer** used by uploads and downloads: checks the first 64 bytes
  (ESP image, chip id 9, app descriptor) *before* erasing, streams with `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)`,
  `esp_ota_end` verifies, failure blanks the slot (erases 64 KB), tracks SHA-256 + marker strings.
- `installer.c/h` — GitHub: `releases/latest`, asset pick by prefix/suffix/exclude, download via the **API
  asset URL**, checks size, `digest`, board marker, `project_name`.
- `webui.c` + `webui.html` — HTTP server and the single-page web UI (no external resources).
- `netmgr.c/h` — Wi-Fi: open AP `BaseCamp-Setup` + captive DNS, station connect on demand, scan, reasons.
- `ui.c/h`, `ui_port.h` — round LVGL screens (menu with 0–3 cards, empty state, progress rings, updates,
  Wi-Fi-needed, launching). Written to compile on a PC too (`UI_HOST`).
- `apps_logic.c/h` — pure helpers (order, names, JSON escape, repo validation, image-header check, KMP
  marker search). No ESP deps → host unit tests.
- `components/board/` — board bring-up (`board_init`, dark start, brightness, AXP2101 `axp2101_init`).

**Wi-Fi modes** (since 0.3.1; details in `main.c: apply_wifi_mode`)
- No home Wi-Fi saved: open setup network **`BaseCamp-Setup`** (192.168.4.1, catch-all DNS → captive portal).
- Home Wi-Fi saved: join it; the setup network stays **off**; the menu's hint line shows
  `HomeNet - 192.168.1.57` and the page is served there. Not joinable within ~40 s (two 20 s attempts) →
  setup network comes back and the hint says why (`BaseCamp-Setup - HomeNet: wrong password?`).
- Saving Wi-Fi on the page: reply first, join, wait for any running upload, 2.5 s, stop the AP.
  Forgetting Wi-Fi brings the AP back. A lost home link falls back to the AP (`footer_task`).
- All Wi-Fi bring-up (driver init, RF calibration, radio start) runs in `radio_boot_task`, started before
  the display init, so a Wi-Fi problem can never keep the menu from appearing (bounded 3 s wait).
- Everything is switched off when an app is launched.

**NVS namespace `basecamp`** (keys ≤ 15 chars): `name<i>`, `col<i>` (u32 RGB), `src<i>`
(`repo\tprefix\tsuffix\texclude\tproject\tmarker`), `tag<i>` (installed release tag), `sha<i>` (guard
baseline), `ver<i>` (version label), `ord` (3-byte permutation), `ssid`, `pass`, `rst0` (last start reason).
`i` = slot 0..2 (ota_0..ota_2). Display order is independent of slot number.

**Web API** (all `POST` need header `X-Basecamp: 1`; unknown path → 302 to 192.168.4.1 while the AP is up,
else 404): `GET /`, `GET /api/state`, `POST /api/upload?slot=&replace=1&name=&color=&version=` (raw body),
`POST /api/order {order:[3]}`, `/api/rename {slot,name?,color?,version?}`, `/api/delete {slot}`,
`/api/install {update:slot | repo,prefix,suffix,exclude,project,marker,name,color,slot?,replace?}`,
`/api/wifi {ssid,pass}`, `/api/wifi/forget`, `POST|GET /api/wifi/scan`. Uploads into an occupied slot
need `replace=1`.

**Kconfig** (`menuconfig → Basecamp launcher`): `BASECAMP_ENABLE_WIFI` (default y; off only for QEMU),
`BASECAMP_WIFI_TX_QDBM` (default 48 = 12 dBm), `BASECAMP_VERIFY_SLOTS`, default-apps table
`BASECAMP_SLOT{0,1,2}_{NAME,REPO,ASSET_PREFIX,ASSET_SUFFIX,ASSET_EXCLUDE,PROJECT,BOARD_MARKER}`.
`sdkconfig.defaults` carries the size trims (see Lessons).

---

## 4. Repo map

```
CMakeLists.txt            PROJECT_VER lives here (drives release version)
partitions.csv            FIXED
sdkconfig.defaults        size trims, PSRAM, main-task stack 8 KB, ...
bootloader_components/main/   custom 2nd-stage bootloader entry
common/include/basecamp_bootsel.h   selection-word contract (shared with apps)
main/                     launcher (see section 3)
components/board/         board bring-up
docs/ADD_YOUR_OWN_APP.md  guide for third-party apps (+ TamaPoke worked example)
test_apps/qemu_selftest   headless test of the REAL bootloader (QEMU)
test_apps/swriter_selftest  QEMU test of swriter/meta/slots (105 checks)
test_apps/dummy_app       on-board test app (Restart / Crash / Sleep buttons)
tools/ci_build.sh         what CI runs
tools/make_images.sh      builds FRESH-INSTALL / launcher-only / SHA256SUMS into dist/
tools/flash.sh            esptool wrapper (fresh | --launcher-only | --offline)
tools/host_tests/         gcc unit tests for apps_logic (145 checks with apps/ images)
tools/qemu_selftest.sh, tools/qemu_swriter_test.sh, tools/check_selftest_log.py
tools/webui_mock.py       mock device API (edit webui.html without hardware)
tools/webui_test.js       Playwright test of the page against the mock
tools/ui_preview/         PC render of every LVGL screen (round mask) -> PNG
tools/fetch_apps.sh, apps.sha256   pinned app bins for --offline flashing
.github/workflows/firmware.yml
```

---

## 5. Build, test, release

```bash
. ~/esp/esp-idf/export.sh                 # ESP-IDF v5.5.6
rm -f sdkconfig                           # AFTER ANY Kconfig/sdkconfig.defaults change (stale sdkconfig bites)
idf.py build                              # launcher + custom bootloader; check the size line
tools/make_images.sh                      # dist/Basecamp-<ver>-FRESH-INSTALL-USB.bin etc.
tools/host_tests/run.sh [apps]            # unit tests (apps/ = dir with the real app .bin files)
tools/qemu_selftest.sh                    # bootloader decisions in QEMU
tools/qemu_swriter_test.sh                # slot writer / meta / slots in QEMU
NODE_PATH=/opt/node-tools/node_modules node tools/webui_test.js    # page vs mock (starts the mock itself)
tools/ui_preview/run.sh                   # screenshots of all screens (needs a prior idf.py build for LVGL)
tools/ci_build.sh                         # the whole CI sequence
```

**Launcher in QEMU** (to check boot order / slot scan): build with `-DSDKCONFIG=<tmp>/sdkconfig
-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;qemu.defaults"` where `qemu.defaults` =
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`, `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`,
`CONFIG_BASECAMP_ENABLE_WIFI=n`. QEMU limits: **no radio** (RF calibration spins forever), **no display**
(panel init stalls), **QIO flash mis-emulated** (use DIO), no deep-sleep wake, no PSRAM.

**CI and releases** (`.github/workflows/firmware.yml`): on push to `main` (ignoring `**.md` and `docs/**`),
pull requests and manual runs: host tests → build → size → `make_images.sh` (checks the fresh image layout)
→ both QEMU tests → upload artifact → publish `v<PROJECT_VER>` if the version has no `-suffix`
(creates the release, or `gh release upload --clobber` if it exists). ~10–14 min. A new push cancels the
previous run of the same ref. Never start the workflow by hand on a branch you do not want released.

**Fresh container setup** (if `~/esp/esp-idf` is missing): `git clone --depth 1 --branch v5.5.6
--recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf &&
~/esp/esp-idf/install.sh esp32s3`; QEMU: `python ~/esp/esp-idf/tools/idf_tools.py install qemu-xtensa` plus
`apt-get install -y libslirp0 libsdl2-2.0-0`. Playwright is preinstalled under `/opt/node-tools`. Fetch the
pinned app bins with `tools/fetch_apps.sh` if a test wants `apps/`.

**Sandbox quirks**: the GitHub *API* for repos other than this one is blocked in the session (403 from the
proxy) — use `git ls-remote` / `git clone` for public repos; `add_repo` with push access to other repos is
denied. Commands that `sleep` for long must use `run_in_background`; very long heredocs mixed with
background subshells can trip the "rm safety check" — write big files with the Write tool and patch with small
Python scripts that `assert s.count(old)==1`. Builds can exceed the 10-minute command limit — background them.

---

## 6. Lessons learned (bugs found and how they were fixed — do not repeat)

- **Size is the constraint.** Wi-Fi + TLS + httpd + LVGL fit in 1 MB only with: `-Os`, warning-level logs,
  silent asserts, nano printf, client-only TLS 1.2 + common CA bundle + trimmed ciphers/curves, no WPA3 /
  enterprise / IPv6, LVGL reduced to label/button/arc with RGB565/A8/ARGB8888 only, no demos/extra fonts.
- **LVGL `LV_LABEL_LONG_DOT` only truncates when the label has a fixed height** → `one_line()` in `ui.c`.
- **`LV_STATE_DISABLED` stops LVGL delivering input** → grey "retry" cards use `LV_STATE_USER_1`.
- **Main task stack (3.5 KB) overflowed in the slot scan** (`esp_image_verify` + structs) → 8 KB.
- **GitHub `releases/download/...` URLs answered 504** after ~11 s (4/5 tries); the **API asset URL**
  (`api.github.com/repos/.../releases/assets/<id>` + `Accept: application/octet-stream`) answers in <1 s.
  Plus automatic retries for 5xx/timeouts.
- **PrintSphere's release has two same-named-project assets** (AMOLED 1.75 and LCD 2.8C): exclude `2.8c`
  and require the marker `esp32_s3_touch_amoled_1_75` in the image.
- **Validate before you erase**: wrong files (zip, bootloader/full image, other chip) must never destroy
  the app already in the slot (`swriter.c`, tested in QEMU).
- **`esp_partition_get_sha256()` for apps == the image's appended digest** (`esp_image_verify` metadata) —
  so the overwrite-guard baseline is consistent between install and scan.
- Percent labels in progress rings: make them children of the ring and `lv_obj_center` them.
- Rebuilding/reloading the whole menu to change one label **flashes the panel** → `ui_set_hint()` updates
  the label in place.
- **The CO5300 holds random pixels until the first frame**; switching it on at full brightness shows a white
  flash at power-up (also documented in LyricFlash). → `board_set_dark_start(true)` and raise brightness
  only after the first frame (0.3.4).
- Wi-Fi: first `esp_wifi_connect()` right after `esp_wifi_start()` can fail → 300 ms settle, 12 retries per
  attempt, two attempts; the disconnect reason is kept and shown (`net_fail_text`).
- Test-writing traps: QEMU has ~300 KB RAM (stream, don't buffer a whole image); marker strings written as
  literals appear in the test's own image (build them at run time); flash writes can only clear bits (pick a
  byte that has a bit to clear when simulating corruption).
- Local `sdkconfig` goes stale when Kconfig changes → `rm sdkconfig` (CI is always clean).
- A flash write (NVS commit etc.) disables the cache briefly; keep display-critical work out of that window.

---

## 7. Third-party apps (what Basecamp installs / has been tested with)

| App | Source | Notes |
|---|---|---|
| PrintSphere | `cptkirki/PrintSphere` v1.6.2, asset `printsphere_ota-v1.6.2.bin` (sha256 6be314d4…) | Uses `sounds` LittleFS. Has an LCD 2.8C twin asset (excluded). Own OTA updater would overwrite the next slot. |
| Capsule Radar | `socquique/capsule-radar` v1.6.0, `CapsuleRadar-ota.bin` | Arduino build: descriptor says `arduino-lib-builder` + hash → version shown only from release tag / typed label. |
| LyricFlash | `skiiirt1983/LyricFlash` (Paul's) v1.0.2, `lyricflash-ota.bin` | Needs Wi-Fi + AudD token to record. PR #61 (draft) adds a persistent setup hint (1.0.1 → see that repo). |
| TamaPoke | `socquique/TamaPoke`, app-only image `web/firmware/app.bin` v1.21 (sha256 77119cc1…) | Arduino, sprites on microSD (its own installer), works unmodified: uses only shared NVS (own namespace), light sleep; verified against upload rules and slot scan; **not yet run on the board**. A copy was handed to Paul as `TamaPoke-v1.21-for-BaseCamp.bin`. |

The **dummy apps** (`test_apps/dummy_app`) were used for the first on-board proof of slot selection.

---

## 8. History (versions and why)

| Version | What / why |
|---|---|
| 0.1.0 | Scaffold: fixed table, custom bootloader (STORE0), launcher with three fixed apps, dummy apps, QEMU bootloader self-test, flash scripts. Paul flashed stage 1 (dummies) and stage 2 (real apps): both worked on the board. |
| 0.2.0 | CI + release images (FRESH-INSTALL / launcher-only). First-boot Wi-Fi portal and GitHub installer, progress rings, Updates screen; size trimmed under 1 MB. |
| 0.2.1 | PrintSphere installed its LCD 2.8C twin (didn't run): asset exclude + board marker; % labels centred. |
| 0.2.2 | GitHub 504s: API asset URL + retries; grey cards tappable again. |
| 0.3.0 | **Dynamic app list**, open `BaseCamp-Setup` AP + web page (upload/order/rename/remove/GitHub/Wi-Fi), slot writer, metadata, docs, host/QEMU/browser/render tests. Paul: "its great" (on hardware). |
| 0.3.1 | Setup network off once a home Wi-Fi is saved. Paul: AP was still running. |
| 0.3.2 | Join more reliably (12 retries, 2 attempts) and show the failure reason. |
| 0.3.3 | Hint line updated in place (no menu rebuild), Wi-Fi 1 s after the menu, last-start reason on the page, optional version label; TamaPoke docs. Paul: did not fix the flash; **it got more pronounced and followed the Wi-Fi text changing**. |
| 0.3.4 | Panel starts dark (no power-up white flash), Wi-Fi bring-up in its own task before the first frame, TX power capped at 12 dBm, `RESTARTED: <reason>` marker in the footer after a fault restart, `BASECAMP_ENABLE_WIFI` switch. **Released; untested on hardware** (waiting for Paul's report). |

Other work done for Paul: a LyricFlash 1.0.1 change (persistent Wi-Fi/AudD setup hint) prepared as draft PR
#61 in `skiiirt1983/LyricFlash` — not merged by us; merging there publishes a release.

---

## 9. Open issues and next steps

1. **Screen flash / "quick reset" after power-off (reported by Paul).** Not resolved. Hypotheses, in order:
   (a) panel random-RAM white flash at power-up (fixed in 0.3.4, unverified); (b) Wi-Fi radio start/connect
   current surge dipping the shared supply → display flash or brown-out reset (mitigated: radio before the
   first frame, TX 12 dBm; unverified); (c) a real chip reset (look at the page's bottom line
   `Last start: …` and the menu footer `RESTARTED: …`). **Ask Paul what those two say if it persists.**
   If it is still a pure display effect, consider LyricFlash's direct esp_lcd path (24-row strips into two
   internal DMA buffers, ISR completion, TE sync — `components/esp32_s3_touch_amoled_1_75` in
   `skiiirt1983/LyricFlash`) instead of `esp_lvgl_port`.
2. **Hand-over from the setup network to the home network (0.3.1+) is untested on hardware.**
3. TamaPoke has not been run on the board under Basecamp.
4. The repo's GitHub default branch may still be the old `ccr-6a25cc3b-3hr42f`; Paul was asked to switch it
   to `main` (we cannot change it).
5. Size headroom is ~36 KB; any new feature needs a size plan first.
6. **Browser-side `.bin` checker / patcher (asked by Paul, not built yet — he was told what is possible).**
   Checker: feasible, fully client-side (parse header/segments/app descriptor, validate checksum + SHA-256
   with SubtleCrypto, chip id, size, app-only vs full image, plus string heuristics: CO5300/CST9217, partition
   labels like spiffs/storage/ffat that Basecamp lacks, SD/OTA hints — heuristics can false-alarm, e.g.
   PrintSphere contains an unused "storage" string). Patcher: **proven** on TamaPoke that changing
   `esp_app_desc_t` name/version and recomputing the checksum byte (XOR 0xEF over segment data, stored at the
   end of the 16-byte-aligned block) and the appended SHA-256 (over header..checksum) gives an image that
   `esptool image_info` reports valid. Not possible/safe: changing logic (in-app OTA updater, code that calls
   `nvs_flash_erase()` or formats a filesystem). NVS itself needs no patching: apps look partitions up by
   name in the table on the device, so any app using `nvs` already shares it. Options if built: standalone
   page in `docs/` (no flash cost) and/or a small pre-check in `webui.html` (~3-4 KB of the 36 KB free).
7. Ideas not done: mDNS (`basecamp.local`) — costs flash; page password option; more than 3 apps would need
   a different partition table (forbidden) — e.g. compressed or relocatable apps would be a separate project.

---

## 10. Quick start for the next session

1. Read this file and `README.md`. Check `git log`, the latest release and CI status.
2. `. ~/esp/esp-idf/export.sh` (install if missing, section 5). Build and run the host tests to confirm the
   toolchain works.
3. Make the change, `rm -f sdkconfig` if Kconfig changed, build, check the size line, run the relevant tests.
4. Update this file (status, history, lessons, open issues) and README/docs **in the same commit**.
5. Push to `main`, watch CI to green, hand Paul the link, file name, address and flash steps — and what was
   and was not tested on hardware.
