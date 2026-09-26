# StickS3 Voice Capture

*[日本語版 README](README.ja.md)*

An offline-first, PLAUD-style voice memo recorder built on the M5Stack StickS3.
Wear it, press the button, talk — no phone, no network required to capture.
Later, sync over Wi-Fi and get a verbatim transcript in your notes automatically.

```text
StickS3                          Mac (local-receiver)
  top button → record             POST /v1/recordings
  Opus/Ogg → internal Flash       durable write + SHA-256 idempotency
                                        ↓
  side button (short press)       whisper.cpp + VAD → verbatim transcript
    → Wi-Fi Sync ───────────────→      ↓
      LAN (mDNS hostname)        mynotebook/00_inbox (no summarization)
      → Tailscale Funnel (HTTPS) if off-LAN
```

## Status

The local-recording → Wi-Fi Sync → local transcription → notebook pipeline
works end-to-end on real hardware. All 11 Device Recording Acceptance Gates
(build, OTA size, partition layout, short/multi-recording, `.ogg` finalize,
real playback, reboot persistence, 20-minute continuous recording, forced-reset
resilience, `.part` recovery) are passing.

Known limitations:
- OTA slots are ~2.69 MiB each (0x2b0000); the internal `storage` partition
  is ~2.56 MB (0x290000, ~17 min of 20 kbps Opus). Recordings prefer the TF
  HAT SD card.
- The first time a build with this partition layout is flashed, the old
  `storage` contents make the new partition fail to mount. Sync all internal
  recordings first, then over USB run `idf.py -p PORT flash` and
  `python -m esptool --chip esp32s3 -p PORT erase_region 0x570000 0x290000`
  (the blank partition is formatted as FAT on first boot).
- `light_sleep_enable` is forced `false` for USB-Serial-JTAG debug stability;
  this needs to be reverted for accurate battery-runtime numbers.
- Off-LAN sync depends on a Tailscale Funnel URL you set up yourself (see
  below) — there's no bundled relay service.

## Repository layout

- `firmware/` — ESP-IDF v5.5.1 firmware (ESP32-S3 / StickS3)
  - `components/audio_pipeline` — I2S capture → Opus encode → write queue
  - `components/recording_store` — FAT-on-flash storage, `.part`→`.ogg`
    finalize, usage/capacity queries
  - `components/wifi_sync` — side-button Wi-Fi Sync client (scan known
    networks, upload pending recordings, LAN-first with an off-LAN fallback)
  - `components/ui_status` — LVGL status screen (icons, battery, hints)
  - `components/stick_s3_board`, `components/voice_ble` — StickS3 board
    bring-up and BLE, inherited from the VoiceStick baseline
- `local-receiver/` — Flask server that receives uploads, transcribes with
  whisper.cpp, and pushes to `mynotebook/00_inbox` (see its own README)
- `scripts/` — flash-extraction (`extract-recordings.sh`) and UI icon
  conversion (`convert-icon.py`) tooling

## Building the firmware

```bash
cd firmware
cp components/wifi_sync/include/secrets.h.example components/wifi_sync/include/secrets.h
# edit secrets.h: Wi-Fi networks, STICKS3_DEVICE_TOKEN, STICKS3_RECEIVER_URL(_FALLBACK)
idf.py build
idf.py -p <port> flash
```

`secrets.h` is gitignored — it holds real Wi-Fi credentials and the device's
shared auth token and is never committed. CI builds against the placeholder
`secrets.h.example` instead, so it verifies compilation and the OTA size
budget without real credentials.

### Off-LAN sync (Tailscale Funnel)

`STICKS3_RECEIVER_URL` (LAN, mDNS hostname) is tried first; if that fails,
`STICKS3_RECEIVER_URL_FALLBACK` is tried. To set the fallback up:

```bash
tailscale funnel --bg --https=10000 8090
```

This exposes the Mac's local-receiver (port 8090) over public HTTPS via your
Tailscale account. The existing Bearer-token auth is what actually protects
the endpoint, since the URL itself is reachable by anyone who has it.

## Running the receiver

See [local-receiver/README.md](local-receiver/README.md) for setup,
requirements (ffmpeg, whisper.cpp + a VAD model), and the durability/
idempotency contract.

## Sources

The imported firmware comes from [`78/voicestick`](https://github.com/78/voicestick)
at commit `e865d68c1d96411571cbe1501a301ebe3c98f3b3`. Its original MIT
license and copyright notice are preserved in [LICENSE](LICENSE).

The Sync queue and server contract were evaluated against
[`guzus/open-plaud`](https://github.com/guzus/open-plaud). Its Ogg/Opus writer
was used as a design reference; this repository implements its local packet
writer separately around the existing VoiceStick encoder.
