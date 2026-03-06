#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_GTEST_HELPERS_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_GTEST_HELPERS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "avif/avif.h"
#include "avif_nvenc_codec.h"

namespace avif_nvenc::testutil
{

bool DiagnosticContains(const avifDiagnostics & diag, std::string_view needle);
bool IsRuntimeSkip(const avifDiagnostics & diag);
bool IsCapabilitySkip(const avifDiagnostics & diag, std::string_view needle);
void SetDiagMessage(avifDiagnostics * diag, std::string_view message);
std::string MakeUniquePoolId(std::string_view prefix);

class PoolGuard
{
public:
    explicit PoolGuard(std::string pool_id);
    ~PoolGuard();

    PoolGuard(const PoolGuard &) = delete;
    PoolGuard & operator=(const PoolGuard &) = delete;

    void release();

private:
    std::string pool_id_;
};

class AppSetupGuard
{
public:
    AppSetupGuard() = default;
    ~AppSetupGuard();

    AppSetupGuard(const AppSetupGuard &) = delete;
    AppSetupGuard & operator=(const AppSetupGuard &) = delete;

    void arm();
    void release();

private:
    bool active_ = false;
};

struct EncodeOptions
{
    uint32_t width = 256;
    uint32_t height = 256;
    uint32_t depth = 8;
    avifPixelFormat yuv_format = AVIF_PIXEL_FORMAT_YUV420;
    bool with_alpha = false;
    bool with_gain_map = false;
    int quality = 80;
    int quality_alpha = 80;
    int quality_gain_map = 80;
    int speed = AVIF_SPEED_DEFAULT;
    std::string pool_id;
    std::optional<std::string> preset;
    std::optional<std::string> tuning;
};

avifResult CreateOnDemandPool(std::string_view pool_id, uint32_t max_sessions, avifDiagnostics * diag);
avifResult CreateOmniPool(std::string_view pool_id,
                          uint32_t max_sessions,
                          uint32_t max_width,
                          uint32_t max_height,
                          avifDiagnostics * diag);
avifResult FillImage(avifImage * image, bool with_alpha);
avifResult EncodeWithOptions(const EncodeOptions & options, avifDiagnostics * diag, avifRWData * output = nullptr);
avifResult GetDecodedImagePsnr(const avifRWData & output,
                               const EncodeOptions & reference_options,
                               double * psnr,
                               avifDiagnostics * diag);
avifResult ValidateAlphaOutput(const avifRWData & output, avifDiagnostics * diag);
avifResult ValidateGainMapOutput(const avifRWData & output, avifDiagnostics * diag);

} // namespace avif_nvenc::testutil

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_GTEST_HELPERS_H
