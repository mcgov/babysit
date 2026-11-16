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
 *   1. babysit files logs under <log-dir>/<boot timestamp>/. Any such
 *      directory that is not the current boot's is packed into
 *      <log-dir>/archive/babysit_logs_<boot timestamp>.tar.gz and removed.
 *      This is done whenever one is found (on start and at every wake-up), so it
 *      does not matter when after a reboot the service first runs.
 *   2. Within the current boot, babysit starts a new generation directory
 *      (<boot>/<N+1>/) whenever a pid comes round again. Every generation
 *      but the newest is packed into <boot>/<N>.tar.gz as soon as nothing
 *      is writing to it any more.
 *   3. Archives, and finished logs of the current boot, older than
 *      max_age_days are deleted.
 *   4. When the filesystem holding the logs is at least
 *      compact_at_disk_percent full, it starts a new generation itself, so
 *      that the logs written so far can be packed (step 2) before the disk
 *      runs out.
 *   5. It watches the kernel's last-allocated pid and warns when it wraps
 *      around: from then on pids of this boot can be reused. babysit itself
 *      deals with an actual collision (see babysit(1), PID REUSE).
 *
 * It does not poll. It sleeps for check_interval (6 hours by default) and
 * does all of the above when it wakes. Writing anything to the FIFO
 * <log-dir>/.request wakes it early for the same full pass; babysit does
 * that whenever it starts a new generation, and tests can too. Only
 * while a superseded generation is waiting for its last writer does it look
 * again every 30 seconds.
 *
 * Logs that still have a writer are never archived or deleted.
 *
 * Packing is done so that a failure at any point loses nothing: the tarball
 * is written under a .partial name and renamed into place, and only then is
 * the directory removed. If it fails (for instance because the disk is
 * already full) the partial file is removed, the directory stays, and the
 * attempt is not repeated for a while. Leftover .partial files from a
 * crashed run are removed on start.
 *
 * The last pid comes from a Linux /proc file; where it does not exist the
 * rollover watch is switched off and the rest still works. Archives are
 * made by running tar(1).
 */
#include "babysit.h"
#include <stdarg.h>
#include <sys/statvfs.h>

#define LAST_PID_FILE "/proc/sys/kernel/ns_last_pid"
#define PENDING_RECHECK_S 30 /* while a generation waits for its writers */
#define GEN_GRACE_S   2 /* a generation must be this quiet to be packed */
#define DISK_EVERY_S  600 /* at most one disk-triggered generation per this */
#define RETRY_PACK_S  600 /* after a failed pack, leave it alone this long */

static const char *prog = "babysitd";
static char logdir[PATH_BUF];
static int journal; /* stderr goes to the systemd journal */

static volatile sig_atomic_t stop; /* SIGTERM or SIGINT: finish and exit */
static int wake_fd = -1;           /* our write end of the request FIFO */

static void on_stop(int sig)
{
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

static void usage(FILE *f)
{
    fprintf(f,
"Usage: babysitd [options]\n"
"\n"
"Housekeeping for babysit logs: pack the logs of previous boots into\n"
"LOGDIR/archive/babysit_logs_<boot time>.tar.gz, delete old logs, and warn\n"
"when the system's pids wrap around.\n"
"\n"
"Options:\n"
"  -l, --log-dir DIR        log directory to look after\n"
"                           (default: $HOME/.local/var/log/babysit)\n"
"  -c, --config FILE        configuration file\n"
"                           (default: $XDG_CONFIG_HOME/babysit/babysit.conf,\n"
"                           or $HOME/.config/babysit/babysit.conf)\n"
"  -a, --max-age-days N     delete archives and finished logs older than N\n"
"                           days; 0 keeps them forever (default: 30)\n"
"  -i, --check-interval T   how often to wake up and do the housekeeping when\n"
"                           nothing asks for it sooner; a number with an\n"
"                           s, m, h or d suffix (default: 6h)\n"
"  -u, --compact-at-disk-percent N\n"
"                           when the log filesystem is N%% full or more,\n"
"                           start a new log generation so the current one\n"
"                           can be packed; 0 disables (default: 90)\n"
"      --last-pid-file F    file holding the last allocated pid\n"
"                           (default: " LAST_PID_FILE ")\n"
"  -1, --once               archive and clean once, then exit\n"
"  -V, --version            show the version\n"
"  -h, --help               show this help\n"
"\n"
"Options given on the command line override the configuration file, which\n"
"holds 'key = value' lines: max_age_days, check_interval,\n"
"compact_at_disk_percent.\n");
}

static void say(int warning, const char *fmt, ...)
{
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

static int parse_num(const char *s, long *out)
{
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
static int parse_duration(const char *s, long *out)
{
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
                        long *interval_s, long *disk_pct)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int lineno = 0;

    if (!f) {
        if (required) {
            fprintf(stderr, "babysitd: cannot read %s: %s\n", path,
                    strerror(errno));
            exit(EXIT_USAGE);
        }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *key = line, *val, *end;
        long v;

        lineno++;
        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || *key == '\n' || *key == '\0')
            continue;
        val = strchr(key, '=');
        if (!val) {
            say(1, "%s:%d: expected 'key = value'", path, lineno);
            continue;
        }
        end = val;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        *end = '\0';
        val++;
        while (*val == ' ' || *val == '\t')
            val++;
        if (strcmp(key, "check_interval") == 0) {
            if (parse_duration(val, &v) < 0)
                say(1, "%s:%d: expected a duration such as 6h or 30m", path,
                    lineno);
            else
                *interval_s = v;
            continue;
        }
        if (strcmp(key, "max_age_days") != 0 &&
            strcmp(key, "compact_at_disk_percent") != 0) {
            say(1, "%s:%d: unknown key '%s'", path, lineno, key);
            continue;
        }
        if (parse_num(val, &v) < 0) {
            say(1, "%s:%d: value must be a non-negative number", path,
                lineno);
            continue;
        }
        if (strcmp(key, "max_age_days") == 0)
            *max_age = v;
        else
            *disk_pct = v;
    }
    fclose(f);
}

/* 1 if any log in any generation under <log-dir>/<boot> has a writer. */
static int boot_dir_has_writer(const char *bootdir)
{
    char path[PATH_BUF + 600];
    struct dirent *de;
    DIR *d = opendir(bootdir);
    int busy = 0;

    if (!d)
        return 0;
    while (!busy && (de = readdir(d)) != NULL) {
        if (!is_pid_name(de->d_name))
            continue;
        snprintf(path, sizeof path, "%s/%s", bootdir, de->d_name);
        busy = gen_dir_has_writer(path);
    }
    closedir(d);
    return busy;
}

/* tar -czf OUT -C DIR NAME */
static int run_tar(const char *out, const char *dir, const char *name)
{
    int status, devnull;
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, 1);
        execlp("tar", "tar", "-czf", out, "-C", dir, name, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * Pack every generation of the current boot except the newest, once it has
 * no writer left. babysit only ever adds logs to the newest generation, so
 * an older one can only become quieter. The grace period covers a babysit
 * that picked a generation just before a newer one appeared.
 */
static time_t pack_retry_at;

/* Returns how many seconds to wait before looking again, 0 if nothing is
 * waiting to be packed. */
static int compact_generations(const char *current)
{
    char bootdir[PATH_BUF + 64], from[PATH_BUF + 400], out[PATH_BUF + 420],
        tmp[PATH_BUF + 440];
    struct dirent *de;
    struct stat sb;
    long newest_gen;
    int again = 0;
    DIR *d;

    if (time(NULL) < pack_retry_at)
        return 0;
    snprintf(bootdir, sizeof bootdir, "%s/%s", logdir, current);
    newest_gen = current_gen(bootdir);
    if (newest_gen < 1 || (d = opendir(bootdir)) == NULL)
        return 0;
    while ((de = readdir(d)) != NULL) {
        if (!is_pid_name(de->d_name) || atol(de->d_name) >= newest_gen)
            continue;
        snprintf(from, sizeof from, "%s/%s", bootdir, de->d_name);
        if (stat(from, &sb) < 0 || !S_ISDIR(sb.st_mode))
            continue;
        if (sb.st_mtime + GEN_GRACE_S > time(NULL)) {
            again = GEN_GRACE_S + 1;
            continue;
        }
        if (gen_dir_has_writer(from)) {
            if (again == 0)
                again = PENDING_RECHECK_S;
            continue;
        }
        snprintf(out, sizeof out, "%s.tar.gz", from);
        snprintf(tmp, sizeof tmp, "%s.partial", out);
        if (run_tar(tmp, bootdir, de->d_name) < 0 || rename(tmp, out) < 0) {
            unlink(tmp);
            pack_retry_at = time(NULL) + RETRY_PACK_S;
            say(1, "could not pack %s (disk full, or tar missing?); it is "
                "left in place and not retried for %d minutes", from,
                RETRY_PACK_S / 60);
            again = 0;
            break;
        }
        remove_tree(from);
        say(0, "packed log generation %s of this boot into %s", de->d_name,
            out);
    }
    closedir(d);
    return again;
}

/* Pack every boot directory other than the current one into the archive. */
static void archive_old_boots(const char *current)
{
    char adir[PATH_BUF + 16], from[PATH_BUF + 300], out[PATH_BUF + 400],
        tmp[PATH_BUF + 420];
    struct dirent *de;
    struct stat sb;
    DIR *d = opendir(logdir);
    int i;

    if (!d)
        return;
    snprintf(adir, sizeof adir, "%s/archive", logdir);
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;

        if (name[0] == '.' || name[0] == '-' || strcmp(name, "archive") == 0 ||
            strcmp(name, current) == 0)
            continue;
        snprintf(from, sizeof from, "%s/%s", logdir, name);
        if (stat(from, &sb) < 0 || !S_ISDIR(sb.st_mode))
            continue;
        if (boot_dir_has_writer(from))
            continue; /* cannot be from an earlier boot after all */
        if (mkdirs(adir) < 0) {
            say(1, "cannot create %s: %s", adir, strerror(errno));
            break;
        }
        /* Never overwrite an archive that is already there. */
        snprintf(out, sizeof out, "%s/babysit_logs_%s.tar.gz", adir, name);
        for (i = 2; stat(out, &sb) == 0 && i < 100; i++)
            snprintf(out, sizeof out, "%s/babysit_logs_%s.%d.tar.gz", adir,
                     name, i);
        snprintf(tmp, sizeof tmp, "%s.partial", out);
        if (run_tar(tmp, logdir, name) < 0 || rename(tmp, out) < 0) {
            unlink(tmp);
            say(1, "could not archive %s (is tar installed?); left in place",
                from);
            continue;
        }
        remove_tree(from);
        say(0, "archived the logs of boot %s to %s", name, out);
    }
    closedir(d);
}

/* Newest modification time of anything in the tree. */
static time_t newest;
static int newest_cb(const char *path, const struct stat *sb, int flag,
                     struct FTW *ftw)
{
    (void)path;
    (void)flag;
    (void)ftw;
    if (sb->st_mtime > newest)
        newest = sb->st_mtime;
    return 0;
}

/* How full the filesystem holding the logs is, rounded up as df does. */
static int disk_used_percent(void)
{
    struct statvfs vfs;
    unsigned long long used, total;

    if (statvfs(logdir, &vfs) < 0)
        return -1;
    used = (unsigned long long)vfs.f_blocks - vfs.f_bfree;
    total = used + vfs.f_bavail;
    if (total == 0)
        return -1;
    return (int)((used * 100 + total - 1) / total);
}

/*
 * If the disk is filling up, start a new generation so that the current
 * one can be packed as soon as its writers are done. Nothing is done if the
 * current generation is empty (there would be nothing to gain), and this is
 * rate limited: when the disk is full of other things, packing our logs
 * over and over would only add churn.
 */
static void check_disk(const char *current, long limit, int rate_limit)
{
    static time_t next_at;
    char gendir[PATH_BUF + 128];
    struct dirent *de;
    long gen;
    int pct, has_logs = 0;
    DIR *d;

    if (limit <= 0 || (rate_limit && time(NULL) < next_at))
        return;
    pct = disk_used_percent();
    if (pct < 0 || pct < limit)
        return;
    snprintf(gendir, sizeof gendir, "%s/%s", logdir, current);
    gen = current_gen(gendir);
    if (gen < 0)
        return;
    snprintf(gendir, sizeof gendir, "%s/%s/%ld", logdir, current, gen);
    if ((d = opendir(gendir)) == NULL)
        return;
    while (!has_logs && (de = readdir(d)) != NULL)
        has_logs = is_pid_name(de->d_name);
    closedir(d);
    if (!has_logs)
        return;
    snprintf(gendir, sizeof gendir, "%s/%s/%ld", logdir, current, gen + 1);
    if (mkdir(gendir, 0777) < 0)
        return; /* no room even for a directory: leave everything be */
    next_at = time(NULL) + DISK_EVERY_S;
    say(1, "log filesystem is %d%% full (limit %ld%%): started log "
        "generation %ld so that generation %ld can be packed", pct, limit,
        gen + 1, gen);
}

/* Remove what a crashed or killed run left half-written. */
static void remove_partials(const char *dir)
{
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
        }
    }
    closedir(d);
}

/* Delete the old finished logs of one live generation. */
static long clean_generation(const char *gendir, time_t cutoff)
{
    char path[PATH_BUF + 800];
    struct dirent *de;
    DIR *d = opendir(gendir);
    long removed = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        if (!is_pid_name(de->d_name))
            continue;
        snprintf(path, sizeof path, "%s/%s", gendir, de->d_name);
        if (ppid_dir_has_writer(path))
            continue;
        newest = 0;
        nftw(path, newest_cb, 16, FTW_PHYS);
        if (newest < cutoff && remove_tree(path) == 0)
            removed++;
    }
    closedir(d);
    return removed;
}

/* Delete old archives, and old finished logs of the current boot. */
static void clean_old(long max_age_days, const char *current)
{
    char dir[PATH_BUF + 64], path[PATH_BUF + 400];
    struct dirent *de;
    struct stat sb;
    time_t cutoff;
    long removed = 0;
    DIR *d;

    if (max_age_days <= 0)
        return;
    cutoff = time(NULL) - (time_t)max_age_days * 86400;

    snprintf(dir, sizeof dir, "%s/archive", logdir);
    if ((d = opendir(dir)) != NULL) {
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) &&
                sb.st_mtime < cutoff && unlink(path) == 0)
                removed++;
        }
        closedir(d);
    }

    snprintf(dir, sizeof dir, "%s/%s", logdir, current);
    if ((d = opendir(dir)) != NULL) {
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            if (stat(path, &sb) < 0)
                continue;
            if (S_ISREG(sb.st_mode)) { /* a packed generation */
                if (sb.st_mtime < cutoff && unlink(path) == 0)
                    removed++;
            } else if (S_ISDIR(sb.st_mode) && is_pid_name(de->d_name)) {
                removed += clean_generation(path, cutoff);
            }
        }
        closedir(d);
    }
    if (removed)
        say(0, "deleted %ld archive%s/log director%s older than %ld days",
            removed, removed == 1 ? "" : "s", removed == 1 ? "y" : "ies",
            max_age_days);
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *config = NULL, *age_s = NULL, *poll_opt = NULL;
    const char *disk_opt = NULL;
    const char *last_pid_file = LAST_PID_FILE;
    const char *v;
    char cfg[PATH_BUF], cwd[PATH_BUF], buf[64], boot[BOOT_STAMP_MAX];
    char tmp[PATH_BUF + 64];
    long max_age = 30, interval_s = 6 * 3600, disk_pct = 90, last = -1, cur;
    int once = 0, i, n, pid_watch = 1, again, rfd, wfd;
    time_t next_full, now;
    struct sigaction sa;

    for (i = 1; i < argc; i++) {
        if (opt_match(prog, argc, argv, &i, "-l", "--log-dir", &v)) {
            dir = v;
        } else if (opt_match(prog, argc, argv, &i, "-c", "--config", &v)) {
            config = v;
        } else if (opt_match(prog, argc, argv, &i, "-a", "--max-age-days",
                             &v)) {
            age_s = v;
        } else if (opt_match(prog, argc, argv, &i, "-i", "--check-interval",
                             &v)) {
            poll_opt = v;
        } else if (opt_match(prog, argc, argv, &i, "-u",
                             "--compact-at-disk-percent", &v)) {
            disk_opt = v;
        } else if (opt_match(prog, argc, argv, &i, "--last-pid-file",
                             "--last-pid-file", &v)) {
            last_pid_file = v;
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
    journal = getenv("JOURNAL_STREAM") != NULL;

    if (config) {
        load_config(config, 1, &max_age, &interval_s, &disk_pct);
    } else {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        n = -1;
        if (xdg && xdg[0] == '/')
            n = snprintf(cfg, sizeof cfg, "%s/babysit/babysit.conf", xdg);
        else if (home && home[0] == '/')
            n = snprintf(cfg, sizeof cfg, "%s/.config/babysit/babysit.conf",
                         home);
        if (n > 0 && (size_t)n < sizeof cfg)
            load_config(cfg, 0, &max_age, &interval_s, &disk_pct);
    }
    if (age_s && parse_num(age_s, &max_age) < 0) {
        fprintf(stderr, "babysitd: invalid --max-age-days '%s'\n", age_s);
        return EXIT_USAGE;
    }
    if (poll_opt && parse_duration(poll_opt, &interval_s) < 0) {
        fprintf(stderr, "babysitd: invalid --check-interval '%s'\n",
                poll_opt);
        return EXIT_USAGE;
    }
    if (disk_opt && parse_num(disk_opt, &disk_pct) < 0) {
        fprintf(stderr, "babysitd: invalid --compact-at-disk-percent '%s'\n",
                disk_opt);
        return EXIT_USAGE;
    }
    if (disk_pct > 100) {
        fprintf(stderr, "babysitd: compact_at_disk_percent must be 0-100\n");
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

    current_boot(logdir, boot, sizeof boot);
    snprintf(tmp, sizeof tmp, "%s/archive", logdir);
    remove_partials(tmp);
    snprintf(tmp, sizeof tmp, "%s/%s", logdir, boot);
    remove_partials(tmp);
    archive_old_boots(boot);
    check_disk(boot, disk_pct, 0);
    again = compact_generations(boot);
    clean_old(max_age, boot);
    if (once)
        return 0;

    if (read_first_line(last_pid_file, buf, sizeof buf) < 0 ||
        parse_num(buf, &last) < 0) {
        say(0, "%s is not readable; pid rollover is not monitored",
            last_pid_file);
        pid_watch = 0;
    }

    /* babysit writes a byte here when it starts a new generation. Holding
     * a write end ourselves keeps the read end from reporting hang-up. */
    snprintf(tmp, sizeof tmp, "%s/%s", logdir, REQUEST_FIFO);
    if (mkfifo(tmp, 0600) < 0 && errno != EEXIST)
        say(1, "cannot create %s: %s", tmp, strerror(errno));
    rfd = open(tmp, O_RDONLY | O_NONBLOCK);
    wfd = rfd >= 0 ? open(tmp, O_WRONLY | O_NONBLOCK) : -1;
    if (rfd < 0 || wfd < 0)
        say(1, "%s is not usable; babysit cannot ask for early packing, "
            "which will wait for the next scheduled check", tmp);

    wake_fd = wfd;
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
        if (again > 0 && again < wait_s)
            wait_s = again; /* a generation is waiting for its writers */
        p.fd = rfd;
        p.events = POLLIN;
        p.revents = 0;
        n = poll(&p, 1, wait_s > INT_MAX / 1000 ? INT_MAX : (int)wait_s * 1000);
        if (stop)
            break;
        if (n < 0 && errno != EINTR)
            sleep(1); /* never spin */
        if (n > 0) {
            char junk[64];
            while (read(rfd, junk, sizeof junk) > 0)
                requested = 1;
            if (!requested) /* error on the FIFO: stop listening to it */
                rfd = -1;
        }
        now = time(NULL);
        full = now >= next_full;
        if (!full && !requested && again == 0)
            continue;
        if (requested)
            say(0, "woken by a request on %s", REQUEST_FIFO);
        if (pid_watch && read_first_line(last_pid_file, buf, sizeof buf) == 0 &&
            parse_num(buf, &cur) == 0) {
            if (cur < last)
                say(1, "pid rollover detected (last pid went from %ld to "
                    "%ld); pids from this boot can now be reused, and "
                    "babysit starts a new log generation when it meets one "
                    "again", last, cur);
            last = cur;
        }
        if (full || requested) { /* the whole pass, scheduled or asked for */
            archive_old_boots(boot);
            check_disk(boot, disk_pct, 1);
        }
        again = compact_generations(boot);
        if (full || requested)
            clean_old(max_age, boot);
        if (full)
            next_full = now + interval_s;
    }
    return 0;
}
