import base64
import json
import os
from datetime import date, datetime, timedelta
from pathlib import Path

import requests


DEFAULT_ENTITY_KINDS = ("person", "organization", "place", "concept")


def env_bool(name: str, default: bool = False) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("", "0", "false", "no", "off")


def _parse_date(value: str | None) -> date | None:
    if not value:
        return None
    try:
        return datetime.strptime(value[:10], "%Y-%m-%d").date()
    except (TypeError, ValueError):
        return None


def _clean_term(value) -> str:
    if not isinstance(value, str):
        return ""
    term = " ".join(value.strip().split())
    if len(term) < 2:
        return ""
    if not any(ch.isalnum() for ch in term):
        return ""
    return term


def select_entity_terms(
    index_data: dict,
    *,
    today: date | None = None,
    kinds: tuple[str, ...] = DEFAULT_ENTITY_KINDS,
    min_mentions: int = 2,
    recent_days: int = 180,
    include_aliases: bool = False,
    max_terms: int = 60,
    max_chars: int = 280,
) -> list[str]:
    """Select high-value Entity names for a short Whisper initial prompt.

    Recent one-off entities are kept because newly introduced people/projects
    are exactly the words Whisper benefits from. Older entities need repeated
    mentions. Canonical names are preferred; aliases are opt-in because the
    warehouse can contain extraction noise in alias lists.
    """
    today = today or date.today()
    cutoff = today - timedelta(days=max(0, recent_days))
    allowed_kinds = set(kinds)
    candidates = []

    for entity in index_data.get("entities", []):
        if entity.get("kind") not in allowed_kinds:
            continue

        name = _clean_term(entity.get("name"))
        if not name:
            continue

        try:
            mention_count = int(entity.get("mention_count", 0) or 0)
        except (TypeError, ValueError):
            mention_count = 0

        last_seen = _parse_date(entity.get("last_seen"))
        is_recent = last_seen is not None and last_seen >= cutoff
        if mention_count < min_mentions and not is_recent:
            continue

        # Prefer currently active vocabulary, then stronger historical signal.
        candidates.append(
            (
                1 if is_recent else 0,
                last_seen or date.min,
                mention_count,
                name,
                entity,
            )
        )

    candidates.sort(key=lambda item: (item[0], item[1], item[2], item[3]), reverse=True)

    result: list[str] = []
    seen: set[str] = set()
    used_chars = 0

    def add_term(term: str) -> bool:
        nonlocal used_chars
        term = _clean_term(term)
        if not term or term in seen:
            return True
        extra_chars = len(term) + (1 if result else 0)  # Japanese delimiter "、"
        if len(result) >= max_terms or used_chars + extra_chars > max_chars:
            return False
        seen.add(term)
        result.append(term)
        used_chars += extra_chars
        return True

    for _recent, _last_seen, _mentions, name, entity in candidates:
        if not add_term(name):
            break
        if include_aliases:
            for alias in entity.get("aliases", []):
                if not add_term(alias):
                    return result

    return result


def _candidate_local_roots(anchor_file: Path) -> list[Path]:
    roots: list[Path] = []
    explicit = os.getenv("MY_STORAGE_NOTE_PATH", "").strip()
    if explicit:
        roots.append(Path(explicit).expanduser())

    # Common layout:
    #   .../GitHub/sticks3-voice-capture/local-receiver/entity_dictionary.py
    #   .../GitHub/my-storage-note/
    resolved = anchor_file.resolve()
    if len(resolved.parents) >= 3:
        roots.append(resolved.parents[2] / "my-storage-note")

    # De-duplicate while preserving priority.
    unique = []
    seen = set()
    for root in roots:
        key = str(root)
        if key not in seen:
            seen.add(key)
            unique.append(root)
    return unique


def load_entity_index(anchor_file: Path) -> tuple[dict | None, str]:
    """Load memory/entities/index.json from local clone first, then GitHub.

    Returns (None, "unavailable") when no readable source is configured.
    Network/auth failures raise so the caller can keep a previous cache while
    surfacing a warning instead of silently replacing good vocabulary.
    """
    rel_path = Path("memory/entities/index.json")
    for root in _candidate_local_roots(anchor_file):
        index_path = root / rel_path
        if index_path.is_file():
            return json.loads(index_path.read_text(encoding="utf-8")), f"local:{index_path}"

    token = (
        os.getenv("KNOWLEDGE_GITHUB_TOKEN", "").strip()
        or os.getenv("GITHUB_TOKEN", "").strip()
    )
    if not token:
        return None, "unavailable"

    repo = os.getenv("ENTITY_DICTIONARY_REPO", "plzsayyes3/my-storage-note").strip()
    branch = os.getenv("ENTITY_DICTIONARY_BRANCH", "main").strip()
    url = f"https://api.github.com/repos/{repo}/contents/{rel_path.as_posix()}"
    response = requests.get(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
        params={"ref": branch},
        timeout=20,
    )
    if response.status_code != 200:
        raise RuntimeError(
            f"Entity dictionary GitHub fetch failed: "
            f"{response.status_code} {response.text[:300]}"
        )

    payload = response.json()
    encoded = payload.get("content", "")
    if payload.get("encoding") != "base64" or not encoded:
        raise RuntimeError("Entity dictionary GitHub response did not contain base64 content")

    decoded = base64.b64decode(encoded).decode("utf-8")
    return json.loads(decoded), f"github:{repo}@{branch}:{rel_path.as_posix()}"


def load_terms_from_environment(anchor_file: Path) -> tuple[list[str], str]:
    index_data, source = load_entity_index(anchor_file)
    if index_data is None:
        return [], source

    kinds_raw = os.getenv(
        "ENTITY_DICTIONARY_KINDS",
        ",".join(DEFAULT_ENTITY_KINDS),
    )
    kinds = tuple(part.strip() for part in kinds_raw.split(",") if part.strip())

    terms = select_entity_terms(
        index_data,
        kinds=kinds,
        min_mentions=int(os.getenv("ENTITY_DICTIONARY_MIN_MENTIONS", "2")),
        recent_days=int(os.getenv("ENTITY_DICTIONARY_RECENT_DAYS", "180")),
        include_aliases=env_bool("ENTITY_DICTIONARY_INCLUDE_ALIASES", False),
        max_terms=int(os.getenv("ENTITY_DICTIONARY_MAX_TERMS", "60")),
        max_chars=int(os.getenv("ENTITY_DICTIONARY_MAX_CHARS", "280")),
    )
    return terms, source
