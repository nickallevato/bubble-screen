# SPDX-License-Identifier: GPL-3.0-only
import json

from app.live import events_url, handle_message


def test_events_url_from_api_base():
    assert events_url("https://paperclip.example.com/api", "cid") == "wss://paperclip.example.com/api/companies/cid/events/ws"
    assert events_url("http://127.0.0.1:3100/api/", "c") == "ws://127.0.0.1:3100/api/companies/c/events/ws"


def test_handle_message_kicks_on_events_and_ignores_junk():
    kicks = []
    handle_message(json.dumps({"type": "heartbeat.run.status", "payload": {"agentId": "a"}}), lambda e: kicks.append(e["type"]))
    handle_message("not json", lambda e: kicks.append("junk"))
    handle_message(json.dumps(["no", "dict"]), lambda e: kicks.append("list"))
    assert kicks == ["heartbeat.run.status"]
