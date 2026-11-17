/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * What the source files of babysit share with one another: the constants,
 * types, state and functions that cross from one file to the next. Nothing
 * here is for babysitd; what the two programs share is in babysit.h.
 *
 *   babysit.c           main(), usage
 *   babysit_cmdline.c   command line and configuration
 *   babysit_time.c      clocks and intervals
 *   babysit_start.c     "start": the parent
 *   babysit_sitter.c    the babysitter process
 *   babysit_output.c    its log and line buffering
 *   babysit_control.c   its control FIFO and the exit_code file
 *   babysit_instance.c  instances on disk
 *   babysit_inspect.c   "check", "info", "signal", "cat"
 *   babysit_watch.c     "watch"
 */
#ifndef BABYSIT_INTERNAL_H
#define BABYSIT_INTERNAL_H

#include "babysit.h"
#include <regex.h>

#define EXIT_TIMEOUT 124 /* same as timeout(1) */
#define EXIT_STALLED 125

#define PREFIX_LEN 5
#define LINE_MAX_ 4096
#define PARTIAL_FLUSH_MS 1000 /* log an unterminated line after this long */
#define FOLLOW_TICK_MS 100    /* how often start --follow looks at the log */

enum {
  MSG_START = 1, /* value: child pid */
  MSG_ERR_LOG,   /* value: errno */
  MSG_ERR_EXEC,  /* value: errno */
  MSG_EXIT,      /* value: exit code */
  MSG_SIGNAL,    /* value: signal number */
  MSG_MATCH,
  MSG_STALL,
  MSG_TIMEOUT,
  MSG_DETACHED,
  MSG_WARN_LOG /* value: errno; the log could not be written. Not final. */
};

struct msg {
  int type;
  int value;
};

struct lstream {
  int fd;
  size_t len;
  char buf[PREFIX_LEN + LINE_MAX_ + 1]; /* prefix, data, newline */
};

/* ---------- looking at an instance from outside ---------- */

enum { ST_RUNNING, ST_EXITED, ST_LOST };

struct instance {
  char dir[PATH_BUF + ID_MAX + 8];
  char log[PATH_BUF + ID_MAX + 16];
};

/*
 * Options and operands of the commands that look at an existing instance:
 * every option may come before or after the operands. Returns the number
 * of operands stored in OPS, or -1 after complaining.
 */
struct look {
  const char *dir, *idle_s, *timeout_s, *onfail;
  int pid, log, code;
};

/* The --expect pattern, if any; set while the command line is read. */
extern const char *expect_pat;
extern int expect_fixed;
extern regex_t expect_re;

/* The babysitter's state. */
extern int log_fd, status_fd;
extern int watching;
extern long long last_read;
extern long long timeout_at;
extern char ctl_path[PATH_BUF + ID_MAX + 16];
extern int ctl_fd, ctl_wfd;

/* babysit.c */
void usage(FILE *f);

/* babysit_time.c */
long long now_ms(void);
int parse_interval(const char *s, long long *ms);

/* babysit_cmdline.c */
char *join_cmdline(char **cmd);
int resolve_logdir(const char *dir, char *logdir, size_t size);
int parse_signal(const char *s);
int look_args(const char *name, int argc, char **argv, int watch, int info,
              struct look *o, const char **ops, int max);
int load_timezone(const char *path, int required, int *utc);

/* babysit_output.c */
int write_all(int fd, const void *buf, size_t n);
void set_nonblock(int fd);
void set_cloexec(int fd);
void send_msg(int type, int value);
void release(int type, int value);
void log_write(const char *buf, size_t n);
void emit(const char *buf, size_t n);
void out_flush(void);
void flush_partial(struct lstream *s);
int read_stream(struct lstream *s);
void drain(struct lstream *s);

/* babysit_control.c */
void ctl_open(const char *dir);
void ctl_close(void);
void ctl_read(pid_t child);
void record_exit(const char *dir, int code);

/* babysit_sitter.c */
void babysitter(const char *dir, char **cmd, const char *cmdline, int detach,
                long long idle_ms, long long timeout_ms);

/* babysit_instance.c */
int new_instance(const char *logdir, const char *minute, char *child_id,
                 size_t size);
void drop_instance(const char *logdir, const char *child_id);
long logged_pid(const char *path);
int find_instance(const char *dir, const char *child_id, struct instance *in);
int instance_state(const struct instance *in, int *code);
int request_signal(const struct instance *in, int sig);

/* babysit_start.c */
int cmd_start(int argc, char **argv);

/* babysit_inspect.c */
int cmd_check(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_signal(int argc, char **argv);
int cmd_cat(int argc, char **argv);

/* babysit_watch.c */
int cmd_watch(int argc, char **argv);

#endif
