// biopass-presence: lock the session when the user walks away, and unlock it
// when the enrolled user comes back. Models stay resident, so verification
// reuses biopass's FaceAuth (detection, IR/AI anti-spoofing, recognition)
// without a per-unlock process start. The lock screen's own PAM flow and the
// password remain available as fallbacks.
//
// Camera is used only while the keyboard/mouse have been idle; while the user
// types, nothing runs.

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pwd.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <wayland-client.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "auth_config.h"
#include "camera_capture.h"
#include "ext-idle-notify-v1.h"
#include "face_as.h"
#include "face_auth.h"
#include "face_detection.h"
#include "ir_verify.h"
#include "model_registry.h"

namespace {
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

struct Options {
  std::string camera = "/dev/v4l/by-path/pci-0000:65:00.4-usb-0:1:1.0-video-index0";
  std::string model;
  int idle_s = 15;    // input idle before the camera starts watching
  int absent_s = 30;  // no face for this long while idle -> lock
  // KDE only starts its own biopass attempt once the lock screen is woken, so
  // probe right away after locking and back off only after waking it.
  int retry_after_fail_s = 5;
  std::string ir_camera = "/dev/v4l/by-path/pci-0000:65:00.4-usb-0:1:1.2-video-index0";
  int ir_timeout_ms = 1500;
  bool enroll_ir = false;
  int locked_period_ms = 300;
  // Keep the camera streaming this long after locking (fast, settled
  // exposure); afterwards fall back to sparse cold probes to save power.
  int locked_stream_s = 600;
  int sparse_period_ms = 2000;
};

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

// ---------------------------------------------------------------- Wayland idle
struct Idle {
  wl_display* display = nullptr;
  ext_idle_notifier_v1* notifier = nullptr;
  wl_seat* seat = nullptr;
  bool idle = false;
};

void onIdled(void* data, ext_idle_notification_v1*) { static_cast<Idle*>(data)->idle = true; }
void onResumed(void* data, ext_idle_notification_v1*) { static_cast<Idle*>(data)->idle = false; }
const ext_idle_notification_v1_listener kIdleListener = {onIdled, onResumed};

void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t) {
  auto* idle = static_cast<Idle*>(data);
  if (!std::strcmp(iface, ext_idle_notifier_v1_interface.name))
    idle->notifier = static_cast<ext_idle_notifier_v1*>(
        wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1));
  else if (!std::strcmp(iface, wl_seat_interface.name) && !idle->seat)
    idle->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
}
void onGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = {onGlobal, onGlobalRemove};

bool setupIdle(Idle& idle, int timeout_s) {
  idle.display = wl_display_connect(nullptr);
  if (!idle.display)
    return false;
  wl_registry* registry = wl_display_get_registry(idle.display);
  wl_registry_add_listener(registry, &kRegistryListener, &idle);
  wl_display_roundtrip(idle.display);
  if (!idle.notifier || !idle.seat)
    return false;
  auto* notification =
      ext_idle_notifier_v1_get_idle_notification(idle.notifier, timeout_s * 1000, idle.seat);
  ext_idle_notification_v1_add_listener(notification, &kIdleListener, &idle);
  wl_display_roundtrip(idle.display);
  return true;
}

// ----------------------------------------------------------- Screensaver DBus
struct Bus {
  sd_bus* bus = nullptr;
  ~Bus() { sd_bus_unref(bus); }

  bool call(const char* method, const char* sig = "", bool* out = nullptr) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                               "org.freedesktop.ScreenSaver", method, &error, &reply, sig);
    if (r >= 0 && out) {
      int value = 0;
      r = sd_bus_message_read(reply, "b", &value);
      *out = value;
    }
    if (r < 0)
      log("dbus %s failed: %s", method, error.message ? error.message : strerror(-r));
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return r >= 0;
  }
};

// ------------------------------------------------------------- Wake keypress
// KDE's lock screen only starts its non-interactive (biopass) authenticator
// on real input; SimulateUserActivity is not enough. A lone Shift tap types
// nothing into the password field.
class WakeKey {
 public:
  WakeKey() {
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0)
      return;
    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    std::snprintf(setup.name, sizeof(setup.name), "biopass-presence wake");
    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
      close(fd_);
      fd_ = -1;
    }
  }
  ~WakeKey() {
    if (fd_ >= 0) {
      ioctl(fd_, UI_DEV_DESTROY);
      close(fd_);
    }
  }
  bool ok() const { return fd_ >= 0; }

  void tap() {
    if (fd_ < 0)
      return;
    emit(EV_KEY, KEY_LEFTSHIFT, 1);
    emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_KEY, KEY_LEFTSHIFT, 0);
    emit(EV_SYN, SYN_REPORT, 0);
  }

 private:
  void emit(int type, int code, int value) {
    input_event event{};
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(fd_, &event, sizeof(event)) < 0)
      log("uinput write failed");
  }
  int fd_ = -1;
};

// ------------------------------------------------------------- Verification
class Verifier {
 public:
  Verifier(std::string username, const biopass::BiopassConfig& config,
           std::shared_ptr<biopass::FaceDetection> detector)
      : username_(std::move(username)), config_(config) {
    runtime_.debug = config_.strategy.debug;
    auth_ = std::make_unique<biopass::FaceAuth>(config_.methods.face, username_, nullptr,
                                                std::move(detector));
  }

  bool enabled() const { return config_.methods.face.enable; }

  // Same retry semantics as biopass-helper: Failure (anti-spoofing) and
  // Unavailable stop immediately, Retry tries again up to `retries`.
  bool verify() {
    presence::enableIrEmitter();
    auth_->beginAuthenticationSession();
    bool ok = false;
    const auto& face = config_.methods.face;
    for (uint32_t attempt = 0; attempt < std::max<uint32_t>(face.retries, 1); ++attempt) {
      const auto result = auth_->authenticate(username_, runtime_);
      if (result == biopass::AuthResult::Success) {
        ok = true;
        break;
      }
      if (result != biopass::AuthResult::Retry)
        break;
      std::this_thread::sleep_for(milliseconds(face.retry_delay));
    }
    auth_->endAuthenticationSession();
    return ok;
  }

 private:
  std::string username_;
  biopass::BiopassConfig config_;
  biopass::AuthConfig runtime_;
  std::unique_ptr<biopass::FaceAuth> auth_;
};

bool unlockSession() {
  sd_bus* bus = nullptr;
  if (sd_bus_open_system(&bus) < 0)
    return false;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                             "org.freedesktop.login1.Session", "Unlock", &error, nullptr, "");
  if (r < 0)
    log("logind Unlock failed: %s", error.message ? error.message : strerror(-r));
  sd_bus_error_free(&error);
  sd_bus_unref(bus);
  return r >= 0;
}

// -------------------------------------------------------------------- Camera
class Watcher {
 public:
  Watcher(const Options& opt, std::shared_ptr<biopass::FaceDetection> detector)
      : opt_(opt), detector_(std::move(detector)) {}

  // Watches through the IR camera so presence works in the dark and the
  // colour camera (and its LED) stays off. The emitter blinks every other
  // frame, so grab two frames and detect on the brighter one.
  // Returns nullopt if the camera could not deliver a frame.
  std::optional<bool> faceVisible(bool keep_open) {
    if (!session_) {
      presence::enableIrEmitter();  // the UVC control resets on power-down
      session_ = biopass::openCameraSession(opt_.ir_camera, biopass::CameraCaptureFormat::V4L2Grey,
                                            0, 3000);
      if (!session_)
        return std::nullopt;
      for (int i = 0; i < 4; ++i) session_->capture();  // let exposure settle
    }
    ImageRGB a = session_->capture();
    ImageRGB b = session_->capture();
    if (!keep_open)
      session_.reset();
    if (a.empty() || b.empty())
      return std::nullopt;
    return !detector_->inference(mean(a) >= mean(b) ? a : b).empty();
  }

  void release() { session_.reset(); }

 private:
  const Options& opt_;
  std::shared_ptr<biopass::FaceDetection> detector_;
  std::unique_ptr<biopass::ICameraCaptureSession> session_;

  static double mean(const ImageRGB& image) {
    double sum = 0;
    for (size_t i = 0; i < image.data.size(); i += 3) sum += image.data[i];
    return image.data.empty() ? 0 : sum * 3 / image.data.size();
  }
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  const char* home = std::getenv("HOME");
  opt.model =
      std::string(home ? home : "") + "/.local/share/com.ticklab.biopass/models/yolov8n-face.onnx";
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--enroll-ir") {
      opt.enroll_ir = true;
      continue;
    }
    if (i + 1 >= argc) {
      log("missing value for %s", key.c_str());
      return 2;
    }
    std::string value = argv[++i];
    if (key == "--camera")
      opt.camera = value;
    else if (key == "--model")
      opt.model = value;
    else if (key == "--idle")
      opt.idle_s = std::stoi(value);
    else if (key == "--absent")
      opt.absent_s = std::stoi(value);
    else if (key == "--ir-camera")
      opt.ir_camera = value;
    else {
      log("unknown option %s", key.c_str());
      return 2;
    }
  }

  passwd* user = getpwuid(getuid());
  if (!user) {
    log("cannot resolve current user");
    return 1;
  }
  const auto loaded = biopass::readConfig(user->pw_name);
  if (!loaded) {
    log("no usable biopass config.yaml");
    return 1;
  }
  const biopass::BiopassConfig& config = *loaded;
  const auto& face_cfg = config.methods.face;
  // One detector (NPU when enabled) serves presence watching, IR verification
  // and the FaceAuth fallback, at the configured auth threshold.
  auto detector = std::make_shared<biopass::FaceDetection>(biopass::loadModelFile(opt.model), 640,
                                                           face_cfg.detection.threshold);
  biopass::ModelRegistry registry(user->pw_name);
  const auto recog_model = registry.resolveModel(face_cfg.recognition.model_id);
  if (!recog_model) {
    log("recognition model not found");
    return 1;
  }
  auto recognizer = std::make_shared<biopass::FaceRecognition>(
      biopass::loadModelFile(recog_model->path), 112, face_cfg.recognition.threshold);
  presence::IrOptions ir_opt;
  ir_opt.camera = opt.ir_camera;
  presence::IrVerifier ir(ir_opt, detector, recognizer);

  if (opt.enroll_ir) {
    log("enrolling IR face: look at the screen and turn your head slightly");
    auto samples = ir.collect(12, 10000);
    if (samples.size() < 6) {
      log("only %zu live IR faces captured; not saved", samples.size());
      return 1;
    }
    std::vector<presence::Embedding> templates;
    float min_live = 1e9f;
    for (const auto& s : samples) {
      templates.push_back(s.embedding);
      min_live = std::min(min_live, s.liveness);
    }
    if (!presence::saveTemplates(templates)) {
      log("cannot write templates");
      return 1;
    }
    log("saved %zu IR templates to %s (min liveness %.1f)", templates.size(),
        presence::templatePath().c_str(), min_live);
    return 0;
  }

  const auto templates = presence::loadTemplates();
  Verifier fallback(user->pw_name, config, detector);
  if (!face_cfg.enable)
    log("face method disabled in biopass config; lock only");
  log(templates.empty() ? "no IR templates; using biopass FaceAuth (run --enroll-ir)"
                        : "IR-only verification with %zu templates",
      templates.size());

  Idle idle;
  if (!setupIdle(idle, opt.idle_s)) {
    log("compositor lacks ext-idle-notify-v1 or WAYLAND_DISPLAY is unset");
    return 1;
  }
  Bus bus;
  if (sd_bus_open_user(&bus.bus) < 0) {
    log("cannot open user bus");
    return 1;
  }

  WakeKey wake;
  if (!wake.ok())
    log("no /dev/uinput access; lock screen will need a keypress");

  Watcher watcher(opt, detector);
  log("ready: idle=%ds absent=%ds ir_camera=%s", opt.idle_s, opt.absent_s, opt.ir_camera.c_str());

  enum class State { Active, Watching, Locked } state = State::Active;
  auto last_face = Clock::now();
  auto locked_since = Clock::now();
  auto next_locked_probe = Clock::now();
  const int fd = wl_display_get_fd(idle.display);

  for (;;) {
    wl_display_flush(idle.display);
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, state == State::Watching ? 3000 : state == State::Locked ? 100 : 500) > 0) {
      if (wl_display_dispatch(idle.display) < 0) {
        log("wayland connection lost");
        return 1;
      }
    } else {
      wl_display_dispatch_pending(idle.display);
    }

    bool locked = false;
    bus.call("GetActive", "", &locked);
    const auto now = Clock::now();

    if (locked) {
      if (state != State::Locked) {
        watcher.release();
        state = State::Locked;
        locked_since = now;
        next_locked_probe = now;
        log("locked");
      }
      // Probe sparsely; open/close the camera each time so the lock screen's
      // own biopass PAM can grab it.
      if (now >= next_locked_probe) {
        const bool streaming = now - locked_since < seconds(opt.locked_stream_s);
        next_locked_probe =
            now + milliseconds(streaming ? opt.locked_period_ms : opt.sparse_period_ms);
        if (!streaming)
          watcher.release();
        if (watcher.faceVisible(streaming).value_or(false) && face_cfg.enable) {
          watcher.release();  // IR verification opens its own session
          const auto start = Clock::now();
          bool ok = false;
          if (templates.empty()) {
            ok = fallback.verify();
          } else {
            std::string detail;
            ok = ir.verify(templates, opt.ir_timeout_ms, detail);
            log("%s", detail.c_str());
          }
          const long ms = std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
          if (ok && unlockSession()) {
            log("verified in %ld ms; unlocked", ms);
            wake.tap();  // turn the display back on
            // The lock screen takes a moment to go away; don't re-verify.
            next_locked_probe = Clock::now() + seconds(10);
          } else {
            log("face seen but verification failed (%ld ms)", ms);
            next_locked_probe = Clock::now() + seconds(opt.retry_after_fail_s);
          }
        }
      }
      continue;
    }

    if (state == State::Locked)
      log("unlocked");
    if (!idle.idle) {
      if (state == State::Watching) {
        watcher.release();
        log("input resumed");
      }
      state = State::Active;
      continue;
    }

    if (state != State::Watching) {
      state = State::Watching;
      last_face = now;
      log("idle; watching for presence");
    }
    auto seen = watcher.faceVisible(false);  // sparse: emitter off between probes
    if (!seen)
      log("camera returned no frame");
    if (seen.value_or(false))
      last_face = now;
    static bool was_seen = true;
    if (seen && *seen != was_seen) {
      was_seen = *seen;
      log(was_seen ? "face present" : "face absent");
    }
    if (now - last_face >= seconds(opt.absent_s)) {
      log("no face for %ds; locking", opt.absent_s);
      watcher.release();
      bus.call("Lock");
    }
  }
}
