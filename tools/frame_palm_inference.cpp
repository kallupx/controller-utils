// Replay pairs of 128x128 grayscale crops through the palm model installed by SteamVR.
// stdin is consecutive pairs of raw images. stdout is center, uncertainty, plus_x, plus_z
// as float32 arrays of shape [2, 23, 3] for each pair, in the model's coordinates.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr,
                     "Usage: frame_palm_inference MODEL.onnx < stereo.gray > features.f32\n");
        return 2;
    }
    try {
        const auto library =
            dlopen("/opt/steamvr/drivers/cv/bin/linuxarm64/libonnxruntime_linux_arm64.so",
                   RTLD_LOCAL | RTLD_NOW);
        if (!library)
            throw std::runtime_error(dlerror());
        const auto get_api =
            reinterpret_cast<decltype(&OrtGetApiBase)>(dlsym(library, "OrtGetApiBase"));
        const auto api = get_api ? get_api()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api)
            throw std::runtime_error("SteamVR palm inference API is unavailable");
        Ort::InitApi(api);
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "frame-palm");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        Ort::Session model(env, argv[1], options);
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        constexpr std::array<int64_t, 4> inputShape{1, 2, 128, 128};
        const std::vector<int64_t> outputShape{2, 23, 3};
        std::array<unsigned char, 2 * 128 * 128> pixels;
        std::array<float, 2 * 128 * 128> normalized;
        auto tensor = Ort::Value::CreateTensor<float>(memory, normalized.data(), normalized.size(),
                                                      inputShape.data(), inputShape.size());
        const char *inputs[] = {"Image"};
        const char *outputs[] = {"center", "uncertainty", "plus_x", "plus_z"};
        for (;;) {
            const auto count = std::fread(pixels.data(), 1, pixels.size(), stdin);
            if (count == 0 && std::feof(stdin) && !std::ferror(stdin))
                break;
            if (count != pixels.size())
                throw std::runtime_error("Incomplete stereo image pair");
            std::transform(pixels.begin(), pixels.end(), normalized.begin(),
                           [](unsigned char pixel) { return pixel / 255.0f; });
            auto features = model.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 4);
            for (const auto &feature : features) {
                const auto info = feature.GetTensorTypeAndShapeInfo();
                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    info.GetShape() != outputShape)
                    throw std::runtime_error("Unexpected palm model output format");
                const auto *values = feature.GetTensorData<float>();
                const auto size = info.GetElementCount();
                if (!std::all_of(values, values + size,
                                 [](float value) { return std::isfinite(value); }))
                    throw std::runtime_error("Nonfinite palm model output");
                if (std::fwrite(values, sizeof(float), size, stdout) != size)
                    throw std::runtime_error("Could not write palm features");
            }
            if (std::fflush(stdout) != 0)
                throw std::runtime_error("Could not flush palm features");
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
