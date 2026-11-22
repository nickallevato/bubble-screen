# SPDX-License-Identifier: GPL-3.0-only
import re

from app.status import Run, Snapshot, StateClock, ascii_upper, icon_for, reduce

NOW = 1_800_000_000.0
FONT = re.compile(r"^[A-Z0-9 :.\-?!/',$&#%]*$")


def snap(**kw) -> Snapshot:
    base = dict(now=NOW, live_runs=[], last_finished=None, last_success_at=None, last_tool_at=None,
                status_message=None, asks=0, waiting=0, next_check=None)
    base.update(kw)
    return Snapshot(**base)


def live(title="Plan the weekly menu", started=NOW - 60, rid="r1") -> Run:
    return Run(id=rid, status="running", started_at=started, finished_at=None, issue_title=title)


def test_ask_beats_everything():
    s = snap(asks=1, live_runs=[live()], last_tool_at=NOW - 2)
    assert reduce(s)["state"] == "ask"


def test_error_window():
    failed = lambda ago: Run("f", "failed", NOW - ago - 60, NOW - ago, "Grocery order")
    assert reduce(snap(last_finished=failed(29 * 60)))["state"] == "error"
    assert reduce(snap(last_finished=failed(31 * 60)))["state"] == "sleep"
    assert reduce(snap(last_finished=failed(10 * 60), last_success_at=NOW - 5 * 60))["state"] == "sleep"


def test_work_vs_think():
    assert reduce(snap(live_runs=[live()], last_tool_at=NOW - 14))["state"] == "work"
    assert reduce(snap(live_runs=[live()], last_tool_at=NOW - 16))["state"] == "think"
    assert reduce(snap(live_runs=[live()], last_tool_at=None))["state"] == "think"


def test_newest_live_run_wins():
    s = snap(live_runs=[live("A", NOW - 100, "a"), live("B", NOW - 50, "b")])
    assert reduce(s)["title"] == "B"


def test_sleep_carries_waiting_and_next():
    out = reduce(snap(waiting=3, next_check="7:00 AM"))
    assert out["state"] == "sleep"
    assert out["waiting"] == 3
    assert out["next"] == "7:00 AM"


def test_icon_keywords():
    assert icon_for("Book dentist appointment") == "cal"
    assert icon_for("Weekly grocery order") == "cart"
    assert icon_for("Reply to school email") == "mail"
    assert icon_for("Plan the weekly menu") == "list"


def test_ascii_upper():
    assert ascii_upper("Café — 🦃 dinner") == "CAFE - DINNER"
    assert len(ascii_upper("x" * 80)) == 60
    assert FONT.match(ascii_upper("Ünïcødé “quotes” & <tags> (parens) 100%"))


def test_detail_prefers_status_message():
    assert reduce(snap(live_runs=[live()], status_message="Booked Dec 3, 8am"))["detail"] == "BOOKED DEC 3, 8AM"
    assert reduce(snap(live_runs=[live()]))["detail"] == ""


def test_error_title_is_failed_issue():
    s = snap(last_finished=Run("f", "failed", NOW - 400, NOW - 300, "Grocery order"))
    out = reduce(s)
    assert out["title"] == "GROCERY ORDER"
    assert out["icon"] == "cart"


def test_clock_since():
    clock = StateClock()
    a = clock.stamp({"state": "work"}, 100.0)
    b = clock.stamp({"state": "work"}, 160.0)
    c = clock.stamp({"state": "sleep"}, 200.0)
    assert a["since"] == b["since"] == 100
    assert c["since"] == 200


def test_ask_title_shown():
    out = reduce(snap(asks=1, ask_title="Approve plan rev 3 — Guardian"))
    assert out["state"] == "ask"
    assert out["title"] == "APPROVE PLAN REV 3 - GUARDIAN"


def test_agent_name_comes_through_folded():
    assert reduce(snap(agent_name="Bubbles"))["agent"] == "BUBBLES"
    assert reduce(snap())["agent"] == "AGENT"
