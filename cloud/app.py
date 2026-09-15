import base64
import hashlib
import hmac
import json
import os
import re
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import requests
from flask import Flask, abort, jsonify, request
from google import genai
from google.api_core.exceptions import PreconditionFailed
from google.cloud import pubsub_v1, storage


app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = int(os.getenv("MAX_UPLOAD_BYTES", str(8 * 1024 * 1024)))

RECORDING_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")
ALLOWED_AUDIO_TYPES = {"audio/ogg", "audio/opus", "application/ogg"}
JST = ZoneInfo("Asia/Tokyo")

_storage_client = None
_publisher = None
_gemini_client = None


def env_required(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"missing required environment variable: {name}")
    return value


def storage_client():
    global _storage_client
    if _storage_client is None:
        _storage_client = storage.Client()
    return _storage_client


def publisher_client():
    global _publisher
    if _publisher is None:
        _publisher = pubsub_v1.PublisherClient()
    return _publisher


def gemini_client():
    global _gemini_client
    if _gemini_client is None:
        _gemini_client = genai.Client(api_key=env_required("GEMINI_API_KEY"))
    return _gemini_client


def require_device_token() -> None:
    expected = env_required("DEVICE_TOKEN")
    header = request.headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        abort(401)
    supplied = header[7:]
    if not hmac.compare_digest(supplied, expected):
        abort(403)


def get_bucket():
    return storage_client().bucket(env_required("STORAGE_BUCKET"))


def publish_process_event(recording_id: str, object_name: str, sha256: str) -> None:
    project_id = env_required("GOOGLE_CLOUD_PROJECT")
    topic_name = env_required("PUBSUB_TOPIC")
    topic_path = publisher_client().topic_path(project_id, topic_name)
    payload = {
        "version": 1,
        "recording_id": recording_id,
        "bucket": env_required("STORAGE_BUCKET"),
        "object": object_name,
        "sha256": sha256,
        "processor_token": env_required("PROCESSOR_TOKEN"),
    }
    future = publisher_client().publish(
        topic_path,
        json.dumps(payload, separators=(",", ":")).encode("utf-8"),
    )
    future.result(timeout=10)


def decode_pubsub_event() -> dict:
    envelope = request.get_json(silent=True) or {}
    message = envelope.get("message") or {}
    encoded = message.get("data")
    if not encoded:
        abort(400)
    try:
        payload = json.loads(base64.b64decode(encoded).decode("utf-8"))
    except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
        abort(400)

    supplied = str(payload.pop("processor_token", ""))
    expected = env_required("PROCESSOR_TOKEN")
    if not hmac.compare_digest(supplied, expected):
        abort(403)
    return payload


def blob_text_if_exists(blob):
    if not blob.exists():
        return None
    return blob.download_as_text(encoding="utf-8")


def transcribe_ogg(local_path: str) -> str:
    client = gemini_client()
    uploaded = client.files.upload(
        file=local_path,
        config={"mime_type": "audio/ogg"},
    )
    try:
        interaction = client.interactions.create(
            model=os.getenv("TRANSCRIBE_MODEL", "gemini-3.5-transcribe"),
            input=[
                {
                    "type": "audio",
                    "uri": uploaded.uri,
                    "mime_type": uploaded.mime_type,
                }
            ],
            generation_config={
                "transcription_config": {
                    "language_codes": [os.getenv("TRANSCRIBE_LANGUAGE", "ja-JP")],
                    "mode": {"type": "verbatim"},
                }
            },
        )
        text = (interaction.output_text or "").strip()
        if not text:
            raise RuntimeError("Gemini returned an empty transcript")
        return text
    finally:
        try:
            client.files.delete(name=uploaded.name)
        except Exception:
            app.logger.exception("failed to delete temporary Gemini file")


def organize_transcript(transcript: str) -> str:
    prompt = f"""You organize short Japanese voice notes for a personal knowledge inbox.
Return Markdown only. Do not use a code fence.
Use exactly these headings:
## Summary
## Tasks
## Ideas

Rules:
- Keep Summary concise and faithful.
- Tasks must include only explicit or strongly implied actions. If none, write '- なし'.
- Ideas must include only ideas actually present in the transcript. If none, write '- なし'.
- Do not invent facts, deadlines, people, or decisions.
- Write in Japanese.

Transcript:
{transcript}
"""
    interaction = gemini_client().interactions.create(
        model=os.getenv("ORGANIZE_MODEL", "gemini-3.5-flash-lite"),
        input=prompt,
    )
    text = (interaction.output_text or "").strip()
    if not text:
        raise RuntimeError("Gemini returned empty organized output")
    return text


def render_markdown(recording_id: str, received_at: str, organized: str, transcript: str) -> str:
    return (
        "---\n"
        "type: voice-capture\n"
        "source: sticks3\n"
        f"recording_id: {recording_id}\n"
        f"received_at: {received_at}\n"
        "---\n"
        f"# StickS3 Voice Capture — {recording_id}\n\n"
        f"{organized}\n\n"
        "## Transcript\n"
        f"{transcript.strip()}\n"
    )


def mynotebook_path(recording_id: str, received_at: str) -> str:
    dt = datetime.fromisoformat(received_at.replace("Z", "+00:00")).astimezone(JST)
    stamp = dt.strftime("%Y%m%d%H%M%S")
    prefix = os.getenv("MYNOTEBOOK_PATH_PREFIX", "00_inbox").strip("/")
    return f"{prefix}/{stamp}-sticks3-{recording_id}.md"


def push_to_mynotebook(path: str, markdown: str) -> str:
    token = os.getenv("GITHUB_TOKEN", "").strip()
    if not token:
        return "disabled"

    repo = os.getenv("MYNOTEBOOK_REPO", "plzsayyes3/mynotebook")
    branch = os.getenv("MYNOTEBOOK_BRANCH", "main")
    url = f"https://api.github.com/repos/{repo}/contents/{path}"
    headers = {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
        "X-GitHub-Api-Version": "2022-11-28",
    }

    existing = requests.get(url, headers=headers, params={"ref": branch}, timeout=20)
    if existing.status_code == 200:
        return "exists"
    if existing.status_code != 404:
        raise RuntimeError(f"GitHub lookup failed: {existing.status_code} {existing.text[:300]}")

    body = {
        "message": f"Add StickS3 voice capture {path.rsplit('/', 1)[-1]}",
        "content": base64.b64encode(markdown.encode("utf-8")).decode("ascii"),
        "branch": branch,
    }
    response = requests.put(url, headers=headers, json=body, timeout=30)
    if response.status_code not in (200, 201):
        raise RuntimeError(f"GitHub write failed: {response.status_code} {response.text[:300]}")
    return "created"


@app.get("/health")
def health():
    return jsonify({"ok": True, "service": "sticks3-voice-cloud"})


@app.post("/v1/recordings")
def receive_recording():
    require_device_token()

    recording_id = request.headers.get("X-Recording-ID", "").strip()
    if not RECORDING_ID_RE.fullmatch(recording_id):
        return jsonify({"ok": False, "error": "invalid_recording_id"}), 400

    content_type = (request.mimetype or "").lower()
    if content_type not in ALLOWED_AUDIO_TYPES:
        return jsonify({"ok": False, "error": "unsupported_content_type"}), 415

    data = request.get_data(cache=False, as_text=False)
    if not data:
        return jsonify({"ok": False, "error": "empty_body"}), 400

    sha256 = hashlib.sha256(data).hexdigest()
    received_at = datetime.now(timezone.utc).isoformat()
    object_name = f"recordings/{recording_id}.ogg"
    blob = get_bucket().blob(object_name)
    blob.metadata = {
        "recording_id": recording_id,
        "sha256": sha256,
        "received_at": received_at,
        "source": "sticks3",
    }

    duplicate = False
    try:
        blob.upload_from_string(
            data,
            content_type="audio/ogg",
            if_generation_match=0,
            checksum="auto",
        )
    except PreconditionFailed:
        duplicate = True
        blob.reload()
        existing_sha = (blob.metadata or {}).get("sha256", "")
        if not hmac.compare_digest(existing_sha, sha256):
            return jsonify({"ok": False, "error": "recording_id_conflict"}), 409
        received_at = (blob.metadata or {}).get("received_at", received_at)

    try:
        publish_process_event(recording_id, object_name, sha256)
    except Exception:
        app.logger.exception("failed to enqueue processing for %s", recording_id)
        return jsonify({"ok": False, "error": "stored_but_not_queued", "recording_id": recording_id}), 503

    return jsonify(
        {
            "ok": True,
            "recording_id": recording_id,
            "status": "already_stored" if duplicate else "stored",
            "sha256": sha256,
        }
    ), 200 if duplicate else 201


@app.post("/v1/process")
def process_recording():
    event = decode_pubsub_event()
    recording_id = str(event.get("recording_id", ""))
    object_name = str(event.get("object", ""))
    expected_sha = str(event.get("sha256", ""))

    if not RECORDING_ID_RE.fullmatch(recording_id) or object_name != f"recordings/{recording_id}.ogg":
        abort(400)

    bucket = get_bucket()
    source_blob = bucket.blob(object_name)
    source_blob.reload()
    metadata = source_blob.metadata or {}
    if expected_sha and metadata.get("sha256") != expected_sha:
        abort(409)
    received_at = metadata.get("received_at") or datetime.now(timezone.utc).isoformat()

    done_blob = bucket.blob(f"done/{recording_id}.json")
    if done_blob.exists():
        return ("", 204)

    transcript_blob = bucket.blob(f"transcripts/{recording_id}.txt")
    transcript = blob_text_if_exists(transcript_blob)
    if transcript is None:
        with tempfile.TemporaryDirectory() as temp_dir:
            local_path = str(Path(temp_dir) / f"{recording_id}.ogg")
            source_blob.download_to_filename(local_path)
            transcript = transcribe_ogg(local_path)
        transcript_blob.upload_from_string(
            transcript,
            content_type="text/plain; charset=utf-8",
            if_generation_match=0,
        )

    markdown_blob = bucket.blob(f"notes/{recording_id}.md")
    markdown = blob_text_if_exists(markdown_blob)
    if markdown is None:
        organized = organize_transcript(transcript)
        markdown = render_markdown(recording_id, received_at, organized, transcript)
        markdown_blob.upload_from_string(
            markdown,
            content_type="text/markdown; charset=utf-8",
            if_generation_match=0,
        )

    notebook_path = mynotebook_path(recording_id, received_at)
    notebook_status = push_to_mynotebook(notebook_path, markdown)

    done_blob.upload_from_string(
        json.dumps(
            {
                "recording_id": recording_id,
                "completed_at": datetime.now(timezone.utc).isoformat(),
                "mynotebook_path": notebook_path,
                "mynotebook_status": notebook_status,
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        content_type="application/json",
        if_generation_match=0,
    )
    return ("", 204)


@app.errorhandler(413)
def too_large(_error):
    return jsonify({"ok": False, "error": "upload_too_large"}), 413


if __name__ == "__main__":
    port = int(os.getenv("PORT", "8080"))
    app.run(host="0.0.0.0", port=port)
