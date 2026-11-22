# SPDX-License-Identifier: GPL-3.0-only
"""Reduce what Paperclip says about the agent to the one status bubble-screen shows.

Pure: no I/O. `gather()` in paperclip.py builds the Snapshot; the poll loop
calls reduce() then StateClock.stamp().
"""
from __future__ import annotations

import re
import unicodedata
from dataclasses import dataclass, field

WORK_WINDOW_S = 15          # a tool call this recent means "working"
ERROR_WINDOW_S = 30 * 60    # a failure this recent (and unsuperseded) means "stuck"
TITLE_LIMIT = 60

# The device's 5x7 font: uppercase, digits, and these.
_FONT_EXTRA = " :.-?!/',$&#%"
_DASHES = dict.fromkeys(map(ord, "‐‑‒–—―−"), "-")
_QUOTES = {ord("‘"): "'", ord("’"): "'", ord("“"): "", ord("”"): "", ord('"'): ""}

_ICONS = [
    ("cal", ("book", "appointment", "schedule", "calendar", "reschedule", "meeting")),
    ("cart", ("order", "buy", "cart", "grocer", "shop", "purchase")),
    ("mail", ("email", "e-mail", "reply", "message", "send", "write to")),
]


@dataclass
class Run:
    id: str
    status: str
    started_at: float | None
    finished_at: float | None
    issue_title: str | None


@dataclass
class Snapshot:
    now: float
    live_runs: list[Run] = field(default_factory=list)
    last_finished: Run | None = None
    last_success_at: float | None = None
    last_tool_at: float | None = None
    status_message: str | None = None
    asks: int = 0
    ask_title: str | None = None
    waiting: int = 0
    next_check: str | None = None
    helpers: list[dict] = field(default_factory=list)
    agent_name: str | None = None


def ascii_upper(text: str | None, limit: int = TITLE_LIMIT) -> str:
    """Fold to the device font: ASCII, uppercase, known punctuation, <= limit."""
    if not text:
        return ""
    t = text.translate(_DASHES).translate(_QUOTES)
    t = unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode()
    t = "".join(c for c in t.upper() if c.isalnum() or c in _FONT_EXTRA)
    t = re.sub(r"\s+", " ", t).strip()
    return t[:limit].rstrip()


def icon_for(title: str | None) -> str:
    low = (title or "").lower()
    for icon, words in _ICONS:
        if any(w in low for w in words):
            return icon
    return "list"


def _current_run(s: Snapshot) -> Run | None:
    if not s.live_runs:
        return None
    return max(s.live_runs, key=lambda r: r.started_at or 0)


def reduce(s: Snapshot) -> dict:
    run = _current_run(s)
    failed = s.last_finished if (s.last_finished and s.last_finished.status == "failed") else None
    stuck = bool(
        failed and failed.finished_at and s.now - failed.finished_at <= ERROR_WINDOW_S
        and not (s.last_success_at and s.last_success_at > failed.finished_at)
    )

    if s.asks > 0:
        state = "ask"
    elif run is None and stuck:
        state = "error"
    elif run is not None:
        state = "work" if (s.last_tool_at and s.now - s.last_tool_at <= WORK_WINDOW_S) else "think"
    else:
        state = "sleep"

    subject = run or (failed if stuck else None)
    title = s.ask_title if state == "ask" else (subject.issue_title if subject else None)
    return {
        "state": state,
        "title": ascii_upper(title),
        "icon": icon_for(title),
        "detail": ascii_upper(s.status_message) if run else "",
        "waiting": s.waiting,
        "next": s.next_check,
        "helpers": s.helpers,
        "agent": ascii_upper(s.agent_name, 12) or "AGENT",
    }


class StateClock:
    """Adds `since`: when the current state began. Unchanged while state holds."""

    def __init__(self) -> None:
        self._state: str | None = None
        self._since: int = 0

    def stamp(self, status: dict, now: float) -> dict:
        if status["state"] != self._state:
            self._state, self._since = status["state"], int(now)
        return {**status, "since": self._since}
