#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

#include "face_detection.h"
#include "image_io.h"

int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  try {
    struct rlimit memlock{};
    if (getrlimit(RLIMIT_MEMLOCK, &memlock) != 0)
      throw std::runtime_error("Cannot read memlock limit");
    std::cout << "uid=" << getuid() << " euid=" << geteuid() << " memlock=" << memlock.rlim_cur
              << "\n";
    auto image = readImage(argv[2]);
    if (image.empty())
      throw std::runtime_error("Cannot read face image");
    auto start = std::chrono::steady_clock::now();
    biopass::FaceDetection detector(biopass::loadModelFile(argv[1]));
    auto ready = std::chrono::steady_clock::now();
    auto detections = detector.inference(image);
    auto end = std::chrono::steady_clock::now();
    std::cout << "init_ms=" << std::chrono::duration<double, std::milli>(ready - start).count()
              << " detection_ms=" << std::chrono::duration<double, std::milli>(end - ready).count()
              << " faces=" << detections.size() << "\n";
    for (const auto& detection : detections)
      std::cout << detection.conf << " " << detection.box.x1 << " " << detection.box.y1 << " "
                << detection.box.x2 << " " << detection.box.y2 << "\n";
    return detections.empty() ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
