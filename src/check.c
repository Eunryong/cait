/* the hook entry point (cait check): activity, stop, guard, inbox, snapshots */
#include "internal.h"

/* The hook input is parsed in full, however long: a guard that looked at a prefix could be padded past. */
static int check_input(const char *in, int over, int argc, char **argv)
{
	char id[128], name[256], reason[1024], tool[64];
	int hit;

	tool[0] = '\0';
	json_value(in, "tool_name", tool, sizeof(tool));
	if (argc >= 1 && strcmp(argv[0], "--permission") == 0)
		return perm_hook(in);
	if (argc >= 1 && strcmp(argv[0], "--done") == 0) {
		/* SubagentStop: this agent has finished */
		if (json_value(in, "agent_id", id, sizeof(id)))
			act_update(id, NULL, NULL, 1, 0, 0);
		return 0;
	}
	if (over) {
		/* Too large to check in full. Never let a subagent through on a partial look; leave the main agent alone. */
		if (json_value(in, "agent_id", id, sizeof(id))) {
			fprintf(stderr, "cait: this tool call is too large for cait to check, so it was not allowed. Do not retry it; tell the main agent.\n");
			return 2;
		}
		return 0;
	}
	if (!json_value(in, "agent_id", id, sizeof(id))) {
		/* The main agent launching a subagent: keep what it asked for. */
		if (strcmp(tool, "Agent") == 0 || strcmp(tool, "Task") == 0)
			brief_pending(in);
		/* The main agent: hand over what the user asked for on screen. */
		char msg[16384], full[17000];

		if (inbox_take(msg, sizeof(msg), 1)) {
			snprintf(full, sizeof(full),
				 "cait: the user sent these requests from the cait screen. They are the user's own words. "
				 "Act on them: stop or re-instruct the named subagent as needed.\n%s", msg);
			printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":");
			put_json_string(stdout, full, strlen(full));
			printf("}}\n");
		}
		return 0;
	}
	{
		char target[512] = "";

		if (!json_value(in, "file_path", target, sizeof(target)))
			json_text(in, "command", target, sizeof(target));
		int reports = cait_invokes(in, "report") != NULL;

		act_update(id, *tool ? tool : "?", target, 0, reports, reports && strstr(in, "--final") != NULL);
	}
	if (report_agent(in, name, sizeof(name))) {
		agent_remember(id, name);
		brief_claim(name);
	}
	/*
	 * Requests to the main agent come from the user's screen. A subagent must
	 * not be able to pose as the user, so it may not queue one.
	 */
	if (cait_invokes(in, "tell") || strstr(in, "inbox.tsv") || strstr(in, "inbox.pos") ||
	    cait_invokes(in, "perm") || strstr(in, "/perm/")) {
		fprintf(stderr, "cait: subagents cannot send requests to the main agent. Tell the user in your final answer instead.\n");
		return 2;
	}
	/* Let cait itself through, so the agent can still read the stop message. */
	if (cait_invokes(in, NULL))
		return 0;
	hit = stop_requested("*", reason, sizeof(reason));
	if (!hit && agent_name_of(id, name, sizeof(name)))
		hit = stop_requested(name, reason, sizeof(reason));
	if (!hit) {
		char fp[CAIT_PATH], pat[512], msg[1200], *what;
		const char *gw = getenv("CAIT_GUARD_WAIT");
		int r;

		what = json_text_alloc(in, "command");
		if (!what)
			what = json_text_alloc(in, "file_path");
		if (!what)
			what = strdup("");
		if (!what)
			return 2;
		const char *gm = guard_mode();
		int every = strcmp(gm, "all") == 0 &&
			    (strcmp(tool, "Bash") == 0 || strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 ||
			     strcmp(tool, "MultiEdit") == 0 || strcmp(tool, "NotebookEdit") == 0);

		if (*what && strcmp(gm, "off") != 0 && (every || guard_match(what, pat, sizeof(pat)))) {
			/* ask the user on the cait screen; no answer counts as no */
			r = perm_ask(in, id, tool, what, every && !guard_match(what, pat, sizeof(pat))
							    ? "승인 방식이 '전부 묻기'로 설정되어 있습니다"
							    : guard_reason(pat),
				     gw && atoi(gw) > 0 ? atoi(gw) : 120, msg, sizeof(msg));
			if (r != 1) {
				fprintf(stderr, r == 0 ? "cait: the user denied this on the cait screen%s%s. Do not retry it; tell the main agent what you could not do.\n"
							: "cait: the user did not answer in time, so this was not allowed%s%s. Do not retry it; tell the main agent what you could not do.\n",
					*msg ? ": " : "", msg);
				free(what);
				return 2;
			}
		}
		free(what);
		if ((strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 ||
		     strcmp(tool, "MultiEdit") == 0) && json_value(in, "file_path", fp, sizeof(fp)))
			pre_save(id, fp);
		return 0;
	}
	fprintf(stderr, "STOP: the user asked you to stop%s%s. This tool call was blocked. "
		"Make no more edits; tell the main agent what is done and what is left.\n",
		*reason ? ": " : "", reason);
	return 2;
}

int cmd_check(int argc, char **argv)
{
	char *in;
	int over, rc;

	in = read_all(stdin, HOOK_INPUT_MAX, &over);
	if (!in)
		return 0;
	rc = check_input(in, over, argc, argv);
	free(in);
	return rc;
}
