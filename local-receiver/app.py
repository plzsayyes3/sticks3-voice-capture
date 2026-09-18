import base64
import hashlib
import hmac
import json
import os
import queue
import re
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import requests
from flask import Flask, abort, jsonify, request

import entity_dictionary

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = int(os.getenv("MAX_UPLOAD_BYTES", str(8 * 1024 * 1024)))

# GitHub's Contents API writes update a single mutable branch ref, so two
# concurrent pushes race even when they touch different files — the loser
# gets a 409 (stale base SHA). Recordings can finish processing in
# parallel (e.g. a batch uploaded together by Wi-Fi Sync), so serialize
# the actual push.
_mynotebook_push_lock = threading.Lock()

# whisper-cli is CPU-bound and heavy (see WHISPER_USE_GPU — even Metal was
# disabled after it crashed). Spawning one thread per upload let a batch
# sync run several whisper-cli processes at once, competing for the same
# CPU/GPU and memory. A single persistent worker processes recordings
# strictly one at a time; the queue absorbs bursts instead.
_processing_queue: "queue.Queue[str]" = queue.Queue()

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
WHISPER_DICTIONARY = Path(os.getenv(
    "WHISPER_DICTIONARY",
    str(Path(__file__).with_name("transcription-dictionary.txt")),
))
WHISPER_PROMPT_MAX_TERMS = int(os.getenv("WHISPER_PROMPT_MAX_TERMS", "80"))
WHISPER_PROMPT_MAX_CHARS = int(os.getenv("WHISPER_PROMPT_MAX_CHARS", "320"))
ENTITY_DICTIONARY_ENABLED = entity_dictionary.env_bool("ENTITY_DICTIONARY_ENABLED", True)
ENTITY_DICTIONARY_REFRESH_SECONDS = int(
    os.getenv("ENTITY_DICTIONARY_REFRESH_SECONDS", str(6 * 60 * 60))
)
AUTO_DICTIONARY_PATH = DATA_DIR / "transcription-dictionary.auto.txt"
_entity_dictionary_lock = threading.Lock()
_entity_dictionary_cache = {
    "terms": [],
    "source": "uninitialized",
    "expires_at": 0.0,
}
# Metal (GPU) whisper-cli aborts with SIGABRT on this Mac; default to CPU
# (-ng / no-GPU) until that's root-caused. Set WHISPER_USE_GPU=1 to opt
# back into Metal once it's fixed or on a machine where it works.
WHISPER_USE_GPU = os.getenv("WHISPER_USE_GPU", "0").strip() not in ("", "0", "false", "False")

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


def atomic_write_text(path: Path, text: str) -> None:
    """Write via a same-directory temp file + os.replace so a crash mid-write
    can never leave a truncated/corrupt JSON or Markdown file behind."""
    tmp_path = path.with_name(f".{path.name}.tmp")
    tmp_path.write_text(text, encoding="utf-8")
    os.replace(tmp_path, path)


def metadata_path(recording_id: str) -> Path:
    return METADATA_DIR / f"{recording_id}.json"


def read_metadata(recording_id: str):
    path = metadata_path(recording_id)
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def write_metadata(recording_id: str, metadata: dict) -> None:
    atomic_write_text(
        metadata_path(recording_id),
        json.dumps(metadata, ensure_ascii=False, separators=(",", ":")),
    )


def store_recording_durably(recording_id: str, data: bytes) -> None:
    """Write the Ogg to disk and fsync before returning, so a durable write
    is a stronger guarantee than the OS page cache alone."""
    ogg_path = RECORDINGS_DIR / f"{recording_id}.ogg"
    with open(ogg_path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def load_dictionary_terms(path: Path) -> list[str]:
    """Load one preferred term per line from a local manual dictionary."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return []

    entries = []
    seen = set()
    for raw_line in lines:
        entry = " ".join(raw_line.strip().split())
        if not entry or entry.startswith("#") or entry in seen:
            continue
        seen.add(entry)
        entries.append(entry)
    return entries


def load_entity_dictionary_terms(force: bool = False) -> list[str]:
    """Return cached my-storage-note Entity terms without risking transcription.

    Refresh failures keep the last good cache. This makes Knowledge System
    access advisory: a private-repo auth problem must never block a recording.
    """
    if not ENTITY_DICTIONARY_ENABLED:
        return []

    now = time.monotonic()
    with _entity_dictionary_lock:
        expires_at = float(_entity_dictionary_cache["expires_at"])
        if not force and now < expires_at:
            return list(_entity_dictionary_cache["terms"])

        previous_terms = list(_entity_dictionary_cache["terms"])
        try:
            terms, source = entity_dictionary.load_terms_from_environment(Path(__file__))
            _entity_dictionary_cache.update(
                terms=terms,
                source=source,
                expires_at=now + max(60, ENTITY_DICTIONARY_REFRESH_SECONDS),
            )
            if terms:
                atomic_write_text(
                    AUTO_DICTIONARY_PATH,
                    "# Auto-generated from my-storage-note/memory/entities/index.json\n"
                    f"# source: {source}\n"
                    + "\n".join(terms)
                    + "\n",
                )
                app.logger.info(
                    "loaded %d auto transcription terms from %s",
                    len(terms),
                    source,
                )
            elif source != "unavailable":
                app.logger.info("Entity dictionary source %s produced no terms", source)
            return list(terms)
        except Exception as exc:
            # Retry sooner after an outage, but preserve any previously-good set.
            _entity_dictionary_cache["expires_at"] = now + min(
                300,
                max(60, ENTITY_DICTIONARY_REFRESH_SECONDS),
            )
            app.logger.warning("Entity dictionary refresh failed: %s", exc)
            return previous_terms


def _merge_prompt_terms(*term_groups: list[str]) -> list[str]:
    result = []
    seen = set()
    used_chars = 0

    for group in term_groups:
        for raw_term in group:
            term = " ".join(raw_term.strip().split())
            if not term or term in seen:
                continue
            extra_chars = len(term) + (1 if result else 0)
            if len(result) >= WHISPER_PROMPT_MAX_TERMS:
                return result
            if used_chars + extra_chars > WHISPER_PROMPT_MAX_CHARS:
                return result
            seen.add(term)
            result.append(term)
            used_chars += extra_chars

    return result


def load_transcription_prompt(path: Path | None = None) -> str:
    """Merge manual vocabulary with filtered my-storage-note Entity names.

    Manual terms are first and therefore win the limited prompt budget.
    The auto layer is advisory and is omitted when no source is available.
    """
    dictionary_path = path or WHISPER_DICTIONARY
    manual_terms = load_dictionary_terms(dictionary_path)
    auto_terms = load_entity_dictionary_terms()
    return "、".join(_merge_prompt_terms(manual_terms, auto_terms))


def build_whisper_command(wav_path: Path, out_prefix: Path) -> list[str]:
    whisper_cmd = [
        WHISPER_BIN,
        "-m", WHISPER_MODEL,
        "-l", WHISPER_LANGUAGE,
        "-f", str(wav_path),
        "--vad", "-vm", WHISPER_VAD_MODEL,
        "-of", str(out_prefix),
        "--output-txt",
        "--no-prints",
    ]

    prompt = load_transcription_prompt()
    if prompt:
        whisper_cmd.extend(["--prompt", prompt])

    if not WHISPER_USE_GPU:
        whisper_cmd.append("-ng")

    return whisper_cmd


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
        whisper_cmd = build_whisper_command(wav_path, out_prefix)
        result = subprocess.run(whisper_cmd, capture_output=True)
        if result.returncode != 0:
            stderr_tail = result.stderr.decode("utf-8", errors="replace")[-2000:]
            raise RuntimeError(
                f"whisper-cli exited {result.returncode} (signal crash is common "
                f"with Metal — check WHISPER_USE_GPU): {stderr_tail}"
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
            atomic_write_text(transcript_path, transcript)

        markdown_path = NOTES_DIR / f"{recording_id}.md"
        if markdown_path.exists():
            markdown = markdown_path.read_text(encoding="utf-8")
        else:
            markdown = render_markdown(recording_id, metadata["received_at"], transcript)
            atomic_write_text(markdown_path, markdown)

        notebook_path = mynotebook_path(recording_id, metadata["received_at"])
        with _mynotebook_push_lock:
            notebook_status = push_to_mynotebook(notebook_path, markdown)

        done_path = DONE_DIR / f"{recording_id}.json"
        atomic_write_text(
            done_path,
            json.dumps(
                {
                    "recording_id": recording_id,
                    "completed_at": datetime.now(timezone.utc).isoformat(),
                    "mynotebook_path": notebook_path,
                    "mynotebook_status": notebook_status,
                },
                ensure_ascii=False,
            ),
        )
    except Exception:
        app.logger.exception("processing failed for %s", recording_id)


def _processing_worker_loop() -> None:
    while True:
        recording_id = _processing_queue.get()
        try:
            process_recording_async(recording_id)
        finally:
            _processing_queue.task_done()


threading.Thread(target=_processing_worker_loop, daemon=True).start()


def reprocess_pending_recordings() -> None:
    """Resume any recording whose upload was durably stored but never
    finished processing (e.g. this service restarted, crashed, or the Mac
    slept between the 201 response and process_recording_async completing).
    The device already renamed the file to .sent after the 201, so without
    this it would never be retried by either side."""
    for meta_path in METADATA_DIR.glob("*.json"):
        recording_id = meta_path.stem
        if (DONE_DIR / f"{recording_id}.json").exists():
            continue
        app.logger.info("resuming unfinished recording %s from startup scan", recording_id)
        _processing_queue.put(recording_id)


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
    # runs in the background (single worker, see _processing_worker_loop)
    # and its failure must not block the device.
    _processing_queue.put(recording_id)

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
    reprocess_pending_recordings()
    port = int(os.getenv("PORT", "8090"))
    app.run(host="0.0.0.0", port=port)
