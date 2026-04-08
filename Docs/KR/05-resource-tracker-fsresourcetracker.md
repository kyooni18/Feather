# 모듈: 리소스 추적 (`FSResourceTracker`)

## 목적

`FSResourceTracker`는 선택적 모듈로, Feather 내부 할당을 추적하여 활성 할당, 피크, 누적 할당/해제량, 누수 여부를 점검할 수 있게 합니다.

주요 파일:
- `System/FeatherRuntime/FSResourceTracker.hpp`
- `System/FeatherRuntime/FSResourceTracker.cpp`

이 모듈은 **링크 가능한 확장** 입니다 — 코어 `Feather` 라이브러리에 의존하는 별도 라이브러리(`FeatherResourceTracking`)로 빌드됩니다. 이 확장을 링크하지 않으면 기본 `Feather` 모델은 변경되지 않습니다.

## 핵심 타입

### `FSResourceTrackerRecord`
- 포인터
- 크기
- 할당 시각(ms)

### `FSResourceTrackerSnapshot`
- `current_bytes`
- `peak_bytes`
- `total_allocated_bytes`
- `total_freed_bytes`
- `active_allocations`

### `FSResourceTrackerConfig`
- `base_allocator`
- `now_fn`
- `now_context`

### `FSResourceTrackerSchedulerSnapshot`
메모리 추적과 스케줄러 작업 상태를 하나로 결합한 뷰:
```c
typedef struct FSResourceTrackerSchedulerSnapshot {
  FSResourceTrackerSnapshot memory;    /* 할당자 카운터 */
  FSSchedulerStateSnapshot  scheduler; /* 작업 큐/힙 카운트 */
} FSResourceTrackerSchedulerSnapshot;
```

## 초기화/수명주기

- `FSResourceTracker_init`
- `FSResourceTracker_init_with_config`
- `FSResourceTracker_deinit`

`FSResourceTracker_allocator`로 tracker 기반 allocator를 얻어 `FeatherConfig`에 주입할 수 있습니다.

## 추적 동작

allocate/reallocate/deallocate를 감싸며 레코드 배열과 카운터를 갱신합니다.

주요 특징:
- 레코드 배열 자동 확장
- realloc 시 증감(delta) 만큼 통계 보정
- `peak_bytes` 최대 동시 사용량 추적

## 조회 API

- `FSResourceTracker_snapshot`
- `FSResourceTracker_copy_active_records`
- `FSResourceTracker_has_leaks`

일반적으로 `Feather_deinit` 전후로 조회하여 누수 여부를 확인합니다.

## 스케줄러 통합 스냅샷

```c
FSResourceTrackerSchedulerSnapshot FSResourceTracker_scheduler_snapshot(
    const FSResourceTracker *tracking, const FSScheduler *scheduler);
```

현재 메모리 추적 스냅샷과 스케줄러의 작업 카운트 상태를 하나의 구조체로 반환합니다. 진단이나 테스트에서 메모리 사용량과 작업 수를 함께 확인할 때 유용합니다.

## 통합 순서

1. tracker 초기화
2. tracker allocator를 `FeatherConfig.allocator`에 설정
3. Feather 초기화
4. 워크로드 실행
5. snapshot/active records 조회
6. Feather 종료 후 tracker 종료
