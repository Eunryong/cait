---
name: cait-report
description: Tell the user what you changed as you work. Use when you are a subagent that edits files, right after you finish changing a file.
---

The user is not reading your log. `cait report` puts a short summary of a file change on their screen right away, so they can follow your work without opening anything. Run it through the Bash tool. Your final answer goes to the main agent as usual; do not repeat it here.

```bash
# Right after you finish a file: what role that part of the file plays and what you did to it.
${CAIT:-cait} report --agent "booking-fix" --file /abs/path/src/booking/create.c "When a booking is created, the duplicate-request check now matches on the request key instead of the booking status"

${CAIT:-cait} report --agent "calc" --file /abs/path/calc.c "Added the four basic operations (add, subtract, multiply, divide) to the calculator"
```

Before your first report, check how the user wants it grouped:

```bash
${CAIT:-cait} level
```

- `files`: one report per file, as above.
- `group`: bundle related files into ONE report by repeating `--file`; cait adds up the changed lines. Use it when you touch many files.

```bash
${CAIT:-cait} report --agent "login" --file /abs/a.c --file /abs/b.c --file /abs/c.c "The login check now also rejects expired tokens and starts a fresh session"
```

If `cait report` ends with `STOP: ...` (exit code 3), the user wants you to halt. Make no more edits, run no more commands, and answer right away with what is done and what is left.

Rules:
- Describe the change by what it does in the program, as a short flow or feature: "the login check now also rejects expired tokens", "added the four basic operations to the calculator". Do NOT lead with function, variable or class names. The reader may not know your names, and a bare `create_booking()` is confusing. If you do mention a name, say right next to it what it does, e.g. "added `days_in_month`, which returns how many days a given month has". Never leave a bare name.
- Say what part of the file's job you touched, not which lines. One or two sentences.
- Give a reason only if your task or what you read supports it. Do not guess why.
- Give `--file` the absolute path of the file. cait measures the change from where it is run, and a relative path can point at a different file than the one you edited.
- `--agent` is a short name for your task; use the same one every time.
- Put the text in ONE quoted argument, in the user's language.
- One report per file (or per group), after you are done with it, not once per edit. The report needs at least one `--file`.
- Report every file you edited before you finish. Edits made after your last report are shown to the user as "unreported edits".
- When all your work is done, send one last report, then end: `cait report --agent NAME --final "what you did overall, in plain words"`. It takes no `--file`. The user sees whether your final report arrived.
