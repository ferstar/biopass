#pragma once

// IR-only face verification with active-illumination liveness.
//
// The IR emitter alternates on/off between frames. A real face close to the
// camera brightens far more than the background when the emitter is on; a
// phone screen does not reflect the emitter (measured: real ~20x, phone ~0.5x
// face-vs-background response). Recognition then runs on the lit IR face and
// is compared to IR templates enrolled the same way.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "camera_capture.h"
#include "face_detection.h"
#include "face_recognition.h"

namespace presence {

using Embedding = std::vector<float>;

struct IrOptions {
  std::string camera;
  float min_liveness = 4.0f;  // face lit/dark ratio divided by background ratio
  float match_threshold = 0.6f;
  int needed_matches = 2;
};

struct IrFrameResult {
  bool face = false;
  float liveness = 0.0f;
  Embedding embedding;  // set only for live faces
  ImageRGB crop;
};

class IrVerifier {
 public:
  IrVerifier(IrOptions options, std::shared_ptr<biopass::FaceDetection> detector,
             std::shared_ptr<biopass::FaceRecognition> recognizer);

  // True once `needed_matches` live IR faces match any template. `camera`
  // reuses an open IR session: reopening powers the camera down, which resets
  // the emitter control.
  bool verify(const std::vector<Embedding>& templates, int timeout_ms, std::string& detail,
              biopass::ICameraCaptureSession* camera = nullptr);

  // Live IR embeddings for enrollment (crops returned for inspection).
  std::vector<IrFrameResult> collect(int wanted, int timeout_ms);

  static float cosine(const Embedding& a, const Embedding& b);

 private:
  // Counts frame pairs, and those where the emitter toggled, into the stats.
  struct PairStats {
    int pairs = 0;
    int toggled = 0;
  };
  bool session(int timeout_ms, const std::function<bool(const IrFrameResult&)>& sink,
               biopass::ICameraCaptureSession* camera = nullptr, PairStats* stats = nullptr);
  IrFrameResult analyse(const ImageRGB& lit, const ImageRGB& dark);

  IrOptions options_;
  std::shared_ptr<biopass::FaceDetection> detector_;
  std::shared_ptr<biopass::FaceRecognition> recognizer_;
};

std::string templatePath();
std::vector<Embedding> loadTemplates();
bool saveTemplates(const std::vector<Embedding>& templates);
void enableIrEmitter();

}  // namespace presence
