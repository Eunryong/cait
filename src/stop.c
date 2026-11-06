/* stop requests: the user asks an agent to halt */
#include "internal.h"

/* ---- stop requests: the user asks an agent to halt ---- */

/*
 * <dir>/stop holds one request per line: epoch, agent ("*" = everyone), reason.
 * A subagent learns of it at its next `cait report`; it stays until cleared.
 */

int stop_add(const char *agent, const char *reason)
{
	char path[4200], who[256], why[1024];
	FILE *f;

	if (ensure_dir() < 0)
		return -1;
	snprintf(who, sizeof(who), "%s", agent);
	snprintf(why, sizeof(why), "%s", reason);
	flatten(who);
	flatten(why);
	path_in_dir(path, sizeof(path), "stop");
	f = fopen(path, "a");
	if (!f)
		return -1;
	fprintf(f, "%ld\t%s\t%s\n", (long)time(NULL), who, why);
	fclose(f);
	return 0;
}

/* Returns 1 and fills reason if a request names this agent or everyone. */
int stop_requested(const char *agent, char *reason, size_t size)
{
	char path[4200], line[2048];
	FILE *f;
	int hit = 0;

	path_in_dir(path, sizeof(path), "stop");
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (!hit && fgets(line, sizeof(line), f)) {
		char *who = strchr(line, '\t'), *why;

		if (!who)
			continue;
		why = strchr(++who, '\t');
		if (!why)
			continue;
		*why++ = '\0';
		why[strcspn(why, "\n")] = '\0';
		if (strcmp(who, "*") == 0 || strcmp(who, agent) == 0) {
			snprintf(reason, size, "%s", why);
			hit = 1;
		}
	}
	fclose(f);
	return hit;
}

int cmd_stop(int argc, char **argv)
{
	const char *agent = "*";
	char reason[1024] = "";
	size_t len = 0;
	char path[4200];

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--clear") == 0) {
			path_in_dir(path, sizeof(path), "stop");
			unlink(path);
			printf("stop requests cleared\n");
			return 0;
		} else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
			agent = argv[++i];
		} else {
			len += (size_t)snprintf(reason + len, sizeof(reason) - len, "%s%s",
						len ? " " : "", argv[i]);
			if (len >= sizeof(reason))
				len = sizeof(reason) - 1;
		}
	}
	if (stop_add(agent, reason) < 0) {
		perror("cait: cannot write stop request");
		return 1;
	}
	printf("stop requested for %s: the agent halts at its next report\n",
	       strcmp(agent, "*") == 0 ? "all agents" : agent);
	return 0;
}
