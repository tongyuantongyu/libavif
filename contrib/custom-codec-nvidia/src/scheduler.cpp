#include "../include/scheduler.h"

#include <algorithm>
#include <cmath>

namespace avif_nvenc
{
namespace
{

constexpr double kDemandDecayAlpha = 0.9;
constexpr double kWeightEpsilon = 1e-9;
constexpr double kPruneWeightThreshold = 1e-3;

double decay_factor_for_events(uint64_t event_delta) noexcept
{
    return (event_delta == 0) ? 1.0 : std::pow(kDemandDecayAlpha, static_cast<double>(event_delta));
}

size_t ceil_weight_to_sessions(double weight) noexcept
{
    return (weight <= kWeightEpsilon) ? 0u : static_cast<size_t>(std::ceil(weight - kWeightEpsilon));
}

size_t floor_weight_to_sessions(double weight) noexcept
{
    return (weight <= kWeightEpsilon) ? 0u : static_cast<size_t>(std::floor(weight + kWeightEpsilon));
}

} // namespace

uint64_t acquire_ticket_gate::issue_ticket() noexcept
{
    return next_ticket_++;
}

bool acquire_ticket_gate::is_serving(uint64_t ticket) const noexcept
{
    return ticket == serving_ticket_;
}

void acquire_ticket_gate::complete_ticket() noexcept
{
    ++serving_ticket_;
}

pool_scheduler::pool_scheduler(size_t max_sessions, size_t frame_capacity) : max_sessions_(max_sessions), frame_capacity_(frame_capacity)
{
}

scheduler_decision pool_scheduler::plan_acquire(const session_key & current_key)
{
    scheduler_decision decision;
    decision.request_epoch = acquire_epoch_ + 1;
    kind_state & current_kind = recompute_targets(current_key, decision.request_epoch);

    if (current_kind.members.size() < current_kind.target_sessions) {
        decision.next_action = scheduler_decision::action::create;
        if (session_order_.size() >= max_sessions_) {
            decision.evict_session_id = extract_admission_victim(current_key, decision.request_epoch);
            if (!decision.evict_session_id.has_value()) {
                decision.next_action = scheduler_decision::action::wait;
            }
        }
        return decision;
    }

    if (std::optional<uint64_t> reusable_session = find_round_robin_session_with_capacity(current_kind)) {
        decision.next_action = scheduler_decision::action::reuse;
        decision.session_id = reusable_session;
    }
    return decision;
}

void pool_scheduler::add_session(uint64_t session_id, const session_key & key)
{
    kind_state & kind = ensure_kind_state(key);
    session_record & record = sessions_[session_id];
    record.key = key;
    record.kind = &kind;
    record.active_reservations = 0;
    record.last_idle_seq = ++idle_sequence_;
    record.last_served_seq = 0;
    session_order_.push_back(session_id);
    kind.members.push_back(session_id);
}

void pool_scheduler::remove_session(uint64_t session_id)
{
    const auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return;
    }

    const session_key key = session_it->second.key;
    remove_session_from_kind(session_id, session_it->second);
    std::erase(session_order_, session_id);
    sessions_.erase(session_it);
    prune_kind_if_unused(key);
}

void pool_scheduler::commit_reservation(uint64_t request_epoch, const session_key & request_key, uint64_t session_id)
{
    const auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return;
    }

    session_record & record = session_it->second;
    ++record.active_reservations;
    record.last_served_seq = ++reservation_sequence_;
    commit_request_sample(request_key, request_epoch);
}

void pool_scheduler::release_reservation(uint64_t session_id)
{
    const auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return;
    }

    session_record & record = session_it->second;
    if (record.active_reservations == 0) {
        return;
    }
    --record.active_reservations;
    if (record.active_reservations == 0) {
        record.last_idle_seq = ++idle_sequence_;
    }
}

void pool_scheduler::set_active_reservations(uint64_t session_id, size_t active_reservations)
{
    const auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return;
    }

    session_record & record = session_it->second;
    const bool was_idle = (record.active_reservations == 0);
    record.active_reservations = active_reservations;
    if (!was_idle && (active_reservations == 0)) {
        record.last_idle_seq = ++idle_sequence_;
    }
}

void pool_scheduler::clear()
{
    session_order_.clear();
    sessions_.clear();
    kind_states_.clear();
    acquire_epoch_ = 0;
    idle_sequence_ = 0;
    reservation_sequence_ = 0;
}

bool pool_scheduler::has_kind(const session_key & key) const
{
    return kind_states_.find(key) != kind_states_.end();
}

size_t pool_scheduler::live_session_count() const noexcept
{
    return session_order_.size();
}

size_t pool_scheduler::live_sessions_for(const session_key & key) const
{
    const auto kind_it = kind_states_.find(key);
    return (kind_it == kind_states_.end()) ? 0u : kind_it->second.members.size();
}

size_t pool_scheduler::target_sessions_for(const session_key & key) const
{
    const auto kind_it = kind_states_.find(key);
    return (kind_it == kind_states_.end()) ? 0u : kind_it->second.target_sessions;
}

size_t pool_scheduler::active_reservations_for(uint64_t session_id) const
{
    const auto session_it = sessions_.find(session_id);
    return (session_it == sessions_.end()) ? 0u : session_it->second.active_reservations;
}

pool_scheduler::kind_state & pool_scheduler::ensure_kind_state(const session_key & key)
{
    return kind_states_.try_emplace(key).first->second;
}

double pool_scheduler::decayed_weight_at_epoch(const kind_state & kind, uint64_t epoch) const noexcept
{
    if (epoch <= kind.last_decay_event) {
        return kind.decayed_weight;
    }
    return kind.decayed_weight * decay_factor_for_events(epoch - kind.last_decay_event);
}

void pool_scheduler::commit_request_sample(const session_key & key, uint64_t epoch)
{
    kind_state & kind = ensure_kind_state(key);
    kind.decayed_weight = decayed_weight_at_epoch(kind, epoch) + 1.0;
    kind.last_decay_event = epoch;
    acquire_epoch_ = epoch;
}

void pool_scheduler::prune_kind_if_unused(const session_key & key)
{
    const auto kind_it = kind_states_.find(key);
    if (kind_it == kind_states_.end()) {
        return;
    }
    if (!kind_it->second.members.empty()) {
        return;
    }
    if (kind_it->second.target_sessions != 0) {
        return;
    }
    if (decayed_weight_at_epoch(kind_it->second, acquire_epoch_) >= kPruneWeightThreshold) {
        return;
    }
    kind_states_.erase(kind_it);
}

pool_scheduler::kind_state & pool_scheduler::recompute_targets(const session_key & current_key, uint64_t epoch)
{
    kind_state & current_kind = ensure_kind_state(current_key);
    for (auto & [_, kind] : kind_states_) {
        kind.target_sessions = 0;
    }

    std::vector<kind_projection> projections;
    std::vector<session_key> prune_candidates;
    projections.reserve(kind_states_.size());

    size_t stable_index = 0;
    double total_weight = 0.0;
    for (auto & [key, kind] : kind_states_) {
        const bool is_current = (key == current_key);
        const double weight = decayed_weight_at_epoch(kind, epoch);
        if (!is_current && kind.members.empty() && (weight < kPruneWeightThreshold)) {
            prune_candidates.push_back(key);
            continue;
        }
        const double projected_weight = weight + (is_current ? 1.0 : 0.0);
        if ((projected_weight > kWeightEpsilon) || !kind.members.empty() || is_current) {
            projections.push_back(kind_projection {
                key,
                &kind,
                projected_weight,
                kind.members.size(),
                0,
                0,
                stable_index++,
                is_current,
            });
            total_weight += projected_weight;
        }
    }

    if (!projections.empty() && (total_weight > kWeightEpsilon)) {
        struct remainder_entry
        {
            size_t projection_index = 0;
            double remainder = 0.0;
            size_t stable_index = 0;
        };

        size_t assigned_targets = 0;
        std::vector<remainder_entry> remainders;
        remainders.reserve(projections.size());

        for (size_t i = 0; i < projections.size(); ++i) {
            kind_projection & projection = projections[i];
            const double scaled_target = static_cast<double>(max_sessions_) * projection.projected_weight / total_weight;
            projection.ratio_target = static_cast<size_t>(std::floor(scaled_target + kWeightEpsilon));
            assigned_targets += projection.ratio_target;
            remainders.push_back(remainder_entry {
                i,
                scaled_target - static_cast<double>(projection.ratio_target),
                projection.stable_index,
            });
        }

        const size_t remaining_targets = (assigned_targets < max_sessions_) ? (max_sessions_ - assigned_targets) : 0u;
        std::ranges::stable_sort(remainders, [](const remainder_entry & lhs, const remainder_entry & rhs) {
            if (std::fabs(lhs.remainder - rhs.remainder) > kWeightEpsilon) {
                return lhs.remainder > rhs.remainder;
            }
            return lhs.stable_index < rhs.stable_index;
        });
        for (size_t i = 0; (i < remaining_targets) && (i < remainders.size()); ++i) {
            ++projections[remainders[i].projection_index].ratio_target;
        }

        for (kind_projection & projection : projections) {
            const size_t evidence_cap = projection.is_current ? ceil_weight_to_sessions(projection.projected_weight)
                                                              : floor_weight_to_sessions(projection.projected_weight);
            projection.effective_target = std::min(projection.ratio_target, evidence_cap);
            if (projection.is_current && (projection.live_sessions == 0) && (projection.projected_weight > kWeightEpsilon)) {
                projection.effective_target = std::max<size_t>(1, projection.effective_target);
            }
            projection.kind->target_sessions = projection.effective_target;
        }
    }

    for (const session_key & key : prune_candidates) {
        kind_states_.erase(key);
    }
    return current_kind;
}

std::optional<uint64_t> pool_scheduler::find_round_robin_session_with_capacity(kind_state & kind)
{
    const size_t session_count = kind.members.size();
    if (session_count == 0) {
        return std::nullopt;
    }

    for (size_t offset = 0; offset < session_count; ++offset) {
        const size_t index = (kind.rr_next + offset) % session_count;
        const uint64_t session_id = kind.members[index];
        const auto session_it = sessions_.find(session_id);
        if (session_it == sessions_.end()) {
            continue;
        }
        if (session_it->second.active_reservations >= frame_capacity_) {
            continue;
        }
        kind.rr_next = (index + 1) % session_count;
        return session_id;
    }
    return std::nullopt;
}

std::optional<uint64_t> pool_scheduler::extract_admission_victim(const session_key & current_key, uint64_t epoch)
{
    const session_record * preferred = nullptr;
    uint64_t preferred_id = 0;
    for (const uint64_t session_id : session_order_) {
        const auto session_it = sessions_.find(session_id);
        if (session_it == sessions_.end()) {
            continue;
        }
        const session_record & record = session_it->second;
        if ((record.active_reservations != 0) || (record.key == current_key)) {
            continue;
        }
        if (record.kind->members.size() <= record.kind->target_sessions) {
            continue;
        }
        if (!preferred || (record.last_idle_seq < preferred->last_idle_seq)) {
            preferred = &record;
            preferred_id = session_id;
        }
    }
    if (preferred) {
        return preferred_id;
    }

    const session_record * fallback = nullptr;
    uint64_t fallback_id = 0;
    double fallback_weight = 0.0;
    for (const uint64_t session_id : session_order_) {
        const auto session_it = sessions_.find(session_id);
        if (session_it == sessions_.end()) {
            continue;
        }
        const session_record & record = session_it->second;
        if ((record.active_reservations != 0) || (record.key == current_key)) {
            continue;
        }
        const double weight = decayed_weight_at_epoch(*record.kind, epoch);
        if (!fallback || (weight < (fallback_weight - kWeightEpsilon)) ||
            ((std::fabs(weight - fallback_weight) <= kWeightEpsilon) && (record.last_idle_seq < fallback->last_idle_seq))) {
            fallback = &record;
            fallback_id = session_id;
            fallback_weight = weight;
        }
    }
    return fallback ? std::optional<uint64_t>(fallback_id) : std::nullopt;
}

void pool_scheduler::remove_session_from_kind(uint64_t session_id, session_record & record)
{
    if (!record.kind) {
        return;
    }

    auto & members = record.kind->members;
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (*it != session_id) {
            continue;
        }
        const size_t removed_index = static_cast<size_t>(it - members.begin());
        members.erase(it);
        if (members.empty()) {
            record.kind->rr_next = 0;
        } else {
            if (record.kind->rr_next > removed_index) {
                --record.kind->rr_next;
            }
            if (record.kind->rr_next >= members.size()) {
                record.kind->rr_next = 0;
            }
        }
        break;
    }
}

} // namespace avif_nvenc
