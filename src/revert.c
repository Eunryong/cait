/* report ids, snapshots, feed append, and undoing a report */
#include "internal.h"

/* ---- report ids, snapshots, feed append ---- */

/* A report has no id column; derive one from what identifies it. */
void rec_key(time_t t, const char *agent, const char *files, char *out, size_t size)
{
	unsigned long long h = 1469598103934665603ULL;
	char head[64];
	const char *parts[3] = { head, agent, files };

	snprintf(head, sizeof(head), "%ld", (long)t);
	for (int i = 0; i < 3; i++) {
		for (const char *p = parts[i]; *p; p++)
			h = (h ^ (unsigned char)*p) * 1099511628211ULL;
		h = (h ^ 0xff) * 1099511628211ULL;
	}
	snprintf(out, size, "%016llx", h);
}

/* Directory holding what one report's files looked like right after it. */
static void snap_path(char *out, size_t size, const char *root, const char *key, int idx, const char *suffix)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/%s/%s/%d%s", dir, root, key, idx, suffix);
}

/*
 * Move the before-edit snapshot the hook took for `file` into this report, as
 * <idx>.before (or <idx>.before.deleted when the file did not exist yet). The
 * hook saved it under the agent's id; the agents file maps ids to the name.
 */
void pre_take(const char *agent, const char *file, const char *key, int idx)
{
	char path[4200], line[1024], from[4600], mk[4600], to[4400];
	FILE *f;

	path_in_dir(path, sizeof(path), "agents");
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');

		if (!tab)
			continue;
		*tab++ = '\0';
		tab[strcspn(tab, "\n")] = '\0';
		if (strcmp(tab, agent) != 0)
			continue;
		pre_path(from, sizeof(from), line, file, "");
		pre_path(mk, sizeof(mk), line, file, ".deleted");
		if (access(from, F_OK) == 0) {
			snap_path(to, sizeof(to), "snap", key, idx, ".before");
			rename(from, to);
			break;
		}
		if (access(mk, F_OK) == 0) {
			snap_path(to, sizeof(to), "snap", key, idx, ".before.deleted");
			rename(mk, to);
			break;
		}
	}
	fclose(f);
}

void snap_save(const char *key, int idx, const char *file)
{
	char dir[4096], root[4200], to[4400];
	struct stat st;

	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/snap", dir);
	mkdir_p2(root, key);
	if (stat(file, &st) != 0) {
		FILE *m;

		snap_path(to, sizeof(to), "snap", key, idx, ".deleted");
		m = fopen(to, "w");
		if (m)
			fclose(m);
		return;
	}
	if (st.st_size > (1 << 20))
		return;	/* too big to keep: this file cannot be restored from a snapshot */
	snap_path(to, sizeof(to), "snap", key, idx, "");
	copy_file(file, to);
}

int feed_append(const char *line, size_t n)
{
	char path[4200];
	int fd;
	ssize_t w;

	path_in_dir(path, sizeof(path), "feed.tsv");
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return -1;
	/* Parallel subagents report at once: keep each line whole. */
	flock(fd, LOCK_EX);
	w = write(fd, line, n);
	flock(fd, LOCK_UN);
	close(fd);
	return w == (ssize_t)n ? 0 : -1;
}

/* ---- undo: put a report's files back to how they were before it ---- */

typedef struct {
	char *line;
	rec_t r;
	char key[24];
} frec_t;

static void split_dir_base(const char *file, char *dir, size_t dsize, char *base, size_t bsize)
{
	const char *slash = strrchr(file, '/');

	if (slash && slash != file) {
		snprintf(dir, dsize, "%.*s", (int)(slash - file), file);
		snprintf(base, bsize, "%s", slash + 1);
	} else {
		snprintf(dir, dsize, "%s", slash ? "/" : ".");
		snprintf(base, bsize, "%s", slash ? slash + 1 : file);
	}
}

/* Does the \x1f-joined list `files` contain exactly `file`? Returns its index or -1. */
static int file_index(const char *files, const char *file)
{
	size_t len = strlen(file);
	int idx = 0;

	for (const char *p = files;; idx++) {
		const char *e = strchr(p, FILE_SEP);
		size_t n = e ? (size_t)(e - p) : strlen(p);

		if (n == len && strncmp(p, file, len) == 0)
			return idx;
		if (!e)
			return -1;
		p = e + 1;
	}
}

int cmd_revert(int argc, char **argv)
{
	const char *id = NULL;
	int dry = 0, bad = 0, target = -1;
	frec_t *recs = NULL;
	size_t n = 0, cap = 0, len = 0;
	char path[4200], *line = NULL, dir[4096], files[8192];
	FILE *f;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--id") == 0 && i + 1 < argc)
			id = argv[++i];
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry = 1;
		else
			id = NULL;
	}
	if (!id) {
		fprintf(stderr, "usage: cait revert --id KEY [--dry-run]   (KEY: see `cait log --json`)\n");
		return 1;
	}
	path_in_dir(path, sizeof(path), "feed.tsv");
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cait: nothing has been reported yet\n");
		return 1;
	}
	while (getline(&line, &len, f) > 0) {
		frec_t fr;

		fr.line = strdup(line);
		if (!fr.line || !parse_rec(fr.line, &fr.r))
			continue;
		rec_key(fr.r.time, fr.r.agent, fr.r.files, fr.key, sizeof(fr.key));
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			recs = realloc(recs, cap * sizeof(*recs));
			if (!recs)
				return 1;
		}
		recs[n++] = fr;
	}
	free(line);
	fclose(f);
	for (size_t i = 0; i < n; i++)
		if (strcmp(recs[i].key, id) == 0)
			target = (int)i;
	if (target < 0) {
		fprintf(stderr, "cait: no report with that id\n");
		return 1;
	}
	if (strcmp(recs[target].r.status, "final") == 0) {
		fprintf(stderr, "cait: a final report has no file changes to undo\n");
		return 1;
	}
	if (strcmp(recs[target].r.status, "reverted") == 0) {
		fprintf(stderr, "cait: that entry is already an undo\n");
		return 1;
	}

	cait_dir(dir, sizeof(dir));
	snprintf(files, sizeof(files), "%s", recs[target].r.files);
	{
		char *p = files, *file;
		int idx = 0;

		for (;; idx++) {
			char *e = strchr(p, FILE_SEP), what[256], backup[4400], root[4200], bs[4400], bm[4400];
			int prev = -1, later = 0, prev_idx = -1, ok = 0;

			file = p;
			if (e)
				*e = '\0';
			/* keep what we are about to overwrite, so even an undo can be undone */
			if (!dry && access(file, F_OK) == 0) {
				snprintf(root, sizeof(root), "%s/undo", dir);
				mkdir_p2(root, recs[target].key);
				snprintf(backup, sizeof(backup), "%s/undo/%s/%d", dir, recs[target].key, idx);
				copy_file(file, backup);
			}
			for (int q = 0; q < target; q++) {
				int fi = file_index(recs[q].r.files, file);

				if (fi >= 0 && strcmp(recs[q].r.status, "reverted") != 0) {
					prev = q;
					prev_idx = fi;
				}
			}
			for (size_t q = (size_t)target + 1; q < n; q++)
				if (file_index(recs[q].r.files, file) >= 0 &&
				    strcmp(recs[q].r.status, "reverted") != 0)
					later++;

			snap_path(bs, sizeof(bs), "snap", recs[target].key, idx, ".before");
			snap_path(bm, sizeof(bm), "snap", recs[target].key, idx, ".before.deleted");
			if (access(bm, F_OK) == 0) {
				snprintf(what, sizeof(what), "remove (the file did not exist before this report's edits)");
				ok = !(!dry && unlink(file) != 0 && errno != ENOENT);
			} else if (access(bs, F_OK) == 0) {
				snprintf(what, sizeof(what), "restore the file as it was just before this report's edits");
				ok = dry || copy_file(bs, file) == 0;
			} else if (prev >= 0) {
				char sp[4400], mark[4400];

				snap_path(sp, sizeof(sp), "snap", recs[prev].key, prev_idx, "");
				snap_path(mark, sizeof(mark), "snap", recs[prev].key, prev_idx, ".deleted");
				if (access(mark, F_OK) == 0) {
					snprintf(what, sizeof(what), "remove (the earlier report had deleted it)");
					ok = 1;
					if (!dry && unlink(file) != 0 && errno != ENOENT)
						ok = 0;
				} else if (access(sp, F_OK) == 0) {
					snprintf(what, sizeof(what), "restore the version right after the earlier report");
					ok = 1;
				} else {
					snprintf(what, sizeof(what), "no snapshot of the earlier version");
				}
				if (ok && !dry && access(mark, F_OK) != 0)
					ok = copy_file(sp, file) == 0;
			} else {
				char bdir[1024], base[1024];
				char *in_repo[] = { "git", "-C", bdir, "rev-parse", "--is-inside-work-tree", NULL };
				char *tracked[] = { "git", "-C", bdir, "ls-files", "--error-unmatch", "--", base, NULL };
				char *head[] = { "git", "-C", bdir, "rev-parse", "-q", "--verify", "HEAD", NULL };
				char *checkout[] = { "git", "-C", bdir, "checkout", "HEAD", "--", base, NULL };

				split_dir_base(file, bdir, sizeof(bdir), base, sizeof(base));
				if (run(in_repo, NULL, 0) != 0) {
					snprintf(what, sizeof(what), "cannot undo: no earlier version known (not a git repo)");
				} else if (run(tracked, NULL, 0) == 0 && run(head, NULL, 0) == 0) {
					snprintf(what, sizeof(what), "restore the last committed version");
					ok = 1;
				} else {
					snprintf(what, sizeof(what), "remove (this report created the file)");
					ok = 1;
				}
				if (ok && !dry) {
					if (strncmp(what, "remove", 6) == 0)
						ok = unlink(file) == 0 || errno == ENOENT;
					else
						ok = run(checkout, NULL, 0) == 0;
				}
			}

			printf("%s\t%s%s\t%s\n", ok ? "ok" : "fail", dry ? "[dry-run] " : "", what, file);
			if (later)
				printf("note\t%d later report(s) also changed this file; their changes go too\t%s\n",
				       later, file);
			if (!ok)
				bad = 1;
			if (!e)
				break;
			p = e + 1;
		}
	}
	if (!dry && !bad) {
		char out[16384];
		time_t now = time(NULL);
		int m = snprintf(out, sizeof(out), "%ld\t%s\t%s\treverted\t\t\t%s\n", (long)now,
				 recs[target].r.agent, recs[target].r.files,
				 "사용자가 이 보고의 변경을 되돌렸습니다");

		if (m > 0 && (size_t)m < sizeof(out))
			feed_append(out, (size_t)m);
	}
	for (size_t i = 0; i < n; i++)
		free(recs[i].line);
	free(recs);
	return bad ? 1 : 0;
}
