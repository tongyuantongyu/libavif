#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "../include/scheduler.h"
#include "gtest/gtest.h"

namespace avif_nvenc
{
namespace
{

TEST(NvencWatermarkTest, ProtectsLargerSuccessorAndPrioritizesOnlyAtHighWatermark)
{
    upload_watermark watermark(8);
    watermark.admit(1, 2);
    watermark.admit(2, 4);
    watermark.upload(1);
    watermark.upload(1); // The two-layer head is ready to encode.
    watermark.upload(2);
    watermark.admit(3, 4);
    watermark.upload(3);
    watermark.admit(4, 4);
    watermark.upload(4); // Three slots remain, protected for a four-layer successor.
    EXPECT_FALSE(watermark.can_upload(3));
    EXPECT_FALSE(watermark.can_upload(4));
    EXPECT_TRUE(watermark.can_upload(2));
    watermark.release(2);
    watermark.remove(1);
    EXPECT_TRUE(watermark.can_upload(3)); // Low watermark: later groups may advance.
    watermark.upload(3);
    watermark.upload(2);
    EXPECT_FALSE(watermark.can_upload(3));
    watermark.upload(2);
    watermark.upload(2);
    EXPECT_EQ(watermark.remaining(2), 0u);
    watermark.release(4);
    watermark.remove(2);
    EXPECT_TRUE(watermark.can_upload(3));
}

TEST(NvencWatermarkTest, SharedAlphaAndCancellation)
{
    upload_watermark watermark(12);
    watermark.admit(1, 8); // Color and alpha share this session.
    watermark.admit(2, 4);
    watermark.upload(2); // A later image may upload first while capacity is available.
    watermark.upload(2);
    watermark.upload(2);
    watermark.upload(2);
    EXPECT_TRUE(watermark.can_admit(9)); // Registration does not reserve buffers.
    EXPECT_FALSE(watermark.can_admit(13));
    EXPECT_TRUE(watermark.can_upload(1));
    watermark.release(4);
    watermark.remove(2);
    for (int i = 0; i < 8; ++i) {
        watermark.upload(1);
    }
    watermark.release(8);
    watermark.remove(1);
    EXPECT_EQ(watermark.used(), 0u);
    EXPECT_TRUE(watermark.can_admit(12));
}

TEST(NvencWatermarkTest, ReadyJobsCanFillTheSessionAndReleaseCapacity)
{
    upload_watermark watermark(4);
    watermark.admit(1, 2);
    watermark.admit(2, 2);
    watermark.upload(2);
    watermark.upload(2); // Ready order differs from arrival order.
    watermark.upload(1);
    watermark.upload(1);
    EXPECT_EQ(watermark.used(), 4u);
    EXPECT_TRUE(watermark.can_admit(2)); // Upload, rather than registration, waits.
    watermark.release(2);
    watermark.remove(2);
    EXPECT_TRUE(watermark.can_admit(2));
}

session_key MakeKey(uint32_t width, uint32_t height, uint32_t depth = 8, NV_ENC_BUFFER_FORMAT format = NV_ENC_BUFFER_FORMAT_NV12)
{
    session_key key;
    key.gpu = 0;
    key.max_encode_width = width;
    key.max_encode_height = height;
    key.depth = depth;
    key.buffer_format = format;
    return key;
}

class SchedulerHarness
{
public:
    explicit SchedulerHarness(size_t max_sessions) : scheduler_(max_sessions, /*frame_capacity=*/32) {}

    struct ServedRequest
    {
        scheduler_decision decision;
        uint64_t session_id = 0;
    };

    ServedRequest Serve(const session_key & key, bool release = true, bool commit = true)
    {
        ServedRequest served;
        served.decision = scheduler_.plan_acquire(key);
        switch (served.decision.next_action) {
            case scheduler_decision::action::create:
                if (served.decision.evict_session_id.has_value()) {
                    scheduler_.remove_session(*served.decision.evict_session_id);
                }
                served.session_id = next_session_id_++;
                scheduler_.add_session(served.session_id, key);
                if (commit) {
                    scheduler_.commit_reservation(served.decision.request_epoch, key, served.session_id);
                    if (release) {
                        scheduler_.release_reservation(served.session_id);
                    }
                }
                break;
            case scheduler_decision::action::reuse:
                served.session_id = *served.decision.session_id;
                if (commit) {
                    scheduler_.commit_reservation(served.decision.request_epoch, key, served.session_id);
                    if (release) {
                        scheduler_.release_reservation(served.session_id);
                    }
                }
                break;
            case scheduler_decision::action::wait:
                break;
        }
        return served;
    }

    scheduler_decision Plan(const session_key & key) { return scheduler_.plan_acquire(key); }

    void RemoveSession(uint64_t session_id) { scheduler_.remove_session(session_id); }

    void SetActiveReservations(uint64_t session_id, size_t active_reservations)
    {
        scheduler_.set_active_reservations(session_id, active_reservations);
    }

    bool HasKind(const session_key & key) const { return scheduler_.has_kind(key); }

    size_t LiveSessions(const session_key & key) const { return scheduler_.live_sessions_for(key); }

    size_t Targets(const session_key & key) const { return scheduler_.target_sessions_for(key); }

    size_t ActiveReservations(uint64_t session_id) const { return scheduler_.active_reservations_for(session_id); }

private:
    pool_scheduler scheduler_;
    uint64_t next_session_id_ = 1;
};

TEST(NvencSchedulerGroupTest, IdenticalKeysAcquireTheSameSessionTwice)
{
    pool_scheduler scheduler(1, 32);
    const auto key = MakeKey(1024, 1024);
    scheduler.add_session(1, key);
    const auto plan = scheduler.plan_acquire_group({ key, key });
    ASSERT_TRUE(plan.ready);
    ASSERT_EQ(plan.sessions.size(), 1u);
    EXPECT_EQ(plan.request_sessions, (std::vector<size_t> { 0, 0 }));
    EXPECT_EQ(plan.sessions[0].session_id, 1u);
    EXPECT_EQ(scheduler.active_reservations_for(1), 0u);
    scheduler.commit_reservations({ key, key }, { 1, 1 });
    EXPECT_EQ(scheduler.active_reservations_for(1), 2u);
    scheduler.release_reservation(1);
    EXPECT_EQ(scheduler.active_reservations_for(1), 1u);
}

TEST(NvencSchedulerGroupTest, ProtectsBothSelectedSessionsFromEviction)
{
    pool_scheduler scheduler(2, 32);
    const auto a = MakeKey(512, 512), b = MakeKey(1024, 1024), c = MakeKey(2048, 2048);
    scheduler.add_session(1, a);
    scheduler.add_session(2, b);
    const auto plan = scheduler.plan_acquire_group({ a, c });
    ASSERT_TRUE(plan.ready);
    ASSERT_EQ(plan.sessions.size(), 2u);
    EXPECT_EQ(plan.sessions[0].session_id, 1u);
    EXPECT_EQ(plan.sessions[1].next_action, scheduler_decision::action::create);
    EXPECT_EQ(plan.sessions[1].evict_session_id, 2u);
    // A later failed acquisition cannot change existing pins.
    scheduler.set_active_reservations(2, 1);
    EXPECT_FALSE(scheduler.plan_acquire_group({ a, c }).ready);
    EXPECT_EQ(scheduler.active_reservations_for(1), 0u);
    EXPECT_EQ(scheduler.active_reservations_for(2), 1u);
}

TEST(NvencSchedulerGroupTest, CommitsDistinctKeysTogetherAndValidatesBeforePinning)
{
    pool_scheduler scheduler(2, 32);
    const auto a = MakeKey(512, 512), b = MakeKey(1024, 1024);
    scheduler.add_session(1, a);
    scheduler.add_session(2, b);
    EXPECT_THROW(scheduler.commit_reservations({ a, b }, { 1, 1 }), std::logic_error);
    EXPECT_EQ(scheduler.active_reservations_for(1), 0u);
    EXPECT_EQ(scheduler.active_reservations_for(2), 0u);
    scheduler.commit_reservations({ a, b }, { 1, 2 });
    EXPECT_EQ(scheduler.active_reservations_for(1), 1u);
    EXPECT_EQ(scheduler.active_reservations_for(2), 1u);
}

TEST(NvencSchedulerGroupTest, InsufficientPoolLimitFailsWithoutPartialAcquisition)
{
    pool_scheduler scheduler(1, 32);
    const auto a = MakeKey(512, 512), b = MakeKey(1024, 1024);
    scheduler.add_session(1, a);
    EXPECT_THROW(scheduler.plan_acquire_group({ a, b }), std::invalid_argument);
    EXPECT_EQ(scheduler.active_reservations_for(1), 0u);
}

TEST(NvencSchedulerGroupTest, DuplicateKeyRequiresCapacityForBothPins)
{
    pool_scheduler scheduler(1, 32);
    const auto key = MakeKey(1024, 1024);
    scheduler.add_session(1, key);
    scheduler.set_active_reservations(1, 31);
    EXPECT_FALSE(scheduler.plan_acquire_group({ key, key }).ready);
    EXPECT_EQ(scheduler.active_reservations_for(1), 31u);
    scheduler.release_reservation(1);
    EXPECT_TRUE(scheduler.plan_acquire_group({ key, key }).ready);
    scheduler.commit_reservations({ key, key }, { 1, 1 });
    EXPECT_EQ(scheduler.active_reservations_for(1), 32u);
}

TEST(NvencSchedulerGroupTest, FailedSessionWaitsForPinsThenCanBeReplaced)
{
    pool_scheduler scheduler(1, 32);
    const auto key = MakeKey(1024, 1024);
    scheduler.add_session(1, key);
    scheduler.commit_reservations({ key }, { 1 });
    scheduler.mark_unusable(1);
    EXPECT_FALSE(scheduler.plan_acquire_group({ key }).ready);
    scheduler.release_reservation(1);
    const auto plan = scheduler.plan_acquire_group({ key });
    ASSERT_TRUE(plan.ready);
    EXPECT_EQ(plan.sessions[0].next_action, scheduler_decision::action::create);
    EXPECT_EQ(plan.sessions[0].evict_session_id, 1u);
}

TEST(NvencWatermarkTest, RegistersWithoutWaitingForFreeBuffers)
{
    upload_watermark watermark(4);
    watermark.admit(1, 4);
    for (int i = 0; i < 4; ++i) {
        watermark.upload(1);
    }
    watermark.admit(2, 4); // No buffer reservation or wait during registration.
    EXPECT_FALSE(watermark.can_upload(2));
    watermark.release(4);
    watermark.remove(1);
    EXPECT_TRUE(watermark.can_upload(2));
}

TEST(NvencSchedulerTest, OneHotKindWarmsUpOneSessionPerAcquire)
{
    SchedulerHarness harness(/*max_sessions=*/4);
    const session_key hot = MakeKey(640, 480);

    for (size_t expected_sessions = 1; expected_sessions <= 4; ++expected_sessions) {
        const SchedulerHarness::ServedRequest served = harness.Serve(hot);
        EXPECT_EQ(served.decision.next_action, scheduler_decision::action::create);
        EXPECT_EQ(harness.LiveSessions(hot), expected_sessions);
    }

    const SchedulerHarness::ServedRequest reused = harness.Serve(hot);
    EXPECT_EQ(reused.decision.next_action, scheduler_decision::action::reuse);
    EXPECT_EQ(harness.LiveSessions(hot), 4u);
}

TEST(NvencSchedulerTest, ProportionalMixConvergesToThreeToOne)
{
    SchedulerHarness harness(/*max_sessions=*/4);
    const session_key key_a = MakeKey(640, 480);
    const session_key key_b = MakeKey(1280, 720);

    for (int i = 0; i < 8; ++i) {
        harness.Serve(key_a);
        harness.Serve(key_a);
        harness.Serve(key_a);
        harness.Serve(key_b);
    }

    (void)harness.Plan(key_a);
    EXPECT_EQ(harness.LiveSessions(key_a), 3u);
    EXPECT_EQ(harness.LiveSessions(key_b), 1u);
    EXPECT_EQ(harness.Targets(key_a), 3u);
    EXPECT_EQ(harness.Targets(key_b), 1u);
}

TEST(NvencSchedulerTest, DecayIsEventBased)
{
    SchedulerHarness harness(/*max_sessions=*/4);
    const session_key key_a = MakeKey(640, 480);
    const session_key key_b = MakeKey(1280, 720);

    for (int i = 0; i < 4; ++i) {
        harness.Serve(key_a);
    }

    (void)harness.Plan(key_a);
    EXPECT_EQ(harness.Targets(key_a), 4u);

    (void)harness.Plan(key_a);
    EXPECT_EQ(harness.Targets(key_a), 4u);

    harness.Serve(key_b);
    (void)harness.Plan(key_b);
    EXPECT_LT(harness.Targets(key_a), 4u);
    EXPECT_GE(harness.Targets(key_b), 1u);
}

TEST(NvencSchedulerTest, CurrentKindGetsAtLeastOneTarget)
{
    SchedulerHarness harness(/*max_sessions=*/4);
    const session_key hot = MakeKey(640, 480);
    const session_key cold = MakeKey(1280, 720);

    for (int i = 0; i < 4; ++i) {
        harness.Serve(hot);
    }

    const scheduler_decision decision = harness.Plan(cold);
    EXPECT_EQ(decision.next_action, scheduler_decision::action::create);
    EXPECT_EQ(harness.Targets(cold), 1u);
}

TEST(NvencSchedulerTest, RoundRobinReuseAcrossLiveSessions)
{
    SchedulerHarness harness(/*max_sessions=*/2);
    const session_key key = MakeKey(640, 480);

    const SchedulerHarness::ServedRequest first = harness.Serve(key);
    const SchedulerHarness::ServedRequest second = harness.Serve(key);

    const SchedulerHarness::ServedRequest third = harness.Serve(key);
    const SchedulerHarness::ServedRequest fourth = harness.Serve(key);

    EXPECT_EQ(third.decision.next_action, scheduler_decision::action::reuse);
    EXPECT_EQ(fourth.decision.next_action, scheduler_decision::action::reuse);
    EXPECT_EQ(third.session_id, first.session_id);
    EXPECT_EQ(fourth.session_id, second.session_id);
}

TEST(NvencSchedulerTest, RoundRobinSkipsFullSessions)
{
    SchedulerHarness harness(/*max_sessions=*/2);
    const session_key key = MakeKey(640, 480);

    const SchedulerHarness::ServedRequest first = harness.Serve(key);
    const SchedulerHarness::ServedRequest second = harness.Serve(key);
    harness.SetActiveReservations(first.session_id, 32);

    const scheduler_decision decision = harness.Plan(key);
    ASSERT_EQ(decision.next_action, scheduler_decision::action::reuse);
    EXPECT_EQ(*decision.session_id, second.session_id);
}

TEST(NvencSchedulerTest, EvictionPrefersIdleOverTargetVictims)
{
    SchedulerHarness harness(/*max_sessions=*/4);
    const session_key key_a = MakeKey(640, 480);
    const session_key key_b = MakeKey(1280, 720);
    const session_key key_c = MakeKey(1920, 1080);

    const SchedulerHarness::ServedRequest a1 = harness.Serve(key_a);
    harness.Serve(key_a);
    harness.Serve(key_a);
    harness.Serve(key_b);

    const scheduler_decision decision = harness.Plan(key_c);
    ASSERT_EQ(decision.next_action, scheduler_decision::action::create);
    ASSERT_TRUE(decision.evict_session_id.has_value());
    EXPECT_EQ(*decision.evict_session_id, a1.session_id);
}

TEST(NvencSchedulerTest, EvictionFallbackUsesColdestThenOldestIdle)
{
    SchedulerHarness harness(/*max_sessions=*/5);
    const session_key key_a = MakeKey(640, 480);
    const session_key key_b = MakeKey(1280, 720);
    const session_key key_c = MakeKey(1920, 1080);
    const session_key key_d = MakeKey(2560, 1440);

    const SchedulerHarness::ServedRequest a1 = harness.Serve(key_a);
    const SchedulerHarness::ServedRequest a2 = harness.Serve(key_a);
    const SchedulerHarness::ServedRequest a3 = harness.Serve(key_a);
    const SchedulerHarness::ServedRequest b1 = harness.Serve(key_b);
    harness.Serve(key_d);

    harness.SetActiveReservations(a1.session_id, 1);
    harness.SetActiveReservations(a2.session_id, 1);
    harness.SetActiveReservations(a3.session_id, 1);

    const scheduler_decision decision = harness.Plan(key_c);
    ASSERT_EQ(decision.next_action, scheduler_decision::action::create);
    ASSERT_TRUE(decision.evict_session_id.has_value());
    EXPECT_EQ(*decision.evict_session_id, b1.session_id);
}

TEST(NvencSchedulerTest, BusyAndCurrentKindSessionsAreNeverEvicted)
{
    {
        SchedulerHarness harness(/*max_sessions=*/4);
        const session_key key_a = MakeKey(640, 480);
        const session_key key_b = MakeKey(1280, 720);

        const SchedulerHarness::ServedRequest a1 = harness.Serve(key_a);
        const SchedulerHarness::ServedRequest a2 = harness.Serve(key_a);
        const SchedulerHarness::ServedRequest a3 = harness.Serve(key_a);
        const SchedulerHarness::ServedRequest b1 = harness.Serve(key_b);
        scheduler_decision decision = harness.Plan(key_b);
        for (int i = 0; (decision.next_action != scheduler_decision::action::create) && (i < 8); ++i) {
            harness.Serve(key_b);
            decision = harness.Plan(key_b);
        }

        ASSERT_EQ(decision.next_action, scheduler_decision::action::create);
        ASSERT_TRUE(decision.evict_session_id.has_value());
        EXPECT_NE(*decision.evict_session_id, b1.session_id);
        EXPECT_TRUE((*decision.evict_session_id == a1.session_id) || (*decision.evict_session_id == a2.session_id) ||
                    (*decision.evict_session_id == a3.session_id));
    }

    {
        SchedulerHarness harness(/*max_sessions=*/4);
        const session_key key_a = MakeKey(640, 480);
        const session_key key_b = MakeKey(1280, 720);

        const SchedulerHarness::ServedRequest a1 = harness.Serve(key_a);
        const SchedulerHarness::ServedRequest a2 = harness.Serve(key_a);
        const SchedulerHarness::ServedRequest a3 = harness.Serve(key_a);
        harness.Serve(key_b);
        scheduler_decision decision = harness.Plan(key_b);
        for (int i = 0; (decision.next_action != scheduler_decision::action::create) && (i < 8); ++i) {
            harness.Serve(key_b);
            decision = harness.Plan(key_b);
        }
        ASSERT_EQ(decision.next_action, scheduler_decision::action::create);
        harness.SetActiveReservations(a1.session_id, 1);
        harness.SetActiveReservations(a2.session_id, 1);
        harness.SetActiveReservations(a3.session_id, 1);

        decision = harness.Plan(key_b);
        EXPECT_EQ(decision.next_action, scheduler_decision::action::wait);
    }
}

TEST(NvencSchedulerTest, FifoAcquireDoesNotOvertake)
{
    acquire_ticket_gate gate;
    const uint64_t first = gate.issue_ticket();
    const uint64_t second = gate.issue_ticket();

    EXPECT_TRUE(gate.is_serving(first));
    EXPECT_FALSE(gate.is_serving(second));

    gate.complete_ticket();

    EXPECT_FALSE(gate.is_serving(first));
    EXPECT_TRUE(gate.is_serving(second));
}

TEST(NvencSchedulerTest, FailedCreateOrAcquireDoesNotSkewDemandOrLeakReservations)
{
    {
        SchedulerHarness harness(/*max_sessions=*/2);
        const session_key key = MakeKey(640, 480);

        const scheduler_decision first = harness.Plan(key);
        ASSERT_EQ(first.next_action, scheduler_decision::action::create);

        const scheduler_decision retry = harness.Plan(key);
        EXPECT_EQ(retry.next_action, scheduler_decision::action::create);
        EXPECT_EQ(harness.LiveSessions(key), 0u);
    }

    {
        SchedulerHarness harness(/*max_sessions=*/1);
        const session_key key = MakeKey(640, 480);
        const SchedulerHarness::ServedRequest created = harness.Serve(key);

        const scheduler_decision failed_reuse = harness.Plan(key);
        ASSERT_EQ(failed_reuse.next_action, scheduler_decision::action::reuse);
        EXPECT_EQ(harness.ActiveReservations(created.session_id), 0u);

        const scheduler_decision retry = harness.Plan(key);
        EXPECT_EQ(retry.next_action, scheduler_decision::action::reuse);
        EXPECT_EQ(harness.ActiveReservations(created.session_id), 0u);
    }
}

TEST(NvencSchedulerTest, PrunesColdUnusedKindsAfterDecay)
{
    SchedulerHarness harness(/*max_sessions=*/2);
    const session_key stale = MakeKey(640, 480);
    const session_key hot = MakeKey(1280, 720);

    const SchedulerHarness::ServedRequest stale_session = harness.Serve(stale);
    harness.RemoveSession(stale_session.session_id);
    EXPECT_TRUE(harness.HasKind(stale));

    for (int i = 0; i < 80; ++i) {
        harness.Serve(hot);
    }

    (void)harness.Plan(hot);
    EXPECT_FALSE(harness.HasKind(stale));
}

} // namespace
} // namespace avif_nvenc
