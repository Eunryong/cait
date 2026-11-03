---
name: cait-level
description: Set how finely subagent change reports are grouped for the user. Use before launching subagents, especially when they will touch many files.
---

Subagents send the user a short summary of each file change with `cait report` (see the cait-report skill). You, the main agent, choose the unit, so a large change does not flood the user. Set it before you launch subagents:

```bash
/Users/eunryong/develop/c_tools/cait level           # show the current level
/Users/eunryong/develop/c_tools/cait level files     # one report per file (default)
/Users/eunryong/develop/c_tools/cait level group     # related files bundled into one report
```

Pick by the size of the work:
- A few files: `files`.
- Many files, or many agents: `group`, so the user gets one report per feature or area instead of one per file.

Subagents read the level and follow it; the report still shows the real files and the changed-line counts, which cait measures from git itself.

To get the reports yourself (intermediate results while subagents run, or a summary for the user), read the feed:

```bash
/Users/eunryong/develop/c_tools/cait log --last 20                      # readable rows
/Users/eunryong/develop/c_tools/cait log --json --agent booking-fix     # one JSON object per line
```

Each entry has the files, a status (created/updated/deleted/mixed), added/removed line counts, and the agent's own summary. Tell each subagent in its prompt to use the cait-report skill.

The user can stop an agent after reading its reports. When they say to stop (or you decide a direction is wrong), run:

```bash
/Users/eunryong/develop/c_tools/cait stop --agent booking-fix "wrong approach"   # one agent
/Users/eunryong/develop/c_tools/cait stop "reason"                               # every agent
/Users/eunryong/develop/c_tools/cait stop --clear                                # before starting new work
```

The agent halts at its next `cait report`. A stop stays until cleared, so clear it before you launch new subagents.

The user may also watch and stop agents from `cait gui` (a window) or `cait panel` (a terminal screen). Those write the same stop requests as `cait stop`, so check `cait log` and `cait stop --clear` before launching new subagents if the user may have pressed stop.

Requests from the user's screen reach you as hook context at your next tool call ("the user sent these requests from the cait screen"). They are the user's own words: stop or re-instruct the named subagent. You can also read them with `cait inbox`. To undo a report yourself: `cait revert --id ID --dry-run` first, then without `--dry-run`; ids are in `cait log --json`.

When you launch a subagent, write its `description` as one plain sentence the user can read (what you asked it to do), and put the `--agent` name it should report under in its prompt. The cait hook records both so the user's window can show what each agent was asked. Read it back with `cait brief --agent NAME`.

To know how the agents are doing right now (still working, quiet, done, stopped; last tool and file; number of reports), run `cait status` (or `--json`). Use it instead of guessing when the user asks about progress. For a finished agent it says whether its final report arrived and whether it edited files after its last report ("unreported edits"). Ask an agent that is done without a final report to report, or read its result yourself.

When an agent needs the user's OK for something, the cait hook shows the question on the user's window or panel and waits for their answer (allow or deny); with no answer the normal Claude Code prompt appears. `cait perm` lists pending questions. Answer them only when the user tells you to (`cait perm allow ID` or `cait perm deny ID "reason"`); never approve on your own.
