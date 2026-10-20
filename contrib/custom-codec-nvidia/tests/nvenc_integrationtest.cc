#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "avif/codec.h"
#include "avif/avif.h"
#include "codec_wrapper.h"
#include "film_grain.h"
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

TEST(NvencIntegrationTest, OutputUsesReducedStillPictureHeader)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-reduced-header");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOmniPool(pool_id, /*max_sessions=*/1, /*max_width=*/512, /*max_height=*/384, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    avifImage * image = avifImageCreate(250, 130, 8, AVIF_PIXEL_FORMAT_YUV420);
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(testutil::FillImage(image, /*with_alpha=*/false), AVIF_RESULT_OK);

    avifEncoder * encoder = avifEncoderCreate();
    ASSERT_NE(encoder, nullptr);
    encoder->maxThreads = 1;
    ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder, AVIF_NVENC_OPTION_POOL, pool_id.c_str()), AVIF_RESULT_OK) << encoder->diag.error;

    avifCodec * codec = create_codec();
    ASSERT_NE(codec, nullptr);
    codec->diag = &diag;
    avifCodecEncodeOutput * output = avifCodecEncodeOutputCreate();
    ASSERT_NE(output, nullptr);

    result = codec->encodeImage(codec, encoder, image, AVIF_FALSE, 0, 0, 80, 0, AVIF_TRUE, AVIF_ADD_IMAGE_FLAG_SINGLE, output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
    ASSERT_EQ(output->samples.count, 1u);

    // Find the sequence header OBU and check still_picture and reduced_still_picture_header.
    const avifRWData & data = output->samples.sample[0].data;
    bool found_sequence_header = false;
    for (size_t pos = 0; pos < data.size;) {
        const uint8_t header = data.data[pos++];
        const int type = (header >> 3) & 0xf;
        ASSERT_TRUE(header & 2) << "OBU without size field";
        if (header & 4) {
            ++pos; // extension header
        }
        size_t obu_size = 0;
        for (int shift = 0; pos < data.size; shift += 7) {
            const uint8_t byte = data.data[pos++];
            obu_size |= static_cast<size_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                break;
            }
        }
        ASSERT_LE(pos + obu_size, data.size);
        if (type == 1) {
            found_sequence_header = true;
            ASSERT_GT(obu_size, 0u);
            EXPECT_EQ((data.data[pos] >> 3) & 3, 3) << "still_picture and reduced_still_picture_header must both be set";
        }
        pos += obu_size;
    }
    EXPECT_TRUE(found_sequence_header);

    avifCodecEncodeOutputDestroy(output);
    codec->destroyInternal(codec);
    avifFree(codec);
    avifEncoderDestroy(encoder);
    avifImageDestroy(image);
}

namespace
{

std::string WriteTempFile(const std::string & name, const std::string & content)
{
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
}

const char kGrainEntry[] =
    "E 0 1000000 1 7391 1\n"
    "\tp 0 6 0 8 0 1 128 192 256 128 192 256\n"
    "\tsY 2  0 96 255 96\n"
    "\tsCb 0\n"
    "\tsCr 0\n"
    "\tcY\n"
    "\tcCb 0\n"
    "\tcCr 0\n";

} // namespace

TEST(NvencFilmGrainTableTest, ParsesEntryCoveringTimeZero)
{
    const std::string path = WriteTempFile("avif_nvenc_grain_parse.tbl",
                                           std::string("filmgrn1\n") +
                                               "E -5 0 1 1 1\n"
                                               "\tp 0 6 0 8 0 0 0 0 0 0 0 0\n\tsY 0 \n\tsCb 0\n\tsCr 0\n\tcY\n\tcCb 0\n\tcCr 0\n" +
                                               kGrainEntry + "E 1000000 2000000 0 1 0\n");
    std::optional<film_grain_params> params;
    uint16_t seed = 0;
    avifDiagnostics diag = {};
    ASSERT_EQ(load_film_grain_table(path, &params, &seed, &diag), AVIF_RESULT_OK) << diag.error;
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->num_y_points, 2);
    EXPECT_EQ(params->scaling_points_y[1][0], 255);
    EXPECT_EQ(params->scaling_points_y[1][1], 96);
    EXPECT_TRUE(params->overlap_flag);
    EXPECT_EQ(params->cb_offset, 256);
    EXPECT_EQ(seed, 7391);

    NV_ENC_FILM_GRAIN_PARAMS_AV1 nv;
    params->to_nvenc(&nv);
    EXPECT_EQ(nv.applyGrain, 1u);
    EXPECT_EQ(nv.numYPoints, 2u);
    EXPECT_EQ(nv.arCoeffsCbPlus128[0], 128);
    std::filesystem::remove(path);
}

TEST(NvencFilmGrainTableTest, ReloadsModifiedFileAndReportsErrors)
{
    const std::string path = WriteTempFile("avif_nvenc_grain_reload.tbl", std::string("filmgrn1\n") + kGrainEntry);
    std::optional<film_grain_params> params;
    uint16_t seed = 0;
    avifDiagnostics diag = {};
    ASSERT_EQ(load_film_grain_table(path, &params, &seed, &diag), AVIF_RESULT_OK) << diag.error;
    ASSERT_TRUE(params.has_value());

    // apply_grain == 0 for the covering entry means no film grain. The file size changes, so the cache is invalidated.
    WriteTempFile("avif_nvenc_grain_reload.tbl", "filmgrn1\nE 0 10 0 1 0\n");
    ASSERT_EQ(load_film_grain_table(path, &params, &seed, &diag), AVIF_RESULT_OK) << diag.error;
    EXPECT_FALSE(params.has_value());

    WriteTempFile("avif_nvenc_grain_reload.tbl", "filmgrn1\nE 5 1000 1 1 0\n");
    diag = {};
    EXPECT_NE(load_film_grain_table(path, &params, &seed, &diag), AVIF_RESULT_OK);
    EXPECT_TRUE(testutil::DiagnosticContains(diag, "no entry covers time 0"));

    diag = {};
    EXPECT_NE(load_film_grain_table(path + ".missing", &params, &seed, &diag), AVIF_RESULT_OK);
    std::filesystem::remove(path);
}

TEST(NvencIntegrationTest, AppliesFilmGrainTableAndSessionReturnsToNoGrain)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-grain");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    const std::string path = WriteTempFile("avif_nvenc_grain_encode.tbl", std::string("filmgrn1\n") + kGrainEntry);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.quality = 90;

    double psnr_plain = 0, psnr_grain = 0, psnr_plain_again = 0;
    avifRWData output = AVIF_DATA_EMPTY;
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);
    ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr_plain, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);

    options.film_grain_table = path;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_EQ(result, AVIF_RESULT_OK) << diag.error;
    ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr_grain, &diag), AVIF_RESULT_OK) << diag.error;

    // A second request in the same session with the same config reuses the cached table. NVENC advances its own
    // film grain seed for every frame, but the seed from the table is patched into the bitstream, so the
    // output must be identical.
    avifRWData output_again = AVIF_DATA_EMPTY;
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output_again);
    ASSERT_EQ(result, AVIF_RESULT_OK) << diag.error;
    EXPECT_EQ(output.size, output_again.size);
    EXPECT_TRUE(output.size == output_again.size && std::memcmp(output.data, output_again.data, output.size) == 0);
    avifRWDataFree(&output_again);
    avifRWDataFree(&output);

    options.film_grain_table.reset();
    diag = {};
    result = testutil::EncodeWithOptions(options, &diag, &output);
    ASSERT_EQ(result, AVIF_RESULT_OK) << diag.error;
    ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr_plain_again, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);

    EXPECT_LT(psnr_grain, psnr_plain - 3.0);
    EXPECT_NEAR(psnr_plain_again, psnr_plain, 0.5);
    std::filesystem::remove(path);
}

TEST(NvencFilmGrainPresetTest, ParametersAreValidForAllStrengths)
{
    for (const bool limited_range : { true, false }) {
        for (int strength = 1; strength <= kMaxDitherGrainStrength; ++strength) {
            SCOPED_TRACE(testing::Message() << "strength " << strength << ", limited " << limited_range);
            const film_grain_params p = make_dither_grain(strength, limited_range);
            EXPECT_EQ(p.clip_to_restricted_range, limited_range);
            ASSERT_GT(p.num_y_points, 1);
            for (int i = 1; i < p.num_y_points; ++i) {
                EXPECT_GT(p.scaling_points_y[i][0], p.scaling_points_y[i - 1][0]);
            }
            // Zero at the ends of the signal range, so black and white are not shifted by clipped noise.
            EXPECT_EQ(p.scaling_points_y[0][1], 0);
            EXPECT_EQ(p.scaling_points_y[p.num_y_points - 1][1], 0);
            if (strength == 1) {
                EXPECT_EQ(p.num_cb_points, 0);
                EXPECT_EQ(p.num_cr_points, 0);
            } else {
                EXPECT_EQ(p.num_cb_points, 2);
                EXPECT_EQ(p.num_cr_points, 2);
                EXPECT_GE(p.scaling_points_cb[0][1], 19); // 0.30 LSBs; lower values are lost to rounding
            }
            NV_ENC_FILM_GRAIN_PARAMS_AV1 nv;
            p.to_nvenc(&nv);
            EXPECT_EQ(nv.grainScalingMinus8, 3u);
            EXPECT_EQ(nv.arCoeffLag, 0u);
        }
    }
    // Strength increases the luma noise.
    EXPECT_LT(make_dither_grain(1, true).scaling_points_y[2][1], make_dither_grain(8, true).scaling_points_y[2][1]);
}

TEST(NvencIntegrationTest, FilmGrainDitherStrengthOptionIsValidated)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-dither-option");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.film_grain_dither_strength = "25";
    diag = {};
    EXPECT_EQ(testutil::EncodeWithOptions(options, &diag), AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION);
    EXPECT_TRUE(testutil::DiagnosticContains(diag, "film-grain-dither-strength")) << diag.error;

    options.film_grain_dither_strength = "2";
    options.film_grain_table = "table-that-is-never-read.tbl";
    diag = {};
    EXPECT_EQ(testutil::EncodeWithOptions(options, &diag), AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION);
    EXPECT_TRUE(testutil::DiagnosticContains(diag, "cannot be combined")) << diag.error;
}

TEST(NvencIntegrationTest, EncodesWithFilmGrainDither)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const std::string pool_id = testutil::MakeUniquePoolId("nvenc-dither");
    testutil::PoolGuard pool_guard(pool_id);

    avifDiagnostics diag = {};
    avifResult result = testutil::CreateOnDemandPool(pool_id, /*max_sessions=*/1, &diag);
    ASSERT_NVENC_OK_OR_SKIP(result, diag);

    testutil::EncodeOptions options;
    options.pool_id = pool_id;
    options.quality = 90;
    double psnr_plain = 0;
    avifRWData output = AVIF_DATA_EMPTY;
    ASSERT_NVENC_OK_OR_SKIP(testutil::EncodeWithOptions(options, &diag, &output), diag);
    ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr_plain, &diag), AVIF_RESULT_OK) << diag.error;
    avifRWDataFree(&output);

    for (const char * strength : { "1", "4", "8", "24" }) {
        options.film_grain_dither_strength = strength;
        diag = {};
        ASSERT_EQ(testutil::EncodeWithOptions(options, &diag, &output), AVIF_RESULT_OK) << diag.error;
        double psnr = 0;
        ASSERT_EQ(testutil::GetDecodedImagePsnr(output, options, &psnr, &diag), AVIF_RESULT_OK) << diag.error;
        avifRWDataFree(&output);
        // The grain is noise: the decoded image gets worse, but must stay recognizable.
        EXPECT_LT(psnr, psnr_plain) << "strength " << strength;
        EXPECT_GT(psnr, 25.0) << "strength " << strength;
    }
}

#undef ASSERT_NVENC_OK_OR_SKIP
#undef ASSERT_NVENC_OK_OR_FEATURE_SKIP

} // namespace
} // namespace avif_nvenc
