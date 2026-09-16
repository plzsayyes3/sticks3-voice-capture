import base64
import hashlib
import hmac
import json
import os
import re
import subprocess
import tempfile
import threading
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import requests
from flask import Flask, abort, jsonify, request

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = int(os.getenv("MAX_UPLOAD_BYTES", str(8 * 1024 * 1024)))

RECORDING_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")
ALLOWED_AUDIO_TYPES = {"audio/ogg", "audio/opus", "application/ogg"}
JST = ZoneInfo("Asia/Tokyo")

DATA_DIR = Path(os.getenv("STICKS3_DATA_DIR", str(Path.home() / "sticks3-voice-capture-data")))
RECORDINGS_DIR = DATA_DIR / "recordings"
TRANSCRIPTS_DIR = DATA_DIR / "transcripts"
NOTES_DIR = DATA_DIR / "notes"
DONE_DIR = DATA_DIR / "done"
METADATA_DIR = DATA_DIR / "metadata"

WHISPER_BIN = os.getenv("WHISPER_CLI", "whisper-cli")
WHISPER_MODEL = os.getenv("WHISPER_MODEL", str(Path.home() / "whisper-models" / "ggml-large-v3-turbo.bin"))
WHISPER_VAD_MODEL = os.getenv("WHISPER_VAD_MODEL", str(Path.home() / "whisper-models" / "ggml-silero-v5.1.2.bin"))
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", "ja")

for _dir in (RECORDINGS_DIR, TRANSCRIPTS_DIR, NOTES_DIR, DONE_DIR, METADATA_DIR):
    _dir.mkdir(parents=True, exist_ok=True)


def env_required(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"missing required environment variable: {name}")
    return value


def require_device_token() -> None:
    expected = env_required("DEVICE_TOKEN")
    header = request.headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        abort(401)
    supplied = header[7:]
    if not hmac.compare_digest(supplied, expected):
        abort(403)


def metadata_path(recording_id: str) -> Path:
    return METADATA_DIR / f"{recording_id}.json"


def read_metadata(recording_id: str):
    path = metadata_path(recording_id)
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def write_metadata(recording_id: str, metadata: dict) -> None:
    metadata_path(recording_id).write_text(
        json.dumps(metadata, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )


def store_recording_durably(recording_id: str, data: bytes) -> None:
    """Write the Ogg to disk and fsync before returning, so a durable write
    is a stronger guarantee than the OS page cache alone."""
    ogg_path = RECORDINGS_DIR / f"{recording_id}.ogg"
    with open(ogg_path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def transcribe_ogg(ogg_path: Path) -> str:
    with tempfile.TemporaryDirectory() as temp_dir:
        wav_path = Path(temp_dir) / "audio.wav"
        subprocess.run(
            [
                "ffmpeg", "-y", "-i", str(ogg_path),
                "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le",
                str(wav_path),
            ],
            check=True, capture_output=True,
        )

        out_prefix = Path(temp_dir) / "transcript"
        subprocess.run(
            [
                WHISPER_BIN,
                "-m", WHISPER_MODEL,
                "-l", WHISPER_LANGUAGE,
                "-f", str(wav_path),
                "--vad", "-vm", WHISPER_VAD_MODEL,
                "-of", str(out_prefix),
                "--output-txt",
                "--no-prints",
            ],
            check=True, capture_output=True,
        )

        text_path = out_prefix.with_suffix(".txt")
        transcript = text_path.read_text(encoding="utf-8").strip()
        if not transcript:
            raise RuntimeError("whisper-cli produced an empty transcript")
        return transcript


def render_markdown(recording_id: str, received_at: str, transcript: str) -> str:
    return (
        "---\n"
        "type: voice-capture\n"
        "source: sticks3\n"
        f"recording_id: {recording_id}\n"
        f"received_at: {received_at}\n"
        "---\n"
        f"# StickS3 Voice Capture — {recording_id}\n\n"
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


def process_recording_async(recording_id: str) -> None:
    try:
        metadata = read_metadata(recording_id)
        ogg_path = RECORDINGS_DIR / f"{recording_id}.ogg"

        transcript_path = TRANSCRIPTS_DIR / f"{recording_id}.txt"
        if transcript_path.exists():
            transcript = transcript_path.read_text(encoding="utf-8")
        else:
            transcript = transcribe_ogg(ogg_path)
            transcript_path.write_text(transcript, encoding="utf-8")

        markdown_path = NOTES_DIR / f"{recording_id}.md"
        if markdown_path.exists():
            markdown = markdown_path.read_text(encoding="utf-8")
        else:
            markdown = render_markdown(recording_id, metadata["received_at"], transcript)
            markdown_path.write_text(markdown, encoding="utf-8")

        notebook_path = mynotebook_path(recording_id, metadata["received_at"])
        notebook_status = push_to_mynotebook(notebook_path, markdown)

        done_path = DONE_DIR / f"{recording_id}.json"
        done_path.write_text(
            json.dumps(
                {
                    "recording_id": recording_id,
                    "completed_at": datetime.now(timezone.utc).isoformat(),
                    "mynotebook_path": notebook_path,
                    "mynotebook_status": notebook_status,
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
    except Exception:
        app.logger.exception("processing failed for %s", recording_id)


@app.get("/health")
def health():
    return jsonify({"ok": True, "service": "sticks3-voice-local-receiver"})


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
    existing = read_metadata(recording_id)

    if existing is not None:
        if not hmac.compare_digest(existing.get("sha256", ""), sha256):
            return jsonify({"ok": False, "error": "recording_id_conflict"}), 409
        duplicate = True
        received_at = existing["received_at"]
    else:
        duplicate = False
        received_at = datetime.now(timezone.utc).isoformat()
        store_recording_durably(recording_id, data)
        write_metadata(
            recording_id,
            {
                "recording_id": recording_id,
                "sha256": sha256,
                "received_at": received_at,
                "source": "sticks3",
            },
        )

    # Device only needs the durable write to count as "sent" — transcription
    # runs in the background and its failure must not block the device.
    threading.Thread(target=process_recording_async, args=(recording_id,), daemon=True).start()

    return jsonify(
        {
            "ok": True,
            "recording_id": recording_id,
            "status": "already_stored" if duplicate else "stored",
            "sha256": sha256,
        }
    ), 200 if duplicate else 201


@app.errorhandler(413)
def too_large(_error):
    return jsonify({"ok": False, "error": "upload_too_large"}), 413


if __name__ == "__main__":
    port = int(os.getenv("PORT", "8090"))
    app.run(host="0.0.0.0", port=port)
