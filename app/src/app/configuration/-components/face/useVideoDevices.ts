import { useEffect, useState } from "react";
import { cmd } from "@/commands";
import type { VideoDeviceInfo } from "@/types/config";

/** The system's video devices, listed once on mount. Empty on failure. */
export function useVideoDevices(): VideoDeviceInfo[] {
  const [videoDevices, setVideoDevices] = useState<VideoDeviceInfo[]>([]);

  useEffect(() => {
    const fetchDevices = async () => {
      try {
        setVideoDevices(await cmd.face.listVideoDevices());
      } catch (err) {
        console.error("Failed to fetch devices:", err);
      }
    };

    fetchDevices();
  }, []);

  return videoDevices;
}

/**
 * The listed path for a configured device. Older configs store /dev/videoN,
 * which can point at another camera after a reboot; map it to its stable
 * alias so the select shows it and saving migrates it.
 */
export function listedDevicePath(
  configured: string | null,
  devices: VideoDeviceInfo[],
): string | null {
  if (!configured) return null;
  return devices.find((device) => device.node === configured)?.path ?? configured;
}
