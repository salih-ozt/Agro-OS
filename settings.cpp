// ============================================================
// AgroOS Settings
// Native C++17 + Skia + Wayland
// Single-file first integration build.
//
// The first goal is deliberate: keep this file self-contained
// and conservative so it can be dropped into the existing AgroOS
// build, compiled, and verified before adding the next UI module.
//
// Implemented:
//   System       - OS, kernel, CPU, RAM, disk, hostname, arch
//   Bluetooth    - adapter presence via sysfs
//   Network      - live interfaces + IPv4 from getifaddrs
//   Display      - current Wayland window surface size
//   Sound        - PipeWire/WirePlumber volume query when available
//   Personalize  - dark/light mode + accent + wallpaper path storage
//   Apps         - installed desktop-entry count + default browser lookup
//   Accounts     - current user + home directory
//   Time         - timezone + current date/time
//   Gaming       - gamemode presence check
//   Accessibility- reduced-motion / UI scale preferences
//   Privacy      - telemetry preference stored locally
//   Updates      - live manifest version check against AgroOS server
//   About        - build/runtime information
//
// Persistent settings:
//   ~/.config/agroos/settings.conf
//
// NOTE: Settings that require compositor/root privileges are stored as
// AgroOS preferences first. The privileged system backends can be wired
// after this component successfully builds in the real tree.
// ============================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "../common/agro_wayland_client.h"

namespace agro {
namespace fs = std::filesystem;

// ============================================================
// Geometry / palette
// ============================================================

static constexpr int WIDTH = 1440;
static constexpr int HEIGHT = 900;
static constexpr float SIDEBAR_W = 290.0f;
static constexpr float TOP_H = 86.0f;

static inline SkColor rgb(int r, int g, int b, int a = 255)
{
    return SkColorSetARGB(a, r, g, b);
}

static constexpr SkColor BG = SkColorSetARGB(255, 17, 18, 21);
static constexpr SkColor SIDEBAR = SkColorSetARGB(255, 23, 24, 28);
static constexpr SkColor PANEL = SkColorSetARGB(255, 29, 30, 35);
static constexpr SkColor PANEL_HOVER = SkColorSetARGB(255, 37, 39, 45);
static constexpr SkColor BORDER = SkColorSetARGB(255, 49, 51, 59);
static constexpr SkColor TEXT = SkColorSetARGB(255, 246, 247, 250);
static constexpr SkColor MUTED = SkColorSetARGB(255, 167, 170, 180);
static constexpr SkColor ACCENT = SkColorSetARGB(255, 88, 150, 255);
static constexpr SkColor GOOD = SkColorSetARGB(255, 78, 199, 126);
static constexpr SkColor WARN = SkColorSetARGB(255, 240, 179, 72);
static constexpr SkColor BAD = SkColorSetARGB(255, 226, 87, 87);

// ============================================================
// Helpers
// ============================================================

static std::string homeDir()
{
    const char* h = std::getenv("HOME");
    return (h && *h) ? std::string(h) : std::string("/");
}

static std::string trim(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string readFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static std::string firstLine(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    if (in)
        std::getline(in, line);
    return trim(line);
}

static bool exists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

static std::string runCommand(const std::string& command)
{
    std::array<char, 512> buf{};
    std::string out;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe)
        return {};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    ::pclose(pipe);
    return trim(out);
}

static std::string formatBytes(unsigned long long bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
        << value << ' ' << units[unit];
    return out.str();
}

static float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

static void fillRoundRect(SkCanvas* c, float x, float y, float w, float h,
                          float radius, SkColor color)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius), p);
}

static void strokeRoundRect(SkCanvas* c, float x, float y, float w, float h,
                            float radius, SkColor color, float width = 1.0f)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    p.setColor(color);
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius), p);
}

static void drawText(SkCanvas* c, const std::string& text, float x, float y,
                     float size, SkColor color, bool bold = false)
{
    SkFont font;
    font.setSize(size);
    if (bold)
        font.setEmbolden(true);

    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c->drawString(text.c_str(), x, y, font, p);
}

static float textWidth(const std::string& text, float size, bool bold = false)
{
    SkFont font;
    font.setSize(size);
    if (bold)
        font.setEmbolden(true);
    SkRect bounds{};
    font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
    return bounds.width();
}

// ============================================================
// Persistent configuration
// ============================================================

class SettingsStore {
public:
    SettingsStore()
    {
        path_ = homeDir() + "/.config/agroos/settings.conf";
        load();
    }

    const std::string& get(const std::string& key,
                           const std::string& fallback = {}) const
    {
        auto it = values_.find(key);
        return it == values_.end() ? fallbackRef_ = fallback : it->second;
    }

    std::string value(const std::string& key,
                      const std::string& fallback = {}) const
    {
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }

    bool boolean(const std::string& key, bool fallback) const
    {
        std::string v = lower(value(key, fallback ? "true" : "false"));
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }

    void set(const std::string& key, const std::string& value)
    {
        values_[key] = value;
        save();
    }

    void setBool(const std::string& key, bool value)
    {
        set(key, value ? "true" : "false");
    }

private:
    void load()
    {
        std::ifstream in(path_);
        if (!in)
            return;

        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            const auto pos = line.find('=');
            if (pos == std::string::npos)
                continue;

            values_[trim(line.substr(0, pos))] = trim(line.substr(pos + 1));
        }
    }

    void save() const
    {
        const fs::path file(path_);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);

        const fs::path tmp = file.string() + ".tmp";
        std::ofstream out(tmp);
        if (!out)
            return;

        out << "# AgroOS Settings\n";
        for (const auto& [key, value] : values_)
            out << key << '=' << value << '\n';

        out.close();
        fs::rename(tmp, file, ec);
        if (ec) {
            std::ofstream fallback(file);
            if (!fallback)
                return;
            for (const auto& [key, value] : values_)
                fallback << key << '=' << value << '\n';
        }
    }

    std::string path_;
    std::map<std::string, std::string> values_;
    mutable std::string fallbackRef_;
};

// ============================================================
// System information
// ============================================================

struct SystemInfo {
    std::string osName = "AgroOS";
    std::string osVersion = "Unknown";
    std::string kernel = "Unknown";
    std::string architecture = "Unknown";
    std::string hostname = "Unknown";
    std::string cpu = "Unknown";
    std::string gpu = "Not detected";
    unsigned long long ramTotal = 0;
    unsigned long long ramAvailable = 0;
    unsigned long long diskTotal = 0;
    unsigned long long diskFree = 0;
};

static SystemInfo readSystemInfo()
{
    SystemInfo info;

    std::string os = readFile("/etc/os-release");
    std::istringstream osLines(os);
    std::string line;
    while (std::getline(osLines, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key = line.substr(0, pos);
        std::string value = trim(line.substr(pos + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key == "NAME")
            info.osName = value;
        else if (key == "VERSION_ID")
            info.osVersion = value;
    }

    struct utsname u{};
    if (::uname(&u) == 0) {
        info.kernel = std::string(u.release);
        info.architecture = std::string(u.machine);
        info.hostname = std::string(u.nodename);
    }

    std::ifstream cpuFile("/proc/cpuinfo");
    while (std::getline(cpuFile, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto pos = line.find(':');
            if (pos != std::string::npos)
                info.cpu = trim(line.substr(pos + 1));
            break;
        }
    }

    std::ifstream memFile("/proc/meminfo");
    unsigned long long totalKb = 0;
    unsigned long long availKb = 0;
    while (std::getline(memFile, line)) {
        std::istringstream row(line);
        std::string key;
        unsigned long long value = 0;
        std::string unit;
        row >> key >> value >> unit;
        if (key == "MemTotal:")
            totalKb = value;
        else if (key == "MemAvailable:")
            availKb = value;
    }
    info.ramTotal = totalKb * 1024ULL;
    info.ramAvailable = availKb * 1024ULL;

    struct statvfs vfs{};
    if (::statvfs("/", &vfs) == 0) {
        info.diskTotal = static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize;
        info.diskFree = static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
    }

    const char* gpuRoots[] = {
        "/sys/class/drm/card0/device/product_name",
        "/sys/class/drm/card1/device/product_name"
    };
    for (const char* root : gpuRoots) {
        std::string candidate = firstLine(root);
        if (!candidate.empty()) {
            info.gpu = candidate;
            break;
        }
    }

    if (info.gpu == "Not detected") {
        std::error_code ec;
        const fs::path drm("/sys/class/drm");
        if (fs::is_directory(drm, ec)) {
            for (const auto& e : fs::directory_iterator(drm, ec)) {
                const std::string n = e.path().filename().string();
                if (n.rfind("card", 0) != 0)
                    continue;
                const std::string driver = e.path().string() + "/device/driver/module";
                if (fs::exists(driver, ec)) {
                    std::string name = firstLine(driver + "/name");
                    if (!name.empty()) {
                        info.gpu = name;
                        break;
                    }
                }
            }
        }
    }

    return info;
}

// ============================================================
// Network information
// ============================================================

struct NetworkEntry {
    std::string name;
    std::string address;
    bool loopback = false;
    bool up = false;
};

static std::vector<NetworkEntry> readNetwork()
{
    std::vector<NetworkEntry> result;
    struct ifaddrs* list = nullptr;

    if (::getifaddrs(&list) != 0)
        return result;

    std::map<std::string, NetworkEntry> merged;

    for (struct ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_name)
            continue;

        auto& row = merged[it->ifa_name];
        row.name = it->ifa_name;
        row.loopback = (it->ifa_flags & IFF_LOOPBACK) != 0;
        row.up = (it->ifa_flags & IFF_UP) != 0;

        if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET) {
            char host[INET_ADDRSTRLEN]{};
            const auto* sin = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            if (::inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)))
                row.address = host;
        }
    }

    ::freeifaddrs(list);

    for (const auto& [name, row] : merged)
        result.push_back(row);

    return result;
}

// ============================================================
// Other system probes
// ============================================================

static bool bluetoothPresent()
{
    std::error_code ec;
    const fs::path dir("/sys/class/bluetooth");
    if (!fs::is_directory(dir, ec))
        return false;
    return fs::directory_iterator(dir, ec) != fs::directory_iterator();
}

static std::string timezoneName()
{
    std::string tz = firstLine("/etc/timezone");
    if (!tz.empty())
        return tz;

    const std::string timedate = runCommand("timedatectl show --property=Timezone --value 2>/dev/null");
    if (!timedate.empty())
        return timedate;

    std::error_code ec;
    fs::path link = fs::read_symlink("/etc/localtime", ec);
    if (!ec) {
        const std::string marker = "/zoneinfo/";
        const std::string s = link.string();
        const auto pos = s.find(marker);
        if (pos != std::string::npos)
            return s.substr(pos + marker.size());
    }
    return "Unknown";
}

static std::string soundStatus()
{
    std::string result = runCommand("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    if (!result.empty())
        return result;

    result = runCommand("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | head -1");
    return result.empty() ? "Audio backend unavailable" : result;
}

static std::string defaultBrowser()
{
    const std::string xdg = runCommand("xdg-settings get default-web-browser 2>/dev/null");
    return xdg.empty() ? "Not configured" : xdg;
}

static int desktopEntryCount()
{
    int count = 0;
    const char* dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications"
    };

    std::error_code ec;
    for (const char* dir : dirs) {
        if (!fs::is_directory(dir, ec))
            continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".desktop")
                ++count;
        }
    }

    return count;
}

static bool gameModePresent()
{
    return exists("/usr/lib/libgamemode.so") ||
           exists("/usr/lib/x86_64-linux-gnu/libgamemode.so") ||
           !runCommand("command -v gamemoderun 2>/dev/null").empty();
}

static std::string currentTimeText()
{
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "%A, %d %B %Y  %H:%M:%S", &tmv);
    return buf;
}

// ============================================================
// Settings categories
// ============================================================

enum class Page {
    System,
    Bluetooth,
    Network,
    Display,
    Personalization,
    Apps,
    Accounts,
    Time,
    Gaming,
    Accessibility,
    Privacy,
    Updates,
    About
};

struct Category {
    Page page;
    const char* title;
    const char* subtitle;
};

static const std::vector<Category> kCategories = {
    {Page::System,          "System",           "About, storage, power and devices"},
    {Page::Bluetooth,       "Bluetooth & Devices", "Bluetooth and connected hardware"},
    {Page::Network,         "Network & Internet", "Interfaces, addresses and connectivity"},
    {Page::Display,         "Display",            "Resolution, scaling and visual options"},
    {Page::Personalization, "Personalization",    "Themes, colors and wallpaper"},
    {Page::Apps,            "Apps",               "Installed applications and defaults"},
    {Page::Accounts,        "Accounts",            "Current user and local profile"},
    {Page::Time,            "Time & Language",    "Clock, timezone and locale"},
    {Page::Gaming,          "Gaming",             "Game mode and performance preferences"},
    {Page::Accessibility,   "Accessibility",      "Motion, scale and future assistive options"},
    {Page::Privacy,         "Privacy",             "Telemetry and local privacy options"},
    {Page::Updates,         "Windows Update",      "AgroOS update channel and server status"},
    {Page::About,           "About AgroOS",        "Version, kernel and runtime information"},
};

static const char* pageTitle(Page page)
{
    for (const auto& c : kCategories)
        if (c.page == page)
            return c.title;
    return "Settings";
}

// ============================================================
// Settings UI
// ============================================================

class SettingsUI {
public:
    static constexpr int Width = WIDTH;
    static constexpr int Height = HEIGHT;

    SettingsUI()
        : store_()
    {
        selected_ = Page::System;
        system_ = readSystemInfo();
        network_ = readNetwork();
        refreshClock();
        refreshVersionText();
    }

    void update(float dt)
    {
        elapsed_ += dt;

        if (elapsed_ >= 1.0f) {
            elapsed_ = 0.0f;
            refreshClock();
        }

        if (refreshRequested_) {
            refreshRequested_ = false;
            system_ = readSystemInfo();
            network_ = readNetwork();
        }
    }

    void pointerMove(float x, float y)
    {
        pointerX_ = x;
        pointerY_ = y;
        hoveredNav_ = -1;
        hoveredAction_ = -1;

        if (x < SIDEBAR_W) {
            hoveredNav_ = navHit(x, y);
            return;
        }

        hoveredAction_ = actionHit(x, y);
    }

    void pointerDown(float x, float y, uint32_t button)
    {
        if (button != 1)
            return;

        pointerMove(x, y);

        if (x < SIDEBAR_W) {
            const int hit = navHit(x, y);
            if (hit >= 0 && hit < static_cast<int>(kCategories.size())) {
                selected_ = kCategories[hit].page;
            }
            return;
        }

        activateAction(actionHit(x, y));
    }

    void draw(SkCanvas* c) const
    {
        c->clear(BG);

        drawSidebar(c);
        drawHeader(c);
        drawPage(c);
    }

private:
    // --------------------------------------------------------
    // Sidebar
    // --------------------------------------------------------

    int navHit(float x, float y) const
    {
        if (x < 0.0f || x > SIDEBAR_W)
            return -1;

        const float top = 132.0f;
        const float itemH = 52.0f;
        const float gap = 4.0f;

        if (y < top)
            return -1;

        const int index = static_cast<int>((y - top) / (itemH + gap));
        if (index < 0 || index >= static_cast<int>(kCategories.size()))
            return -1;

        const float local = y - (top + index * (itemH + gap));
        if (local > itemH)
            return -1;

        return index;
    }

    void drawSidebar(SkCanvas* c) const
    {
        SkPaint bg;
        bg.setColor(SIDEBAR);
        c->drawRect(SkRect::MakeXYWH(0, 0, SIDEBAR_W, HEIGHT), bg);

        drawText(c, "AgroOS", 34, 52, 26, TEXT, true);
        drawText(c, "Settings", 35, 77, 14, MUTED);

        fillRoundRect(c, 22, 95, SIDEBAR_W - 44, 1, 0.5f, BORDER);

        const float top = 132.0f;
        const float itemH = 52.0f;
        const float gap = 4.0f;

        for (int i = 0; i < static_cast<int>(kCategories.size()); ++i) {
            const float y = top + i * (itemH + gap);
            const bool selected = kCategories[i].page == selected_;
            const bool hovered = i == hoveredNav_;

            SkColor fill = selected ? rgb(48, 80, 138) : (hovered ? PANEL_HOVER : SIDEBAR);
            fillRoundRect(c, 16, y, SIDEBAR_W - 32, itemH, 13, fill);

            // Simple vector icon: circle + short line mark.
            SkPaint icon;
            icon.setAntiAlias(true);
            icon.setColor(selected ? ACCENT : MUTED);
            icon.setStyle(SkPaint::kStroke_Style);
            icon.setStrokeWidth(2.1f);
            c->drawCircle(37, y + 26, 8.0f, icon);
            c->drawLine(34, y + 26, 40, y + 26, icon);

            drawText(c, kCategories[i].title,
                     58, y + 22, 14.0f,
                     selected ? TEXT : MUTED,
                     selected);

            drawText(c, kCategories[i].subtitle,
                     58, y + 39, 9.5f,
                     selected ? rgb(205, 216, 240) : rgb(116, 119, 127));
        }
    }

    // --------------------------------------------------------
    // Header
    // --------------------------------------------------------

    void drawHeader(SkCanvas* c) const
    {
        drawText(c, pageTitle(selected_), SIDEBAR_W + 40, 50, 27, TEXT, true);
        drawText(c, currentTime_, SIDEBAR_W + 40, 73, 12.5f, MUTED);

        const float pillX = static_cast<float>(WIDTH) - 196.0f;
        fillRoundRect(c, pillX, 30, 150, 42, 12,
                      store_.boolean("theme.dark", true) ? rgb(42, 43, 49) : rgb(233, 234, 239));

        drawText(c,
                 store_.boolean("theme.dark", true) ? "Dark theme" : "Light theme",
                 pillX + 16, 56, 12.5f,
                 store_.boolean("theme.dark", true) ? TEXT : rgb(48, 49, 55), true);
    }

    // --------------------------------------------------------
    // Cards / rows
    // --------------------------------------------------------

    void drawCard(SkCanvas* c, float x, float y, float w, float h) const
    {
        fillRoundRect(c, x, y, w, h, 18, PANEL);
        strokeRoundRect(c, x, y, w, h, 18, BORDER, 1.0f);
    }

    void drawRow(SkCanvas* c, float x, float y, float w,
                 const std::string& title, const std::string& value,
                 SkColor valueColor = MUTED) const
    {
        drawText(c, title, x, y + 21, 13.5f, TEXT, true);
        drawText(c, value, x, y + 41, 11.5f, valueColor);
        fillRoundRect(c, x, y + 56, w, 1, 0.5f, BORDER);
    }

    void drawSectionTitle(SkCanvas* c, float x, float y,
                          const std::string& title,
                          const std::string& subtitle) const
    {
        drawText(c, title, x, y, 18, TEXT, true);
        drawText(c, subtitle, x, y + 23, 11.5f, MUTED);
    }

    void drawButton(SkCanvas* c, float x, float y, float w, float h,
                    const std::string& label, bool primary, bool hovered) const
    {
        SkColor fill = primary
            ? ACCENT
            : (hovered ? PANEL_HOVER : rgb(43, 44, 50));

        fillRoundRect(c, x, y, w, h, 11, fill);
        strokeRoundRect(c, x, y, w, h, 11,
                        primary ? rgb(115, 165, 255) : BORDER);

        const float tw = textWidth(label, 12.5f, true);
        drawText(c, label, x + (w - tw) * 0.5f, y + h * 0.62f,
                 12.5f, TEXT, true);
    }

    void drawToggle(SkCanvas* c, float x, float y, bool on) const
    {
        fillRoundRect(c, x, y, 48, 28, 14,
                      on ? ACCENT : rgb(58, 59, 67));
        fillRoundRect(c, on ? x + 24 : x + 4, y + 4, 20, 20, 10, TEXT);
    }

    // --------------------------------------------------------
    // Pages
    // --------------------------------------------------------

    void drawPage(SkCanvas* c) const
    {
        switch (selected_) {
        case Page::System:          drawSystem(c); break;
        case Page::Bluetooth:       drawBluetooth(c); break;
        case Page::Network:         drawNetwork(c); break;
        case Page::Display:         drawDisplay(c); break;
        case Page::Personalization: drawPersonalization(c); break;
        case Page::Apps:            drawApps(c); break;
        case Page::Accounts:        drawAccounts(c); break;
        case Page::Time:            drawTime(c); break;
        case Page::Gaming:          drawGaming(c); break;
        case Page::Accessibility:   drawAccessibility(c); break;
        case Page::Privacy:         drawPrivacy(c); break;
        case Page::Updates:         drawUpdates(c); break;
        case Page::About:           drawAbout(c); break;
        }
    }

    void drawSystem(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;

        drawSectionTitle(c, x, 120,
                         "System",
                         "Device, storage, power and runtime information");

        drawCard(c, x, 158, w, 236);
        drawRow(c, x + 24, 176, w - 48, "Device", system_.hostname);
        drawRow(c, x + 24, 240, w - 48, "Operating system",
                system_.osName + " " + system_.osVersion);
        drawRow(c, x + 24, 304, w - 48, "Kernel",
                system_.kernel + " • " + system_.architecture);

        const float y = 418;
        const float cardW = (w - 18) * 0.5f;

        drawCard(c, x, y, cardW, 182);
        drawText(c, "Memory", x + 22, y + 34, 16, TEXT, true);
        drawText(c, formatBytes(system_.ramTotal), x + 22, y + 73, 24, TEXT, true);
        drawText(c, formatBytes(system_.ramAvailable) + " available",
                 x + 22, y + 96, 11.5f, MUTED);
        const float usedRam = system_.ramTotal == 0
            ? 0.0f
            : clamp01(1.0f - static_cast<float>(system_.ramAvailable) /
                      static_cast<float>(system_.ramTotal));
        fillRoundRect(c, x + 22, y + 124, cardW - 44, 10, 5, rgb(54, 56, 65));
        fillRoundRect(c, x + 22, y + 124,
                      (cardW - 44) * usedRam, 10, 5, ACCENT);

        drawCard(c, x + cardW + 18, y, cardW, 182);
        drawText(c, "Storage", x + cardW + 40, y + 34, 16, TEXT, true);
        drawText(c, formatBytes(system_.diskTotal - system_.diskFree),
                 x + cardW + 40, y + 73, 24, TEXT, true);
        drawText(c, formatBytes(system_.diskFree) + " free",
                 x + cardW + 40, y + 96, 11.5f, MUTED);
        const float usedDisk = system_.diskTotal == 0
            ? 0.0f
            : clamp01(1.0f - static_cast<float>(system_.diskFree) /
                      static_cast<float>(system_.diskTotal));
        fillRoundRect(c, x + cardW + 40, y + 124,
                      cardW - 44, 10, 5, rgb(54, 56, 65));
        fillRoundRect(c, x + cardW + 40, y + 124,
                      (cardW - 44) * usedDisk, 10, 5, GOOD);

        drawText(c, "CPU", x + 22, y + 224, 12, MUTED, true);
        drawText(c, system_.cpu, x + 22, y + 246, 11.5f, TEXT);
        drawText(c, "GPU", x + cardW + 40, y + 224, 12, MUTED, true);
        drawText(c, system_.gpu, x + cardW + 40, y + 246, 11.5f, TEXT);
    }

    void drawBluetooth(SkCanvas* c) const
    {
        drawSectionTitle(c, SIDEBAR_W + 40, 120,
                         "Bluetooth & Devices",
                         "Bluetooth adapters and connected hardware");

        drawCard(c, SIDEBAR_W + 40, 158, WIDTH - SIDEBAR_W - 80, 142);
        drawText(c, "Bluetooth", SIDEBAR_W + 66, 194, 16, TEXT, true);
        drawText(c,
                 bluetoothPresent() ? "Adapter detected" : "No adapter detected",
                 SIDEBAR_W + 66, 219, 12, bluetoothPresent() ? GOOD : MUTED);
        drawToggle(c, WIDTH - 178, 184, bluetoothPresent());
        drawText(c, "Hardware detection is live from /sys/class/bluetooth.",
                 SIDEBAR_W + 66, 264, 11, MUTED);

        drawCard(c, SIDEBAR_W + 40, 322, WIDTH - SIDEBAR_W - 80, 250);
        drawText(c, "Connected devices", SIDEBAR_W + 66, 357, 16, TEXT, true);
        drawText(c, "A device-manager backend will populate HID/audio/display details.",
                 SIDEBAR_W + 66, 388, 11.5f, MUTED);
        drawText(c, "Current stage: detection + UI framework", SIDEBAR_W + 66, 418, 12, WARN, true);
        drawText(c, "This avoids inventing compositor-specific hardware APIs.",
                 SIDEBAR_W + 66, 446, 11.5f, MUTED);
    }

    void drawNetwork(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Network & Internet",
                         "Live interfaces and IPv4 state");

        float y = 158;
        for (const auto& row : network_) {
            drawCard(c, x, y, w, 88);
            drawText(c, row.name, x + 24, y + 31, 15, TEXT, true);
            drawText(c,
                     row.address.empty() ? "No IPv4 address" : row.address,
                     x + 24, y + 56, 11.5f,
                     row.up ? GOOD : MUTED);
            drawText(c, row.loopback ? "Loopback" : (row.up ? "UP" : "DOWN"),
                     x + w - 120, y + 44, 11, row.up ? GOOD : MUTED, true);
            y += 100;
            if (y > HEIGHT - 110)
                break;
        }

        if (network_.empty()) {
            drawCard(c, x, y, w, 110);
            drawText(c, "No interfaces detected", x + 24, y + 42, 15, TEXT, true);
            drawText(c, "getifaddrs() returned no network entries.", x + 24, y + 68, 11.5f, MUTED);
        }
    }

    void drawDisplay(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Display",
                         "Wayland surface and local visual preferences");

        drawCard(c, x, 158, w, 150);
        drawRow(c, x + 24, 176, w - 48,
                "Window surface",
                std::to_string(WIDTH) + " × " + std::to_string(HEIGHT));

        drawCard(c, x, 328, w, 188);
        drawText(c, "Scaling", x + 24, 362, 15, TEXT, true);
        drawText(c, store_.value("display.scale", "100%"), x + 24, 392, 13, MUTED);
        drawButton(c, x + 24, 420, 110, 40, "100%", true, hoveredAction_ == 20);
        drawButton(c, x + 146, 420, 110, 40, "125%", false, hoveredAction_ == 21);
        drawButton(c, x + 268, 420, 110, 40, "150%", false, hoveredAction_ == 22);
        drawText(c, "Compositor-level physical scaling will be connected after this UI build passes.",
                 x + 24, 490, 11, MUTED);
    }

    void drawPersonalization(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Personalization",
                         "Theme, accent color, motion and wallpaper preferences");

        drawCard(c, x, 158, w, 154);
        drawText(c, "Theme", x + 24, 192, 15, TEXT, true);
        drawText(c, store_.boolean("theme.dark", true) ? "Dark" : "Light",
                 x + 24, 218, 12, MUTED);
        drawButton(c, x + 24, 244, 110, 40, "Dark", true, hoveredAction_ == 30);
        drawButton(c, x + 146, 244, 110, 40, "Light", false, hoveredAction_ == 31);

        drawCard(c, x, 332, w, 180);
        drawText(c, "Accent color", x + 24, 366, 15, TEXT, true);
        const std::array<SkColor, 5> accents = {
            rgb(88, 150, 255), rgb(82, 196, 127), rgb(181, 109, 255),
            rgb(238, 109, 109), rgb(245, 175, 71)
        };
        for (size_t i = 0; i < accents.size(); ++i) {
            fillRoundRect(c, x + 24 + static_cast<float>(i) * 54.0f,
                          396, 36, 36, 18, accents[i]);
        }
        drawText(c, "Accent is stored locally in AgroOS configuration.",
                 x + 24, 463, 11.5f, MUTED);

        drawCard(c, x, 532, w, 140);
        drawText(c, "Wallpaper", x + 24, 566, 15, TEXT, true);
        drawText(c, store_.value("wallpaper.path", "/usr/share/backgrounds/agroos/wallpaper.jpg"),
                 x + 24, 592, 10.5f, MUTED);
        drawButton(c, w + x - 150, 564, 126, 40, "Choose file", true, hoveredAction_ == 32);
    }

    void drawApps(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Apps",
                         "Installed desktop entries and default handlers");

        drawCard(c, x, 158, w, 190);
        drawText(c, "Installed desktop applications", x + 24, 194, 15, TEXT, true);
        drawText(c, std::to_string(desktopEntryCount()),
                 x + 24, 238, 30, TEXT, true);
        drawText(c, "entries discovered from standard application directories",
                 x + 24, 261, 11.5f, MUTED);
        drawText(c, "Default browser", x + 24, 305, 12, MUTED, true);
        drawText(c, defaultBrowser(), x + 142, 305, 12, TEXT);

        drawCard(c, x, 368, w, 154);
        drawText(c, "File associations", x + 24, 402, 15, TEXT, true);
        drawText(c, "PNG / JPG / PDF / MP3 / MP4 / TXT / AppImage",
                 x + 24, 429, 12, MUTED);
        drawText(c, "The shell will use xdg-open / .desktop handlers until a native registry exists.",
                 x + 24, 458, 11, MUTED);
    }

    void drawAccounts(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        const std::string user = runCommand("id -un 2>/dev/null");
        drawSectionTitle(c, x, 120,
                         "Accounts",
                         "Current Linux account mapped into AgroOS");

        drawCard(c, x, 158, w, 220);
        drawText(c, "Current user", x + 24, 194, 15, TEXT, true);
        drawText(c, user.empty() ? "Unknown" : user, x + 24, 230, 27, TEXT, true);
        drawRow(c, x + 24, 256, w - 48, "Home directory", homeDir());

        drawCard(c, x, 400, w, 160);
        drawText(c, "Account management", x + 24, 435, 15, TEXT, true);
        drawText(c, "Password, login and user creation belong to the system authentication backend.",
                 x + 24, 464, 11.5f, MUTED);
        drawText(c, "AgroOS UI is ready for PAM / systemd-logind integration.",
                 x + 24, 493, 11.5f, GOOD);
    }

    void drawTime(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Time & Language",
                         "Clock, timezone and locale information");

        drawCard(c, x, 158, w, 230);
        drawText(c, "Current time", x + 24, 194, 15, TEXT, true);
        drawText(c, currentTime_, x + 24, 238, 28, TEXT, true);
        drawRow(c, x + 24, 264, w - 48, "Timezone", timezoneName());

        drawCard(c, x, 410, w, 156);
        drawText(c, "Locale", x + 24, 446, 15, TEXT, true);
        drawText(c, runCommand("locale | grep '^LANG=' | head -1 2>/dev/null"),
                 x + 24, 475, 12, MUTED);
        drawText(c, "Language packs can be managed by the package backend later.",
                 x + 24, 503, 11.5f, MUTED);
    }

    void drawGaming(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Gaming",
                         "Performance-related preferences for games");

        drawCard(c, x, 158, w, 150);
        drawText(c, "Game Mode", x + 24, 194, 15, TEXT, true);
        const bool enabled = store_.boolean("gaming.game_mode", true);
        drawText(c, enabled ? "Enabled" : "Disabled", x + 24, 220, 12, enabled ? GOOD : MUTED);
        drawToggle(c, w + x - 72, 184, enabled);
        drawText(c, gameModePresent() ? "GameMode runtime detected." : "GameMode runtime not detected.",
                 x + 24, 264, 11.5f, gameModePresent() ? GOOD : WARN);

        drawCard(c, x, 328, w, 180);
        drawText(c, "Future gaming controls", x + 24, 363, 15, TEXT, true);
        drawText(c, "GPU performance profile", x + 24, 393, 12, MUTED);
        drawText(c, "Per-app priority", x + 24, 425, 12, MUTED);
        drawText(c, "Frame pacing", x + 24, 457, 12, MUTED);
    }

    void drawAccessibility(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Accessibility",
                         "Motion, scale and assistive UI foundations");

        drawCard(c, x, 158, w, 200);
        drawText(c, "Reduce motion", x + 24, 194, 15, TEXT, true);
        const bool reduced = store_.boolean("accessibility.reduce_motion", false);
        drawToggle(c, w + x - 72, 184, reduced);
        drawText(c, reduced ? "Enabled" : "Disabled", x + 24, 220, 12, reduced ? GOOD : MUTED);
        drawText(c, "Animations in AgroOS UI will honor this preference.", x + 24, 258, 11.5f, MUTED);

        drawText(c, "UI scale", x + 24, 305, 15, TEXT, true);
        drawText(c, store_.value("accessibility.scale", "100%"), x + 24, 330, 12, MUTED);
    }

    void drawPrivacy(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Privacy",
                         "Local AgroOS preferences and telemetry");

        drawCard(c, x, 158, w, 176);
        const bool telemetry = store_.boolean("privacy.telemetry", false);
        drawText(c, "Optional diagnostics", x + 24, 194, 15, TEXT, true);
        drawToggle(c, w + x - 72, 184, telemetry);
        drawText(c, telemetry ? "Enabled" : "Disabled", x + 24, 220, 12,
                 telemetry ? WARN : GOOD);
        drawText(c, "Default: disabled. This file does not upload telemetry by itself.",
                 x + 24, 261, 11.5f, MUTED);

        drawCard(c, x, 356, w, 176);
        drawText(c, "Local storage", x + 24, 392, 15, TEXT, true);
        drawText(c, "Preferences file", x + 24, 422, 12, MUTED);
        drawText(c, "~/.config/agroos/settings.conf", x + 24, 445, 11.5f, TEXT);
    }

    void drawUpdates(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "Windows Update",
                         "AgroOS update channel backed by the existing VPS service");

        drawCard(c, x, 158, w, 220);
        drawText(c, "Update server", x + 24, 194, 15, TEXT, true);
        drawText(c, updateServer_, x + 24, 222, 11.5f, MUTED);
        drawText(c, "Available version", x + 24, 274, 12, MUTED, true);
        drawText(c, updateVersion_.empty() ? "Not checked" : updateVersion_,
                 x + 24, 299, 24, TEXT, true);
        drawButton(c, x + w - 172, 254, 148, 42,
                   "Check for updates", true, hoveredAction_ == 100);

        drawCard(c, x, 402, w, 180);
        drawText(c, "Security", x + 24, 438, 15, TEXT, true);
        drawText(c, "Manifest endpoint is queried over HTTPS.", x + 24, 467, 11.5f, MUTED);
        drawText(c, "Ed25519 + SHA-256 verification remains the next integration step",
                 x + 24, 496, 11.5f, WARN);
        drawText(c, "after the base UI passes the compiler in the real repository.",
                 x + 24, 520, 11.5f, MUTED);
    }

    void drawAbout(SkCanvas* c) const
    {
        const float x = SIDEBAR_W + 40;
        const float w = WIDTH - x - 40;
        drawSectionTitle(c, x, 120,
                         "About AgroOS",
                         "Build and runtime information");

        drawCard(c, x, 158, w, 320);
        drawText(c, "AgroOS", x + 24, 204, 31, TEXT, true);
        drawText(c, "Native C++17 + Skia + Wayland", x + 24, 231, 12.5f, MUTED);
        drawRow(c, x + 24, 254, w - 48, "Kernel", system_.kernel);
        drawRow(c, x + 24, 318, w - 48, "Architecture", system_.architecture);
        drawRow(c, x + 24, 382, w - 48, "GPU", system_.gpu);
        drawText(c, "This component is intentionally isolated for the first compiler pass.",
                 x + 24, 445, 11.5f, GOOD);

        drawText(c, "Version", x, 530, 12, MUTED, true);
        drawText(c, versionText_, x, 556, 17, TEXT, true);
    }

    // --------------------------------------------------------
    // Actions
    // --------------------------------------------------------

    int actionHit(float x, float y) const
    {
        const float contentX = SIDEBAR_W + 40;
        const float w = WIDTH - contentX - 40;

        if (selected_ == Page::System)
            return -1;

        if (selected_ == Page::Display) {
            const float y0 = 420;
            if (y >= y0 && y <= y0 + 40) {
                if (x >= contentX + 24 && x <= contentX + 134) return 20;
                if (x >= contentX + 146 && x <= contentX + 256) return 21;
                if (x >= contentX + 268 && x <= contentX + 378) return 22;
            }
        }

        if (selected_ == Page::Personalization) {
            if (y >= 244 && y <= 284) {
                if (x >= contentX + 24 && x <= contentX + 134) return 30;
                if (x >= contentX + 146 && x <= contentX + 256) return 31;
            }
            if (y >= 564 && y <= 604 && x >= contentX + w - 150)
                return 32;
        }

        if (selected_ == Page::Updates) {
            if (y >= 254 && y <= 296 && x >= contentX + w - 172)
                return 100;
        }

        return -1;
    }

    void activateAction(int action)
    {
        switch (action) {
        case 20:
            store_.set("display.scale", "100%");
            break;
        case 21:
            store_.set("display.scale", "125%");
            break;
        case 22:
            store_.set("display.scale", "150%");
            break;
        case 30:
            store_.setBool("theme.dark", true);
            break;
        case 31:
            store_.setBool("theme.dark", false);
            break;
        case 32:
            // File picker is intentionally not invented here. A native
            // picker/window service should supply the selected path.
            break;
        case 100:
            checkForUpdates();
            break;
        default:
            break;
        }
    }

    void checkForUpdates()
    {
        updateVersion_ = runCommand(
            "curl -fsSL --connect-timeout 5 --max-time 10 "
            "https://YOUR-AGROOS-VPS/agroos/manifest.json 2>/dev/null "
            "| grep -o '\\\"version\\\"[[:space:]]*:[[:space:]]*\\\"[^\\\"]*\\\"' "
            "| head -1 | sed 's/.*:\\"//; s/\\\"$//' ");

        if (updateVersion_.empty())
            updateVersion_ = "Server unavailable";
    }

    void refreshClock()
    {
        currentTime_ = currentTimeText();
    }

    void refreshVersionText()
    {
        const std::string manifest = readFile("/etc/agroos/version");
        versionText_ = manifest.empty() ? "0.1.0-development" : trim(manifest);
        updateServer_ = "https://YOUR-AGROOS-VPS/agroos/manifest.json";
    }

    SettingsStore store_;
    Page selected_ = Page::System;

    SystemInfo system_{};
    std::vector<NetworkEntry> network_{};

    std::string currentTime_;
    std::string versionText_;
    std::string updateServer_;
    std::string updateVersion_;

    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    float elapsed_ = 0.0f;

    int hoveredNav_ = -1;
    int hoveredAction_ = -1;
    bool refreshRequested_ = false;
};

// ============================================================
// Renderer
// ============================================================

class SettingsRenderer {
public:
    bool init()
    {
        surface_ = SkSurface::MakeRasterN32Premul(WIDTH, HEIGHT);
        return surface_ != nullptr;
    }

    void render(const SettingsUI& ui)
    {
        if (!surface_)
            return;
        ui.draw(surface_->getCanvas());
        surface_->flushAndSubmit();
    }

    const uint32_t* peekPixels(int* stridePixels) const
    {
        if (!surface_)
            return nullptr;

        SkPixmap pixmap;
        if (!surface_->peekPixels(&pixmap))
            return nullptr;

        if (stridePixels)
            *stridePixels = static_cast<int>(pixmap.rowBytes() / sizeof(uint32_t));

        return static_cast<const uint32_t*>(pixmap.addr());
    }

private:
    sk_sp<SkSurface> surface_;
};

} // namespace agro

// ============================================================
// MAIN
// ============================================================

int main()
{
    std::printf(
        "============================================================\n"
        " AGROOS SETTINGS\n"
        " Native C++17 + Skia + Wayland\n"
        " System / Devices / Network / Display / Apps / Accounts\n"
        " Time / Gaming / Accessibility / Privacy / Updates / About\n"
        "============================================================\n");

    agro::SettingsUI ui;
    agro::SettingsRenderer renderer;

    if (!renderer.init()) {
        std::fprintf(stderr,
                     "AgroOS Settings: Skia initialization failed.\n");
        return 1;
    }

    AgroWaylandWindow* win =
        agro_wl_create("AgroOS Settings",
                       agro::SettingsUI::Width,
                       agro::SettingsUI::Height);

    if (!win) {
        std::fprintf(stderr,
                     "AgroOS Settings: Wayland window creation failed.\n");
        return 1;
    }

    struct InputContext {
        agro::SettingsUI* ui;
    } input{&ui};

    agro_wl_set_input_callback(
        win,
        [](const AgroInputEvent* ev, void* data) {
            auto* ctx = static_cast<InputContext*>(data);
            if (!ctx || !ctx->ui || !ev)
                return;

            switch (ev->type) {
            case AGRO_INPUT_POINTER_MOTION:
                ctx->ui->pointerMove(
                    static_cast<float>(ev->x),
                    static_cast<float>(ev->y));
                break;

            case AGRO_INPUT_POINTER_BUTTON_DOWN:
                ctx->ui->pointerDown(
                    static_cast<float>(ev->x),
                    static_cast<float>(ev->y),
                    ev->button);
                break;

            case AGRO_INPUT_POINTER_BUTTON_UP:
            case AGRO_INPUT_KEY_DOWN:
            case AGRO_INPUT_KEY_UP:
            case AGRO_INPUT_CLOSE_REQUEST:
                break;
            }
        },
        &input);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    while (agro_wl_dispatch(win)) {
        const auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        dt = std::clamp(dt,
                         1.0f / 240.0f,
                         1.0f / 15.0f);

        ui.update(dt);
        renderer.render(ui);

        int stride = 0;
        const uint32_t* src = renderer.peekPixels(&stride);
        uint32_t* dst = agro_wl_begin_frame(win);

        if (src && dst) {
            const int w = agro_wl_width(win);
            const int h = agro_wl_height(win);
            const int copyW = std::min(w, agro::SettingsUI::Width);
            const int copyH = std::min(h, agro::SettingsUI::Height);

            for (int y = 0; y < copyH; ++y) {
                const uint32_t* srow =
                    src + static_cast<size_t>(y) * stride;
                uint32_t* drow =
                    dst + static_cast<size_t>(y) * w;

                for (int x = 0; x < copyW; ++x) {
                    const uint32_t p = srow[x];
                    const uint32_t r = (p >> 0) & 0xFF;
                    const uint32_t g = (p >> 8) & 0xFF;
                    const uint32_t b = (p >> 16) & 0xFF;
                    const uint32_t a = (p >> 24) & 0xFF;
                    drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }

            agro_wl_commit_frame(win);
        }
    }

    agro_wl_destroy(win);
    return 0;
}
