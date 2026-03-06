#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "avif/codec.h"
#include "avif/avif.h"
#include "codec_wrapper.h"
#include "gtest/gtest.h"
#include "gtest_helpers.h"

namespace avif_nvenc
{
namespace
{

#define ASSERT_NVENC_OK_OR_SKIP(result_expr, diag_expr)                                                                                   \
    do {                                                                                                                                 \
        const avifResult result_value = (result_expr);                                                                                   \
        if (result_value != AVIF_RESULT_OK) {                                                                                            \
            if (testutil::IsRuntimeSkip(diag_expr)) {                                                                                    \
                GTEST_SKIP() << (diag_expr).error;                                                                                       \
            }                                                                                                                            \
            ASSERT_EQ(result_value, AVIF_RESULT_OK) << avifResultToString(result_value) << " " << (diag_expr).error;                    \
        }                                                                                                                                \
    } while (false)

#define ASSERT_NVENC_OK_OR_FEATURE_SKIP(result_expr, diag_expr, needle)                                                                  \
    do {                                                                                                                                 \
        const avifResult result_value = (result_expr);                                                                                   \
        if (result_value != AVIF_RESULT_OK) {                                                                                            \
            if (testutil::IsCapabilitySkip(diag_expr, needle)) {                                                                         \
                GTEST_SKIP() << (diag_expr).error;                                                                                       \
            }                                                                                                                            \
            ASSERT_EQ(result_value, AVIF_RESULT_OK) << avifResultToString(result_value) << " " << (diag_expr).error;                    \
        }                                                                                                                                \
    } while (false)

struct ThreadedResult
{
    avifResult result = AVIF_RESULT_OK;
    avifDiagnostics diag = {};
};

TEST(NvencIntegrationTest, RegistersCodec)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    EXPECT_NE(avifNvencCodecChoice(), AVIF_CODEC_CHOICE_AUTO);
    EXPECT_EQ(avifCodecChoiceFromName("nvenc"), avifNvencCodecChoice());
}

TEST(NvencIntegrationTest, RegisterAndAppHooksAreIdempotent)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);

    avifDiagnostics diag = {};
    testutil::AppSetupGuard app_guard;
    avifResult result = avifAppsCustomCodecSetup(&diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
    app_guard.arm();

    diag = {};
    ASSERT_EQ(avifAppsCustomCodecSetup(&diag), AVIF_RESULT_OK) << diag.error;

    diag = {};
    ASSERT_EQ(avifAppsCustomCodecShutdown(&diag), AVIF_RESULT_OK) << diag.error;
    app_guard.release();

    diag = {};
    ASSERT_EQ(avifAppsCustomCodecShutdown(&diag), AVIF_RESULT_OK) << diag.error;
}

TEST(NvencIntegrationTest, CreatesAndShutsDownOnDemandPool)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-pool");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    const avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    ASSERT_EQ(avifNvencShutdownPool(pool_id.c_str(), &diag), AVIF_RESULT_OK) << diag.error;
    pool_guard.release();
}

TEST(NvencIntegrationTest, Encodes8BitColor)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-color");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.quality = 60;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
}

TEST(NvencIntegrationTest, ReusesWarmSessionForSameSizeRequests)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-reuse");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.quality = 60;
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    options.quality = 80;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
}

TEST(NvencIntegrationTest, SequentialRequestsReuseSingleWorker)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-sequential");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.quality = 60;
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    options.quality = 80;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
}

TEST(NvencIntegrationTest, OmniPoolEncodesDifferentImageSizes)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-omni");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result =
        testutil::CreateOmniPool(pool_id, /*max_sessions=*/1, /*max_width=*/512, /*max_height=*/384, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    struct EncodeCase
    {
        uint32_t width;
        uint32_t height;
        int quality;
        double min_psnr;
    };
    constexpr std::array<EncodeCase, 3> kCases = {
        EncodeCase{ 256, 256, 60, 40.0 },
        EncodeCase{ 512, 384, 80, 40.0 },
        EncodeCase{ 320, 192, 70, 40.0 },
    };

    for (const EncodeCase & encode_case : kCases) {
        SCOPED_TRACE(::testing::Message() << encode_case.width << "x" << encode_case.height << " q=" << encode_case.quality);

        testutil::EncodeOptions options;
        options.pool_id = pool_id;
        options.width = encode_case.width;
        options.height = encode_case.height;
        options.quality = encode_case.quality;

        avifRWData output = AVIF_DATA_EMPTY;
        diag = {};
        result = testutil::EncodeWithOptions(options, &diag, &output);
        ASSERT_NVENC_OK_OR_SKIP(result, diag);

        double psnr = -1.0;
        diag = {};
        ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr, &diag), AVIF_RESULT_OK) << diag.error;
        EXPECT_GE(psnr, encode_case.min_psnr);
        avifRWDataFree(&output);
    }
}

TEST(NvencIntegrationTest, Encodes8BitAlphaAndDecodedOutputHasAlpha)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-alpha");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.with_alpha = true;
    avifRWData output = AVIF_DATA_EMPTY;

    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    EXPECT_EQ(testutil::ValidateAlphaOutput(output, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);
}

TEST(NvencIntegrationTest, ReusesSessionAcrossColorAlphaColor)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-color-alpha");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    options.with_alpha = true;
    avifRWData alpha_output = AVIF_DATA_EMPTY;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &alpha_output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    EXPECT_EQ(testutil::ValidateAlphaOutput(alpha_output, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&alpha_output);

    options.with_alpha = false;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
}

TEST(NvencIntegrationTest, Encodes8BitGainMapAndDecodedOutputHasGainMap)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-gainmap");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.with_gain_map = true;
    options.quality_gain_map = 70;
    avifRWData output = AVIF_DATA_EMPTY;

    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    EXPECT_EQ(testutil::ValidateGainMapOutput(output, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);
}

TEST(NvencIntegrationTest, ConcurrentSameKeyEncodesShareSingleSessionBudget)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-overlap");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    std::mutex start_mutex;
    std::condition_variable start_condition;
    int ready_threads = 0;
    bool start = false;
    ThreadedResult first;
    ThreadedResult second;

    auto worker = [&](ThreadedResult * thread_result, int quality) {
        {
            std::unique_lock lock(start_mutex);
            ++ready_threads;
            if (ready_threads == 2) {
                start = true;
                start_condition.notify_all();
            } else {
                start_condition.wait(lock, [&start] { return start; });
            }
        }

        testutil::EncodeOptions options;
        options.pool_id = pool_id;
        options.width = 512;
        options.height = 512;
        options.quality = quality;
        thread_result->result = testutil::EncodeWithOptions(options, &thread_result->diag);
    };

    std::thread first_thread(worker, &first, 70);
    std::thread second_thread(worker, &second, 90);
    first_thread.join();
    second_thread.join();

    ASSERT_NVENC_OK_OR_SKIP(first.result, first.diag);
    ASSERT_NVENC_OK_OR_SKIP(second.result, second.diag);
}

TEST(NvencIntegrationTest, Encodes10BitColorWhenSupported)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-10bit-color");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.depth = 10;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_FEATURE_SKIP(result, diag, "10-bit");
}

TEST(NvencIntegrationTest, Encodes10BitAlphaWhenSupported)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-10bit-alpha");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.depth = 10;
    options.with_alpha = true;
    avifRWData output = AVIF_DATA_EMPTY;

    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_NVENC_OK_OR_FEATURE_SKIP(result, diag, "10-bit");

    diag = {};
    EXPECT_EQ(testutil::ValidateAlphaOutput(output, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);
}

TEST(NvencIntegrationTest, EncodesYuv422WhenSupported)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-yuv422");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.yuv_format = AVIF_PIXEL_FORMAT_YUV422;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    if (result != AVIF_RESULT_OK &&
        (testutil::IsRuntimeSkip(diag) || testutil::DiagnosticContains(diag, "required NVENC AV1 input format") ||
         testutil::DiagnosticContains(diag, "YUV422"))) {
        GTEST_SKIP() << diag.error;
    }
    ASSERT_EQ(result, AVIF_RESULT_OK) << avifResultToString(result) << " " << diag.error;
}

TEST(NvencIntegrationTest, EncodesYuv444WhenSupported)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-yuv444");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.yuv_format = AVIF_PIXEL_FORMAT_YUV444;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    if (result != AVIF_RESULT_OK &&
        (testutil::IsRuntimeSkip(diag) || testutil::DiagnosticContains(diag, "required NVENC AV1 input format") ||
         testutil::DiagnosticContains(diag, "YUV444"))) {
        GTEST_SKIP() << diag.error;
    }
    ASSERT_EQ(result, AVIF_RESULT_OK) << avifResultToString(result) << " " << diag.error;
}

TEST(NvencIntegrationTest, ImplicitDefaultPoolEncodeWorks)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);

    avifDiagnostics diag = {};
    testutil::AppSetupGuard app_guard;
    avifResult result = avifAppsCustomCodecSetup(&diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
    app_guard.arm();

    testutil::EncodeOptions options;
    options.pool_id.clear();
    options.quality = 75;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    ASSERT_EQ(avifAppsCustomCodecShutdown(&diag), AVIF_RESULT_OK) << diag.error;
    app_guard.release();
}

TEST(NvencIntegrationTest, RejectsSecondStillImageOnSameCodecInstance)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-single-still");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    avifImage * image = avifImageCreate(256, 256, 8, AVIF_PIXEL_FORMAT_YUV420);
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(testutil::FillImage(image, /*with_alpha=*/false), AVIF_RESULT_OK);

    avifEncoder * encoder = avifEncoderCreate();
    ASSERT_NE(encoder, nullptr);
    encoder->quality = 80;
    encoder->maxThreads = 1;
    ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder, AVIF_NVENC_OPTION_POOL, pool_id.c_str()), AVIF_RESULT_OK) << encoder->diag.error;

    avifCodec * codec = create_codec();
    ASSERT_NE(codec, nullptr);
    codec->diag = &diag;

    avifCodecEncodeOutput * output = avifCodecEncodeOutputCreate();
    ASSERT_NE(output, nullptr);

    result = codec->encodeImage(codec,
                                encoder,
                                image,
                                AVIF_FALSE,
                                /*tileRowsLog2=*/0,
                                /*tileColsLog2=*/0,
                                /*quality=*/80,
                                /*encoderChanges=*/0,
                                AVIF_TRUE,
                                AVIF_ADD_IMAGE_FLAG_SINGLE,
                                output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    diag = {};
    result = codec->encodeImage(codec,
                                encoder,
                                image,
                                AVIF_FALSE,
                                /*tileRowsLog2=*/0,
                                /*tileColsLog2=*/0,
                                /*quality=*/80,
                                /*encoderChanges=*/0,
                                AVIF_TRUE,
                                AVIF_ADD_IMAGE_FLAG_SINGLE,
                                output);
    EXPECT_EQ(result, AVIF_RESULT_NOT_IMPLEMENTED);
    EXPECT_TRUE(testutil::DiagnosticContains(diag, "one still image per codec instance"));

    avifCodecEncodeOutputDestroy(output);
    codec->destroyInternal(codec);
    avifFree(codec);
    avifEncoderDestroy(encoder);
    avifImageDestroy(image);
}

#undef ASSERT_NVENC_OK_OR_SKIP
#undef ASSERT_NVENC_OK_OR_FEATURE_SKIP

} // namespace
} // namespace avif_nvenc
