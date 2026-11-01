use serde::Serialize;
use std::fs;

#[derive(Debug, Serialize, Clone, specta::Type)]
pub struct VideoDeviceInfo {
    pub path: String,
    pub node: String,
    pub name: String,
    pub display_name: String,
}

#[tauri::command]
#[specta::specta]
pub fn get_current_username() -> Result<String, String> {
    username_from(|key| std::env::var(key).ok())
        .ok_or_else(|| "Could not determine current username".to_string())
}

/// The login name from the environment: USER, then LOGNAME (set by login and
/// PAM when USER isn't), then USERNAME. Empty values are skipped. This is only
/// a hint, not authoritative: fprintd's polkit check on the D-Bus caller is
/// the real guarantee (claiming a device for another name needs
/// net.reactivated.fprint.device.setusername). A uid-based lookup would need
/// getpwuid from a libc crate, which isn't a direct dependency.
fn username_from(var: impl Fn(&str) -> Option<String>) -> Option<String> {
    ["USER", "LOGNAME", "USERNAME"]
        .into_iter()
        .filter_map(var)
        .find(|name| !name.is_empty())
}

fn video_device_sort_key(path: &str) -> i32 {
    path.trim_start_matches("/dev/video")
        .parse::<i32>()
        .unwrap_or(-1)
}

// /dev/videoN numbering follows probe order and can swap between boots, so
// prefer a udev alias that is tied to the physical port (or the device id).
fn stable_video_path(node: &str) -> String {
    let target = fs::canonicalize(node).unwrap_or_else(|_| node.into());
    for dir in ["/dev/v4l/by-path", "/dev/v4l/by-id"] {
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        let mut aliases: Vec<_> = entries
            .flatten()
            .map(|entry| entry.path())
            .filter(|alias| fs::canonicalize(alias).ok().as_ref() == Some(&target))
            .collect();
        aliases.sort();
        if let Some(alias) = aliases.first() {
            return alias.to_string_lossy().into_owned();
        }
    }
    node.to_string()
}

/// Short form of a device path for the picker: the port part of a by-path
/// alias ("usb-0:1:1.2-video-index0"), else the alias or node file name.
fn device_label(path: &str) -> String {
    let file = path.rsplit('/').next().unwrap_or(path);
    match file.find("usb-") {
        Some(start) => file[start..].to_string(),
        None => file.to_string(),
    }
}

#[tauri::command]
#[specta::specta]
pub fn list_video_devices() -> Result<Vec<VideoDeviceInfo>, String> {
    let mut devices = Vec::new();
    let entries = fs::read_dir("/dev").map_err(|e| format!("Failed to read /dev: {}", e))?;

    for entry in entries.flatten() {
        let file_name = entry.file_name().to_string_lossy().to_string();
        if !file_name.starts_with("video") {
            continue;
        }

        let node = format!("/dev/{}", file_name);
        let path = stable_video_path(&node);
        let name = fs::read_to_string(format!("/sys/class/video4linux/{}/name", file_name))
            .map(|value| value.trim().to_string())
            .unwrap_or_default();
        let label = device_label(&path);
        let display_name = if name.is_empty() {
            label
        } else {
            format!("{} ({})", name, label)
        };

        devices.push(VideoDeviceInfo {
            path,
            node,
            name,
            display_name,
        });
    }

    devices.sort_by_key(|a| video_device_sort_key(&a.node));

    Ok(devices)
}

#[cfg(test)]
mod tests {
    use super::{device_label, username_from, video_device_sort_key};

    #[test]
    fn device_label_keeps_the_usb_port_part() {
        assert_eq!(
            device_label("/dev/v4l/by-path/pci-0000:65:00.4-usb-0:1:1.2-video-index0"),
            "usb-0:1:1.2-video-index0"
        );
        assert_eq!(
            device_label("/dev/v4l/by-id/usb-Cam_123-video-index0"),
            "usb-Cam_123-video-index0"
        );
        assert_eq!(device_label("/dev/video2"), "video2");
    }

    fn env<'a>(vars: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<String> + 'a {
        move |key| {
            vars.iter()
                .find(|(k, _)| *k == key)
                .map(|(_, v)| v.to_string())
        }
    }

    #[test]
    fn prefers_user_then_logname_then_username() {
        let all = [("USER", "a"), ("LOGNAME", "b"), ("USERNAME", "c")];
        assert_eq!(username_from(env(&all)).as_deref(), Some("a"));
        assert_eq!(username_from(env(&all[1..])).as_deref(), Some("b"));
        assert_eq!(username_from(env(&all[2..])).as_deref(), Some("c"));
    }

    #[test]
    fn empty_value_falls_through() {
        let vars = [("USER", ""), ("LOGNAME", "b")];
        assert_eq!(username_from(env(&vars)).as_deref(), Some("b"));
        assert_eq!(username_from(env(&[("USER", "")])), None);
    }

    #[test]
    fn none_set_is_none() {
        assert_eq!(username_from(env(&[])), None);
    }

    #[test]
    fn video_device_sort_key_is_the_device_number() {
        assert_eq!(video_device_sort_key("/dev/video0"), 0);
        assert_eq!(video_device_sort_key("/dev/video2"), 2);
        assert_eq!(video_device_sort_key("/dev/video10"), 10);
    }

    #[test]
    fn video_device_without_a_number_sorts_first() {
        for path in [
            "/dev/video",
            "/dev/video-meta",
            "/dev/videoX1",
            "video2",
            "",
        ] {
            assert_eq!(video_device_sort_key(path), -1, "{path}");
        }
    }

    #[test]
    fn video_devices_sort_numerically_not_lexically() {
        let mut paths = ["/dev/video10", "/dev/video2", "/dev/videoX", "/dev/video0"];
        paths.sort_by_key(|p| video_device_sort_key(p));
        assert_eq!(
            paths,
            ["/dev/videoX", "/dev/video0", "/dev/video2", "/dev/video10"]
        );
    }
}
