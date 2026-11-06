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
	char dir[CAIT_PATH];

	cait_dir(dir, sizeof(dir));
	if (snprintf(out, size, "%s/%s", dir, name) >= (int)size)
		out[0] = '\0';	/* too long for the caller's buffer: an empty path fails to open instead of opening a cut one */
}

int ensure_dir(void)
{
	char dir[CAIT_PATH];

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
	char path[CAIT_PATH];

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

/*
 * Read a whole stream into a malloc'd, NUL-terminated buffer that grows as needed, up to max bytes.
 * *over is set when the stream was longer than max; what was read so far is still returned.
 */
char *read_all(FILE *f, size_t max, int *over)
{
	size_t cap = 8192, len = 0, n;
	char *buf = malloc(cap + 1);

	*over = 0;
	if (!buf)
		return NULL;
	while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap) {
			char *bigger;

			if (cap >= max) {
				char probe;

				if (fread(&probe, 1, 1, f) == 1)
					*over = 1;
				break;
			}
			cap = cap * 2 > max ? max : cap * 2;
			bigger = realloc(buf, cap + 1);
			if (!bigger) {
				free(buf);
				return NULL;
			}
			buf = bigger;
		}
	}
	buf[len] = '\0';
	return buf;
}

/* Like json_text, but the whole value, however long: a malloc'd string, or NULL when absent or empty. */
char *json_text_alloc(const char *in, const char *key)
{
	char pat[64], *out;
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(in, pat);
	if (!p)
		return NULL;
	p = strchr(p + strlen(pat), ':');
	while (p && *++p == ' ')
		;
	if (!p || *p != '"')
		return NULL;
	out = malloc(strlen(p) + 1);	/* the decoded value is never longer than the text after it */
	if (!out)
		return NULL;
	for (p++; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			out[n++] = *p == 'n' ? '\n' : *p == 't' ? ' ' : *p;
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
	if (n == 0) {
		free(out);
		return NULL;
	}
	return out;
}

/* ---- a small string -> (long, long) map: open addressing, grows as needed ---- */

struct strmap {
	char **key;
	long *a, *b;
	size_t cap, used;
};

static size_t str_hash(const char *s)
{
	size_t h = 1469598103934665603ULL;

	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 1099511628211ULL;
	return h;
}

strmap_t *strmap_new(void)
{
	strmap_t *m = calloc(1, sizeof(*m));

	if (!m)
		return NULL;
	m->cap = 64;
	m->key = calloc(m->cap, sizeof(*m->key));
	m->a = calloc(m->cap, sizeof(*m->a));
	m->b = calloc(m->cap, sizeof(*m->b));
	if (!m->key || !m->a || !m->b) {
		strmap_free(m);
		return NULL;
	}
	return m;
}

void strmap_free(strmap_t *m)
{
	if (!m)
		return;
	for (size_t i = 0; i < m->cap && m->key; i++)
		free(m->key[i]);
	free(m->key);
	free(m->a);
	free(m->b);
	free(m);
}

static size_t strmap_slot(const strmap_t *m, const char *k)
{
	size_t i = str_hash(k) & (m->cap - 1);

	while (m->key[i] && strcmp(m->key[i], k) != 0)
		i = (i + 1) & (m->cap - 1);
	return i;
}

int strmap_get(const strmap_t *m, const char *k, long *a, long *b)
{
	size_t i = strmap_slot(m, k);

	if (!m->key[i])
		return 0;
	if (a)
		*a = m->a[i];
	if (b)
		*b = m->b[i];
	return 1;
}

int strmap_put(strmap_t *m, const char *k, long a, long b)
{
	size_t i;

	if ((m->used + 1) * 2 > m->cap) {	/* keep it at most half full */
		strmap_t grown = { calloc(m->cap * 2, sizeof(*m->key)), calloc(m->cap * 2, sizeof(*m->a)),
				   calloc(m->cap * 2, sizeof(*m->b)), m->cap * 2, 0 };

		if (!grown.key || !grown.a || !grown.b) {
			free(grown.key);
			free(grown.a);
			free(grown.b);
			return -1;
		}
		for (size_t j = 0; j < m->cap; j++) {
			if (!m->key[j])
				continue;
			i = strmap_slot(&grown, m->key[j]);
			grown.key[i] = m->key[j];
			grown.a[i] = m->a[j];
			grown.b[i] = m->b[j];
			grown.used++;
		}
		free(m->key);
		free(m->a);
		free(m->b);
		*m = grown;
	}
	i = strmap_slot(m, k);
	if (!m->key[i]) {
		m->key[i] = strdup(k);
		if (!m->key[i])
			return -1;
		m->used++;
	}
	m->a[i] = a;
	m->b[i] = b;
	return 0;
}
