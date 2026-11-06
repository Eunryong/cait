/* permission requests: the hook asks, the user answers on the cait screen */
#include "internal.h"

/* ---- permission requests: the hook asks, the user answers on the cait screen ---- */

/* <dir>/perm/<id>.req: epoch \t agent id \t agent name \t tool \t detail
 * <dir>/perm/<id>.ans: allow|deny [\t message] */
static void perm_dir(char *out, size_t size)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/perm", dir);
}

int perm_load(perm_t *out, int max)
{
	char root[4200], path[4500], line[2048];
	struct dirent *e;
	DIR *d;
	int n = 0;

	memset(out, 0, sizeof(*out) * (size_t)max);
	perm_dir(root, sizeof(root));
	d = opendir(root);
	while (d && (e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		FILE *f;
		char *p[6] = { 0 }, *c = line;

		if (l < 5 || strcmp(e->d_name + l - 4, ".req") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%.*s.ans", root, (int)(l - 4), e->d_name);
		if (access(path, F_OK) == 0)
			continue;	/* already answered */
		snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(line, sizeof(line), f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		for (int i = 0; i < 6 && c; i++) {
			p[i] = c;
			c = strchr(c, '\t');
			if (c)
				*c++ = '\0';
		}
		for (int i = 5; i >= 4; i--)
			if (p[i]) {
				p[i][strcspn(p[i], "\n")] = '\0';
				break;
			}
		snprintf(out[n].id, sizeof(out[n].id), "%.*s", (int)(l - 4), e->d_name);
		out[n].when = p[0] ? atol(p[0]) : 0;
		if (out[n].when < (long)time(NULL) - 120)
			continue;	/* the hook gave up long ago */
		snprintf(out[n].agent, sizeof(out[n].agent), "%s", p[2] && *p[2] ? p[2] : p[1] ? p[1] : "?");
		snprintf(out[n].tool, sizeof(out[n].tool), "%s", p[3] ? p[3] : "");
		snprintf(out[n].detail, sizeof(out[n].detail), "%s", p[4] ? p[4] : "");
		snprintf(out[n].why, sizeof(out[n].why), "%s", p[5] ? p[5] : "");
		n++;
	}
	if (d)
		closedir(d);
	return n;
}

int perm_answer(const char *id, int allow, const char *msg)
{
	char root[4200], path[4600], w[128];
	FILE *f;

	snprintf(w, sizeof(w), "%s", id);
	safe_name(w);
	perm_dir(root, sizeof(root));
	snprintf(path, sizeof(path), "%s/%s.req", root, w);
	if (access(path, F_OK) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s.ans", root, w);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "%s\t%s\n", allow ? "allow" : "deny", msg ? msg : "");
	fclose(f);
	return 0;
}

int cmd_perm(int argc, char **argv)
{
	perm_t *q = calloc(32, sizeof(*q));
	int n;

	if (!q)
		return 1;
	n = perm_load(q, 32);
	if (argc >= 2 && (strcmp(argv[0], "allow") == 0 || strcmp(argv[0], "deny") == 0)) {
		int rc = perm_answer(argv[1], argv[0][0] == 'a', argc > 2 ? argv[2] : "");

		if (rc < 0)
			fprintf(stderr, "cait: no such pending request: %s\n", argv[1]);
		else
			printf("%s: %s\n", argv[0], argv[1]);
		free(q);
		return rc < 0 ? 1 : 0;
	}
	if (argc > 0 && strcmp(argv[0], "--json") != 0) {
		fprintf(stderr, "usage: cait perm [--json] | allow ID | deny ID [reason]\n");
		free(q);
		return 1;
	}
	for (int i = 0; i < n; i++) {
		if (argc > 0) {
			printf("{\"id\":");
			put_json_string(stdout, q[i].id, strlen(q[i].id));
			printf(",\"agent\":");
			put_json_string(stdout, q[i].agent, strlen(q[i].agent));
			printf(",\"tool\":");
			put_json_string(stdout, q[i].tool, strlen(q[i].tool));
			printf(",\"detail\":");
			put_json_string(stdout, q[i].detail, strlen(q[i].detail));
			printf(",\"why\":");
			put_json_string(stdout, q[i].why, strlen(q[i].why));
			printf("}\n");
		} else {
			printf("%s\t%s\t%s %s\n", q[i].id, q[i].agent, q[i].tool, q[i].detail);
		}
	}
	if (n == 0 && argc == 0)
		printf("no pending requests\n");
	free(q);
	return 0;
}

/*
 * Put one question on the cait screen and wait for the user's answer.
 * Returns 1 (allow), 0 (deny, reason in msg) or -1 (no answer in time).
 */
int perm_ask(const char *in, const char *id, const char *tool, const char *detail, const char *why_asked,
		    int wait_s, char *msg, size_t msize)
{
	char name[256] = "", root[4200], path[4600], rid[64], t[64], d[1024], y[256], ans[1200];
	FILE *f;

	msg[0] = '\0';
	snprintf(t, sizeof(t), "%s", tool);
	snprintf(d, sizeof(d), "%s", detail);
	snprintf(y, sizeof(y), "%s", why_asked);
	flatten(t);
	flatten(d);
	flatten(y);
	if (*id && !agent_name_of(id, name, sizeof(name)))
		snprintf(name, sizeof(name), "%.8s", id);
	flatten(name);
	if (ensure_dir() < 0)
		return -1;
	perm_dir(root, sizeof(root));
	mkdir(root, 0755);
	/* keep the last request as the hook got it: handy to see which fields Claude Code sends */
	snprintf(path, sizeof(path), "%s/last-request.json", root);
	f = fopen(path, "w");
	if (f) {
		fputs(in, f);
		fclose(f);
	}
	snprintf(rid, sizeof(rid), "%ld-%ld", (long)time(NULL), (long)getpid());
	snprintf(path, sizeof(path), "%s/%s.req", root, rid);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "%ld\t%s\t%s\t%s\t%s\t%s\n", (long)time(NULL), id, name, t, d, y);
	fclose(f);

	for (int waited = 0; waited < wait_s * 1000; waited += 200) {
		struct timespec ts = { 0, 200 * 1000 * 1000 };
		char *why;

		snprintf(path, sizeof(path), "%s/%s.ans", root, rid);
		f = fopen(path, "r");
		if (f) {
			ans[0] = '\0';
			if (fgets(ans, sizeof(ans), f)) {
				int ok;

				ans[strcspn(ans, "\n")] = '\0';
				why = strchr(ans, '\t');
				if (why)
					*why++ = '\0';
				ok = strcmp(ans, "allow") == 0;
				snprintf(msg, msize, "%s", why ? why : "");
				fclose(f);
				unlink(path);
				snprintf(path, sizeof(path), "%s/%s.req", root, rid);
				unlink(path);
				return ok;
			}
			fclose(f);
		}
		nanosleep(&ts, NULL);
	}
	snprintf(path, sizeof(path), "%s/%s.req", root, rid);
	unlink(path);
	return -1;
}

/*
 * PermissionRequest hook: Claude Code is about to ask the user to allow a tool call.
 * Put the question on the cait screen; with no answer in time, print nothing so
 * the normal prompt appears.
 */
int perm_hook(const char *in)
{
	char id[128] = "", tool[64] = "", detail[1024] = "", msg[1200], line[1500];
	const char *w = getenv("CAIT_PERM_WAIT");
	int r;

	json_value(in, "agent_id", id, sizeof(id));
	json_value(in, "tool_name", tool, sizeof(tool));
	if (!json_text(in, "command", detail, sizeof(detail)))
		json_value(in, "file_path", detail, sizeof(detail));
	r = perm_ask(in, id, tool, detail, "Claude Code가 사용자 확인을 요청했습니다", w && atoi(w) > 0 ? atoi(w) : 40,
		     msg, sizeof(msg));
	if (r == 1) {
		printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PermissionRequest\",\"decision\":{\"behavior\":\"allow\"}}}\n");
	} else if (r == 0) {
		snprintf(line, sizeof(line), "The user denied this on the cait screen%s%s.", *msg ? ": " : "", msg);
		printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PermissionRequest\",\"decision\":{\"behavior\":\"deny\",\"message\":");
		put_json_string(stdout, line, strlen(line));
		printf("}}}\n");
	}
	return 0;
}
