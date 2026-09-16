"""API-contract tests for the local receiver.

Runs entirely offline: transcribe_ogg and push_to_mynotebook are monkeypatched
so no ffmpeg/whisper-cli binary or GitHub network access is required. This
covers the request/response contract (auth, idempotency, validation) plus the
startup reprocessing scan; it does not exercise real transcription quality —
that still needs a manual run against whisper-cli (see README).
"""
import importlib
import time

import pytest

DEVICE_TOKEN = "test-token-not-a-real-secret"


@pytest.fixture
def app_module(tmp_path, monkeypatch):
    monkeypatch.setenv("STICKS3_DATA_DIR", str(tmp_path))
    monkeypatch.setenv("DEVICE_TOKEN", DEVICE_TOKEN)
    monkeypatch.delenv("GITHUB_TOKEN", raising=False)

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
