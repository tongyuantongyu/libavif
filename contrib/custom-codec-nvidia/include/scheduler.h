#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SCHEDULER_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "codec_options.h"

namespace avif_nvenc
{

struct scheduler_decision
{
    enum class action
    {
        create,
        reuse,
        wait,
    };

    action next_action = action::wait;
    uint64_t request_epoch = 0;
    std::optional<uint64_t> session_id;
    std::optional<uint64_t> evict_session_id;
};

class acquire_ticket_gate
{
public:
    uint64_t issue_ticket() noexcept;
    bool is_serving(uint64_t ticket) const noexcept;
    void complete_ticket() noexcept;

private:
    uint64_t next_ticket_ = 0;
    uint64_t serving_ticket_ = 0;
};

class pool_scheduler
{
public:
    pool_scheduler(size_t max_sessions, size_t frame_capacity);

    scheduler_decision plan_acquire(const session_key & current_key);

    void add_session(uint64_t session_id, const session_key & key);
    void remove_session(uint64_t session_id);
    void commit_reservation(uint64_t request_epoch, const session_key & request_key, uint64_t session_id);
    void release_reservation(uint64_t session_id);
    void set_active_reservations(uint64_t session_id, size_t active_reservations);
    void clear();

    bool has_kind(const session_key & key) const;
    size_t live_session_count() const noexcept;
    size_t live_sessions_for(const session_key & key) const;
    size_t target_sessions_for(const session_key & key) const;
    size_t active_reservations_for(uint64_t session_id) const;

private:
    struct kind_state;

    struct session_record
    {
        session_key key;
        kind_state * kind = nullptr;
        size_t active_reservations = 0;
        uint64_t last_idle_seq = 0;
        uint64_t last_served_seq = 0;
    };

    struct kind_state
    {
        std::vector<uint64_t> members;
        size_t rr_next = 0;
        double decayed_weight = 0.0;
        uint64_t last_decay_event = 0;
        size_t target_sessions = 0;
    };

    struct kind_projection
    {
        session_key key;
        kind_state * kind = nullptr;
        double projected_weight = 0.0;
        size_t live_sessions = 0;
        size_t ratio_target = 0;
        size_t effective_target = 0;
        size_t stable_index = 0;
        bool is_current = false;
    };

    kind_state & ensure_kind_state(const session_key & key);
    double decayed_weight_at_epoch(const kind_state & kind, uint64_t epoch) const noexcept;
    void commit_request_sample(const session_key & key, uint64_t epoch);
    void prune_kind_if_unused(const session_key & key);
    kind_state & recompute_targets(const session_key & current_key, uint64_t epoch);
    std::optional<uint64_t> find_round_robin_session_with_capacity(kind_state & kind);
    std::optional<uint64_t> extract_admission_victim(const session_key & current_key, uint64_t epoch);
    void remove_session_from_kind(uint64_t session_id, session_record & record);

    size_t max_sessions_ = 0;
    size_t frame_capacity_ = 0;
    std::vector<uint64_t> session_order_;
    std::map<uint64_t, session_record> sessions_;
    std::map<session_key, kind_state> kind_states_;
    uint64_t acquire_epoch_ = 0;
    uint64_t idle_sequence_ = 0;
    uint64_t reservation_sequence_ = 0;
};

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SCHEDULER_H
