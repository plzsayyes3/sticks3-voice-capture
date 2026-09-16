# StickS3 Voice Cloud

Cloud Run service for receiving completed StickS3 Ogg/Opus recordings, storing them durably in Cloud Storage, transcribing them with Gemini, organizing the transcript, and optionally writing the resulting Markdown to `plzsayyes3/mynotebook`.

## Flow

```text
StickS3
  -> POST /v1/recordings
  -> Cloud Storage: recordings/<recording_id>.ogg
  -> Pub/Sub
  -> POST /v1/process
  -> Gemini 3.5 Transcribe (verbatim)
  -> Cloud Storage: transcripts/<recording_id>.txt
  -> Gemini Flash-Lite organization
  -> Cloud Storage: notes/<recording_id>.md
  -> optional GitHub Contents API -> mynotebook/00_inbox
  -> Cloud Storage: done/<recording_id>.json
```

The device receives success as soon as the Ogg file is durably stored and a processing event has been queued. It does not wait for Gemini.

## Upload API

`POST /v1/recordings`

Headers:

```text
Authorization: Bearer <DEVICE_TOKEN>
X-Recording-ID: <stable recording id>
Content-Type: audio/ogg
```

Body: raw Ogg/Opus bytes.

Example response:

```json
{
  "ok": true,
  "recording_id": "00000012-abcd1234",
  "status": "stored",
  "sha256": "..."
}
```

Retries are idempotent. If the same recording ID is uploaded again with the same SHA-256, the service returns success and requeues processing. If the ID exists with different bytes, it returns HTTP 409 and does not overwrite the existing recording.

## Environment variables

Required:

- `STORAGE_BUCKET`: Cloud Storage bucket name.
- `PUBSUB_TOPIC`: Pub/Sub topic name used to queue transcription.
- `GOOGLE_CLOUD_PROJECT`: Google Cloud project ID.
- `DEVICE_TOKEN`: bearer token used by StickS3.
- `PROCESSOR_TOKEN`: random internal token embedded in the Pub/Sub processing event.
- `GEMINI_API_KEY`: Gemini API key.

Optional:

- `MAX_UPLOAD_BYTES`: upload limit; default 8 MiB.
- `TRANSCRIBE_MODEL`: default `gemini-3.5-transcribe`.
- `TRANSCRIBE_LANGUAGE`: default `ja-JP`.
- `ORGANIZE_MODEL`: default `gemini-3.5-flash-lite`.
- `GITHUB_TOKEN`: when set, enables writing completed Markdown to GitHub.
- `MYNOTEBOOK_REPO`: default `plzsayyes3/mynotebook`.
- `MYNOTEBOOK_BRANCH`: default `main`.
- `MYNOTEBOOK_PATH_PREFIX`: default `00_inbox`.

Do not put secrets in the repository or firmware source. Store server-side secrets in Secret Manager. The device should contain only its dedicated `DEVICE_TOKEN`.

## Storage layout

```text
recordings/<recording_id>.ogg     original audio, canonical binary
transcripts/<recording_id>.txt    verbatim Gemini transcript
notes/<recording_id>.md            organized note + transcript
done/<recording_id>.json           processing completion marker
```

Completed audio is never overwritten by a retry.

## Local container

```bash
docker build -t sticks3-voice-cloud ./cloud
```

Cloud credentials and environment variables are required before exercising endpoints that access Storage, Pub/Sub, Gemini, or GitHub.

## Cloud Run

The container listens on the `PORT` environment variable supplied by Cloud Run. `GET /health` can be used as a basic health check.

A deploy helper is provided in `deploy.sh`. It creates/updates the Cloud Storage bucket, Pub/Sub topic, runtime service account, Secret Manager values, Cloud Run service, and Pub/Sub push subscription.

## First acceptance test

Before connecting StickS3 firmware, upload one already-extracted recording from a Mac:

```bash
curl -i \
  -X POST "$SERVICE_URL/v1/recordings" \
  -H "Authorization: Bearer $DEVICE_TOKEN" \
  -H "X-Recording-ID: mac-smoke-001" \
  -H "Content-Type: audio/ogg" \
  --data-binary @recording.ogg
```

Acceptance criteria:

1. Response is 201 `stored`.
2. `recordings/mac-smoke-001.ogg` appears in Cloud Storage.
3. `transcripts/mac-smoke-001.txt` appears.
4. `notes/mac-smoke-001.md` appears.
5. `done/mac-smoke-001.json` appears.
6. If `GITHUB_TOKEN` is configured, one note appears under `mynotebook/00_inbox`.
7. Repeating the identical upload does not create a second recording.
