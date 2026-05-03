#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Scheduler/Scheduler.hpp"

class FSEvent {
private:
    static constexpr size_t InlineStorageSize = 64;

    struct Ops {
        void (*destroy_storage)(void*) = nullptr;
        void (*move_construct_storage)(void* dst, void* src) = nullptr;
        bool (*condition)(void*, uint64_t) = nullptr;
        void (*invoke_task)(void*, uint64_t) = nullptr;
    };

    alignas(std::max_align_t) unsigned char inline_storage_[InlineStorageSize];
    void* storage_ = nullptr;
    const Ops* ops_ = nullptr;
    bool enabled_ = true;
    bool uses_heap_storage_ = false;
    bool dispatch_pending_ = false;
    bool dispatching_ = false;
    uint32_t dispatch_epoch_ = 0;
    uint8_t priority_ = 0;

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
        dispatch_pending_ = false;
        dispatching_ = false;
        dispatch_epoch_ = 0;
        priority_ = 0;
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

        struct Storage {
            StoredCondition condition;
            StoredTask task;
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
            [](void* ptr, uint64_t now_ms) {
                auto* typed = static_cast<Storage*>(ptr);
                if constexpr (std::is_invocable_r_v<void, StoredTask&, uint64_t>) {
                    typed->task(now_ms);
                } else {
                    typed->task();
                }
            }
        };

        FSEvent event;
        if constexpr (fits_inline_storage<Storage>()) {
            event.storage_ = event.inline_storage_ptr();
            new (event.storage_) Storage{
                std::forward<Condition>(condition),
                std::forward<Task>(task)
            };
            event.uses_heap_storage_ = false;
        } else {
            event.storage_ = ::operator new(sizeof(Storage));
            new (event.storage_) Storage{
                std::forward<Condition>(condition),
                std::forward<Task>(task)
            };
            event.uses_heap_storage_ = true;
        }

        event.ops_ = &ops;
        event.enabled_ = enabled;
        event.priority_ = static_cast<uint8_t>(priority & 0x0F);
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
        , uses_heap_storage_(other.uses_heap_storage_)
        , dispatch_pending_(other.dispatch_pending_)
        , dispatching_(false)
        , dispatch_epoch_(other.dispatch_epoch_)
        , priority_(other.priority_) {
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
        other.dispatch_pending_ = false;
        other.dispatching_ = false;
        other.dispatch_epoch_ = 0;
        other.priority_ = 0;
    }

    FSEvent& operator=(FSEvent&& other) noexcept {
        if (this != &other) {
            reset();
            ops_ = other.ops_;
            enabled_ = other.enabled_;
            uses_heap_storage_ = other.uses_heap_storage_;
            dispatch_pending_ = other.dispatch_pending_;
            dispatching_ = false;
            dispatch_epoch_ = other.dispatch_epoch_;
            priority_ = other.priority_;

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
            other.dispatch_pending_ = false;
            other.dispatching_ = false;
            other.dispatch_epoch_ = 0;
            other.priority_ = 0;
        }
        return *this;
    }

    FSEvent(const FSEvent&) = delete;
    FSEvent& operator=(const FSEvent&) = delete;

    bool is_enabled() const {
        return enabled_;
    }

    void set_enabled(bool enabled) {
        if (!enabled) {
            cancel_pending_dispatch();
        }
        enabled_ = enabled;
    }

    uint8_t priority() const {
        return priority_;
    }

    bool is_dispatching() const {
        return dispatching_;
    }

    uint32_t dispatch_epoch() const {
        return dispatch_epoch_;
    }

    bool mark_dispatch_pending() {
        if (dispatch_pending_) {
            return false;
        }
        dispatch_pending_ = true;
        return true;
    }

    void cancel_pending_dispatch() {
        if (dispatch_pending_) {
            dispatch_pending_ = false;
            ++dispatch_epoch_;
        }
    }

    bool consume_pending_dispatch(uint32_t epoch) {
        if (dispatch_epoch_ != epoch || !dispatch_pending_) {
            return false;
        }
        dispatch_pending_ = false;
        return true;
    }

    bool condition_matches(uint64_t now_ms) {
        if (!enabled_ || storage_ == nullptr || ops_ == nullptr ||
            ops_->condition == nullptr || ops_->invoke_task == nullptr) {
            return false;
        }

        if (!ops_->condition(storage_, now_ms)) {
            return false;
        }

        return true;
    }

    void dispatch(uint64_t now_ms) {
        if (storage_ == nullptr || ops_ == nullptr || ops_->invoke_task == nullptr) {
            return;
        }
        dispatching_ = true;
        ops_->invoke_task(storage_, now_ms);
        dispatching_ = false;
    }

    bool poll(FSScheduler& scheduler, uint64_t now_ms) {
        if (!condition_matches(now_ms) || !mark_dispatch_pending()) {
            return false;
        }

        const uint32_t epoch = dispatch_epoch_;
        scheduler.enqueue_ready_task(
            [this, epoch, now_ms]() {
                if (!consume_pending_dispatch(epoch) || !is_enabled()) {
                    return;
                }
                dispatch(now_ms);
            },
            priority_
        );
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

    struct EventSlot {
        FSEvent event;
        uint32_t generation = 0;
        bool occupied = false;
        bool pending_destroy = false;
        size_t enabled_position = InvalidPosition;
    };

    FSScheduler* scheduler = nullptr;
    std::vector<EventSlot> slots;
    std::vector<size_t> free_indices;

    std::vector<size_t> enabled_dense_indices;

    size_t active_event_count = 0;
    bool fixed_capacity_mode = false;

    bool is_handle_live(FSEventHandle handle) const {
        if (!handle.is_valid() || handle.index >= slots.size()) {
            return false;
        }
        const auto& slot = slots[handle.index];
        return slot.occupied && slot.generation == handle.generation;
    }

    void add_enabled_index(size_t index) {
        if (index >= slots.size() || slots[index].enabled_position != InvalidPosition) {
            return;
        }

        slots[index].enabled_position = enabled_dense_indices.size();
        enabled_dense_indices.push_back(index);
    }

    void remove_enabled_index(size_t index) {
        if (index >= slots.size()) {
            return;
        }

        const size_t position = slots[index].enabled_position;
        if (position == InvalidPosition || position >= enabled_dense_indices.size()) {
            return;
        }

        const size_t tail_index = enabled_dense_indices.back();
        enabled_dense_indices[position] = tail_index;
        slots[tail_index].enabled_position = position;

        enabled_dense_indices.pop_back();
        slots[index].enabled_position = InvalidPosition;
    }

    bool enqueue_dispatch_for_index(size_t index, uint64_t now_ms) {
        if (scheduler == nullptr || index >= slots.size()) {
            return false;
        }

        auto& slot = slots[index];
        if (!slot.occupied || !slot.event.condition_matches(now_ms) ||
            !slot.event.mark_dispatch_pending()) {
            return false;
        }

        const uint32_t generation = slot.generation;
        const uint32_t epoch = slot.event.dispatch_epoch();
        scheduler->enqueue_ready_task(
            [this, index, generation, epoch, now_ms]() {
                if (index >= slots.size()) {
                    return;
                }

                auto& current_slot = slots[index];
                if (!current_slot.occupied || current_slot.generation != generation) {
                    return;
                }

                auto& event = current_slot.event;
                if (!event.consume_pending_dispatch(epoch) || !event.is_enabled()) {
                    return;
                }

                event.dispatch(now_ms);
                if (current_slot.pending_destroy && !event.is_dispatching()) {
                    current_slot.event = FSEvent{};
                    current_slot.pending_destroy = false;
                    current_slot.enabled_position = InvalidPosition;
                    free_indices.push_back(index);
                }
            },
            slot.event.priority()
        );
        return true;
    }

public:
    explicit FSEvents(FSScheduler* schedsrc = nullptr)
        : scheduler(schedsrc) {}

    void bind(FSScheduler* schedsrc) {
        scheduler = schedsrc;
    }

    void reserve_events(size_t count) {
        slots.reserve(count);
        free_indices.reserve(count);
    }

    void set_fixed_capacity_mode(bool enabled) {
        fixed_capacity_mode = enabled;
    }

    FSEventHandle add_event(FSEvent event) {
        size_t index;
        if (!free_indices.empty()) {
            index = free_indices.back();
            free_indices.pop_back();
            slots[index].event = std::move(event);
            slots[index].occupied = true;
            slots[index].pending_destroy = false;
        } else {
            if (fixed_capacity_mode && slots.size() == slots.capacity()) {
                return FSEventHandle::invalid();
            }

            slots.emplace_back();
            index = slots.size() - 1;
            slots[index].event = std::move(event);
            slots[index].occupied = true;
            slots[index].pending_destroy = false;
        }

        if (slots[index].event.is_enabled()) {
            add_enabled_index(index);
        }

        ++active_event_count;
        return FSEventHandle{index, slots[index].generation};
    }

    FSEvent* event_at(FSEventHandle handle) {
        if (!is_handle_live(handle)) {
            return nullptr;
        }
        return &slots[handle.index].event;
    }

    const FSEvent* event_at(FSEventHandle handle) const {
        if (!is_handle_live(handle)) {
            return nullptr;
        }
        return &slots[handle.index].event;
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

        if (slots[handle.index].event.is_dispatching()) {
            slots[handle.index].event.set_enabled(false);
            slots[handle.index].occupied = false;
            slots[handle.index].pending_destroy = true;
            slots[handle.index].enabled_position = InvalidPosition;
            ++slots[handle.index].generation;
            --active_event_count;
            return true;
        }

        slots[handle.index].event = FSEvent{};
        slots[handle.index].occupied = false;
        slots[handle.index].pending_destroy = false;
        slots[handle.index].enabled_position = InvalidPosition;
        ++slots[handle.index].generation;

        free_indices.push_back(handle.index);
        --active_event_count;
        return true;
    }

    bool poll_event(FSEventHandle handle, uint64_t now_ms) {
        if (scheduler == nullptr || !is_handle_live(handle)) {
            return false;
        }
        return enqueue_dispatch_for_index(handle.index, now_ms);
    }

    size_t poll_all(uint64_t now_ms) {
        if (scheduler == nullptr) {
            return 0;
        }

        size_t dispatched = 0;
        for (size_t index : enabled_dense_indices) {
            if (index >= slots.size() || !slots[index].occupied) {
                continue;
            }
            if (enqueue_dispatch_for_index(index, now_ms)) {
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
