#include "gtest_helpers.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace avif_nvenc::testutil
{
namespace
{

std::atomic<uint64_t> g_pool_counter = 0;

avifResult AttachGainMap(avifImage * image);

avifResult SetCodecOption(avifEncoder * encoder,
                          const char * key,
                          const std::optional<std::string> & value,
                          avifDiagnostics * diag)
{
    if (!value) {
        return AVIF_RESULT_OK;
    }
    const avifResult result = avifEncoderSetCodecSpecificOption(encoder, key, value->c_str());
    *diag = encoder->diag;
    return result;
}

avifResult CreateImageForOptions(const EncodeOptions & options, avifImage ** image)
{
    *image = avifImageCreate(options.width, options.height, options.depth, options.yuv_format);
    if (!*image) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    (*image)->yuvRange = AVIF_RANGE_FULL;
    (*image)->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    (*image)->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    (*image)->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;

    avifResult result = FillImage(*image, options.with_alpha);
    if (result == AVIF_RESULT_OK && options.with_gain_map) {
        result = AttachGainMap(*image);
    }
    if (result != AVIF_RESULT_OK) {
        avifImageDestroy(*image);
        *image = nullptr;
    }
    return result;
}

template <typename Sample>
uint64_t SquaredDiffSum(const Sample * samples1, const Sample * samples2, uint32_t num_samples)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < num_samples; ++i) {
        const int32_t diff = static_cast<int32_t>(samples1[i]) - samples2[i];
        sum += static_cast<uint64_t>(diff * diff);
    }
    return sum;
}

double GetPsnr(const avifImage & image1, const avifImage & image2)
{
    if (image1.width != image2.width || image1.height != image2.height || image1.depth != image2.depth ||
        image1.yuvFormat != image2.yuvFormat || image1.yuvRange != image2.yuvRange) {
        return -1.0;
    }

    uint64_t squared_diff_sum = 0;
    uint32_t num_samples = 0;
    const uint32_t max_sample_value = (1u << image1.depth) - 1u;
    for (avifChannelIndex channel : { AVIF_CHAN_Y, AVIF_CHAN_U, AVIF_CHAN_V, AVIF_CHAN_A }) {
        const uint32_t plane_width = std::max(avifImagePlaneWidth(&image1, channel), avifImagePlaneWidth(&image2, channel));
        const uint32_t plane_height =
            std::max(avifImagePlaneHeight(&image1, channel), avifImagePlaneHeight(&image2, channel));
        if (plane_width == 0 || plane_height == 0) {
            continue;
        }

        const uint8_t * row1 = avifImagePlane(&image1, channel);
        const uint8_t * row2 = avifImagePlane(&image2, channel);
        if (!row1 != !row2 && channel != AVIF_CHAN_A) {
            return -1.0;
        }
        uint32_t row_bytes1 = avifImagePlaneRowBytes(&image1, channel);
        uint32_t row_bytes2 = avifImagePlaneRowBytes(&image2, channel);

        // Consider a missing alpha plane equivalent to fully opaque samples.
        std::vector<uint8_t> opaque_alpha_samples;
        if (!row1 != !row2) {
            opaque_alpha_samples.resize(std::max(row_bytes1, row_bytes2));
            if (avifImageUsesU16(&image1)) {
                uint16_t * opaque_alpha_samples_16 = reinterpret_cast<uint16_t *>(opaque_alpha_samples.data());
                std::fill(opaque_alpha_samples_16, opaque_alpha_samples_16 + plane_width, static_cast<uint16_t>(max_sample_value));
            } else {
                std::fill(opaque_alpha_samples.begin(), opaque_alpha_samples.end(), uint8_t{ 255 });
            }
            if (!row1) {
                row1 = opaque_alpha_samples.data();
                row_bytes1 = 0;
            } else {
                row2 = opaque_alpha_samples.data();
                row_bytes2 = 0;
            }
        }

        for (uint32_t y = 0; y < plane_height; ++y) {
            if (avifImageUsesU16(&image1)) {
                squared_diff_sum += SquaredDiffSum(reinterpret_cast<const uint16_t *>(row1),
                                                   reinterpret_cast<const uint16_t *>(row2),
                                                   plane_width);
            } else {
                squared_diff_sum += SquaredDiffSum(row1, row2, plane_width);
            }
            row1 += row_bytes1;
            row2 += row_bytes2;
            num_samples += plane_width;
        }
    }

    if (squared_diff_sum == 0) {
        return 99.0;
    }
    const double normalized_error =
        squared_diff_sum / (static_cast<double>(num_samples) * max_sample_value * max_sample_value);
    if (normalized_error <= std::numeric_limits<double>::epsilon()) {
        return 98.99;
    }
    return std::min(-10.0 * std::log10(normalized_error), 98.99);
}

} // namespace

avifResult FillImage(avifImage * image, bool with_alpha)
{
    const avifPlanesFlags planes = with_alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV;
    const avifResult allocate_result = avifImageAllocatePlanes(image, planes);
    if (allocate_result != AVIF_RESULT_OK) {
        return allocate_result;
    }

    const uint32_t max_value = (image->depth <= 8) ? 255u : ((1u << image->depth) - 1u);
    for (int channel = AVIF_CHAN_Y; channel <= AVIF_CHAN_V; ++channel) {
        uint8_t * plane = image->yuvPlanes[channel];
        if (!plane) {
            continue;
        }

        const uint32_t plane_width = avifImagePlaneWidth(image, channel);
        const uint32_t plane_height = avifImagePlaneHeight(image, channel);
        const bool luma = (channel == AVIF_CHAN_Y);
        const uint32_t fill_value = (channel == AVIF_CHAN_U) ? (max_value / 4) : ((max_value * 3) / 4);

        for (uint32_t y = 0; y < plane_height; ++y) {
            if (image->depth <= 8) {
                uint8_t * row = plane + static_cast<size_t>(y) * image->yuvRowBytes[channel];
                for (uint32_t x = 0; x < plane_width; ++x) {
                    row[x] = luma ? static_cast<uint8_t>((x * max_value) / ((plane_width > 1) ? (plane_width - 1) : 1)) :
                                    static_cast<uint8_t>(fill_value);
                }
            } else {
                uint16_t * row = reinterpret_cast<uint16_t *>(plane + static_cast<size_t>(y) * image->yuvRowBytes[channel]);
                for (uint32_t x = 0; x < plane_width; ++x) {
                    row[x] = luma ? static_cast<uint16_t>((x * max_value) / ((plane_width > 1) ? (plane_width - 1) : 1)) :
                                    static_cast<uint16_t>(fill_value);
                }
            }
        }
    }

    if (!with_alpha) {
        return AVIF_RESULT_OK;
    }

    const uint32_t divisor = (image->width > 1) ? (image->width - 1) : 1;
    for (uint32_t y = 0; y < image->height; ++y) {
        if (image->depth <= 8) {
            uint8_t * row = image->alphaPlane + static_cast<size_t>(y) * image->alphaRowBytes;
            for (uint32_t x = 0; x < image->width; ++x) {
                row[x] = static_cast<uint8_t>((x * max_value) / divisor);
            }
        } else {
            uint16_t * row = reinterpret_cast<uint16_t *>(image->alphaPlane + static_cast<size_t>(y) * image->alphaRowBytes);
            for (uint32_t x = 0; x < image->width; ++x) {
                row[x] = static_cast<uint16_t>((x * max_value) / divisor);
            }
        }
    }
    return AVIF_RESULT_OK;
}

namespace
{

avifResult AttachGainMap(avifImage * image)
{
    avifGainMap * gain_map = avifGainMapCreate();
    if (!gain_map) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    avifImage * gain_map_image = avifImageCreate(image->width, image->height, image->depth, image->yuvFormat);
    if (!gain_map_image) {
        avifGainMapDestroy(gain_map);
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    gain_map_image->yuvRange = AVIF_RANGE_FULL;
    gain_map_image->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    gain_map_image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    gain_map_image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;

    const avifResult fill_result = FillImage(gain_map_image, /*with_alpha=*/false);
    if (fill_result != AVIF_RESULT_OK) {
        avifImageDestroy(gain_map_image);
        avifGainMapDestroy(gain_map);
        return fill_result;
    }

    gain_map->altColorPrimaries = image->colorPrimaries;
    gain_map->altTransferCharacteristics = image->transferCharacteristics;
    gain_map->altMatrixCoefficients = image->matrixCoefficients;
    gain_map->altYUVRange = image->yuvRange;
    gain_map->altDepth = image->depth;
    gain_map->altPlaneCount = 3;
    gain_map->image = gain_map_image;
    image->gainMap = gain_map;
    return AVIF_RESULT_OK;
}

} // namespace

bool DiagnosticContains(const avifDiagnostics & diag, std::string_view needle)
{
    return std::string_view(diag.error).find(needle) != std::string_view::npos;
}

bool IsRuntimeSkip(const avifDiagnostics & diag)
{
    return DiagnosticContains(diag, "failed loading module") || DiagnosticContains(diag, "failed loading function") ||
           DiagnosticContains(diag, "no encode capable devices") || DiagnosticContains(diag, "device not supported") ||
           DiagnosticContains(diag, "does not expose NVENC AV1 encode support") ||
           DiagnosticContains(diag, "does not support this NVENC API version");
}

bool IsCapabilitySkip(const avifDiagnostics & diag, std::string_view needle)
{
    return IsRuntimeSkip(diag) || DiagnosticContains(diag, needle);
}

void SetDiagMessage(avifDiagnostics * diag, std::string_view message)
{
    if (!diag) {
        return;
    }
    avifDiagnosticsClearError(diag);
    std::snprintf(diag->error, sizeof(diag->error), "%.*s", static_cast<int>(message.size()), message.data());
}

std::string MakeUniquePoolId(std::string_view prefix)
{
    const uint64_t value = ++g_pool_counter;
    return std::string(prefix) + "-" + std::to_string(value);
}

PoolGuard::PoolGuard(std::string pool_id) : pool_id_(std::move(pool_id)) {}

PoolGuard::~PoolGuard()
{
    if (!pool_id_.empty()) {
        (void)avifNvencShutdownPool(pool_id_.c_str(), nullptr);
    }
}

void PoolGuard::release()
{
    pool_id_.clear();
}

AppSetupGuard::~AppSetupGuard()
{
    if (active_) {
        (void)avifAppsCustomCodecShutdown(nullptr);
    }
}

void AppSetupGuard::arm()
{
    active_ = true;
}

void AppSetupGuard::release()
{
    active_ = false;
}

avifResult CreateOnDemandPool(std::string_view pool_id, uint32_t max_sessions, avifDiagnostics * diag)
{
    avifNvencPoolConfig pool_config = {};
    pool_config.id = pool_id.data();
    pool_config.mode = AVIF_NVENC_POOL_MODE_ON_DEMAND;
    pool_config.maxSessions = max_sessions;
    return avifNvencCreatePool(&pool_config, diag);
}

avifResult CreateOmniPool(std::string_view pool_id,
                          uint32_t max_sessions,
                          uint32_t max_width,
                          uint32_t max_height,
                          avifDiagnostics * diag)
{
    avifNvencPoolConfig pool_config = {};
    pool_config.id = pool_id.data();
    pool_config.mode = AVIF_NVENC_POOL_MODE_OMNI;
    pool_config.maxSessions = max_sessions;
    pool_config.maxSize.width = max_width;
    pool_config.maxSize.height = max_height;
    return avifNvencCreatePool(&pool_config, diag);
}

avifResult EncodeWithOptions(const EncodeOptions & options, avifDiagnostics * diag, avifRWData * output)
{
    avifImage * image = nullptr;
    avifResult result = CreateImageForOptions(options, &image);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    avifEncoder * encoder = avifEncoderCreate();
    if (!encoder) {
        avifImageDestroy(image);
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    encoder->codecChoice = avifNvencCodecChoice();
    encoder->quality = options.quality;
    encoder->qualityAlpha = options.quality_alpha;
    encoder->qualityGainMap = options.quality_gain_map;
    encoder->speed = options.speed;
    encoder->maxThreads = 1;

    if (!options.pool_id.empty()) {
        result = avifEncoderSetCodecSpecificOption(encoder, AVIF_NVENC_OPTION_POOL, options.pool_id.c_str());
        *diag = encoder->diag;
    }
    if (result == AVIF_RESULT_OK) {
        result = SetCodecOption(encoder, AVIF_NVENC_OPTION_PRESET, options.preset, diag);
    }
    if (result == AVIF_RESULT_OK) {
        result = SetCodecOption(encoder, AVIF_NVENC_OPTION_TUNING, options.tuning, diag);
    }
    if (result != AVIF_RESULT_OK) {
        avifEncoderDestroy(encoder);
        avifImageDestroy(image);
        return result;
    }

    avifRWData encoded = AVIF_DATA_EMPTY;
    result = avifEncoderWrite(encoder, image, &encoded);
    *diag = encoder->diag;

    avifEncoderDestroy(encoder);
    avifImageDestroy(image);

    if (output) {
        *output = encoded;
    } else {
        avifRWDataFree(&encoded);
    }
    return result;
}

avifResult GetDecodedImagePsnr(const avifRWData & output,
                               const EncodeOptions & reference_options,
                               double * psnr,
                               avifDiagnostics * diag)
{
    if (!psnr) {
        SetDiagMessage(diag, "Missing PSNR output pointer");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    *psnr = -1.0;

    avifImage * reference = nullptr;
    avifResult result = CreateImageForOptions(reference_options, &reference);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    avifDecoder * decoder = avifDecoderCreate();
    if (!decoder) {
        avifImageDestroy(reference);
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    result = avifDecoderSetIOMemory(decoder, output.data, output.size);
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderParse(decoder);
    }
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderNextImage(decoder);
    }
    if (result != AVIF_RESULT_OK) {
        *diag = decoder->diag;
        avifDecoderDestroy(decoder);
        avifImageDestroy(reference);
        return result;
    }
    if (!decoder->image) {
        SetDiagMessage(diag, "Decoded AVIF did not produce an image");
        avifDecoderDestroy(decoder);
        avifImageDestroy(reference);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }

    *psnr = GetPsnr(*reference, *decoder->image);
    if (*psnr < 0.0) {
        SetDiagMessage(diag, "Decoded AVIF image was not comparable to the reference image");
        avifDecoderDestroy(decoder);
        avifImageDestroy(reference);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }

    avifDecoderDestroy(decoder);
    avifImageDestroy(reference);
    return AVIF_RESULT_OK;
}

avifResult ValidateAlphaOutput(const avifRWData & output, avifDiagnostics * diag)
{
    avifDecoder * decoder = avifDecoderCreate();
    if (!decoder) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    avifResult result = avifDecoderSetIOMemory(decoder, output.data, output.size);
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderParse(decoder);
    }
    if (result != AVIF_RESULT_OK) {
        *diag = decoder->diag;
        avifDecoderDestroy(decoder);
        return result;
    }
    if (!decoder->alphaPresent) {
        SetDiagMessage(diag, "Encoded AVIF did not advertise an alpha item");
        avifDecoderDestroy(decoder);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }

    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK) {
        *diag = decoder->diag;
        avifDecoderDestroy(decoder);
        return result;
    }
    if (!decoder->image || !decoder->image->alphaPlane) {
        SetDiagMessage(diag, "Decoded AVIF did not produce an alpha plane");
        avifDecoderDestroy(decoder);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
    if (avifImageIsOpaque(decoder->image)) {
        SetDiagMessage(diag, "Decoded AVIF alpha plane is fully opaque");
        avifDecoderDestroy(decoder);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }

    avifDecoderDestroy(decoder);
    return AVIF_RESULT_OK;
}

avifResult ValidateGainMapOutput(const avifRWData & output, avifDiagnostics * diag)
{
    avifDecoder * decoder = avifDecoderCreate();
    if (!decoder) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;

    avifResult result = avifDecoderSetIOMemory(decoder, output.data, output.size);
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderParse(decoder);
    }
    if (result != AVIF_RESULT_OK) {
        *diag = decoder->diag;
        avifDecoderDestroy(decoder);
        return result;
    }

    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK) {
        *diag = decoder->diag;
        avifDecoderDestroy(decoder);
        return result;
    }
    if (!decoder->image || !decoder->image->gainMap || !decoder->image->gainMap->image) {
        SetDiagMessage(diag, "Decoded AVIF did not produce a gain map image");
        avifDecoderDestroy(decoder);
        return AVIF_RESULT_UNKNOWN_ERROR;
    }

    avifDecoderDestroy(decoder);
    return AVIF_RESULT_OK;
}

} // namespace avif_nvenc::testutil
