# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path

import httpx
import pytest

from app.paperclip import ReadOnlyClient, gather, parse_log_tail

FIX = Path(__file__).parent / "fixtures"
AID = "00000000-0000-4000-8000-0000000000a1"
CID = "00000000-0000-4000-8000-0000000000c1"
NOW = 1_795_400_000.0  # 2026-11-22T02:13:20Z


def iso(t: float) -> str:
    from datetime import UTC, datetime
    return datetime.fromtimestamp(t, UTC).isoformat().replace("+00:00", "Z")


def log_record(ts: float, inner: dict) -> str:
    return json.dumps({"ts": iso(ts), "stream": "stdout", "chunk": json.dumps(inner) + "\n"})


def tool_use(name="Bash") -> dict:
    return {"type": "assistant", "message": {"content": [{"type": "tool_use", "name": name, "input": {}}]}}


def text_msg() -> dict:
    return {"type": "assistant", "message": {"content": [{"type": "text", "text": "thinking about it"}]}}


class Fake:
    """Routes Paperclip paths to canned JSON and records every request."""

    def __init__(self, routes: dict):
        self.routes, self.seen = routes, []

    def __call__(self, req: httpx.Request) -> httpx.Response:
        self.seen.append(req)
        path = req.url.path.removeprefix("/api")
        if path not in self.routes:
            return httpx.Response(404, json={"error": "not found"})
        body = self.routes[path]
        return httpx.Response(200, json=body(req) if callable(body) else body)

    def paths(self):
        return [r.url.path.removeprefix("/api") for r in self.seen]


def issue(iid, title, status="in_progress", assignee=AID, activity="2026-11-21T20:00:00Z"):
    return {"id": iid, "identifier": iid.upper(), "title": title, "status": status,
            "assigneeAgentId": assignee, "lastActivityAt": activity}


def base_routes(runs=None, issues=None, approvals=None, inters=None, extra=None):
    r = {
        f"/companies/{CID}/heartbeat-runs": runs if runs is not None else json.loads((FIX / "runs_idle.json").read_text()),
        f"/companies/{CID}/issues": issues if issues is not None else [],
        f"/companies/{CID}/approvals": approvals if approvals is not None else [],
        f"/companies/{CID}/agents": [],
    }
    for iid, items in (inters or {}).items():
        r[f"/issues/{iid}/interactions"] = items
    r.update(extra or {})
    return r


def client_for(fake: Fake) -> ReadOnlyClient:
    return ReadOnlyClient("https://paperclip.test/api", "tok", transport=httpx.MockTransport(fake))


async def test_non_get_refused():
    fake = Fake({})
    with pytest.raises(PermissionError):
        await client_for(fake).request("POST", "/agents/x/keys")
    assert fake.seen == []


def live_run(rid="live1", iid="iss1", log_bytes=3_582_317):
    return {"id": rid, "agentId": AID, "status": "running", "startedAt": iso(NOW - 120), "finishedAt": None,
            "issueId": iid, "error": None, "logBytes": log_bytes}


def live_detail(rid="live1", tool=None, event_at=None, message=None, log_bytes=3_582_317):
    return {**live_run(rid, log_bytes=log_bytes), "currentToolName": tool, "lastEventAt": event_at,
            "currentStatusMessage": message}


async def test_gather_live_run():
    log = "artial line from the middle\n" + log_record(NOW - 30, tool_use()) + "\n" + log_record(NOW - 5, text_msg()) + "\n"
    fake = Fake(base_routes(
        runs=[live_run()], issues=[issue("iss1", "Book dentist appointment")], inters={"iss1": []},
        extra={"/heartbeat-runs/live1": live_detail(message="Calling the office"),
               "/heartbeat-runs/live1/log": {"content": log, "nextOffset": "3582317"}}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.live_runs[0].issue_title == "Book dentist appointment"
    assert s.last_tool_at == pytest.approx(NOW - 30)
    assert s.status_message == "Calling the office"


async def test_log_tail_offset():
    fake = Fake(base_routes(
        runs=[live_run()], issues=[issue("iss1", "x")], inters={"iss1": []},
        extra={"/heartbeat-runs/live1": live_detail(),
               "/heartbeat-runs/live1/log": {"content": "{broken\n", "nextOffset": "0"}}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    log_req = [r for r in fake.seen if r.url.path.endswith("/log")][0]
    assert log_req.url.params["offset"] == "3516781"
    assert log_req.url.params["limitBytes"] == "65536"
    assert s.last_tool_at is None


async def test_live_status_fields_skip_log():
    fake = Fake(base_routes(
        runs=[live_run()], issues=[issue("iss1", "x")], inters={"iss1": []},
        extra={"/heartbeat-runs/live1": live_detail(tool="Bash", event_at=iso(NOW - 3))}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.last_tool_at == pytest.approx(NOW - 3)
    assert not any(p.endswith("/log") for p in fake.paths())


async def test_idle_makes_no_run_detail_requests():
    fake = Fake(base_routes())
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.live_runs == []
    assert s.last_finished is not None and s.last_finished.status == "succeeded"
    assert not any(p.startswith("/heartbeat-runs/") for p in fake.paths())


def open_q(iid):
    return {"id": f"q-{iid}", "issueId": iid, "kind": "request_confirmation", "status": "pending",
            "resolvedAt": None, "dismissedAt": None, "addresseeAgentId": None}


async def test_asks_only_main_agents_issues():
    resolved = json.loads((FIX / "interactions_resolved.json").read_text())
    fake = Fake(base_routes(
        issues=[issue("iss1", "a"), issue("iss2", "b"), issue("other", "c", assignee="someone-else")],
        inters={"iss1": [open_q("iss1")], "iss2": resolved, "other": [open_q("other")]},
        approvals=[{"id": "ap1", "status": "pending", "requestedByAgentId": AID},
                   {"id": "ap2", "status": "pending", "requestedByAgentId": "someone-else"}]))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.asks == 2  # iss1's open confirmation + the agent's pending approval
    assert "/issues/other/interactions" not in fake.paths()


async def test_waiting_counts_blocked_and_review():
    fake = Fake(base_routes(
        issues=[issue("i1", "a", "blocked"), issue("i2", "b", "in_review"), issue("i3", "c", "in_review"),
                issue("i4", "d", "backlog")],
        inters={"i1": [], "i2": [], "i3": [], "i4": [open_q("i4")]}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.asks == 1
    assert s.waiting == 4  # 1 blocked + 2 in review + 1 ask


async def test_interactions_refetched_only_on_activity():
    issues = [issue("i1", "a", activity="2026-11-21T20:00:00Z")]
    fake = Fake(base_routes(issues=issues, inters={"i1": []}))   # quiet issue: open asks are always re-read (I3)
    cache: dict = {}
    await gather(client_for(fake), AID, CID, NOW, cache=cache)
    await gather(client_for(fake), AID, CID, NOW + 3, cache=cache)
    assert fake.paths().count("/issues/i1/interactions") == 1
    issues[0]["lastActivityAt"] = "2026-11-21T21:00:00Z"
    s = await gather(client_for(fake), AID, CID, NOW + 6, cache=cache)
    assert fake.paths().count("/issues/i1/interactions") == 2
    assert s.asks == 0


async def test_failed_run_title_fetched_when_issue_closed():
    failed = {"id": "f1", "agentId": AID, "status": "failed", "startedAt": iso(NOW - 600),
              "finishedAt": iso(NOW - 300), "issueId": "done1", "error": "boom", "logBytes": 10}
    fake = Fake(base_routes(runs=[failed], extra={"/issues/done1": {"id": "done1", "title": "Grocery order"}}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.last_finished.issue_title == "Grocery order"


def test_parse_log_tail_last_tool():
    log = log_record(100, tool_use()) + "\n" + log_record(200, tool_use("Read")) + "\n" + log_record(300, text_msg())
    assert parse_log_tail(log) == pytest.approx(200)
    assert parse_log_tail("") is None


async def test_stale_asks_wait_but_do_not_wave():
    stale = {**open_q("i1"), "createdAt": iso(NOW - 3 * 86400), "title": "Old question"}
    fresh = {**open_q("i2"), "id": "q2", "createdAt": iso(NOW - 3600), "title": "OK to order?"}
    fake = Fake(base_routes(issues=[issue("i1", "a", "backlog"), issue("i2", "b", "backlog")],
                            inters={"i1": [stale], "i2": [fresh]}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.asks == 1
    assert s.ask_title == "OK to order?"
    assert s.waiting == 2


async def test_waiting_counts_an_issue_once():
    fake = Fake(base_routes(issues=[issue("i1", "a", "in_review")],
                            inters={"i1": [{**open_q("i1"), "createdAt": iso(NOW - 60)}]}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.asks == 1
    assert s.waiting == 1


async def test_answered_ask_clears_without_activity_change():
    inters = {"i1": [open_q("i1")]}
    fake = Fake(base_routes(issues=[issue("i1", "a", "backlog")], inters=inters))
    cache: dict = {}
    s1 = await gather(client_for(fake), AID, CID, NOW, cache=cache)
    fake.routes["/issues/i1/interactions"] = [{**open_q("i1"), "status": "answered", "resolvedAt": iso(NOW)}]
    s2 = await gather(client_for(fake), AID, CID, NOW + 3, cache=cache)   # same lastActivityAt
    assert s1.asks == 1 and s2.asks == 0


async def test_quiet_issues_refetched_after_ttl():
    fake = Fake(base_routes(issues=[issue("i1", "a", "backlog")], inters={"i1": []}))
    cache: dict = {}
    await gather(client_for(fake), AID, CID, NOW, cache=cache)
    await gather(client_for(fake), AID, CID, NOW + 60, cache=cache)
    assert fake.paths().count("/issues/i1/interactions") == 1
    await gather(client_for(fake), AID, CID, NOW + 301, cache=cache)
    assert fake.paths().count("/issues/i1/interactions") == 2


def test_parse_log_tail_split_chunk():
    line = json.dumps(tool_use("Write"))
    a = json.dumps({"ts": iso(500), "stream": "stdout", "chunk": line[:40]})
    b = json.dumps({"ts": iso(501), "stream": "stdout", "chunk": line[40:] + "\n"})
    assert parse_log_tail(a + "\n" + b + "\n") == pytest.approx(501)


def test_parse_log_tail_odd_lines_are_skipped():
    odd = "\n".join([json.dumps(["tool_use"]), json.dumps("tool_use"),
                      json.dumps({"ts": 12345, "stream": "stdout", "chunk": json.dumps(tool_use()) + "\n"}),
                      json.dumps({"ts": iso(700), "stream": "stdout", "chunk": json.dumps(["\"tool_use\""]) + "\n"})])
    assert parse_log_tail(odd) is None


async def test_detail_prefers_agents_words_and_drops_generic_status():
    def run_with(**fields):
        d = {**live_detail(tool="Bash", event_at=iso(NOW - 2)), **fields}
        return Fake(base_routes(runs=[live_run()], issues=[issue("iss1", "x")], inters={"iss1": []},
                                extra={"/heartbeat-runs/live1": d}))
    s = await gather(client_for(run_with(lastAssistantSnippet="Checking the calendar for next week",
                                         currentStatusMessage="Receiving agent output")), AID, CID, NOW, cache={})
    assert s.status_message == "Checking the calendar for next week"
    s = await gather(client_for(run_with(currentStatusMessage="Receiving agent output")), AID, CID, NOW, cache={})
    assert s.status_message is None
    s = await gather(client_for(run_with(currentStatusMessage="Syncing workspace to environment")), AID, CID, NOW, cache={})
    assert s.status_message == "Syncing workspace to environment"


SCOUT = "5c0a7e11-1111-4222-8333-444444444444"


def company_runs(main_runs, all_runs):
    """heartbeat-runs answers the agent's runs when filtered by agentId, everyone's otherwise."""
    return lambda req: main_runs if req.url.params.get("agentId") else all_runs


async def test_gather_includes_helpers():
    comms_run = {"id": "c1", "agentId": SCOUT, "status": "running", "startedAt": iso(NOW - 40),
                 "finishedAt": None, "issueId": "hx"}
    fake = Fake(base_routes(
        runs=company_runs([], [comms_run]),
        extra={f"/companies/{CID}/agents": [{"id": SCOUT, "name": "Scout", "adapterConfig": {"model": "claude-sonnet-5-5"}}],
               "/issues/hx": {"id": "hx", "title": "Send the invites"}}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert [(h["name"], h["state"], h["model"], h["line"]) for h in s.helpers] == [("SCOUT", "working", "sonnet", "SEND THE INVITES")]


async def test_agents_and_titles_cached_across_polls():
    comms_run = {"id": "c1", "agentId": SCOUT, "status": "running", "startedAt": iso(NOW - 40),
                 "finishedAt": None, "issueId": "hx"}
    fake = Fake(base_routes(runs=company_runs([], [comms_run]),
                            extra={"/issues/hx": {"id": "hx", "title": "Send the invites"}}))
    cache: dict = {}
    for dt in (0, 3, 6):
        await gather(client_for(fake), AID, CID, NOW + dt, cache=cache)
    assert fake.paths().count(f"/companies/{CID}/agents") == 1
    assert fake.paths().count("/issues/hx") == 1
    await gather(client_for(fake), AID, CID, NOW + 301, cache=cache)
    assert fake.paths().count(f"/companies/{CID}/agents") == 2


async def test_finished_helper_whispers_its_comment_fetched_once():
    done = {"id": "c9", "agentId": SCOUT, "status": "succeeded", "startedAt": iso(NOW - 90),
            "finishedAt": iso(NOW - 10), "issueId": "hx"}
    comments = [{"id": "m1", "createdByRunId": "c9", "authorAgentId": SCOUT, "body": "Sent the invites to 9 people."},
                {"id": "m0", "createdByRunId": "older", "authorAgentId": SCOUT, "body": "old"}]
    fake = Fake(base_routes(runs=company_runs([], [done]),
                            extra={"/issues/hx": {"id": "hx", "title": "Send the invites"}, "/issues/hx/comments": comments}))
    cache: dict = {}
    s = await gather(client_for(fake), AID, CID, NOW, cache=cache)
    await gather(client_for(fake), AID, CID, NOW + 3, cache=cache)
    assert s.helpers[0]["line"] == "SENT THE INVITES TO 9 PEOPLE."
    assert fake.paths().count("/issues/hx/comments") == 1


async def test_agent_name_from_roster():
    fake = Fake(base_routes(extra={f"/companies/{CID}/agents": [{"id": AID, "name": "Bubbles", "adapterConfig": {}}]}))
    s = await gather(client_for(fake), AID, CID, NOW, cache={})
    assert s.agent_name == "Bubbles"
