#pragma once
//
// sysinfo.h — the box itself, as far as an operator needs to see it.
//
// A campus appliance has no keyboard and no screen worth reading, so the
// things somebody would normally check at a terminal — what my address is,
// whether the clock is right, how full the cache disk is — have to be
// answerable over the network and, for the address, on the splash screen
// before any network tool has been opened.
//
#include <string>
#include <vector>

namespace multisite_player {

struct NetInterface {
    std::string name;           // eth0, wlan0
    std::string ipv4;
    std::string mac;
    bool        up = false;
    bool        wireless = false;
};

// Every address the box can be reached on, loopback excluded. The first
// non-empty one is what the splash screen shows: it is the number somebody
// types into a phone.
std::vector<NetInterface> network_interfaces();

std::string hostname();

struct TimeInfo {
    long long   now_ms = 0;
    std::string timezone;
    std::string local_time;         // "2026-09-05 14:03:11"
    bool        ntp_synchronised = false;
    bool        ntp_enabled = false;
};
TimeInfo time_info();

// Time zones the box knows about, for the picker. Read from the system's own
// zone database rather than a list baked in here, which would go stale.
std::vector<std::string> available_timezones();

// Both go through timedatectl, so they behave exactly as they would if
// somebody had set them at a terminal. Returns an empty string on success or
// a human-readable reason on failure — usually "not running as root".
std::string set_timezone(const std::string& tz);
std::string set_ntp(bool enabled);
// Only meaningful with NTP off; a box in a building with no internet still
// needs a roughly correct clock for the times on its timeline to mean
// anything.
std::string set_time(long long epoch_ms);

struct DiskInfo {
    std::string path;
    long long   total_bytes = 0;
    long long   free_bytes = 0;
    // Whether this looks like removable/USB storage. The cache writes about
    // 3 GB an hour and will wear an SD card out, so it matters whether the
    // cache is on one.
    bool        is_sd_card = false;
};
DiskInfo disk_info(const std::string& path);

struct SystemInfo {
    std::string model;              // "Raspberry Pi 5 Model B Rev 1.0"
    std::string os_version;
    std::string kernel;
    double      uptime_s = 0;
    double      load_1min = 0;
    double      cpu_temp_c = 0;     // 0 when the box cannot report one
    // The Pi reports under-voltage and thermal throttling; both explain a
    // service that stutters, and neither is visible any other way.
    bool        throttled = false;
    bool        under_voltage = false;
};
SystemInfo system_info();

// Restart the player service, reboot, or shut down. Each returns an empty
// string once the request has been made.
std::string restart_service();
std::string reboot_box();
std::string shutdown_box();

// The version this build reports, in the UI and the log.
const char* player_version();

} // namespace multisite_player
