# Cloud Receiver Handoff

This file is the handoff for continuing the StickS3 cloud pipeline work.

## Repository / branch

- Repository: `plzsayyes3/sticks3-voice-capture`
- Work branch: `feature/cloud-receiver-api`
- Draft PR: #2 `Add Cloud Run receiver and Gemini transcription pipeline`
- Base branch for PR #2: `feature/local-recording-mvp`
- Cloud CI is green as of the handoff.

Before changing anything, fetch and confirm the latest remote HEAD. Other agents may be working in parallel. Re-check HEAD immediately before each write and do not overwrite unrelated changes.

## Canonical project

Knowledge System canonical source:

- Repository: `plzsayyes3/my-storage-note`
- Project Object: `objects/projects/sticks3-voice-capture.md`

`plzsayyes3/gpts` is legacy / read-only and should not be used as the canonical source.

## What is already proven on real hardware

The local recording path has passed the first real-device gate:

- StickS3 front button starts/stops recording.
- ES8311 / I2S capture works.
- PCM 16 kHz mono -> Opus works.
- Ogg/Opus is written to internal FAT storage.
- `.part` is finalized to `.ogg` on successful stop.
- BLE does not need to be connected for local recording.
- A real recorded `.ogg` was extracted to the Mac and played successfully with audible speech.

Example successful device log:

```text
recording_store: recording finalized: /recordings/00000001-b439253d.ogg (14727 bytes)
audio_pipeline: recording saved: packets=95 size=14727
```

The original FAT failure (`errno=22`) was fixed by enabling long filenames in `sdkconfig.defaults`.

## Important local Mac state

On the user's Mac, `firmware/main/main.c` has a LOCAL uncommitted modification from debugging: automatic light sleep was changed from `true` to `false` so USB serial stays available.

Do not accidentally commit, discard, or overwrite this local modification unless the user explicitly asks to resolve the power-management debug change.

The user has already switched the local checkout to `feature/cloud-receiver-api` and pulled it.

## Cloud architecture already implemented

```text
StickS3
  -> POST /v1/recordings
  -> Cloud Run
  -> Cloud Storage: recordings/<recording_id>.ogg
  -> Pub/Sub
  -> POST /v1/process
  -> Gemini transcription
  -> Cloud Storage: transcripts/<recording_id>.txt
  -> Gemini organization
  -> Cloud Storage: notes/<recording_id>.md
  -> optional GitHub Contents API -> plzsayyes3/mynotebook/00_inbox
  -> Cloud Storage: done/<recording_id>.json
```

Key files:

- `cloud/app.py`
- `cloud/requirements.txt`
- `cloud/Dockerfile`
- `cloud/deploy.sh`
- `cloud/README.md`
- `.github/workflows/cloud-ci.yml`

## Upload API behavior

Endpoint:

```text
POST /v1/recordings
```

Headers:

```text
Authorization: Bearer <DEVICE_TOKEN>
X-Recording-ID: <stable recording id>
Content-Type: audio/ogg
```

Body: raw Ogg/Opus bytes.

The service stores the recording durably before acknowledging the device. Processing is queued asynchronously; StickS3 must not wait for Gemini transcription.

Retries are idempotent:

- same `recording_id` + same SHA-256 -> success / requeue
- same `recording_id` + different bytes -> HTTP 409, never overwrite original

## Secrets

Never commit secrets to the repository or firmware source.

Deployment expects these values in the local shell and stores server-side secrets in Google Secret Manager:

- `PROJECT_ID`
- `DEVICE_TOKEN`
- `PROCESSOR_TOKEN`
- `GEMINI_API_KEY`
- optional `GITHUB_TOKEN`

Generate dedicated random device/internal tokens. Do not reuse unrelated credentials.

## Immediate task

Continue from the current point and deploy the cloud pipeline to the user's Google Cloud account.

1. Confirm `gcloud` is installed. If not, guide/install it using the normal supported method for the user's Mac.
2. Authenticate with `gcloud auth login` and identify/select the intended Google Cloud project.
3. Generate `DEVICE_TOKEN` and `PROCESSOR_TOKEN` locally.
4. Obtain/use the user's Gemini API key without committing or echoing it unnecessarily.
5. Run `cloud/deploy.sh` and fix any deployment/IAM/API issues encountered.
6. Verify `GET /health` on the deployed Cloud Run URL.
7. Use an already-extracted real StickS3 `.ogg` from the Mac for the first end-to-end upload test. Do NOT connect the StickS3 side-button uploader yet.
8. Verify, in order:
   - upload returns `201 stored`
   - `recordings/<id>.ogg` exists
   - `transcripts/<id>.txt` exists
   - `notes/<id>.md` exists
   - `done/<id>.json` exists
   - transcript is sensible Japanese and corresponds to the real recording
9. If `GITHUB_TOKEN` is configured, verify one file is created under `plzsayyes3/mynotebook/00_inbox`. If not configured, stop after Cloud Storage/Gemini success and report that mynotebook delivery remains intentionally disabled.
10. Re-upload the exact same file and confirm idempotent behavior (no duplicate recording).

## First real audio file

A previous extraction produced a file similar to:

```text
~/GitHub/sticks3-voice-capture/recording-dump/extracted/NO NAME/00000001-b439253d.ogg
```

Do not assume this exact filename still exists. Locate the current extracted `.ogg` on the Mac before testing.

## Acceptance criteria for this handoff

The handoff is complete when one real StickS3 recording has successfully traveled through:

```text
Mac test upload
-> Cloud Run
-> Cloud Storage
-> Pub/Sub
-> Gemini transcription
-> generated Markdown
```

and the transcript has been inspected for correctness.

mynotebook write is an additional acceptance criterion only if `GITHUB_TOKEN` is intentionally configured during this session.

## Do not do yet

- Do not merge PR #2 automatically.
- Do not implement the StickS3 Wi-Fi side-button upload client until the cloud endpoint has passed the real `.ogg` test.
- Do not erase StickS3 flash.
- Do not modify the working local recorder path unless cloud testing proves it necessary.
- Do not remove the local USB/light-sleep debugging change without explicitly handling that separate issue.

## After cloud acceptance passes

Next workstream:

- implement StickS3 Wi-Fi configuration and HTTPS upload
- side button sends all unsent completed `.ogg` files
- mark sent only after server success
- offline/network failure leaves files untouched for retry
- preserve idempotency using a stable recording ID

Keep device capture and cloud sync failure-independent: failure to upload must never endanger locally completed recordings.
