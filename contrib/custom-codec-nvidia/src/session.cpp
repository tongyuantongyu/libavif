// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "session.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {

#include <libyuv/convert.h>
#include <libyuv/planar_functions.h>
}

#include "exception.h"

namespace avif_nvenc
{
namespace
{

constexpr uint32_t kEncoderBufferCount = nvenc_session::kFrameCapacity;
#ifdef _WIN32
static_assert(kEncoderBufferCount + 1 <= MAXIMUM_WAIT_OBJECTS, "Too many encode buffers");
#endif
constexpr uint16_t kNeutralChroma10BitMsb = 512u << 6;
constexpr size_t kInvalidSlotIndex = static_cast<size_t>(-1);

bool guid_supported(const GUID & target, const GUID * values, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (compare_guid(target, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool input_format_supported(NV_ENC_BUFFER_FORMAT target, const NV_ENC_BUFFER_FORMAT * values, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (values[i] == target) {
            return true;
        }
    }
    return false;
}

bool buffer_format_is_yuv422(NV_ENC_BUFFER_FORMAT format)
{
    return format == NV_ENC_BUFFER_FORMAT_NV16 || format == NV_ENC_BUFFER_FORMAT_P210;
}

bool buffer_format_is_yuv444(NV_ENC_BUFFER_FORMAT format)
{
    return format == NV_ENC_BUFFER_FORMAT_YUV444 || format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
}

uint32_t av1_chroma_format_idc_for(NV_ENC_BUFFER_FORMAT format)
{
    if (buffer_format_is_yuv444(format)) {
        return 3;
    }
    if (buffer_format_is_yuv422(format)) {
        return 2;
    }
    return 1;
}

void copy_msb_plane_16(const uint8_t * source,
                       uint32_t source_row_bytes,
                       uint8_t * destination,
                       uint32_t destination_pitch,
                       uint32_t width,
                       uint32_t height)
{
    // libyuv 16-bit strides are expressed in uint16_t elements.
    libyuv::ConvertToMSBPlane_16(reinterpret_cast<const uint16_t *>(source),
                                 static_cast<int>(source_row_bytes / sizeof(uint16_t)),
                                 reinterpret_cast<uint16_t *>(destination),
                                 static_cast<int>(destination_pitch / sizeof(uint16_t)),
                                 static_cast<int>(width),
                                 static_cast<int>(height),
                                 10);
}

void copy_nv12(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    uint8_t * uv_dst = destination + static_cast<size_t>(pitch) * max_height;
    libyuv::I420ToNV12(image->yuvPlanes[AVIF_CHAN_Y],
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_Y]),
                       image->yuvPlanes[AVIF_CHAN_U],
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_U]),
                       image->yuvPlanes[AVIF_CHAN_V],
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_V]),
                       destination,
                       static_cast<int>(pitch),
                       uv_dst,
                       static_cast<int>(pitch),
                       static_cast<int>(image->width),
                       static_cast<int>(image->height));
}

void copy_nv16(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    uint8_t * uv_dst = destination + static_cast<size_t>(pitch) * max_height;
    libyuv::CopyPlane(image->yuvPlanes[AVIF_CHAN_Y],
                      static_cast<int>(image->yuvRowBytes[AVIF_CHAN_Y]),
                      destination,
                      static_cast<int>(pitch),
                      static_cast<int>(image->width),
                      static_cast<int>(image->height));
    libyuv::MergeUVPlane(image->yuvPlanes[AVIF_CHAN_U],
                         static_cast<int>(image->yuvRowBytes[AVIF_CHAN_U]),
                         image->yuvPlanes[AVIF_CHAN_V],
                         static_cast<int>(image->yuvRowBytes[AVIF_CHAN_V]),
                         uv_dst,
                         static_cast<int>(pitch),
                         static_cast<int>((image->width + 1) / 2),
                         static_cast<int>(image->height));
}

void copy_yuv444(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    const size_t plane_size = static_cast<size_t>(pitch) * max_height;
    uint8_t * u_dst = destination + plane_size;
    uint8_t * v_dst = u_dst + plane_size;
    libyuv::CopyPlane(image->yuvPlanes[AVIF_CHAN_Y],
                      static_cast<int>(image->yuvRowBytes[AVIF_CHAN_Y]),
                      destination,
                      static_cast<int>(pitch),
                      static_cast<int>(image->width),
                      static_cast<int>(image->height));
    libyuv::CopyPlane(image->yuvPlanes[AVIF_CHAN_U],
                      static_cast<int>(image->yuvRowBytes[AVIF_CHAN_U]),
                      u_dst,
                      static_cast<int>(pitch),
                      static_cast<int>(image->width),
                      static_cast<int>(image->height));
    libyuv::CopyPlane(image->yuvPlanes[AVIF_CHAN_V],
                      static_cast<int>(image->yuvRowBytes[AVIF_CHAN_V]),
                      v_dst,
                      static_cast<int>(pitch),
                      static_cast<int>(image->width),
                      static_cast<int>(image->height));
}

void copy_p010(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    const int stride_y = static_cast<int>(pitch / sizeof(uint16_t));
    auto * uv_dst = reinterpret_cast<uint16_t *>(destination + static_cast<size_t>(pitch) * max_height);
    libyuv::I010ToP010(reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_Y]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_Y]) / 2,
                       reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_U]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_U]) / 2,
                       reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_V]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_V]) / 2,
                       reinterpret_cast<uint16_t *>(destination),
                       stride_y,
                       uv_dst,
                       stride_y,
                       static_cast<int>(image->width),
                       static_cast<int>(image->height));
}

void copy_p210(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    const int stride_y = static_cast<int>(pitch / sizeof(uint16_t));
    auto * uv_dst = reinterpret_cast<uint16_t *>(destination + static_cast<size_t>(pitch) * max_height);
    libyuv::I210ToP210(reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_Y]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_Y]) / 2,
                       reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_U]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_U]) / 2,
                       reinterpret_cast<const uint16_t *>(image->yuvPlanes[AVIF_CHAN_V]),
                       static_cast<int>(image->yuvRowBytes[AVIF_CHAN_V]) / 2,
                       reinterpret_cast<uint16_t *>(destination),
                       stride_y,
                       uv_dst,
                       stride_y,
                       static_cast<int>(image->width),
                       static_cast<int>(image->height));
}

void copy_yuv444_10bit(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    const size_t plane_size = static_cast<size_t>(pitch) * max_height;
    uint8_t * u_dst = destination + plane_size;
    uint8_t * v_dst = u_dst + plane_size;
    copy_msb_plane_16(image->yuvPlanes[AVIF_CHAN_Y], image->yuvRowBytes[AVIF_CHAN_Y], destination, pitch, image->width, image->height);
    copy_msb_plane_16(image->yuvPlanes[AVIF_CHAN_U], image->yuvRowBytes[AVIF_CHAN_U], u_dst, pitch, image->width, image->height);
    copy_msb_plane_16(image->yuvPlanes[AVIF_CHAN_V], image->yuvRowBytes[AVIF_CHAN_V], v_dst, pitch, image->width, image->height);
}

void copy_alpha_nv12(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    uint8_t * uv_dst = destination + static_cast<size_t>(pitch) * max_height;
    libyuv::I400ToNV21(image->alphaPlane,
                       static_cast<int>(image->alphaRowBytes),
                       destination,
                       static_cast<int>(pitch),
                       uv_dst,
                       static_cast<int>(pitch),
                       static_cast<int>(image->width),
                       static_cast<int>(image->height));
}

void copy_alpha_p010(const avifImage * image, uint8_t * destination, uint32_t pitch, uint32_t max_height)
{
    copy_msb_plane_16(image->alphaPlane, image->alphaRowBytes, destination, pitch, image->width, image->height);

    uint8_t * uv_dst = destination + static_cast<size_t>(pitch) * max_height;
    const int chroma_width = static_cast<int>((image->width + 1) / 2);
    const int chroma_height = static_cast<int>((image->height + 1) / 2);
    constexpr uint32_t neutral_uv = static_cast<uint32_t>(kNeutralChroma10BitMsb) |
                                    (static_cast<uint32_t>(kNeutralChroma10BitMsb) << 16);
    libyuv::ARGBRect(uv_dst, static_cast<int>(pitch), 0, 0, chroma_width, chroma_height, neutral_uv);
}

avifBool picture_type_is_sync(NV_ENC_PIC_TYPE picture_type)
{
    return (picture_type == NV_ENC_PIC_TYPE_IDR || picture_type == NV_ENC_PIC_TYPE_I || picture_type == NV_ENC_PIC_TYPE_SWITCH)
               ? AVIF_TRUE
               : AVIF_FALSE;
}

[[nodiscard]] constexpr bool async_events_supported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

} // namespace

struct nvenc_session::impl
{
    enum class slot_state
    {
        available,
        reserved,
        submitted,
    };

    struct buffer_slot
    {
        NV_ENC_INPUT_PTR input = nullptr;
        NV_ENC_OUTPUT_PTR output = nullptr;
        void * completion_event = nullptr;
        slot_state state = slot_state::available;
    };

    struct request_result
    {
        avifResult result = AVIF_RESULT_OK;
        std::string error;
        encoded_frame encoded;
    };

    enum class request_stage
    {
        queued,
        submitting,
        picture_submitted,
        ready_to_lock,
        finished,
    };

    struct encode_request
    {
        size_t slot_index = kInvalidSlotIndex;
        uint32_t pitch = 0;
        session_config config;
        request_stage stage = request_stage::queued;
        request_result result;
        bool ready_notification_sent = false;
        std::promise<void> ready_promise;
    };

    impl(const session_key & key,
         const session_config & config,
         CUcontext cuda_context,
         const NV_ENCODE_API_FUNCTION_LIST & api_functions)
        : key(key), active_config(config), cuda_context(cuda_context), api(api_functions)
    {
        try {
            open_encoder();
            validate_capabilities();
            initialize_encoder(active_config);
            create_drain_wake_event();
            allocate_buffers();
            submitter = std::thread([this] { submit_loop(); });
            if constexpr (async_events_supported()) {
                drainer = std::thread([this] { drain_loop(); });
            }
        } catch (...) {
            destroy();
            throw;
        }
    }

    ~impl() { shutdown(); }

    [[nodiscard]] bool should_use_monochrome(const session_config & config) const
    {
        return config.is_alpha && monochrome_supported;
    }

    void check_status(const char * step, NVENCSTATUS status) const
    {
        if (status == NV_ENC_SUCCESS) {
            return;
        }

        std::string message = step;
        message += " failed";
        if (encoder && api.nvEncGetLastErrorString) {
            if (const char * last_error = api.nvEncGetLastErrorString(encoder); last_error && *last_error) {
                message += ": ";
                message += last_error;
            }
        }
        throw msg_exception(
            std::format("{} (status {}: {})", message, static_cast<int>(status), std::string(nvenc_exception(status).what())));
    }

    void shutdown() noexcept
    {
        {
            std::unique_lock lock(state_mutex);
            stop_requested = true;
        }
        signal_async_drain_event();
        submit_condition.notify_all();
        drain_condition.notify_all();
        slot_condition.notify_all();
        if (submitter.joinable()) {
            submitter.join();
        }
        if constexpr (async_events_supported()) {
            if (drainer.joinable()) {
                drainer.join();
            }
        }
        destroy();
    }

    void destroy() noexcept
    {
        destroy_drain_wake_event();
        for (auto & slot : slots) {
            unregister_completion_event(&slot);
            if (slot.input && api.nvEncDestroyInputBuffer) {
                api.nvEncDestroyInputBuffer(encoder, slot.input);
                slot.input = nullptr;
            }
            if (slot.output && api.nvEncDestroyBitstreamBuffer) {
                api.nvEncDestroyBitstreamBuffer(encoder, slot.output);
                slot.output = nullptr;
            }
        }
        if (encoder && api.nvEncDestroyEncoder) {
            api.nvEncDestroyEncoder(encoder);
            encoder = nullptr;
        }
    }

    void open_encoder()
    {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
        open_params.device = cuda_context;
        open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        open_params.apiVersion = NVENCAPI_VERSION;
        check_status("nvEncOpenEncodeSessionEx", api.nvEncOpenEncodeSessionEx(&open_params, &encoder));
    }

    [[nodiscard]] int capability(const NV_ENC_CAPS caps) const
    {
        NV_ENC_CAPS_PARAM caps_param = { NV_ENC_CAPS_PARAM_VER };
        caps_param.capsToQuery = caps;
        int value = 0;
        check_status("nvEncGetEncodeCaps", api.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_AV1_GUID, &caps_param, &value));
        return value;
    }

    void validate_capabilities()
    {
        std::array<GUID, 16> encode_guids {};
        uint32_t encode_guid_count = 0;
        check_status("nvEncGetEncodeGUIDs",
                     api.nvEncGetEncodeGUIDs(encoder, encode_guids.data(), static_cast<uint32_t>(encode_guids.size()), &encode_guid_count));
        if (!guid_supported(NV_ENC_CODEC_AV1_GUID, encode_guids.data(), encode_guid_count)) {
            throw msg_exception("The selected GPU does not expose NVENC AV1 encode support");
        }

        std::array<GUID, 16> profile_guids {};
        uint32_t profile_guid_count = 0;
        check_status("nvEncGetEncodeProfileGUIDs",
                     api.nvEncGetEncodeProfileGUIDs(encoder,
                                                    NV_ENC_CODEC_AV1_GUID,
                                                    profile_guids.data(),
                                                    static_cast<uint32_t>(profile_guids.size()),
                                                    &profile_guid_count));
        if (!guid_supported(NV_ENC_AV1_PROFILE_MAIN_GUID, profile_guids.data(), profile_guid_count)) {
            throw msg_exception("The selected GPU does not expose NVENC AV1 main profile support");
        }

        std::array<NV_ENC_BUFFER_FORMAT, 16> input_formats {};
        uint32_t input_format_count = 0;
        check_status("nvEncGetInputFormats",
                     api.nvEncGetInputFormats(encoder,
                                              NV_ENC_CODEC_AV1_GUID,
                                              input_formats.data(),
                                              static_cast<uint32_t>(input_formats.size()),
                                              &input_format_count));
        if (!input_format_supported(key.buffer_format, input_formats.data(), input_format_count)) {
            throw msg_exception("The selected GPU does not support the required NVENC AV1 input format");
        }
        if (buffer_format_is_yuv422(key.buffer_format) && capability(NV_ENC_CAPS_SUPPORT_YUV422_ENCODE) == 0) {
            throw msg_exception("The selected GPU does not support NVENC AV1 YUV422 encoding");
        }
        if (buffer_format_is_yuv444(key.buffer_format) && capability(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE) == 0) {
            throw msg_exception("The selected GPU does not support NVENC AV1 YUV444 encoding");
        }

        if (static_cast<uint32_t>(capability(NV_ENC_CAPS_WIDTH_MAX)) < key.max_encode_width ||
            static_cast<uint32_t>(capability(NV_ENC_CAPS_HEIGHT_MAX)) < key.max_encode_height) {
            throw msg_exception("The selected GPU cannot encode the requested image dimensions");
        }
        const auto min_width = static_cast<uint32_t>(capability(NV_ENC_CAPS_WIDTH_MIN));
        const auto min_height = static_cast<uint32_t>(capability(NV_ENC_CAPS_HEIGHT_MIN));
        if (key.max_encode_width < min_width || key.max_encode_height < min_height) {
            throw msg_exception(
                std::format("The selected GPU requires image dimensions at least {}x{} for NVENC AV1", min_width, min_height));
        }
        if (key.depth > 8 && capability(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) == 0) {
            throw msg_exception("The selected GPU does not support NVENC 10-bit AV1 encoding");
        }
        monochrome_supported = (capability(NV_ENC_CAPS_SUPPORT_MONOCHROME) != 0);
    }

    void build_encoder_state(const session_config & config, NV_ENC_CONFIG * config_out, NV_ENC_INITIALIZE_PARAMS * init_out) const
    {
        NV_ENC_PRESET_CONFIG preset_config = { NV_ENC_PRESET_CONFIG_VER, 0, { NV_ENC_CONFIG_VER } };
        check_status("nvEncGetEncodePresetConfigEx",
                     api.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_AV1_GUID, config.preset_guid, config.tuning, &preset_config));

        *config_out = preset_config.presetCfg;
        config_out->version = NV_ENC_CONFIG_VER;
        config_out->profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
        config_out->gopLength = 1;
        config_out->frameIntervalP = 1;
        config_out->monoChromeEncoding = should_use_monochrome(config) ? 1u : 0u;
        config_out->rcParams.version = NV_ENC_RC_PARAMS_VER;
        config_out->rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        config_out->rcParams.zeroReorderDelay = 1;
        config_out->rcParams.enableAQ = config.spatial_aq ? 1u : 0u;
        config_out->rcParams.enableLookahead = 0;
        config_out->rcParams.enableTemporalAQ = 0;
        config_out->rcParams.enableNonRefP = 0;
        config_out->rcParams.enableMinQP = 1;
        config_out->rcParams.enableMaxQP = 1;
        config_out->rcParams.aqStrength = config.spatial_aq ? config.aq_strength.value_or(8) : 0u;
        config_out->rcParams.minQP = { config.min_qp, config.min_qp, config.min_qp };
        config_out->rcParams.maxQP = { config.max_qp, config.max_qp, config.max_qp };
        const uint32_t qp = config.qp;
        config_out->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        config_out->rcParams.constQP = { qp, qp, qp };
        config_out->rcParams.averageBitRate = 0;
        config_out->rcParams.maxBitRate = 0;
        config_out->rcParams.targetQuality = 0;
        if (config.y_dc_qp_offset.has_value()) {
            config_out->rcParams.yDcQPIndexOffset = config.y_dc_qp_offset.value();
        }
        if (config.u_dc_qp_offset.has_value()) {
            config_out->rcParams.uDcQPIndexOffset = config.u_dc_qp_offset.value();
        }
        if (config.v_dc_qp_offset.has_value()) {
            config_out->rcParams.vDcQPIndexOffset = config.v_dc_qp_offset.value();
        }
        if (config.cb_qp_offset.has_value()) {
            config_out->rcParams.cbQPIndexOffset = config.cb_qp_offset.value();
        }
        if (config.cr_qp_offset.has_value()) {
            config_out->rcParams.crQPIndexOffset = config.cr_qp_offset.value();
        }

        NV_ENC_CONFIG_AV1 & av1_config = config_out->encodeCodecConfig.av1Config;
        if (config.min_part_size.has_value()) {
            av1_config.minPartSize = config.min_part_size.value();
        }
        if (config.max_part_size.has_value()) {
            av1_config.maxPartSize = config.max_part_size.value();
        }
        av1_config.outputAnnexBFormat = 0;
        av1_config.disableSeqHdr = 0;
        av1_config.repeatSeqHdr = 1;
        av1_config.chromaFormatIDC = av1_chroma_format_idc_for(key.buffer_format);
        av1_config.idrPeriod = 1;
        av1_config.numTileColumns = config.tile_columns;
        av1_config.numTileRows = config.tile_rows;
        av1_config.colorPrimaries = static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(config.color_primaries);
        av1_config.transferCharacteristics = static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(config.transfer_characteristics);
        av1_config.matrixCoefficients = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(config.matrix_coefficients);
        av1_config.colorRange = (config.yuv_range == AVIF_RANGE_FULL) ? 1u : 0u;
        av1_config.chromaSamplePosition = (config.chroma_sample_position == AVIF_CHROMA_SAMPLE_POSITION_VERTICAL)
                                              ? 1u
                                              : (config.chroma_sample_position == AVIF_CHROMA_SAMPLE_POSITION_COLOCATED ? 2u : 0u);
        av1_config.outputMaxCll = config.clli.has_value() ? 1u : 0u;
        av1_config.outputBitDepth = (key.depth > 8) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        av1_config.inputBitDepth = av1_config.outputBitDepth;

        *init_out = { NV_ENC_INITIALIZE_PARAMS_VER };
        init_out->encodeGUID = NV_ENC_CODEC_AV1_GUID;
        init_out->presetGUID = config.preset_guid;
        init_out->encodeWidth = config.encode_width;
        init_out->encodeHeight = config.encode_height;
        init_out->darWidth = config.encode_width;
        init_out->darHeight = config.encode_height;
        init_out->frameRateNum = 1;
        init_out->frameRateDen = 1;
        init_out->enablePTD = 1;
        init_out->reportSliceOffsets = 0;
        init_out->enableSubFrameWrite = 0;
        init_out->enableEncodeAsync = async_events_supported() ? 1u : 0u;
        init_out->splitEncodeMode = config.split_encode_mode;
        init_out->maxEncodeWidth = key.max_encode_width;
        init_out->maxEncodeHeight = key.max_encode_height;
        init_out->tuningInfo = config.tuning;
        init_out->encodeConfig = config_out;
    }

    void initialize_encoder(const session_config & config)
    {
        build_encoder_state(config, &encode_config, &initialize_params);
        check_status("nvEncInitializeEncoder", api.nvEncInitializeEncoder(encoder, &initialize_params));
    }

    void reconfigure_encoder(const session_config & config)
    {
        if (active_config == config) {
            return;
        }

        NV_ENC_CONFIG updated_encode_config = {};
        NV_ENC_INITIALIZE_PARAMS updated_initialize_params = {};
        build_encoder_state(config, &updated_encode_config, &updated_initialize_params);

        NV_ENC_RECONFIGURE_PARAMS reconfigure_params = { NV_ENC_RECONFIGURE_PARAMS_VER };
        reconfigure_params.reInitEncodeParams = updated_initialize_params;
        reconfigure_params.resetEncoder = 1;
        reconfigure_params.forceIDR = 1;

        check_status("nvEncReconfigureEncoder", api.nvEncReconfigureEncoder(encoder, &reconfigure_params));
        encode_config = updated_encode_config;
        initialize_params = updated_initialize_params;
        initialize_params.encodeConfig = &encode_config;
        active_config = config;
        frame_index = 0;
    }

    void destroy_drain_wake_event() noexcept
    {
        if constexpr (!async_events_supported()) {
            return;
        }
#ifdef _WIN32
        if (drain_wake_event) {
            CloseHandle(static_cast<HANDLE>(drain_wake_event));
            drain_wake_event = nullptr;
        }
#endif
    }

    void unregister_completion_event(buffer_slot * slot) const noexcept
    {
        if constexpr (!async_events_supported()) {
            (void)slot;
            return;
        }
#ifdef _WIN32
        if (slot && slot->completion_event) {
            if (encoder && api.nvEncUnregisterAsyncEvent) {
                NV_ENC_EVENT_PARAMS event_params = { NV_ENC_EVENT_PARAMS_VER };
                event_params.completionEvent = slot->completion_event;
                api.nvEncUnregisterAsyncEvent(encoder, &event_params);
            }
            CloseHandle(static_cast<HANDLE>(slot->completion_event));
            slot->completion_event = nullptr;
        }
#else
        (void)slot;
#endif
    }

    void register_completion_event(buffer_slot * slot) const
    {
        if constexpr (!async_events_supported()) {
            (void)slot;
            return;
        }
#ifdef _WIN32
        if (!api.nvEncRegisterAsyncEvent) {
            throw msg_exception("The loaded NVENC runtime does not expose async event registration");
        }
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle) {
            throw msg_exception(std::format("CreateEventA failed with error {}", GetLastError()));
        }
        slot->completion_event = event_handle;
        try {
            NV_ENC_EVENT_PARAMS event_params = { NV_ENC_EVENT_PARAMS_VER };
            event_params.completionEvent = slot->completion_event;
            check_status("nvEncRegisterAsyncEvent", api.nvEncRegisterAsyncEvent(encoder, &event_params));
        } catch (...) {
            CloseHandle(event_handle);
            slot->completion_event = nullptr;
            throw;
        }
#endif
    }

    void create_drain_wake_event()
    {
        if constexpr (!async_events_supported()) {
            return;
        }
#ifdef _WIN32
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle) {
            throw msg_exception(std::format("CreateEventA failed with error {}", GetLastError()));
        }
        drain_wake_event = event_handle;
#endif
    }

    void allocate_buffers()
    {
        slots.resize(kEncoderBufferCount);
        for (buffer_slot & slot : slots) {
            NV_ENC_CREATE_INPUT_BUFFER input_buffer = { NV_ENC_CREATE_INPUT_BUFFER_VER };
            input_buffer.width = key.max_encode_width;
            input_buffer.height = key.max_encode_height;
            input_buffer.bufferFmt = key.buffer_format;
            check_status("nvEncCreateInputBuffer", api.nvEncCreateInputBuffer(encoder, &input_buffer));
            slot.input = input_buffer.inputBuffer;

            NV_ENC_CREATE_BITSTREAM_BUFFER bitstream_buffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
            check_status("nvEncCreateBitstreamBuffer", api.nvEncCreateBitstreamBuffer(encoder, &bitstream_buffer));
            slot.output = bitstream_buffer.bitstreamBuffer;
            register_completion_event(&slot);
        }
    }

    [[nodiscard]] size_t find_available_slot_locked() const
    {
        if (slots.empty()) {
            return kInvalidSlotIndex;
        }
        for (size_t offset = 0; offset < slots.size(); ++offset) {
            const size_t index = (next_slot + offset) % slots.size();
            if (slots[index].state == slot_state::available) {
                return index;
            }
        }
        return kInvalidSlotIndex;
    }

    [[nodiscard]] size_t reserve_frame_slot()
    {
        std::unique_lock lock(state_mutex);
        slot_condition.wait(lock, [this] { return stop_requested || find_available_slot_locked() != kInvalidSlotIndex; });
        if (stop_requested) {
            throw msg_exception("NVENC session is shutting down");
        }
        const size_t index = find_available_slot_locked();
        if (index == kInvalidSlotIndex) {
            throw msg_exception("NVENC session did not expose a free frame slot");
        }
        slots[index].state = slot_state::reserved;
        next_slot = (index + 1) % slots.size();
        return index;
    }

    void release_slot(size_t index) noexcept
    {
        {
            std::unique_lock lock(state_mutex);
            if (index < slots.size()) {
                slots[index].state = slot_state::available;
            }
        }
        slot_condition.notify_one();
    }

    void upload_image_to_slot(size_t index, const session_config & config, const avifImage * image, uint32_t * pitch_out)
    {
        {
            std::unique_lock lock(state_mutex);
            if (index >= slots.size() || slots[index].state != slot_state::reserved) {
                throw msg_exception("NVENC frame slot is not reserved");
            }
        }

        const buffer_slot & slot = slots[index];
        NV_ENC_LOCK_INPUT_BUFFER locked = { NV_ENC_LOCK_INPUT_BUFFER_VER };
        locked.inputBuffer = slot.input;
        check_status("nvEncLockInputBuffer", api.nvEncLockInputBuffer(encoder, &locked));

        try {
            switch (key.buffer_format) {
                case NV_ENC_BUFFER_FORMAT_NV12:
                    if (config.is_alpha) {
                        copy_alpha_nv12(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    } else {
                        copy_nv12(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    }
                    break;
                case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
                    if (config.is_alpha) {
                        copy_alpha_p010(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    } else {
                        copy_p010(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    }
                    break;
                case NV_ENC_BUFFER_FORMAT_NV16:
                    copy_nv16(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    break;
                case NV_ENC_BUFFER_FORMAT_P210:
                    copy_p210(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    break;
                case NV_ENC_BUFFER_FORMAT_YUV444:
                    copy_yuv444(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    break;
                case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
                    copy_yuv444_10bit(image, static_cast<uint8_t *>(locked.bufferDataPtr), locked.pitch, key.max_encode_height);
                    break;
                default:
                    throw msg_exception("Unsupported NVENC AV1 input format requested");
            }
        } catch (...) {
            api.nvEncUnlockInputBuffer(encoder, slot.input);
            throw;
        }

        check_status("nvEncUnlockInputBuffer", api.nvEncUnlockInputBuffer(encoder, slot.input));
        *pitch_out = locked.pitch;
    }

    [[nodiscard]] encoded_frame lock_output_bitstream(const buffer_slot & slot) const
    {
        NV_ENC_LOCK_BITSTREAM locked = { NV_ENC_LOCK_BITSTREAM_VER };
        locked.outputBitstream = slot.output;
        locked.doNotWait = false;
        check_status("nvEncLockBitstream", api.nvEncLockBitstream(encoder, &locked));

        encoded_frame encoded;
        try {
            if (locked.bitstreamSizeInBytes > 0) {
                const auto * bytes = static_cast<const uint8_t *>(locked.bitstreamBufferPtr);
                encoded.bytes.assign(bytes, bytes + locked.bitstreamSizeInBytes);
                encoded.sync = picture_type_is_sync(locked.pictureType);
            }
        } catch (...) {
            api.nvEncUnlockBitstream(encoder, slot.output);
            throw;
        }

        check_status("nvEncUnlockBitstream", api.nvEncUnlockBitstream(encoder, slot.output));
        return encoded;
    }

    void submit_picture(encode_request * request)
    {
        if (!request) {
            throw msg_exception("Missing NVENC encode request");
        }

        reconfigure_encoder(request->config);

        buffer_slot & slot = slots[request->slot_index];
        CONTENT_LIGHT_LEVEL max_cll = {};
        if (request->config.clli.has_value()) {
            max_cll.maxContentLightLevel = request->config.clli->max_content_light_level;
            max_cll.maxPicAverageLightLevel = request->config.clli->max_pic_average_light_level;
        }
        NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
        pic_params.inputWidth = request->config.encode_width;
        pic_params.inputHeight = request->config.encode_height;
        pic_params.inputPitch = request->pitch;
        pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        pic_params.frameIdx = frame_index;
        pic_params.inputTimeStamp = frame_index;
        pic_params.inputDuration = 1;
        pic_params.inputBuffer = slot.input;
        pic_params.outputBitstream = slot.output;
        pic_params.bufferFmt = key.buffer_format;
        pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic_params.pictureType = NV_ENC_PIC_TYPE_IDR;
        pic_params.codecPicParams.av1PicParams.displayPOCSyntax = frame_index;
        pic_params.codecPicParams.av1PicParams.refPicFlag = 1;
        pic_params.codecPicParams.av1PicParams.pMaxCll = request->config.clli.has_value() ? &max_cll : nullptr;
        if constexpr (async_events_supported()) {
            pic_params.completionEvent = slot.completion_event;
        }

        ++frame_index;
        check_status("nvEncEncodePicture", api.nvEncEncodePicture(encoder, &pic_params));
    }

    void notify_request_ready(const std::shared_ptr<encode_request> & request)
    {
        bool should_notify = false;
        {
            std::unique_lock lock(state_mutex);
            if (request->stage != request_stage::finished && !request->ready_notification_sent) {
                request->ready_notification_sent = true;
                should_notify = true;
            }
        }
        if (should_notify) {
            request->ready_promise.set_value();
        }
    }

    void signal_async_drain_event() const noexcept
    {
        if constexpr (!async_events_supported()) {
            return;
        }
#ifdef _WIN32
        if (drain_wake_event) {
            SetEvent(static_cast<HANDLE>(drain_wake_event));
        }
#endif
    }

    void finish_request(const std::shared_ptr<encode_request> & request)
    {
        {
            std::unique_lock lock(state_mutex);
            switch (request->stage) {
                case request_stage::queued: {
                    const auto it = std::ranges::find(incoming_requests, request);
                    if (it == incoming_requests.end()) {
                        throw msg_exception("unreachable: unexpected request");
                    }
                    incoming_requests.erase(it);
                    break;
                }
                case request_stage::picture_submitted:
                case request_stage::ready_to_lock: {
                    const auto it = std::ranges::find(inflight_requests, request);
                    if (it == inflight_requests.end()) {
                        throw msg_exception("unreachable: unexpected request");
                    }
                    inflight_requests.erase(it);
                    break;
                }
                case request_stage::submitting:
                case request_stage::finished:
                    break;
            }
        }
        request->stage = request_stage::finished;
        release_slot(request->slot_index);
        signal_async_drain_event();
    }

    void fail_request(const std::shared_ptr<encode_request> & request,
                      avifResult result_code,
                      std::string error,
                      bool notify_submit_frame = true)
    {
        bool should_notify = false;
        if (request->stage == request_stage::finished) {
            return;
        }
        if (notify_submit_frame) {
            if (request->ready_notification_sent) {
                return;
            }
            request->ready_notification_sent = true;
            should_notify = true;
        }
        request->result.result = result_code;
        request->result.error = std::move(error);
        finish_request(request);
        if (should_notify) {
            request->ready_promise.set_value();
        }
    }

    void signal_drain_loop() noexcept
    {
        signal_async_drain_event();
        drain_condition.notify_one();
    }

    void fail_outstanding_requests(avifResult result_code, const std::string & error)
    {
        std::vector<std::shared_ptr<encode_request>> requests;
        {
            std::unique_lock lock(state_mutex);
            requests.reserve(incoming_requests.size() + inflight_requests.size());
            for (const auto & request : incoming_requests) {
                if (!request->ready_notification_sent) {
                    requests.push_back(request);
                }
            }
            for (const auto & request : inflight_requests) {
                if (!request->ready_notification_sent) {
                    requests.push_back(request);
                }
            }
        }
        for (const auto & request : requests) {
            fail_request(request, result_code, error);
        }
    }

    void submit_loop()
    {
        for (;;) {
            std::shared_ptr<encode_request> request;
            {
                std::unique_lock lock(state_mutex);
                submit_condition.wait(lock, [this] { return stop_requested || !incoming_requests.empty(); });
                if (stop_requested) {
                    lock.unlock();
                    fail_outstanding_requests(AVIF_RESULT_UNKNOWN_ERROR, "NVENC session is shutting down");
                    return;
                }
                request = incoming_requests.front();
                incoming_requests.pop_front();
                request->stage = request_stage::submitting;
            }

            try {
                submit_picture(request.get());
                {
                    std::unique_lock lock(state_mutex);
                    request->stage = request_stage::picture_submitted;
                    inflight_requests.push_back(request);
                }
                if constexpr (async_events_supported()) {
                    signal_drain_loop();
                } else {
                    notify_request_ready(request);
                }
            } catch (const std::bad_alloc &) {
                fail_request(request, AVIF_RESULT_OUT_OF_MEMORY, "out of memory");
            } catch (const std::exception & e) {
                fail_request(request, AVIF_RESULT_UNKNOWN_ERROR, e.what());
            }
        }
    }

    void drain_loop()
    {
        if constexpr (!async_events_supported()) {
            return;
        }
#ifdef _WIN32
        std::vector<HANDLE> wait_handles;
        std::vector<std::shared_ptr<encode_request>> wait_requests;
        for (;;) {
            std::shared_ptr<encode_request> request;

            try {
                for (;;) {
                    {
                        std::unique_lock lock(state_mutex);
                        drain_condition.wait(lock, [this] { return stop_requested || !inflight_requests.empty(); });
                        if (stop_requested) {
                            return;
                        }

                        // Reset the drain wake event if it's set.
                        // We are currently loading the latest requests, so we don't want to be waked up by an outdated
                        // requests update notification later.
                        // The condition variable and wake event only coordinate handle-list rebuilds.
                        ResetEvent(drain_wake_event);

                        wait_handles.clear();
                        wait_requests.clear();
                        wait_handles.reserve(inflight_requests.size() + 1);
                        wait_requests.reserve(inflight_requests.size());

                        for (const auto & inflight_request : inflight_requests) {
                            if (inflight_request->slot_index >= slots.size()) {
                                request = inflight_request;
                                throw msg_exception("Submitted NVENC request used an invalid frame slot");
                            }
                            HANDLE completion_event = slots[inflight_request->slot_index].completion_event;
                            if (!completion_event) {
                                request = inflight_request;
                                throw msg_exception("Submitted NVENC request was missing a completion event");
                            }
                            wait_handles.push_back(completion_event);
                            wait_requests.push_back(inflight_request);
                        }

                        wait_handles.push_back(static_cast<HANDLE>(drain_wake_event));
                    }

                    const DWORD wait_result =
                        WaitForMultipleObjects(wait_handles.size(), wait_handles.data(), FALSE, 1000);
                    if (wait_result == WAIT_OBJECT_0 + wait_requests.size() || wait_result == WAIT_TIMEOUT) {
                        continue;
                    }
                    if (wait_result < WAIT_OBJECT_0 + wait_requests.size()) {
                        request = wait_requests[wait_result - WAIT_OBJECT_0];
                        break;
                    }
                    if (wait_result == WAIT_FAILED) {
                        throw msg_exception(
                            std::format("WaitForMultipleObjects failed with error {}", GetLastError()));
                    }
                    throw msg_exception(std::format("Unexpected WaitForMultipleObjects return value {}", wait_result));
                }

                {
                    std::unique_lock lock(state_mutex);
                    if (stop_requested) {
                        return;
                    }
                    if (request->stage != request_stage::picture_submitted) {
                        continue;
                    }
                    request->stage = request_stage::ready_to_lock;
                }
                notify_request_ready(request);
            } catch (const std::bad_alloc &) {
                if (request) {
                    fail_request(request, AVIF_RESULT_OUT_OF_MEMORY, "out of memory");
                }
            } catch (const std::exception & e) {
                if (request) {
                    fail_request(request, AVIF_RESULT_UNKNOWN_ERROR, e.what());
                }
            }
        }
#endif
    }

    session_key key;
    session_config active_config;
    CUcontext cuda_context = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api;
    void * encoder = nullptr;
    NV_ENC_INITIALIZE_PARAMS initialize_params = {};
    NV_ENC_CONFIG encode_config = {};
    std::vector<buffer_slot> slots;
    size_t next_slot = 0;
    uint32_t frame_index = 0;
    bool monochrome_supported = false;
    std::mutex state_mutex;
    std::condition_variable slot_condition;
    std::condition_variable submit_condition;
    std::condition_variable drain_condition;
    std::deque<std::shared_ptr<encode_request>> incoming_requests;
    std::vector<std::shared_ptr<encode_request>> inflight_requests;
    bool stop_requested = false;
    void * drain_wake_event = nullptr;
    std::thread submitter;
    std::thread drainer;
};

nvenc_session::frame_buffer::frame_buffer(nvenc_session * session, size_t slot_index) noexcept : session_(session), slot_index_(slot_index) {}

nvenc_session::frame_buffer::frame_buffer(frame_buffer && other) noexcept
{
    session_ = other.session_;
    slot_index_ = other.slot_index_;
    pitch_ = other.pitch_;
    other.detach();
}

nvenc_session::frame_buffer & nvenc_session::frame_buffer::operator=(frame_buffer && other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (session_) {
        session_->release_frame(this);
    }
    session_ = other.session_;
    slot_index_ = other.slot_index_;
    pitch_ = other.pitch_;
    other.detach();
    return *this;
}

nvenc_session::frame_buffer::~frame_buffer()
{
    if (session_) {
        session_->release_frame(this);
    }
}

nvenc_session::frame_buffer::operator bool() const noexcept
{
    return session_ != nullptr;
}

void nvenc_session::frame_buffer::detach() noexcept
{
    session_ = nullptr;
    slot_index_ = kInvalidSlotIndex;
    pitch_ = 0;
}

nvenc_session::nvenc_session(const session_key & key,
                             const session_config & config,
                             CUcontext cuda_context,
                             const NV_ENCODE_API_FUNCTION_LIST & api_functions)
    : impl_(std::make_unique<impl>(key, config, cuda_context, api_functions))
{
}

nvenc_session::~nvenc_session() = default;

void nvenc_session::release_frame(frame_buffer * frame) const noexcept
{
    if (!frame || frame->session_ != this) {
        return;
    }
    impl_->release_slot(frame->slot_index_);
    frame->detach();
}

avifResult nvenc_session::acquire_frame(frame_buffer * frame, avifDiagnostics * diag)
{
    try {
        if (!frame) {
            throw msg_exception("Missing NVENC frame buffer handle");
        }
        if (*frame) {
            release_frame(frame);
        }
        *frame = frame_buffer(this, impl_->reserve_frame_slot());
        return AVIF_RESULT_OK;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diag, "out of memory");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult nvenc_session::upload_image(frame_buffer * frame,
                                       const session_config & config,
                                       const avifImage * image,
                                       avifDiagnostics * diag) const
{
    try {
        if (!frame || !*frame) {
            throw msg_exception("NVENC frame buffer is not reserved");
        }
        if (!image) {
            throw msg_exception("Missing AVIF image for NVENC upload");
        }
        impl_->upload_image_to_slot(frame->slot_index_, config, image, &frame->pitch_);
        return AVIF_RESULT_OK;
    } catch (const std::bad_alloc &) {
        release_frame(frame);
        set_diagnostic(diag, "out of memory");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        release_frame(frame);
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult nvenc_session::submit_frame(frame_buffer * frame,
                                       const session_config & config,
                                       encoded_frame * encoded,
                                       avifDiagnostics * diag) const
{
    try {
        if (!frame || !*frame) {
            throw msg_exception("NVENC frame buffer is not reserved");
        }
        if (frame->pitch_ == 0) {
            throw msg_exception("NVENC frame buffer was submitted before upload");
        }

        auto const request = std::make_shared<impl::encode_request>();
        request->slot_index = frame->slot_index_;
        request->pitch = frame->pitch_;
        request->config = config;
        auto ready_future = request->ready_promise.get_future();

        {
            std::unique_lock lock(impl_->state_mutex);
            if (impl_->stop_requested) {
                throw msg_exception("NVENC session is shutting down");
            }
            impl_->slots[request->slot_index].state = impl::slot_state::submitted;
            impl_->incoming_requests.push_back(request);
        }

        frame->detach();
        impl_->submit_condition.notify_one();

        try {
            ready_future.get();

            {
                std::unique_lock lock(impl_->state_mutex);
                if (request->result.result != AVIF_RESULT_OK) {
                    set_diagnostic(diag, "{}",
                                   request->result.error.empty() ? "NVENC encode failed" : request->result.error);
                    return request->result.result;
                }
            }

            encoded_frame result = impl_->lock_output_bitstream(impl_->slots[request->slot_index]);
            if (result.bytes.empty()) {
                throw msg_exception("NVENC encode produced no bitstream");
            }

            impl_->finish_request(request);
            if (encoded) {
                *encoded = std::move(result);
            }
            return AVIF_RESULT_OK;
        } catch (const std::bad_alloc &) {
            impl_->fail_request(request, AVIF_RESULT_OUT_OF_MEMORY, "out of memory", false);
            set_diagnostic(diag, "out of memory");
            return AVIF_RESULT_OUT_OF_MEMORY;
        } catch (const std::exception & e) {
            impl_->fail_request(request, AVIF_RESULT_UNKNOWN_ERROR, e.what(), false);
            set_diagnostic(diag, "{}", e.what());
            return AVIF_RESULT_UNKNOWN_ERROR;
        }
    } catch (const std::bad_alloc &) {
        release_frame(frame);
        set_diagnostic(diag, "out of memory");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        release_frame(frame);
        set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

} // namespace avif_nvenc
