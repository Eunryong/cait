#ifndef CAIT_INTERNAL_H
#define CAIT_INTERNAL_H
/* Shared by the command modules in src; not part of the public cait.h. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>

#include "cait.h"

#define FILE_SEP '\x1f'

#define MAX_FILES 32

typedef struct {
	char status[16];	/* "" when unknown */
	long added, removed;	/* -1 when unknown */
} change_t;


extern const char *const level_names[];

/* util.c */
void cait_dir(char *out, size_t size);
void path_in_dir(char *out, size_t size, const char *name);
int ensure_dir(void);
void flatten(char *s);
int run(char *const argv[], char *out, size_t cap);
int json_value(const char *in, const char *key, char *out, size_t size);
int json_text(const char *in, const char *key, char *out, size_t size);
void safe_name(char *s);
void json_str_out(FILE *o, const char *s);
void clip_utf8(char *s, size_t max);
void mkdir_p2(const char *a, const char *b);
int copy_file(const char *from, const char *to);
void put_json_string(FILE *o, const char *s, size_t len);
const char *cait_invokes(const char *in, const char *sub);

/* measure.c */
void measure_change(const char *file, change_t *c);

/* stop.c */
int stop_add(const char *agent, const char *reason);
int stop_requested(const char *agent, char *reason, size_t size);

/* agents.c */
int report_agent(const char *in, char *out, size_t size);
void agent_remember(const char *id, const char *name);
int agent_name_of(const char *id, char *name, size_t size);
void pre_path(char *out, size_t size, const char *who, const char *file, const char *suffix);
void pre_save(const char *who, const char *file);
void brief_pending(const char *in);
void brief_claim(const char *name);

/* status.c */
void act_update(const char *id, const char *tool, const char *target, int done, int report, int final);

/* perm.c */
int perm_ask(const char *in, const char *id, const char *tool, const char *detail, const char *why_asked, int wait_s, char *msg, size_t msize);
int perm_hook(const char *in);

/* guard.c */
const char *guard_mode(void);
const char *guard_reason(const char *pat);
int guard_match(const char *detail, char *hit, size_t size);

/* revert.c */
void rec_key(time_t t, const char *agent, const char *files, char *out, size_t size);
void pre_take(const char *agent, const char *file, const char *key, int idx);
void snap_save(const char *key, int idx, const char *file);
int feed_append(const char *line, size_t n);

/* tell.c */
size_t inbox_take(char *out, size_t size, int mark);

/* report.c */
int read_level(void);

/* watch.c */
int parse_rec(char *line, rec_t *r);

#endif
