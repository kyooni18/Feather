# 모듈: 할당자 추상화 (`FSAllocator`)

## 목적

`FSAllocator`는 런타임 내부 메모리 할당을 사용자 정의 가능하게 만드는 추상화 계층입니다.

주요 파일:
- `System/FeatherRuntime/FSAllocator.h`
- `System/FeatherRuntime/FSAllocator.cpp`

## 인터페이스

구성 필드:
- `context`
- `allocate`
- `reallocate`
- `deallocate`

## 기본 할당자

`FSAllocator_system`은 `malloc/realloc/free` 기반 구현입니다.

동작 규칙:
- size 0 allocate → `NULL`
- `reallocate(NULL, size)` → allocate 동작
- `reallocate(ptr, 0)` → free 후 `NULL`

## Resolver/Wrapper

### `FSAllocator_resolve`
입력이 `NULL`이거나 함수 포인터가 누락되면 자동으로 시스템 할당자를 반환합니다.

### 래퍼 API
- `FSAllocator_allocate`
- `FSAllocator_reallocate`
- `FSAllocator_deallocate`

모듈 전체에서 동일한 방어 규칙으로 할당 동작을 유지합니다.

## Feather 통합

`FeatherConfig.allocator`로 커스텀 할당자를 주입하면, 스케줄러 내부 큐/힙 메모리 관리가 해당 정책을 따릅니다.
