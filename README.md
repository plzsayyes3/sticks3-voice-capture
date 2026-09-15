# StickS3 Voice Capture

An offline-first voice memo recorder for M5Stack StickS3.

The intended device flow is record → internal Flash → explicit later Sync. The
StickS3 does not transcribe audio. A Mac or server will transcribe uploaded clips
and submit an AI-labelled transcript to `plzsayyes3/mynotebook`.

## Implementation status

The initial commit imports the tested StickS3 firmware from VoiceStick as an
audio and board baseline. **It is not yet a local recorder.** The imported code
currently sends Opus packets over BLE. The partition table now keeps OTA and
reserves internal Flash for recording, but no filesystem is mounted yet. Do not
rely on this firmware to retain recordings while offline.

The first functional milestone is one-button recording to independent Ogg/Opus
files in internal Flash. Success requires:

- Recording without BLE, Wi-Fi, phone, or Mac present.
- At least 20 minutes of accumulated pending clips.
- A failed or interrupted recording cannot overwrite earlier completed clips.
- Packet or write backlog produces an explicit error, never silent audio loss.
- Boot leaves any interrupted clip for recovery or inspection.

Sync, transcription, and notebook ingestion follow only after the storage
milestone is verified on the device.

## Flash layout and OTA

The 8 MiB Flash is split into two 2 MiB OTA app slots and a `0x3f0000` byte
(3.9375 MiB) FAT data partition, plus NVS, OTA metadata, and PHY data. The
VoiceStick v0.3.2 OTA image is 1,418,048 bytes, leaving 679,104 bytes of headroom
in each app slot before adding local storage and Wi-Fi Sync code. Each future
firmware image must be checked against the 2 MiB slot limit before release.

At a fixed 20 kbps, 20 minutes of Opus audio is 3,000,000 bytes before Ogg and
filesystem overhead. Grouping multiple packets per Ogg page is preferred; even
one page for every 60 ms packet adds roughly 560,000 bytes of page overhead.
The 20-minute guarantee remains a device test gate, not a claim based only on
partition size.

Changing the partition table requires an initial USB flash of the new table.
VoiceStick's existing BLE app OTA then continues between `ota_0` and `ota_1`;
that app-only updater does not migrate a previously installed partition table.
Back up any existing device recordings before installing a different table.

## Sources

The imported firmware comes from [`78/voicestick`](https://github.com/78/voicestick)
at commit `e865d68c1d96411571cbe1501a301ebe3c98f3b3`. Its original MIT
license and copyright notice are preserved in [LICENSE](LICENSE).

The deferred Sync queue and server contract are being evaluated against
[`guzus/open-plaud`](https://github.com/guzus/open-plaud). No code from that
repository is included yet.
