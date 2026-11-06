#ifndef CAIT_H
#define CAIT_H

/* argv are the arguments after the subcommand name */
int cmd_report(int argc, char **argv);
int cmd_level(int argc, char **argv);
int cmd_watch(int argc, char **argv);
int cmd_stop(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_log(int argc, char **argv);

#include <stdio.h>
#include <time.h>

typedef struct {
	time_t time;
	const char *agent, *files, *status, *text;
	long added, removed;	/* -1 when unknown */
} rec_t;

/* shared by feed.c and panel.c */
void path_in_dir(char *out, size_t size, const char *name);
int ensure_dir(void);
int parse_rec(char *line, rec_t *r);
int stop_add(const char *agent, const char *reason);
int cmd_gui(int argc, char **argv);
void rec_key(time_t t, const char *agent, const char *files, char *out, size_t size);
int cmd_revert(int argc, char **argv);
int cmd_show(int argc, char **argv);
int cmd_tell(int argc, char **argv);
int cmd_inbox(int argc, char **argv);
int cmd_brief(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_panel(int argc, char **argv);

/* what one agent is doing now; state: working quiet done stopped reported */
typedef struct {
	char name[128], id[128], tool[128], target[512], task[512], text[512], state[16];
	long first, last, last_report, idle;
	int calls, done, reports, seen_act, unreported, final, final_ok, hidden, acked, reverted, file_reports;
	/* final_ok: the final report arrived, or the agent never reported or edited anything (nothing to wait for) */
} agent_info_t;
int status_load(agent_info_t *out, int max);
/* every agent, however many: a malloc'd array (free it); *n is its length */
agent_info_t *status_load_all(int *n);

/* permission requests waiting for the user */
typedef struct {
	char id[64], agent[128], tool[64], detail[512], why[256];
	long when;
} perm_t;
int perm_load(perm_t *out, int max);
/* a tool call in plain words ("수정 중: cart.c") */
void activity_label(const char *tool, const char *target, char *out, size_t size);
int perm_answer(const char *id, int allow, const char *msg);
int cmd_perm(int argc, char **argv);
int cmd_guard(int argc, char **argv);
int cmd_forget(int argc, char **argv);
int forget_agents(const char *name, int *running);
int cmd_ack(int argc, char **argv);
int ack_agent(const char *name);

#endif
