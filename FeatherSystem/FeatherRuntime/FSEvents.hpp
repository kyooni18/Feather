#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Scheduler/Scheduler.hpp"

class Feather;

struct FSEventHandle {
    size_t index = std::numeric_limits<size_t>::max();
    uint32_t generation = 0;

    static constexpr FSEventHandle invalid() { return FSEventHandle{}; }
    bool is_valid() const { return index != std::numeric_limits<size_t>::max(); }
};

class FSEvents {
private:
    static constexpr size_t InlineStorageSize = 64;
    static constexpr size_t InvalidPosition = std::numeric_limits<size_t>::max();

    struct EventOps {
        void (*destroy_storage)(void*) = nullptr;
        void (*move_construct_storage)(void* dst, void* src) = nullptr;
        bool (*condition)(void*, uint64_t) = nullptr;
        void (*invoke_task)(void*, uint64_t) = nullptr;
    };

    struct EventRecord {
        alignas(std::max_align_t) unsigned char inline_storage[InlineStorageSize];
        void* storage = nullptr;
        const EventOps* ops = nullptr;
        uint8_t priority = 0;
        bool enabled = true;
        bool uses_heap_storage = false;
        bool dispatch_pending = false;
        bool dispatching = false;
        uint32_t dispatch_epoch = 0;
        EventRecord() = default;
        ~EventRecord() { reset(); }
        EventRecord(const EventRecord&) = delete;
        EventRecord& operator=(const EventRecord&) = delete;
        EventRecord(EventRecord&& other) noexcept;
        EventRecord& operator=(EventRecord&& other) noexcept;

        void* inline_storage_ptr();
        void reset();
        bool condition_matches(uint64_t now_ms) const;
        bool mark_dispatch_pending();
        void cancel_pending_dispatch();
        bool consume_pending_dispatch(uint32_t epoch);
        void dispatch(uint64_t now_ms);
    };

    struct EventSlot {
        EventRecord event;
        uint32_t generation = 0;
        bool occupied = false;
        bool pending_destroy = false;
        size_t enabled_position = InvalidPosition;
    };

    FSScheduler* scheduler_ = nullptr;
    std::vector<EventSlot> slots_;
    std::vector<size_t> free_indices_;
    std::vector<size_t> enabled_dense_indices_;
    size_t active_event_count_ = 0;
    bool fixed_capacity_mode_ = false;

    bool is_handle_live(FSEventHandle handle) const;
    void add_enabled_index(size_t index);
    void remove_enabled_index(size_t index);
    bool enqueue_dispatch_for_index(size_t index, uint64_t now_ms);

    template<typename Storage>
    static constexpr bool fits_inline_storage() {
        return sizeof(Storage) <= InlineStorageSize &&
               alignof(Storage) <= alignof(std::max_align_t) &&
               std::is_nothrow_move_constructible_v<Storage>;
    }

public:
    explicit FSEvents(Feather& feather);

    void reserve_events(size_t count);
    void set_fixed_capacity_mode(bool enabled);

    template<typename Condition, typename Task>
    FSEventHandle add_event(Condition&& condition, Task&& task, uint8_t priority, bool enabled = true) {
        using StoredCondition = std::decay_t<Condition>;
        using StoredTask = std::decay_t<Task>;
        static_assert(std::is_invocable_r_v<bool, StoredCondition&, uint64_t>, "Condition must be callable as bool(uint64_t)");
        static_assert(std::is_invocable_r_v<void, StoredTask&> || std::is_invocable_r_v<void, StoredTask&, uint64_t>, "Task must be callable as void() or void(uint64_t)");

        struct Storage { StoredCondition condition; StoredTask task; };
        static const EventOps ops{
            [](void* ptr) { static_cast<Storage*>(ptr)->~Storage(); },
            [](void* dst, void* src) { new (dst) Storage(std::move(*static_cast<Storage*>(src))); },
            [](void* ptr, uint64_t now_ms) -> bool { return static_cast<Storage*>(ptr)->condition(now_ms); },
            [](void* ptr, uint64_t now_ms) {
                auto* typed = static_cast<Storage*>(ptr);
                if constexpr (std::is_invocable_r_v<void, StoredTask&, uint64_t>) { typed->task(now_ms); } else { typed->task(); }
            }
        };

        EventRecord record;
        if constexpr (fits_inline_storage<Storage>()) {
            record.storage = record.inline_storage_ptr();
            new (record.storage) Storage{std::forward<Condition>(condition), std::forward<Task>(task)};
            record.uses_heap_storage = false;
        } else {
            record.storage = ::operator new(sizeof(Storage));
            new (record.storage) Storage{std::forward<Condition>(condition), std::forward<Task>(task)};
            record.uses_heap_storage = true;
        }
        record.ops = &ops;
        record.priority = static_cast<uint8_t>(priority & 0x0F);
        record.enabled = enabled;

        size_t index;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
            slots_[index].event = std::move(record);
            slots_[index].occupied = true;
            slots_[index].pending_destroy = false;
        } else {
            if (fixed_capacity_mode_ && slots_.size() == slots_.capacity()) { return FSEventHandle::invalid(); }
            slots_.emplace_back();
            index = slots_.size() - 1;
            slots_[index].event = std::move(record);
            slots_[index].occupied = true;
            slots_[index].pending_destroy = false;
        }
        if (slots_[index].event.enabled) { add_enabled_index(index); }
        ++active_event_count_;
        return FSEventHandle{index, slots_[index].generation};
    }

    bool start_event(FSEventHandle handle);
    bool stop_event(FSEventHandle handle);
    bool delete_event(FSEventHandle handle);

    size_t poll_all(uint64_t now_ms);
    size_t poll_all();

    bool start_loop(uint32_t poll_interval_ms = 1, uint8_t priority = 0);
    bool stop_loop();

    size_t size() const;
    bool has_loop() const;

private:
    uint64_t loop_task_id_ = 0;
};
