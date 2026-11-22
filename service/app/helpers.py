# SPDX-License-Identifier: GPL-3.0-only
"""The other agents in the company, as the main agent's off-screen helpers.

Pure: runs + agents + issue titles in, the `helpers` list for /api/now out.
A helper is `working` while a run is running, and `done`/`failed` for a short
window after it finishes, long enough for the device to notice and play the
pop-in once.
"""
from __future__ import annotations

import re
import zlib
from datetime import datetime

from .status import ascii_upper

FINISH_WINDOW_S = 120
MAX_HELPERS = 4


def _ts(value: str | None) -> float | None:
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp() if value else None
    except (ValueError, AttributeError):
        return None


def model_class(model: str | None) -> str:
    m = (model or "").lower()
    return "opus" if "opus" in m else "haiku" if "haiku" in m else "sonnet"


def side_for(agent_id: str) -> str:
    return "L" if zlib.crc32(agent_id.encode()) & 1 else "R"


def first_line(md: str | None) -> str:
    """First line of a Markdown comment, without the Markdown, folded for the device."""
    for raw in (md or "").splitlines():
        line = re.sub(r"^[#>\-*\s]+", "", raw)
        line = re.sub(r"[*_`~\[\]()]", "", line).strip()
        if line:
            return ascii_upper(line)
    return ""


def helpers_from(runs: list[dict], agents: dict[str, dict], titles: dict[str, str | None],
                 now: float, main_agent_id: str, run_lines: dict[str, str] | None = None) -> list[dict]:
    """One entry per helper agent (its newest relevant run), newest first."""
    best: dict[str, tuple[float, dict]] = {}
    for r in runs:
        aid = r.get("agentId")
        if not aid or aid == main_agent_id:
            continue
        status = r.get("status")
        if status == "running":
            state, at = "working", _ts(r.get("startedAt"))
        elif status in ("succeeded", "failed"):
            at = _ts(r.get("finishedAt"))
            if at is None or now - at > FINISH_WINDOW_S:
                continue
            state = "done" if status == "succeeded" else "failed"
        else:
            continue
        at = at or now
        if aid not in best or at > best[aid][0]:
            best[aid] = (at, {**r, "_state": state})

    out = []
    for aid, (at, r) in sorted(best.items(), key=lambda kv: kv[1][0], reverse=True)[:MAX_HELPERS]:
        agent = agents.get(aid) or {}
        title = titles.get(r.get("issueId")) or ""
        said = first_line((run_lines or {}).get(r.get("id"))) if r["_state"] == "done" else ""
        line = said or (title if r["_state"] != "failed" else (f"COULDN'T FINISH: {title}" if title else "COULDN'T FINISH"))
        out.append({"id": aid[:8], "name": ascii_upper(agent.get("name") or "Helper", 12),
                    "model": model_class((agent.get("adapterConfig") or {}).get("model")),
                    "side": side_for(aid), "state": r["_state"], "line": ascii_upper(line), "at": int(at)})
    return out
