# SPDX-License-Identifier: GPL-3.0-only
"""bubble-screen: what a Paperclip agent is doing, for the desk display.

GET /api/now  device JSON (Bearer DEVICE_PSK)
GET /         a plain status page

The route handlers only read the cache; one background loop talks to Paperclip.
Run with:  uvicorn --factory app.main:from_env --host 0.0.0.0 --port 8000
"""
from __future__ import annotations

import asyncio
import contextlib
import hmac
import html
import logging
import os
import time
from typing import Awaitable, Callable

from fastapi import FastAPI, Header, HTTPException, Query
from fastapi.responses import HTMLResponse

from .live import events_url, listen_forever
from .paperclip import ReadOnlyClient, gather
from .status import Snapshot, StateClock, reduce

log = logging.getLogger("bubble-screen")

LIVE_POLL_S, IDLE_POLL_S, RETRY_S = 3, 30, 10
STALE_S = 90          # older than this and /api/now says ok: false
REFRESH_LIMIT_S = 30  # one whole Paperclip read, all requests included
KICK_DEBOUNCE_S = .3  # let a burst of live events settle into one re-read
MAX_WAIT_S = 25       # longest a device long-poll is held open
LABELS = {"sleep": "RESTING", "think": "THINKING", "work": "WORKING", "ask": "NEEDS YOU", "error": "STUCK"}


class Service:
    def __init__(self, gather_fn: Callable[[float], Awaitable[Snapshot]], psk: str,
                 now_fn: Callable[[], float] = time.time):
        self._gather, self.psk, self.now_fn = gather_fn, psk, now_fn
        self._clock = StateClock()
        self.version = 0
        self._kick = asyncio.Event()
        self._changed = asyncio.Event()
        self.cache: dict = {"state": "sleep", "title": "", "icon": "list", "detail": "", "since": 0,
                            "waiting": 0, "next": None, "helpers": [], "agent": "AGENT", "updated": None}

    def view(self) -> dict:
        """The cache as served: `ok` means fresh (a single failed read doesn't flip
        it), and `now` lets the device count elapsed time from `since`."""
        now = self.now_fn()
        fresh = self.cache["updated"] is not None and now - self.cache["updated"] <= STALE_S
        return {**self.cache, "ok": fresh, "now": int(now), "v": self.version}

    def kick(self) -> None:
        """Something happened in Paperclip: re-read now instead of at the next poll."""
        self._kick.set()

    async def wait_change(self, seen: int, timeout: float) -> None:
        if seen != self.version:
            return
        changed = self._changed
        try:
            await asyncio.wait_for(changed.wait(), timeout)
        except asyncio.TimeoutError:
            pass

    async def refresh_once(self, now: float | None = None) -> float:
        """One Paperclip read. Returns seconds until the next one."""
        now = time.time() if now is None else now
        try:
            snap = await asyncio.wait_for(self._gather(now), REFRESH_LIMIT_S)
            new = self._clock.stamp(reduce(snap), now)
            if {k: v for k, v in self.cache.items() if k != "updated"} != new:
                self.version += 1
                self._changed.set()
                self._changed = asyncio.Event()
            self.cache = {**new, "updated": int(now)}
        except Exception as e:  # keep serving the last good status; view() marks it stale in time
            log.warning("refresh failed: %s: %s", type(e).__name__, e)
            return RETRY_S
        busy = snap.live_runs or any(h.get("state") == "working" for h in snap.helpers)
        return LIVE_POLL_S if busy else IDLE_POLL_S

    async def run_forever(self) -> None:
        while True:
            delay = await self.refresh_once()
            try:
                await asyncio.wait_for(self._kick.wait(), delay)
                await asyncio.sleep(KICK_DEBOUNCE_S)
            except asyncio.TimeoutError:
                pass
            self._kick.clear()


def create_app(service: Service, start_loop: bool = True, extra_tasks=()) -> FastAPI:
    @contextlib.asynccontextmanager
    async def lifespan(_app):
        tasks = [asyncio.create_task(service.run_forever())] + [asyncio.create_task(f()) for f in extra_tasks] if start_loop else []
        yield
        for task in tasks:
            task.cancel()

    app = FastAPI(title="bubble-screen", lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)

    @app.get("/api/now")
    async def now(authorization: str = Header(default=""), v: int | None = Query(default=None),
                  wait: float = Query(default=0, ge=0)):
        token = authorization.removeprefix("Bearer ").strip()
        if not hmac.compare_digest(token.encode(), service.psk.encode()):
            raise HTTPException(status_code=401, detail="unauthorized")
        if v is not None and wait > 0:   # long-poll: hold until the status changes
            await service.wait_change(v, min(wait, MAX_WAIT_S))
        return service.view()

    @app.get("/", response_class=HTMLResponse)
    def page():
        c = service.view()
        rows = "".join(f"<tr><th>{html.escape(k)}</th><td>{html.escape(str(v))}</td></tr>"
                       for k, v in c.items() if k in ("title", "detail", "waiting", "since", "updated", "ok"))
        return (f"<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=5>"
                f"<title>{html.escape(c['agent'])} - bubble-screen</title><body style='font:16px monospace;background:#12303f;color:#e6f6ff;padding:24px'>"
                f"<h1>{html.escape(c['agent'])}: {LABELS.get(c['state'], c['state'])}</h1><table>{rows}</table></body>")

    return app


def from_env() -> FastAPI:
    logging.basicConfig(level=logging.INFO)
    logging.getLogger("httpx").setLevel(logging.WARNING)   # one line per Paperclip GET is noise
    client = ReadOnlyClient(os.environ["PAPERCLIP_API_BASE"],
                            os.environ["PAPERCLIP_TOKEN"])
    agent, company = os.environ["AGENT_ID"], os.environ["COMPANY_ID"]
    cache: dict = {}
    service = Service(lambda now: gather(client, agent, company, now, cache), os.environ["DEVICE_PSK"])
    url = events_url(os.environ["PAPERCLIP_API_BASE"], company)
    def on_event(e: dict) -> None:
        log.info("live event %s", e.get("type"))
        service.kick()
    listener = lambda: listen_forever(url, os.environ["PAPERCLIP_TOKEN"], on_event)
    return create_app(service, extra_tasks=[listener])
