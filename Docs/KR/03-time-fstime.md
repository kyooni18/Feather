# 모듈: 시간 계층 (`FSTime`)

## 목적

`FSTime`은 플랫폼별 시간 조회/슬립 구현을 공통 인터페이스로 추상화합니다. 스케줄러는 이 계층 덕분에 플랫폼 독립적으로 동작합니다.

주요 파일:
- `System/FeatherRuntime/FSTime.h`
- `System/FeatherRuntime/FSTime.c`
- `System/FeatherRuntime/FSTime_posix.c`
- `System/FeatherRuntime/FSTime_windows.c`
- `System/FeatherRuntime/FSTime_arduino.c`

## 인터페이스

`FSTime` 구조체 함수 포인터:
- `now_monotonic_ms`
- `now_unix_ms`
- `sleep_ms`

`FSTime_init`은 실제 백엔드 함수와 연결된 싱글턴입니다.

공개 래퍼 함수:
- `FSTime_now_monotonic`
- `FSTime_now_unix`
- `FSTime_sleep_ms`

## 플랫폼 백엔드

### POSIX
- monotonic: `clock_gettime(CLOCK_MONOTONIC)`
- unix time: `clock_gettime(CLOCK_REALTIME)`
- sleep: `nanosleep` (EINTR 재시도)

### Windows
- monotonic: `QueryPerformanceCounter`
- unix time: `GetSystemTimeAsFileTime` 변환
- sleep: `Sleep`

### Arduino
- monotonic/unix 유사 시간: `millis()`
- sleep: `delay()`

## 스케줄러 연계

기본적으로 스케줄러는 `FSTime_init`을 사용합니다. 필요하면 커스텀 provider 또는 now 콜백으로 대체할 수 있습니다.
