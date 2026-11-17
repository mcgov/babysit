/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysitd - housekeeping for babysit's log directory.
 *
 * babysit files each instance's log under
 * <log-dir>/<YYYYmmdd>/<HH>/<MM>/<salt>/log (see babysit.h). At every
 * pass, babysitd does this:
 *
 *   1. Every day directory before today's is packed into
 *      <log-dir>/archive/babysit_logs_<YYYYmmdd>.tar.gz and removed, as far
 *      as it is finished: an instance whose log is still being written is
 *      left out and stays in place, and goes into an archive of its own
 *      (babysit_logs_<YYYYmmdd>.2.tar.gz) by the first pass that finds it
 *      finished. The same is done for anything dated later than now, which
 *      is there because the clock was set back: later days go to the
 *      archive, later hours of today and later minutes of this hour are
 *      packed in place. babysit refuses to start anything (exit status 75)
 *      while such records are in the way.
 *   2. When the filesystem holding the logs is at least
 *      compact_at_disk_percent full, it does not wait for today to be over
 *      either: every hour of today that is over is packed, as far as it is
 *      finished, into <YYYYmmdd>/<HH>.tar.gz.
 *   3. Archives and packed hours, and finished logs still lying in a day
 *      directory, older than max_age_days are deleted.
 *   4. When that filesystem is at least delete_at_disk_percent full (off by
 *      default), it deletes packed logs, oldest first, until enough has
 *      gone to bring usage back under the limit or none are left. Only
 *      tarballs are deleted: never a live log directory.
 *
 * It does not poll. It sleeps for check_interval (6 hours by default) and
 * does all of the above when it wakes. Writing anything to the FIFO
 * <log-dir>/.request wakes it early for the same pass.
 *
 * Logs that still have a writer are never archived or deleted, and neither
 * is anything created in the last couple of seconds: a babysit that has
 * made its directory but not yet locked its log must not find it gone.
 *
 * "Today" and "this hour" are reckoned in local time, or in UTC if the
 * configuration says timezone = utc; babysit names its directories by the
 * same setting.
 *
 * Packing is done so that a failure at any point loses nothing: the tarball
 * is written under a .partial name, flushed to disk and renamed into place,
 * and only then is the directory removed. If it fails (for instance because
 * the disk is already full) the partial file is removed, the directory
 * stays, and the attempt is not repeated for a while. The directory is
 * removed by first renaming it to <name>.removing, so that a run killed
 * part-way never leaves a half-empty directory under its real name to be
 * packed again. Leftover .partial files and .removing directories from a
 * crashed run are removed on start.
 *
 * Only one babysitd works on a log directory at a time. Each pass is done
 * holding a write lock on <log-dir>/.lock, so a second instance (a
 * "babysitd --once" run by hand or from cron next to the service, say)
 * waits for the first to finish instead of packing the same directory.
 *
 * Archives are made by running tar(1).
 */
#include "babysit.h"
#include <stdarg.h>
#include <sys/statvfs.h>

#define QUIET_S 2        /* untouched for this long before it may be packed */
#define RETRY_PACK_S 600 /* after a failed pack, leave it alone this long */

static const char *prog = "babysitd";
static char logdir[PATH_BUF];
static int journal; /* stderr goes to the systemd journal */
static int use_utc; /* timezone = utc: days and hours are UTC ones */

static volatile sig_atomic_t stop; /* SIGTERM or SIGINT: finish and exit */
static int wake_fd = -1;           /* our write end of the request FIFO */

static void on_stop(int sig) {
  int saved = errno;
  ssize_t w;

  (void)sig;
  stop = 1;
  /* Wake poll() even if the signal lands just before it is entered. */
  if (wake_fd >= 0) {
    w = write(wake_fd, "", 1);
    (void)w;
  }
  errno = saved;
}

/*
 * Housekeeping passes of different babysitd processes must not overlap:
 * two of them packing the same directory would each remove what the other
 * is reading. pass_begin() waits for the other one to finish.
 */
static int pass_fd = -1; /* <log-dir>/.lock */

static int pass_lock(short type, int cmd) {
  struct flock fl;

  memset(&fl, 0, sizeof fl);
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  return fcntl(pass_fd, cmd, &fl);
}

/* Is FD still the file that PATH names? */
static int still_at(int fd, const char *path) {
  struct stat a, b;

  return fd >= 0 && stat(path, &a) == 0 && fstat(fd, &b) == 0 &&
         a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/*
 * Returns -1 if the lock could not be taken; the pass must then be skipped.
 *
 * The log directory can be emptied or replaced under a running service. A
 * lock on a file that is no longer there excludes nobody, so the lock file
 * is opened again, and the directory created again, whenever that happens.
 */
static int pass_begin(void) {
  char path[PATH_BUF + 16];

  snprintf(path, sizeof path, "%s/.lock", logdir);
  for (;;) {
    if (pass_fd < 0) {
      if (mkdirs(logdir) < 0)
        return -1;
      pass_fd = open(path, O_RDWR | O_CREAT, 0600);
      if (pass_fd < 0)
        return -1;
      fcntl(pass_fd, F_SETFD, FD_CLOEXEC);
    }
    while (pass_lock(F_WRLCK, F_SETLKW) < 0)
      if (errno != EINTR || stop)
        return -1;
    if (still_at(pass_fd, path))
      return 0;
    close(pass_fd); /* which also drops the lock */
    pass_fd = -1;
  }
}

static void pass_end(void) { pass_lock(F_UNLCK, F_SETLK); }

static void usage(FILE *f) {
  fprintf(
      f, "Usage: babysitd [options]\n"
         "\n"
         "Housekeeping for babysit logs: pack the logs of past days into\n"
         "LOGDIR/archive/babysit_logs_<YYYYmmdd>.tar.gz, pack sooner when the "
         "disk\n"
         "fills up, and delete old logs.\n"
         "\n"
         "Options:\n"
         "  -l, --log-dir DIR        log directory to look after\n"
         "                           (default: $HOME/.local/var/log/babysit)\n"
         "  -c, --config FILE        configuration file\n"
         "                           (default: "
         "$XDG_CONFIG_HOME/babysit/babysit.conf,\n"
         "                           or $HOME/.config/babysit/babysit.conf)\n"
         "  -a, --max-age-days N     delete archives and finished logs older "
         "than N\n"
         "                           days; 0 keeps them forever (default: 30)\n"
         "  -i, --check-interval T   how often to wake up and do the "
         "housekeeping when\n"
         "                           nothing asks for it sooner; a number with "
         "an\n"
         "                           s, m, h or d suffix (default: 6h)\n"
         "  -u, --compact-at-disk-percent N\n"
         "                           when the log filesystem is N%% full or "
         "more,\n"
         "                           also pack the finished hours of days that "
         "are\n"
         "                           not over yet; 0 disables (default: 90)\n"
         "  -D, --delete-at-disk-percent N\n"
         "                           when the log filesystem is N%% full or "
         "more,\n"
         "                           delete packed logs, oldest first, until "
         "it is\n"
         "                           not; 0 disables (default: 0)\n"
         "  -1, --once               do the housekeeping once, then exit\n"
         "  -V, --version            show the version\n"
         "  -h, --help               show this help\n"
         "\n"
         "Options given on the command line override the configuration file, "
         "which\n"
         "holds 'key = value' lines: max_age_days, check_interval,\n"
         "compact_at_disk_percent, delete_at_disk_percent, timezone.\n");
}

static void say(int warning, const char *fmt, ...) {
  va_list ap;

  if (journal)
    fputs(warning ? "<4>" : "<6>", stderr);
  fprintf(stderr, "babysitd: %s", warning ? "warning: " : "");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

/*
 * The request FIFO: a byte written to it asks for a pass now. Holding a
 * write end ourselves keeps the read end from
 * reporting hang-up, and lets the signal handler wake poll().
 *
 * Called at the start of every pass: if the FIFO has been removed along
 * with the rest of the log directory, the one we hold is unreachable and a
 * new one is made.
 */
static int req_fd = -1;

static void open_request_fifo(void) {
  static int warned;
  char path[PATH_BUF + 16];
  struct stat sb;
  int r, w;

  snprintf(path, sizeof path, "%s/%s", logdir, REQUEST_FIFO);
  if (still_at(req_fd, path))
    return;
  w = wake_fd;
  wake_fd = -1; /* before closing it: the signal handler writes to it */
  if (w >= 0)
    close(w);
  if (req_fd >= 0)
    close(req_fd);
  req_fd = -1;
  if (mkfifo(path, 0600) < 0 && errno != EEXIST && !warned)
    say(1, "cannot create %s: %s", path, strerror(errno));
  r = open(path, O_RDONLY | O_NONBLOCK);
  if (r >= 0 && (fstat(r, &sb) < 0 || !S_ISFIFO(sb.st_mode))) {
    close(r); /* something else has that name; reading it would never block */
    r = -1;
  }
  w = r >= 0 ? open(path, O_WRONLY | O_NONBLOCK) : -1;
  if (r < 0 || w < 0) {
    if (!warned)
      say(1,
          "%s is not usable; a pass cannot be asked for early and "
          "will wait for the next scheduled check",
          path);
    warned = 1;
    if (r >= 0)
      close(r);
    return;
  }
  warned = 0;
  fcntl(r, F_SETFD, FD_CLOEXEC);
  fcntl(w, F_SETFD, FD_CLOEXEC);
  req_fd = r;
  wake_fd = w;
}

/*
 * Is stderr the journal stream systemd opened for us? JOURNAL_STREAM holds
 * its device and inode numbers. The variable alone proves nothing: it is
 * inherited by everything started from a service, a terminal for one, and
 * priority prefixes there would only be noise.
 */
static int stderr_is_journal(void) {
  const char *js = getenv("JOURNAL_STREAM");
  unsigned long long dev, ino;
  struct stat sb;
  char *end;

  if (!js || fstat(2, &sb) < 0)
    return 0;
  dev = strtoull(js, &end, 10);
  if (end == js || *end != ':')
    return 0;
  js = end + 1;
  ino = strtoull(js, &end, 10);
  if (end == js || *end != '\0')
    return 0;
  return (unsigned long long)sb.st_dev == dev &&
         (unsigned long long)sb.st_ino == ino;
}

static int parse_num(const char *s, long *out) {
  char *end;
  long v;

  errno = 0;
  v = strtol(s, &end, 10);
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    end++;
  if (end == s || *end || errno || v < 0)
    return -1;
  *out = v;
  return 0;
}

/* "90", "90s", "30m", "6h", "1d" -> seconds (at least 1). */
static int parse_duration(const char *s, long *out) {
  char *end;
  long v, mult = 1;

  errno = 0;
  v = strtol(s, &end, 10);
  if (end == s || errno || v < 0)
    return -1;
  if (*end == 's')
    end++;
  else if (*end == 'm')
    mult = 60, end++;
  else if (*end == 'h')
    mult = 3600, end++;
  else if (*end == 'd')
    mult = 86400, end++;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    end++;
  if (*end || v > LONG_MAX / mult)
    return -1;
  *out = v * mult < 1 ? 1 : v * mult;
  return 0;
}

static void load_config(const char *path, int required, long *max_age,
                        long *interval_s, long *disk_pct, long *delete_pct) {
  char line[512];
  FILE *f = fopen(path, "r");
  int lineno = 0;

  if (!f) {
    if (required) {
      fprintf(stderr, "babysitd: cannot read %s: %s\n", path, strerror(errno));
      exit(EXIT_USAGE);
    }
    return;
  }
  while (fgets(line, sizeof line, f)) {
    char *key, *val;
    long v;
    int kind;

    lineno++;
    kind = config_split(line, &key, &val);
    if (kind == 0)
      continue;
    if (kind < 0) {
      say(1, "%s:%d: expected 'key = value'", path, lineno);
      continue;
    }
    if (strcmp(key, "timezone") == 0) {
      if (parse_timezone(val, &use_utc) < 0)
        say(1, "%s:%d: timezone must be local or utc", path, lineno);
      continue;
    }
    if (strcmp(key, "check_interval") == 0) {
      if (parse_duration(val, &v) < 0)
        say(1, "%s:%d: expected a duration such as 6h or 30m", path, lineno);
      else
        *interval_s = v;
      continue;
    }
    if (strcmp(key, "max_age_days") != 0 &&
        strcmp(key, "compact_at_disk_percent") != 0 &&
        strcmp(key, "delete_at_disk_percent") != 0) {
      say(1, "%s:%d: unknown key '%s'", path, lineno, key);
      continue;
    }
    if (parse_num(val, &v) < 0) {
      say(1, "%s:%d: value must be a non-negative number", path, lineno);
      continue;
    }
    if (strcmp(key, "max_age_days") == 0)
      *max_age = v;
    else if (strcmp(key, "compact_at_disk_percent") == 0)
      *disk_pct = v;
    else
      *delete_pct = v;
  }
  fclose(f);
}

/*
 * Remove a directory that has just been packed. It is renamed out of the
 * way first, which is atomic: whatever happens after that, the directory
 * is no longer there under the name that would get it packed again.
 */
static void discard(const char *path) {
  char gone[PATH_BUF + 700];
  int n = snprintf(gone, sizeof gone, "%s.removing", path);

  if (n > 0 && (size_t)n < sizeof gone && rename(path, gone) == 0)
    remove_tree(gone);
  else
    remove_tree(path);
}

/*
 * tar -czf OUT -C DIR [--exclude=...]... NAME
 *
 * EXCL holds NEXCL ready-made "--exclude=<path>" arguments, for what is
 * still being written; they have to come before NAME to apply to it.
 */
static int run_tar(const char *out, const char *dir, const char *name,
                   char **excl, size_t nexcl) {
  const char **argv = (const char **)malloc((nexcl + 8) * sizeof *argv);
  int status, devnull;
  size_t i, n = 0;
  pid_t pid;

  if (!argv)
    return -1;
  argv[n++] = "tar";
  argv[n++] = "-czf";
  argv[n++] = out;
  argv[n++] = "-C";
  argv[n++] = dir;
  for (i = 0; i < nexcl; i++)
    argv[n++] = excl[i];
  argv[n++] = name;
  argv[n] = NULL;

  pid = fork();
  if (pid < 0) {
    free((void *)argv);
    return -1;
  }
  if (pid == 0) {
    devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0)
      dup2(devnull, 1);
    /* Arguments go straight to tar, not through a shell, and callers
     * never pass a name that starts with '-'. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint) */
    execvp("tar", (char *const *)argv);
    _exit(127);
  }
  free((void *)argv);
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int sync_file(const char *path) {
  int fd = open(path, O_WRONLY), rc;

  if (fd < 0)
    return -1;
  rc = fsync(fd);
  if (close(fd) < 0)
    rc = -1;
  return rc;
}

static void sync_dir(const char *path) {
  int fd = open(path, O_RDONLY);

  if (fd >= 0) {
    fsync(fd);
    close(fd);
  }
}

/*
 * Pack PARENT/NAME into the tarball OUT, which lives in HOME, by way of
 * TMP. Returns -1, with nothing left behind, if that did not work.
 */
static int pack(const char *tmp, const char *out, const char *home,
                const char *parent, const char *name, char **excl,
                size_t nexcl) {
  if (run_tar(tmp, parent, name, excl, nexcl) < 0 || sync_file(tmp) < 0 ||
      rename(tmp, out) < 0) {
    unlink(tmp);
    return -1;
  }
  sync_dir(home);
  return 0;
}

/* Newest modification time of anything in the tree. */
static time_t newest;
static int newest_cb(const char *path, const struct stat *sb, int flag,
                     struct FTW *ftw) {
  (void)path;
  (void)flag;
  (void)ftw;
  if (sb->st_mtime > newest)
    newest = sb->st_mtime;
  return 0;
}

/* How full the filesystem holding the logs is, rounded up as df does. */
static int disk_used_percent(void) {
  struct statvfs vfs;
  unsigned long long used, total;

  if (statvfs(logdir, &vfs) < 0)
    return -1;
  used = (unsigned long long)vfs.f_blocks - vfs.f_bfree;
  total = used + vfs.f_bavail;
  if (total == 0)
    return -1;
  return (int)(((used * 100) + total - 1) / total);
}

/* Has anything under DIR been created or changed in the last QUIET_S? */
static int recently_touched(const char *dir) {
  newest = 0;
  nftw(dir, newest_cb, 16, FTW_PHYS);
  return newest + QUIET_S > time(NULL);
}

/*
 * Remove the empty directories under DIR, and DIR itself if that leaves it
 * empty: instances whose command could not be executed, and logs that have
 * expired, leave them behind. One made a moment ago is left alone, since it
 * may be an instance that has not created its log yet.
 */
static int prune_cb(const char *path, const struct stat *sb, int flag,
                    struct FTW *ftw) {
  (void)ftw;
  if (flag == FTW_DP && sb->st_mtime + QUIET_S <= time(NULL))
    rmdir(path); /* fails, harmlessly, on one that is not empty */
  return 0;
}

static void prune_empty(const char *dir) {
  nftw(dir, prune_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/*
 * Choose the tarball to pack into: STEM.tar.gz, or STEM.2.tar.gz and so on
 * if that exists, so that nothing already packed is ever overwritten.
 * Returns -1 if every name up to .99 is taken.
 */
static int free_tarball_name(char *out, size_t n, const char *stem) {
  struct stat sb;
  int i;

  snprintf(out, n, "%s.tar.gz", stem);
  for (i = 2; stat(out, &sb) == 0 && i < 100; i++)
    snprintf(out, n, "%s.%d.tar.gz", stem, i);
  return stat(out, &sb) == 0 ? -1 : 0;
}

/*
 * What a look through a directory that is due for packing found: the
 * instances whose log still has a writer, which have to stay, and what is
 * finished and can go into the tarball, which is every other instance and
 * every tarball made by an earlier, smaller packing.
 */
static struct {
  char **live, **done;
  size_t nlive, ndone;
  int unsure; /* something could not be looked at: leave it all alone */
} seen;

static void seen_add(char ***list, size_t *n, const char *path, size_t len) {
  char **grown = (char **)realloc((void *)*list, (*n + 1) * sizeof **list);
  char *copy = malloc(len + 1);

  if (grown)
    *list = grown;
  if (!grown || !copy) {
    free(copy);
    seen.unsure = 1;
    return;
  }
  memcpy(copy, path, len);
  copy[len] = '\0';
  (*list)[(*n)++] = copy;
}

static void seen_clear(void) {
  size_t i;

  for (i = 0; i < seen.nlive; i++)
    free(seen.live[i]);
  for (i = 0; i < seen.ndone; i++)
    free(seen.done[i]);
  free((void *)seen.live);
  free((void *)seen.done);
  memset(&seen, 0, sizeof seen);
}

static int seen_cb(const char *path, const struct stat *sb, int flag,
                   struct FTW *ftw) {
  const char *base = path + ftw->base;
  size_t len = strlen(path);

  (void)sb;
  if (flag == FTW_DNR || flag == FTW_NS) {
    seen.unsure = 1;
  } else if (flag == FTW_F && strcmp(base, "log") == 0 && ftw->base > 0) {
    /* An instance is the directory its log is in. Any doubt about the
     * writer counts as one (see log_has_writer). */
    if (log_has_writer(path))
      seen_add(&seen.live, &seen.nlive, path, (size_t)ftw->base - 1);
    else
      seen_add(&seen.done, &seen.ndone, path, (size_t)ftw->base - 1);
  } else if (flag == FTW_F && len > 7 &&
             strcmp(path + len - 7, ".tar.gz") == 0) {
    seen_add(&seen.done, &seen.ndone, path, len);
  }
  return 0;
}

/*
 * Pack PARENT/NAME into STEM.tar.gz (or STEM.2.tar.gz and so on), which
 * lives in HOME, and remove what was packed.
 *
 * If nothing in it is being written, that is the whole directory. If
 * something is, the instances still being written are left out of the
 * tarball and stay where they are, so that their identifiers go on
 * working; they are packed by a later pass, once they have finished, into
 * a tarball of their own. A command that runs for a week therefore does
 * not keep the logs of everything else started that day unpacked.
 *
 * Returns 1 if a tarball was made (*LEFT is how many instances stayed
 * behind, OUT its name), 0 if there was nothing to pack yet, -1 if packing
 * failed, -2 if every tarball name is taken and -3 if HOME cannot be
 * created.
 */
static int pack_finished(const char *parent, const char *name, const char *stem,
                         const char *home, char *out, size_t outn,
                         size_t *left) {
  char from[PATH_BUF + 600], tmp[PATH_BUF + 700];
  char **excl = NULL;
  struct stat sb;
  size_t i, nlive, ndone, skip = strlen(parent) + 1;
  int rc;

  snprintf(from, sizeof from, "%s/%s", parent, name);
  if (recently_touched(from))
    return 0;
  prune_empty(from); /* after the look at the clock: this touches it too */
  if (stat(from, &sb) < 0 || !S_ISDIR(sb.st_mode))
    return 0; /* gone: there was nothing in it */

  seen_clear();
  if (nftw(from, seen_cb, 16, FTW_PHYS) < 0)
    seen.unsure = 1;
  if (seen.unsure || (seen.nlive > 0 && seen.ndone == 0)) {
    seen_clear();
    return 0; /* cannot tell, or nothing there has finished yet */
  }
  if (mkdirs(home) < 0) {
    seen_clear();
    return -3;
  }
  if (free_tarball_name(out, outn, stem) < 0) {
    seen_clear();
    return -2;
  }
  snprintf(tmp, sizeof tmp, "%s.partial", out);
  *left = seen.nlive;

  if (seen.nlive == 0) {
    rc = pack(tmp, out, home, parent, name, NULL, 0);
    if (rc == 0)
      discard(from);
    seen_clear();
    return rc == 0 ? 1 : -1;
  }

  /* Leave out what is still being written, by its path below PARENT. */
  nlive = seen.nlive;
  ndone = seen.ndone;
  excl = (char **)calloc(nlive, sizeof *excl);
  rc = excl ? 0 : -1;
  for (i = 0; rc == 0 && i < nlive; i++) {
    size_t n = strlen(seen.live[i] + skip) + sizeof "--exclude=";

    excl[i] = malloc(n);
    if (excl[i])
      snprintf(excl[i], n, "--exclude=%s", seen.live[i] + skip);
    else
      rc = -1;
  }
  if (rc == 0)
    rc = pack(tmp, out, home, parent, name, excl, nlive);
  /* Remove exactly what was found finished before the tarball was made,
   * and nothing that has appeared since. */
  for (i = 0; rc == 0 && i < ndone; i++)
    remove_tree(seen.done[i]);
  for (i = 0; excl && i < nlive; i++)
    free(excl[i]);
  free((void *)excl);
  seen_clear();
  return rc == 0 ? 1 : -1;
}

/*
 * Pack every day other than TODAY into the archive, as far as it is
 * finished. Those are the days that are over, and also any dated after
 * today: the clock has been set back since they were filed, and babysit
 * starts nothing until they are out of the way.
 */
static void archive_days(const char *today) {
  char adir[PATH_BUF + 16], from[PATH_BUF + 300], stem[PATH_BUF + 400],
      out[PATH_BUF + 420];
  struct dirent *de;
  DIR *d = opendir(logdir);
  size_t left = 0;
  int rc;

  if (!d)
    return;
  snprintf(adir, sizeof adir, "%s/archive", logdir);
  while ((de = readdir(d)) != NULL) {
    const char *name = de->d_name;

    /* Only what babysit itself names: the log directory may hold other
     * things, and they are not ours to pack and remove. */
    if (!is_day_name(name) || strcmp(name, today) == 0)
      continue;
    snprintf(from, sizeof from, "%s/%s", logdir, name);
    snprintf(stem, sizeof stem, "%s/babysit_logs_%s", adir, name);
    rc = pack_finished(logdir, name, stem, adir, out, sizeof out, &left);
    if (rc == -3)
      say(1, "cannot create %s: %s", adir, strerror(errno));
    else if (rc == -2)
      say(1, "every archive name for %s is taken; %s left in place", name,
          from);
    else if (rc < 0)
      say(1, "could not archive %s (is tar installed?); left in place", from);
    else if (rc > 0 && left > 0)
      say(0,
          "archived the finished logs of %s to %s; %lu still being written "
          "stay in place",
          name, out, (unsigned long)left);
    else if (rc > 0)
      say(0, "archived the logs of %s to %s", name, out);
  }
  closedir(d);
}

/*
 * Pack directories with two-digit names (hours in a day, minutes in an
 * hour) in place, DIR/<nn> into DIR/<nn>.tar.gz: those sorting before REF
 * if SIDE is negative, after it if positive. WHY starts the message that
 * reports each one.
 */
static time_t pack_retry_at;

static void pack_children(const char *dir, const char *ref, int side,
                          const char *why) {
  char from[PATH_BUF + 600], out[PATH_BUF + 620];
  struct dirent *de;
  DIR *d = opendir(dir);
  size_t left = 0;
  int cmp, rc;

  if (!d)
    return;
  while ((de = readdir(d)) != NULL && time(NULL) >= pack_retry_at) {
    if (!is_hour_name(de->d_name))
      continue;
    cmp = strcmp(de->d_name, ref);
    if (cmp == 0 || (cmp < 0) != (side < 0))
      continue;
    snprintf(from, sizeof from, "%s/%s", dir, de->d_name);
    rc = pack_finished(dir, de->d_name, from, dir, out, sizeof out, &left);
    if (rc == -1) {
      pack_retry_at = time(NULL) + RETRY_PACK_S;
      say(1,
          "could not pack %s (disk full, or tar missing?); it is "
          "left in place and not retried for %d minutes",
          from, RETRY_PACK_S / 60);
      break;
    }
    if (rc > 0 && left > 0)
      say(1,
          "%s: packed the finished logs of %s into %s; %lu still being "
          "written stay in place",
          why, from, out, (unsigned long)left);
    else if (rc > 0)
      say(1, "%s: packed %s into %s", why, from, out);
  }
  closedir(d);
}

/*
 * When the disk is filling up, do not wait for today to be over: pack each
 * of its hours that is, in place, as <day>/<HH>.tar.gz. (Other days need
 * no such help: archive_days() has already packed all of them that is
 * finished.)
 */
static void pack_finished_hours(const char *today, const char *hour,
                                long limit) {
  char dir[PATH_BUF + 300], why[96];
  int pct;

  if (limit <= 0)
    return;
  pct = disk_used_percent();
  if (pct < 0 || pct < limit)
    return;
  snprintf(why, sizeof why, "log filesystem is %d%% full (limit %ld%%)", pct,
           limit);
  snprintf(dir, sizeof dir, "%s/%s", logdir, today);
  pack_children(dir, hour, -1, why);
}

/*
 * Pack what is dated later than now within today: the hours after this
 * one, and the minutes after this one in this hour. Days after today are
 * dealt with by archive_days(). After this, and once whatever is still
 * being written there has finished, babysit will start commands again.
 */
static void pack_later(const char *today, const char *hour,
                       const char *minute) {
  static const char why[] = "the clock has gone backwards";
  char dir[PATH_BUF + 300];

  snprintf(dir, sizeof dir, "%s/%s", logdir, today);
  pack_children(dir, hour, 1, why);
  snprintf(dir, sizeof dir, "%s/%s/%s", logdir, today, hour);
  pack_children(dir, minute, 1, why);
}

/* Remember PATH as the deletion candidate if it is the oldest tarball yet. */
static void consider_oldest(const char *path, char *oldest, size_t n,
                            time_t *oldest_time) {
  struct stat sb;

  if (stat(path, &sb) < 0 || !S_ISREG(sb.st_mode))
    return;
  if (oldest[0] && sb.st_mtime >= *oldest_time)
    return;
  snprintf(oldest, n, "%s", path);
  *oldest_time = sb.st_mtime;
}

/* Consider the tarballs in DIR whose names IS_OURS accepts. */
static void scan_tarballs(const char *dir, int (*is_ours)(const char *),
                          char *oldest, size_t n, time_t *oldest_time) {
  char path[PATH_BUF + 600];
  struct dirent *de;
  DIR *d = opendir(dir);

  if (!d)
    return;
  while ((de = readdir(d)) != NULL) {
    if (!is_ours(de->d_name))
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    consider_oldest(path, oldest, n, oldest_time);
  }
  closedir(d);
}

/*
 * How many bytes must be freed for the log filesystem to be less than LIMIT
 * percent full: 0 if it already is, -1 if that cannot be found out. *PCT
 * is set to how full it is now.
 */
static long long disk_excess(long limit, int *pct) {
  struct statvfs vfs;
  unsigned long long used, total, allowed;

  if (statvfs(logdir, &vfs) < 0)
    return -1;
  used = (unsigned long long)vfs.f_blocks - vfs.f_bfree;
  total = used + vfs.f_bavail;
  if (total == 0)
    return -1;
  *pct = (int)(((used * 100) + total - 1) / total);
  if (*pct < limit)
    return 0;
  allowed = total * (unsigned long long)(limit - 1) / 100;
  return (long long)((used - allowed) * vfs.f_frsize);
}

/* Every tarball of ours: the archives, and the packed hours in each day. */
static void scan_all_tarballs(char *oldest, size_t n, time_t *oldest_time) {
  char dir[PATH_BUF + 300];
  struct dirent *de;
  DIR *d;

  snprintf(dir, sizeof dir, "%s/archive", logdir);
  scan_tarballs(dir, is_archive_name, oldest, n, oldest_time);
  d = opendir(logdir);
  if (!d)
    return;
  while ((de = readdir(d)) != NULL) {
    if (!is_day_name(de->d_name))
      continue;
    snprintf(dir, sizeof dir, "%s/%s", logdir, de->d_name);
    scan_tarballs(dir, is_packed_hour_name, oldest, n, oldest_time);
  }
  closedir(d);
}

/*
 * Last resort when the disk is nearly full: delete packed logs, oldest
 * first, until enough has gone to bring usage back under the limit or
 * there are none left. The candidates are the archives of past days and
 * the packed hours of days still in place. Live log directories are never
 * touched, so a command that is still running, or one nobody has had the
 * chance to pack yet, is safe.
 *
 * How much must go is worked out once, and the sizes of the deleted files
 * are counted against it. Asking the filesystem again after each deletion
 * would be simpler but is not safe: several filesystems (btrfs, ZFS) give
 * space back some time after the unlink, or not at all while a snapshot
 * holds the file, and every packed log would be gone before the number
 * moved.
 */
static void free_disk(long limit) {
  char oldest[PATH_BUF + 700];
  long long need, freed = 0;
  time_t oldest_time;
  struct stat sb;
  long removed = 0;
  int pct = 0;

  if (limit <= 0)
    return;
  need = disk_excess(limit, &pct);
  if (need <= 0)
    return;
  while (freed < need) {
    oldest[0] = '\0';
    oldest_time = 0;
    scan_all_tarballs(oldest, sizeof oldest, &oldest_time);
    if (!oldest[0] || stat(oldest, &sb) < 0 || unlink(oldest) < 0)
      break;
    freed += (long long)sb.st_blocks * 512;
    removed++;
    say(1, "log filesystem is %d%% full (limit %ld%%): deleted %s", pct, limit,
        oldest);
  }
  if (freed >= need)
    return;
  if (removed)
    say(1,
        "log filesystem is still %d%% full after deleting every "
        "packed log; the rest of the space is not babysit's",
        disk_used_percent());
  else
    say(1,
        "log filesystem is %d%% full (limit %ld%%) but there are no "
        "packed logs to delete",
        pct, limit);
}

/*
 * Remove what a crashed or killed run left behind: half-written tarballs
 * and half-removed directories that had already been packed.
 */
static void remove_leftovers(const char *dir) {
  char path[PATH_BUF + 600];
  struct dirent *de;
  DIR *d = opendir(dir);
  size_t len;

  if (!d)
    return;
  while ((de = readdir(d)) != NULL) {
    len = strlen(de->d_name);
    if (len > 8 && strcmp(de->d_name + len - 8, ".partial") == 0) {
      snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
      unlink(path);
    } else if (len > 9 && strcmp(de->d_name + len - 9, ".removing") == 0) {
      snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
      remove_tree(path);
    }
  }
  closedir(d);
}

static void remove_all_leftovers(void) {
  char dir[PATH_BUF + 300];
  struct dirent *de;
  DIR *d;

  remove_leftovers(logdir);
  snprintf(dir, sizeof dir, "%s/archive", logdir);
  remove_leftovers(dir);
  d = opendir(logdir);
  if (!d)
    return;
  while ((de = readdir(d)) != NULL) {
    if (!is_day_name(de->d_name))
      continue;
    snprintf(dir, sizeof dir, "%s/%s", logdir, de->d_name);
    remove_leftovers(dir);
  }
  closedir(d);
}

/* Delete the old finished instances in one minute directory. */
static long clean_minute(const char *dir, time_t cutoff) {
  char path[PATH_BUF + 900];
  struct dirent *de;
  DIR *d = opendir(dir);
  long removed = 0;

  if (!d)
    return 0;
  while ((de = readdir(d)) != NULL) {
    if (!is_salt_name(de->d_name))
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (tree_has_writer(path))
      continue;
    newest = 0;
    nftw(path, newest_cb, 16, FTW_PHYS);
    if (newest < cutoff && remove_tree(path) == 0)
      removed++;
  }
  closedir(d);
  if (removed)
    rmdir(dir); /* if that emptied it; fails, harmlessly, if not */
  return removed;
}

/* The same for every minute of one hour directory. */
static long clean_hour(const char *dir, time_t cutoff) {
  char path[PATH_BUF + 800];
  struct dirent *de;
  DIR *d = opendir(dir);
  long removed = 0;

  if (!d)
    return 0;
  while ((de = readdir(d)) != NULL) {
    if (!is_hour_name(de->d_name)) /* a minute is two digits too */
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    removed += clean_minute(path, cutoff);
  }
  closedir(d);
  if (removed)
    rmdir(dir);
  return removed;
}

/* Delete old archives and packed hours, and old finished logs that are
 * still in a day directory because something else there is being written. */
static void clean_old(long max_age_days) {
  char dir[PATH_BUF + 300], path[PATH_BUF + 600];
  struct dirent *de, *he;
  struct stat sb;
  time_t cutoff;
  long removed = 0;
  DIR *d, *h;

  if (max_age_days <= 0)
    return;
  cutoff = time(NULL) - ((time_t)max_age_days * 86400);

  snprintf(dir, sizeof dir, "%s/archive", logdir);
  d = opendir(dir);
  if (d != NULL) {
    while ((de = readdir(d)) != NULL) {
      if (!is_archive_name(de->d_name))
        continue;
      snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
      if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_mtime < cutoff &&
          unlink(path) == 0)
        removed++;
    }
    closedir(d);
  }

  d = opendir(logdir);
  if (d != NULL) {
    while ((de = readdir(d)) != NULL) {
      if (!is_day_name(de->d_name))
        continue;
      snprintf(dir, sizeof dir, "%s/%s", logdir, de->d_name);
      h = opendir(dir);
      if (h == NULL)
        continue;
      while ((he = readdir(h)) != NULL) {
        snprintf(path, sizeof path, "%s/%s", dir, he->d_name);
        if (is_packed_hour_name(he->d_name)) {
          if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) &&
              sb.st_mtime < cutoff && unlink(path) == 0)
            removed++;
        } else if (is_hour_name(he->d_name)) {
          removed += clean_hour(path, cutoff);
        }
      }
      closedir(h);
    }
    closedir(d);
  }
  if (removed)
    say(0, "deleted %ld archive%s/log director%s older than %ld days", removed,
        removed == 1 ? "" : "s", removed == 1 ? "y" : "ies", max_age_days);
}

/* One round of housekeeping. The caller holds the pass lock. */
static void full_pass(long max_age, long disk_pct, long delete_pct) {
  char now[ID_MAX], today[9], hour[3];

  if (minute_path(time(NULL), use_utc, now, sizeof now) < 0)
    return;
  memcpy(today, now, 8);
  today[8] = '\0';
  memcpy(hour, now + 9, 2);
  hour[2] = '\0';
  archive_days(today);
  pack_later(today, hour, now + 12);
  pack_finished_hours(today, hour, disk_pct);
  clean_old(max_age);
  free_disk(delete_pct);
}

int main(int argc, char **argv) {
  const char *dir = NULL, *config = NULL, *age_s = NULL, *poll_opt = NULL;
  const char *disk_opt = NULL, *delete_opt = NULL;
  const char *v;
  char cfg[PATH_BUF], cwd[PATH_BUF];
  long max_age = 30, interval_s = 6L * 3600, disk_pct = 90, delete_pct = 0;
  int once = 0, i, n;
  time_t next_full, now;
  struct sigaction sa;

  for (i = 1; i < argc; i++) {
    if (opt_match(prog, argc, argv, &i, "-l", "--log-dir", &v)) {
      dir = v;
    } else if (opt_match(prog, argc, argv, &i, "-c", "--config", &v)) {
      config = v;
    } else if (opt_match(prog, argc, argv, &i, "-a", "--max-age-days", &v)) {
      age_s = v;
    } else if (opt_match(prog, argc, argv, &i, "-i", "--check-interval", &v)) {
      poll_opt = v;
    } else if (opt_match(prog, argc, argv, &i, "-D", "--delete-at-disk-percent",
                         &v)) {
      delete_opt = v;
    } else if (opt_match(prog, argc, argv, &i, "-u",
                         "--compact-at-disk-percent", &v)) {
      disk_opt = v;
    } else if (opt_match(prog, argc, argv, &i, "-1", "--once", NULL)) {
      once = 1;
    } else if (opt_match(prog, argc, argv, &i, "-V", "--version", NULL)) {
      puts("babysitd " BABYSIT_VERSION);
      return 0;
    } else if (opt_match(prog, argc, argv, &i, "-h", "--help", NULL)) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "babysitd: unknown option '%s'\n\n", argv[i]);
      usage(stderr);
      return EXIT_USAGE;
    }
  }
  journal = stderr_is_journal();

  if (config)
    load_config(config, 1, &max_age, &interval_s, &disk_pct, &delete_pct);
  else if (default_config_path(cfg, sizeof cfg) == 0)
    load_config(cfg, 0, &max_age, &interval_s, &disk_pct, &delete_pct);
  if (age_s && parse_num(age_s, &max_age) < 0) {
    fprintf(stderr, "babysitd: invalid --max-age-days '%s'\n", age_s);
    return EXIT_USAGE;
  }
  if (poll_opt && parse_duration(poll_opt, &interval_s) < 0) {
    fprintf(stderr, "babysitd: invalid --check-interval '%s'\n", poll_opt);
    return EXIT_USAGE;
  }
  if (disk_opt && parse_num(disk_opt, &disk_pct) < 0) {
    fprintf(stderr, "babysitd: invalid --compact-at-disk-percent '%s'\n",
            disk_opt);
    return EXIT_USAGE;
  }
  if (delete_opt && parse_num(delete_opt, &delete_pct) < 0) {
    fprintf(stderr, "babysitd: invalid --delete-at-disk-percent '%s'\n",
            delete_opt);
    return EXIT_USAGE;
  }
  if (disk_pct > 100 || delete_pct > 100) {
    fprintf(stderr, "babysitd: compact_at_disk_percent and "
                    "delete_at_disk_percent must be 0-100\n");
    return EXIT_USAGE;
  }

  if (!dir) {
    if (default_log_dir(logdir, sizeof logdir) < 0) {
      fprintf(stderr, "babysitd: HOME is not set; use --log-dir\n");
      return EXIT_USAGE;
    }
  } else {
    if (dir[0] == '/') {
      n = snprintf(logdir, sizeof logdir, "%s", dir);
    } else {
      if (!getcwd(cwd, sizeof cwd)) {
        perror("babysitd: getcwd");
        return EXIT_USAGE;
      }
      n = snprintf(logdir, sizeof logdir, "%s/%s", cwd, dir);
    }
    if (n < 0 || (size_t)n >= sizeof logdir) {
      fprintf(stderr, "babysitd: log path too long\n");
      return EXIT_USAGE;
    }
  }
  if (mkdirs(logdir) < 0) {
    fprintf(stderr, "babysitd: cannot create %s: %s\n", logdir,
            strerror(errno));
    return EXIT_USAGE;
  }

  if (pass_begin() < 0) {
    fprintf(stderr, "babysitd: cannot lock %s/.lock: %s\n", logdir,
            strerror(errno));
    return EXIT_USAGE;
  }
  remove_all_leftovers();
  full_pass(max_age, disk_pct, delete_pct);
  pass_end();
  if (once)
    return 0;

  open_request_fifo();
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop; /* no SA_RESTART: poll() must be interrupted */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  next_full = time(NULL) + interval_s;
  while (!stop) {
    struct pollfd p;
    long long wait_s;
    int requested = 0, full;

    now = time(NULL);
    wait_s = next_full > now ? (long long)(next_full - now) : 0;
    p.fd = req_fd;
    p.events = POLLIN;
    p.revents = 0;
    n = poll(&p, 1, wait_s > INT_MAX / 1000 ? INT_MAX : (int)wait_s * 1000);
    if (stop)
      break;
    if (n < 0 && errno != EINTR)
      sleep(1); /* never spin */
    if (n > 0) {
      char junk[64];
      while (read(req_fd, junk, sizeof junk) > 0)
        requested = 1;
      if (!requested) { /* error on the FIFO: stop listening to it */
        close(req_fd);
        req_fd = -1;
      }
    }
    now = time(NULL);
    full = now >= next_full;
    if (!full && !requested)
      continue;
    if (requested)
      say(0, "woken by a request on %s", REQUEST_FIFO);
    if (full)
      next_full = now + interval_s;
    if (pass_begin() < 0) {
      if (stop)
        break;
      say(1, "cannot lock %s/.lock (%s); skipping this pass", logdir,
          strerror(errno));
      continue;
    }
    /* The log directory may have been emptied since the last pass. */
    open_request_fifo();
    full_pass(max_age, disk_pct, delete_pct);
    pass_end();
  }
  return 0;
}
