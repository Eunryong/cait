# cait

[![ci](https://github.com/Eunryong/cait/actions/workflows/ci.yml/badge.svg)](https://github.com/Eunryong/cait/actions/workflows/ci.yml)

서브에이전트가 한 일을 **검증**하도록 돕는 macOS용 도구입니다. (cat + AI)

![cait 창: 에이전트 목록과 보고, 아키텍처 도면 미리보기](docs/screenshot.png)

## 왜 만들었나

서브에이전트에게 일을 나눠 맡기면 빨라지지만, 사용자는 "무엇을 바꿨는지, 시킨 대로 했는지, 위험한 일은 하지 않았는지"를 확인하기 어렵습니다. 결과만 믿고 넘기거나, 로그를 일일이 열어 봐야 합니다.

cait는 그 검증을 쉽게 만듭니다.

- **무엇을 바꿨는지 바로 본다.** 서브에이전트가 파일을 고칠 때마다 쉬운 말로 된 요약이 화면에 올라옵니다. 변경량(+N −M)은 에이전트의 말이 아니라 cait가 git으로 직접 잽니다.
- **빠진 것을 잡는다.** 끝났는데 완료 보고가 없거나, 마지막 보고 뒤에 고친 파일이 있으면 "확인 필요"로 표시합니다.
- **문서와 도면을 읽는다.** 에이전트가 쓴 마크다운(바뀐 줄은 색으로 표시)과 Mermaid 아키텍처 도면을 그 자리에서 봅니다.
- **그 자리에서 조치한다.** 이상하면 중지하고, 한 건씩 되돌리고, 메인 에이전트에게 수정이나 보고를 요청합니다. 위험한 명령(`rm -rf`, `git push` 등)은 사용자가 화면에서 허용해야 실행됩니다.

파일을 많이 바꾸는 작업은 알림이 넘칠 수 있어서 **메인 에이전트가 보고 단위를 조절**합니다.

## 어떻게 쓰나

1. 메인 에이전트가 서브에이전트를 띄우면서 `cait-report` 스킬을 쓰라고 지시합니다.
2. 서브에이전트는 파일을 고칠 때마다 `cait report`로 요약을 보내고, 끝나면 `--final`로 마무리합니다.
3. 사용자는 `cait gui` 창(또는 터미널의 `cait panel`)에서 에이전트별 상태와 보고를 보고, 필요하면 바로 중지하거나 되돌리거나 수정을 요청합니다.

```
19:04:09  booking-fix  업데이트됨 src/booking/create.c  +3 −1
            예약을 만들 때 중복 요청을 확인하는 기준을 예약 상태가 아니라 요청 키로 바꿨습니다
19:04:12  login        3개 파일  +86 −12
            로그인 검사가 만료된 토큰도 거부하고 세션을 새로 만들도록 바꿨습니다
```

화면 읽는 법과 버튼은 [docs/usage.md](docs/usage.md)에 있습니다.

## 지원 환경

- **macOS 전용**입니다. 창(`cait gui`)이 Cocoa로 되어 있고, 명령과 터미널 화면도 macOS에서 만들고 시험했습니다.
- 서브에이전트 연동은 **Claude Code**의 스킬과 훅을 씁니다. 훅이 없어도 보고를 받고 보는 것은 됩니다.
- 다이어그램을 그릴 때만 인터넷이 필요합니다.

## 설치

```bash
git clone https://github.com/Eunryong/cait.git && cd cait
make                 # 빌드
make test            # 테스트 (선택)
export CAIT="$PWD/cait"
./cait gui           # 창 띄우기
```

Claude Code에서 쓰려면 아래 셋을 연결합니다. 스킬과 훅은 모두 `${CAIT:-cait}`로 cait를 부르므로, **환경변수 `CAIT`에 실행 파일 경로를 주입**하면 어느 위치에 설치해도 됩니다. `cait`를 `PATH`에 두면 `CAIT` 없이도 됩니다.

1. **스킬:** [cait-report](.claude/skills/cait-report/SKILL.md)(서브에이전트용), [cait-level](.claude/skills/cait-level/SKILL.md)(메인 에이전트용)을 프로젝트의 `.claude/skills/`나 `~/.claude/skills/`에 둡니다.
2. **훅:** [.claude/settings.json](.claude/settings.json)의 `hooks`를 설정에 합칩니다. 진행 상황, 되돌리기, 승인, 강제 중단은 이 훅이 있어야 동작합니다.
3. **경로:** Claude Code 설정(`.claude/settings.local.json` 또는 `~/.claude/settings.json`)의 `env`에 넣거나 셸 프로필에서 내보냅니다.

```json
{ "env": { "CAIT": "/절대/경로/cait" } }
```

## 에이전트에게 맡기기

설치나 기능 추가를 Claude Code 같은 에이전트에게 시킬 수 있습니다. 에이전트가 따를 절차는 [AGENTS.md](AGENTS.md)에 정리되어 있습니다. 저장소 링크와 함께 이렇게 요청하세요.

```text
https://github.com/Eunryong/cait 를 읽고 AGENTS.md의 "설치" 절차대로 이 프로젝트에 cait 스킬과 훅을 적용해 줘.
```

```text
https://github.com/Eunryong/cait 의 AGENTS.md "기능 추가" 지침을 따라서, ○○ 기능을 추가해 줘.
```

## 문서

| 문서 | 내용 |
| --- | --- |
| [docs/usage.md](docs/usage.md) | 화면 사용법: 에이전트 상태, 버튼, 문서·도면 미리보기, 단축키, 터미널 패널 |
| [docs/commands.md](docs/commands.md) | 모든 명령과 보고 단위 |
| [docs/internals.md](docs/internals.md) | 동작 방식, 저장 위치, 소스 구성 |
| [AGENTS.md](AGENTS.md) | 에이전트가 설치하거나 기능을 추가할 때 따를 지침 |

## 로드맵

- 같은 파일을 여러 에이전트가 고친 경우의 표시와, 그 에이전트의 변경만 되돌리기
- 되돌림을 메인 에이전트에게 요약해서 알리기
- 동시 쓰기에 대한 파일 락 정리
