# 팀 컨벤션

## 브랜치
- master: PR 로만 병합. 직접 push·force push 금지
- 개인 작업: 본인 이름 브랜치 (예: Jun)
- 작업 시작 전 최신 master 반영: `git pull origin master`

## 커밋 메시지
- 형식: `[Type] 내용` (콜론 없이)
- Type: Feat / Fix / Refactor / Test / Docs / Chore
- 예: `[Feat] exec 시스템 콜 구현`

## PR
- 제목: 커밋 메시지와 같은 형식
- 매일 코어타임에 같이 리뷰 후 병합 (merge commit)
- 템플릿의 테스트 결과 칸 필수

## 코드 스타일
- 원본 Pintos 스타일을 따른다: 탭 들여쓰기, 함수명·괄호 사이 공백(`func (arg)`),
  80자 이하, snake_case + 모듈 접두사(`thread_`, `sema_`), 매크로는 대문자
- 새 함수는 같은 파일의 기존 함수 모양을 그대로 따라 쓴다
- 파일 전체 자동 정렬(Format Document / Format on Save) 금지 — 고친 줄만 바뀌게
- 디버깅용 printf 는 PR 전에 지운다

## 기타
- 빌드 산출물(build/) 커밋 금지
- Windows: 줄바꿈이 CRLF 로 바뀌면 컨테이너에서 스크립트 오류
  → 클론할 때 `git -c core.autocrlf=false clone ...`
