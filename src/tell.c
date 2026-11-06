/* requests from the user's screen to the main agent (tell / inbox) */
#include "internal.h"

/* ---- tell: the user's request for the main agent ---- */

/*
 * <dir>/inbox.tsv: epoch, agent, text. <dir>/inbox.pos: how far the main agent
 * has read. The PreToolUse hook hands new entries to the main agent as context.
 */
size_t inbox_take(char *out, size_t size, int mark)
{
	char path[4200], line[4096];
	FILE *f;
	long pos = 0;
	size_t len = 0;

	out[0] = '\0';
	path_in_dir(path, sizeof(path), "inbox.pos");
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &pos) != 1)
			pos = 0;
		fclose(f);
	}
	path_in_dir(path, sizeof(path), "inbox.tsv");
	f = fopen(path, "r");
	if (!f)
		return 0;
	fseek(f, pos, SEEK_SET);
	while (fgets(line, sizeof(line), f)) {
		char *who = strchr(line, '\t'), *text;

		if (!who || !strchr(line, '\n'))
			continue;
		*who++ = '\0';
		text = strchr(who, '\t');
		if (!text)
			continue;
		*text++ = '\0';
		text[strcspn(text, "\n")] = '\0';
		len += (size_t)snprintf(out + len, size - len, "- [%s] %s\n", who, text);
		if (len >= size)
			len = size - 1;
	}
	pos = ftell(f);
	fclose(f);
	if (mark && len) {
		path_in_dir(path, sizeof(path), "inbox.pos");
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "%ld\n", pos);
			fclose(f);
		}
	}
	return len;
}

int cmd_tell(int argc, char **argv)
{
	const char *agent = "-";
	char text[3000] = "", path[4200];
	size_t len = 0;
	FILE *f;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
			agent = argv[++i];
		} else {
			len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", len ? " " : "", argv[i]);
			if (len >= sizeof(text))
				len = sizeof(text) - 1;
		}
	}
	if (!*text) {
		fprintf(stderr, "usage: cait tell [--agent NAME] \"request for the main agent\"\n");
		return 1;
	}
	if (ensure_dir() < 0)
		return 1;
	flatten(text);
	path_in_dir(path, sizeof(path), "inbox.tsv");
	f = fopen(path, "a");
	if (!f) {
		perror("cait: cannot write inbox");
		return 1;
	}
	{
		char who[256];

		snprintf(who, sizeof(who), "%s", agent);
		flatten(who);
		fprintf(f, "%ld\t%s\t%s\n", (long)time(NULL), who, text);
	}
	fclose(f);
	printf("queued: the main agent receives it at its next tool call\n");
	return 0;
}

/* The main agent can also read the queue itself. --peek leaves it unread. */
int cmd_inbox(int argc, char **argv)
{
	char out[16384];

	if (inbox_take(out, sizeof(out), !(argc > 0 && strcmp(argv[0], "--peek") == 0)))
		fputs(out, stdout);
	return 0;
}
