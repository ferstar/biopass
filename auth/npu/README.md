# Ryzen AI detector replacement (1.4.2)

This local bundle enables strict X1 NPU heads plus the original CPU decoder.
Preprocessing, NMS, recognition and anti-spoofing decisions remain unchanged.
Initialization/inference exceptions disable NPU and use the original CPU model.
If CPU also fails, existing authentication error handling receives the error.
Public headers, class layouts and existing detector exports remain compatible
with 1.4.2. Detector-internal calls bind locally to prevent old companion
libraries from overriding the updated OnnxSession.

## Build and dependencies

```sh
cmake -S auth/npu -B /tmp/biopass-model-bench/task6/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRYZEN_AI_ROOT=/path/to/ryzenai/1.9.0 \
  -DSTB_ROOT=/path/to/stb
cmake --build /tmp/biopass-model-bench/task6/build -j4
python auth/npu/set_runpaths.py /path/to/staged/dist
```

Use stb revision `904aa67e1e2d1dec92959df63e700b166d5c1022`.
The isolated target avoids building camera, GUI and PAM components.
Only this target defines `BIOPASS_VITISAI`; ordinary builds remain CPU.

The distribution includes the detector, smoke program, heads/decoder models,
Ryzen AI 1.9.0 / ORT 1.30.0, XRT dependencies and the install script.
The ORT bridge exports both `VERS_1.19.2` (existing helper and companion
libraries) and `VERS_1.30.0`, forwarding to one vendor runtime's versioned API.
Its renamed vendor library is
`runtime/deployment/lib/libonnxruntime_ryzenai.so.1`.

Build the bridge from `ort_compat.cc` and `ort_compat.map`, using
`-shared -fPIC -ldl`, `--no-as-needed` and the renamed vendor library.
The RUNPATH script updates each staged ELF independently, breaking hardlinks
before patching. It includes relative paths for staging plus
`/usr/lib/biopass`, `/opt/ryzen-ai/deployment/lib`,
`/opt/ryzen-ai/xrt/lib` and `/opt/ryzen-ai/compat`. Each dependency has its
own RUNPATH because a parent's RUNPATH does not cover indirect dependencies.
No `LD_LIBRARY_PATH` or environment sourcing is required.

## In-process configuration

The detector reads `/etc/biopass/npu.env` once inside the process. It accepts
only known keys; an absent/untrusted/malformed file disables NPU. The file must
be regular, root-owned and not group/other writable; symlinks are rejected.
`BIOPASS_NPU_ENV_FILE` can select another file with the same trust requirements.
NPU enable/model/runtime settings come from that file, rather than inherited
process variables. `LD_LIBRARY_PATH` is not accepted. Parsing does not execute
a shell or expand variables.

The installer writes `BIOPASS_NPU=1`, CPU threads (default four), runtime/model
paths and a model-specific cache key. The library sets required vendor
activation variables inside the process before appending the provider.
Optional keys are `BIOPASS_NPU_CONTEXT`, `BIOPASS_NPU_EXPORT_CONTEXT`,
`BIOPASS_NPU_CACHE` (absolute, user-owned), and the smoke diagnostic
`BIOPASS_NPU_LOG_STDERR=1`. Own logs normally go to syslog; vendor logs use the
user's cache log directory and may additionally log independently.

## Per-user cache

This deployment chooses ordinary compiled-file caching. The library creates
`${XDG_CACHE_HOME}/biopass/npu` when XDG_CACHE_HOME is absolute; otherwise it
uses the effective user's passwd home plus `.cache/biopass/npu`.
Cache and log directories must belong to that user and are set to mode 0700.
There is no shared root-owned writable cache. First initialization compiles;
later helper processes reuse their user's cache.

Embedded context export/reload remains an optional alternative. Put absolute
paths in the trusted config; loading a context omits `cache_key`. This installer
does not install a precompiled shared context. Change the cache key/path after
changing models, config or runtime. No resident daemon is added.

## Installation and rollback

`install.sh` is generated only and has not been executed. When separately
authorized, root can run `bash install.sh install` from this distribution.
It refuses existing runtime/model/config/drop-in destinations (including the
obsolete SDDM drop-in), backs up `/usr/lib/biopass`, installs root-owned runtime
and models, and creates:

- `/etc/biopass/npu.env`, root-owned, mode 0644.
- `/etc/systemd/system/user@.service.d/90-biopass-npu.conf`, containing
  `LimitMEMLOCK=infinity`, without an EnvironmentFile or SDDM drop-in.

No PAM modification, display-service restart, user-manager restart or limits.d
change occurs. Save work, fully log out, and log back in so user@.service is
recreated. With lingering enabled, a user manager may survive logout and needs
a separately arranged restart after saving work. Existing processes retain
old limits. Child services may impose their own limits; the effective helper
limit matters. Before provider setup the library checks a conservative 64 MiB
minimum soft memlock budget and falls back to CPU if lower; this guard is not
a universal runtime sizing claim. The user also needs `/dev/accel/accel0` access.

The generated `/var/backups/biopass-npu-*/rollback.sh` restores old libraries
and removes runtime/models/config and the user@.service drop-in. Per-user caches
are preserved; users can delete their own `biopass/npu` directory. New limits
after rollback also require a fresh user manager/login.

## Minimal validation boundary

The ordinary-user smoke uses the same code and RUNPATH deployment with a
temporary KEY=VALUE fixture. Since root/sudo writes are forbidden, a separate
smoke build sets `BIOPASS_NPU_TEST_CONFIG` to that fixture and accepts its user's
ownership. **The delivered library is built with this option empty and requires
root ownership.** The test path is fixed at compile time and is not enabled in
the distributed library.

Validation covers one image, strict execution/EXEC_CMD and a second process
reusing the cache, plus static dependency resolution. It does not certify
login/unlock or reevaluate accuracy. Models/runtime retain their original
licensing; this local bundle is not a redistribution license grant.
