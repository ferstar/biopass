#include "onnx_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <syslog.h>

#include "user_file.h"

#ifdef BIOPASS_VITISAI
#include "npu_config.h"
#endif

namespace biopass {
namespace {
std::string setting(const char* name, const char* fallback = "") {
#ifdef BIOPASS_VITISAI
  return npu::setting(name, fallback);
#else
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
#endif
}

int cpuThreads() {
  try {
    int threads = std::stoi(setting("BIOPASS_CPU_THREADS", "4"));
    if (threads > 0 && threads <= 256) return threads;
  } catch (const std::exception&) {}
  return 4;
}

#ifdef BIOPASS_VITISAI
void npuLog(const std::string& message) {
  syslog(LOG_NOTICE, "biopass: %s", message.c_str());
  if (setting("BIOPASS_NPU_LOG_STDERR") == "1")
    std::fprintf(stderr, "biopass: %s\n", message.c_str());
}

// Process-owned sessions keep the 1.4.2 public object layouts and inline
// destructors ABI-compatible. One detector pipeline is shared per process.
struct Pipeline {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "BiopassNPU"};
  std::unique_ptr<Ort::Session> heads, decoder;
  std::vector<std::string> head_names, decoder_names;
  bool attempted = false;
  bool ready = false;
  bool cleanup_registered = false;
  std::mutex mutex;

  void initialize() {
    if (attempted) return;
    attempted = true;
    const auto start = std::chrono::steady_clock::now();
    const std::string cache = npu::prepareCache();
    npu::prepareRuntime(cache);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(cpuThreads());
    options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    const std::string runtime = setting("RYZEN_AI_PATH", "/opt/ryzen-ai");
    const std::string context = setting("BIOPASS_NPU_CONTEXT");
    std::unordered_map<std::string, std::string> provider{
        {"target", "X1"},
        {"config_file", setting("BIOPASS_NPU_CONFIG", "/opt/ryzen-ai/biopass-x1.json")},
        {"xclbin", runtime + "/voe-4.0-linux_x86_64/xclbins/phoenix/4x4.xclbin"},
        {"cache_dir", cache},
        {"enable_cache_file_io_in_mem", "0"}};
    // An EP context already contains its cache key; supplying another prevents reload.
    if (context.empty())
      provider["cache_key"] = setting("BIOPASS_NPU_CACHE_KEY", "biopass-face256-v1");
    options.AppendExecutionProvider_VitisAI(provider);
    const std::string export_path = setting("BIOPASS_NPU_EXPORT_CONTEXT");
    if (context.empty() && !export_path.empty()) {
      options.AddConfigEntry("ep.context_enable", "1");
      options.AddConfigEntry("ep.context_embed_mode", "1");
      options.AddConfigEntry("ep.context_file_path", export_path.c_str());
    }
    heads = std::make_unique<Ort::Session>(
        env, (context.empty() ? setting("BIOPASS_NPU_HEADS",
              "/usr/lib/biopass/npu-models/heads-xint8-face256.onnx") : context).c_str(), options);
    Ort::SessionOptions cpu;
    cpu.SetIntraOpNumThreads(cpuThreads());
    decoder = std::make_unique<Ort::Session>(
        env, setting("BIOPASS_NPU_DECODER", "/usr/lib/biopass/npu-models/decoder_fp32.onnx").c_str(), cpu);
    Ort::AllocatorWithDefaultOptions allocator;
    if (heads->GetOutputCount() != 9 || decoder->GetInputCount() != 9)
      throw std::runtime_error("Expected nine face heads");
    for (size_t i = 0; i < 9; ++i) {
      head_names.emplace_back(heads->GetOutputNameAllocated(i, allocator).get());
      decoder_names.emplace_back(decoder->GetInputNameAllocated(i, allocator).get());
      if (head_names.back() != decoder_names.back())
        throw std::runtime_error("Face decoder input name/order mismatch");
    }
    if (decoder->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape() !=
        std::vector<int64_t>({1, 20, 8400}))
      throw std::runtime_error("Unexpected face decoder output shape");
    ready = true;
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    npuLog("strict NPU heads + CPU decoder ready (" +
           std::string(context.empty() ? "model/cache" : "EP context") +
           "), init " + std::to_string(elapsed) + " ms");
  }

  std::vector<Ort::Value> run(Ort::Value& input) {
    Ort::AllocatorWithDefaultOptions allocator;
    auto in = heads->GetInputNameAllocated(0, allocator);
    auto out = decoder->GetOutputNameAllocated(0, allocator);
    const char* ins[] = {in.get()};
    const char* outs[] = {out.get()};
    std::vector<const char*> hn, dn;
    for (auto& name : head_names) hn.push_back(name.c_str());
    for (auto& name : decoder_names) dn.push_back(name.c_str());
    auto values = heads->Run(Ort::RunOptions{nullptr}, ins, &input, 1, hn.data(), hn.size());
    return decoder->Run(Ort::RunOptions{nullptr}, dn.data(), values.data(), values.size(), outs, 1);
  }
};

Pipeline& pipeline() {
  static Pipeline* value = new Pipeline;
  return *value;
}
#endif
}  // namespace

ModelBytes loadModelFile(const std::string& path) {
  std::optional<std::string> data = readRegularFile(path, kMaxModelBytes);
  if (!data) {
    throw std::runtime_error("cannot read model file: " + path);
  }
  return ModelBytes{std::move(*data)};
}

OnnxSession::OnnxSession(const ModelBytes& model, const char* log_name)
    : env_(ORT_LOGGING_LEVEL_WARNING, log_name) {
  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(cpuThreads());
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

  session_ = std::make_unique<Ort::Session>(env_, model.data.data(), model.data.size(), opts);

  for (size_t i = 0; i < session_->GetInputCount(); i++) {
    auto name = session_->GetInputNameAllocated(i, allocator_);
    input_names_str_.push_back(name.get());
  }
  for (size_t i = 0; i < session_->GetOutputCount(); i++) {
    auto name = session_->GetOutputNameAllocated(i, allocator_);
    output_names_str_.push_back(name.get());
  }
  for (auto& s : input_names_str_) input_names_cstr_.push_back(s.c_str());
  for (auto& s : output_names_str_) output_names_cstr_.push_back(s.c_str());
#ifdef BIOPASS_VITISAI
  if (std::string(log_name) == "FaceDetection" && setting("BIOPASS_NPU") == "1") {
    auto& npu = pipeline();
    std::lock_guard<std::mutex> guard(npu.mutex);
    try {
      npu.initialize();
    } catch (const std::exception& error) {
      npu.ready = false;
      npu.heads.reset();
      npu.decoder.reset();
      npuLog(std::string("NPU initialization failed; CPU fallback: ") + error.what());
    }
    // Register after the dynamically loaded provider's globals. Release OrtEnv
    // before those globals are destroyed, otherwise vendor cleanup can crash.
    if (!npu.cleanup_registered) {
      std::atexit([] { delete &pipeline(); });
      npu.cleanup_registered = true;
    }
    // Append only after building the actual input-name pointer array.
    // Reserve relocation cannot invalidate those pointers: the marker goes in
    // the output strings, whose pointers are rebuilt below.
    if (npu.ready) {
      output_names_str_.push_back("__biopass_npu_pipeline__");
      output_names_cstr_.clear();
      for (size_t i = 0; i + 1 < output_names_str_.size(); ++i)
        output_names_cstr_.push_back(output_names_str_[i].c_str());
    }
  }
#endif
}

std::vector<Ort::Value> OnnxSession::run(std::vector<float>& input,
                                         const std::vector<int64_t>& shape) {
  auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, input.data(), input.size(),
                                                            shape.data(), shape.size());

#ifdef BIOPASS_VITISAI
  if (output_names_str_.size() > output_names_cstr_.size()) {
    auto& npu = pipeline();
    std::lock_guard<std::mutex> guard(npu.mutex);
    if (npu.ready) {
      try {
        return npu.run(input_tensor);
      } catch (const std::exception& error) {
        npu.ready = false;
        npu.heads.reset();
        npu.decoder.reset();
        npuLog(std::string("NPU inference failed; CPU fallback: ") + error.what());
      }
    }
  }
#endif
  return session_->Run(Ort::RunOptions{nullptr}, input_names_cstr_.data(), &input_tensor, 1,
                       output_names_cstr_.data(), output_names_cstr_.size());
}

std::vector<int64_t> OnnxSession::inputShape() const {
  return session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
}

}  // namespace biopass
