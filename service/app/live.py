# SPDX-License-Identifier: GPL-3.0-only
"""Paperclip's live event feed, used only as a doorbell.

Any event for the company kicks the service into an immediate re-read; the
event contents are not trusted or needed beyond that. The socket only
receives, so the read-only promise holds.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Callable

import websockets

log = logging.getLogger("bubble-screen.live")
BACKOFF_S = (1, 2, 5, 10, 30)


def events_url(api_base: str, company_id: str) -> str:
    base = api_base.rstrip("/")
    base = "wss://" + base[len("https://"):] if base.startswith("https://") else "ws://" + base.removeprefix("http://")
    return f"{base}/companies/{company_id}/events/ws"


def handle_message(raw: str, on_event: Callable[[dict], None]) -> None:
    try:
        event = json.loads(raw)
    except ValueError:
        return
    if isinstance(event, dict) and event.get("type"):
        on_event(event)


async def listen_forever(url: str, token: str, on_event: Callable[[dict], None]) -> None:
    attempt = 0
    while True:
        try:
            async with websockets.connect(url, additional_headers={"Authorization": f"Bearer {token}"},
                                          open_timeout=10, ping_interval=20, ping_timeout=20) as ws:
                log.info("live events connected")
                attempt = 0
                async for raw in ws:
                    handle_message(raw, on_event)
        except asyncio.CancelledError:
            raise
        except Exception as e:  # network, auth, server restart: back off and retry
            delay = BACKOFF_S[min(attempt, len(BACKOFF_S) - 1)]
            attempt += 1
            log.warning("live events dropped (%s: %s); retrying in %ss", type(e).__name__, e, delay)
            await asyncio.sleep(delay)
