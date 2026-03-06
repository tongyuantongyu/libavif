#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "argparse.hpp"
#include "avif/avif.h"
#include "avif_nvenc_codec.h"
#include "avifutil.h"
#include "gtest_helpers.h"

namespace avif_nvenc
{
namespace
{

using Clock = std::chrono::steady_clock;
using ImagePtr = std::unique_ptr<avifImage, void (*)(avifImage *)>;

struct BenchmarkConfig
{
    std::optional<std::string> input_filename;
    avifAppFileFormat input_format = AVIF_APP_FILE_FORMAT_UNKNOWN;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t depth = 8;
    avifPixelFormat yuv_format = AVIF_PIXEL_FORMAT_YUV420;
    bool has_alpha = false;
    int quality = 80;
    int speed = AVIF_SPEED_DEFAULT;
    uint32_t max_sessions = 1;
    uint32_t submit_concurrency = 1;
    uint32_t iterations = 100;
    uint32_t warmup_iterations = 1;
    std::optional<std::string> preset;
    std::optional<std::string> tuning;
};

struct PhaseStats
{
    uint64_t completed = 0;
    uint64_t total_output_bytes = 0;
    double total_latency_ms = 0.0;
    double min_latency_ms = std::numeric_limits<double>::infinity();
    double max_latency_ms = 0.0;

    void merge(const PhaseStats & other)
    {
        completed += other.completed;
        total_output_bytes += other.total_output_bytes;
        total_latency_ms += other.total_latency_ms;
        min_latency_ms = std::min(min_latency_ms, other.min_latency_ms);
        max_latency_ms = std::max(max_latency_ms, other.max_latency_ms);
    }
};

struct WorkerResult
{
    avifResult result = AVIF_RESULT_OK;
    avifDiagnostics diag = {};
    PhaseStats stats = {};
};

struct PhaseResult
{
    avifResult result = AVIF_RESULT_OK;
    avifDiagnostics diag = {};
    PhaseStats stats = {};
    double wall_seconds = 0.0;
};

class StartGate
{
public:
    explicit StartGate(uint32_t participant_count) : participant_count_(participant_count) {}

    void wait()
    {
        std::unique_lock lock(mutex_);
        ++arrived_;
        if (arrived_ == participant_count_) {
            start_time_ = Clock::now();
            started_ = true;
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this] { return started_; });
    }

    Clock::time_point start_time() const noexcept { return start_time_; }

private:
    const uint32_t participant_count_;
    uint32_t arrived_ = 0;
    bool started_ = false;
    Clock::time_point start_time_ = {};
    std::mutex mutex_;
    std::condition_variable condition_;
};

ImagePtr MakeImagePtr(avifImage * image = nullptr)
{
    return ImagePtr(image, &avifImageDestroy);
}

void PrintFailure(std::string_view context, avifResult result, const avifDiagnostics * diag)
{
    std::cerr << context << " failed: " << avifResultToString(result);
    if (diag && diag->error[0] != '\0') {
        std::cerr << " (" << diag->error << ")";
    }
    std::cerr << "\n";
}

bool ValidatePositive(int value, std::string_view name)
{
    if (value > 0) {
        return true;
    }
    std::cerr << "Invalid " << name << ": expected a positive integer\n";
    return false;
}

bool ValidateNonNegative(int value, std::string_view name)
{
    if (value >= 0) {
        return true;
    }
    std::cerr << "Invalid " << name << ": expected a non-negative integer\n";
    return false;
}

bool ValidateRange(int value, int min_value, int max_value, std::string_view name)
{
    if (value >= min_value && value <= max_value) {
        return true;
    }
    std::cerr << "Invalid " << name << ": expected a value in [" << min_value << ", " << max_value << "]\n";
    return false;
}

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

avifResult CreateBenchmarkImage(const BenchmarkConfig & config, ImagePtr * image, avifDiagnostics * diag)
{
    *image = MakeImagePtr(avifImageCreate(config.width, config.height, config.depth, AVIF_PIXEL_FORMAT_YUV420));
    if (!*image) {
        testutil::SetDiagMessage(diag, "Failed to allocate benchmark image");
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    (*image)->yuvRange = AVIF_RANGE_FULL;
    (*image)->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    (*image)->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    (*image)->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;

    const avifResult fill_result = testutil::FillImage(image->get(), /*with_alpha=*/false);
    if (fill_result != AVIF_RESULT_OK) {
        image->reset();
        if (diag && diag->error[0] == '\0') {
            testutil::SetDiagMessage(diag, "Failed to fill benchmark image");
        }
        return fill_result;
    }
    return AVIF_RESULT_OK;
}

avifResult CloneBenchmarkImage(const avifImage * prototype, ImagePtr * image, avifDiagnostics * diag)
{
    *image = MakeImagePtr(avifImageCreateEmpty());
    if (!*image) {
        testutil::SetDiagMessage(diag, "Failed to allocate cloned benchmark image");
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    const avifResult result = avifImageCopy(image->get(), prototype, AVIF_PLANES_ALL);
    if (result != AVIF_RESULT_OK) {
        image->reset();
        if (diag && diag->error[0] == '\0') {
            testutil::SetDiagMessage(diag, "Failed to clone benchmark image");
        }
        return result;
    }
    return AVIF_RESULT_OK;
}

avifResult CreateBenchmarkSource(BenchmarkConfig * config, ImagePtr * image, avifDiagnostics * diag)
{
    avifResult result = AVIF_RESULT_OK;
    if (config->input_filename) {
        *image = MakeImagePtr(avifImageCreateEmpty());
        if (!*image) {
            testutil::SetDiagMessage(diag, "Failed to allocate decoded benchmark image");
            return AVIF_RESULT_OUT_OF_MEMORY;
        }

        config->input_format = avifReadImage(config->input_filename->c_str(),
                                             AVIF_APP_FILE_FORMAT_UNKNOWN,
                                             AVIF_PIXEL_FORMAT_YUV420,
                                             static_cast<int>(config->depth),
                                             AVIF_CHROMA_DOWNSAMPLING_AUTOMATIC,
                                             AVIF_FALSE,
                                             AVIF_FALSE,
                                             AVIF_FALSE,
                                             AVIF_TRUE,
                                             AVIF_DEFAULT_IMAGE_SIZE_LIMIT,
                                             image->get(),
                                             nullptr,
                                             nullptr,
                                             nullptr);
        if (config->input_format == AVIF_APP_FILE_FORMAT_UNKNOWN) {
            image->reset();
            const std::string message = "Failed to decode input image: " + *config->input_filename;
            testutil::SetDiagMessage(diag, message);
            return AVIF_RESULT_INVALID_ARGUMENT;
        }
    } else {
        result = CreateBenchmarkImage(*config, image, diag);
        if (result != AVIF_RESULT_OK) {
            return result;
        }
    }

    config->width = (*image)->width;
    config->height = (*image)->height;
    config->depth = (*image)->depth;
    config->yuv_format = (*image)->yuvFormat;
    config->has_alpha = ((*image)->alphaPlane != nullptr);
    return AVIF_RESULT_OK;
}

avifResult EncodeOnce(const BenchmarkConfig & config,
                      const std::string & pool_id,
                      avifImage * image,
                      size_t * output_size,
                      avifDiagnostics * diag)
{
    if (output_size) {
        *output_size = 0;
    }

    avifEncoder * encoder = avifEncoderCreate();
    if (!encoder) {
        testutil::SetDiagMessage(diag, "Failed to allocate avifEncoder");
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    encoder->codecChoice = avifNvencCodecChoice();
    encoder->quality = config.quality;
    encoder->qualityAlpha = config.quality;
    encoder->speed = config.speed;
    encoder->maxThreads = 1;

    avifResult result = avifEncoderSetCodecSpecificOption(encoder, AVIF_NVENC_OPTION_POOL, pool_id.c_str());
    *diag = encoder->diag;
    if (result == AVIF_RESULT_OK) {
        result = SetCodecOption(encoder, AVIF_NVENC_OPTION_PRESET, config.preset, diag);
    }
    if (result == AVIF_RESULT_OK) {
        result = SetCodecOption(encoder, AVIF_NVENC_OPTION_TUNING, config.tuning, diag);
    }

    avifRWData output = AVIF_DATA_EMPTY;
    if (result == AVIF_RESULT_OK) {
        result = avifEncoderWrite(encoder, image, &output);
        *diag = encoder->diag;
        if (output_size) {
            *output_size = output.size;
        }
    }

    avifRWDataFree(&output);
    avifEncoderDestroy(encoder);
    return result;
}

PhaseResult RunPhase(const BenchmarkConfig & config,
                     const avifImage * prototype_image,
                     const std::string & pool_id,
                     uint32_t phase_iterations)
{
    PhaseResult phase_result = {};
    if (phase_iterations == 0) {
        return phase_result;
    }

    std::vector<ImagePtr> images;
    images.reserve(config.submit_concurrency);
    for (uint32_t i = 0; i < config.submit_concurrency; ++i) {
        avifDiagnostics diag = {};
        ImagePtr image = MakeImagePtr();
        const avifResult result = CloneBenchmarkImage(prototype_image, &image, &diag);
        if (result != AVIF_RESULT_OK) {
            phase_result.result = result;
            phase_result.diag = diag;
            return phase_result;
        }
        images.push_back(std::move(image));
    }

    std::atomic<uint32_t> next_job = 0;
    std::atomic<bool> stop_requested = false;
    std::vector<WorkerResult> worker_results(config.submit_concurrency);
    std::vector<std::thread> workers;
    workers.reserve(config.submit_concurrency);
    StartGate start_gate(config.submit_concurrency);

    auto worker = [&](uint32_t worker_index) {
        start_gate.wait();
        while (!stop_requested.load()) {
            const uint32_t job_index = next_job.fetch_add(1);
            if (job_index >= phase_iterations) {
                break;
            }

            size_t output_size = 0;
            avifDiagnostics diag = {};
            const Clock::time_point begin = Clock::now();
            const avifResult result = EncodeOnce(config, pool_id, images[worker_index].get(), &output_size, &diag);
            const Clock::time_point end = Clock::now();
            if (result != AVIF_RESULT_OK) {
                worker_results[worker_index].result = result;
                worker_results[worker_index].diag = diag;
                stop_requested = true;
                break;
            }

            const double latency_ms = std::chrono::duration<double, std::milli>(end - begin).count();
            PhaseStats & stats = worker_results[worker_index].stats;
            ++stats.completed;
            stats.total_output_bytes += output_size;
            stats.total_latency_ms += latency_ms;
            stats.min_latency_ms = std::min(stats.min_latency_ms, latency_ms);
            stats.max_latency_ms = std::max(stats.max_latency_ms, latency_ms);
        }
    };

    for (uint32_t i = 0; i < config.submit_concurrency; ++i) {
        workers.emplace_back(worker, i);
    }

    for (std::thread & thread : workers) {
        thread.join();
    }

    phase_result.wall_seconds = std::chrono::duration<double>(Clock::now() - start_gate.start_time()).count();
    for (const WorkerResult & worker_result : worker_results) {
        if (worker_result.result != AVIF_RESULT_OK) {
            phase_result.result = worker_result.result;
            phase_result.diag = worker_result.diag;
            return phase_result;
        }
        phase_result.stats.merge(worker_result.stats);
    }
    return phase_result;
}

void PrintPhaseSummary(std::string_view phase_name, const BenchmarkConfig & config, const PhaseResult & result)
{
    const double wall_seconds = std::max(result.wall_seconds, std::numeric_limits<double>::min());
    const double encodes_per_second = result.stats.completed / wall_seconds;
    const double megapixels_per_second =
        (static_cast<double>(config.width) * config.height * result.stats.completed) / (1'000'000.0 * wall_seconds);
    const double average_latency_ms =
        (result.stats.completed > 0) ? (result.stats.total_latency_ms / result.stats.completed) : 0.0;
    const double average_output_bytes =
        (result.stats.completed > 0) ? (static_cast<double>(result.stats.total_output_bytes) / result.stats.completed) : 0.0;

    std::cout << phase_name << ":\n";
    std::cout << "  encodes: " << result.stats.completed << "\n";
    std::cout << "  wall time: " << std::fixed << std::setprecision(3) << result.wall_seconds << " s\n";
    std::cout << "  throughput: " << std::setprecision(2) << encodes_per_second << " fps, " << megapixels_per_second
              << " MPix/s\n";
    std::cout << "  latency: avg " << average_latency_ms << " ms, min " << result.stats.min_latency_ms << " ms, max "
              << result.stats.max_latency_ms << " ms\n";
    std::cout << "  average output size: " << average_output_bytes << " bytes\n";
}

BenchmarkConfig ParseCommandLine(int argc, const char * const argv[])
{
    argparse::ArgumentParser parser("custom-codec-nvenc-benchmark",
                                    "Benchmark NVENC custom codec still-image encode throughput.");

    argparse::ArgValue<int> arg_width;
    argparse::ArgValue<int> arg_height;
    argparse::ArgValue<int> arg_depth;
    argparse::ArgValue<int> arg_quality;
    argparse::ArgValue<int> arg_speed;
    argparse::ArgValue<int> arg_max_sessions;
    argparse::ArgValue<int> arg_submit_concurrency;
    argparse::ArgValue<int> arg_iterations;
    argparse::ArgValue<int> arg_warmup;
    argparse::ArgValue<std::string> arg_input;
    argparse::ArgValue<std::string> arg_preset;
    argparse::ArgValue<std::string> arg_tuning;

    parser.add_argument(arg_width, "--width")
        .help("Synthetic benchmark image width when --input is omitted")
        .default_value("1920");
    parser.add_argument(arg_height, "--height")
        .help("Synthetic benchmark image height when --input is omitted")
        .default_value("1080");
    parser.add_argument(arg_depth, "--depth", "-d")
        .help("Synthetic image depth, or requested decode depth for input PNG/JPEG")
        .choices({ "8", "10" })
        .default_value("8");
    parser.add_argument(arg_quality, "--quality", "-q")
        .help("Color quality (0-100)")
        .default_value("80");
    parser.add_argument(arg_speed, "--speed", "-s")
        .help("Generic avifEncoder speed (-1 for codec default, otherwise 0-10)")
        .default_value("-1");
    parser.add_argument(arg_max_sessions, "--max-sessions")
        .help("Pool-wide NVENC session budget")
        .default_value("1");
    parser.add_argument(arg_submit_concurrency, "--submit-concurrency", "-j")
        .help("Number of simultaneous encode submissions")
        .default_value("1");
    parser.add_argument(arg_iterations, "--iterations", "-n")
        .help("Number of timed encodes")
        .default_value("100");
    parser.add_argument(arg_warmup, "--warmup")
        .help("Number of warmup encodes before the timed phase (default: submit-concurrency)")
        .default_value("-1");
    parser.add_argument(arg_input, "--input", "-i")
        .help("Optional input image to decode with avif_apps (JPEG, PNG, or Y4M)");
    parser.add_argument(arg_preset, "--preset")
        .help("Optional NVENC preset override such as P4 or P7");
    parser.add_argument(arg_tuning, "--tuning")
        .help("Optional NVENC tuning override such as HIGH_QUALITY");

    parser.parse_args(argc, argv);

    if (!ValidatePositive(arg_width.value(), "--width") || !ValidatePositive(arg_height.value(), "--height") ||
        !ValidatePositive(arg_max_sessions.value(), "--max-sessions") ||
        !ValidatePositive(arg_submit_concurrency.value(), "--submit-concurrency") ||
        !ValidatePositive(arg_iterations.value(), "--iterations") || !ValidateRange(arg_quality.value(), 0, 100, "--quality") ||
        !ValidateRange(arg_speed.value(), AVIF_SPEED_DEFAULT, 10, "--speed") ||
        (arg_warmup.provenance() == argparse::Provenance::SPECIFIED &&
         !ValidateNonNegative(arg_warmup.value(), "--warmup"))) {
        std::exit(1);
    }

    BenchmarkConfig config;
    if (arg_input.provenance() == argparse::Provenance::SPECIFIED) {
        config.input_filename = arg_input.value();
    }
    config.width = static_cast<uint32_t>(arg_width.value());
    config.height = static_cast<uint32_t>(arg_height.value());
    config.depth = static_cast<uint32_t>(arg_depth.value());
    config.quality = arg_quality.value();
    config.speed = arg_speed.value();
    config.max_sessions = static_cast<uint32_t>(arg_max_sessions.value());
    config.submit_concurrency = static_cast<uint32_t>(arg_submit_concurrency.value());
    config.iterations = static_cast<uint32_t>(arg_iterations.value());
    config.warmup_iterations =
        (arg_warmup.provenance() == argparse::Provenance::SPECIFIED) ? static_cast<uint32_t>(arg_warmup.value()) :
                                                                       config.submit_concurrency;
    if (arg_preset.provenance() == argparse::Provenance::SPECIFIED) {
        config.preset = arg_preset.value();
    }
    if (arg_tuning.provenance() == argparse::Provenance::SPECIFIED) {
        config.tuning = arg_tuning.value();
    }
    return config;
}

void PrintConfiguration(const BenchmarkConfig & config)
{
    std::cout << "custom-codec-nvenc benchmark\n";
    std::cout << "  nvenc version: " << avifNvencVersion() << "\n";
    if (config.input_filename) {
        std::cout << "  input: " << *config.input_filename << " (" << avifFileFormatToString(config.input_format) << ")\n";
    } else {
        std::cout << "  input: synthetic gradient\n";
    }
    std::cout << "  frame: " << config.width << "x" << config.height << " depth=" << config.depth
              << " yuv=" << avifPixelFormatToString(config.yuv_format);
    if (config.has_alpha) {
        std::cout << " alpha=present";
    }
    std::cout << "\n";
    std::cout << "  quality: " << config.quality << "\n";
    if (config.speed == AVIF_SPEED_DEFAULT) {
        std::cout << "  speed: default\n";
    } else {
        std::cout << "  speed: " << config.speed << "\n";
    }
    std::cout << "  max sessions: " << config.max_sessions << "\n";
    std::cout << "  submit concurrency: " << config.submit_concurrency << "\n";
    std::cout << "  timed encodes: " << config.iterations << "\n";
    std::cout << "  warmup encodes: " << config.warmup_iterations << "\n";
    if (config.preset) {
        std::cout << "  preset override: " << *config.preset << "\n";
    }
    if (config.tuning) {
        std::cout << "  tuning override: " << *config.tuning << "\n";
    }
    std::cout << "\n";
}

} // namespace
} // namespace avif_nvenc

int main(int argc, const char * const argv[])
{
    avif_nvenc::BenchmarkConfig config = avif_nvenc::ParseCommandLine(argc, argv);

    avif_nvenc::ImagePtr source_image = avif_nvenc::MakeImagePtr();
    avifDiagnostics diag = {};
    avifResult result = avif_nvenc::CreateBenchmarkSource(&config, &source_image, &diag);
    if (result != AVIF_RESULT_OK) {
        avif_nvenc::PrintFailure("benchmark input setup", result, &diag);
        return 1;
    }

    avif_nvenc::PrintConfiguration(config);

    const avifResult register_result = avifNvencRegister();
    if (register_result != AVIF_RESULT_OK) {
        std::cerr << "avifNvencRegister failed: " << avifResultToString(register_result) << "\n";
        return 1;
    }

    const std::string pool_id = avif_nvenc::testutil::MakeUniquePoolId("nvenc-bench");
    avif_nvenc::testutil::PoolGuard pool_guard(pool_id);

    diag = {};
    result = avif_nvenc::testutil::CreateOnDemandPool(pool_id, config.max_sessions, &diag);
    if (result != AVIF_RESULT_OK) {
        avif_nvenc::PrintFailure("avifNvencCreatePool", result, &diag);
        return 1;
    }

    const avif_nvenc::PhaseResult warmup_result =
        avif_nvenc::RunPhase(config, source_image.get(), pool_id, config.warmup_iterations);
    if (warmup_result.result != AVIF_RESULT_OK) {
        avif_nvenc::PrintFailure("warmup phase", warmup_result.result, &warmup_result.diag);
        return 1;
    }

    const avif_nvenc::PhaseResult measured_result =
        avif_nvenc::RunPhase(config, source_image.get(), pool_id, config.iterations);
    if (measured_result.result != AVIF_RESULT_OK) {
        avif_nvenc::PrintFailure("timed phase", measured_result.result, &measured_result.diag);
        return 1;
    }

    if (config.warmup_iterations > 0) {
        avif_nvenc::PrintPhaseSummary("warmup", config, warmup_result);
        std::cout << "\n";
    }
    avif_nvenc::PrintPhaseSummary("timed", config, measured_result);

    diag = {};
    result = avifNvencShutdownPool(pool_id.c_str(), &diag);
    if (result != AVIF_RESULT_OK) {
        avif_nvenc::PrintFailure("avifNvencShutdownPool", result, &diag);
        return 1;
    }
    pool_guard.release();
    return 0;
}
