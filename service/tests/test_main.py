# SPDX-License-Identifier: GPL-3.0-only
import httpx
import pytest
from fastapi.testclient import TestClient

from app.main import Service, create_app
from app.status import Run, Snapshot

PSK = "test-psk-0123456789"
KEYS = {"state", "title", "icon", "detail", "since", "waiting", "next", "ok", "updated", "now", "helpers", "v", "agent"}


def working(now: float) -> Snapshot:
    return Snapshot(now=now, live_runs=[Run("r", "running", now - 60, None, "Grocery order")], last_tool_at=now - 2)


def resting(now: float) -> Snapshot:
    return Snapshot(now=now, waiting=2)


class Scripted:
    """A gather stand-in that plays back a list of snapshots or exceptions."""

    def __init__(self, *steps):
        self.steps = list(steps)

    async def __call__(self, now: float) -> Snapshot:
        step = self.steps.pop(0)
        if isinstance(step, Exception):
            raise step
        return step(now)


def client(service: Service) -> TestClient:
    return TestClient(create_app(service, start_loop=False))


async def test_now_requires_psk():
    svc = Service(Scripted(working), PSK, now_fn=lambda: 1001.0)
    await svc.refresh_once(1000.0)
    c = client(svc)
    assert c.get("/api/now").status_code == 401
    assert c.get("/api/now", headers={"Authorization": "Bearer nope"}).status_code == 401
    r = c.get("/api/now", headers={"Authorization": f"Bearer {PSK}"})
    assert r.status_code == 200
    assert set(r.json()) == KEYS
    assert r.json()["state"] == "work" and r.json()["ok"] is True


async def test_upstream_failure_keeps_last_good():
    svc = Service(Scripted(working, httpx.ConnectError("down")), PSK, now_fn=lambda: 1010.0)
    await svc.refresh_once(1000.0)
    await svc.refresh_once(1003.0)
    body = client(svc).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["state"] == "work"
    assert body["ok"] is True   # one blip is not an outage: still fresh (M1)


async def test_stale_cache_is_not_ok():
    clock = [1000.0]
    svc = Service(Scripted(working), PSK, now_fn=lambda: clock[0])
    await svc.refresh_once(1000.0)
    clock[0] = 1091.0
    body = client(svc).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["state"] == "work" and body["ok"] is False


async def test_reduce_error_does_not_kill_the_loop():
    bad = lambda now: Snapshot(now=now, asks=1, ask_title=123)   # non-string title from upstream
    svc = Service(Scripted(working, bad), PSK, now_fn=lambda: 1003.0)
    await svc.refresh_once(1000.0)
    assert await svc.refresh_once(1003.0) == 10                  # caught, retry soon
    body = client(svc).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["state"] == "work"


async def test_now_field_is_serve_time():
    svc = Service(Scripted(working), PSK, now_fn=lambda: 1042.0)
    await svc.refresh_once(1000.0)
    body = client(svc).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["now"] == 1042 and body["since"] == 1000


async def test_cadence():
    svc = Service(Scripted(working, resting, httpx.ConnectError("down")), PSK)
    assert await svc.refresh_once(1000.0) == 3
    assert await svc.refresh_once(1003.0) == 30
    assert await svc.refresh_once(1033.0) == 10


async def test_before_first_fetch_is_not_ok():
    body = client(Service(Scripted(), PSK)).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["ok"] is False and body["state"] == "sleep"


async def test_root_page():
    svc = Service(Scripted(working), PSK, now_fn=lambda: 1001.0)
    await svc.refresh_once(1000.0)
    r = client(svc).get("/")
    assert r.status_code == 200
    assert "WORKING" in r.text and "GROCERY ORDER" in r.text
    assert PSK not in r.text


async def test_working_helper_keeps_fast_cadence():
    busy = lambda now: Snapshot(now=now, helpers=[{"id": "5c0a7e11", "name": "SCOUT", "model": "sonnet", "side": "L",
                                                   "state": "working", "line": "", "at": int(now)}])
    svc = Service(Scripted(busy), PSK, now_fn=lambda: 1001.0)
    assert await svc.refresh_once(1000.0) == 3
    body = client(svc).get("/api/now", headers={"Authorization": f"Bearer {PSK}"}).json()
    assert body["helpers"][0]["name"] == "SCOUT"


import asyncio


async def test_kick_triggers_prompt_refresh():
    calls = []

    async def gather(now):
        calls.append(now)
        return Snapshot(now=now)
    svc = Service(gather, PSK)
    task = asyncio.create_task(svc.run_forever())
    await asyncio.sleep(0.05)
    assert len(calls) == 1          # idle: next poll would be 30 s away
    svc.kick()
    await asyncio.sleep(0.6)
    task.cancel()
    assert len(calls) == 2


async def test_version_bumps_only_on_content_change():
    svc = Service(Scripted(resting, resting, working), PSK, now_fn=lambda: 1001.0)
    await svc.refresh_once(1000.0); v1 = svc.view()["v"]
    await svc.refresh_once(1003.0); v2 = svc.view()["v"]   # same content, newer `updated`
    await svc.refresh_once(1006.0); v3 = svc.view()["v"]
    assert v1 == v2 and v3 == v2 + 1


async def test_long_poll_returns_when_status_changes():
    svc = Service(Scripted(resting, working), PSK, now_fn=lambda: 1001.0)
    await svc.refresh_once(1000.0)
    v = svc.view()["v"]
    app = create_app(svc, start_loop=False)
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://t") as c:
        req = asyncio.create_task(c.get(f"/api/now?v={v}&wait=5", headers={"Authorization": f"Bearer {PSK}"}))
        await asyncio.sleep(0.1)
        assert not req.done()
        await svc.refresh_once(1002.0)
        r = await asyncio.wait_for(req, 1)
    assert r.json()["state"] == "work" and r.json()["v"] == v + 1


async def test_long_poll_times_out_with_same_version():
    svc = Service(Scripted(resting), PSK, now_fn=lambda: 1001.0)
    await svc.refresh_once(1000.0)
    v = svc.view()["v"]
    app = create_app(svc, start_loop=False)
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://t") as c:
        r = await c.get(f"/api/now?v={v}&wait=0.2", headers={"Authorization": f"Bearer {PSK}"})
    assert r.status_code == 200 and r.json()["v"] == v
