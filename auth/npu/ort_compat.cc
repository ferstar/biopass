#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>

namespace {
void* resolve(const char* name) {
  void* symbol = dlvsym(RTLD_NEXT, name, "VERS_1.30.0");
  if (!symbol) {
    std::fprintf(stderr, "biopass: missing Ryzen AI ORT symbol: %s\n", dlerror());
    std::abort();
  }
  return symbol;
}
}

// The packaged helper and companion libraries request the 1.19.2 ELF version.
// Both versions use the vendor's version-negotiated OrtApi table.
extern "C" const void* biopass_api() {
  using Function = const void* (*)();
  static auto function = reinterpret_cast<Function>(resolve("OrtGetApiBase"));
  return function();
}
extern "C" void* biopass_cpu(void* options, int arena) {
  using Function = void* (*)(void*, int);
  static auto function = reinterpret_cast<Function>(
      resolve("OrtSessionOptionsAppendExecutionProvider_CPU"));
  return function(options, arena);
}
asm(".symver biopass_api,OrtGetApiBase@VERS_1.19.2");
asm(".symver biopass_api,OrtGetApiBase@@VERS_1.30.0");
asm(".symver biopass_cpu,OrtSessionOptionsAppendExecutionProvider_CPU@VERS_1.19.2");
asm(".symver biopass_cpu,OrtSessionOptionsAppendExecutionProvider_CPU@@VERS_1.30.0");
