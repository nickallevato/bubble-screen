// SPDX-License-Identifier: GPL-3.0-only
// Just enough of Arduino for the drawing code to build on a PC (BUBBLE_HOST).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

// By value: decltype(a > b ? a : b) would be a reference to a parameter here.
template <class A, class B> inline std::common_type_t<A, B> max(A a, B b) { return a > b ? a : b; }
template <class A, class B> inline std::common_type_t<A, B> min(A a, B b) { return a < b ? a : b; }

extern uint32_t g_hostMillis;   // the harness drives time
inline uint32_t millis() { return g_hostMillis; }
inline uint32_t micros() { return g_hostMillis * 1000u; }

struct HostSerial {
  template <class... A> int printf(const char *, A...) { return 0; }
  void println(const char *) {}
};
static HostSerial Serial;
