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

    NV_ENCODE_API_FUNCTION_LIST functions{};
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

class encoder_pool
{
public:
    encoder_pool(registered_pool_options options, std::shared_ptr<api_bundle> api_bundle)
        : options_(std::move(options)),
          context_(std::make_shared<cuda_context_holder>(options_.gpu)),
          api_bundle_(std::move(api_bundle)),
          scheduler_(options_.max_sessions, nvenc_session::kFrameCapacity)
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
    }

    AVIF_NODISCARD avifResult acquire(const session_request & request, pool_frame * frame, avifDiagnostics * diag)
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
                condition_.wait(lock, [this, acquire_ticket] {
                    return shutting_down_ || acquire_tickets_.is_serving(acquire_ticket);
                });

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

    void release_reservation(nvenc_session * session) noexcept
    {
        {
            std::scoped_lock lock(mutex_);
            if (session) {
                const auto record_it = session_lookup_.find(session);
                if (record_it != session_lookup_.end()) {
                    scheduler_.release_reservation(record_it->second->scheduler_session_id);
                    if (active_reservations_ > 0) {
                        --active_reservations_;
                    }
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
        scheduler_.add_session(raw_record->scheduler_session_id, key);
        try {
            publish_record_locked(std::move(record));
        } catch (...) {
            scheduler_.remove_session(raw_record->scheduler_session_id);
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
    std::unique_ptr<encoder_pool> pool;
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
        state.pools.emplace(options.id, registry_pool_entry { std::make_unique<encoder_pool>(options, state.api) });
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
                set_diagnostic(diag, "NVENC pool '{}' is shutting down", request.pool_id.empty() ? state.pools.begin()->first : request.pool_id);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }

            ++entry->active_calls;
            pool = entry->pool.get();
        }

        const avifResult result = pool->acquire(request, frame, diag);

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

} // namespace avif_nvenc
