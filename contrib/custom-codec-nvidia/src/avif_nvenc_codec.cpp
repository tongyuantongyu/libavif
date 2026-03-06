// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif_nvenc_codec.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <new>

#include "codec_wrapper.h"
#include "exception.h"
#include "runtime.h"

namespace
{

std::mutex g_register_mutex;
avifCodecChoice g_nvenc_codec_choice = AVIF_CODEC_CHOICE_AUTO;
bool g_apps_setup_complete = false;
constexpr char kAppsPoolId[] = "avifenc_pool";

bool has_image_size(const avifNvencImageSize & size)
{
    return size.width != 0 || size.height != 0;
}

bool image_size_area_less(const avif_nvenc::image_size & lhs, const avif_nvenc::image_size & rhs)
{
    const uint64_t lhs_area = static_cast<uint64_t>(lhs.width) * lhs.height;
    const uint64_t rhs_area = static_cast<uint64_t>(rhs.width) * rhs.height;
    if (lhs_area != rhs_area) {
        return lhs_area < rhs_area;
    }
    if (lhs.width != rhs.width) {
        return lhs.width < rhs.width;
    }
    return lhs.height < rhs.height;
}

avifResult build_registered_pool_options(const avifNvencPoolConfig * config,
                                         avif_nvenc::registered_pool_options * options,
                                         avifDiagnostics * diag)
{
    if (!config) {
        avif_nvenc::set_diagnostic(diag, "Missing NVENC pool configuration");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (!config->id || config->id[0] == '\0') {
        avif_nvenc::set_diagnostic(diag, "NVENC pool id must not be empty");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    options->id = config->id;
    options->gpu = config->gpu;
    options->max_sessions = (config->maxSessions == 0) ? 1u : config->maxSessions;

    switch (config->mode) {
        case AVIF_NVENC_POOL_MODE_ON_DEMAND:
            options->mode = avif_nvenc::pool_mode::on_demand;
            break;
        case AVIF_NVENC_POOL_MODE_OMNI:
            options->mode = avif_nvenc::pool_mode::omni;
            break;
        case AVIF_NVENC_POOL_MODE_BUCKET:
            options->mode = avif_nvenc::pool_mode::bucket;
            break;
        default:
            avif_nvenc::set_diagnostic(diag, "Invalid NVENC pool mode for '{}'", options->id);
            return AVIF_RESULT_INVALID_ARGUMENT;
    }

    if (has_image_size(config->maxSize)) {
        if (config->maxSize.width == 0 || config->maxSize.height == 0) {
            avif_nvenc::set_diagnostic(diag, "Invalid NVENC maxSize for pool '{}': {}x{}", options->id, config->maxSize.width, config->maxSize.height);
            return AVIF_RESULT_INVALID_ARGUMENT;
        }
        options->max_size = { config->maxSize.width, config->maxSize.height };
    }

    if (config->bucketSizeCount > 0) {
        if (!config->bucketSizes) {
            avif_nvenc::set_diagnostic(diag, "Missing NVENC bucketSizes for pool '{}'", options->id);
            return AVIF_RESULT_INVALID_ARGUMENT;
        }
        options->bucket_sizes.reserve(config->bucketSizeCount);
        for (size_t i = 0; i < config->bucketSizeCount; ++i) {
            const avifNvencImageSize & bucket = config->bucketSizes[i];
            if (bucket.width == 0 || bucket.height == 0) {
                avif_nvenc::set_diagnostic(diag, "Invalid NVENC bucketSize[{}] for pool '{}': {}x{}", i, options->id, bucket.width, bucket.height);
                return AVIF_RESULT_INVALID_ARGUMENT;
            }
            options->bucket_sizes.push_back({ bucket.width, bucket.height });
        }
        std::ranges::sort(options->bucket_sizes, image_size_area_less);
    }

    return AVIF_RESULT_OK;
}

avifResult register_codec_unlocked()
{
    const avifCodecChoice existing_choice = avifCodecChoiceFromName(AVIF_NVENC_CODEC_NAME);
    if (existing_choice != AVIF_CODEC_CHOICE_AUTO) {
        g_nvenc_codec_choice = existing_choice;
        return AVIF_RESULT_OK;
    }

    avifCodecInformation info = {};
    info.type = AVIF_CODEC_TYPE_AV1;
    info.name = AVIF_NVENC_CODEC_NAME;
    info.version = avif_nvenc::codec_version;
    info.create = avif_nvenc::create_codec;
    info.flags = AVIF_CODEC_FLAG_CAN_ENCODE;

    const avifResult result = avifRegisterCustomCodec(&info);
    if (result == AVIF_RESULT_OK) {
        g_nvenc_codec_choice = info.choice;
    }
    return result;
}

} // namespace

extern "C" {

const char * avifNvencVersion(void)
{
    return avif_nvenc::codec_version();
}

avifCodecChoice avifNvencCodecChoice(void)
{
    const avifCodecChoice choice = avifCodecChoiceFromName(AVIF_NVENC_CODEC_NAME);
    return (choice != AVIF_CODEC_CHOICE_AUTO) ? choice : g_nvenc_codec_choice;
}

avifResult avifNvencCreatePool(const avifNvencPoolConfig * config, avifDiagnostics * diag)
{
    try {
        avif_nvenc::registered_pool_options options;
        const avifResult build_result = build_registered_pool_options(config, &options, diag);
        if (build_result != AVIF_RESULT_OK) {
            return build_result;
        }
        return avif_nvenc::create_pool(options, diag);
    } catch (const std::bad_alloc &) {
        avif_nvenc::set_diagnostic(diag, "Out of memory creating NVENC pool");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        avif_nvenc::set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult avifNvencShutdownPool(const char * poolId, avifDiagnostics * diag)
{
    try {
        return avif_nvenc::shutdown_pool(poolId ? poolId : "", diag);
    } catch (const std::bad_alloc &) {
        avif_nvenc::set_diagnostic(diag, "Out of memory shutting down NVENC pool");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        avif_nvenc::set_diagnostic(diag, "{}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult avifNvencRegister(void)
{
    std::scoped_lock lock(g_register_mutex);
    return register_codec_unlocked();
}

avifResult avifAppsCustomCodecSetup(avifDiagnostics * diag)
{
    std::scoped_lock lock(g_register_mutex);
    if (g_apps_setup_complete) {
        return AVIF_RESULT_OK;
    }

    avifResult result = register_codec_unlocked();
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    avifNvencPoolConfig config = {};
    config.id = kAppsPoolId;
    config.mode = AVIF_NVENC_POOL_MODE_ON_DEMAND;
    config.maxSessions = 1;
    result = avifNvencCreatePool(&config, diag);
    if (result == AVIF_RESULT_OK) {
        g_apps_setup_complete = true;
    }
    return result;
}

avifResult avifAppsCustomCodecShutdown(avifDiagnostics * diag)
{
    std::scoped_lock lock(g_register_mutex);
    if (!g_apps_setup_complete) {
        return AVIF_RESULT_OK;
    }

    const avifResult result = avifNvencShutdownPool(kAppsPoolId, diag);
    if (result == AVIF_RESULT_OK) {
        g_apps_setup_complete = false;
    }
    return result;
}

} // extern "C"
