#include "FSEvents.hpp"

#include "../Feather.hpp"

void* FSEvents::EventRecord::inline_storage_ptr() { return static_cast<void*>(inline_storage); }

FSEvents::EventRecord::EventRecord(EventRecord&& other) noexcept
    : storage(nullptr), ops(other.ops), priority(other.priority), enabled(other.enabled),
      uses_heap_storage(other.uses_heap_storage), dispatch_pending(other.dispatch_pending), dispatching(false), dispatch_epoch(other.dispatch_epoch) {
    if (other.storage != nullptr) {
        if (other.uses_heap_storage) {
            storage = other.storage;
        } else {
            storage = inline_storage_ptr();
            ops->move_construct_storage(storage, other.storage);
            ops->destroy_storage(other.storage);
        }
    }
    other.storage = nullptr; other.ops = nullptr; other.enabled = true; other.uses_heap_storage = false; other.dispatch_pending = false; other.dispatch_epoch = 0;
}
FSEvents::EventRecord& FSEvents::EventRecord::operator=(EventRecord&& other) noexcept {
    if (this != &other) {
        reset();
        ops = other.ops; priority = other.priority; enabled = other.enabled; uses_heap_storage = other.uses_heap_storage; dispatch_pending = other.dispatch_pending; dispatching = false; dispatch_epoch = other.dispatch_epoch;
        if (other.storage != nullptr) {
            if (other.uses_heap_storage) storage = other.storage;
            else { storage = inline_storage_ptr(); ops->move_construct_storage(storage, other.storage); ops->destroy_storage(other.storage); }
        }
        other.storage = nullptr; other.ops = nullptr; other.enabled = true; other.uses_heap_storage = false; other.dispatch_pending = false; other.dispatch_epoch = 0;
    }
    return *this;
}

void FSEvents::EventRecord::reset() {
    if (ops != nullptr && ops->destroy_storage != nullptr && storage != nullptr) {
        ops->destroy_storage(storage);
        if (uses_heap_storage) { ::operator delete(storage); }
    }
    storage = nullptr; ops = nullptr; priority = 0; enabled = true; uses_heap_storage = false; dispatch_pending = false; dispatching = false; dispatch_epoch = 0;
}
bool FSEvents::EventRecord::condition_matches(uint64_t now_ms) const { return enabled && storage && ops && ops->condition && ops->invoke_task && ops->condition(storage, now_ms); }
bool FSEvents::EventRecord::mark_dispatch_pending() { if (dispatch_pending) return false; dispatch_pending = true; return true; }
void FSEvents::EventRecord::cancel_pending_dispatch() { if (dispatch_pending) { dispatch_pending = false; ++dispatch_epoch; } }
bool FSEvents::EventRecord::consume_pending_dispatch(uint32_t epoch) { if (dispatch_epoch != epoch || !dispatch_pending) return false; dispatch_pending = false; return true; }
void FSEvents::EventRecord::dispatch(uint64_t now_ms) { if (!storage || !ops || !ops->invoke_task) return; dispatching = true; ops->invoke_task(storage, now_ms); dispatching = false; }

FSEvents::FSEvents(Feather& feather) : scheduler_(&feather.scheduler) {}
void FSEvents::reserve_events(size_t count){ slots_.reserve(count); free_indices_.reserve(count);} 
void FSEvents::set_fixed_capacity_mode(bool enabled){ fixed_capacity_mode_ = enabled; }
bool FSEvents::is_handle_live(FSEventHandle h) const { return h.is_valid() && h.index < slots_.size() && slots_[h.index].occupied && slots_[h.index].generation == h.generation; }
void FSEvents::add_enabled_index(size_t i){ if(i>=slots_.size()||slots_[i].enabled_position!=InvalidPosition) return; slots_[i].enabled_position=enabled_dense_indices_.size(); enabled_dense_indices_.push_back(i);} 
void FSEvents::remove_enabled_index(size_t i){ if(i>=slots_.size()) return; size_t p=slots_[i].enabled_position; if(p==InvalidPosition||p>=enabled_dense_indices_.size()) return; size_t t=enabled_dense_indices_.back(); enabled_dense_indices_[p]=t; slots_[t].enabled_position=p; enabled_dense_indices_.pop_back(); slots_[i].enabled_position=InvalidPosition; }

bool FSEvents::enqueue_dispatch_for_index(size_t index, uint64_t now_ms){ if(!scheduler_||index>=slots_.size()) return false; auto& slot=slots_[index]; if(!slot.occupied||!slot.event.condition_matches(now_ms)||!slot.event.mark_dispatch_pending()) return false; uint32_t gen=slot.generation, epoch=slot.event.dispatch_epoch; uint8_t pri=slot.event.priority; scheduler_->enqueue_ready_task([this,index,gen,epoch,now_ms](){ if(index>=slots_.size()) return; auto& s=slots_[index]; if(!s.occupied||s.generation!=gen) return; auto& e=s.event; if(!e.consume_pending_dispatch(epoch)||!e.enabled) return; e.dispatch(now_ms); if(s.pending_destroy && !e.dispatching){ s.event=EventRecord{}; s.pending_destroy=false; s.enabled_position=InvalidPosition; free_indices_.push_back(index);} }, pri); return true; }

bool FSEvents::start_event(FSEventHandle h){ if(!is_handle_live(h)) return false; slots_[h.index].event.enabled=true; add_enabled_index(h.index); return true; }
bool FSEvents::stop_event(FSEventHandle h){ if(!is_handle_live(h)) return false; slots_[h.index].event.enabled=false; slots_[h.index].event.cancel_pending_dispatch(); remove_enabled_index(h.index); return true; }
bool FSEvents::delete_event(FSEventHandle h){ if(!is_handle_live(h)) return false; remove_enabled_index(h.index); auto& s=slots_[h.index]; if(s.event.dispatching){ s.event.enabled=false; s.occupied=false; s.pending_destroy=true; s.enabled_position=InvalidPosition; ++s.generation; --active_event_count_; return true;} s.event=EventRecord{}; s.occupied=false; s.pending_destroy=false; s.enabled_position=InvalidPosition; ++s.generation; free_indices_.push_back(h.index); --active_event_count_; return true; }
size_t FSEvents::poll_all(uint64_t now_ms){ size_t d=0; for(size_t i:enabled_dense_indices_){ if(i<slots_.size() && slots_[i].occupied && enqueue_dispatch_for_index(i,now_ms)) ++d;} return d; }
size_t FSEvents::poll_all(){ return scheduler_ ? poll_all(scheduler_->now_ms()) : 0; }
bool FSEvents::start_loop(uint32_t poll_interval_ms, uint8_t priority){ if(!scheduler_||loop_task_id_!=0||poll_interval_ms==0) return false; loop_task_id_=scheduler_->add_periodic_task([this](){ poll_all(); }, scheduler_->now_ms()+poll_interval_ms, poll_interval_ms, priority, FSSchedulerPeriodicTaskRepeatAllocationType::Absolute); return loop_task_id_!=0; }
bool FSEvents::stop_loop(){ if(!scheduler_||loop_task_id_==0) return false; bool ok=scheduler_->cancel_task(loop_task_id_); loop_task_id_=0; return ok; }
size_t FSEvents::size() const { return active_event_count_; }
bool FSEvents::has_loop() const { return loop_task_id_!=0; }
