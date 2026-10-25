#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <vector>

#include "avif/avif.h"
#include "avif_nvenc_codec.h"
#include "gtest_helpers.h"
#include "obu_rewrite.h"
#include "runtime.h"
#include "gtest/gtest.h"

namespace avif_nvenc
{
namespace
{
#define ASSERT_POOL_OK(expression)                                     \
    do {                                                               \
        const auto result = (expression);                              \
        if (result != AVIF_RESULT_OK && testutil::IsRuntimeSkip(diag)) \
            GTEST_SKIP() << diag.error;                                \
        ASSERT_EQ(result, AVIF_RESULT_OK) << diag.error;               \
    } while (false)

using Encoder = std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)>;
using Image = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;

Encoder MakeEncoder(const std::string & pool, int layers)
{
    Encoder e(avifEncoderCreate(), avifEncoderDestroy);
    e->codecChoice = avifNvencCodecChoice();
    e->extraLayerCount = layers - 1;
    e->width = 1024;
    e->height = 1024;
    e->speed = 6;
    EXPECT_EQ(avifEncoderSetCodecSpecificOption(e.get(), "pool", pool.c_str()), AVIF_RESULT_OK);
    return e;
}

Image MakeImage(unsigned width, unsigned height, unsigned depth, bool alpha)
{
    Image image(avifImageCreate(width, height, depth, AVIF_PIXEL_FORMAT_YUV420), avifImageDestroy);
    image->yuvRange = AVIF_RANGE_FULL;
    EXPECT_EQ(testutil::FillImage(image.get(), alpha), AVIF_RESULT_OK);
    return image;
}

std::vector<uint8_t> Pixels(const avifImage * image)
{
    std::vector<uint8_t> pixels;
    const size_t bytes = image->depth > 8 ? 2 : 1;
    for (int c = 0; c < 4; ++c) {
        const auto * plane = c == 3 ? image->alphaPlane : image->yuvPlanes[c];
        if (!plane) {
            continue;
        }
        const unsigned width = (c == 1 || c == 2) ? (image->width + 1) / 2 : image->width;
        const unsigned height = (c == 1 || c == 2) ? (image->height + 1) / 2 : image->height;
        const size_t stride = c == 3 ? image->alphaRowBytes : image->yuvRowBytes[c];
        for (unsigned y = 0; y < height; ++y) {
            pixels.insert(pixels.end(), plane + y * stride, plane + y * stride + width * bytes);
        }
    }
    return pixels;
}

struct Prefix
{
    const avifRWData & data;
    size_t available;

    static avifResult Read(avifIO * io, uint32_t flags, uint64_t offset, size_t size, avifROData * result)
    {
        auto & p = *static_cast<Prefix *>(io->data);
        if (flags || offset > p.data.size) {
            return AVIF_RESULT_IO_ERROR;
        }
        size = std::min(size, p.data.size - size_t(offset));
        if (offset + size > p.available) {
            return AVIF_RESULT_WAITING_ON_IO;
        }
        result->data = p.data.data + offset;
        result->size = size;
        return AVIF_RESULT_OK;
    }
};

void CheckDecode(const avifRWData & bytes, int layers, bool alpha)
{
    for (auto codec : { AVIF_CODEC_CHOICE_DAV1D, AVIF_CODEC_CHOICE_AOM }) {
        if (!avifCodecName(codec, AVIF_CODEC_FLAG_CAN_DECODE)) {
            continue;
        }
        SCOPED_TRACE(avifCodecName(codec, AVIF_CODEC_FLAG_CAN_DECODE));
        Prefix source { bytes, bytes.size };
        avifIO io {};
        Decoder decoder(avifDecoderCreate(), avifDecoderDestroy);
        decoder->codecChoice = codec;
        decoder->maxThreads = 1;
        decoder->allowProgressive = AVIF_TRUE;
        io.data = &source;
        io.read = Prefix::Read;
        io.persistent = AVIF_TRUE;
        io.sizeHint = bytes.size;
        avifDecoderSetIO(decoder.get(), &io);
        ASSERT_EQ(avifDecoderParse(decoder.get()), AVIF_RESULT_OK) << decoder->diag.error;
        ASSERT_EQ(decoder->progressiveState, AVIF_PROGRESSIVE_STATE_ACTIVE);
        ASSERT_EQ(decoder->imageCount, layers);
        std::vector<uint8_t> last;
        for (int i = 0; i < layers; ++i) {
            avifExtent extent {};
            ASSERT_EQ(avifDecoderNthImageMaxExtent(decoder.get(), i, &extent), AVIF_RESULT_OK);
            source.available = extent.offset + extent.size;
            if (i + 1 < layers) {
                EXPECT_LT(source.available, bytes.size);
            }
            ASSERT_EQ(avifDecoderNextImage(decoder.get()), AVIF_RESULT_OK) << decoder->diag.error;
            EXPECT_EQ(decoder->image->width, 1024u);
            EXPECT_EQ(decoder->image->height, 1024u);
            EXPECT_EQ(decoder->image->alphaPlane != nullptr, alpha);
            last = Pixels(decoder->image);
            if (i + 1 < layers) {
                EXPECT_EQ(avifDecoderNextImage(decoder.get()), AVIF_RESULT_WAITING_ON_IO);
            }
        }
        Decoder ordinary(avifDecoderCreate(), avifDecoderDestroy);
        ordinary->codecChoice = codec;
        ordinary->maxThreads = 1;
        ASSERT_EQ(avifDecoderSetIOMemory(ordinary.get(), bytes.data, bytes.size), AVIF_RESULT_OK);
        ASSERT_EQ(avifDecoderParse(ordinary.get()), AVIF_RESULT_OK);
        ASSERT_EQ(avifDecoderNextImage(ordinary.get()), AVIF_RESULT_OK) << ordinary->diag.error;
        EXPECT_EQ(Pixels(ordinary->image), last);
    }
}

class NvencLayeredTest : public testing::TestWithParam<std::tuple<int, int, bool, bool, int>>
{
};

TEST_P(NvencLayeredTest, RoundtripAndStreaming)
{
    const auto [layers, depth, alpha, grain, mode] = GetParam();
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    avifNvencImageSize buckets[] = { { 768, 768 }, { 1280, 1280 } };
    avifNvencPoolConfig config {};
    config.id = pool.c_str();
    config.maxSessions = 1;
    config.mode = static_cast<avifNvencPoolMode>(mode);
    if (mode == AVIF_NVENC_POOL_MODE_OMNI) {
        config.maxSize = { 1280, 1280 };
    }
    if (mode == AVIF_NVENC_POOL_MODE_BUCKET) {
        config.bucketSizes = buckets;
        config.bucketSizeCount = 2;
    }
    const auto result = avifNvencCreatePool(&config, &diag);
    if (result != AVIF_RESULT_OK && testutil::IsRuntimeSkip(diag)) {
        GTEST_SKIP() << diag.error;
    }
    ASSERT_EQ(result, AVIF_RESULT_OK) << diag.error;
    // Reuse the same session and verify that grain seeds and output do not
    // depend on what the previous batch left in the reference buffers.
    std::vector<uint8_t> first;
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto encoder = MakeEncoder(pool, layers);
        if (grain) {
            ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "film-grain-dither-strength", "8"), AVIF_RESULT_OK);
        }
        for (int i = 0; i < layers; ++i) {
            const unsigned width = i + 1 == layers ? 1024 : 256 * (i + 1);
            auto image = MakeImage(width, width, depth, alpha);
            encoder->quality = 35 + 15 * i;
            encoder->qualityAlpha = 60 + 8 * i;
            ASSERT_EQ(avifEncoderAddImage(encoder.get(), image.get(), 1, AVIF_ADD_IMAGE_FLAG_NONE), AVIF_RESULT_OK)
                << encoder->diag.error;
        }
        avifRWData output = AVIF_DATA_EMPTY;
        ASSERT_EQ(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK) << encoder->diag.error;
        CheckDecode(output, layers, alpha);
        const std::vector<uint8_t> current(output.data, output.data + output.size);
        if (!repeat) {
            first = current;
        } else {
            EXPECT_EQ(first, current);
        }
        avifRWDataFree(&output);
    }
}

INSTANTIATE_TEST_SUITE_P(
    All,
    NvencLayeredTest,
    testing::Combine(testing::Values(2, 3, 4), testing::Values(8, 10), testing::Bool(), testing::Bool(), testing::Values(0, 1, 2)));

TEST(NvencLayeredSchedulingTest, LaterReadyImageFinishesBeforeOlderIncompleteImage)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-ready");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto old = MakeEncoder(pool, 4);
    auto later = MakeEncoder(pool, 2);
    auto small = MakeImage(256, 256, 8, false);
    auto full = MakeImage(1024, 1024, 8, false);
    ASSERT_EQ(avifEncoderAddImage(old.get(), small.get(), 1, 0), AVIF_RESULT_OK) << old->diag.error;
    ASSERT_EQ(avifEncoderAddImage(later.get(), small.get(), 1, 0), AVIF_RESULT_OK) << later->diag.error;
    ASSERT_EQ(avifEncoderAddImage(later.get(), full.get(), 1, 0), AVIF_RESULT_OK) << later->diag.error;
    auto ready = std::async(std::launch::async, [&] {
        avifRWData out = AVIF_DATA_EMPTY;
        auto r = avifEncoderFinish(later.get(), &out);
        avifRWDataFree(&out);
        return r;
    });
    if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        old.reset(); // Cancel the old image so a regression cannot hang the test process.
        FAIL() << "Ready batch was blocked by an older incomplete image";
    }
    EXPECT_EQ(ready.get(), AVIF_RESULT_OK) << later->diag.error;
    old.reset();
}

TEST(NvencLayeredSchedulingTest, RejectsScalingAndIncompleteSequences)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-invalid");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto image = MakeImage(256, 256, 8, false);
    auto encoder = MakeEncoder(pool, 2);
    encoder->scalingMode = { { 1, 2 }, { 1, 2 } };
    EXPECT_NE(avifEncoderAddImage(encoder.get(), image.get(), 1, 0), AVIF_RESULT_OK);
    encoder = MakeEncoder(pool, 2);
    ASSERT_EQ(avifEncoderAddImage(encoder.get(), image.get(), 1, 0), AVIF_RESULT_OK);
    avifRWData output = AVIF_DATA_EMPTY;
    EXPECT_NE(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK);
    avifRWDataFree(&output);
    encoder.reset();
}

TEST(NvencLayeredSchedulingTest, HighWatermarkBlocksYoungUploadsAndOldHeadCanFinish)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-watermark");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    std::vector<Encoder> encoders;
    auto small = MakeImage(256, 256, 8, false);
    // 29 first layers occupy all slots except the three needed by the head.
    for (int i = 0; i < 29; ++i) {
        auto e = MakeEncoder(pool, 4);
        ASSERT_EQ(avifEncoderAddImage(e.get(), small.get(), 1, 0), AVIF_RESULT_OK) << e->diag.error;
        encoders.push_back(std::move(e));
    }
    auto middle = MakeImage(512, 512, 8, false);
    std::promise<void> entered;
    auto upload = std::async(std::launch::async, [&] {
        entered.set_value();
        return avifEncoderAddImage(encoders.back().get(), middle.get(), 1, 0);
    });
    entered.get_future().wait();
    EXPECT_EQ(upload.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    for (unsigned size : { 512u, 768u, 1024u }) {
        auto input = MakeImage(size, size, 8, false);
        ASSERT_EQ(avifEncoderAddImage(encoders[0].get(), input.get(), 1, 0), AVIF_RESULT_OK) << encoders[0]->diag.error;
    }
    const auto status = upload.wait_for(std::chrono::seconds(10));
    if (status != std::future_status::ready) {
        for (size_t i = 0; i + 1 < encoders.size(); ++i) {
            encoders[i].reset();
        }
        FAIL() << "Head completion did not wake the blocked upload";
    }
    EXPECT_EQ(upload.get(), AVIF_RESULT_OK);
    avifRWData output = AVIF_DATA_EMPTY;
    EXPECT_EQ(avifEncoderFinish(encoders[0].get(), &output), AVIF_RESULT_OK) << encoders[0]->diag.error;
    avifRWDataFree(&output);
    encoders.clear();
}

TEST(NvencLayeredSchedulingTest, ShutdownAllowsAdmittedContinuationsAndReleasesSessionsBeforeEncoderDestruction)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-shutdown");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto encoder = MakeEncoder(pool, 2);
    auto small = MakeImage(256, 256, 8, true);
    auto full = MakeImage(1024, 1024, 8, true);
    ASSERT_EQ(avifEncoderAddImage(encoder.get(), small.get(), 1, 0), AVIF_RESULT_OK) << encoder->diag.error;
    std::promise<void> entered;
    auto shutdown = std::async(std::launch::async, [&] {
        avifDiagnostics local {};
        entered.set_value();
        return avifNvencShutdownPool(pool.c_str(), &local);
    });
    entered.get_future().wait();
    EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    EXPECT_EQ(avifEncoderAddImage(encoder.get(), full.get(), 1, 0), AVIF_RESULT_OK) << encoder->diag.error;
    if (shutdown.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        encoder.reset();
        FAIL() << "Admitted batch did not release shutdown";
    }
    EXPECT_EQ(shutdown.get(), AVIF_RESULT_OK);
    guard.release();
    avifRWData output = AVIF_DATA_EMPTY;
    ASSERT_EQ(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK) << encoder->diag.error;
    CheckDecode(output, 2, true);
    avifRWDataFree(&output);
}

TEST(NvencLayeredSchedulingTest, EqualSizesRectanglesForcedKeyframesAndTableGrain)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-table");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    std::array<std::filesystem::path, 4> paths;

    struct Cleanup
    {
        std::array<std::filesystem::path, 4> & paths;

        ~Cleanup()
        {
            for (const auto & path : paths) {
                if (!path.empty()) {
                    std::filesystem::remove(path);
                }
            }
        }
    } cleanup { paths };

    for (size_t i = 0; i < paths.size(); ++i) {
        paths[i] = std::filesystem::temp_directory_path() / (pool + "-" + std::to_string(i) + ".tbl");
        std::ofstream file(paths[i]);
        file << "filmgrn1" << std::endl
             << "E 0 1000000 1 " << 12345 + i << " 1" << std::endl
             << "  p 0 6 0 8 0 1 128 192 256 128 192 256" << std::endl
             << "  sY 2  0 " << 96 + i * 16 << " 255 " << 96 + i * 16 << std::endl
             << "  sCb 0" << std::endl
             << "  sCr 0" << std::endl
             << "  cY" << std::endl
             << "  cCb 0" << std::endl
             << "  cCr 0" << std::endl;
    }
    std::vector<uint8_t> first;
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto encoder = MakeEncoder(pool, 4);
        encoder->autoTiling = AVIF_TRUE;
        encoder->speed = 0;
        ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "y-dc-qp-offset", "-7"), AVIF_RESULT_OK);
        const unsigned widths[] = { 256, 256, 768, 1024 }, heights[] = { 256, 256, 512, 1024 };
        for (int i = 0; i < 4; ++i) {
            auto image = MakeImage(widths[i], heights[i], 10, true);
            // The first two layers have identical geometry and QP, so the
            // grain change must work without reconfiguring the sequence.
            encoder->quality = 70;
            ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "film-grain-table", paths[i].string().c_str()), AVIF_RESULT_OK);
            ASSERT_EQ(avifEncoderAddImage(encoder.get(), image.get(), 1, i == 2 ? AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME : 0), AVIF_RESULT_OK)
                << encoder->diag.error;
        }
        avifRWData output = AVIF_DATA_EMPTY;
        ASSERT_EQ(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK) << encoder->diag.error;
        CheckDecode(output, 4, true);
        std::vector<uint8_t> current(output.data, output.data + output.size);
        if (!repeat) {
            first = current;
        } else {
            EXPECT_EQ(first, current);
        }
        avifRWDataFree(&output);
    }
}

TEST(NvencLayeredRewriteTest, RejectsTruncatedAndUnterminatedObus)
{
    const std::vector<std::pair<uint32_t, uint32_t>> sizes { { 256, 256 }, { 1024, 1024 } };
    for (const std::vector<uint8_t> packet : { std::vector<uint8_t> { 0x0a },
                                               std::vector<uint8_t> { 0x0a, 0x80 },
                                               std::vector<uint8_t> { 0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 } }) {
        avifDiagnostics diag {};
        std::vector<std::vector<uint8_t>> output;
        EXPECT_NE(rewrite_layered_picture({ packet, packet }, sizes, 1024, 1024, { std::nullopt, std::nullopt }, &output, &diag),
                  AVIF_RESULT_OK);
    }
}

TEST(NvencLayeredSchedulingTest, HighQualityFixedSizeRefinementWithGrain)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-high-quality");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto encoder = MakeEncoder(pool, 3);
    ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "aq", "0"), AVIF_RESULT_OK);
    ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "y-dc-qp-offset", "0"), AVIF_RESULT_OK);
    ASSERT_EQ(avifEncoderSetCodecSpecificOption(encoder.get(), "film-grain-dither-strength", "8"), AVIF_RESULT_OK);
    for (int quality : { 99, 95, 99 }) {
        encoder->quality = encoder->qualityAlpha = quality;
        auto image = MakeImage(1024, 1024, 8, true);
        ASSERT_EQ(avifEncoderAddImage(encoder.get(), image.get(), 1, 0), AVIF_RESULT_OK) << encoder->diag.error;
    }
    avifRWData output = AVIF_DATA_EMPTY;
    ASSERT_EQ(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK) << encoder->diag.error;
    CheckDecode(output, 3, true);
    avifRWDataFree(&output);
}

TEST(NvencLayeredSchedulingTest, StillAndReferenceProfilesShareThePoolLimit)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-mixed");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 2, &diag));
    auto encoder = MakeEncoder(pool, 2);
    auto small = MakeImage(256, 256, 8, false);
    ASSERT_EQ(avifEncoderAddImage(encoder.get(), small.get(), 1, 0), AVIF_RESULT_OK) << encoder->diag.error;
    testutil::EncodeOptions options;
    options.pool_id = pool;
    ASSERT_EQ(testutil::EncodeWithOptions(options, &diag), AVIF_RESULT_OK) << diag.error;
    auto full = MakeImage(1024, 1024, 8, false);
    ASSERT_EQ(avifEncoderAddImage(encoder.get(), full.get(), 1, 0), AVIF_RESULT_OK) << encoder->diag.error;
    avifRWData output = AVIF_DATA_EMPTY;
    ASSERT_EQ(avifEncoderFinish(encoder.get(), &output), AVIF_RESULT_OK) << encoder->diag.error;
    CheckDecode(output, 2, false);
    avifRWDataFree(&output);
}

TEST(NvencLayeredSchedulingTest, DriverFailureCancelsStagedOwnersAndSessionCanBeReplaced)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-failed-session");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto encoder = MakeEncoder(pool, 2);
    auto small = MakeImage(256, 256, 8, false);
    auto full = MakeImage(1024, 1024, 8, false);
    session_request request;
    ASSERT_EQ(build_session_request(encoder.get(), small.get(), false, 0, 0, 60, 0, &request, &diag), AVIF_RESULT_OK);
    std::shared_ptr<layered_group> bad, pending;
    ASSERT_EQ(acquire_layered_group(request, nullptr, 2, { 1024, 1024 }, &bad, &diag), AVIF_RESULT_OK) << diag.error;
    ASSERT_EQ(acquire_layered_group(request, nullptr, 4, { 1024, 1024 }, &pending, &diag), AVIF_RESULT_OK) << diag.error;
    ASSERT_EQ(bad->acquire_upload_image(false, request, small.get(), &diag), AVIF_RESULT_OK);
    ASSERT_EQ(pending->acquire_upload_image(false, request, small.get(), &diag), AVIF_RESULT_OK);
    // Deliberately bypass the codec's input validation to cause a real driver
    // reconfiguration failure, without adding a production fault-injection API.
    auto invalid = request;
    invalid.config.encode_width = 0;
    invalid.config.encode_height = 1024;
    ASSERT_EQ(bad->acquire_upload_image(false, invalid, full.get(), &diag), AVIF_RESULT_OK);
    std::vector<nvenc_session::encoded_frame> packets;
    EXPECT_NE(bad->submit_job(false, &packets, &diag), AVIF_RESULT_OK);
    auto replacement = std::async(std::launch::async, [&] {
        std::shared_ptr<layered_group> fresh;
        avifDiagnostics local {};
        auto result = acquire_layered_group(request, nullptr, 2, { 1024, 1024 }, &fresh, &local);
        if (result != AVIF_RESULT_OK) {
            return result;
        }
        result = fresh->acquire_upload_image(false, request, small.get(), &local);
        if (result != AVIF_RESULT_OK) {
            return result;
        }
        auto final = request;
        final.config.encode_width = final.config.encode_height = 1024;
        result = fresh->acquire_upload_image(false, final, full.get(), &local);
        if (result != AVIF_RESULT_OK) {
            return result;
        }
        std::vector<nvenc_session::encoded_frame> output;
        return fresh->submit_job(false, &output, &local);
    });
    if (replacement.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        pending->cancel();
        FAIL() << "Failed session kept an incomplete owner's pin alive";
    }
    EXPECT_EQ(replacement.get(), AVIF_RESULT_OK);
    // Calls on the cancelled owner must not dereference the evicted session.
    EXPECT_NE(pending->acquire_upload_image(false, request, small.get(), &diag), AVIF_RESULT_OK);
}

TEST(NvencLayeredSchedulingTest, RejectsInsufficientDistinctSessionCapacityBeforeUploadingColor)
{
    ASSERT_EQ(avifNvencRegister(), AVIF_RESULT_OK);
    const auto pool = testutil::MakeUniquePoolId("layered-capacity");
    testutil::PoolGuard guard(pool);
    avifDiagnostics diag {};
    ASSERT_POOL_OK(testutil::CreateOnDemandPool(pool, 1, &diag));
    auto encoder = MakeEncoder(pool, 2);
    Image image(avifImageCreate(256, 256, 8, AVIF_PIXEL_FORMAT_YUV444), avifImageDestroy);
    ASSERT_EQ(testutil::FillImage(image.get(), true), AVIF_RESULT_OK);
    // This tests allocation-key admission only; no unsupported 4:4:4 encode is
    // submitted. Color and alpha need different input formats in this case.
    EXPECT_EQ(avifEncoderAddImage(encoder.get(), image.get(), 1, 0), AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION);
    EXPECT_TRUE(testutil::DiagnosticContains(encoder->diag, "distinct sessions"));
}
} // namespace
} // namespace avif_nvenc
