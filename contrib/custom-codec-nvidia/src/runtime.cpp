// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "runtime.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "../include/scheduler.h"
#include "avif_nvenc_codec.h"
#include "cuda_driver.h"
#include "exception.h"
#include "nvenc.h"
#include "session.h"

namespace avif_nvenc
{
namespace
{

struct cuda_context_holder
{
    explicit cuda_context_holder(int gpu_ordinal) : gpu_ordinal(gpu_ordinal)
    {
        cuda::cuInit(0);
        CUresult cu_result = cuda::cuDeviceGet(&device, gpu_ordinal);
        if (cu_result != CUDA_SUCCESS) {
            throw cuda_exception(cu_result);
        }
        cu_result = cuda::cuDevicePrimaryCtxRetain(&context, device);
        if (cu_result != CUDA_SUCCESS) {
            throw cuda_exception(cu_result);
        }
    }

    ~cuda_context_holder()
    {
        if (!context) {
            return;
        }
        try {
            cuda::cuDevicePrimaryCtxRelease(device);
        } catch (...) {
        }
    }

    int gpu_ordinal = 0;
    CUdevice device = 0;
    CUcontext context = nullptr;
};

struct api_bundle
{
    api_bundle()
    {
        uint32_t max_supported_version = 0;
        nvenc::NvEncodeAPIGetMaxSupportedVersion(&max_supported_version);
        constexpr uint32_t current_version = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (current_version > max_supported_version) {
            throw msg_exception("The installed NVIDIA driver does not support this NVENC API version");
        }

        functions = { NV_ENCODE_API_FUNCTION_LIST_VER };
        if (NVENCSTATUS status = nvenc::NvEncodeAPICreateInstance(&functions); status != NV_ENC_SUCCESS) {
            throw nvenc_exception(status);
        }
    }

    NV_ENCODE_API_FUNCTION_LIST functions {};
};

avifResult validate_pool_options(const registered_pool_options & options, avifDiagnostics * diag)
{
    if (options.id.empty()) {
        set_diagnostic(diag, "NVENC pool id must not be empty");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (options.gpu < 0) {
        set_diagnostic(diag, "Invalid NVENC gpu for pool '{}': {}", options.id, options.gpu);
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (options.max_sessions == 0) {
        set_diagnostic(diag, "Invalid NVENC maxSessions for pool '{}': {}", options.id, options.max_sessions);
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    switch (options.mode) {
        case pool_mode::on_demand:
            if (options.max_size || !options.bucket_sizes.empty()) {
                set_diagnostic(diag, "NVENC on-demand pool '{}' does not accept maxSize or bucketSizes", options.id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            return AVIF_RESULT_OK;
        case pool_mode::omni:
            if (!options.max_size) {
                set_diagnostic(diag, "NVENC omni pool '{}' requires maxSize", options.id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            if (!options.bucket_sizes.empty()) {
                set_diagnostic(diag, "NVENC omni pool '{}' does not accept bucketSizes", options.id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            return AVIF_RESULT_OK;
        case pool_mode::bucket:
            if (options.bucket_sizes.empty()) {
                set_diagnostic(diag, "NVENC bucket pool '{}' requires bucketSizes", options.id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            if (options.max_size) {
                set_diagnostic(diag, "NVENC bucket pool '{}' does not accept maxSize", options.id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            return AVIF_RESULT_OK;
    }

    set_diagnostic(diag, "Invalid NVENC pool mode for '{}'", options.id);
    return AVIF_RESULT_INVALID_ARGUMENT;
}

image_size resolve_session_size(const registered_pool_options & options,
                                uint32_t encode_width,
                                uint32_t encode_height,
                                avifDiagnostics * diag,
                                avifResult * result)
{
    *result = AVIF_RESULT_OK;
    if (options.mode == pool_mode::on_demand) {
        return { encode_width, encode_height };
    }
    if (options.mode == pool_mode::omni) {
        if (!options.max_size || options.max_size->width < encode_width || options.max_size->height < encode_height) {
            set_diagnostic(diag, "NVENC pool '{}' maxSize cannot serve {}x{}", options.id, encode_width, encode_height);
            *result = AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            return {};
        }
        return *options.max_size;
    }

    for (const image_size & bucket : options.bucket_sizes) {
        if (bucket.width >= encode_width && bucket.height >= encode_height) {
            return bucket;
        }
    }
    set_diagnostic(diag, "NVENC pool '{}' has no bucket that can serve {}x{}", options.id, encode_width, encode_height);
    *result = AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
    return {};
}

} // namespace

struct layered_group::impl
{
    std::mutex mutex;
    std::shared_ptr<nvenc_session::layered_job> jobs[2];
    uint32_t stream_indices[2] = {};
    bool cancelled = false;
};

class encoder_pool : public std::enable_shared_from_this<encoder_pool>
{
public:
    encoder_pool(registered_pool_options options, std::shared_ptr<api_bundle> api_bundle)
        : options_(std::move(options))
        , context_(std::make_shared<cuda_context_holder>(options_.gpu))
        , api_bundle_(std::move(api_bundle))
        , scheduler_(options_.max_sessions, nvenc_session::kFrameCapacity)
    {
    }

    void begin_shutdown()
    {
        {
            std::scoped_lock lock(mutex_);
            shutting_down_ = true;
        }
        condition_.notify_all();
    }

    void shutdown()
    {
        std::vector<std::unique_ptr<session_record>> sessions;
        {
            std::unique_lock lock(mutex_);
            shutting_down_ = true;
            condition_.wait(lock, [this] { return active_reservations_ == 0; });
            session_id_lookup_.clear();
            session_lookup_.clear();
            scheduler_.clear();
            sessions = std::move(sessions_);
        }
        sessions.clear();
        context_.reset();
        api_bundle_.reset();
    }

    AVIF_NODISCARD avifResult acquire_single(const session_request & request, pool_frame * frame, avifDiagnostics * diag)
    {
        try {
            if (!frame) {
                set_diagnostic(diag, "Missing NVENC pool frame handle");
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            frame->reset();

            avifResult size_result = AVIF_RESULT_OK;
            const image_size session_size =
                resolve_session_size(options_, request.config.encode_width, request.config.encode_height, diag, &size_result);
            if (size_result != AVIF_RESULT_OK) {
                return size_result;
            }

            session_key key;
            key.gpu = options_.gpu;
            key.max_encode_width = session_size.width;
            key.max_encode_height = session_size.height;
            key.depth = request.depth;
            key.buffer_format = request.buffer_format;

            std::unique_lock lock(mutex_);
            const uint64_t acquire_ticket = acquire_tickets_.issue_ticket();

            struct acquire_ticket_guard
            {
                explicit acquire_ticket_guard(encoder_pool * pool) : pool(pool) {}

                ~acquire_ticket_guard()
                {
                    pool->acquire_tickets_.complete_ticket();
                    pool->condition_.notify_all();
                }

                encoder_pool * pool = nullptr;
            } ticket_guard(this);

            for (;;) {
                condition_.wait(lock,
                                [this, acquire_ticket] { return shutting_down_ || acquire_tickets_.is_serving(acquire_ticket); });

                if (shutting_down_) {
                    set_diagnostic(diag, "NVENC pool '{}' is shutting down", options_.id);
                    return AVIF_RESULT_UNKNOWN_ERROR;
                }

                const scheduler_decision decision = scheduler_.plan_acquire(key);
                if (decision.next_action == scheduler_decision::action::create) {
                    std::unique_ptr<session_record> evicted;
                    if (decision.evict_session_id.has_value()) {
                        const auto evicted_it = session_id_lookup_.find(*decision.evict_session_id);
                        if (evicted_it == session_id_lookup_.end()) {
                            set_diagnostic(diag, "NVENC scheduler selected an unknown eviction victim");
                            return AVIF_RESULT_UNKNOWN_ERROR;
                        }
                        evicted = detach_record_locked(evicted_it->second);
                    }
                    return create_record_locked(key, request.config, decision.request_epoch, frame, diag, std::move(evicted));
                }

                if (decision.next_action == scheduler_decision::action::reuse) {
                    const auto reusable_it = session_id_lookup_.find(*decision.session_id);
                    if (reusable_it == session_id_lookup_.end()) {
                        set_diagnostic(diag, "NVENC scheduler selected an unknown reusable session");
                        return AVIF_RESULT_UNKNOWN_ERROR;
                    }
                    return reserve_record_locked(reusable_it->second, key, decision.request_epoch, frame, diag);
                }

                condition_.wait(lock);
            }
        } catch (const std::bad_alloc &) {
            set_diagnostic(diag, "Out of memory acquiring NVENC frame");
            return AVIF_RESULT_OUT_OF_MEMORY;
        } catch (const std::exception & e) {
            set_diagnostic(diag, "{}", e.what());
            return AVIF_RESULT_UNKNOWN_ERROR;
        }
    }

    AVIF_NODISCARD avifResult acquire_layered(const session_request & color,
                                              const session_request * alpha,
                                              uint32_t layers,
                                              image_size final_size,
                                              std::shared_ptr<layered_group> * output,
                                              avifDiagnostics * diag)
    {
        // Transfer pins to completion callbacks only after registration succeeds.
        struct reservation_guard
        {
            encoder_pool * pool {};
            nvenc_session * pending[2] = {};

            ~reservation_guard()
            {
                for (auto * session : pending) {
                    if (session) {
                        pool->release_reservation(session, 1, !session->is_usable());
                    }
                }
            }
        } reservations { this };

        try {
            if (!output || layers < 2 || layers > 4) {
                set_diagnostic(diag, "Invalid layered NVENC acquisition");
                return AVIF_RESULT_INVALID_ARGUMENT;
            }

            avifResult result;
            const auto size = resolve_session_size(options_, final_size.width, final_size.height, diag, &result);
            if (result != AVIF_RESULT_OK) {
                return result;
            }

            std::vector<session_key> keys;
            const session_request * requests[2] = { &color, alpha };
            const size_t role_count = alpha ? 2 : 1;
            for (size_t i = 0; i < role_count; ++i) {
                session_key key;
                key.gpu = options_.gpu;
                key.max_encode_width = size.width;
                key.max_encode_height = size.height;
                key.depth = requests[i]->depth;
                key.buffer_format = requests[i]->buffer_format;
                key.reference_sequence = true;
                keys.push_back(key);
            }
            if (role_count == 2 && keys[0] != keys[1] && options_.max_sessions < 2) {
                set_diagnostic(diag,
                               "NVENC pool '{}' needs 2 distinct sessions for layered color/alpha, but maxSessions is {}",
                               options_.id,
                               options_.max_sessions);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }

            auto group = std::make_shared<layered_group>(std::make_unique<layered_group::impl>());
            scheduler_group_decision plan;
            std::vector<session_record *> selected;
            {
                std::vector<uint64_t> ids(role_count);
                std::unique_lock lock(mutex_);
                for (;;) {
                    if (shutting_down_) {
                        set_diagnostic(diag, "NVENC pool '{}' is shutting down", options_.id);
                        return AVIF_RESULT_UNKNOWN_ERROR;
                    }
                    plan = scheduler_.plan_acquire_group(keys);
                    if (plan.ready) {
                        break;
                    }
                    condition_.wait(lock);
                }

                selected.resize(plan.sessions.size());
                for (size_t i = 0; i < plan.sessions.size(); ++i) {
                    const auto & decision = plan.sessions[i];
                    if (decision.next_action == scheduler_decision::action::reuse) {
                        selected[i] = session_id_lookup_.at(*decision.session_id);
                        continue;
                    }
                    if (decision.evict_session_id) {
                        auto evicted = detach_record_locked(session_id_lookup_.at(*decision.evict_session_id));
                        evicted.reset();
                    }
                    size_t role = 0;
                    while (plan.request_sessions[role] != i) {
                        ++role;
                    }
                    auto record = std::make_unique<session_record>();
                    record->key = keys[role];
                    record->scheduler_session_id = next_session_id_++;
                    record->session =
                        std::make_unique<nvenc_session>(record->key, requests[role]->config, context_->context, api_bundle_->functions);
                    auto * raw = record.get();
                    const auto id = raw->scheduler_session_id;
                    scheduler_.add_session(id, raw->key);
                    try {
                        publish_record_locked(std::move(record));
                    } catch (...) {
                        scheduler_.remove_session(id);
                        throw;
                    }
                    selected[i] = raw;
                }
                for (size_t role = 0; role < role_count; ++role) {
                    ids[role] = selected[plan.request_sessions[role]]->scheduler_session_id;
                }

                scheduler_.commit_reservations(keys, ids);
                active_reservations_ += role_count;
                for (size_t role = 0; role < role_count; ++role) {
                    reservations.pending[role] = selected[plan.request_sessions[role]]->session.get();
                }
            }

            for (size_t i = 0; i < selected.size(); ++i) {
                auto * session = selected[i]->session.get();
                const auto count = static_cast<size_t>(std::count(plan.request_sessions.begin(), plan.request_sessions.end(), i));
                std::weak_ptr<layered_group> weak = group;
                auto completion = [pool = shared_from_this(), session, count, weak](avifResult status) noexcept {
                    if (status != AVIF_RESULT_OK) {
                        if (auto owner = weak.lock()) {
                            owner->cancel();
                        }
                    }
                    const bool unusable = !session->is_usable();
                    pool->release_reservation(session, count, unusable);
                };
                std::shared_ptr<nvenc_session::layered_job> job;
                result = session->create_layered_job(layers, uint32_t(count), std::move(completion), &job, diag);
                if (result != AVIF_RESULT_OK) {
                    group->cancel();
                    return result;
                }
                for (auto & pending : reservations.pending) {
                    if (pending == session) {
                        pending = nullptr;
                    }
                }
                bool cancelled;
                {
                    std::scoped_lock lock(group->state_->mutex);
                    uint32_t stream = 0;
                    for (size_t role = 0; role < role_count; ++role) {
                        if (plan.request_sessions[role] == i) {
                            group->state_->jobs[role] = job;
                            group->state_->stream_indices[role] = stream++;
                        }
                    }
                    cancelled = group->state_->cancelled;
                }
                if (cancelled) {
                    job->cancel();
                }
            }

            *output = std::move(group);
            return AVIF_RESULT_OK;
        } catch (const std::bad_alloc &) {
            condition_.notify_all();
            set_diagnostic(diag, "Out of memory acquiring layered NVENC sessions");
            return AVIF_RESULT_OUT_OF_MEMORY;
        } catch (const std::exception & e) {
            condition_.notify_all();
            set_diagnostic(diag, "{}", e.what());
            return AVIF_RESULT_UNKNOWN_ERROR;
        }
    }

    void release_reservation(nvenc_session * session, size_t count = 1, bool unusable = false) noexcept
    {
        {
            std::scoped_lock lock(mutex_);
            const auto it = session_lookup_.find(session);
            if (it != session_lookup_.end()) {
                if (unusable) {
                    scheduler_.mark_unusable(it->second->scheduler_session_id);
                }
                for (size_t i = 0; i < count; ++i) {
                    scheduler_.release_reservation(it->second->scheduler_session_id);
                    --active_reservations_;
                }
            }
        }
        condition_.notify_all();
    }

private:
    struct session_record
    {
        session_key key;
        uint64_t scheduler_session_id = 0;
        std::unique_ptr<nvenc_session> session;
    };

    void publish_record_locked(std::unique_ptr<session_record> record)
    {
        session_record * const raw_record = record.get();
        sessions_.push_back(std::move(record));
        try {
            session_id_lookup_.emplace(raw_record->scheduler_session_id, raw_record);
            session_lookup_.emplace(raw_record->session.get(), raw_record);
        } catch (...) {
            session_id_lookup_.erase(raw_record->scheduler_session_id);
            sessions_.pop_back();
            throw;
        }
    }

    std::unique_ptr<session_record> detach_record_locked(session_record * record)
    {
        if (!record) {
            return nullptr;
        }

        scheduler_.remove_session(record->scheduler_session_id);
        session_id_lookup_.erase(record->scheduler_session_id);
        session_lookup_.erase(record->session.get());
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->get() != record) {
                continue;
            }
            auto detached = std::move(*it);
            sessions_.erase(it);
            return detached;
        }
        return nullptr;
    }

    AVIF_NODISCARD avifResult reserve_record_locked(session_record * record,
                                                    const session_key & request_key,
                                                    uint64_t request_epoch,
                                                    pool_frame * frame,
                                                    avifDiagnostics * diag)
    {
        nvenc_session::frame_buffer reserved_frame;
        const avifResult result = record->session->acquire_frame(&reserved_frame, diag);
        if (result != AVIF_RESULT_OK) {
            condition_.notify_all();
            return result;
        }

        ++active_reservations_;
        scheduler_.commit_reservation(request_epoch, request_key, record->scheduler_session_id);
        frame->attach(this, record->session.get(), std::move(reserved_frame));
        return AVIF_RESULT_OK;
    }

    AVIF_NODISCARD avifResult create_record_locked(const session_key & key,
                                                   const session_config & config,
                                                   uint64_t request_epoch,
                                                   pool_frame * frame,
                                                   avifDiagnostics * diag,
                                                   std::unique_ptr<session_record> evicted = nullptr)
    {
        evicted.reset();

        auto record = std::make_unique<session_record>();
        record->key = key;
        record->scheduler_session_id = next_session_id_++;
        record->session = std::make_unique<nvenc_session>(key, config, context_->context, api_bundle_->functions);

        nvenc_session::frame_buffer reserved_frame;
        const avifResult result = record->session->acquire_frame(&reserved_frame, diag);
        if (result != AVIF_RESULT_OK) {
            return result;
        }

        session_record * const raw_record = record.get();
        const auto session_id = raw_record->scheduler_session_id;
        scheduler_.add_session(session_id, key);
        try {
            publish_record_locked(std::move(record));
        } catch (...) {
            scheduler_.remove_session(session_id);
            throw;
        }
        ++active_reservations_;
        scheduler_.commit_reservation(request_epoch, key, raw_record->scheduler_session_id);
        frame->attach(this, raw_record->session.get(), std::move(reserved_frame));
        return AVIF_RESULT_OK;
    }

    registered_pool_options options_;
    std::shared_ptr<cuda_context_holder> context_;
    std::shared_ptr<api_bundle> api_bundle_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::unique_ptr<session_record>> sessions_;
    std::map<uint64_t, session_record *> session_id_lookup_;
    std::map<nvenc_session *, session_record *> session_lookup_;
    pool_scheduler scheduler_;
    size_t active_reservations_ = 0;
    uint64_t next_session_id_ = 1;
    acquire_ticket_gate acquire_tickets_;
    bool shutting_down_ = false;
};

namespace
{

struct registry_pool_entry
{
    std::shared_ptr<encoder_pool> pool;
    size_t active_calls = 0;
    bool shutting_down = false;
};

struct registry_state
{
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<api_bundle> api;
    std::map<std::string, registry_pool_entry> pools;

    ~registry_state() noexcept(false)
    {
        // This destructor is only called upon library exit.
        // At that time CUDA driver and NVENC libraries are already unloaded,
        // so destructing pool now will result in weird crash
        // when calling these libraries' shutdown functions.

        // Instead, throw explicitly here and let runtime terminate with a clear error message.
        if (!pools.empty()) {
            throw msg_exception("There are still active NVENC pools. avifNvencShutdownPool() must be called with all pools before exiting.");
        }
    }
};

registry_state & shared_registry_state()
{
    static auto * state = new registry_state();
    return *state;
}

} // namespace

// -----------------------------------------------------------------------------
// Pool management

avifResult create_pool(const registered_pool_options & options, avifDiagnostics * diag)
{
    try {
        const avifResult validate_result = validate_pool_options(options, diag);
        if (validate_result != AVIF_RESULT_OK) {
            return validate_result;
        }

        registry_state & state = shared_registry_state();
        std::scoped_lock lock(state.mutex);
        if (state.pools.find(options.id) != state.pools.end()) {
            set_diagnostic(diag, "NVENC pool '{}' already exists", options.id);
            return AVIF_RESULT_INVALID_ARGUMENT;
        }
        if (!state.api) {
            state.api = std::make_shared<api_bundle>();
        }
        state.pools.emplace(options.id, registry_pool_entry { std::make_shared<encoder_pool>(options, state.api) });
        return AVIF_RESULT_OK;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diag, "Out of memory creating NVENC pool");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult shutdown_pool(std::string_view pool_id, avifDiagnostics * diag)
{
    try {
        if (pool_id.empty()) {
            set_diagnostic(diag, "NVENC pool id must not be empty");
            return AVIF_RESULT_INVALID_ARGUMENT;
        }

        encoder_pool * pool = nullptr;
        registry_pool_entry * entry = nullptr;
        const std::string id(pool_id);
        registry_state & state = shared_registry_state();
        {
            std::unique_lock lock(state.mutex);
            auto pool_it = state.pools.find(id);
            if (pool_it == state.pools.end()) {
                set_diagnostic(diag, "Unknown NVENC pool '{}'", pool_id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            entry = &pool_it->second;
            if (entry->shutting_down) {
                set_diagnostic(diag, "NVENC pool '{}' is already shutting down", pool_id);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            entry->shutting_down = true;
            pool = entry->pool.get();
        }

        pool->begin_shutdown();

        {
            std::unique_lock lock(state.mutex);
            state.condition.wait(lock, [entry] { return entry->active_calls == 0; });
        }

        pool->shutdown();

        {
            std::scoped_lock lock(state.mutex);
            state.pools.erase(id);
        }
        return AVIF_RESULT_OK;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diag, "Out of memory shutting down NVENC pool");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

// -----------------------------------------------------------------------------
// Single image encoding

pool_frame::pool_frame(pool_frame && other) noexcept
    : pool_(other.pool_), session_(other.session_), frame_(std::move(other.frame_))
{
    other.pool_ = nullptr;
    other.session_ = nullptr;
}

pool_frame & pool_frame::operator=(pool_frame && other) noexcept
{
    if (this == &other) {
        return *this;
    }
    reset();
    pool_ = other.pool_;
    session_ = other.session_;
    frame_ = std::move(other.frame_);
    other.pool_ = nullptr;
    other.session_ = nullptr;
    return *this;
}

pool_frame::~pool_frame()
{
    reset();
}

pool_frame::operator bool() const noexcept
{
    return session_ != nullptr;
}

avifResult pool_frame::upload_image(const session_config & config, const avifImage * image, avifDiagnostics * diag)
{
    if (!session_) {
        set_diagnostic(diag, "NVENC frame is not reserved");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    return session_->upload_image(&frame_, config, image, diag);
}

avifResult pool_frame::submit_frame(const session_config & config, nvenc_session::encoded_frame * encoded, avifDiagnostics * diag)
{
    if (!session_) {
        set_diagnostic(diag, "NVENC frame is not reserved");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    const avifResult result = session_->submit_frame(&frame_, config, encoded, diag);
    encoder_pool * pool = pool_;
    nvenc_session * session = session_;
    pool_ = nullptr;
    session_ = nullptr;
    {
        nvenc_session::frame_buffer frame(std::move(frame_));
    }
    pool->release_reservation(session);
    return result;
}

void pool_frame::reset()
{
    if (!session_ || !pool_) {
        return;
    }

    encoder_pool * pool = pool_;
    nvenc_session * session = session_;
    pool_ = nullptr;
    session_ = nullptr;
    {
        nvenc_session::frame_buffer frame(std::move(frame_));
    }
    pool->release_reservation(session);
}

void pool_frame::attach(encoder_pool * pool, nvenc_session * session, nvenc_session::frame_buffer frame) noexcept
{
    pool_ = pool;
    session_ = session;
    frame_ = std::move(frame);
}

avifResult acquire_pool_frame(const session_request & request, pool_frame * frame, avifDiagnostics * diag)
{
    registry_state & state = shared_registry_state();
    registry_pool_entry * entry = nullptr;
    encoder_pool * pool = nullptr;

    try {
        {
            std::unique_lock lock(state.mutex);
            if (!request.pool_id.empty()) {
                auto pool_it = state.pools.find(request.pool_id);
                if (pool_it == state.pools.end()) {
                    set_diagnostic(diag, "Unknown NVENC pool '{}'", request.pool_id);
                    return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
                }
                entry = &pool_it->second;
            } else if (state.pools.empty()) {
                set_diagnostic(diag, "No NVENC pool is registered; call avifNvencCreatePool() before encoding");
                return AVIF_RESULT_NO_CODEC_AVAILABLE;
            } else if (state.pools.size() == 1) {
                entry = &state.pools.begin()->second;
            } else {
                set_diagnostic(diag, "Multiple NVENC pools are registered; set codec option '{}' to select one", AVIF_NVENC_OPTION_POOL);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }

            if (entry->shutting_down) {
                set_diagnostic(diag,
                               "NVENC pool '{}' is shutting down",
                               request.pool_id.empty() ? state.pools.begin()->first : request.pool_id);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }

            ++entry->active_calls;
            pool = entry->pool.get();
        }

        const avifResult result = pool->acquire_single(request, frame, diag);

        {
            std::scoped_lock lock(state.mutex);
            --entry->active_calls;
        }
        state.condition.notify_all();
        return result;
    } catch (const std::bad_alloc &) {
        {
            std::scoped_lock lock(state.mutex);
            if (entry && entry->active_calls > 0) {
                --entry->active_calls;
            }
        }
        state.condition.notify_all();
        set_diagnostic(diag, "Out of memory acquiring NVENC frame");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        {
            std::scoped_lock lock(state.mutex);
            if (entry && entry->active_calls > 0) {
                --entry->active_calls;
            }
        }
        state.condition.notify_all();
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

// -----------------------------------------------------------------------------
// Layered image encoding

layered_group::layered_group(std::unique_ptr<impl> state) : state_(std::move(state))
{
}

layered_group::~layered_group()
{
    cancel();
}

avifResult layered_group::acquire_upload_image(bool alpha, const session_request & request, const avifImage * image, avifDiagnostics * diag)
{
    std::shared_ptr<nvenc_session::layered_job> job;
    uint32_t stream;
    {
        std::scoped_lock lock(state_->mutex);
        job = state_->jobs[alpha ? 1 : 0];
        stream = state_->stream_indices[alpha ? 1 : 0];
    }
    if (!job) {
        set_diagnostic(diag, "Unexpected NVENC layered stream");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    const auto result = job->acquire_upload_image(stream, request.config, image, diag, request.force_keyframe);
    if (result != AVIF_RESULT_OK) {
        cancel();
    }
    return result;
}

avifResult layered_group::submit_job(bool alpha, std::vector<nvenc_session::encoded_frame> * output, avifDiagnostics * diag)
{
    std::shared_ptr<nvenc_session::layered_job> job;
    uint32_t stream;
    {
        std::scoped_lock lock(state_->mutex);
        job = state_->jobs[alpha ? 1 : 0];
        stream = state_->stream_indices[alpha ? 1 : 0];
    }
    if (!job) {
        set_diagnostic(diag, "Unexpected NVENC layered stream");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    const auto result = job->submit_job(stream, output, diag);
    if (result != AVIF_RESULT_OK) {
        cancel();
    }
    return result;
}

void layered_group::cancel() noexcept
{
    std::shared_ptr<nvenc_session::layered_job> jobs[2];
    {
        std::scoped_lock lock(state_->mutex);
        state_->cancelled = true;
        jobs[0] = state_->jobs[0];
        jobs[1] = state_->jobs[1];
    }
    for (size_t i = 0; i < 2; ++i) {
        if (jobs[i] && (!i || jobs[i] != jobs[0])) {
            jobs[i]->cancel();
        }
    }
}

avifResult acquire_layered_group(const session_request & color,
                                 const session_request * alpha,
                                 uint32_t layer_count,
                                 image_size final_size,
                                 std::shared_ptr<layered_group> * group,
                                 avifDiagnostics * diag)
{
    auto & state = shared_registry_state();
    registry_pool_entry * entry = nullptr;
    std::shared_ptr<encoder_pool> pool;
    {
        std::scoped_lock lock(state.mutex);
        if (!color.pool_id.empty()) {
            auto it = state.pools.find(color.pool_id);
            if (it != state.pools.end()) {
                entry = &it->second;
            }
        } else if (state.pools.size() == 1) {
            entry = &state.pools.begin()->second;
        }
        if (!entry) {
            set_diagnostic(diag, "Select a registered NVENC pool for layered encoding");
            return state.pools.empty() ? AVIF_RESULT_NO_CODEC_AVAILABLE : AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
        }
        if (entry->shutting_down) {
            set_diagnostic(diag, "NVENC pool is shutting down");
            return AVIF_RESULT_UNKNOWN_ERROR;
        }
        ++entry->active_calls;
        pool = entry->pool;
    }
    const auto result = pool->acquire_layered(color, alpha, layer_count, final_size, group, diag);
    {
        std::scoped_lock lock(state.mutex);
        --entry->active_calls;
    }
    state.condition.notify_all();
    return result;
}

} // namespace avif_nvenc
