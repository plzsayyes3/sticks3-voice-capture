# StickS3 Local Receiver

Receives Ogg/Opus recordings uploaded by the StickS3 over Wi-Fi (the side-button
Sync flow), transcribes them locally with [whisper.cpp](https://github.com/ggml-org/whisper.cpp),
and pushes the verbatim transcript straight into `mynotebook/00_inbox` — no
summarization step.

This replaces the Cloud Run / Cloud Storage / Pub/Sub / Gemini design from
`feature/cloud-receiver-api` (PR #2), which is on hold. The HTTP API shape
(`POST /v1/recordings`, device Bearer token, `X-Recording-ID` idempotency) is
carried over unchanged so the StickS3 firmware side does not need a different
protocol.

## Why local

- No Gemini API cost or external dependency for transcription.
- No summarization: the transcript is the note. Summarizing risks dropping or
  distorting content from a personal voice memo.
- Runs on the same Mac already used for Flash extraction and Whisper testing.

## Requirements

- Python 3.11+ (uses `zoneinfo`)
- `ffmpeg` on PATH
- `whisper-cli` (whisper.cpp) on PATH — `brew install whisper-cpp`
- A whisper.cpp GGML model, e.g. `ggml-large-v3-turbo.bin`
- A whisper.cpp VAD model, e.g. `ggml-silero-v5.1.2.bin` — **required**.
  Without VAD, whisper.cpp hallucinates repeated stock phrases (e.g. "ご視聴
  ありがとうございました") during silence, which is common in an
  always-recording capture device. See project History for the incident that
  led to this requirement.

## Setup

```bash
cd local-receiver
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
cp transcription-dictionary.example.txt transcription-dictionary.txt
# edit .env: set DEVICE_TOKEN, optionally GITHUB_TOKEN
# edit transcription-dictionary.txt: add one preferred term per line
```

## Run

```bash
source .venv/bin/activate
set -a; source .env; set +a
python app.py
```

Listens on `0.0.0.0:8090` by default (see `PORT` in `.env.example`).

## Transcription dictionary

The optional `transcription-dictionary.txt` file supplies whisper.cpp's
`--prompt` with preferred vocabulary. Put one term per line. Blank lines and
lines beginning with `#` are ignored, and duplicate terms are removed while
preserving their first occurrence.

This is a recognition hint, not a forced replacement table: Whisper can still
choose another spelling when the audio strongly suggests it. The transcript
remains the direct Whisper output, so the existing "verbatim transcript first"
design is preserved.

The real dictionary is ignored by Git so names or other personal vocabulary do
not get committed accidentally. Copy `transcription-dictionary.example.txt`
to `transcription-dictionary.txt` and edit it locally. To keep the file
elsewhere, set `WHISPER_DICTIONARY` to an absolute path. If the file is
missing or contains no usable entries, the manual layer contributes nothing.

### Automatic vocabulary from my-storage-note

The receiver also reads `my-storage-note/memory/entities/index.json` and adds a
filtered set of Entity names after the manual terms. It first looks for a
`my-storage-note` repository beside `sticks3-voice-capture`. If the clone
lives elsewhere, set `MY_STORAGE_NOTE_PATH`. If no local clone is available,
the receiver can use the GitHub Contents API with
`KNOWLEDGE_GITHUB_TOKEN` (falling back to `GITHUB_TOKEN`).

The automatic layer is deliberately conservative:

- kinds: `person`, `organization`, `place`, `concept`
- canonical `name` only by default; aliases are opt-in because Entity aliases
  can contain extraction noise
- one-off entities are included when seen within the recent window (180 days by
  default)
- older entities require at least two mentions
- one-character/noise names are rejected
- results are ranked toward recent vocabulary and capped by both term count and
  character budget before being added to Whisper's `--prompt`

Manual terms always come first, so they take priority when the prompt budget is
tight. The selected automatic vocabulary is written for inspection to
`STICKS3_DATA_DIR/transcription-dictionary.auto.txt`. It is refreshed every
six hours by default. If the Knowledge System cannot be read, transcription
continues with the manual dictionary (and the last good cached Entity terms, if
available).

The auto vocabulary is still only a Whisper recognition hint. It never rewrites
the resulting transcript.

## Durability contract

The device is only told "stored" after the Ogg bytes are written and
`fsync`ed to `STICKS3_DATA_DIR/recordings/<id>.ogg`. Transcription and the
mynotebook push happen afterward in a background thread — a failure there
does not affect the device's "sent" status, and does not need to be retried
by the device. If `GITHUB_TOKEN` is unset, the transcript and Markdown note
are still written locally, and the mynotebook push is skipped (`"disabled"` in
the `done/<id>.json` marker) — inspect the file and push manually.

## Idempotency

The same `X-Recording-ID` with the same audio bytes returns `200` and does
nothing further. The same ID with different bytes returns `409
recording_id_conflict` — the client must not silently retry with a mutated ID
scheme that overwrites unrelated content.

## Data layout

```
STICKS3_DATA_DIR/  (default: ~/sticks3-voice-capture-data)
  recordings/<id>.ogg     raw upload
  metadata/<id>.json      sha256, received_at
  transcripts/<id>.txt    verbatim Whisper output
  transcription-dictionary.auto.txt  inspected auto-selected Entity vocabulary
  notes/<id>.md           rendered Markdown pushed to mynotebook
  done/<id>.json          completion marker + mynotebook push status
```
