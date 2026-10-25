// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H

#include <functional>
#include <memory>
#include <vector>

#include "codec_options.h"
#include "cuda_stub.h"
#include "nvEncodeAPI.h"

namespace avif_nvenc
{

class nvenc_session
{
    struct layered_state;

public:
    static constexpr size_t kFrameCapacity = 32;

    struct encoded_frame
    {
        std::vector<uint8_t> bytes;
        avifBool sync = AVIF_FALSE;
    };

    class frame_buffer
    {
    public:
        frame_buffer() = default;
        frame_buffer(const frame_buffer &) = delete;
        frame_buffer & operator=(const frame_buffer &) = delete;
        frame_buffer(frame_buffer && other) noexcept;
        frame_buffer & operator=(frame_buffer && other) noexcept;
        ~frame_buffer();

        explicit operator bool() const noexcept;

    private:
        friend class nvenc_session;

        explicit frame_buffer(nvenc_session * session, size_t slot_index) noexcept;
        void detach() noexcept;

        nvenc_session * session_ = nullptr;
        size_t slot_index_ = static_cast<size_t>(-1);
        uint32_t pitch_ = 0;
    };

    // One image's streams assigned to this physical session. A job contains
    // one stream, or two when color and alpha share this session. The watermark
    // counts their remaining uploads together; each stream becomes independently
    // eligible for encoding when all of its layers have uploaded.
    class layered_job
    {
    public:
        layered_job(const layered_job &) = delete;
        layered_job & operator=(const layered_job &) = delete;
        ~layered_job();

        // Upload immediately into session-owned storage, waiting for this
        // session's watermark when necessary. stream must be less than the registered stream_count.
        // The caller may reuse/destroy image after this returns. Calls for the
        // same stream must be serialized; different jobs may upload concurrently.
        AVIF_NODISCARD avifResult acquire_upload_image(uint32_t stream,
                                                       const session_config & config,
                                                       const avifImage * image,
                                                       avifDiagnostics * diag,
                                                       bool force_keyframe = false);

        // Wait for one stream and transfer its compressed results once. Results
        // survive release of the pool reservations and destruction of the session.
        AVIF_NODISCARD avifResult submit_job(uint32_t stream, std::vector<encoded_frame> * output, avifDiagnostics * diag);

        // Cancel both streams, wake blocked uploads/finish calls, and release
        // staged inputs. Submitted work drains before ownership is released.
        // Destruction cancels unfinished work; cancellation itself does not wait
        // for GPU work. Safe concurrently with upload/finish and idempotent.
        void cancel() noexcept;

    private:
        friend class nvenc_session;
        friend struct layered_state;
        struct impl;
        explicit layered_job(std::shared_ptr<impl> state);
        std::shared_ptr<impl> impl_;
    };

    // Runs exactly once per successfully registered job, after all of its input
    // buffers and queued/running work have drained (success or cancellation).
    // Called without session locks; it releases the pool pins represented by the
    // job and can notify the image owner to cancel a failed job's sibling on a
    // different session. Must not throw. No callback occurs if registration fails.
    using layered_completion = std::function<void(avifResult)>;

    nvenc_session(const session_key & key, const session_config & config, CUcontext cuda_context, const NV_ENCODE_API_FUNCTION_LIST & api_functions);
    ~nvenc_session();
    // Failed reference sessions remain alive for existing jobs, but are no
    // longer candidates for new pool acquisitions.
    bool is_usable() const noexcept;

    AVIF_NODISCARD avifResult acquire_frame(frame_buffer * frame, avifDiagnostics * diag);
    AVIF_NODISCARD avifResult upload_image(frame_buffer * frame,
                                           const session_config & config,
                                           const avifImage * image,
                                           avifDiagnostics * diag) const;
    AVIF_NODISCARD avifResult submit_frame(frame_buffer * frame,
                                           const session_config & config,
                                           encoded_frame * encoded,
                                           avifDiagnostics * diag,
                                           bool sequence_start = false,
                                           bool force_keyframe = false) const;

    // Requires a reference-sequence session already pinned by the pool's atomic
    // acquisition. layer_count is 2-4; stream_count is 1 or 2. The caller supplies
    // one release notification for all pins represented by this job (including
    // both acquisitions when color and alpha share the session).
    //
    // Registration establishes this job's arrival order on this session. It does
    // not reserve pixel buffers or wait for free slots; only upload waits on the
    // watermark. This permits registration on two sessions without holding a
    // partial buffer admission while waiting on the other session.
    //
    // The session owns watermark accounting, readiness ordering, and exclusive
    // reference-state access for each complete stream's consecutive layer batch.
    // A driver failure cancels every outstanding job on this session and rejects
    // new registration. The pool only handles session selection/lifetime.
    AVIF_NODISCARD avifResult create_layered_job(uint32_t layer_count,
                                                 uint32_t stream_count,
                                                 layered_completion on_complete,
                                                 std::shared_ptr<layered_job> * job,
                                                 avifDiagnostics * diag);

private:
    friend class frame_buffer;

    void release_frame(frame_buffer * frame) const noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
    std::shared_ptr<layered_state> layered_;
};

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H
