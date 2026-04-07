# 모듈: 빌드 / 테스트 / 패키징

## 목적

이 문서는 Feather의 빌드 요구 사항, 전체 Make 타깃, 설치 방법, 외부 프로젝트에서의 CMake/pkg-config 연동, Arduino 내보내기를 설명합니다.

주요 파일:
- `Makefile`
- `CMakeLists.txt`
- `cmake/FeatherConfig.cmake.in`
- `cmake/Feather.pc.in`

## 요구 사항

- CMake ≥ 3.20
- C99 컴파일러 (GCC, Clang, MSVC)
- GNU Make (편의 타깃 사용 시)

## 빌드 구조

- 실제 빌드는 CMake 기반입니다.
- Makefile은 자주 쓰는 명령을 래핑합니다.

출력 경로:

| 경로 | 내용 |
|---|---|
| `build/static/` | 정적 라이브러리 빌드 산출물 |
| `build/shared/` | 공유 라이브러리 빌드 산출물 |
| `build/arduino/Feather/` | Arduino 호환 라이브러리 내보내기 |

## Make 타깃

### 빌드

| 타깃 | 설명 |
|---|---|
| `make` / `make build` | 정적 라이브러리 및 모든 도구 빌드 |
| `make shared` | 공유 라이브러리 및 모든 도구 빌드 |

### 테스트

| 타깃 | 설명 |
|---|---|
| `make check` | 정적 빌드 후 `Feather`, `FeatherTestCLI all`, `FeatherResourceTrackingTest` 실행 |
| `make check-shared` | 공유 라이브러리 빌드로 동일한 테스트 실행 |
| `make check-memory` | `FeatherMemoryTest` 빌드 및 실행 |
| `make run-system-score-test` | `FeatherSystemScoreTest` 빌드 및 실행 |

### 개별 빌드 타깃

| 타깃 | 생성 실행 파일 |
|---|---|
| `make demo` | `FeatherDemo` |
| `make fast-demo` | `FeatherFastDemo` |
| `make test-cli` | `FeatherTestCLI` |
| `make memory-test` | `FeatherMemoryTest` |
| `make system-score-test` | `FeatherSystemScoreTest` |
| `make tracking-test` | `FeatherResourceTrackingTest` |

### 개별 실행 타깃

| 타깃 | 설명 |
|---|---|
| `make run` | `Feather` 스케줄러 통합 테스트 실행 |
| `make run-demo` | `FeatherDemo` 실행 |
| `make run-fast-demo` | `FeatherFastDemo` 실행 |
| `make run-test-cli` | `FeatherTestCLI all` 실행 |
| `make run-memory-test` | `FeatherMemoryTest` 실행 |
| `make run-tracking-test` | `FeatherResourceTrackingTest` 실행 |
| `make run-full-system-benchmark` | 벤치마크 빌드 및 실행 |

### 설치

```sh
# 기본 prefix(/usr/local)에 정적 라이브러리 설치
make install

# 공유 라이브러리 설치
make install-shared

# 사용자 지정 prefix에 설치
make install prefix=/설치/경로
make install-shared prefix=/설치/경로
```

내부적으로 `cmake --install <build_dir> --prefix <prefix>` 명령을 실행합니다.

### 정리

| 타깃 | 설명 |
|---|---|
| `make clean` | 모든 빌드 디렉터리 제거 |
| `make distclean` | 빌드 디렉터리 및 로그 제거 |

### Arduino

```sh
make arduino-export
```

`build/arduino/Feather/`에 다음을 생성합니다:
- `System/` 소스를 `src/` 아래에 복사
- `library.properties` 메타데이터

## CMake 옵션

모든 옵션은 별도 명시가 없으면 기본값 `ON`입니다:

| 옵션 | 설명 |
|---|---|
| `FEATHER_BUILD_SHARED` | 공유 라이브러리로 빌드 (기본: `OFF`) |
| `FEATHER_BUILD_EXAMPLE` | `Feather` 예제 실행 파일 빌드 |
| `FEATHER_BUILD_DEMO` | `FeatherDemo` 빌드 |
| `FEATHER_BUILD_FAST_DEMO` | `FeatherFastDemo` 빌드 |
| `FEATHER_BUILD_TEST_CLI` | `FeatherTestCLI` 빌드 |
| `FEATHER_BUILD_MEMORY_TEST` | `FeatherMemoryTest` 빌드 (`FEATHER_BUILD_RESOURCE_TRACKING` 필요) |
| `FEATHER_BUILD_SYSTEM_SCORE_TEST` | `FeatherSystemScoreTest` 빌드 |
| `FEATHER_BUILD_RESOURCE_TRACKING` | `libFeatherResourceTracking` 확장 라이브러리 빌드 |
| `FEATHER_BUILD_RESOURCE_TRACKING_TEST` | `FeatherResourceTrackingTest` 빌드 |
| `FEATHER_INSTALL_PKGCONFIG` | pkg-config용 `Feather.pc` 설치 |

예시 — 정적 라이브러리만 빌드:

```sh
cmake -S . -B build/minimal \
  -DFEATHER_BUILD_EXAMPLE=OFF \
  -DFEATHER_BUILD_DEMO=OFF \
  -DFEATHER_BUILD_FAST_DEMO=OFF \
  -DFEATHER_BUILD_TEST_CLI=OFF \
  -DFEATHER_BUILD_MEMORY_TEST=OFF \
  -DFEATHER_BUILD_SYSTEM_SCORE_TEST=OFF \
  -DFEATHER_BUILD_RESOURCE_TRACKING_TEST=OFF
cmake --build build/minimal
```

## 설치 결과물

| 산출물 | 설치 경로 |
|---|---|
| `libFeather.a` / `.so` / `.dylib` | `<prefix>/lib/` |
| `libFeatherResourceTracking.*` (선택) | `<prefix>/lib/` |
| `Feather.h`, `FeatherRuntime/*.h`, `FeatherExport.h` | `<prefix>/include/` |
| `FeatherConfig.cmake`, `FeatherConfigVersion.cmake`, `FeatherTargets.cmake` | `<prefix>/lib/cmake/Feather/` |
| `Feather.pc` (선택) | `<prefix>/lib/pkgconfig/` |

## CMake 소비자 연동

설치 후 외부 CMake 프로젝트에서 Feather를 사용하는 방법:

```cmake
find_package(Feather REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE Feather::Feather)
```

리소스 트래킹 확장을 함께 사용할 경우:

```cmake
target_link_libraries(my_app PRIVATE Feather::FeatherResourceTracking)
```

비표준 prefix에 설치했다면 `CMAKE_PREFIX_PATH`를 지정하세요:

```sh
cmake -DCMAKE_PREFIX_PATH=/설치/경로 -S . -B build
```

## pkg-config 연동

```sh
# 컴파일 및 링크 플래그 출력
pkg-config --cflags --libs Feather

# Makefile에서 사용
CFLAGS  += $(shell pkg-config --cflags Feather)
LDFLAGS += $(shell pkg-config --libs   Feather)
```

비표준 prefix에 설치했다면 `PKG_CONFIG_PATH`를 설정하세요:

```sh
export PKG_CONFIG_PATH=/설치/경로/lib/pkgconfig:$PKG_CONFIG_PATH
```
