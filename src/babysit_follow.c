/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit follow-all".
 *
 * babysit follow-all: show, on stdout and as it is written, the output of
 * every command under the log directory: those running now, from this
 * moment on, and every one started from now on, from its first line. It
 * is what "tail -f" would be if it could be pointed at logs that do not
 * exist yet. Each line is the line of the log with the salt of its
 * instance (the last part of the identifier) in front, to tell the
 * commands apart:
 *
 *     k3f9w2mq OUT: Listening on :8080
 *
 * It goes on until it is interrupted.
 *
 * --since T also shows, from its first line, every log that was written to
 * in the last T, whether its command is still running or not.
 *
 * A log that nothing has been written to for --idle (ten seconds unless
 * told otherwise) is closed, so that a hundred quiet servers do not cost a
 * hundred open files. It is not forgotten: once a second it is asked how
 * long it is, and if it has grown it is opened again and followed from
 * where it was left, so nothing is missed. It is forgotten when its
 * command has gone.
 *
 * Nothing announces a new instance, so the directories where one can
 * appear are looked at ten times a second: those of this minute and the
 * last, by local time and by UTC, since babysit may be naming them by
 * either. That is four directories at most. An instance found there is
 * followed from the start even if its command has already exited: a
 * command that ran for a few milliseconds is as much part of what happened
 * as one that runs for an hour.
 */
#include "babysit_internal.h"

#define FOLLOW_ALL_TICK_MS 100
#define SETTLE_MS 2000 /* how long a new log may be without a writer */
#define IDLE_DEFAULT_MS 10000
#define ASLEEP_CHECK_MS 1000 /* how often a closed log is looked at */
#define NMINUTES 4

struct followed {
  struct followed *next;
  char id[ID_MAX];
  int fd;     /* -1 once it is finished; kept so that it is not found again */
  int asleep; /* closed for being quiet (fd is -1), to be opened if it grows */
  off_t pos;  /* where to go on reading it then */
  long long since, last; /* when it was picked up; when it last said anything */
  size_t len;
  char *buf; /* a line that is not yet all there */
};
#define FOLLOW_BUF ((size_t)2 * (PREFIX_LEN + LINE_MAX_ + 1))

static struct followed *all;
static char now_minutes[NMINUTES][ID_MAX];
static time_t since_time = -1; /* --since: logs written to after this, too */
static volatile sig_atomic_t follow_stop;

static void on_follow_stop(int sig) {
  (void)sig;
  follow_stop = 1;
}

static const char *salt_of(const struct followed *f) { return f->id + 15; }

static int is_now_minute(const char *id) {
  int i;

  for (i = 0; i < NMINUTES; i++)
    if (strncmp(id, now_minutes[i], 14) == 0)
      return 1;
  return 0;
}

static struct followed *known(const char *id) {
  struct followed *f;

  for (f = all; f; f = f->next)
    if (strcmp(f->id, id) == 0)
      return f;
  return NULL;
}

/* Is the babysitter of the log open at FD still there to write to it? */
static int fd_has_writer(int fd) {
  struct flock fl;

  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = LOCK_WRITER;
  fl.l_len = 1;
  return fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK;
}

/* A line, with the salt in front. A carriage return at its end, as a
 * program that writes CR LF leaves there, would send the cursor back over
 * the salt of the next line; it is left out. */
static void put(const struct followed *f, const char *text, size_t n) {
  while (n > 0 && text[n - 1] == '\r')
    n--;
  fputs(salt_of(f), stdout);
  putc(' ', stdout);
  fwrite(text, 1, n, stdout);
  putc('\n', stdout);
}

static void finish(struct followed *f) {
  if (f->len)
    put(f, f->buf, f->len);
  f->len = 0;
  if (f->fd >= 0)
    close(f->fd);
  f->fd = -1;
  free(f->buf);
  f->buf = NULL;
}

/* Print what has been added to the log since it was last read. */
static int catch_up(struct followed *f) {
  int any = 0;

  for (;;) {
    ssize_t r = read(f->fd, f->buf + f->len, FOLLOW_BUF - f->len);
    char *line = f->buf, *nl;

    if (r <= 0)
      return any;
    any = 1;
    f->len += (size_t)r;
    while ((nl = memchr(line, '\n', f->len - (size_t)(line - f->buf)))) {
      put(f, line, (size_t)(nl - line));
      line = nl + 1;
    }
    f->len -= (size_t)(line - f->buf);
    memmove(f->buf, line, f->len);
    if (f->len == FOLLOW_BUF) { /* longer than any line babysit writes */
      put(f, f->buf, f->len);
      f->len = 0;
    }
  }
}

/* The first line of a log is the command line. */
static void first_line(int fd, char *out, size_t size) {
  ssize_t n = pread(fd, out, size - 1, 0);
  char *nl;

  out[n > 0 ? n : 0] = '\0';
  nl = strchr(out, '\n');
  if (nl)
    *nl = '\0';
}

/*
 * Look at the instance ID, which is not known yet. OLD says that it was
 * there before we were: then it is followed only if it is still running,
 * and from where its log now ends.
 */
static void pick_up(const char *logdir, const char *id, int old) {
  char path[PATH_BUF + ID_MAX + 16], cmdline[512];
  struct followed *f;
  struct stat sb;
  int fd, running, over, recent;

  snprintf(path, sizeof path, "%s/%s/log", logdir, id);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return; /* not there yet, or not ours to read: look again later */
  running = fd_has_writer(fd);
  recent = old && since_time >= 0 && fstat(fd, &sb) == 0 &&
           sb.st_mtime >= since_time;
  if (recent)
    old = 0; /* shown from its first line, like one that has just begun */
  snprintf(path, sizeof path, "%s/%s/exit_code", logdir, id);
  over = stat(path, &sb) == 0;
  if (old && !running && (over || !is_now_minute(id))) {
    close(fd);
    if (!is_now_minute(id))
      return;
    fd = -1; /* finished a moment ago: remember not to show it */
  }
  f = calloc(1, sizeof *f);
  if (f && fd >= 0)
    f->buf = malloc(FOLLOW_BUF);
  if (!f || (fd >= 0 && !f->buf)) {
    free(f);
    if (fd >= 0)
      close(fd);
    return; /* no memory now: look again later */
  }
  snprintf(f->id, sizeof f->id, "%s", id);
  f->fd = fd;
  f->since = f->last = now_ms();
  f->next = all;
  all = f;
  if (fd < 0)
    return;
  if (old && running) {
    first_line(fd, cmdline, sizeof cmdline);
    printf("%s babysit: following %s, already running: %s\n", salt_of(f), id,
           cmdline);
    lseek(fd, 0, SEEK_END);
  } else {
    printf("%s babysit: following %s\n", salt_of(f), id);
  }
}

/* The instances of one minute: DIR is <log-dir>/<YYYYmmdd>/<HH>/<MM>. */
static void scan_minute(const char *logdir, const char *minute, int old) {
  char path[PATH_BUF + ID_MAX], id[ID_MAX];
  struct dirent *e;
  DIR *d;

  snprintf(path, sizeof path, "%s/%s", logdir, minute);
  d = opendir(path);
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {
    if (!is_salt_name(e->d_name))
      continue;
    snprintf(id, sizeof id, "%s/%s", minute, e->d_name);
    if (!known(id))
      pick_up(logdir, id, old);
  }
  closedir(d);
}

/* Call FN for every directory in <log-dir>/SOFAR whose name passes OK. */
static void each_sub(const char *logdir, const char *sofar,
                     int (*ok)(const char *),
                     void (*fn)(const char *, const char *)) {
  char path[PATH_BUF + ID_MAX], sub[ID_MAX];
  struct dirent *e;
  DIR *d;

  snprintf(path, sizeof path, "%s/%s", logdir, sofar);
  d = opendir(path);
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {
    if (!ok(e->d_name))
      continue;
    snprintf(sub, sizeof sub, "%s%s%s", sofar, *sofar ? "/" : "", e->d_name);
    fn(logdir, sub);
  }
  closedir(d);
}

/* Everything under the log directory, once, for what is running now. */
static void scan_old_minute(const char *logdir, const char *minute) {
  scan_minute(logdir, minute, 1);
}

static void scan_hour(const char *logdir, const char *hour) {
  each_sub(logdir, hour, is_hour_name, scan_old_minute); /* MM is like HH */
}

static void scan_day(const char *logdir, const char *day) {
  each_sub(logdir, day, is_hour_name, scan_hour);
}

static void set_now_minutes(void) {
  time_t t = time(NULL);
  int i;

  for (i = 0; i < NMINUTES; i++)
    if (minute_path(t - (time_t)(i / 2 * 60), i % 2, now_minutes[i],
                    sizeof now_minutes[i]) < 0)
      now_minutes[i][0] = '\0';
}

/* Close a log that has been quiet for a while, remembering the place. */
static void fall_asleep(struct followed *f) {
  f->pos = lseek(f->fd, 0, SEEK_CUR);
  close(f->fd);
  f->fd = -1;
  f->asleep = 1;
}

/* A closed log: has it grown, or has its command gone? */
static void look_at_sleeper(const char *logdir, struct followed *f) {
  char path[PATH_BUF + ID_MAX + 16];
  struct stat sb;

  snprintf(path, sizeof path, "%s/%s/log", logdir, f->id);
  if (stat(path, &sb) == 0 && sb.st_size > f->pos) {
    int fd = open(path, O_RDONLY);

    if (fd < 0)
      return; /* next time */
    lseek(fd, f->pos, SEEK_SET);
    f->fd = fd;
    f->asleep = 0;
    f->last = now_ms();
  } else if (!log_has_writer(path)) {
    f->asleep = 0;
    finish(f);
  }
}

/* babysit follow-all [--idle SECS] [--since SECS] [--log-dir DIR] */
int cmd_follow_all(int argc, char **argv) {
  struct timespec tick = {0, FOLLOW_ALL_TICK_MS * 1000000L};
  char logdir[PATH_BUF], path[PATH_BUF + ID_MAX + 16];
  const char *dir = NULL, *idle_s = NULL, *since_s = NULL, *v;
  long long idle_ms = IDLE_DEFAULT_MS, since_ms, now, next_look = 0;
  struct followed *f, **link;
  struct sigaction sa;
  struct stat sb;
  int i, look;

  since_time = -1;
  for (i = 1; i < argc; i++) {
    if (opt_match("babysit", argc, argv, &i, "-l", "--log-dir", &v)) {
      dir = v;
    } else if (opt_match("babysit", argc, argv, &i, "-i", "--idle", &v)) {
      idle_s = v;
    } else if (opt_match("babysit", argc, argv, &i, "-s", "--since", &v)) {
      since_s = v;
    } else {
      fprintf(stderr, "babysit follow-all: unexpected argument '%s'\n\n",
              argv[i]);
      usage(stderr);
      return EXIT_USAGE;
    }
  }
  if (idle_s && strcmp(idle_s, "0") == 0) {
    idle_ms = 0; /* never */
  } else if (idle_s && parse_interval(idle_s, &idle_ms) < 0) {
    fprintf(stderr, "babysit: invalid --idle interval '%s'\n", idle_s);
    return EXIT_USAGE;
  }
  if (since_s) {
    if (parse_interval(since_s, &since_ms) < 0) {
      fprintf(stderr, "babysit: invalid --since interval '%s'\n", since_s);
      return EXIT_USAGE;
    }
    since_time = time(NULL) - (time_t)((since_ms + 999) / 1000);
    if (since_time < 0)
      since_time = 0;
  }
  if (resolve_logdir(dir, logdir, sizeof logdir) < 0)
    return EXIT_USAGE;
  if (stat(logdir, &sb) == 0 && !S_ISDIR(sb.st_mode)) {
    fprintf(stderr, "babysit: %s is not a directory\n", logdir);
    return EXIT_USAGE;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_follow_stop;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  set_now_minutes();
  each_sub(logdir, "", is_day_name, scan_day);
  while (!follow_stop) {
    set_now_minutes();
    for (i = 0; i < NMINUTES; i++)
      if (now_minutes[i][0])
        scan_minute(logdir, now_minutes[i], 0);

    now = now_ms();
    look = now >= next_look;
    if (look)
      next_look = now + ASLEEP_CHECK_MS;
    for (link = &all; (f = *link) != NULL;) {
      if (f->asleep && look)
        look_at_sleeper(logdir, f);
      if (f->fd >= 0) {
        if (catch_up(f)) {
          f->last = now;
        } else if (!fd_has_writer(f->fd) && !catch_up(f)) {
          /* Nothing new and nobody to write more. A log that was only
           * just made may not have its writer yet; one with an exit
           * status has had it. */
          snprintf(path, sizeof path, "%s/%s/exit_code", logdir, f->id);
          if (stat(path, &sb) == 0 || now - f->since >= SETTLE_MS)
            finish(f);
        } else if (idle_ms > 0 && now - f->last >= idle_ms) {
          fall_asleep(f);
        }
      }
      if (f->fd < 0 && !f->asleep && !is_now_minute(f->id)) {
        *link = f->next; /* it cannot be found again */
        free(f->buf);
        free(f);
        continue;
      }
      link = &f->next;
    }
    if (fflush(stdout) != 0 || ferror(stdout)) {
      fprintf(stderr, "babysit: cannot write the output: %s\n",
              strerror(errno));
      return EXIT_USAGE;
    }
    nanosleep(&tick, NULL);
  }
  while ((f = all) != NULL) {
    all = f->next;
    finish(f);
    free(f);
  }
  fflush(stdout);
  return 0;
}
