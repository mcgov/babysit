/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: one housekeeping pass of babysitd over a log
 * directory that the input lays out. This is where babysitd allocates:
 * the lists of what is finished and what is not, the arguments for tar.
 *
 * The first four bytes are the settings: max_age_days (0 to 3),
 * archive_after_days (0 to 2), archive_on_usage_percentage (off, always
 * reached, never reached) and delete_at_disk_percent (off, off, always
 * reached). Then come up to eight things to put in the directory, four
 * bytes each (what, a, b, c):
 *
 *   0-3  an instance, a%4 days ago at hour b and minute c, holding a log
 *        with a text of its own. Bits of "what" make it fresh (written
 *        just now, so not to be touched), ten days old, or without an
 *        exit_code file.
 *   4    a file that is not ours in a day directory
 *   5    something already under the name the day's archive would get
 *   6    a leftover of an interrupted pass: .partial or .removing
 *   7    a packed hour in a day directory (not a real archive)
 *
 * Then leftovers are cleared and the pass is run, as babysitd does.
 *
 * Fails if afterwards the text of an instance is neither in its log nor
 * in an archive, unless the settings allowed its deletion: older than
 * max_age_days, or the disk "full" enough to delete packed logs. A fresh
 * instance must be exactly where it was.
 *
 * tar is run for real, so this target is slow next to the others.
 */
#define main babysitd_main
#include "../../src/babysitd/babysitd.c"
#undef main

#include "fuzz_common.h"

#define MAX_THINGS 8
#define OLD_S 60          /* settled: older than QUIET_S */
#define ANCIENT_S 864000L /* ten days */

struct planted {
  char dir[PATH_BUF + 64]; /* the instance's directory */
  char text[32];
  int fresh, ancient;
};
static struct planted inst[MAX_THINGS];
static int ninst;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  fuzz_init();
  snprintf(logdir, sizeof logdir, "%s/logs", fuzz_root);
  return 0;
}

static void day_name(int days_ago, char *buf, size_t n) {
  time_t t = time(NULL) - ((time_t)days_ago * 86400);
  struct tm *tm = localtime(&t);

  if (!tm || strftime(buf, n, "%Y%m%d", tm) == 0)
    die("cannot tell the time");
}

static void put_text(const char *path, const char *text) {
  if (put_file(path, (const uint8_t *)text, strlen(text)) < 0)
    die("cannot write into the log directory");
}

/* Make PATH and the directories above it, up to the log directory, look
 * AGE seconds old. */
static void age_path(char *path, long age) {
  struct timespec ts[2];
  char *slash;

  ts[0].tv_sec = ts[1].tv_sec = time(NULL) - age;
  ts[0].tv_nsec = ts[1].tv_nsec = 0;
  while (strlen(path) > strlen(logdir)) {
    utimensat(AT_FDCWD, path, ts, 0);
    slash = strrchr(path, '/');
    *slash = '\0';
  }
}

static void plant(unsigned what, unsigned a, unsigned b, unsigned c, int idx) {
  char day[16], path[PATH_BUF + 128];
  struct planted *p = &inst[ninst];
  const char *names[] = {"notes.txt", ".hidden", "0", "core", "x.tar.gz"};

  day_name((int)(a % 4), day, sizeof day);
  switch (what % 8) {
  case 0:
  case 1:
  case 2:
  case 3:
    snprintf(p->dir, sizeof p->dir, "%s/%s/%02u/%02u/%c%c%c%c", logdir, day,
             b % 24, c % 60, salt_chars[idx], salt_chars[idx], 'a', 'a');
    if (mkdirs(p->dir) < 0)
      die("cannot make an instance directory");
    snprintf(p->text, sizeof p->text, "the log of instance %d", idx);
    snprintf(path, sizeof path, "%s/log", p->dir);
    put_text(path, p->text);
    if (!(what & 0x40)) {
      snprintf(path, sizeof path, "%s/exit_code", p->dir);
      put_text(path, "0\n");
    }
    p->fresh = (what & 0x10) != 0;
    p->ancient = !p->fresh && (what & 0x20) != 0;
    ninst++;
    break;
  case 4:
    snprintf(path, sizeof path, "%s/%s", logdir, day);
    if (mkdirs(path) < 0)
      die("cannot make a day directory");
    snprintf(path, sizeof path, "%s/%s/%s", logdir, day, names[b % 5]);
    put_text(path, "not ours");
    break;
  case 5:
    snprintf(path, sizeof path, "%s/archive", logdir);
    if (mkdirs(path) < 0)
      die("cannot make the archive directory");
    snprintf(path, sizeof path, "%s/archive/babysit_logs_%s%s.tar.gz", logdir,
             day, b % 2 ? ".2" : "");
    put_text(path, "in the way");
    break;
  case 6:
    snprintf(path, sizeof path, "%s/archive", logdir);
    if (mkdirs(path) < 0)
      die("cannot make the archive directory");
    snprintf(path, sizeof path, "%s/archive/babysit_logs_%s.tar.gz%s", logdir,
             day, b % 2 ? ".partial" : ".removing");
    put_text(path, "left over");
    break;
  default:
    snprintf(path, sizeof path, "%s/%s", logdir, day);
    if (mkdirs(path) < 0)
      die("cannot make a day directory");
    snprintf(path, sizeof path, "%s/%s/%02u.tar.gz", logdir, day, b % 24);
    put_text(path, "not an archive");
    break;
  }
}

/* Everything the archives under the log directory hold, as one text. */
static char *archived;
static size_t narchived;

static int unpack_cb(const char *path, const struct stat *sb, int flag,
                     struct FTW *ftw) {
  char cmd[PATH_BUF + 64], buf[4096];
  size_t len = strlen(path), n;
  FILE *f;

  (void)sb;
  (void)ftw;
  if (flag != FTW_F || len < 7 || strcmp(path + len - 7, ".tar.gz") != 0)
    return 0;
  snprintf(cmd, sizeof cmd, "tar -xzOf '%s' 2>/dev/null", path);
  f = popen(cmd, "r");
  if (!f)
    die("cannot run tar");
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    char *grown = realloc(archived, narchived + n + 1);

    if (!grown)
      die("out of memory");
    archived = grown;
    memcpy(archived + narchived, buf, n);
    narchived += n;
    archived[narchived] = '\0';
  }
  pclose(f);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const long usage_pct[] = {0, 1, 100}, delete_pct[] = {0, 0, 1};
  char path[PATH_BUF + 128], line[64];
  long max_age, disk, del;
  size_t i;
  int k;

  FUZZ_GUARD();
  if (size < 8)
    return 0;
  if (remove_tree(logdir) < 0 && errno != ENOENT)
    die("cannot clear the log directory");
  if (mkdirs(logdir) < 0)
    return 0; /* no room */
  max_age = data[0] % 4;
  archive_after = data[1] % 3;
  disk = usage_pct[data[2] % 3];
  del = delete_pct[data[3] % 3];
  use_utc = 0;
  pack_retry_at = 0;

  ninst = 0;
  for (i = 4, k = 0; i + 3 < size && k < MAX_THINGS; i += 4, k++)
    plant(data[i], data[i + 1], data[i + 2], data[i + 3], k);
  /* Age everything, then make the fresh ones fresh again: a directory
   * they share with an old one has just been written to. */
  for (k = 0; k < ninst; k++) {
    snprintf(path, sizeof path, "%s/exit_code", inst[k].dir);
    age_path(path, inst[k].ancient ? ANCIENT_S : OLD_S);
    snprintf(path, sizeof path, "%s/log", inst[k].dir);
    age_path(path, inst[k].ancient ? ANCIENT_S : OLD_S);
  }
  for (k = 0; k < ninst; k++)
    if (inst[k].fresh) {
      snprintf(path, sizeof path, "%s/log", inst[k].dir);
      age_path(path, 0);
    }

  remove_all_leftovers();
  full_pass(max_age, disk, del);

  free(archived);
  archived = NULL;
  narchived = 0;
  for (k = 0; k < ninst; k++) {
    int in_place;

    snprintf(path, sizeof path, "%s/log", inst[k].dir);
    in_place = read_first_line(path, line, sizeof line) == 0 &&
               strcmp(line, inst[k].text) == 0;
    if (in_place)
      continue;
    if (inst[k].fresh)
      die("an instance that was just written has been touched");
    if (del > 0 || (max_age > 0 && inst[k].ancient))
      continue; /* the settings allowed its deletion */
    if (!archived) {
      archived = calloc(1, 1);
      if (!archived || nftw(logdir, unpack_cb, 16, FTW_PHYS) < 0)
        die("cannot look through the archives");
    }
    if (!strstr(archived, inst[k].text))
      die("a log is neither where it was nor in an archive");
  }
  return 0;
}
