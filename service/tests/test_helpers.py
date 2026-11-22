# SPDX-License-Identifier: GPL-3.0-only
from app.helpers import FINISH_WINDOW_S, helpers_from, model_class, side_for

NOW = 1_795_400_000.0
MAIN = "00000000-0000-4000-8000-0000000000a1"
AGENTS = {
    "5c0a7e11-0000": {"name": "Scout", "adapterConfig": {"model": "claude-sonnet-5-5"}},
    "a7a50000-0000": {"name": "Atlas", "adapterConfig": {"model": "claude-opus-5-5"}},
    "b1b00000-0000": {"name": "Pip", "adapterConfig": {"model": "claude-haiku-4-5"}},
    MAIN: {"name": "Bubbles", "adapterConfig": {"model": "claude-opus-5-5"}},
}


def iso(t):
    from datetime import UTC, datetime
    return datetime.fromtimestamp(t, UTC).isoformat().replace("+00:00", "Z")


def run(agent, status, started_ago, finished_ago=None, issue="i1", rid=None):
    return {"id": rid or f"{agent}-{started_ago}", "agentId": agent, "status": status, "issueId": issue,
            "startedAt": iso(NOW - started_ago), "finishedAt": iso(NOW - finished_ago) if finished_ago is not None else None}


TITLES = {"i1": "Send the dinner invites", "i2": "Build the grocery cart"}


def test_working_helper():
    out = helpers_from([run("5c0a7e11-0000", "running", 30)], AGENTS, TITLES, NOW, MAIN)
    assert out == [{"id": "5c0a7e11", "name": "SCOUT", "model": "sonnet", "side": side_for("5c0a7e11-0000"),
                    "state": "working", "line": "SEND THE DINNER INVITES", "at": int(NOW - 30)}]


def test_recent_finish_is_done_old_finish_is_dropped():
    runs = [run("a7a50000-0000", "succeeded", 100, 20, "i2"),
            run("5c0a7e11-0000", "succeeded", 400, FINISH_WINDOW_S + 5)]
    out = helpers_from(runs, AGENTS, TITLES, NOW, MAIN)
    assert [(h["name"], h["state"], h["line"], h["at"]) for h in out] == [("ATLAS", "done", "BUILD THE GROCERY CART", int(NOW - 20))]


def test_failed_and_cancelled():
    runs = [run("a7a50000-0000", "failed", 100, 10, "i2"), run("5c0a7e11-0000", "cancelled", 50, 5)]
    out = helpers_from(runs, AGENTS, TITLES, NOW, MAIN)
    assert [(h["name"], h["state"], h["line"]) for h in out] == [("ATLAS", "failed", "COULDN'T FINISH: BUILD THE GROCERY CART")]


def test_main_agent_is_never_a_helper():
    assert helpers_from([run(MAIN, "running", 5)], AGENTS, TITLES, NOW, MAIN) == []


def test_one_entry_per_agent_newest_first_max_four():
    runs = [run("5c0a7e11-0000", "running", 10, rid="new"), run("5c0a7e11-0000", "succeeded", 90, 60, rid="old")]
    out = helpers_from(runs, AGENTS, TITLES, NOW, MAIN)
    assert len(out) == 1 and out[0]["state"] == "working"
    many = {f"a{n}-x": {"name": f"Agent{n}", "adapterConfig": {}} for n in range(6)}
    out = helpers_from([run(f"a{n}-x", "running", n) for n in range(6)], many, {}, NOW, MAIN)
    assert len(out) == 4 and out[0]["name"] == "AGENT0"


def test_unknown_agent_and_missing_title():
    out = helpers_from([run("ffff0000-1", "running", 3, issue=None)], {}, {}, NOW, MAIN)
    assert out[0]["name"] == "HELPER" and out[0]["model"] == "sonnet" and out[0]["line"] == ""


def test_model_class_and_side_are_stable():
    assert model_class("claude-opus-5-5") == "opus"
    assert model_class("claude-haiku-4-5") == "haiku"
    assert model_class("claude-sonnet-5-5") == "sonnet"
    assert model_class(None) == "sonnet"
    assert side_for("5c0a7e11-0000") == side_for("5c0a7e11-0000") in ("L", "R")


def test_whisper_uses_the_runs_comment():
    r = run("5c0a7e11-0000", "succeeded", 100, 10, rid="run-9")
    out = helpers_from([r], AGENTS, TITLES, NOW, MAIN, run_lines={"run-9": "**Done: invites sent.** ✅\n\n- 7 of 9 replied"})
    assert out[0]["line"] == "DONE: INVITES SENT."


def test_first_line_cleans_markdown():
    from app.helpers import first_line
    assert first_line("## Logged: `tank` 74.0% (5.68 in), 4 bags\nmore") == "LOGGED: TANK 74.0% 5.68 IN, 4 BAGS"
    assert first_line("") == ""
