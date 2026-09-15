#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${PROJECT_ID:?Set PROJECT_ID to your Google Cloud project ID}"
: "${DEVICE_TOKEN:?Set DEVICE_TOKEN to a random StickS3 upload token}"
: "${PROCESSOR_TOKEN:?Set PROCESSOR_TOKEN to a separate random internal token}"
: "${GEMINI_API_KEY:?Set GEMINI_API_KEY}"

REGION="${REGION:-asia-northeast1}"
SERVICE="${SERVICE:-sticks3-voice-cloud}"
BUCKET="${BUCKET:-${PROJECT_ID}-sticks3-voice}"
TOPIC="${TOPIC:-sticks3-voice-processing}"
SUBSCRIPTION="${SUBSCRIPTION:-sticks3-voice-processing-push}"
RUNTIME_SA="${RUNTIME_SA:-sticks3-voice-api}"
RUNTIME_SA_EMAIL="${RUNTIME_SA}@${PROJECT_ID}.iam.gserviceaccount.com"

DEVICE_SECRET="${SERVICE}-device-token"
PROCESSOR_SECRET="${SERVICE}-processor-token"
GEMINI_SECRET="${SERVICE}-gemini-api-key"
GITHUB_SECRET="${SERVICE}-github-token"

upsert_secret() {
  local name="$1"
  local value="$2"
  if gcloud secrets describe "$name" --project "$PROJECT_ID" >/dev/null 2>&1; then
    printf '%s' "$value" | gcloud secrets versions add "$name" --data-file=- --project "$PROJECT_ID" >/dev/null
  else
    printf '%s' "$value" | gcloud secrets create "$name" --replication-policy=automatic --data-file=- --project "$PROJECT_ID" >/dev/null
  fi
}

echo "Using project: $PROJECT_ID"
gcloud config set project "$PROJECT_ID" >/dev/null

gcloud services enable \
  run.googleapis.com \
  cloudbuild.googleapis.com \
  artifactregistry.googleapis.com \
  storage.googleapis.com \
  pubsub.googleapis.com \
  secretmanager.googleapis.com \
  --project "$PROJECT_ID"

if ! gcloud iam service-accounts describe "$RUNTIME_SA_EMAIL" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud iam service-accounts create "$RUNTIME_SA" \
    --display-name="StickS3 Voice Cloud runtime" \
    --project "$PROJECT_ID"
fi

for role in roles/storage.objectAdmin roles/pubsub.publisher roles/secretmanager.secretAccessor; do
  gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member="serviceAccount:${RUNTIME_SA_EMAIL}" \
    --role="$role" \
    --quiet >/dev/null
done

if ! gcloud storage buckets describe "gs://${BUCKET}" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud storage buckets create "gs://${BUCKET}" \
    --location="$REGION" \
    --uniform-bucket-level-access \
    --project "$PROJECT_ID"
fi

if ! gcloud pubsub topics describe "$TOPIC" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud pubsub topics create "$TOPIC" --project "$PROJECT_ID"
fi

upsert_secret "$DEVICE_SECRET" "$DEVICE_TOKEN"
upsert_secret "$PROCESSOR_SECRET" "$PROCESSOR_TOKEN"
upsert_secret "$GEMINI_SECRET" "$GEMINI_API_KEY"

SECRET_BINDINGS="DEVICE_TOKEN=${DEVICE_SECRET}:latest,PROCESSOR_TOKEN=${PROCESSOR_SECRET}:latest,GEMINI_API_KEY=${GEMINI_SECRET}:latest"
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
  upsert_secret "$GITHUB_SECRET" "$GITHUB_TOKEN"
  SECRET_BINDINGS="${SECRET_BINDINGS},GITHUB_TOKEN=${GITHUB_SECRET}:latest"
fi

gcloud run deploy "$SERVICE" \
  --source "$SCRIPT_DIR" \
  --region "$REGION" \
  --platform managed \
  --service-account "$RUNTIME_SA_EMAIL" \
  --allow-unauthenticated \
  --timeout 300 \
  --memory 512Mi \
  --set-env-vars="GOOGLE_CLOUD_PROJECT=${PROJECT_ID},STORAGE_BUCKET=${BUCKET},PUBSUB_TOPIC=${TOPIC},TRANSCRIBE_MODEL=gemini-3.5-transcribe,TRANSCRIBE_LANGUAGE=ja-JP,ORGANIZE_MODEL=gemini-3.5-flash-lite,MYNOTEBOOK_REPO=plzsayyes3/mynotebook,MYNOTEBOOK_BRANCH=main,MYNOTEBOOK_PATH_PREFIX=00_inbox" \
  --set-secrets="$SECRET_BINDINGS" \
  --project "$PROJECT_ID"

SERVICE_URL="$(gcloud run services describe "$SERVICE" --region "$REGION" --project "$PROJECT_ID" --format='value(status.url)')"

if gcloud pubsub subscriptions describe "$SUBSCRIPTION" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud pubsub subscriptions update "$SUBSCRIPTION" \
    --push-endpoint="${SERVICE_URL}/v1/process" \
    --ack-deadline=600 \
    --project "$PROJECT_ID"
else
  gcloud pubsub subscriptions create "$SUBSCRIPTION" \
    --topic="$TOPIC" \
    --push-endpoint="${SERVICE_URL}/v1/process" \
    --ack-deadline=600 \
    --project "$PROJECT_ID"
fi

cat <<EOF

Deployment complete.
Service URL: ${SERVICE_URL}
Health:      ${SERVICE_URL}/health
Upload:      ${SERVICE_URL}/v1/recordings
Bucket:      gs://${BUCKET}

Next smoke test:
  curl -i -X POST "${SERVICE_URL}/v1/recordings" \\
    -H "Authorization: Bearer <DEVICE_TOKEN>" \\
    -H "X-Recording-ID: mac-smoke-001" \\
    -H "Content-Type: audio/ogg" \\
    --data-binary @recording.ogg
EOF
