#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "FSScheduler.hpp"

class FSEvent {
private:
    static constexpr size_t InlineStorageSize = 64;

    struct Ops {
        void (*destroy_storage)(void*) = nullptr;
        void (*move_construct_storage)(void* dst, void* src) = nullptr;
        bool (*condition)(void*, uint64_t) = nullptr;
        void (*dispatch_task)(void*, FSScheduler&, uint64_t) = nullptr;
    };

    alignas(std::max_align_t) unsigned char inline_storage_[InlineStorageSize];
    void* storage_ = nullptr;
    const Ops* ops_ = nullptr;
    bool enabled_ = true;
    bool uses_heap_storage_ = false;

    void* inline_storage_ptr() {
        return static_cast<void*>(inline_storage_);
    }

    const void* inline_storage_ptr() const {
        return static_cast<const void*>(inline_storage_);
    }

    void reset() {
        if (ops_ != nullptr && ops_->destroy_storage != nullptr && storage_ != nullptr) {
            ops_->destroy_storage(storage_);
            if (uses_heap_storage_) {
                ::operator delete(storage_);
            }
        }
        storage_ = nullptr;
        ops_ = nullptr;
        enabled_ = true;
        uses_heap_storage_ = false;
    }

    template<typename Storage>
    static constexpr bool fits_inline_storage() {
        return sizeof(Storage) <= InlineStorageSize &&
               alignof(Storage) <= alignof(std::max_align_t) &&
               std::is_nothrow_move_constructible_v<Storage>;
    }

public:
    FSEvent() = default;

    template<typename Condition, typename Task>
    static FSEvent make(
        Condition&& condition,
        Task&&      task,
        uint8_t     priority,
        bool        enabled = true
    ) {
        using StoredCondition = std::decay_t<Condition>;
        using StoredTask = std::decay_t<Task>;

        static_assert(
            std::is_invocable_r_v<bool, StoredCondition&, uint64_t>,
            "Condition must be callable as bool(uint64_t)"
        );
        static_assert(
            std::is_invocable_r_v<void, StoredTask&> ||
                std::is_invocable_r_v<void, StoredTask&, uint64_t>,
            "Task must be callable as void() or void(uint64_t)"
        );
        static_assert(
            std::is_copy_constructible_v<StoredTask>,
            "Task must be copy-constructible so it can be queued when the event fires"
        );

        struct Storage {
            StoredCondition condition;
            StoredTask task;
            uint8_t priority;
        };

        static const Ops ops{
            [](void* ptr) {
                static_cast<Storage*>(ptr)->~Storage();
            },
            [](void* dst, void* src) {
                new (dst) Storage(std::move(*static_cast<Storage*>(src)));
            },
            [](void* ptr, uint64_t now_ms) -> bool {
                auto* typed = static_cast<Storage*>(ptr);
                return typed->condition(now_ms);
            },
            [](void* ptr, FSScheduler& scheduler, uint64_t now_ms) {
                auto* typed = static_cast<Storage*>(ptr);
                if constexpr (std::is_invocable_r_v<void, StoredTask&, uint64_t>) {
                    scheduler.enqueue_ready_task(
                        [task = typed->task, now_ms]() mutable {
                            task(now_ms);
                        },
                        typed->priority
                    );
                } else {
                    scheduler.enqueue_ready_task(typed->task, typed->priority);
                }
            }
        };

        FSEvent event;
        if constexpr (fits_inline_storage<Storage>()) {
            event.storage_ = event.inline_storage_ptr();
            new (event.storage_) Storage{
                std::forward<Condition>(condition),
                std::forward<Task>(task),
                static_cast<uint8_t>(priority & 0x0F)
            };
            event.uses_heap_storage_ = false;
        } else {
            event.storage_ = ::operator new(sizeof(Storage));
            new (event.storage_) Storage{
                std::forward<Condition>(condition),
                std::forward<Task>(task),
                static_cast<uint8_t>(priority & 0x0F)
            };
            event.uses_heap_storage_ = true;
        }

        event.ops_ = &ops;
        event.enabled_ = enabled;
        return event;
    }

    template<typename Condition, typename Task>
    FSEvent(
        Condition&& condition,
        Task&&      task,
        uint8_t     priority,
        bool        enabled = true
    )
        : FSEvent(
              make(
                  std::forward<Condition>(condition),
                  std::forward<Task>(task),
                  priority,
                  enabled
              )
          ) {}

    ~FSEvent() {
        reset();
    }

    FSEvent(FSEvent&& other) noexcept
        : storage_(nullptr)
        , ops_(other.ops_)
        , enabled_(other.enabled_)
        , uses_heap_storage_(other.uses_heap_storage_) {
        if (other.storage_ != nullptr) {
            if (other.uses_heap_storage_) {
                storage_ = other.storage_;
            } else {
                storage_ = inline_storage_ptr();
                ops_->move_construct_storage(storage_, other.storage_);
                ops_->destroy_storage(other.storage_);
            }
        }

        other.storage_ = nullptr;
        other.ops_ = nullptr;
        other.enabled_ = true;
        other.uses_heap_storage_ = false;
    }

    FSEvent& operator=(FSEvent&& other) noexcept {
        if (this != &other) {
            reset();
            ops_ = other.ops_;
            enabled_ = other.enabled_;
            uses_heap_storage_ = other.uses_heap_storage_;

            if (other.storage_ != nullptr) {
                if (other.uses_heap_storage_) {
                    storage_ = other.storage_;
                } else {
                    storage_ = inline_storage_ptr();
                    ops_->move_construct_storage(storage_, other.storage_);
                    ops_->destroy_storage(other.storage_);
                }
            }

            other.storage_ = nullptr;
            other.ops_ = nullptr;
            other.enabled_ = true;
            other.uses_heap_storage_ = false;
        }
        return *this;
    }

    FSEvent(const FSEvent&) = delete;
    FSEvent& operator=(const FSEvent&) = delete;

    bool is_enabled() const {
        return enabled_;
    }

    void set_enabled(bool enabled) {
        enabled_ = enabled;
    }

    bool poll(FSScheduler& scheduler, uint64_t now_ms) {
        if (!enabled_ || storage_ == nullptr || ops_ == nullptr ||
            ops_->condition == nullptr || ops_->dispatch_task == nullptr) {
            return false;
        }

        if (!ops_->condition(storage_, now_ms)) {
            return false;
        }

        ops_->dispatch_task(storage_, scheduler, now_ms);
        return true;
    }
};

struct FSEventHandle {
    size_t index = std::numeric_limits<size_t>::max();
    uint32_t generation = 0;

    static constexpr FSEventHandle invalid() {
        return FSEventHandle{};
    }

    bool is_valid() const {
        return index != std::numeric_limits<size_t>::max();
    }
};

class FSEvents {
private:
    static constexpr size_t InvalidPosition = std::numeric_limits<size_t>::max();

    FSScheduler* scheduler = nullptr;
    std::vector<FSEvent> events;
    std::vector<uint32_t> generations;
    std::vector<bool> occupied;
    std::vector<size_t> free_indices;

    std::vector<size_t> enabled_dense_indices;
    std::vector<size_t> enabled_sparse_positions;

    size_t active_event_count = 0;
    bool fixed_capacity_mode = false;

    bool is_handle_live(FSEventHandle handle) const {
        if (!handle.is_valid() || handle.index >= events.size()) {
            return false;
        }
        return occupied[handle.index] && generations[handle.index] == handle.generation;
    }

    void add_enabled_index(size_t index) {
        if (index >= enabled_sparse_positions.size() ||
            enabled_sparse_positions[index] != InvalidPosition) {
            return;
        }

        enabled_sparse_positions[index] = enabled_dense_indices.size();
        enabled_dense_indices.push_back(index);
    }

    void remove_enabled_index(size_t index) {
        if (index >= enabled_sparse_positions.size()) {
            return;
        }

        const size_t position = enabled_sparse_positions[index];
        if (position == InvalidPosition || position >= enabled_dense_indices.size()) {
            return;
        }

        const size_t tail_index = enabled_dense_indices.back();
        enabled_dense_indices[position] = tail_index;
        enabled_sparse_positions[tail_index] = position;

        enabled_dense_indices.pop_back();
        enabled_sparse_positions[index] = InvalidPosition;
    }

public:
    explicit FSEvents(FSScheduler* schedsrc = nullptr)
        : scheduler(schedsrc) {}

    void bind(FSScheduler* schedsrc) {
        scheduler = schedsrc;
    }

    void reserve_events(size_t count) {
        events.reserve(count);
        generations.reserve(count);
        occupied.reserve(count);
        free_indices.reserve(count);
        enabled_sparse_positions.reserve(count);
    }

    void set_fixed_capacity_mode(bool enabled) {
        fixed_capacity_mode = enabled;
    }

    FSEventHandle add_event(FSEvent event) {
        size_t index;
        if (!free_indices.empty()) {
            index = free_indices.back();
            free_indices.pop_back();
            events[index] = std::move(event);
            occupied[index] = true;
        } else {
            if (fixed_capacity_mode && events.size() == events.capacity()) {
                return FSEventHandle::invalid();
            }

            events.push_back(std::move(event));
            generations.push_back(0);
            occupied.push_back(true);
            enabled_sparse_positions.push_back(InvalidPosition);
            index = events.size() - 1;
        }

        if (events[index].is_enabled()) {
            add_enabled_index(index);
        }

        ++active_event_count;
        return FSEventHandle{index, generations[index]};
    }

    FSEvent* event_at(FSEventHandle handle) {
        if (!is_handle_live(handle)) {
            return nullptr;
        }
        return &events[handle.index];
    }

    const FSEvent* event_at(FSEventHandle handle) const {
        if (!is_handle_live(handle)) {
            return nullptr;
        }
        return &events[handle.index];
    }

    bool start_event(FSEventHandle handle) {
        auto* event = event_at(handle);
        if (event == nullptr) {
            return false;
        }
        event->set_enabled(true);
        add_enabled_index(handle.index);
        return true;
    }

    bool stop_event(FSEventHandle handle) {
        auto* event = event_at(handle);
        if (event == nullptr) {
            return false;
        }
        event->set_enabled(false);
        remove_enabled_index(handle.index);
        return true;
    }

    bool delete_event(FSEventHandle handle) {
        if (!is_handle_live(handle)) {
            return false;
        }

        remove_enabled_index(handle.index);

        events[handle.index] = FSEvent{};
        occupied[handle.index] = false;
        ++generations[handle.index];

        free_indices.push_back(handle.index);
        --active_event_count;
        return true;
    }

    bool poll_event(FSEventHandle handle, uint64_t now_ms) {
        if (scheduler == nullptr || !is_handle_live(handle)) {
            return false;
        }
        return events[handle.index].poll(*scheduler, now_ms);
    }

    size_t poll_all(uint64_t now_ms) {
        if (scheduler == nullptr) {
            return 0;
        }

        size_t dispatched = 0;
        for (size_t index : enabled_dense_indices) {
            if (!occupied[index]) {
                continue;
            }
            if (events[index].poll(*scheduler, now_ms)) {
                ++dispatched;
            }
        }
        return dispatched;
    }

    size_t poll_all() {
        if (scheduler == nullptr) {
            return 0;
        }
        return poll_all(scheduler->now_ms());
    }

    size_t size() const {
        return active_event_count;
    }
};
