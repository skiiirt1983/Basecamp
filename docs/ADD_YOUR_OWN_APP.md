# Adding your own app to BaseCamp

BaseCamp is a launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**. It keeps up to **three apps** in
flash and lets you pick one on the round screen. Any app that follows the rules below can be added,
whether it is yours or an open-source project that ships a prebuilt `.bin`.

> **Limit: 3 apps, 4 MB each.** The flash layout is fixed (see [Flash layout](#flash-layout)). Fewer
> than three is fine: the menu shows one card per installed app.

## 1. Check that your app qualifies

| Requirement | Why |
|---|---|
| **App-only image** (the `.bin` your build produces for the application itself), not a merged/"full flash" image | The launcher, bootloader and partition table are already on the device. A merged image starts with a bootloader and is rejected. |
| Built for **ESP32-S3** (chip id 9) | Rejected otherwise. |
| **At most 4 MB** (4,194,304 bytes) | Size of one slot. |
| Has the normal **app descriptor** (every ESP-IDF and Arduino-ESP32 build does) | Gives the app a name and version; it is also how BaseCamp tells an app from a bootloader. |
| Runs from a **16 MB flash** board with **8 MB octal PSRAM** | That is this board. If you use PSRAM, enable octal mode (`CONFIG_SPIRAM_MODE_OCT`, 80 MHz). |
| Drives this board's hardware itself: CO5300 AMOLED over QSPI, CST9217 touch, AXP2101 power chip | BaseCamp hands over a restarted chip, not a ready display. |
| **Does not update itself into the "next OTA slot"** | See [In-app updaters](#in-app-updaters-are-dangerous). |

The page checks the first four automatically and says what is wrong if a file is rejected.

## 2. Add it

### Option A: from your phone or laptop (no tools)

1. Power the device. Under the title the menu shows how to reach the page: with no home Wi-Fi saved yet
   it is the Wi-Fi name **BaseCamp-Setup**; once a home Wi-Fi has been saved it is the device's address on
   your network (see step 5).
2. On your phone, join **BaseCamp-Setup**. It is an **open network with no password**. The setup page
   opens by itself; if it doesn't, open **http://192.168.4.1** in a browser.
   - If the page opened in a small "sign in to network" window and the file chooser doesn't work,
     open `http://192.168.4.1` in Safari/Chrome instead.
3. Under **Add an app → Upload .bin**, pick a slot, choose your `.bin`, optionally set the **name** and
   **colour** for its card, and press **Upload**. The device shows a progress ring while it receives
   the file, then the new card appears.
4. Use the **▲ ▼** arrows on the same page to put the cards in the order you want. The round preview
   on the page shows what the device will draw. Use **✎** to rename or recolour, **✕** to remove.

A new app is added at the end of the list. If all three slots are full, choose which one to replace
(you'll be asked to confirm).
5. **Optional, recommended: save your home Wi-Fi** (card *Wi-Fi*, at the bottom of the page). The device then
   joins your network and **switches the BaseCamp-Setup network off**. Put your phone back on your normal
   Wi-Fi and open the address now shown under the title on the device (for example `192.168.1.57`). From
   then on the page is always there, and GitHub downloads and updates work. If the saved network can't be
   found at start-up, BaseCamp-Setup comes back so you can correct it.

### Option B: straight from a GitHub release

For apps that publish their `.bin` on GitHub releases, the page can download and later update them:

1. In the page's **Wi-Fi for GitHub downloads** card, pick your home Wi-Fi and enter its password.
   (2.4 GHz, WPA/WPA2 or open.)
2. **Add an app → From GitHub**: enter `owner/name`. Under *Advanced* you can tell it which release file
   to take:

   | Field | Meaning |
   |---|---|
   | Release file starts with / ends with | Picks the file in the release, e.g. starts with `myapp-ota`, ends with `.bin`. |
   | Skip files containing | Comma-separated. Use it when a release has builds for other boards, e.g. `2.8c`. |
   | App name inside the image must contain | Optional safety check against the app's `project_name`. |
   | Image must contain this text | Optional board check, e.g. `CO5300`. |

3. Press **Install from GitHub**. Progress shows on the device and on the page.
4. Later, the device's **Updates** button (or the **⟳** button on the page) installs newer releases.
   An update replaces the app but keeps its settings.

Details: it uses the repository's **latest release** (GitHub's "latest" skips drafts and pre-releases),
needs the file to be **4 MB or smaller**, and checks the SHA-256 digest GitHub publishes for the file.
If a release has several files that match, the first one listed is used, so tighten the *starts with* /
*skip* fields until only one matches.

### Option C: with esptool

Write the app-only image to a slot's address:

| Slot | Address |
|---|---|
| 1 (`ota_0`) | `0x190000` |
| 2 (`ota_1`) | `0x590000` |
| 3 (`ota_2`) | `0x990000` |

```bash
esptool.py --chip esp32s3 -b 921600 write_flash 0x590000 my_app.bin
```

After the next reboot the launcher finds the app and names it from its `project_name`; rename it on the
web page if you like. Writing a slot this way keeps everything else (other apps and all settings).

## 3. Make your app behave well

### Getting back to the menu
A **power cycle (or the RST button) always returns to the menu.** Your app's own restarts, crashes and
deep-sleep wake-ups stay in your app: that is the point of the launcher.

To return to the menu from inside your app (for example from a "Home" button), clear the selection
word and restart:

```c
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

REG_WRITE(RTC_CNTL_STORE0_REG, 0);   // forget the selection
esp_restart();                       // the bootloader now starts the launcher
```

`RTC_CNTL_STORE0_REG` is the one register BaseCamp uses for this. ESP-IDF itself leaves it alone, so
please don't use it for anything else. (`common/include/basecamp_bootsel.h` documents the format.)

### Display and power
BaseCamp turns the display off before it restarts into your app, and leaves the AXP2101 rails it needs
on (DCDC1 for the chip, ALDO1/ALDO2 at 3.3 V for the peripherals). Do your normal initialisation
(AXP2101, display reset, touch reset) as if you had just been flashed.

### In-app updaters are dangerous
`esp_ota_get_next_update_partition()` returns the **next** slot, so an app updating itself overwrites a
**neighbouring app**, and then BaseCamp (which ignores `otadata`) boots your old version again:

| App in | Its own updater overwrites |
|---|---|
| slot 1 | slot 2 |
| slot 2 | slot 3 |
| slot 3 | slot 1 |

Please disable such an updater, or have it point users to BaseCamp's **Updates** button (publish your
release on GitHub and add the repository as in Option B). BaseCamp flags a slot whose image changed
behind its back with an amber **⚠ changed** on the card.

### Shared storage
- **`nvs`** (512 KB) is shared by the launcher and every app. Use your own namespace, never call
  `nvs_flash_erase()`, and don't touch the namespace `basecamp`. Wi-Fi credentials saved by the IDF's
  Wi-Fi driver (`nvs.net80211`) are shared too, so an app may find another app's network there.
- **`sounds`** is a 2.4 MB data partition (type `0x83`, label `sounds`, LittleFS in PrintSphere) at
  `0xD90000`, available to any app. Start from a blank filesystem: another app may have used it.
- Use the same partition table when you build your app, so partition lookups behave on your desk
  exactly as on the device: [`partitions.csv`](../partitions.csv).

### Name and version on the card
The card shows the name you gave on the web page. If you didn't give one, it is made from the app's
`project_name` (`my_cool_app` → "My Cool App"). The version line is the app's `version` (set
`PROJECT_VER` in your top-level `CMakeLists.txt` for ESP-IDF projects).

Arduino-ESP32 builds all report the project name `arduino-lib-builder`, so name them on the web page.

## 4. Building the right file

**ESP-IDF:** `idf.py build` produces `build/<project>.bin`. That is the app-only image to upload.
(`idf.py merge-bin` and the `*-full.bin` / `bootloader.bin` / `partition-table.bin` files are not.)

**Arduino IDE / PlatformIO:** use the **app-only** binary, e.g. *Sketch → Export Compiled Binary* gives
`<sketch>.ino.bin` (upload this one) next to `.bootloader.bin`, `.partitions.bin` and `.merged.bin`
(don't). Select an ESP32-S3 board with **16 MB flash** and **OPI PSRAM**.

**GitHub Actions:** publish the app-only `.bin` as a release file (for example `myapp-ota.bin`) so the
launcher can install and update it.

## 5. Flash layout

Fixed; it is what the launcher and its bootloader are built for.

| Name | Offset | Size | Used for |
|---|---|---|---|
| nvs | 0x9000 | 512 KB | settings of the launcher and all apps |
| otadata | 0x89000 | 8 KB | unused (BaseCamp doesn't select apps with it) |
| phy_init | 0x8B000 | 4 KB | Wi-Fi calibration |
| launcher (factory) | 0x90000 | 1 MB | the BaseCamp launcher |
| ota_0 / ota_1 / ota_2 | 0x190000 / 0x590000 / 0x990000 | 4 MB each | **your three apps** |
| sounds | 0xD90000 | 2.4 MB | shared data partition |

How BaseCamp starts an app: tapping a card writes the slot number into `RTC_CNTL_STORE0_REG` and
restarts. A custom second-stage bootloader reads it, checks the slot's image, and boots it. After a cold
reset (power-on, RST, brown-out) the register is cleared and the launcher starts.

## 6. If something goes wrong

| What you see | Meaning / fix |
|---|---|
| *"not an ESP32 .bin (zip, elf or an error page?)"* | The file isn't an ESP image. Upload the `.bin`, not a `.zip`/`.elf`. A downloaded GitHub error page also looks like this. |
| *"no app descriptor: use the app-only OTA .bin…"* | You picked a merged/full-flash or bootloader image. Use the app-only `.bin`. |
| *"built for another chip (id N), needs ESP32-S3"* | Rebuild for `esp32s3`. |
| *"image failed verification"* | Incomplete or corrupt upload. Try again; check the file size. |
| *"image is larger than the 4 MB slot"* | Shrink the app (optimise for size, drop features). |
| Warning *"No CO5300 display driver found"* | The app may not be built for this board's screen. It installed anyway. |
| Tapping a card returns to the menu at once | The bootloader found the image invalid. Reinstall the app. |
| Card is grey with a **⟳ … retry** | A download failed or the image is damaged. Tap it to reinstall (GitHub apps), or use the page. |
| Black screen after tapping | The app didn't initialise the display/power. Power-cycle to return to the menu. |
| Page doesn't open | No home Wi-Fi saved: join **BaseCamp-Setup** (no password), then browse to `http://192.168.4.1`. Home Wi-Fi saved: be on that same network and open the address shown under the title on the device. If the device says `… not found - join BaseCamp-Setup`, it could not find your saved network: join BaseCamp-Setup and correct it. Some phones silently stay on mobile data: turn it off. |

## 7. A word on security

The page has no password. Until a home Wi-Fi is saved, the setup network is open by design, so that
nothing needs to be typed to get started, and **anyone within range can use the page while the menu is
showing**. After you save your home Wi-Fi the setup network stays off and the page lives on your home
network, where **anyone on that network can use it**. Either way it switches off the moment an app starts
and returns only when the menu does. Don't leave the device sitting on the menu in a place you don't trust.
