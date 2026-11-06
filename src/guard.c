/* guard: calls from subagents that must be approved on the cait screen first */
#include "internal.h"

/* ---- guard: calls from subagents that must be approved on the cait screen first ---- */

/* <dir>/guardmode: off (never ask), on (risky phrases, the default), all (every command and edit) */
const char *guard_mode(void)
{
	static char m[16];
	char path[4200];
	FILE *f;

	snprintf(m, sizeof(m), "on");
	path_in_dir(path, sizeof(path), "guardmode");
	f = fopen(path, "r");
	if (f) {
		char l[16] = "";

		if (fgets(l, sizeof(l), f)) {
			l[strcspn(l, "\n")] = '\0';
			if (strcmp(l, "off") == 0 || strcmp(l, "all") == 0)
				snprintf(m, sizeof(m), "%s", l);
		}
		fclose(f);
	}
	return m;
}

static const char *guard_default[] = {
	"rm -rf", "rm -r ", "sudo ", "git push", "git reset --hard", "curl ", "wget ", "npm publish",
	"chmod ", "mkfs", "dd if=", "DROP TABLE", ".env", ".ssh/", NULL
};

/* Why a phrase needs the user's OK, in plain words. */
const char *guard_reason(const char *pat)
{
	static const struct { const char *pat, *why; } r[] = {
		{ "rm -rf", "파일이나 폴더를 통째로 지우는 명령입니다" },
		{ "rm -r ", "폴더를 지우는 명령입니다" },
		{ "sudo ", "관리자 권한으로 실행합니다" },
		{ "git push", "변경을 원격 저장소에 올립니다" },
		{ "git reset --hard", "저장하지 않은 변경을 버립니다" },
		{ "curl ", "외부 인터넷에 접속합니다" },
		{ "wget ", "외부 인터넷에서 파일을 받습니다" },
		{ "npm publish", "패키지를 외부에 공개합니다" },
		{ "chmod ", "파일 권한을 바꿉니다" },
		{ "mkfs", "디스크를 지웁니다" },
		{ "dd if=", "디스크나 파일을 덮어쓸 수 있습니다" },
		{ "DROP TABLE", "데이터베이스 표를 지웁니다" },
		{ ".env", "비밀번호 등이 든 설정 파일입니다" },
		{ ".ssh/", "접속용 비밀 키가 든 폴더입니다" },
	};
	static char buf[600];

	for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		if (strcmp(pat, r[i].pat) == 0)
			return r[i].why;
	snprintf(buf, sizeof(buf), "승인 목록에 있는 문구(%s)가 들어 있습니다", pat);
	return buf;
}

/* <dir>/guard: one substring per line. Absent: the defaults. */
int guard_match(const char *detail, char *hit, size_t size)
{
	char path[4200], line[512];
	FILE *f;

	path_in_dir(path, sizeof(path), "guard");
	f = fopen(path, "r");
	if (!f) {
		for (int i = 0; guard_default[i]; i++)
			if (strstr(detail, guard_default[i])) {
				snprintf(hit, size, "%s", guard_default[i]);
				return 1;
			}
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (*line && line[0] != '#' && strstr(detail, line)) {
			snprintf(hit, size, "%s", line);
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

int cmd_guard(int argc, char **argv)
{
	char path[4200], line[512];
	FILE *f;

	if (argc >= 1 && strcmp(argv[0], "mode") == 0) {
		if (argc >= 2) {
			if (strcmp(argv[1], "off") != 0 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "all") != 0) {
				fprintf(stderr, "usage: cait guard mode off|on|all\n");
				return 1;
			}
			if (ensure_dir() < 0)
				return 1;
			path_in_dir(path, sizeof(path), "guardmode");
			f = fopen(path, "w");
			if (!f)
				return 1;
			fprintf(f, "%s\n", argv[1]);
			fclose(f);
		}
		printf("guard mode: %s\n", guard_mode());
		return 0;
	}
	path_in_dir(path, sizeof(path), "guard");
	if (argc >= 2 && strcmp(argv[0], "add") == 0) {
		if (ensure_dir() < 0)
			return 1;
		if (access(path, F_OK) != 0) {	/* start from the defaults, then add */
			f = fopen(path, "w");
			for (int i = 0; f && guard_default[i]; i++)
				fprintf(f, "%s\n", guard_default[i]);
			if (f)
				fclose(f);
		}
		f = fopen(path, "a");
		if (!f)
			return 1;
		fprintf(f, "%s\n", argv[1]);
		fclose(f);
		printf("added: %s\n", argv[1]);
		return 0;
	}
	printf("guard mode: %s (cait guard mode off|on|all)\n", guard_mode());
	printf("Subagent calls containing one of these need your OK on the cait screen (edit %s):\n", path);
	f = fopen(path, "r");
	if (!f) {
		for (int i = 0; guard_default[i]; i++)
			printf("  %s\n", guard_default[i]);
		return 0;
	}
	while (fgets(line, sizeof(line), f))
		if (line[0] != '#' && line[0] != '\n')
			printf("  %s", line);
	fclose(f);
	return 0;
}
