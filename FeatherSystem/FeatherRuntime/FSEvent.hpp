#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "FSScheduler.hpp"

struct FSEventPollResult {
    bool trigger_matched   = false;
    bool condition_matched = false;
    bool action_dispatched = false;
};

class FSEvent {
private:
    struct Ops {
        void (*destroy_storage)(void*) = nullptr;
        void* (*context_ptr)(void*) = nullptr;
        const void* (*const_context_ptr)(const void*) = nullptr;
        bool (*trigger)(void*, uint64_t) = nullptr;
        bool (*condition)(void*, uint64_t) = nullptr;
        void (*action)(void*, FSScheduler&, uint64_t) = nullptr;
    };

    void* storage_ = nullptr;
    const Ops* ops_ = nullptr;
    bool enabled_ = true;

    void reset() {
        if (ops_ != nullptr && ops_->destroy_storage != nullptr) {
            ops_->destroy_storage(storage_);
        }
        storage_ = nullptr;
        ops_ = nullptr;
        enabled_ = true;
    }

public:
    FSEvent() = default;

    template<typename Context, typename Trigger, typename Condition, typename Action>
    static FSEvent make(
        Context&&   context,
        Trigger&&   trigger,
        Condition&& condition,
        Action&&    action,
        bool        enabled = true
    ) {
        using StoredContext = std::decay_t<Context>;
        using StoredTrigger = std::decay_t<Trigger>;
        using StoredCondition = std::decay_t<Condition>;
        using StoredAction = std::decay_t<Action>;

        static_assert(
            std::is_invocable_r_v<bool, StoredTrigger&, StoredContext&, uint64_t>,
            "Trigger must be callable as bool(Context&, uint64_t)"
        );
        static_assert(
            std::is_invocable_r_v<bool, StoredCondition&, StoredContext&, uint64_t>,
            "Condition must be callable as bool(Context&, uint64_t)"
        );
        static_assert(
            std::is_invocable_r_v<void, StoredAction&, StoredContext&, FSScheduler&, uint64_t>,
            "Action must be callable as void(Context&, FSScheduler&, uint64_t)"
        );

        struct Storage {
            StoredContext context;
            StoredTrigger trigger;
            StoredCondition condition;
            StoredAction action;
        };

        // One dispatch table is shared per concrete event type, so each event
        // instance only keeps a storage pointer and this table pointer.
        static const Ops ops{
            [](void* ptr) {
                delete static_cast<Storage*>(ptr);
            },
            [](void* ptr) -> void* {
                return &static_cast<Storage*>(ptr)->context;
            },
            [](const void* ptr) -> const void* {
                return &static_cast<const Storage*>(ptr)->context;
            },
            [](void* ptr, uint64_t now_ms) -> bool {
                auto* typed = static_cast<Storage*>(ptr);
                return typed->trigger(typed->context, now_ms);
            },
            [](void* ptr, uint64_t now_ms) -> bool {
                auto* typed = static_cast<Storage*>(ptr);
                return typed->condition(typed->context, now_ms);
            },
            [](void* ptr, FSScheduler& scheduler, uint64_t now_ms) {
                auto* typed = static_cast<Storage*>(ptr);
                typed->action(typed->context, scheduler, now_ms);
            }
        };

        auto* storage = new Storage{
            std::forward<Context>(context),
            std::forward<Trigger>(trigger),
            std::forward<Condition>(condition),
            std::forward<Action>(action)
        };

        FSEvent event;
        event.storage_ = storage;
        event.ops_ = &ops;
        event.enabled_ = enabled;
        return event;
    }

    ~FSEvent() {
        reset();
    }

    FSEvent(FSEvent&& other) noexcept
        : storage_(other.storage_)
        , ops_(other.ops_)
        , enabled_(other.enabled_) {
        other.storage_ = nullptr;
        other.ops_ = nullptr;
        other.enabled_ = true;
    }

    FSEvent& operator=(FSEvent&& other) noexcept {
        if (this != &other) {
            reset();
            storage_ = other.storage_;
            ops_ = other.ops_;
            enabled_ = other.enabled_;

            other.storage_ = nullptr;
            other.ops_ = nullptr;
            other.enabled_ = true;
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

    template<typename Context>
    Context* context_as() {
        if (storage_ == nullptr || ops_ == nullptr || ops_->context_ptr == nullptr) {
            return nullptr;
        }
        return static_cast<Context*>(ops_->context_ptr(storage_));
    }

    template<typename Context>
    const Context* context_as() const {
        if (storage_ == nullptr || ops_ == nullptr || ops_->const_context_ptr == nullptr) {
            return nullptr;
        }
        return static_cast<const Context*>(ops_->const_context_ptr(storage_));
    }

    FSEventPollResult poll(FSScheduler& scheduler, uint64_t now_ms) {
        FSEventPollResult result;
        if (!enabled_ || storage_ == nullptr || ops_ == nullptr ||
            ops_->trigger == nullptr || ops_->action == nullptr) {
            return result;
        }

        result.trigger_matched = ops_->trigger(storage_, now_ms);
        if (!result.trigger_matched) {
            return result;
        }

        result.condition_matched =
            (ops_->condition == nullptr) ? true : ops_->condition(storage_, now_ms);
        if (!result.condition_matched) {
            return result;
        }

        ops_->action(storage_, scheduler, now_ms);
        result.action_dispatched = true;
        return result;
    }
};

class FSEvents {
private:
    FSScheduler* scheduler = nullptr;
    std::vector<FSEvent> events;

public:
    explicit FSEvents(FSScheduler* schedsrc = nullptr)
        : scheduler(schedsrc)
        , events{} {}

    void bind(FSScheduler* schedsrc) {
        scheduler = schedsrc;
    }

    size_t add_event(FSEvent event) {
        events.push_back(std::move(event));
        return events.size() - 1;
    }

    FSEvent* event_at(size_t index) {
        if (index >= events.size()) {
            return nullptr;
        }
        return &events[index];
    }

    const FSEvent* event_at(size_t index) const {
        if (index >= events.size()) {
            return nullptr;
        }
        return &events[index];
    }

    bool poll_event(size_t index, uint64_t now_ms) {
        if (scheduler == nullptr || index >= events.size()) {
            return false;
        }
        return events[index].poll(*scheduler, now_ms).action_dispatched;
    }

    size_t poll_all(uint64_t now_ms) {
        if (scheduler == nullptr) {
            return 0;
        }

        size_t dispatched = 0;
        for (auto& event : events) {
            if (event.poll(*scheduler, now_ms).action_dispatched) {
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
        return events.size();
    }
};
