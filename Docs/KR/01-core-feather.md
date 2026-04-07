# 모듈: 코어 API (`Feather`)

## 목적

`Feather` 모듈은 라이브러리 사용자를 위한 공개 진입점입니다. 내부의 `FSScheduler`를 감싸서 초기화, 작업 등록, step 실행, 시간 창 기반 처리, 작업 취소, 상태 조회, 스케줄러 상태 스냅샷 API를 단일 인터페이스로 제공합니다.

주요 파일:
- `System/Feather.h`
- `System/Feather.c`

## 핵심 타입

### `FeatherConfig`
초기화 설정 구조체:
- `allocator`: 사용자 할당자(`FSAllocator`) 선택 주입
- `now_fn`: 단조 시간 콜백
- `now_context`: 콜백 컨텍스트
- `time_provider`: `FSTime` 제공자 (`NULL`이면 기본 플랫폼 시간 사용)

기본값은 `FeatherConfig_init`로 제공됩니다.

### `Feather`
실제 스케줄러 하나를 담는 래퍼:
- `FSScheduler scheduler`

### `FeatherComponentMemorySnapshot`
스케줄러 구성요소 메모리 스냅샷 타입 별칭입니다.

## 공개 API 동작

### 초기화
- `Feather_init(feather)`
- `Feather_init_with_config(feather, config)`
- `Feather_deinit(feather)`

시간 제공자/시간 소스 설정이 실패하면 초기화는 실패를 반환하고 내부 상태를 정리합니다.

### 작업 등록
- `Feather_add_instant_task`
- `Feather_add_deferred_task`
- `Feather_add_repeating_task`

입력 오류, 용량 초과, 내부 enqueue 실패 시 `false`를 반환합니다.

### 시간 설정
- `Feather_set_time_source`
- `Feather_set_time_provider`

### 실행
- `Feather_step`
- `Feather_process_for_ms`
- `Feather_has_pending_tasks`
- `Feather_next_sleep_ms`

### 작업 취소

```c
bool Feather_cancel_task(Feather *feather, uint64_t task_id);
```

지정된 ID의 작업을 ready 큐 또는 waiting 힙에서 즉시 제거합니다. 작업을 찾아 제거했으면 `true`, 없거나 ID가 0이면 `false`를 반환합니다. 취소된 작업의 콜백은 절대 호출되지 않습니다.

### 작업 상태 조회

```c
FSSchedulerTaskStatus Feather_task_status(const Feather *feather, uint64_t task_id);
```

반환값:
- `FSSchedulerTaskStatus_NOT_FOUND` — 스케줄러에 없음 (실행 완료, 취소됨, 또는 알 수 없는 ID)
- `FSSchedulerTaskStatus_PENDING_READY` — ready 큐에 있으며 다음 step에서 실행될 예정
- `FSSchedulerTaskStatus_PENDING_WAITING` — waiting 힙에 있으며 아직 실행 시각이 되지 않음
- `FSSchedulerTaskStatus_PENDING_PAUSED` — pause되어 보관 중
- `FSSchedulerTaskStatus_TIMED_OUT` — timeout 정책으로 드롭됨

### 스케줄러 상태 스냅샷

```c
FSSchedulerStateSnapshot Feather_state_snapshot(const Feather *feather);
```

특정 시점의 전체 큐 카운트 현황을 반환합니다:

| 필드 | 설명 |
|---|---|
| `background_ready_count` | BG ready 큐의 작업 수 |
| `ui_ready_count` | UI ready 큐의 작업 수 |
| `interactive_ready_count` | interactive ready 큐의 작업 수 |
| `waiting_count` | waiting 힙의 작업 수 |
| `paused_count` | paused 보관소의 작업 수 |
| `total_pending` | 전체 추적 중인 작업 수 |
| `has_earliest_wake_time` | 예약된 future wakeup 존재 여부 |
| `earliest_wake_time_ms` | 다음 scheduled wakeup의 단조 시간(ms) |

### 메모리 스냅샷
- `Feather_component_memory_snapshot` — 큐/힙 버퍼 바이트 사용량 반환

## 확장 모듈

`Feather` 구조체와 API는 의도적으로 간결하게 유지됩니다. 선택적 확장 라이브러리가 이 위에 추가됩니다:

- **`FeatherResourceTracking`** (`FSResourceTracker.h`) — 할당자 레벨 바이트 추적, 누수 감지, 스케줄러+메모리 통합 스냅샷 (`FSResourceTracker_scheduler_snapshot`)

확장 라이브러리는 `Feather::Feather`를 링크하며 `FeatherConfig.allocator`를 통해 기능을 주입합니다.

핸들/`pause`/`resume`/`reschedule`/`deadline`/`timeout` 같은 고급 제어는 `Feather` 파사드가 아닌 `FSScheduler` 확장 API(`feather.scheduler`)에 배치됩니다.

## 방어 로직

코어 래퍼는 `NULL` 및 비정상 상태를 검사하여 안정적으로 `false`를 반환합니다. deinit 이후에는 `now_fn == NULL` 상태를 통해 사용 불가 상태를 감지합니다.

## 일반 사용 순서

1. `Feather` 인스턴스 생성
2. 기본 또는 사용자 설정으로 초기화
3. 작업 등록
4. `step` 반복 또는 `process_for_ms` 실행
5. 필요 시 작업 취소 또는 상태 조회
6. 사용 종료 시 `deinit`
