"""API-contract tests for the local receiver.

Runs entirely offline: transcribe_ogg and push_to_mynotebook are monkeypatched
so no ffmpeg/whisper-cli binary or GitHub network access is required. This
covers the request/response contract (auth, idempotency, validation) plus the
startup reprocessing scan; it does not exercise real transcription quality —
that still needs a manual run against whisper-cli (see README).
"""
import importlib
import threading
import time
from datetime import date

import pytest

import entity_dictionary

DEVICE_TOKEN = "test-token-not-a-real-secret"


@pytest.fixture
def app_module(tmp_path, monkeypatch):
    monkeypatch.setenv("STICKS3_DATA_DIR", str(tmp_path))
    monkeypatch.setenv("DEVICE_TOKEN", DEVICE_TOKEN)
    monkeypatch.delenv("GITHUB_TOKEN", raising=False)
    monkeypatch.delenv("KNOWLEDGE_GITHUB_TOKEN", raising=False)
    monkeypatch.setenv("ENTITY_DICTIONARY_ENABLED", "0")

    import app as app_module  # noqa: PLC0415
    importlib.reload(app_module)

    monkeypatch.setattr(app_module, "transcribe_ogg", lambda ogg_path: "テスト文字起こし")
    monkeypatch.setattr(app_module, "push_to_mynotebook", lambda path, markdown: "created")

    return app_module


@pytest.fixture
def client(app_module):
    app_module.app.testing = True
    return app_module.app.test_client()


def auth_headers(recording_id: str, token: str = DEVICE_TOKEN) -> dict:
    return {
        "Authorization": f"Bearer {token}",
        "X-Recording-ID": recording_id,
        "Content-Type": "audio/ogg",
    }


def wait_for_done(app_module, recording_id: str, timeout: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    done_path = app_module.DONE_DIR / f"{recording_id}.json"
    while time.monotonic() < deadline:
        if done_path.exists():
            return True
        time.sleep(0.02)
    return False


def test_missing_auth_rejected(client):
    resp = client.post("/v1/recordings", data=b"OggS...", headers={
        "X-Recording-ID": "rec-001",
        "Content-Type": "audio/ogg",
    })
    assert resp.status_code == 401


def test_wrong_token_rejected(client):
    resp = client.post(
        "/v1/recordings", data=b"OggS...",
        headers=auth_headers("rec-002", token="wrong"),
    )
    assert resp.status_code == 403


def test_invalid_recording_id_rejected(client):
    resp = client.post(
        "/v1/recordings", data=b"OggS...",
        headers=auth_headers("../../etc/passwd"),
    )
    assert resp.status_code == 400


def test_unsupported_content_type_rejected(client):
    headers = auth_headers("rec-003")
    headers["Content-Type"] = "text/plain"
    resp = client.post("/v1/recordings", data=b"not audio", headers=headers)
    assert resp.status_code == 415


def test_empty_body_rejected(client):
    resp = client.post("/v1/recordings", data=b"", headers=auth_headers("rec-004"))
    assert resp.status_code == 400


def test_new_upload_stored_and_processed(client, app_module):
    resp = client.post("/v1/recordings", data=b"OggS-fake-audio", headers=auth_headers("rec-005"))
    assert resp.status_code == 201
    body = resp.get_json()
    assert body["status"] == "stored"

    assert (app_module.RECORDINGS_DIR / "rec-005.ogg").exists()
    assert wait_for_done(app_module, "rec-005")
    markdown = (app_module.NOTES_DIR / "rec-005.md").read_text(encoding="utf-8")
    assert "テスト文字起こし" in markdown


def test_duplicate_upload_same_audio_returns_200(client, app_module):
    data = b"OggS-identical-bytes"
    first = client.post("/v1/recordings", data=data, headers=auth_headers("rec-006"))
    assert first.status_code == 201

    second = client.post("/v1/recordings", data=data, headers=auth_headers("rec-006"))
    assert second.status_code == 200
    assert second.get_json()["status"] == "already_stored"

    # Every POST kicks off a background processing thread even on the
    # duplicate path; wait for both to finish before the monkeypatched
    # transcribe_ogg/push_to_mynotebook get torn down, or a daemon thread
    # can outlive the test and hit the real (unpatched) functions.
    assert wait_for_done(app_module, "rec-006")


def test_same_id_different_audio_returns_409(client, app_module):
    first = client.post("/v1/recordings", data=b"OggS-version-a", headers=auth_headers("rec-007"))
    assert first.status_code == 201

    second = client.post("/v1/recordings", data=b"OggS-version-b-different", headers=auth_headers("rec-007"))
    assert second.status_code == 409

    assert wait_for_done(app_module, "rec-007")


def test_recordings_are_transcribed_one_at_a_time(client, app_module):
    """A batch Wi-Fi Sync uploads several recordings back to back; whisper-cli
    is CPU/GPU-heavy, so they must be processed by a single worker, not one
    thread per upload racing on the same machine."""
    lock = threading.Lock()
    state = {"concurrent": 0, "max_concurrent": 0}

    def tracked_transcribe(ogg_path):
        with lock:
            state["concurrent"] += 1
            state["max_concurrent"] = max(state["max_concurrent"], state["concurrent"])
        time.sleep(0.1)
        with lock:
            state["concurrent"] -= 1
        return "テスト文字起こし"

    app_module.transcribe_ogg = tracked_transcribe

    for i in range(4):
        resp = client.post(
            "/v1/recordings", data=f"OggS-batch-{i}".encode(),
            headers=auth_headers(f"rec-batch-{i}"),
        )
        assert resp.status_code == 201

    for i in range(4):
        assert wait_for_done(app_module, f"rec-batch-{i}", timeout=5.0)

    assert state["max_concurrent"] == 1


def test_reprocess_pending_recordings_resumes_unfinished(app_module):
    recording_id = "rec-crash-before-done"
    app_module.store_recording_durably(recording_id, b"OggS-resumed")
    app_module.write_metadata(recording_id, {
        "recording_id": recording_id,
        "sha256": "irrelevant-for-this-test",
        "received_at": "2026-09-16T00:00:00+00:00",
        "source": "sticks3",
    })
    assert not (app_module.DONE_DIR / f"{recording_id}.json").exists()

    app_module.reprocess_pending_recordings()

    assert wait_for_done(app_module, recording_id)


def test_load_transcription_prompt_ignores_comments_blank_and_duplicates(app_module, tmp_path):
    dictionary = tmp_path / "dictionary.txt"
    dictionary.write_text(
        "# preferred vocabulary\n"
        "StickS3\n"
        "\n"
        "TaskLiner\n"
        "StickS3\n"
        "ON HAND\n",
        encoding="utf-8",
    )

    assert app_module.load_transcription_prompt(dictionary) == "StickS3、TaskLiner、ON HAND"


def test_missing_transcription_dictionary_disables_prompt(app_module, tmp_path, monkeypatch):
    missing = tmp_path / "missing-dictionary.txt"
    monkeypatch.setattr(app_module, "WHISPER_DICTIONARY", missing)

    cmd = app_module.build_whisper_command(
        tmp_path / "audio.wav",
        tmp_path / "transcript",
    )

    assert "--prompt" not in cmd


def test_whisper_command_includes_dictionary_prompt(app_module, tmp_path, monkeypatch):
    dictionary = tmp_path / "transcription-dictionary.txt"
    dictionary.write_text("StickS3\nM5Stack\nObsidian\n", encoding="utf-8")
    monkeypatch.setattr(app_module, "WHISPER_DICTIONARY", dictionary)

    cmd = app_module.build_whisper_command(
        tmp_path / "audio.wav",
        tmp_path / "transcript",
    )

    prompt_index = cmd.index("--prompt")
    assert cmd[prompt_index + 1] == "StickS3、M5Stack、Obsidian"


def test_entity_selection_keeps_recent_one_off_and_repeated_history():
    index_data = {
        "entities": [
            {
                "kind": "person",
                "name": "新しい先生",
                "aliases": [],
                "mention_count": 1,
                "last_seen": "2026-09-10",
            },
            {
                "kind": "concept",
                "name": "Obsidian",
                "aliases": ["オブシディアン"],
                "mention_count": 20,
                "last_seen": "2021-01-01",
            },
            {
                "kind": "organization",
                "name": "古い一回だけ",
                "aliases": [],
                "mention_count": 1,
                "last_seen": "2020-01-01",
            },
            {
                "kind": "organization",
                "name": "田",
                "aliases": [],
                "mention_count": 99,
                "last_seen": "2026-09-10",
            },
            {
                "kind": "event",
                "name": "運動会",
                "aliases": [],
                "mention_count": 99,
                "last_seen": "2026-09-10",
            },
        ]
    }

    terms = entity_dictionary.select_entity_terms(
        index_data,
        today=date(2026, 9, 18),
        recent_days=180,
        min_mentions=2,
        max_terms=20,
        max_chars=200,
    )

    assert "新しい先生" in terms
    assert "Obsidian" in terms
    assert "古い一回だけ" not in terms
    assert "田" not in terms
    assert "運動会" not in terms


def test_entity_aliases_are_opt_in():
    index_data = {
        "entities": [
            {
                "kind": "concept",
                "name": "Obsidian",
                "aliases": ["オブシディアン", "シリアン"],
                "mention_count": 3,
                "last_seen": "2026-09-17",
            }
        ]
    }

    default_terms = entity_dictionary.select_entity_terms(
        index_data,
        today=date(2026, 9, 18),
        max_chars=200,
    )
    alias_terms = entity_dictionary.select_entity_terms(
        index_data,
        today=date(2026, 9, 18),
        include_aliases=True,
        max_chars=200,
    )

    assert default_terms == ["Obsidian"]
    assert alias_terms == ["Obsidian", "オブシディアン", "シリアン"]


def test_entity_selection_respects_prompt_budget():
    index_data = {
        "entities": [
            {
                "kind": "concept",
                "name": name,
                "aliases": [],
                "mention_count": 3,
                "last_seen": "2026-09-17",
            }
            for name in ["Alpha", "Bravo", "Charlie", "Delta"]
        ]
    }

    terms = entity_dictionary.select_entity_terms(
        index_data,
        today=date(2026, 9, 18),
        max_terms=2,
        max_chars=100,
    )

    assert len(terms) == 2


def test_manual_dictionary_precedes_auto_terms(app_module, tmp_path, monkeypatch):
    dictionary = tmp_path / "dictionary.txt"
    dictionary.write_text("TaskLiner\nObsidian\n", encoding="utf-8")
    monkeypatch.setattr(
        app_module,
        "load_entity_dictionary_terms",
        lambda force=False: ["Obsidian", "扇こころ保育園", "足立区"],
    )

    prompt = app_module.load_transcription_prompt(dictionary)

    assert prompt == "TaskLiner、Obsidian、扇こころ保育園、足立区"


def test_entity_dictionary_failure_keeps_previous_cache(app_module, monkeypatch):
    monkeypatch.setattr(app_module, "ENTITY_DICTIONARY_ENABLED", True)
    monkeypatch.setattr(app_module, "ENTITY_DICTIONARY_REFRESH_SECONDS", 60)
    app_module._entity_dictionary_cache.update(
        terms=["既存語"],
        source="test",
        expires_at=0.0,
    )

    def fail(_anchor):
        raise RuntimeError("temporary source failure")

    monkeypatch.setattr(app_module.entity_dictionary, "load_terms_from_environment", fail)

    assert app_module.load_entity_dictionary_terms(force=True) == ["既存語"]
