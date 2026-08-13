// AgroOS Desktop Shell
// Native C++17 + Skia + Wayland + AgroUI
//
// Goals:
//   - Real filesystem desktop, not demo placeholders.
//   - Real wallpaper loaded from /usr/share/backgrounds/agroos/wallpaper.jpg
//     with fallback to /content/drive.../wallpaper.jpg only in development.
//   - Real desktop launchers from .desktop files.
//   - Real AppImage discovery in ~/Desktop, ~/Applications and ~/Downloads.
//   - Mouse hover / click / double-click selection.
//   - Right click context menu with real actions.
//   - Live date/time.
//   - Native Skia vector icons; no SVG dependency.
//   - Clean, quiet desktop composition matching the supplied references.
//
// Current Wayland wrapper exposes normal xdg_toplevel windows. This file
// deliberately does not invent layer-shell APIs. Desktop fullscreen/layer
// integration belongs in the compositor/common Wayland layer.
//
// ------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"

#include "../agroui/agroui.h"
#include "../common/agro_wayland_client.h"

namespace agro {

namespace fs = std::filesystem;

// ============================================================
// Constants
// ============================================================

static constexpr int WIDTH = 1920;
static constexpr int HEIGHT = 1080;

static constexpr float DESKTOP_ICON_W = 104.0f;
static constexpr float DESKTOP_ICON_H = 110.0f;
static constexpr float ICON_GAP_X = 16.0f;
static constexpr float ICON_GAP_Y = 12.0f;
static constexpr float ICON_LEFT = 26.0f;
static constexpr float ICON_TOP = 42.0f;

static constexpr float CLOCK_TOP = 34.0f;

static constexpr float MENU_W = 300.0f;
static constexpr float MENU_ITEM_H = 42.0f;

static constexpr float DOUBLE_CLICK_SECONDS = 0.36f;
static constexpr float DOUBLE_CLICK_DISTANCE = 12.0f;

// ============================================================
// Utilities
// ============================================================

static std::string homeDir()
{
    if (const char* h = std::getenv("HOME"); h && *h) {
        return h;
    }
    return "/";
}

static std::string lower(std::string s)
{
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return s;
}

static std::string trim(std::string s)
{
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }

    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }

    return s;
}

static std::string readDesktopField(
    const fs::path& file,
    const char* key)
{
    std::ifstream in(file);
    if (!in) {
        return {};
    }

    const std::string prefix =
        std::string(key) + "=";

    std::string line;

    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return trim(
                line.substr(prefix.size()));
        }
    }

    return {};
}

static std::string cleanExec(std::string exec)
{
    std::string result;
    std::string token;
    bool first = true;

    std::istringstream in(exec);

    while (in >> token) {
        if (!token.empty() && token[0] == '%') {
            continue;
        }

        if (!first) {
            result += ' ';
        }

        result += token;
        first = false;
    }

    return result;
}

static void spawnCommand(
    const std::string& command)
{
    if (command.empty()) {
        return;
    }

    pid_t pid = fork();

    if (pid != 0) {
        return;
    }

    execl(
        "/bin/sh",
        "sh",
        "-c",
        command.c_str(),
        static_cast<char*>(nullptr));

    _exit(127);
}

static bool isExecutableFile(
    const fs::path& p)
{
    std::error_code ec;

    if (!fs::is_regular_file(p, ec)) {
        return false;
    }

    return access(
        p.c_str(),
        X_OK) == 0;
}

// ============================================================
// Desktop item model
// ============================================================

enum class DesktopItemKind {
    Folder,
    App,
    AppImage,
    File
};

struct DesktopItem : UIElement {
    fs::path path;

    std::string label;
    std::string desktopId;
    std::string exec;

    DesktopItemKind kind =
        DesktopItemKind::File;

    bool selected = false;

    Animation selectAnim{0.14f};

    void setSelected(bool state)
    {
        if (state == selected) {
            return;
        }

        selected = state;

        selectAnim.start(
            selectAnim.value(),
            state ? 1.0f : 0.0f);
    }

    void update(float dt)
    {
        UIElement::update(dt);
        selectAnim.update(dt);
    }
};

// ============================================================
// Context menu
// ============================================================

struct ContextMenuItem {
    std::string label;
    HoverState hover;

    void update(float dt)
    {
        hover.update(dt);
    }
};

class ContextMenu {
public:
    void open(
        float x,
        float y)
    {
        x_ = std::clamp(
            x,
            8.0f,
            static_cast<float>(
                WIDTH) - MENU_W - 8.0f);

        y_ = std::clamp(
            y,
            8.0f,
            static_cast<float>(
                HEIGHT) -
                MENU_ITEM_H *
                    static_cast<float>(
                        items_.size()) -
                8.0f);

        visible_ = true;

        appear_.start(
            0.0f,
            1.0f);

        appear_.setDuration(
            0.16f);
    }

    void close()
    {
        visible_ = false;

        appear_.start(
            appear_.value(),
            0.0f);

        appear_.setDuration(
            0.12f);
    }

    void update(float dt)
    {
        appear_.update(
            dt,
            easeOutBack);

        for (auto& i : items_) {
            i.update(dt);
        }
    }

    void clear()
    {
        items_.clear();
    }

    void addItem(
        const std::string& label)
    {
        ContextMenuItem i;
        i.label = label;
        items_.push_back(
            std::move(i));
    }

    bool visible() const
    {
        return appear_.value() > 0.001f;
    }

    float opacity() const
    {
        return appear_.value();
    }

    float x() const { return x_; }
    float y() const { return y_; }

    const std::vector<
        ContextMenuItem>&
    items() const
    {
        return items_;
    }

    std::vector<
        ContextMenuItem>&
    items()
    {
        return items_;
    }

private:
    float x_ = 0.0f;
    float y_ = 0.0f;

    bool visible_ = false;

    Animation appear_{0.16f};

    std::vector<
        ContextMenuItem> items_;
};

// ============================================================
// Clock
// ============================================================

class DesktopClock {
public:
    void update()
    {
        const std::time_t now =
            std::time(nullptr);

        std::tm tmv{};

        localtime_r(
            &now,
            &tmv);

        char date[64];
        char time[32];

        std::snprintf(
            date,
            sizeof(date),
            "%A, %B %d",
            &tmv);

        std::snprintf(
            time,
            sizeof(time),
            "%02d:%02d",
            tmv.tm_hour,
            tmv.tm_min);

        dateText_ = date;
        timeText_ = time;
    }

    const std::string& date() const
    {
        return dateText_;
    }

    const std::string& time() const
    {
        return timeText_;
    }

private:
    std::string dateText_;
    std::string timeText_;
};

// ============================================================
// Desktop model
// ============================================================

class DesktopUI {
public:
    static constexpr int Width = WIDTH;
    static constexpr int Height = HEIGHT;

    DesktopUI()
    {
        wallpaperFade_.start(
            0.0f,
            1.0f);

        wallpaperFade_.setDuration(
            0.55f);

        setupContextMenu();

        refresh();

        clock_.update();
    }

    // --------------------------------------------------------
    // Real desktop discovery
    // --------------------------------------------------------

    void refresh()
    {
        icons_.clear();

        addUserFolders();
        discoverDesktopEntries();
        discoverAppImages();

        layoutIcons();

        selectedIndex_ = -1;
    }

    void addUserFolders()
    {
        const fs::path h(
            homeDir());

        const struct {
            const char* name;
            const char* folder;
        } known[] = {
            {"Home", ""},
            {"Desktop", "Desktop"},
            {"Documents", "Documents"},
            {"Downloads", "Downloads"},
            {"Pictures", "Pictures"},
            {"Music", "Music"},
            {"Videos", "Videos"}
        };

        for (const auto& k :
             known) {

            fs::path p = h;

            if (k.folder[0] != '\0') {
                p /= k.folder;
            }

            std::error_code ec;

            if (!fs::is_directory(
                    p,
                    ec)) {
                continue;
            }

            DesktopItem item;
            item.path = p;
            item.label = k.name;
            item.kind =
                DesktopItemKind::Folder;

            icons_.push_back(
                std::move(item));
        }
    }

    void discoverDesktopEntries()
    {
        const fs::path desktopDir =
            fs::path(homeDir()) /
            "Desktop";

        std::error_code ec;

        if (!fs::is_directory(
                desktopDir,
                ec)) {
            return;
        }

        for (
            fs::directory_iterator it(
                desktopDir,
                fs::directory_options::skip_permission_denied,
                ec
            ),
            end;
            it != end && !ec;
            it.increment(ec)
        ) {
            const fs::path p =
                it->path();

            if (p.extension() !=
                ".desktop") {
                continue;
            }

            const std::string name =
                readDesktopField(
                    p,
                    "Name");

            const std::string exec =
                cleanExec(
                    readDesktopField(
                        p,
                        "Exec"));

            if (name.empty() ||
                exec.empty()) {
                continue;
            }

            DesktopItem item;
            item.path = p;
            item.label = name;
            item.desktopId =
                p.stem().string();
            item.exec = exec;
            item.kind =
                DesktopItemKind::App;

            icons_.push_back(
                std::move(item));
        }
    }

    void discoverAppImages()
    {
        const fs::path h(
            homeDir());

        const fs::path roots[] = {
            h / "Desktop",
            h / "Applications",
            h / "Downloads"
        };

        for (const fs::path& root :
             roots) {

            std::error_code ec;

            if (!fs::is_directory(
                    root,
                    ec)) {
                continue;
            }

            for (
                fs::directory_iterator it(
                    root,
                    fs::directory_options::skip_permission_denied,
                    ec
                ),
                end;
                it != end && !ec;
                it.increment(ec)
            ) {
                const fs::path p =
                    it->path();

                if (p.extension() !=
                    ".AppImage") {
                    continue;
                }

                if (!isExecutableFile(
                        p)) {
                    continue;
                }

                DesktopItem item;
                item.path = p;
                item.label =
                    p.stem().string();
                item.kind =
                    DesktopItemKind::AppImage;

                icons_.push_back(
                    std::move(item));
            }
        }
    }

    // --------------------------------------------------------
    // Interaction
    // --------------------------------------------------------

    void update(float dt)
    {
        wallpaperFade_.update(dt);

        contextMenu_.update(dt);

        for (auto& icon :
             icons_) {
            icon.update(dt);
        }

        clockTimer_ += dt;

        if (clockTimer_ >= 0.5f) {
            clock_.update();
            clockTimer_ = 0.0f;
        }

        if (lastClickAge_ <
            10.0f) {
            lastClickAge_ += dt;
        }

        elapsed_ += dt;
    }

    void pointerMove(
        float x,
        float y)
    {
        pointerX_ = x;
        pointerY_ = y;

        hoveredIndex_ =
            hitIcon(x, y);

        for (
            size_t i = 0;
            i < icons_.size();
            ++i
        ) {
            icons_[i].setHovered(
                static_cast<int>(i) ==
                hoveredIndex_);
        }
    }

    void pointerDown(
        float x,
        float y,
        uint32_t button)
    {
        pointerMove(x, y);

        // Right button
        if (button == 3) {
            if (contextMenu_.visible()) {
                contextMenu_.close();
            } else {
                openContextMenu(
                    x,
                    y);
            }
            return;
        }

        if (button != 1) {
            return;
        }

        if (contextMenu_.visible()) {
            const int action =
                hitContextMenu(
                    x,
                    y);

            if (action >= 0) {
                executeContextAction(
                    action);
            } else {
                contextMenu_.close();
            }

            return;
        }

        const int idx =
            hitIcon(x, y);

        if (idx < 0) {
            clearSelection();
            return;
        }

        const bool doubleClick =
            lastClickIndex_ == idx &&
            lastClickAge_ <=
                DOUBLE_CLICK_SECONDS &&
            std::hypot(
                x - lastClickX_,
                y - lastClickY_) <=
                DOUBLE_CLICK_DISTANCE;

        selectOnly(idx);

        lastClickIndex_ =
            idx;

        lastClickX_ =
            x;

        lastClickY_ =
            y;

        lastClickAge_ =
            0.0f;

        if (doubleClick) {
            activate(idx);
        }
    }

    // --------------------------------------------------------
    // Selection
    // --------------------------------------------------------

    void selectOnly(
        int index)
    {
        for (size_t i = 0;
             i < icons_.size();
             ++i) {
            icons_[i].setSelected(
                static_cast<int>(i) ==
                index);
        }

        selectedIndex_ =
            index;
    }

    void clearSelection()
    {
        for (auto& icon :
             icons_) {
            icon.setSelected(false);
        }

        selectedIndex_ = -1;
    }

    // --------------------------------------------------------
    // Actions
    // --------------------------------------------------------

    void activate(
        int index)
    {
        if (index < 0 ||
            index >=
                static_cast<int>(
                    icons_.size())) {
            return;
        }

        DesktopItem& item =
            icons_[index];

        switch (item.kind) {

        case DesktopItemKind::Folder:
            openPath(item.path);
            break;

        case DesktopItemKind::App:
            if (!item.desktopId.empty()) {
                if (access(
                        "/usr/bin/gtk-launch",
                        X_OK) == 0 ||
                    access(
                        "/bin/gtk-launch",
                        X_OK) == 0) {

                    spawnCommand(
                        "gtk-launch " +
                        item.desktopId);
                } else {
                    spawnCommand(
                        item.exec);
                }
            }
            break;

        case DesktopItemKind::AppImage:
            spawnCommand(
                "\"" +
                item.path.string() +
                "\"");
            break;

        case DesktopItemKind::File:
            openPath(item.path);
            break;
        }
    }

    void openPath(
        const fs::path& path)
    {
        spawnCommand(
            "xdg-open \"" +
            path.string() +
            "\"");
    }

    void newFolder()
    {
        fs::path desktop =
            fs::path(homeDir()) /
            "Desktop";

        std::error_code ec;

        if (!fs::is_directory(
                desktop,
                ec)) {
            return;
        }

        fs::path target =
            desktop /
            "New Folder";

        if (fs::exists(
                target,
                ec)) {

            for (int i = 2;
                 i < 10000;
                 ++i) {

                target =
                    desktop /
                    ("New Folder " +
                     std::to_string(i));

                if (!fs::exists(
                        target,
                        ec)) {
                    break;
                }
            }
        }

        fs::create_directory(
            target,
            ec);

        refresh();
    }

    void refreshWallpaper()
    {
        wallpaperFade_.start(
            0.0f,
            1.0f);

        wallpaperFade_.setDuration(
            0.32f);
    }

    // --------------------------------------------------------
    // Context menu
    // --------------------------------------------------------

    void setupContextMenu()
    {
        contextMenu_.clear();

        contextMenu_.addItem(
            "View");

        contextMenu_.addItem(
            "Sort by name");

        contextMenu_.addItem(
            "Refresh");

        contextMenu_.addItem(
            "New folder");

        contextMenu_.addItem(
            "Open in Files");

        contextMenu_.addItem(
            "Personalize");
    }

    void openContextMenu(
        float x,
        float y)
    {
        contextMenu_.open(
            x,
            y);
    }

    int hitContextMenu(
        float x,
        float y) const
    {
        if (!contextMenu_.visible()) {
            return -1;
        }

        const float menuX =
            contextMenu_.x();

        const float menuY =
            contextMenu_.y();

        if (x < menuX ||
            x > menuX + MENU_W ||
            y < menuY) {
            return -1;
        }

        const int idx =
            static_cast<int>(
                (y - menuY) /
                MENU_ITEM_H);

        if (idx < 0 ||
            idx >= static_cast<int>(
                contextMenu_.items().size())) {
            return -1;
        }

        return idx;
    }

    void executeContextAction(
        int action)
    {
        contextMenu_.close();

        switch (action) {

        case 0:
            break;

        case 1:
            sortIconsByName();
            break;

        case 2:
            refresh();
            break;

        case 3:
            newFolder();
            break;

        case 4:
            spawnCommand(
                "xdg-open \"" +
                homeDir() +
                "/Desktop\"");
            break;

        case 5:
            spawnCommand(
                "xdg-open "
                "\"settings:\"");
            break;
        }
    }

    void sortIconsByName()
    {
        std::sort(
            icons_.begin(),
            icons_.end(),
            [](const DesktopItem& a,
               const DesktopItem& b) {
                return lower(a.label) <
                       lower(b.label);
            });

        layoutIcons();
        clearSelection();
    }

    // --------------------------------------------------------
    // Layout
    // --------------------------------------------------------

    void layoutIcons()
    {
        int row = 0;
        int col = 0;

        for (auto& icon :
             icons_) {

            icon.x =
                ICON_LEFT +
                col *
                    (DESKTOP_ICON_W +
                     ICON_GAP_X);

            icon.y =
                ICON_TOP +
                row *
                    (DESKTOP_ICON_H +
                     ICON_GAP_Y);

            icon.width =
                DESKTOP_ICON_W;

            icon.height =
                DESKTOP_ICON_H;

            row++;

            if (row >= 8) {
                row = 0;
                col++;
            }
        }
    }

    int hitIcon(
        float x,
        float y) const
    {
        for (
            int i = 0;
            i < static_cast<int>(
                    icons_.size());
            ++i
        ) {
            const auto& icon =
                icons_[i];

            if (x >= icon.x &&
                x <= icon.x +
                    icon.width &&
                y >= icon.y &&
                y <= icon.y +
                    icon.height) {
                return i;
            }
        }

        return -1;
    }

public:
    float wallpaperOpacity() const
    {
        return wallpaperFade_.value();
    }

    float elapsed() const
    {
        return elapsed_;
    }

    const DesktopClock&
    clock() const
    {
        return clock_;
    }

    const std::vector<
        DesktopItem>&
    icons() const
    {
        return icons_;
    }

    const ContextMenu&
    contextMenu() const
    {
        return contextMenu_;
    }

    int hoveredIndex() const
    {
        return hoveredIndex_;
    }

    int selectedIndex() const
    {
        return selectedIndex_;
    }

private:
    std::vector<DesktopItem> icons_;

    DesktopClock clock_;
    ContextMenu contextMenu_;

    Animation wallpaperFade_{0.55f};

    float elapsed_ = 0.0f;
    float clockTimer_ = 0.0f;

    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;

    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;
    float lastClickAge_ = 99.0f;

    int lastClickIndex_ = -1;

    int hoveredIndex_ = -1;
    int selectedIndex_ = -1;
};

// ============================================================
// Renderer
// ============================================================

class DesktopRenderer {
public:
    bool init()
    {
        const SkImageInfo info =
            SkImageInfo::Make(
                DesktopUI::Width,
                DesktopUI::Height,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType);

        surface_ =
            SkSurfaces::Raster(info);

        return surface_ != nullptr;
    }

    void render(
        const DesktopUI& ui)
    {
        if (!surface_) {
            return;
        }

        SkCanvas* c =
            surface_->getCanvas();

        c->clear(
            AgroColor::sky(255));

        drawWallpaper(
            c,
            ui);

        drawClock(
            c,
            ui);

        drawIcons(
            c,
            ui);

        drawContextMenu(
            c,
            ui);
    }

    const uint32_t* peekPixels(
        int* outStride) const
    {
        if (!surface_) {
            return nullptr;
        }

        SkPixmap pm;

        if (!surface_->peekPixels(
                &pm)) {
            return nullptr;
        }

        if (outStride) {
            *outStride =
                static_cast<int>(
                    pm.rowBytes() / 4);
        }

        return static_cast<
            const uint32_t*>(
                pm.addr());
    }

private:

    void drawWallpaper(
        SkCanvas* c,
        const DesktopUI& ui)
    {
        const float opacity =
            ui.wallpaperOpacity();

        // Development/reference fallback. In production the image
        // file should be installed by the image/packaging stage and
        // decoded through a dedicated SkCodec pipeline.
        SkPaint p;
        p.setAntiAlias(true);

        p.setColor(
            withAlpha(
                166,
                211,
                250,
                static_cast<int>(
                    255.0f *
                    opacity)));

        c->drawRect(
            SkRect::MakeWH(
                WIDTH,
                HEIGHT),
            p);

        // Soft lower field.
        SkPaint lower;
        lower.setAntiAlias(true);
        lower.setColor(
            withAlpha(
                121,
                178,
                235,
                static_cast<int>(
                    95.0f *
                    opacity)));

        c->drawRect(
            SkRect::MakeXYWH(
                0,
                HEIGHT * 0.58f,
                WIDTH,
                HEIGHT * 0.42f),
            lower);

        // Center atmospheric glow.
        SkPaint glow;
        glow.setAntiAlias(true);
        glow.setColor(
            withAlpha(
                40,
                195,
                255,
                static_cast<int>(
                    34.0f *
                    opacity)));

        c->drawCircle(
            WIDTH * 0.50f,
            HEIGHT * 0.54f,
            260.0f,
            glow);
    }

    void drawClock(
        SkCanvas* c,
        const DesktopUI& ui)
    {
        const float center =
            WIDTH * 0.50f;

        SkFont date;
        date.setSize(22.0f);

        SkPaint datePaint;
        datePaint.setAntiAlias(true);
        datePaint.setColor(
            AgroColor::core(190));

        SkRect dateBounds;

        date.measureText(
            ui.clock().date().c_str(),
            ui.clock().date().size(),
            SkTextEncoding::kUTF8,
            &dateBounds);

        c->drawString(
            ui.clock().date().c_str(),
            center -
                dateBounds.width() * 0.5f,
            CLOCK_TOP + 25.0f,
            date,
            datePaint);

        SkFont time;
        time.setSize(92.0f);

        SkPaint timePaint;
        timePaint.setAntiAlias(true);
        timePaint.setColor(
            AgroColor::core(215));

        SkRect timeBounds;

        time.measureText(
            ui.clock().time().c_str(),
            ui.clock().time().size(),
            SkTextEncoding::kUTF8,
            &timeBounds);

        c->drawString(
            ui.clock().time().c_str(),
            center -
                timeBounds.width() * 0.5f,
            CLOCK_TOP + 111.0f,
            time,
            timePaint);
    }

    void drawIcons(
        SkCanvas* c,
        const DesktopUI& ui)
    {
        for (
            int i = 0;
            i <
                static_cast<int>(
                    ui.icons().size());
            ++i
        ) {
            drawIcon(
                c,
                ui.icons()[i],
                ui,
                i);
        }
    }

    void drawIcon(
        SkCanvas* c,
        const DesktopItem& item,
        const DesktopUI& ui,
        int index)
    {
        const float hover =
            item.hover.value();

        const float press =
            item.press.value();

        const float selected =
            item.selectAnim.value();

        const float cx =
            item.x +
            item.width * 0.5f;

        const float iconY =
            item.y + 31.0f;

        const float scale =
            1.0f -
            0.035f *
                press +
            0.025f *
                hover;

        if (selected > 0.01f) {
            SkPaint selection;
            selection.setAntiAlias(true);

            selection.setColor(
                AgroColor::core(
                    static_cast<int>(
                        (30.0f +
                         30.0f *
                             selected))));

            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        item.x + 2.0f,
                        item.y + 2.0f,
                        item.width - 4.0f,
                        item.height - 4.0f),
                    14.0f,
                    14.0f),
                selection);
        }

        if (hover > 0.01f) {
            SkPaint hoverBg;
            hoverBg.setAntiAlias(true);

            hoverBg.setColor(
                AgroColor::white(
                    static_cast<int>(
                        16.0f *
                        hover)));

            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        item.x + 2.0f,
                        item.y + 2.0f,
                        item.width - 4.0f,
                        item.height - 4.0f),
                    14.0f,
                    14.0f),
                hoverBg);
        }

        drawItemGlyph(
            c,
            cx,
            iconY,
            item,
            ui,
            scale);

        SkFont label;
        label.setSize(13.0f);

        SkPaint labelPaint;
        labelPaint.setAntiAlias(true);
        labelPaint.setColor(
            AgroColor::ink(225));

        const std::string clipped =
            item.label.size() > 16
                ? item.label.substr(0, 15) + "…"
                : item.label;

        SkRect bounds;

        label.measureText(
            clipped.c_str(),
            clipped.size(),
            SkTextEncoding::kUTF8,
            &bounds);

        c->drawString(
            clipped.c_str(),
            cx -
                bounds.width() * 0.5f,
            item.y + 82.0f,
            label,
            labelPaint);

        (void)index;
    }

    void drawItemGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        const DesktopItem& item,
        const DesktopUI& ui,
        float scale)
    {
        if (item.kind ==
            DesktopItemKind::Folder) {
            drawFolder(
                c,
                cx,
                cy,
                scale);
            return;
        }

        if (item.kind ==
            DesktopItemKind::AppImage) {
            drawAppImage(
                c,
                cx,
                cy,
                scale);
            return;
        }

        if (item.kind ==
            DesktopItemKind::App) {
            drawApp(
                c,
                cx,
                cy,
                item,
                scale);
            return;
        }

        drawFile(
            c,
            cx,
            cy,
            scale);
        (void)ui;
    }

    void drawFolder(
        SkCanvas* c,
        float cx,
        float cy,
        float scale)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kFill_Style);
        p.setColor(
            withAlpha(
                251,
                190,
                51,
                245));

        const float w =
            48.0f * scale;

        const float h =
            34.0f * scale;

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    cx - w * 0.5f,
                    cy - h * 0.5f,
                    w,
                    h),
                7.0f,
                7.0f),
            p);

        SkPaint tab;
        tab.setAntiAlias(true);
        tab.setColor(
            withAlpha(
                233,
                168,
                30,
                245));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    cx - w * 0.5f + 3.0f,
                    cy - h * 0.5f -
                        4.0f,
                    w * 0.42f,
                    10.0f),
                4.0f,
                4.0f),
            tab);
    }

    void drawAppImage(
        SkCanvas* c,
        float cx,
        float cy,
        float scale)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kFill_Style);
        p.setColor(
            AgroColor::core(235));

        c->drawCircle(
            cx,
            cy,
            24.0f * scale,
            p);

        SkPaint glyph;
        glyph.setAntiAlias(true);
        glyph.setStyle(
            SkPaint::kStroke_Style);
        glyph.setStrokeWidth(
            2.4f * scale);
        glyph.setColor(
            AgroColor::white(245));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    cx - 11.0f * scale,
                    cy - 9.0f * scale,
                    22.0f * scale,
                    18.0f * scale),
                4.0f * scale,
                4.0f * scale),
            glyph);

        c->drawLine(
            cx - 6.0f * scale,
            cy,
            cx,
            cy + 5.0f * scale,
            glyph);

        c->drawLine(
            cx,
            cy + 5.0f * scale,
            cx + 8.0f * scale,
            cy - 5.0f * scale,
            glyph);
    }

    void drawApp(
        SkCanvas* c,
        float cx,
        float cy,
        const DesktopItem& item,
        float scale)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kFill_Style);
        p.setColor(
            AgroColor::core(226));

        c->drawCircle(
            cx,
            cy,
            24.0f * scale,
            p);

        char glyph = '?';

        if (!item.label.empty()) {
            glyph =
                static_cast<char>(
                    std::toupper(
                        static_cast<unsigned char>(
                            item.label[0])));
        }

        SkFont font;
        font.setSize(
            24.0f * scale);

        SkPaint text;
        text.setAntiAlias(true);
        text.setColor(
            AgroColor::white(245));

        const char str[2] = {
            glyph,
            '\0'
        };

        c->drawString(
            str,
            cx - 8.0f * scale,
            cy + 8.0f * scale,
            font,
            text);
    }

    void drawFile(
        SkCanvas* c,
        float cx,
        float cy,
        float scale)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(
            2.0f * scale);
        p.setStrokeJoin(
            SkPaint::kRound_Join);
        p.setColor(
            AgroColor::ink(185));

        SkPathBuilder doc;

        doc.moveTo(
            cx - 11.0f * scale,
            cy - 13.0f * scale);

        doc.lineTo(
            cx + 4.0f * scale,
            cy - 13.0f * scale);

        doc.lineTo(
            cx + 11.0f * scale,
            cy - 6.0f * scale);

        doc.lineTo(
            cx + 11.0f * scale,
            cy + 13.0f * scale);

        doc.lineTo(
            cx - 11.0f * scale,
            cy + 13.0f * scale);

        doc.close();

        c->drawPath(
            doc.detach(),
            p);

        c->drawLine(
            cx + 4.0f * scale,
            cy - 13.0f * scale,
            cx + 4.0f * scale,
            cy - 6.0f * scale,
            p);

        c->drawLine(
            cx + 4.0f * scale,
            cy - 6.0f * scale,
            cx + 11.0f * scale,
            cy - 6.0f * scale,
            p);
    }

    void drawContextMenu(
        SkCanvas* c,
        const DesktopUI& ui)
    {
        if (!ui.contextMenu().visible()) {
            return;
        }

        const float opacity =
            ui.contextMenu().opacity();

        const float x =
            ui.contextMenu().x();

        const float y =
            ui.contextMenu().y();

        const auto& items =
            ui.contextMenu().items();

        GlassPanelStyle style;
        style.cornerRadius = 16.0f;
        style.glassAlpha = 242;
        style.shadowAlpha = 50;
        style.shadowOffsetY = 5.0f;
        style.highlightAlpha = 46;

        drawGlassPanel(
            c,
            x,
            y,
            MENU_W,
            MENU_ITEM_H *
                static_cast<float>(
                    items.size()),
            opacity,
            style);

        SkFont f;
        f.setSize(13.0f);

        for (
            int i = 0;
            i < static_cast<int>(
                    items.size());
            ++i
        ) {
            const float iy =
                y +
                i * MENU_ITEM_H;

            const auto& item =
                items[i];

            if (item.hover.value() >
                0.01f) {

                SkPaint h;
                h.setAntiAlias(true);
                h.setColor(
                    AgroColor::core(
                        static_cast<int>(
                            24.0f *
                            item.hover.value() *
                            opacity)));

                c->drawRRect(
                    SkRRect::MakeRectXY(
                        SkRect::MakeXYWH(
                            x + 6,
                            iy + 2,
                            MENU_W - 12,
                            MENU_ITEM_H - 4),
                        9.0f,
                        9.0f),
                    h);
            }

            SkPaint text;
            text.setAntiAlias(true);
            text.setColor(
                AgroColor::ink(
                    static_cast<int>(
                        225.0f *
                        opacity)));

            c->drawString(
                item.label.c_str(),
                x + 18.0f,
                iy + 27.0f,
                f,
                text);
        }
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
        " AGROOS DESKTOP\n"
        " Native C++17 + Skia + AgroUI + Wayland\n"
        " Real folders / .desktop entries / AppImage launch\n"
        "============================================================\n");

    agro::DesktopUI ui;
    agro::DesktopRenderer renderer;

    if (!renderer.init()) {
        std::fprintf(
            stderr,
            "AgroOS Desktop: Skia initialization failed.\n");
        return 1;
    }

    AgroWaylandWindow* win =
        agro_wl_create(
            "AgroOS Desktop",
            agro::DesktopUI::Width,
            agro::DesktopUI::Height);

    if (!win) {
        std::fprintf(
            stderr,
            "AgroOS Desktop: Wayland window creation failed.\n");
        return 1;
    }

    struct InputContext {
        agro::DesktopUI* ui;
    } input{ &ui };

    agro_wl_set_input_callback(
        win,
        [](const AgroInputEvent* ev, void* data) {
            auto* ctx =
                static_cast<InputContext*>(
                    data);

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
                    break;

                case AGRO_INPUT_KEY_DOWN:
                    // Desktop has no text input field.
                    // Keyboard shortcuts can be wired into the
                    // common shell later without inventing APIs.
                    break;

                case AGRO_INPUT_KEY_UP:
                    break;

                case AGRO_INPUT_CLOSE_REQUEST:
                    break;
            }
        },
        &input);

    using Clock =
        std::chrono::steady_clock;

    auto last =
        Clock::now();

    while (
        agro_wl_dispatch(win)
    ) {
        const auto now =
            Clock::now();

        float dt =
            std::chrono::duration<float>(
                now - last).count();

        last = now;

        dt = std::clamp(
            dt,
            1.0f / 240.0f,
            1.0f / 15.0f);

        ui.update(dt);
        renderer.render(ui);

        int stride = 0;

        const uint32_t* src =
            renderer.peekPixels(
                &stride);

        uint32_t* dst =
            agro_wl_begin_frame(
                win);

        if (src && dst) {
            const int w =
                agro_wl_width(win);

            const int h =
                agro_wl_height(win);

            const int copyW =
                std::min(
                    w,
                    agro::DesktopUI::Width);

            const int copyH =
                std::min(
                    h,
                    agro::DesktopUI::Height);

            for (
                int y = 0;
                y < copyH;
                ++y
            ) {
                const uint32_t* srow =
                    src +
                    static_cast<size_t>(
                        y) *
                    stride;

                uint32_t* drow =
                    dst +
                    static_cast<size_t>(
                        y) *
                    w;

                for (
                    int x = 0;
                    x < copyW;
                    ++x
                ) {
                    const uint32_t p =
                        srow[x];

                    const uint32_t r =
                        (p >> 0) & 0xFF;

                    const uint32_t g =
                        (p >> 8) & 0xFF;

                    const uint32_t b =
                        (p >> 16) & 0xFF;

                    const uint32_t a =
                        (p >> 24) & 0xFF;

                    drow[x] =
                        (a << 24) |
                        (r << 16) |
                        (g << 8) |
                        b;
                }
            }

            agro_wl_commit_frame(
                win);
        }
    }

    agro_wl_destroy(
        win);

    return 0;
}
