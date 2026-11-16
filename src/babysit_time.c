/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: clocks and intervals.
 */
#include "babysit_internal.h"

/* ---------- helpers ---------- */

/* Monotonic milliseconds where available, wall clock otherwise. */
long long now_ms(void) {
  struct timeval tv;
#ifdef CLOCK_MONOTONIC
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return ((long long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
#endif
  gettimeofday(&tv, NULL);
  return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

/* Parse "5", "0.5", "30s", "2m", "1h" into milliseconds (> 0). */
int parse_interval(const char *s, long long *ms) {
  char *end;
  double d, mult = 1;

  errno = 0;
  d = strtod(s, &end);
  if (end == s || errno)
    return -1;
  if (*end == 's') {
    end++;
  } else if (*end == 'm') {
    mult = 60;
    end++;
  } else if (*end == 'h') {
    mult = 3600;
    end++;
  }
  if (*end || !(d > 0) || d * mult > 1e9)
    return -1;
  *ms = (long long)((d * mult * 1000) + 0.5);
  if (*ms < 1)
    *ms = 1;
  return 0;
}
