#include "sysinfo.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <linux/if_packet.h>
#include <sys/sysinfo.h>
#else
#include <net/if_dl.h>
#endif

#ifndef MULTISITE_PLAYER_VERSION
#define MULTISITE_PLAYER_VERSION "dev"
#endif

namespace multisite_player {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t' || s.back() == '\0'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return trim(ss.str());
}

// Run a command and collect its output. Used only for the handful of things
// that genuinely belong to systemd (the clock, restarting, rebooting): asking
// the tools that own them keeps this box behaving like any other Debian
// machine, rather than inventing a second way to set the time.
std::string run(const std::string& cmd, int* exit_code = nullptr) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p) {
        if (exit_code) *exit_code = -1;
        return "cannot run: " + cmd;
    }
    std::array<char, 512> buf{};
    while (::fgets(buf.data(), (int)buf.size(), p)) out += buf.data();
    const int rc = ::pclose(p);
    if (exit_code) *exit_code = rc;
    return trim(out);
}

bool have_command(const char* name) {
    int rc = 0;
    run(std::string("command -v ") + name, &rc);
    return rc == 0;
}

} // namespace

const char* player_version() { return MULTISITE_PLAYER_VERSION; }

// ── Network ──────────────────────────────────────────────────────────────────

std::vector<NetInterface> network_interfaces() {
    std::vector<NetInterface> out;
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return out;

    auto find_or_add = [&out](const std::string& name) -> NetInterface& {
        for (auto& i : out) if (i.name == name) return i;
        out.push_back(NetInterface{});
        out.back().name = name;
        return out.back();
    };

    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name) continue;
        const std::string name = p->ifa_name;
        if (name == "lo" || name == "lo0") continue;
        // Docker and similar bridges would only confuse somebody looking for
        // the address to type into a phone.
        if (name.rfind("docker", 0) == 0 || name.rfind("veth", 0) == 0 ||
            name.rfind("br-", 0) == 0)
            continue;

        if (p->ifa_addr->sa_family == AF_INET) {
            char ip[INET_ADDRSTRLEN] = {};
            auto* sin = (sockaddr_in*)p->ifa_addr;
            ::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            NetInterface& n = find_or_add(name);
            n.ipv4 = ip;
            n.up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
        }
#ifdef __linux__
        else if (p->ifa_addr->sa_family == AF_PACKET) {
            auto* ll = (sockaddr_ll*)p->ifa_addr;
            char mac[32] = {};
            if (ll->sll_halen == 6) {
                std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                              ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2],
                              ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
            }
            NetInterface& n = find_or_add(name);
            n.mac = mac;
            if (!n.up) n.up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
        }
#else
        else if (p->ifa_addr->sa_family == AF_LINK) {
            auto* dl = (sockaddr_dl*)p->ifa_addr;
            const unsigned char* a = (const unsigned char*)LLADDR(dl);
            char mac[32] = {};
            if (dl->sdl_alen == 6)
                std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                              a[0], a[1], a[2], a[3], a[4], a[5]);
            NetInterface& n = find_or_add(name);
            n.mac = mac;
        }
#endif
    }
    ::freeifaddrs(ifa);

#ifdef __linux__
    for (auto& n : out)
        n.wireless = !read_file("/sys/class/net/" + n.name + "/phy80211/name").empty();
#endif

    // Wired first, then anything with an address: an appliance should be on
    // ethernet, and that is the address worth showing first.
    std::stable_sort(out.begin(), out.end(),
        [](const NetInterface& a, const NetInterface& b) {
            if (a.ipv4.empty() != b.ipv4.empty()) return !a.ipv4.empty();
            return a.wireless < b.wireless;
        });
    return out;
}

std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "multisite-player";
    return buf;
}

// ── Time ─────────────────────────────────────────────────────────────────────

TimeInfo time_info() {
    TimeInfo t;
    using namespace std::chrono;
    t.now_ms = duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch()).count();

    const std::time_t now = (std::time_t)(t.now_ms / 1000);
    std::tm tm{};
    ::localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    t.local_time = buf;

    t.timezone = read_file("/etc/timezone");
    if (t.timezone.empty()) {
        char zone[8] = {};
        std::strftime(zone, sizeof(zone), "%Z", &tm);
        t.timezone = zone;
    }

    if (have_command("timedatectl")) {
        const std::string s = run("timedatectl show -p NTPSynchronized "
                                  "-p NTP --value");
        // Two lines: NTP enabled, then synchronised.
        std::istringstream is(s);
        std::string line;
        int idx = 0;
        while (std::getline(is, line)) {
            const bool yes = (trim(line) == "yes");
            if (idx == 0) t.ntp_enabled = yes; else t.ntp_synchronised = yes;
            ++idx;
        }
    }
    return t;
}

std::vector<std::string> available_timezones() {
    std::vector<std::string> out;
    if (have_command("timedatectl")) {
        std::istringstream is(run("timedatectl list-timezones"));
        std::string line;
        while (std::getline(is, line)) {
            line = trim(line);
            if (!line.empty()) out.push_back(line);
        }
    }
    if (out.empty()) {
        // A box without systemd still deserves the common ones rather than an
        // empty picker.
        out = {"Etc/UTC", "Europe/London", "Europe/Dublin", "Europe/Paris",
               "America/New_York", "America/Chicago", "America/Denver",
               "America/Los_Angeles", "Australia/Sydney", "Pacific/Auckland"};
    }
    return out;
}

std::string set_timezone(const std::string& tz) {
    // A time zone name goes on a command line, so it must be a time zone name
    // and nothing else.
    for (char c : tz)
        if (!(std::isalnum((unsigned char)c) || c == '/' || c == '_' ||
              c == '-' || c == '+'))
            return "that is not a valid time zone name";
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    int rc = 0;
    const std::string out = run("timedatectl set-timezone '" + tz + "'", &rc);
    if (rc != 0) return out.empty() ? "could not set the time zone" : out;
    plog_info("time zone set to %s", tz.c_str());
    return {};
}

std::string set_ntp(bool enabled) {
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    int rc = 0;
    const std::string out =
        run(std::string("timedatectl set-ntp ") + (enabled ? "true" : "false"), &rc);
    if (rc != 0) return out.empty() ? "could not change network time" : out;
    plog_info("network time %s", enabled ? "enabled" : "disabled");
    return {};
}

std::string set_time(long long epoch_ms) {
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    const std::time_t t = (std::time_t)(epoch_ms / 1000);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    int rc = 0;
    const std::string out = run(std::string("timedatectl set-time '") + buf + "'", &rc);
    if (rc != 0)
        return out.empty() ? "could not set the clock"
                           : out + " (turn network time off first)";
    plog_info("clock set to %s", buf);
    return {};
}

// ── Disk ─────────────────────────────────────────────────────────────────────

DiskInfo disk_info(const std::string& path) {
    DiskInfo d;
    d.path = path;
    struct statvfs st{};
    if (::statvfs(path.c_str(), &st) == 0) {
        d.total_bytes = (long long)st.f_blocks * (long long)st.f_frsize;
        d.free_bytes  = (long long)st.f_bavail * (long long)st.f_frsize;
    }
#ifdef __linux__
    // mmcblk0 is the SD card slot on a Pi. Writing the segment cache there
    // wears the card out, so it is worth saying plainly in the interface.
    int rc = 0;
    const std::string src = run("findmnt -n -o SOURCE --target '" + path + "'", &rc);
    d.is_sd_card = rc == 0 && src.find("mmcblk") != std::string::npos;
#endif
    return d;
}

// ── The machine ──────────────────────────────────────────────────────────────

SystemInfo system_info() {
    SystemInfo s;
#ifdef __linux__
    s.model = read_file("/sys/firmware/devicetree/base/model");
    if (s.model.empty()) s.model = read_file("/proc/device-tree/model");
    s.kernel = run("uname -r");

    std::ifstream os("/etc/os-release");
    std::string line;
    while (std::getline(os, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            s.os_version = line.substr(12);
            if (s.os_version.size() >= 2 && s.os_version.front() == '"')
                s.os_version = s.os_version.substr(1, s.os_version.size() - 2);
            break;
        }
    }

    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        s.uptime_s  = (double)si.uptime;
        s.load_1min = (double)si.loads[0] / 65536.0;
    }

    const std::string temp = read_file("/sys/class/thermal/thermal_zone0/temp");
    if (!temp.empty()) {
        try { s.cpu_temp_c = std::stod(temp) / 1000.0; } catch (...) {}
    }

    // Sustained decode on a passively cooled Pi throttles during a long
    // service, and an under-powered supply produces exactly the same symptom
    // as a bad network. Both are worth reporting rather than guessing at.
    if (have_command("vcgencmd")) {
        const std::string th = run("vcgencmd get_throttled");
        const size_t eq = th.find("0x");
        if (eq != std::string::npos) {
            try {
                const unsigned long bits = std::stoul(th.substr(eq), nullptr, 16);
                s.under_voltage = (bits & 0x1) || (bits & 0x10000);
                s.throttled     = (bits & 0x4) || (bits & 0x40000) ||
                                  (bits & 0x2) || (bits & 0x20000);
            } catch (...) {}
        }
    }
#else
    s.model = "development machine";
    s.kernel = run("uname -sr");
    s.os_version = run("uname -sr");
#endif
    return s;
}

std::string restart_service() {
    plog_info("restarting the player service at the operator's request");
    // Detached, because systemd will stop this very process: replying first
    // and acting a moment later is what lets the browser see the answer.
    int rc = 0;
    run("(sleep 1; systemctl restart multisite-player) >/dev/null 2>&1 &", &rc);
    return {};
}

std::string reboot_box() {
    plog_warn("rebooting at the operator's request");
    int rc = 0;
    run("(sleep 1; systemctl reboot) >/dev/null 2>&1 &", &rc);
    return {};
}

std::string shutdown_box() {
    plog_warn("shutting down at the operator's request");
    int rc = 0;
    run("(sleep 1; systemctl poweroff) >/dev/null 2>&1 &", &rc);
    return {};
}

} // namespace multisite_player
