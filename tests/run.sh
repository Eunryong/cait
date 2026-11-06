#!/usr/bin/env bash
# Black-box tests: run the cait binary against a throw-away $CAIT_DIR and git repo.
#   make test            # builds, then runs this
#   CAIT=./cait tests/run.sh
set -u
CAIT=${CAIT:-./cait}
CAIT=$(cd "$(dirname "$CAIT")" && pwd)/$(basename "$CAIT")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export CAIT_DIR="$TMP/cait" CAIT_GUARD_WAIT=2 CAIT_PERM_WAIT=2 HOME="$TMP/home"
mkdir -p "$HOME" "$TMP/repo"

pass=0 fail=0
ok()   { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }
has()  { case "$2" in *"$1"*) return 0 ;; *) return 1 ;; esac; }
expect_has()  { if has "$2" "$3"; then ok "$1"; else bad "$1" "wanted '$2' in: $3"; fi; }
expect_lacks() { if has "$2" "$3"; then bad "$1" "did not want '$2' in: $3"; else ok "$1"; fi; }
expect_code() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "exit $3, wanted $2"; fi; }

# a tool call as the hook sees it: kind id tool command file
hook() { # $1 id, $2 tool, $3 command, $4 file
  printf '{"hook_event_name":"PreToolUse","tool_name":"%s","agent_id":"%s","tool_input":{"command":"%s","file_path":"%s"}}' \
    "$2" "$1" "$3" "${4:-}" | "$CAIT" check 2>"$TMP/err"
}
main_hook() { printf '{"tool_name":"%s","tool_input":{"command":"%s"}}' "$1" "$2" | "$CAIT" check 2>"$TMP/err"; }

cd "$TMP/repo" && git init -q . && git config user.email t@t && git config user.name t
printf 'one\n' > a.txt && git add a.txt && git commit -qm init

echo "report and log"
"$CAIT" report --agent alpha --file "$TMP/repo/a.txt" "first" >/dev/null
printf 'one\ntwo\n' > a.txt
out=$("$CAIT" report --agent alpha --file "$TMP/repo/a.txt" "added a line")
expect_has "report is accepted" "shown to the user" "$out"
log=$("$CAIT" log)
expect_has "log shows the agent" "alpha" "$log"
expect_has "log shows the summary" "added a line" "$log"
expect_has "log shows measured lines" "+1" "$log"
json=$("$CAIT" log --json --agent alpha --last 1)
expect_has "json has the agent" '"agent":"alpha"' "$json"
expect_has "json has the summary" '"text":"added a line"' "$json"
expect_lacks "log filters by agent" "alpha" "$("$CAIT" log --agent nobody)"

echo "level"
expect_has "default level is files" "files" "$("$CAIT" level)"
"$CAIT" level group >/dev/null
expect_has "level can be set to group" "group" "$("$CAIT" level)"
out=$("$CAIT" report --agent alpha --file "$TMP/repo/a.txt" "one file under group" 2>&1); code=$?
expect_has "a single file under group is turned down" "group" "$out"
"$CAIT" level files >/dev/null

echo "stop"
"$CAIT" stop --agent alpha "wrong way" >/dev/null
out=$("$CAIT" report --agent alpha --file "$TMP/repo/a.txt" "after stop" 2>&1); code=$?
expect_code "a stopped agent's next report exits 3" 3 "$code"
expect_has "the stop reason is shown" "wrong way" "$out"
hook idA Bash "echo hi" >/dev/null; "$CAIT" report --agent alpha --final "x" >/dev/null 2>&1
hook idA Bash "$CAIT report --agent alpha --file /x \\\"s\\\"" >/dev/null
hook idB Edit "" "$TMP/repo/a.txt"; code=$?
"$CAIT" stop --clear >/dev/null
hook idB Edit "" "$TMP/repo/a.txt"; code2=$?
expect_code "after --clear the agent may work again" 0 "$code2"

echo "hook: stop blocks a named subagent"
"$CAIT" stop --agent beta "halt" >/dev/null
hook idBeta Bash "cait report --agent beta --file /x \\\"s\\\"" >/dev/null   # teaches id -> name
hook idBeta Edit "" "$TMP/repo/a.txt"; code=$?
expect_code "tool call from a stopped agent is refused" 2 "$code"
expect_has "refusal says STOP" "STOP" "$(cat "$TMP/err")"
hook idOther Edit "" "$TMP/repo/a.txt"; expect_code "other agents are not blocked" 0 $?
main_hook Bash "ls"; expect_code "the main agent is never blocked" 0 $?
"$CAIT" stop --clear >/dev/null

echo "revert"
printf 'one\n' > a.txt && git commit -qam base
hook idR Edit "" "$TMP/repo/a.txt" >/dev/null           # hook snapshots the file before the edit
printf 'one\nedited\n' > a.txt
"$CAIT" report --agent rev --file "$TMP/repo/a.txt" "edited" >/dev/null
id=$("$CAIT" log --json --agent rev --last 1 | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
"$CAIT" revert --id "$id" --dry-run >/dev/null
expect_has "dry run changes nothing" "edited" "$(cat a.txt)"
"$CAIT" revert --id "$id" >/dev/null
expect_has "revert restores the file" "one" "$(cat a.txt)"
expect_lacks "revert removes the edit" "edited" "$(cat a.txt)"
expect_has "the undo is logged" "되돌림" "$("$CAIT" log --last 1)"
"$CAIT" report --agent rev --final "done" >/dev/null
fid=$("$CAIT" log --json --agent rev --last 1 | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
out=$("$CAIT" revert --id "$fid" 2>&1); expect_code "a final report cannot be undone" 1 $?
expect_has "and says why" "final report" "$out"

echo "final report and status"
hook idS1 Write "" "$TMP/repo/n.txt"
printf 'n\n' > "$TMP/repo/n.txt"
"$CAIT" report --agent zeta --file "$TMP/repo/n.txt" "wrote n" >/dev/null
hook idS1 Bash "$CAIT report --agent zeta --file /x \\\"s\\\"" >/dev/null
hook idS1 Edit "" "$TMP/repo/n.txt"
printf '{"agent_id":"idS1"}' | "$CAIT" check --done
st=$("$CAIT" status)
expect_has "status lists the agent" "zeta" "$st"
expect_has "an edit after the last report is counted" "unreported edits: 1" "$st"
expect_has "a missing final report is flagged" "NOT delivered" "$st"
hook idS1 Bash "$CAIT report --agent zeta --final \\\"all\\\"" >/dev/null
printf '{"agent_id":"idS1"}' | "$CAIT" check --done
st=$("$CAIT" status)
expect_lacks "nothing unreported after the final report" "unreported edits" "$(echo "$st" | grep -A4 '^zeta')"
expect_has "final report is delivered" "final report: delivered" "$st"
sj=$("$CAIT" status --json)
expect_has "status json has the flag" '"final_report":true' "$sj"
hook idRead Read "" "$TMP/repo/a.txt"
printf '{"agent_id":"idRead"}' | "$CAIT" check --done
expect_lacks "an agent that only read is not flagged" "idRead	done	reports 0	tool calls 1	idle 0s
  final report" "$("$CAIT" status)"

echo "ack and forget"
hook idK Write "" "$TMP/repo/k.txt"; printf '{"agent_id":"idK"}' | "$CAIT" check --done
expect_has "needs a look before ack" "NOT delivered" "$("$CAIT" status)"
"$CAIT" ack --agent idK >/dev/null
st=$("$CAIT" status)
expect_has "ack is shown" "looked at" "$st"
expect_lacks "ack clears the unreported edit" "unreported edits" "$(echo "$st" | grep -A3 '^idK')"
hook idK Write "" "$TMP/repo/k2.txt"
expect_lacks "doing more clears the ack" "looked at" "$("$CAIT" status | grep -A3 '^idK')"
"$CAIT" forget --done >/dev/null
expect_lacks "forget hides finished agents" "zeta" "$("$CAIT" status)"
expect_has "status --all brings them back" "zeta" "$("$CAIT" status --all)"
expect_has "the log keeps their reports" "zeta" "$("$CAIT" log)"

echo "guard"
hook idG Bash "ls"; expect_code "a harmless command passes" 0 $?
hook idG Bash "rm -rf /tmp/zzz"; code=$?
expect_code "a risky command with no answer is blocked" 2 "$code"
expect_has "and says to not retry" "Do not retry" "$(cat "$TMP/err")"
( hook idG Bash "git push origin main" ) & pid=$!
sleep 0.6
pid_req=$("$CAIT" perm | awk 'NR==1{print $1}')
expect_has "the request appears for the user" "git push" "$("$CAIT" perm)"
"$CAIT" perm allow "$pid_req" >/dev/null
wait $pid; expect_code "an allowed command goes through" 0 $?
( hook idG Bash "sudo ls" ) & pid=$!
sleep 0.6
"$CAIT" perm deny "$("$CAIT" perm | awk 'NR==1{print $1}')" "no" >/dev/null
wait $pid; expect_code "a denied command is blocked" 2 $?
"$CAIT" guard mode off >/dev/null
hook idG Bash "rm -rf /tmp/zzz"; expect_code "guard mode off asks nothing" 0 $?
"$CAIT" guard mode on >/dev/null
"$CAIT" guard add "frobnicate" >/dev/null
hook idG Bash "frobnicate now"; expect_code "a custom phrase is guarded" 2 $?

echo "subagents cannot answer for the user"
hook idX Bash "$CAIT perm allow 123"; expect_code "cait perm from a subagent is refused" 2 $?
hook idX Bash "\${CAIT:-cait} tell hello"; expect_code "cait tell via \$CAIT is refused" 2 $?
hook idX Bash "echo x >> $CAIT_DIR/inbox.tsv"; expect_code "writing the inbox is refused" 2 $?

echo "tell reaches the main agent"
"$CAIT" tell --agent alpha "please use a lock" >/dev/null
out=$(printf '{"tool_name":"Bash","tool_input":{"command":"ls"}}' | "$CAIT" check)
expect_has "the main agent gets the request as context" "please use a lock" "$out"
out=$(printf '{"tool_name":"Bash","tool_input":{"command":"ls"}}' | "$CAIT" check)
expect_lacks "it is delivered once" "please use a lock" "$out"

echo "brief"
printf '{"tool_name":"Agent","tool_input":{"description":"fix the login check","prompt":"Report as login-fix with cait."}}' | "$CAIT" check
hook idL Bash "$CAIT report --agent login-fix --file /x \\\"s\\\"" >/dev/null
expect_has "brief pairs the task with the agent" "fix the login check" "$("$CAIT" brief --agent login-fix)"

echo "large inputs"
pad=$(printf '%*s' 3000 '')
hook idPad Bash "echo ok;$pad rm -rf /tmp/zzz"; code=$?
expect_code "a risky phrase after 3KB of padding is still caught" 2 "$code"
big=$(head -c 200000 /dev/zero | tr '\0' 'x')
printf '{"tool_name":"Agent","tool_input":{"description":"big job","prompt":"%s. Report as bigjob"}}' "$big" | "$CAIT" check
hook idBig Bash "$CAIT report --agent bigjob --file /x \\\"s\\\"" >/dev/null
expect_has "a 200KB prompt still pairs with the agent that is named late in it" "big job" "$("$CAIT" brief --agent bigjob)"
huge=$(head -c 17000000 /dev/zero | tr '\0' 'y')
printf '{"tool_name":"Bash","agent_id":"idHuge","tool_input":{"command":"%s"}}' "$huge" 2>/dev/null | "$CAIT" check 2>"$TMP/err"; code=$?
expect_code "a subagent call over the input cap is refused, not half-checked" 2 "$code"
printf '{"tool_name":"Bash","tool_input":{"command":"%s"}}' "$huge" 2>/dev/null | "$CAIT" check 2>/dev/null
expect_code "the main agent is not blocked by the cap" 0 $?

echo "many agents"
for i in $(seq 1 300); do hook "scale$i" Read "" "$TMP/repo/a.txt" >/dev/null; done
n=$("$CAIT" status --json | grep -c '"agent"')
if [ "$n" -ge 300 ]; then ok "status lists every agent ($n), not just the first 64"; else bad "status lists every agent" "got $n, wanted at least 300"; fi
"$CAIT" forget --done >/dev/null
printf '{"agent_id":"scale7"}' | "$CAIT" check --done
expect_has "an agent far down the list is still reachable" "scale7" "$("$CAIT" status | grep -o '^scale7')"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
