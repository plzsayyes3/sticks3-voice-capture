# StickS3 Voice Capture

An offline-first voice memo recorder for M5Stack StickS3.

The intended device flow is record → internal Flash → explicit later Sync. The
StickS3 does not transcribe audio. A Mac or server will transcribe uploaded clips
and submit an AI-labelled transcript to `plzsayyes3/mynotebook`.

## Implementation status

The initial commit imports the tested StickS3 firmware from VoiceStick as an
audio and board baseline. **It is not yet a local recorder.** The imported code
currently sends Opus packets over BLE and uses a 1.984 MiB storage partition.
Do not rely on it to retain recordings while offline.

The first functional milestone is one-button recording to independent Ogg/Opus
files in internal Flash. Success requires:

- Recording without BLE, Wi-Fi, phone, or Mac present.
- At least 20 minutes of accumulated pending clips.
- A failed or interrupted recording cannot overwrite earlier completed clips.
- Packet or write backlog produces an explicit error, never silent audio loss.
- Boot leaves any interrupted clip for recovery or inspection.

Sync, transcription, and notebook ingestion follow only after the storage
milestone is verified on the device.

## Sources

The imported firmware comes from [`78/voicestick`](https://github.com/78/voicestick)
at commit `e865d68c1d96411571cbe1501a301ebe3c98f3b3`. Its original MIT
license and copyright notice are preserved in [LICENSE](LICENSE).

The deferred Sync queue and server contract are being evaluated against
[`guzus/open-plaud`](https://github.com/guzus/open-plaud). No code from that
repository is included yet.
