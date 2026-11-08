# Working rules for this repo (SLOPEFORM Basecamp launcher)

Read README.md first. Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash, 8 MB octal PSRAM).

- **Never ask Paul to build, compile or run scripts.** You build, test and package everything yourself.
- **Builds run in GitHub Actions** (`.github/workflows/firmware.yml`), like skiiirt1983/LyricFlash:
  `espressif/esp-idf-ci-action`, ESP-IDF v5.5.x, target esp32s3. If ESP-IDF isn't available locally,
  push and use CI. Watch the run (`gh run watch`, or the GitHub MCP actions tools) and fix failures
  yourself until it's green.
- **Every successful build uploads artifacts.** A version without a suffix (`PROJECT_VER` in
  `CMakeLists.txt`, e.g. `0.2.0`) also publishes a GitHub Release `v<ver>`; a suffixed version
  (`0.2.1-rc1`) only produces artifacts.
- **When a build is ready, tell Paul:** the release/artifact link, the exact file name, the flash
  address (0x0 for the FRESH-INSTALL image, 0x90000 for launcher-only) and the esptool /
  web-flasher instructions.
- **The partition table is fixed.** Never change offsets or sizes in `partitions.csv`.
- **The launcher must stay under 1 MB** (factory partition). CI prints `idf.py size`.
- **Be clear about what was tested on hardware and what wasn't.** A green CI (or QEMU) is not
  hardware proof.
- Paul prefers concise updates.
- Work on `main`.
