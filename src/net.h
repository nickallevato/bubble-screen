// SPDX-License-Identifier: GPL-3.0-only
// Live status from the bubble-screen service, fetched on core 0 so the renderer never waits.
#pragma once
#include <stdint.h>

// Another agent in the company, working off-screen for the main agent or just back with a result.
struct Helper {
  char id[9], name[13];
  uint8_t model;            // 0 haiku, 1 sonnet, 2 opus
  uint8_t side;             // 0 left, 1 right
  uint8_t state;            // 0 working, 1 done, 2 failed
  char line[64];
  uint32_t at;              // run start (working) or finish (done/failed), unix seconds
};

struct LiveStatus {
  bool have, ok;
  uint8_t state;            // 0 sleep, 1 think, 2 work, 3 ask, 4 error (firmware Mode order)
  uint8_t icon;             // 0 cal, 1 list, 2 cart, 3 mail (firmware ICONS order)
  char title[64], detail[64], next[12], agent[13];
  uint32_t since, updated, now;  // server unix seconds; `now` is when the response was served
  uint16_t waiting;
  uint32_t lastGoodMs;      // millis() of the last good response
  Helper helpers[4];
  uint8_t nHelpers;
};

void net_reserve();                                // init the Wi-Fi driver (allocates its buffers); call before big allocations
bool net_configured();                             // a service PSK is saved (otherwise the device runs the demo)
void net_begin();                                  // reads NVS, starts the fetch task on core 0
bool net_snapshot(LiveStatus &out);                // false until the first good response
void net_set(const char *key, const char *val);    // host | port | psk | ssid | pass -> NVS, applied next poll
