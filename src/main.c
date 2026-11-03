/*
 * cait: subagents tell the user what they changed, as it happens.
 *
 *   cait report ...   subagent -> feed        (feed.c)
 *   cait level ...    main agent sets volume  (feed.c)
 *   cait watch        user reads the feed     (feed.c)
 */
#include <stdio.h>
#include <string.h>

#include "cait.h"

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "report") == 0)
		return cmd_report(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "level") == 0)
		return cmd_level(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "stop") == 0)
		return cmd_stop(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "gui") == 0)
		return cmd_gui(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "panel") == 0)
		return cmd_panel(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "revert") == 0)
		return cmd_revert(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "tell") == 0)
		return cmd_tell(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "inbox") == 0)
		return cmd_inbox(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "status") == 0)
		return cmd_status(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "brief") == 0)
		return cmd_brief(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "perm") == 0)
		return cmd_perm(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "guard") == 0)
		return cmd_guard(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "check") == 0)
		return cmd_check(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "watch") == 0)
		return cmd_watch(argc - 2, argv + 2);
	if (argc >= 2 && strcmp(argv[1], "log") == 0)
		return cmd_log(argc - 2, argv + 2);

	fprintf(stderr,
		"usage: cait report [--agent NAME] --file PATH [--file PATH ...] \"summary\"   (subagents)\n"
		"       cait level [files|group]                                         (main agent)\n"
		"       cait log [--json] [--agent NAME] [--last N]                      (main agent, panels)\n"
		"       cait watch [--notify] [--once]                                   (user; keys s/x/c stop agents)\n"
		"       cait gui                                                         (user; window, macOS)\n"
		"       cait panel                                                       (user; full-screen terminal view)\n"
		"       cait stop [--agent NAME] [reason] | --clear                      (user, main agent)\n"
		"       cait revert --id KEY [--dry-run]                                 (screens, main agent)\n"
		"       cait tell [--agent NAME] \"request\" | cait inbox                  (screens, main agent)\n"
		"       cait status [--json]                                             (what every agent is doing now)\n"
		"       cait brief [--agent NAME]                                        (what the main agent asked an agent)\n"
		"       cait guard [add PATTERN]                                         (what subagents may not do without your OK)\n"
		"       cait perm [--json] | allow ID | deny ID [reason]                 (permission requests from agents)\n");
	return 1;
}
