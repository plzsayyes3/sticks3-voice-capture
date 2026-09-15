# StickS3 Voice Capture

An offline-first voice memo recorder for M5Stack StickS3.

The intended device flow is record → internal Flash → explicit later Sync. The
StickS3 does not transcribe audio. A Mac or server will transcribe uploaded clips
and submit an AI-labelled transcript to `plzsayyes3/mynotebook`.

## Implementation status

The firmware baseline comes from VoiceStick, but the current local-recording MVP
no longer uses BLE as the audio sink. VoiceStick still supplies the proven
StickS3 board, ES8311/I2S, Opus, button, display, power, and OTA foundation.

On `feature/local-recording-mvp` the first storage implementation now exists:

- 16 kHz mono audio is encoded by the existing VoiceStick Opus encoder at
  20 kbps using 60 ms frames.
- Encoded Opus packets go to a dedicated Flash-writer queue rather than the BLE
  audio queue.
- Queue overflow is a recording failure. No oldest-packet drop path remains.
- A dedicated `recording_store` component mounts the internal FAT partition and
  writes independent Ogg/Opus recordings.
- Recording starts as `*.part`; only a successful close, flush, and sync renames
  it to `*.ogg`.
- An interrupted or failed recording is retained as `*.part` for inspection
  instead of overwriting or deleting earlier completed clips.
- FAT auto-formatting is enabled only when the raw recording partition still
  appears blank. A later mount failure does not trigger a destructive retry.
- Ogg pages group up to 10 Opus packets and the file is periodically flushed and
  `fsync`ed to limit the amount of audio exposed to sudden power loss.
- The imported app shell is temporarily shimmed so BLE readiness, BLE button
  notifications, and BLE disconnects cannot veto or terminate local capture.

**This code has not yet passed the StickS3 build and device gates below.** Do not
consider local recording reliable until those tests pass.

## First functional milestone

One-button recording to independent Ogg/Opus files in internal Flash is accepted
only when all of the following are verified on the physical StickS3:

- Recording starts and stops with no BLE, Wi-Fi, phone, or Mac present.
- A completed `.ogg` decodes and plays from beginning to end.
- Multiple recordings create independent files and do not overwrite each other.
- At least 20 minutes of accumulated pending clips fit and remain readable.
- A failed or interrupted recording cannot damage earlier completed clips.
- Packet or write backlog reports failure; audio is never silently dropped.
- Sudden reset/power interruption leaves a `.part` file and previously completed
  `.ogg` files intact.
- Reboot reports retained `.part` files rather than auto-deleting them.

Sync, transcription, and notebook ingestion follow only after this storage
milestone is verified on the device.

## Device test order

1. Build the firmware and confirm the final application image still fits each
   2 MiB OTA slot.
2. For the **first development install of this new partition layout**, perform a
   clean USB install (`idf.py erase-flash` followed by `idf.py flash`). The new
   FAT partition overlaps bytes that previously belonged to VoiceStick's larger
   `ota_1` and SPIFFS layout, so simply writing the new partition table can leave
   nonblank old data in the new recording region. The recorder intentionally
   refuses to auto-format a nonblank partition because that could destroy real
   recordings on later boots.
3. Boot with BLE unavailable and verify the recording FAT partition mounts.
4. Record 10–30 seconds with the front button, stop, and confirm an `.ogg` file
   is finalized.
5. Copy/read the file and verify duration and intelligible audio with a standard
   Ogg/Opus decoder.
6. Create several short recordings and verify unique filenames and preservation
   across reboot.
7. Record for 20 minutes and measure actual file size and remaining FAT space.
8. During another recording, force reset/power loss after several seconds;
   verify a `.part` remains and all earlier `.ogg` files are unchanged.
9. Only after the above passes, begin side-button Wi-Fi Sync work.

A production migration path that preserves recordings across partition-layout
changes is a separate problem. Do not use `erase-flash` once the device contains
recordings that need to be kept.

## Flash layout and OTA

The 8 MiB Flash is split into two 2 MiB OTA app slots and a `0x3f0000` byte
(3.9375 MiB) FAT data partition, plus NVS, OTA metadata, and PHY data. The
VoiceStick v0.3.2 OTA image used as the baseline was 1,418,048 bytes, leaving
679,104 bytes of headroom in each app slot before local recorder and later Wi-Fi
Sync code are added. Every firmware image must be checked against the 2 MiB slot
limit before release.

At a fixed 20 kbps, 20 minutes of Opus audio is 3,000,000 bytes before Ogg,
FAT, and wear-levelling overhead. The current writer groups up to 10 × 60 ms
Opus packets per Ogg page to keep container overhead small. The 20-minute target
remains a physical-device capacity test, not a claim based only on arithmetic.

Changing the partition table requires an initial USB flash of the new table.
VoiceStick's existing BLE app OTA can then continue between `ota_0` and `ota_1`;
that app-only updater does not migrate a previously installed partition table.
Back up any existing device recordings before installing a different table.

## Sources

The imported firmware comes from [`78/voicestick`](https://github.com/78/voicestick)
at commit `e865d68c1d96411571cbe1501a301ebe3c98f3b3`. Its original MIT
license and copyright notice are preserved in [LICENSE](LICENSE).

The deferred Sync queue and server contract are being evaluated against
[`guzus/open-plaud`](https://github.com/guzus/open-plaud). Its Ogg/Opus writer
was used as a design reference; this repository implements its local packet
writer separately around the existing VoiceStick encoder.
