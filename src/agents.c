/* agent identity (id <-> name), before-edit snapshots taken by the hook, and what the main agent asked each subagent */
#include "internal.h"

/* The NAME after --agent in a report command, quotes (even escaped) skipped. */
int report_agent(const char *in, char *out, size_t size)
{
	const char *p = cait_invokes(in, "report");
	size_t n = 0;

	if (p)
		p = strstr(p, "--agent");
	if (!p)
		return 0;
	for (p += 7; *p == ' ' || *p == '\\' || *p == '"' || *p == '\''; p++)
		;
	while (*p && *p != ' ' && *p != '"' && *p != '\'' && *p != '\\' && n + 1 < size)
		out[n++] = *p++;
	out[n] = '\0';
	return n > 0;
}

/* <dir>/names/<id> holds the name of one agent, so finding it never means reading a growing list. */
static void name_file(char *out, size_t size, const char *id)
{
	char dir[CAIT_PATH], w[256];

	snprintf(w, sizeof(w), "%s", id);
	safe_name(w);
	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/names/%s", dir, w);
}

void agent_remember(const char *id, const char *name)
{
	char path[CAIT_PATH], dir[CAIT_PATH], have[256] = "";
	FILE *f;

	if (ensure_dir() < 0)
		return;
	name_file(path, sizeof(path), id);
	f = fopen(path, "r");
	if (f) {
		if (fgets(have, sizeof(have), f))
			have[strcspn(have, "\n")] = '\0';
		fclose(f);
	}
	if (strcmp(have, name) == 0)
		return;	/* already known: the hook calls this on every report, so do not repeat it */
	cait_dir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/names", dir);
	mkdir(path, 0755);
	name_file(path, sizeof(path), id);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", name);
		fclose(f);
	}
	/* the id -> name list is still kept: pre_take reads it the other way, from a name to its ids */
	path_in_dir(path, sizeof(path), "agents");
	f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\t%s\n", id, name);
		fclose(f);
	}
}

int agent_name_of(const char *id, char *name, size_t size)
{
	char path[CAIT_PATH], line[1024], dir[CAIT_PATH];
	FILE *f;
	int found = 0;

	name_file(path, sizeof(path), id);
	f = fopen(path, "r");
	if (f) {	/* an empty file means: known to have no name yet */
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = '\0';
			snprintf(name, size, "%s", line);
			found = *line != '\0';
		}
		fclose(f);
		return found;
	}
	path_in_dir(path, sizeof(path), "agents");	/* records from before names/ existed */
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');

		if (!tab)
			continue;
		*tab++ = '\0';
		tab[strcspn(tab, "\n")] = '\0';
		if (strcmp(line, id) == 0) {
			snprintf(name, size, "%s", tab);
			found = 1;
		}
	}
	if (f)
		fclose(f);
	cait_dir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/names", dir);
	mkdir(path, 0755);
	name_file(path, sizeof(path), id);	/* remember the answer, or that there is none */
	f = fopen(path, "w");
	if (f) {
		if (found)
			fprintf(f, "%s\n", name);
		fclose(f);
	}
	return found;
}

void pre_path(char *out, size_t size, const char *who, const char *file, const char *suffix)
{
	char dir[CAIT_PATH], w[256];
	unsigned long long h = 1469598103934665603ULL;

	for (const char *p = file; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ULL;
	cait_dir(dir, sizeof(dir));
	snprintf(w, sizeof(w), "%s", who);
	safe_name(w);
	snprintf(out, size, "%s/pre/%s/%016llx%s", dir, w, h, suffix);
}

/* Keep the file as it is now, unless an earlier edit since the last report already did. */
void pre_save(const char *who, const char *file)
{
	char dir[CAIT_PATH], root[CAIT_PATH], w[256], p[CAIT_PATH], mk[CAIT_PATH];
	struct stat st;

	if (ensure_dir() < 0)
		return;
	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/pre", dir);
	snprintf(w, sizeof(w), "%s", who);
	safe_name(w);
	mkdir_p2(root, w);
	pre_path(p, sizeof(p), who, file, "");
	pre_path(mk, sizeof(mk), who, file, ".deleted");
	if (access(p, F_OK) == 0 || access(mk, F_OK) == 0)
		return;
	if (stat(file, &st) != 0) {
		FILE *m = fopen(mk, "w");

		if (m)
			fclose(m);
		return;
	}
	if (st.st_size > (1 << 20))
		return;
	copy_file(file, p);
}

/* ---- what the main agent asked each subagent to do ---- */

void brief_pending(const char *in)
{
	char desc[512] = "", dir[CAIT_PATH], root[CAIT_PATH], path[CAIT_PATH];
	char *prompt = json_text_alloc(in, "prompt");
	FILE *f;

	if (!prompt)
		return;
	json_text(in, "description", desc, sizeof(desc));
	flatten(desc);
	if (ensure_dir() < 0) {
		free(prompt);
		return;
	}
	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/brief", dir);
	mkdir_p2(root, "pending");
	snprintf(path, sizeof(path), "%s/pending/%ld-%05d", root, (long)time(NULL), (int)getpid());
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", desc, prompt);
		fclose(f);
	}
	free(prompt);
}

/* An agent just named itself: give it the pending brief that mentions that name, else the oldest. */
void brief_claim(const char *name)
{
	char dir[CAIT_PATH], pdir[CAIT_PATH], safe[256], dst[CAIT_PATH], path[CAIT_PATH], oldest[256] = "";
	struct dirent *e;
	DIR *d;

	cait_dir(dir, sizeof(dir));
	snprintf(pdir, sizeof(pdir), "%s/brief/pending", dir);
	snprintf(safe, sizeof(safe), "%s", name);
	safe_name(safe);
	snprintf(dst, sizeof(dst), "%s/brief/%s", dir, safe);
	d = opendir(pdir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char *buf;
		int over;
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", pdir, e->d_name);
		if (!*oldest || strcmp(e->d_name, oldest) < 0)
			snprintf(oldest, sizeof(oldest), "%s", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		buf = read_all(f, HOOK_INPUT_MAX, &over);	/* the name may come late in a long prompt */
		fclose(f);
		if (buf && strstr(buf, name)) {
			free(buf);
			rename(path, dst);
			closedir(d);
			return;
		}
		free(buf);
	}
	closedir(d);
	if (*oldest && access(dst, F_OK) != 0) {
		snprintf(path, sizeof(path), "%s/%s", pdir, oldest);
		rename(path, dst);
	}
}

int cmd_brief(int argc, char **argv)
{
	char dir[CAIT_PATH], path[CAIT_PATH], line[4096];
	const char *agent = NULL;
	FILE *f;

	for (int i = 0; i < argc; i++)
		if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc)
			agent = argv[++i];
	cait_dir(dir, sizeof(dir));
	if (!agent) {
		struct dirent *e;
		DIR *d;

		snprintf(path, sizeof(path), "%s/brief", dir);
		d = opendir(path);
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.' || strcmp(e->d_name, "pending") == 0)
				continue;
			snprintf(path, sizeof(path), "%s/brief/%s", dir, e->d_name);
			f = fopen(path, "r");
			if (f && fgets(line, sizeof(line), f))
				printf("%s\t%s", e->d_name, line);
			if (f)
				fclose(f);
		}
		if (d)
			closedir(d);
		return 0;
	}
	snprintf(line, sizeof(line), "%s", agent);
	safe_name(line);
	snprintf(path, sizeof(path), "%s/brief/%s", dir, line);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cait: no brief recorded for that agent\n");
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
	return 0;
}
