#include "ir_verify.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "camera_capture.h"

namespace presence {
namespace {

using Clock = std::chrono::steady_clock;

// Frames are grey replicated into RGB; channel 0 is enough.
double meanLuma(const ImageRGB& image, int x1, int y1, int x2, int y2, bool inside) {
  double sum = 0;
  size_t count = 0;
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const bool in = x >= x1 && x < x2 && y >= y1 && y < y2;
      if (in != inside) continue;
      sum += image.data[(static_cast<size_t>(y) * image.width + x) * 3];
      ++count;
    }
  }
  return count ? sum / count : 0.0;
}

double frameMean(const ImageRGB& image) { return meanLuma(image, 0, 0, 0, 0, false); }

}  // namespace

IrVerifier::IrVerifier(IrOptions options, std::shared_ptr<biopass::FaceDetection> detector,
                       std::shared_ptr<biopass::FaceRecognition> recognizer)
    : options_(std::move(options)),
      detector_(std::move(detector)),
      recognizer_(std::move(recognizer)) {}

float IrVerifier::cosine(const Embedding& a, const Embedding& b) {
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  return na > 0 && nb > 0 ? static_cast<float>(dot / std::sqrt(na * nb)) : 0.0f;
}

IrFrameResult IrVerifier::analyse(const ImageRGB& lit, const ImageRGB& dark) {
  IrFrameResult result;
  auto detections = detector_->inference(lit);
  if (detections.empty()) return result;
  const auto& best = *std::max_element(
      detections.begin(), detections.end(),
      [](const auto& a, const auto& b) { return a.area() < b.area(); });
  result.face = true;
  const auto& box = best.box;
  // +1 keeps the ratio finite on fully dark frames.
  const double face = (meanLuma(lit, box.x1, box.y1, box.x2, box.y2, true) + 1) /
                      (meanLuma(dark, box.x1, box.y1, box.x2, box.y2, true) + 1);
  const double background = (meanLuma(lit, box.x1, box.y1, box.x2, box.y2, false) + 1) /
                            (meanLuma(dark, box.x1, box.y1, box.x2, box.y2, false) + 1);
  result.liveness = static_cast<float>(face / background);
  result.crop = best.image;
  if (result.liveness >= options_.min_liveness) result.embedding = recognizer_->inference(best.image);
  return result;
}

bool IrVerifier::session(int timeout_ms, const std::function<bool(const IrFrameResult&)>& sink) {
  enableIrEmitter();
  // No per-capture warmup: liveness needs two adjacent frames.
  auto camera = biopass::openCameraSession(options_.camera, biopass::CameraCaptureFormat::V4L2Grey,
                                           0, 2000);
  if (!camera) return false;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (int i = 0; i < 4; ++i) camera->capture();  // let exposure settle
  while (Clock::now() < deadline) {
    // Back-to-back captures so the pair is adjacent; inference happens after.
    ImageRGB a = camera->capture();
    ImageRGB b = camera->capture();
    if (a.empty() || b.empty()) return false;
    const double ma = frameMean(a), mb = frameMean(b);
    const bool a_lit = ma > mb;
    // Emitter must actually have toggled between the two frames.
    if (std::max(ma, mb) < 2 * std::min(ma, mb) + 1) continue;
    if (sink(analyse(a_lit ? a : b, a_lit ? b : a))) return true;
  }
  return true;
}

bool IrVerifier::verify(const std::vector<Embedding>& templates, int timeout_ms,
                        std::string& detail) {
  int faces = 0, live = 0, matches = 0;
  float best = -1, best_live = 0;
  session(timeout_ms, [&](const IrFrameResult& r) {
    if (!r.face) return false;
    ++faces;
    best_live = std::max(best_live, r.liveness);
    if (r.embedding.empty()) return false;
    ++live;
    float score = -1;
    for (const auto& t : templates) score = std::max(score, cosine(r.embedding, t));
    best = std::max(best, score);
    if (score >= options_.match_threshold) ++matches;
    return matches >= options_.needed_matches;
  });
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "ir faces=%d live=%d matched=%d best_cos=%.3f max_liveness=%.1f",
                faces, live, matches, best, best_live);
  detail = buffer;
  return matches >= options_.needed_matches;
}

std::vector<IrFrameResult> IrVerifier::collect(int wanted, int timeout_ms) {
  std::vector<IrFrameResult> out;
  session(timeout_ms, [&](const IrFrameResult& r) {
    if (!r.embedding.empty()) out.push_back(r);
    return static_cast<int>(out.size()) >= wanted;
  });
  return out;
}

std::string templatePath() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "") + "/.local/share/com.ticklab.biopass/ir_templates.f32";
}

std::vector<Embedding> loadTemplates() {
  std::ifstream in(templatePath(), std::ios::binary);
  uint32_t count = 0, dim = 0;
  if (!in.read(reinterpret_cast<char*>(&count), 4) || !in.read(reinterpret_cast<char*>(&dim), 4) ||
      count > 1000 || dim == 0 || dim > 4096)
    return {};
  std::vector<Embedding> templates(count, Embedding(dim));
  for (auto& t : templates)
    if (!in.read(reinterpret_cast<char*>(t.data()), dim * sizeof(float))) return {};
  return templates;
}

bool saveTemplates(const std::vector<Embedding>& templates) {
  if (templates.empty()) return false;
  const std::string path = templatePath();
  const std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const uint32_t header[2] = {static_cast<uint32_t>(templates.size()),
                              static_cast<uint32_t>(templates[0].size())};
  bool ok = write(fd, header, sizeof(header)) == sizeof(header);
  for (const auto& t : templates)
    ok = ok && write(fd, t.data(), t.size() * sizeof(float)) ==
                   static_cast<ssize_t>(t.size() * sizeof(float));
  ok = fsync(fd) == 0 && ok;
  close(fd);
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

// The UVC emitter control resets whenever the camera powers down.
void enableIrEmitter() {
  const char* home = std::getenv("HOME");
  const std::string config =
      std::string(home ? home : "") + "/.config/linux-enable-ir-emitter.toml";
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
      dup2(null, 1);
      dup2(null, 2);
    }
    execl("/usr/bin/linux-enable-ir-emitter", "linux-enable-ir-emitter", "run", "--config",
          config.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}

}  // namespace presence
