# 모듈: 스케줄러 (`FSScheduler`)

## 목적

`FSScheduler`는 Feather의 핵심 협력형(cooperative) 스케줄러입니다. 우선순위 큐, 지연 실행, 반복 실행, 공정성 예산, sleep 힌트를 관리합니다.

주요 파일:
- `System/FeatherRuntime/FSScheduler.hpp`
- `System/FeatherRuntime/FSScheduler.cpp`

## 확장 모델

`FSScheduler`는 풍부한 런타임 API입니다. `Feather` 파사드는 기본 모델을 단순하게 유지하기 위해 핵심 동작(추가, step, sleep-hint, 메모리 스냅샷)만 노출합니다. 작업 취소, 상태 조회, 스케줄러 상태 스냅샷 같은 고급 기능은 `feather.scheduler`를 통해 `FSScheduler` API에 직접 접근하여 사용합니다.

## 작업 모델

### 작업 종류
- `FSSchedulerInstantTask`: 즉시 실행
- `FSSchedulerDeferredTask` (API 명칭이 기존 표기(Deferred)를 사용): `start_time` 이후 1회 실행
- `FSSchedulerRepeatingTask`: `execute_cycle` 주기로 반복 실행
- 각 작업은 선택적으로 `deadline`, `timeout`(절대 단조 시각, `0`=미사용)을 가질 수 있습니다.
- 핸들 기반 제어를 위해 `FSSchedulerTaskHandler`이 제공됩니다.

### 반복 모드
- `FSSchedulerTaskRepeat_FIXEDDELAY`: 콜백 완료 시점 기준 다음 실행
- `FSSchedulerTaskRepeat_FIXEDRATE`: 원래 cadence를 유지하며 필요 시 산술적으로 catch-up

### 우선순위
- `BACKGROUND`(0)
- `UI`(1)
- `INTERACTIVE`(2)

실행 선택은 높은 우선순위를 먼저 고려합니다.

## 용량과 ID

- 전체 최대 작업 수: `FSScheduler_TASK_CAPACITY` (1024)
- 초기 버퍼 크기: `FSScheduler_TASK_INITIAL_CAPACITY` (16)
- 큐/힙 버퍼는 필요 시 증가하고 작업 소진 후 적응적으로 축소되며, 최소 크기는 `FSScheduler_TASK_INITIAL_CAPACITY`를 유지합니다.
- 작업 ID는 slot+generation 방식으로 enqueue 시점에 부여됩니다.

## 내부 자료구조

### Ready 큐 (원형 버퍼)
- `bgReadyQueue`
- `uiReadyQueue`
- `interactiveReadyQueue`

### Waiting 큐 (최소 힙)
- `waitingTasks`
- `start_time` 오름차순
- 동일 start_time이면 더 높은 priority 우선
- 동일 priority에서는 `deadline`이 더 빠른 작업 우선 (`deadline=0`은 우선순위 없음)

다음 wake 시각 계산을 위해 `earliest_wake_time` 캐시를 유지합니다.

## 공정성 예산

Ready 사이클 예산:
- BG: 1
- UI: 2
- INTERACTIVE: 4

예산은 dispatch 시 감소하고, 모든 ready 큐가 예산 소진으로 막히면 예산을 재설정합니다.

## 시간 모델

스케줄러는 `now_fn(context)`를 기준 시간으로 사용합니다.
`set_time_provider`는 provider 기반 기본 시간 모드로 되돌리고,
`set_time_source`는 사용자 콜백으로 직접 덮어씁니다.

## 처리 흐름

### `FSScheduler_step`
1. 현재 시각 1회 조회
2. waiting에서 실행 가능 작업을 ready로 승격
3. ready가 없으면 `false`
4. 필요 시 예산 리셋
5. 우선순위 순서대로 1개 작업 실행

### 반복 작업 재등록
- non-repeating: 카운트 감소 후 종료
- repeating: 모드에 따라 다음 start_time 계산 후 재등록

### `FSScheduler_process_for_ms`
지정 시간 창 동안 step/sleep 루프를 수행합니다.

## 작업 취소

```c
bool FSScheduler_cancel_task(FSScheduler *scheduler, uint64_t task_id);
```

지정된 ID의 작업을 ready 큐 또는 waiting 힙에서 즉시 제거합니다. 우선순위 카운트를 감소시켜 `has_pending_tasks`가 정확하게 유지됩니다. 작업을 찾아 제거했으면 `true`, 없거나 ID가 0이면 `false`를 반환합니다. 취소된 작업의 콜백은 절대 호출되지 않습니다.

## pause/resume/reschedule

```c
bool FSScheduler_pause_task(FSScheduler *scheduler, uint64_t task_id);
bool FSScheduler_resume_task(FSScheduler *scheduler, uint64_t task_id);
bool FSScheduler_reschedule_task(FSScheduler *scheduler, uint64_t task_id,
                                 uint64_t start_time_ms);
```

- pause: READY/WAITING에서 분리해 `PAUSED` 상태로 이동
- resume: 원래 스케줄 정책으로 READY/WAITING에 복귀
- reschedule: 다음 실행 시각(`start_time`)을 갱신 (반복 작업 포함)

## 작업 상태 조회

```c
FSSchedulerTaskStatus FSScheduler_task_status(const FSScheduler *scheduler,
                                              uint64_t task_id);
```

반환값:
- `FSSchedulerTaskStatus_NOT_FOUND` — 스케줄러에 없음 (실행 완료, 취소됨, 또는 알 수 없는 ID)
- `FSSchedulerTaskStatus_PENDING_READY` — ready 큐에 있으며 다음 step에서 실행될 예정
- `FSSchedulerTaskStatus_PENDING_WAITING` — waiting 힙에 있으며 아직 실행 시각이 되지 않음
- `FSSchedulerTaskStatus_PENDING_PAUSED` — pause되어 보관 중
- `FSSchedulerTaskStatus_TIMED_OUT` — timeout 정책에 의해 드롭됨

## deadline/timeout 제어

```c
bool FSScheduler_set_task_deadline(FSScheduler *scheduler, uint64_t task_id,
                                   uint64_t deadline_ms);
bool FSScheduler_set_task_timeout(FSScheduler *scheduler, uint64_t task_id,
                                  uint64_t timeout_ms);
```

- 같은 priority 내에서는 deadline이 빠른 작업을 먼저 실행
- `timeout`은 “이 시각 이후 실행 의미 없음” 정책이며, `now > timeout` 시 실행 없이 제거되고 `TIMED_OUT` 상태를 반환

## task handle API

```c
FSSchedulerTaskHandler FSScheduler_add_*_task_handle(...);
bool FSScheduler_task_handle_is_valid(const FSSchedulerTaskHandler *handle);
bool FSScheduler_task_handle_cancel(FSScheduler *scheduler, FSSchedulerTaskHandler *handle);
bool FSScheduler_task_handle_pause(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle);
bool FSScheduler_task_handle_resume(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle);
bool FSScheduler_task_handle_reschedule(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t start_time_ms);
bool FSScheduler_task_handle_set_deadline(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t deadline_ms);
bool FSScheduler_task_handle_set_timeout(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t timeout_ms);
FSSchedulerTaskStatus FSScheduler_task_handle_status(const FSScheduler *scheduler, const FSSchedulerTaskHandler *handle);
uint64_t FSScheduler_task_handle_user_tag(const FSSchedulerTaskHandler *handle);
bool FSScheduler_task_handle_set_user_tag(FSSchedulerTaskHandler *handle, uint64_t user_tag);
```

핸들은 `task_id`를 감싸는 경량 타입으로, `user_tag`를 붙여 상위 계층 메타데이터를 함께 관리할 수 있습니다.

## 스케줄러 상태 스냅샷

```c
FSSchedulerStateSnapshot FSScheduler_state_snapshot(const FSScheduler *scheduler);
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

## 메모리 스냅샷

`FSScheduler_component_memory_snapshot`는 BG/UI/INTERACTIVE ready 큐 + waiting 힙 바이트 및 총합을 반환합니다.

## 실패 조건

잘못된 입력, 잘못된 priority/repeat_mode, 0 주기, 메모리 할당 실패, 용량 초과 시 `false`를 반환합니다.
