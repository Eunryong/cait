/* paths, the feed directory, JSON and file helpers shared by every command */
#include "internal.h"

void cait_dir(char *out, size_t size)
{
	const char *dir = getenv("CAIT_DIR");
	const char *home = getenv("HOME");

	/*
	 * Never derive this from the current directory: a subagent that cds
	 * into its repo would then write a feed the user is not watching.
	 */
	if (dir && *dir)
		snprintf(out, size, "%s", dir);
	else
		snprintf(out, size, "%s/.cait", home && *home ? home : "/tmp");
}

void path_in_dir(char *out, size_t size, const char *name)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/%s", dir, name);
}

int ensure_dir(void)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	return mkdir(dir, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* Tabs, newlines and our separator would break the line format. */
void flatten(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r' || *s == FILE_SEP)
			*s = ' ';
}

/* Run argv without a shell; capture stdout. Returns exit status or -1. */
int run(char *const argv[], char *out, size_t cap)
{
	int fds[2];
	pid_t pid;
	size_t len = 0;
	int status;

	if (out && cap)
		out[0] = '\0';
	if (pipe(fds) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(fds[1], STDOUT_FILENO);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			dup2(null, STDIN_FILENO);
		}
		close(fds[0]);
		close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		char tmp[1024];
		ssize_t n = read(fds[0], tmp, sizeof(tmp));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (out && len + 1 < cap) {
			size_t take = (size_t)n < cap - len - 1 ? (size_t)n : cap - len - 1;
			memcpy(out + len, tmp, take);
			len += take;
		}
	}
	close(fds[0]);
	if (out && cap)
		out[len] = '\0';
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * PreToolUse hook (Claude Code only): refuses a subagent's tool call while a
 * stop request names it. The hook input is JSON on stdin; only calls from
 * subagents carry "agent_id", so the main agent is never blocked.
 *
 * The hook sees an id, the user names agents by --agent. The two meet when the
 * agent runs `cait report --agent NAME`: that Bash call carries both, so we
 * remember id -> NAME in <dir>/agents. Until an agent has reported once, only
 * a stop-all can reach it.
 */

/* Copy the JSON string value following `"key":` into out; 0 if absent. */
int json_value(const char *in, const char *key, char *out, size_t size)
{
	char pat[64];
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(in, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	while (p && *++p == ' ')
		;
	if (!p || *p != '"')
		return 0;
	for (p++; *p && *p != '"' && n + 1 < size; p++)
		out[n++] = *p;
	out[n] = '\0';
	return n > 0;
}

/* ---- before-edit snapshots: the hook runs before Edit/Write, so it sees the "before" ---- */

/* Like json_value, but understands \" \\ \n escapes (prompts are full of them). */
int json_text(const char *in, const char *key, char *out, size_t size)
{
	char pat[64];
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(in, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	while (p && *++p == ' ')
		;
	if (!p || *p != '"')
		return 0;
	for (p++; *p && *p != '"' && n + 1 < size; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			out[n++] = *p == 'n' ? '\n' : *p == 't' ? ' ' : *p;
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
	return n > 0;
}

void safe_name(char *s)
{
	for (; *s; s++)
		if (*s == '/' || *s == '\\')
			*s = '_';
}

void json_str_out(FILE *o, const char *s)
{
	put_json_string(o, s, strlen(s));
}

/* A tool call in words a person reads: "Edit /a/cart.c" -> "수정 중: cart.c". */
void clip_utf8(char *s, size_t max)
{
	size_t n = strlen(s);

	if (n <= max)
		return;
	while (max > 0 && ((unsigned char)s[max] & 0xc0) == 0x80)
		max--;
	snprintf(s + max, 4, "…");
}

void mkdir_p2(const char *a, const char *b)
{
	char path[4200];

	snprintf(path, sizeof(path), "%s", a);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s", a, b);
	mkdir(path, 0755);
}

int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[8192];
	size_t n;

	if (!in)
		return -1;
	out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	return fclose(out);
}

void put_json_string(FILE *o, const char *s, size_t len)
{
	fputc('"', o);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '"' || c == '\\')
			fprintf(o, "\\%c", c);
		else if (c < 0x20)
			fprintf(o, "\\u%04x", c);
		else
			fputc(c, o);
	}
	fputc('"', o);
}

/*
 * Does the hook input contain a call to cait? Agents write it as `cait report`, `/path/cait report`,
 * `$CAIT report` or `${CAIT:-cait} report`, so look for the word cait, skip the quotes and braces
 * after it, and require a space and then `sub` (any subcommand when sub is NULL). Returns a pointer
 * to the subcommand, or NULL.
 */
const char *cait_invokes(const char *in, const char *sub)
{
	for (const char *p = in; *p; p++) {
		const char *q;

		if (strncasecmp(p, "cait", 4) != 0)
			continue;
		q = p + 4;
		while (*q == '}' || *q == '"' || *q == '\'' || *q == '\\')
			q++;
		if (*q != ' ')
			continue;
		while (*q == ' ')
			q++;
		if (!sub || strncmp(q, sub, strlen(sub)) == 0)
			return q;
	}
	return NULL;
}
