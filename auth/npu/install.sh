#!/usr/bin/env bash
set -euo pipefail

# This script is shipped for manual execution only. It never restarts a session.
if [[ "${1:-}" != install ]]; then
  echo "Usage: $0 install"
  echo "Installs staged libraries/runtime, in-process config and user@.service memlock."
  echo "Keep a password-authenticated terminal open; rollback path is printed."
  exit 2
fi
[[ $EUID == 0 ]] || { echo "Run as root when installation is authorized." >&2; exit 1; }
dist=$(cd -- "$(dirname -- "$0")" && pwd)
target=/usr/lib/biopass
runtime=/opt/ryzen-ai
models=$target/npu-models
envfile=/etc/biopass/npu.env
override=/etc/systemd/system/user@.service.d/90-biopass-npu.conf
legacy_override=/etc/systemd/system/sddm.service.d/90-biopass-npu.conf
for path in "$runtime" "$models" "$envfile" "$override" "$legacy_override"; do
  [[ ! -e "$path" && ! -L "$path" ]] || {
    echo "Refusing to overwrite existing $path" >&2; exit 1;
  }
done
for file in libbiopass_det.so libonnxruntime.so.1 runtime/deployment/lib/libonnxruntime_ryzenai.so.1; do
  [[ -f "$dist/$file" ]] || { echo "Missing $file" >&2; exit 1; }
done
backup=/var/backups/biopass-npu-$(date +%Y%m%d-%H%M%S)
install -d -m 0700 "$backup"
cp -a "$target" "$backup/lib"
cat > "$backup/rollback.sh" <<'ROLLBACK'
#!/usr/bin/env bash
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
backup=$(cd -- "$(dirname -- "$0")" && pwd)
rm -f /usr/lib/biopass/libbiopass_det.so /usr/lib/biopass/libonnxruntime.so.1
cp -a "$backup/lib/." /usr/lib/biopass/
rm -rf /opt/ryzen-ai /usr/lib/biopass/npu-models
rm -f /etc/biopass/npu.env /etc/systemd/system/user@.service.d/90-biopass-npu.conf
systemctl daemon-reload
echo "Rollback complete. Log out fully and log back in after saving your session."
echo "Per-user ~/.cache/biopass/npu caches are preserved; users may remove them."
ROLLBACK
chmod 0700 "$backup/rollback.sh"
echo "Backup and rollback: $backup/rollback.sh"
cp -a "$dist/runtime" "$runtime"
install -d -m 0755 "$models" /etc/biopass /etc/systemd/system/user@.service.d
cp -a "$dist/models/." "$models/"
chown -R root:root "$runtime" "$models"
chmod -R u=rwX,go=rX "$runtime" "$models"
install -m 0755 "$dist/libbiopass_det.so" "$target/libbiopass_det.so.npu-new"
install -m 0755 "$dist/libonnxruntime.so.1" "$target/libonnxruntime.so.1.npu-new"
mv -f "$target/libbiopass_det.so.npu-new" "$target/libbiopass_det.so"
mv -f "$target/libonnxruntime.so.1.npu-new" "$target/libonnxruntime.so.1"
cat > "$envfile" <<'ENV'
BIOPASS_NPU=1
BIOPASS_CPU_THREADS=4
BIOPASS_NPU_CONFIG=/opt/ryzen-ai/biopass-x1.json
BIOPASS_NPU_HEADS=/usr/lib/biopass/npu-models/heads-xint8-face256.onnx
BIOPASS_NPU_DECODER=/usr/lib/biopass/npu-models/decoder_fp32.onnx
BIOPASS_NPU_CACHE_KEY=biopass-face256-v1
RYZEN_AI_PATH=/opt/ryzen-ai
RYZEN_AI_INSTALLATION_PATH=/opt/ryzen-ai
XILINX_XRT=/opt/ryzen-ai/xrt
ENV
chown root:root "$envfile"
chmod 0644 "$envfile"
cat > "$override" <<'UNIT'
[Service]
LimitMEMLOCK=infinity
UNIT
systemctl daemon-reload
echo "Installed. No PAM changes and no service restart performed."
echo "Fully log out and log back in to recreate user@.service with the new limit."
echo "Linger users need their user manager restarted separately after saving work."
echo "The detector reads /etc/biopass/npu.env itself; no environment sourcing is needed."
echo "The detector creates each user's cache with mode 0700."
echo "Rollback: $backup/rollback.sh"
