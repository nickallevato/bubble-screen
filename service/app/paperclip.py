# SPDX-License-Identifier: GPL-3.0-only
"""Read-only view of the agent (and its company) in Paperclip.

ReadOnlyClient refuses every method but GET before anything leaves the
process: the agent key could write, so this module is what keeps it read-only.
gather() turns a handful of GETs into a status.Snapshot.
"""
from __future__ import annotations

import json
from datetime import datetime
from typing import Any

import httpx

from .helpers import FINISH_WINDOW_S, helpers_from
from .status import Run, Snapshot

OPEN_ISSUE_STATUSES = "todo,in_progress,in_review,blocked,backlog"
LIVE_RUN_STATUSES = {"running", "queued"}
GENERIC_STATUS = {None, "", "Receiving agent output"}
CLOSED_INTERACTION = {"answered", "expired", "cancelled", "canceled", "dismissed", "resolved", "rejected"}
LOG_TAIL_BYTES = 65536
INTERACTIONS_TTL_S = 300  # re-read every issue's interactions at least this often
AGENTS_TTL_S = 300        # names and models change rarely
TITLE_CACHE_MAX = 200
FRESH_ASK_S = 48 * 3600   # only asks this new make him wave; older ones just count as waiting


class ReadOnlyClient:
    def __init__(self, base: str, token: str, transport: httpx.AsyncBaseTransport | None = None, timeout: float = 10.0):
        self._http = httpx.AsyncClient(base_url=base.rstrip("/"), transport=transport, timeout=timeout,
                                       headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})

    async def request(self, method: str, path: str, params: dict | None = None) -> Any:
        if method.upper() != "GET":
            raise PermissionError(f"bubble-screen is read-only; refused {method} {path}")
        r = await self._http.request("GET", path, params=params)
        r.raise_for_status()
        return r.json()

    async def get(self, path: str, params: dict | None = None) -> Any:
        return await self.request("GET", path, params)

    async def aclose(self) -> None:
        await self._http.aclose()


def _ts(value: str | None) -> float | None:
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _items(body: Any, *keys: str) -> list:
    if isinstance(body, list):
        return body
    if isinstance(body, dict):
        for k in keys + ("items", "data"):
            if isinstance(body.get(k), list):
                return body[k]
    return []


def parse_log_tail(ndjson: str) -> float | None:
    """Unix time of the last tool call in a run-log slice.

    Records are {ts, stream, chunk}; `chunk` is a raw stdout read, so one
    stream-json line can span several records. Chunks are joined per stream and
    each completed line takes the ts of the record that finished it. The first
    record may be cut mid-way by the byte offset; anything odd is skipped."""
    last, pending = None, {}
    for line in ndjson.splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if not isinstance(rec, dict) or not isinstance(rec.get("chunk"), str):
            continue
        stream = rec.get("stream") or "stdout"
        buf = pending.get(stream, "") + rec["chunk"]
        *done, pending[stream] = buf.split("\n")
        for inner_line in done:
            if '"tool_use"' not in inner_line:
                continue
            try:
                inner = json.loads(inner_line)
                content = inner.get("message", {}).get("content") or []
            except (ValueError, AttributeError, TypeError):
                continue
            if isinstance(content, list) and any(isinstance(b, dict) and b.get("type") == "tool_use" for b in content):
                ts = rec.get("ts")
                last = (_ts(ts) if isinstance(ts, str) else None) or last
    return last


def _is_open(inter: dict) -> bool:
    if inter.get("resolvedAt") or inter.get("dismissedAt") or inter.get("addresseeAgentId"):
        return False
    return (inter.get("status") or "").lower() not in CLOSED_INTERACTION


async def _open_asks(client: ReadOnlyClient, issues: list[dict], cache: dict, now: float) -> dict[str, list[tuple[float | None, str]]]:
    """Open interactions per issue, as (created unix time, title). An issue's
    interactions are re-fetched only when its lastActivityAt changes."""
    seen = cache.setdefault("interactions", {})
    out = {}
    for iss in issues:
        iid, activity = iss["id"], iss.get("lastActivityAt") or iss.get("updatedAt")
        hit = seen.get(iid)
        # Re-read when the issue moved, when it had open asks (answering one may
        # not bump lastActivityAt, and a stale ask would keep him waving), or when
        # the TTL lapses.
        if hit is None or hit[0] != activity or hit[1] or now - hit[2] > INTERACTIONS_TTL_S:
            items = _items(await client.get(f"/issues/{iid}/interactions"), "interactions")
            hit = seen[iid] = (activity, [(_ts(x.get("createdAt")), x.get("title") or iss.get("title") or "")
                                          for x in items if isinstance(x, dict) and _is_open(x)], now)
        out[iid] = hit[1]
    for gone in set(seen) - {i["id"] for i in issues}:
        del seen[gone]
    return out


async def gather(client: ReadOnlyClient, agent_id: str, company_id: str, now: float, cache: dict | None = None) -> Snapshot:
    cache = {} if cache is None else cache
    runs = [r for r in _items(await client.get(f"/companies/{company_id}/heartbeat-runs",
                                               {"agentId": agent_id, "limit": 10, "summary": "true"}), "runs")
            if r.get("agentId") in (None, agent_id)]
    issues = [i for i in _items(await client.get(f"/companies/{company_id}/issues",
                                                 {"assigneeAgentId": agent_id, "status": OPEN_ISSUE_STATUSES}), "issues")
              if i.get("assigneeAgentId") == agent_id]
    approvals = _items(await client.get(f"/companies/{company_id}/approvals", {"status": "pending"}), "approvals")
    titles = cache.setdefault("titles", {})   # kept across polls: closed issues' titles never change
    titles.update({i["id"]: i.get("title") for i in issues})
    if len(titles) > TITLE_CACHE_MAX:
        for k in list(titles)[: len(titles) - TITLE_CACHE_MAX]:
            del titles[k]

    async def title_of(iid: str | None) -> str | None:
        if not iid:
            return None
        if iid not in titles:
            try:
                titles[iid] = (await client.get(f"/issues/{iid}")).get("title")
            except httpx.HTTPError:
                titles[iid] = None
        return titles[iid]

    def as_run(r: dict, title: str | None) -> Run:
        return Run(r["id"], r.get("status") or "", _ts(r.get("startedAt")), _ts(r.get("finishedAt")), title)

    live = [r for r in runs if r.get("status") in LIVE_RUN_STATUSES]
    finished = sorted((r for r in runs if r.get("finishedAt")), key=lambda r: _ts(r["finishedAt"]) or 0, reverse=True)
    successes = [_ts(r["finishedAt"]) for r in finished if r.get("status") == "succeeded"]

    snap = Snapshot(now=now)
    snap.live_runs = [as_run(r, await title_of(r.get("issueId"))) for r in live]
    if finished:
        top = finished[0]
        snap.last_finished = as_run(top, await title_of(top.get("issueId")) if top.get("status") == "failed" else None)
    snap.last_success_at = max(successes) if successes else None

    if live:
        current = max(live, key=lambda r: _ts(r.get("startedAt")) or 0)
        detail = await client.get(f"/heartbeat-runs/{current['id']}")
        # What the agent last said beats Paperclip's own progress text; the
        # runtime's default "Receiving agent output" says nothing at all.
        status = detail.get("currentStatusMessage")
        snap.status_message = detail.get("lastAssistantSnippet") or (status if status not in GENERIC_STATUS else None)
        if detail.get("currentToolName") and detail.get("lastEventAt"):
            snap.last_tool_at = _ts(detail["lastEventAt"])
        else:
            size = int(detail.get("logBytes") or current.get("logBytes") or 0)
            log = await client.get(f"/heartbeat-runs/{current['id']}/log",
                                   {"offset": max(0, size - LOG_TAIL_BYTES), "limitBytes": LOG_TAIL_BYTES})
            snap.last_tool_at = parse_log_tail((log or {}).get("content") or "")

    # Helpers: every other agent in the company, from one company-wide run list.
    agents_hit = cache.get("agents")
    if agents_hit is None or now - agents_hit[0] > AGENTS_TTL_S:
        roster = _items(await client.get(f"/companies/{company_id}/agents"), "agents")
        agents_hit = cache["agents"] = (now, {a["id"]: a for a in roster if isinstance(a, dict) and a.get("id")})
    company = _items(await client.get(f"/companies/{company_id}/heartbeat-runs", {"limit": 20, "summary": "true"}), "runs")
    for r in company:
        recent = r.get("status") == "running" or (r.get("finishedAt") and now - (_ts(r["finishedAt"]) or 0) <= FINISH_WINDOW_S)
        if recent and r.get("agentId") != agent_id:
            await title_of(r.get("issueId"))
    said = cache.setdefault("run_lines", {})
    for r in company:
        if (r.get("status") == "succeeded" and r.get("agentId") != agent_id and r.get("issueId") and r["id"] not in said
                and now - (_ts(r.get("finishedAt")) or 0) <= FINISH_WINDOW_S):
            try:
                comments = _items(await client.get(f"/issues/{r['issueId']}/comments", {"limit": 5}), "comments")
                said[r["id"]] = next((c.get("body") or "" for c in comments
                                      if isinstance(c, dict) and c.get("createdByRunId") == r["id"]), "")
            except httpx.HTTPError:
                said[r["id"]] = ""
    for old in list(said)[:-50]:
        del said[old]
    snap.agent_name = (agents_hit[1].get(agent_id) or {}).get("name")
    snap.helpers = helpers_from(company, agents_hit[1], titles, now, agent_id, run_lines=said)

    per_issue = await _open_asks(client, issues, cache, now)
    mine = [a for a in approvals if a.get("requestedByAgentId") == agent_id
            and (a.get("status") or "pending").lower() == "pending"]
    every = [a for asks in per_issue.values() for a in asks]
    every += [(_ts(a.get("createdAt")), a.get("title") or (a.get("payload") or {}).get("title") or "Approval") for a in mine]
    fresh = [a for a in every if a[0] is None or now - a[0] <= FRESH_ASK_S]
    snap.asks = len(fresh)
    snap.ask_title = max(fresh, key=lambda a: a[0] or now)[1] if fresh else None
    snap.waiting = sum(1 for i in issues if i.get("status") in ("blocked", "in_review") or per_issue.get(i["id"])) + len(mine)
    return snap
