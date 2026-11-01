#include "npu_config.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace biopass::npu {
namespace {
using Values = std::unordered_map<std::string, std::string>;

std::string trim(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r");
  return first == std::string::npos ? "" :
      value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

Values loadConfig() {
  const char* path = "/etc/biopass/npu.env";
  uid_t owner = 0;
#ifdef BIOPASS_NPU_TEST_CONFIG
  // Only the separate, non-distributed smoke build accepts its user's fixture.
  path = BIOPASS_NPU_TEST_CONFIG;
  owner = geteuid();
#else
  if (const char* override_path = std::getenv("BIOPASS_NPU_ENV_FILE"))
    path = override_path;
#endif
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return {};
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != owner ||
      (info.st_mode & (S_IWGRP | S_IWOTH)) || info.st_size > 16384) {
    close(fd);
    syslog(LOG_WARNING, "biopass: untrusted NPU config ignored; using CPU");
    return {};
  }
  std::string content;
  char buffer[1024];
  ssize_t count;
  while ((count = read(fd, buffer, sizeof(buffer))) > 0 && content.size() <= 16384)
    content.append(buffer, static_cast<size_t>(count));
  close(fd);
  if (count < 0 || content.size() > 16384 || content.find('\0') != std::string::npos)
    return {};
  static const std::unordered_set<std::string> known{
      "BIOPASS_NPU", "BIOPASS_CPU_THREADS", "BIOPASS_NPU_CONFIG",
      "BIOPASS_NPU_HEADS", "BIOPASS_NPU_DECODER", "BIOPASS_NPU_CACHE",
      "BIOPASS_NPU_CACHE_KEY", "BIOPASS_NPU_CONTEXT", "BIOPASS_NPU_EXPORT_CONTEXT",
      "BIOPASS_NPU_LOG_STDERR", "RYZEN_AI_PATH", "RYZEN_AI_INSTALLATION_PATH",
      "XILINX_XRT"};
  Values values;
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto equal = line.find('=');
    if (equal == std::string::npos) return {};
    auto key = trim(line.substr(0, equal));
    auto value = trim(line.substr(equal + 1));
    if (!known.count(key)) continue;
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\'')))
      value = value.substr(1, value.size() - 2);
    values[key] = value;
  }
  return values;
}

void privateDirectory(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("Cannot open private NPU cache");
  struct stat info {};
  bool valid = fstat(fd, &info) == 0 && info.st_uid == geteuid();
  if (valid) valid = fchmod(fd, 0700) == 0;
  close(fd);
  if (!valid) throw std::runtime_error("NPU cache is not owned/writable by current user");
}
}  // namespace

std::string setting(const char* name, const char* fallback) {
  static const Values values = loadConfig();
  auto found = values.find(name);
  return found != values.end() && !found->second.empty() ? found->second : fallback;
}

std::string prepareCache() {
  std::filesystem::path base;
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    base = xdg;
  if (!base.is_absolute()) {
    struct passwd entry {}, *result = nullptr;
    std::vector<char> buffer(16384);
    if (getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || !result)
      throw std::runtime_error("Cannot resolve user's NPU cache home");
    base = std::filesystem::path(entry.pw_dir) / ".cache";
  }
  const auto default_cache = (base / "biopass/npu").string();
  const std::filesystem::path cache = setting("BIOPASS_NPU_CACHE", default_cache.c_str());
  if (!cache.is_absolute()) throw std::runtime_error("NPU cache path must be absolute");
  privateDirectory(cache);
  privateDirectory(cache / "logs");
  return cache.string();
}

void prepareRuntime(const std::string& cache) {
  struct rlimit limit {};
  if (getrlimit(RLIMIT_MEMLOCK, &limit) != 0 ||
      (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < 64ULL * 1024 * 1024))
    throw std::runtime_error("NPU requires at least 64 MiB memlock; using CPU until new login");
  const std::string root = setting("RYZEN_AI_PATH", "/opt/ryzen-ai");
  const auto installation = setting("RYZEN_AI_INSTALLATION_PATH", root.c_str());
  const auto xrt = setting("XILINX_XRT", (root + "/xrt").c_str());
  if (setenv("RYZEN_AI_PATH", root.c_str(), 1) != 0 ||
      setenv("RYZEN_AI_INSTALLATION_PATH", installation.c_str(), 1) != 0 ||
      setenv("XILINX_XRT", xrt.c_str(), 1) != 0 ||
      setenv("GLOG_logtostderr", "0", 1) != 0 ||
      setenv("GLOG_log_dir", (cache + "/logs").c_str(), 1) != 0)
    throw std::runtime_error("Cannot initialize NPU runtime environment");
}
}  // namespace biopass::npu
