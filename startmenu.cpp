// AgroOS Start Menu
// Native C++17 + Skia + Wayland + AgroUI
//
// Design goals:
//   - Windows 11-class visual hierarchy, but with a more spacious,
//     glass-first AgroOS composition.
//   - No hard-coded demo application cards.
//   - Applications are discovered from real .desktop files.
//   - User folders are taken from the real filesystem.
//   - Search filters the discovered application list.
//   - Keyboard navigation supports Esc/Enter/Up/Down.
//   - Mouse hover, selection and click are fully stateful.
//   - Launch uses gtk-launch when available and falls back to xdg-open.
//   - Native Skia vector glyphs; no SVG dependency.
//
// Important API compatibility constraints:
//   - Uses only the AgroUI symbols visible in the supplied agroui.h.
//   - Uses only the supplied agro_wayland_client.h API.
//   - Does not assume layer-shell support; positioning is handled by
//     the existing Wayland window wrapper. Layer-shell can be added
//     later in the common Wayland layer.
//
// ------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"

#include "../agroui/agroui.h"
#include "../common/agro_wayland_client.h"

namespace agro {

namespace fs = std::filesystem;

// ============================================================
// LAYOUT
// ============================================================

static constexpr int WINDOW_WIDTH = 1040;
static constexpr int WINDOW_HEIGHT = 700;

static constexpr float OUTER_MARGIN = 18.0f;
static constexpr float PANEL_RADIUS = 28.0f;

static constexpr float SEARCH_H = 52.0f;
static constexpr float SEARCH_X = 34.0f;
static constexpr float SEARCH_Y = 28.0f;
static constexpr float SEARCH_W = WINDOW_WIDTH - 68.0f;

static constexpr float SECTION_GAP = 18.0f;

static constexpr float TILE_W = 154.0f;
static constexpr float TILE_H = 74.0f;
static constexpr float TILE_GAP = 10.0f;

static constexpr float APP_W = 126.0f;
static constexpr float APP_H = 104.0f;
static constexpr float APP_GAP_X = 12.0f;
static constexpr float APP_GAP_Y = 12.0f;

static constexpr int MAX_APPS = 96;

// ============================================================
// UTILITIES
// ============================================================

static std::string homeDir()
{
    if (const char* h = std::getenv("HOME"); h && *h) {
        return h;
    }
    return "/";
}

static std::string lowerCopy(std::string s)
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

static std::string unquote(std::string s)
{
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
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
            return trim(line.substr(prefix.size()));
        }
    }

    return {};
}

static std::string desktopIdFor(const fs::path& file)
{
    return file.stem().string();
}

static std::string cleanExec(std::string exec)
{
    std::istringstream in(exec);
    std::ostringstream out;

    std::string token;
    bool first = true;

    while (in >> token) {
        if (!token.empty() && token[0] == '%') {
            if (token == "%U" || token == "%u" ||
                token == "%F" || token == "%f" ||
                token == "%i" || token == "%c" ||
                token == "%k" || token == "%D" ||
                token == "%N" || token == "%v") {
                continue;
            }
        }

        if (!first) {
            out << ' ';
        }

        out << token;
        first = false;
    }

    return out.str();
}

static void spawnExec(const std::string& command)
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

static void launchDesktopFile(
    const std::string& desktopId,
    const std::string& execLine,
    const fs::path& desktopFile)
{
    // Prefer gtk-launch when present. This preserves desktop-entry
    // environment and startup notification semantics better than
    // directly executing the parsed Exec line.
    const std::string launcher = "gtk-launch " + desktopId;

    if (access("/usr/bin/gtk-launch", X_OK) == 0 ||
        access("/bin/gtk-launch", X_OK) == 0) {
        spawnExec(launcher);
        return;
    }

    if (!execLine.empty()) {
        spawnExec(execLine);
        return;
    }

    spawnExec(
        "xdg-open \"" + desktopFile.string() + "\"");
}

static bool desktopShouldShow(const fs::path& p)
{
    // Never show malformed desktop files as fake UI entries.
    const std::string type =
        readDesktopField(p, "Type");

    if (!type.empty() && type != "Application") {
        return false;
    }

    const std::string hidden =
        lowerCopy(readDesktopField(p, "Hidden"));

    if (hidden == "true" || hidden == "1") {
        return false;
    }

    const std::string nodisplay =
        lowerCopy(readDesktopField(p, "NoDisplay"));

    if (nodisplay == "true" || nodisplay == "1") {
        return false;
    }

    const std::string name =
        readDesktopField(p, "Name");

    const std::string exec =
        readDesktopField(p, "Exec");

    return !name.empty() && !exec.empty();
}

// ============================================================
// APP MODEL
// ============================================================

struct AppItem {
    std::string id;
    std::string name;
    std::string genericName;
    std::string comment;
    std::string categories;
    std::string exec;
    fs::path desktopFile;
};

static std::vector<fs::path> desktopSearchRoots()
{
    std::vector<fs::path> roots;

    const fs::path home =
        homeDir();

    roots.push_back(
        home /
        ".local/share/applications");

    roots.push_back(
        "/usr/local/share/applications");

    roots.push_back(
        "/usr/share/applications");

    return roots;
}

static std::vector<AppItem> discoverApplications()
{
    std::vector<AppItem> apps;
    std::vector<std::string> seenIds;

    for (const fs::path& root :
         desktopSearchRoots()) {

        std::error_code ec;

        if (!fs::is_directory(root, ec)) {
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
            const fs::path file =
                it->path();

            if (file.extension() != ".desktop") {
                continue;
            }

            if (!desktopShouldShow(file)) {
                continue;
            }

            const std::string id =
                desktopIdFor(file);

            if (std::find(
                    seenIds.begin(),
                    seenIds.end(),
                    id) != seenIds.end()) {
                continue;
            }

            AppItem app;
            app.id = id;
            app.name =
                readDesktopField(file, "Name");
            app.genericName =
                readDesktopField(
                    file,
                    "GenericName");
            app.comment =
                readDesktopField(
                    file,
                    "Comment");
            app.categories =
                readDesktopField(
                    file,
                    "Categories");
            app.exec =
                cleanExec(
                    readDesktopField(
                        file,
                        "Exec"));
            app.desktopFile = file;

            apps.push_back(
                std::move(app));

            seenIds.push_back(id);

            if (apps.size() >= MAX_APPS) {
                return apps;
            }
        }
    }

    std::sort(
        apps.begin(),
        apps.end(),
        [](const AppItem& a,
           const AppItem& b) {
            return lowerCopy(a.name) <
                   lowerCopy(b.name);
        });

    return apps;
}

// ============================================================
// START MENU MODEL
// ============================================================

class StartMenuUI {
public:
    enum KeyCode : uint32_t {
        KeyEnter = 28,
        KeyBackspace = 14,
        KeyEsc = 1,
        KeyUp = 103,
        KeyDown = 108,
        KeyLeft = 105,
        KeyRight = 106,
        KeyHome = 102,
        KeyEnd = 107,
        KeySpace = 57
    };

    StartMenuUI()
    {
        apps_ =
            discoverApplications();

        refreshFilter();

        searchFocus_.start(0.0f, 1.0f);
        startupOpacity_.start(0.0f, 1.0f);
        startupSlide_.start(14.0f, 0.0f);

        tiles_.resize(5);
    }

    void update(float dt)
    {
        startupOpacity_.update(dt);
        startupSlide_.update(dt);
        searchFocus_.update(dt);
        selectionPulse_.update(dt);

        for (auto& b : tiles_) {
            b.update(dt);
        }

        elapsed_ += dt;
    }

    // --------------------------------------------------------
    // Search
    // --------------------------------------------------------

    void setSearchFocused(bool focused)
    {
        searchFocused_ = focused;

        searchFocus_.start(
            searchFocus_.value(),
            focused ? 1.0f : 0.0f);
    }

    void appendSearch(uint32_t key)
    {
        if (!searchFocused_) {
            return;
        }

        if (key == KeyBackspace) {
            if (!search_.empty()) {
                search_.pop_back();
                refreshFilter();
            }
            return;
        }

        if (key == KeyEsc) {
            search_.clear();
            refreshFilter();
            setSearchFocused(false);
            return;
        }

        if (key == KeyEnter) {
            launchSelected();
            return;
        }

        // agro_wayland_client exposes raw Linux evdev key codes.
        // We intentionally support a conservative printable mapping
        // for the standard US key positions; this keeps the component
        // usable without inventing an xkbcommon API the current wrapper
        // does not expose. Full IME/layout support belongs in the
        // shared Wayland input layer.
        const char c = evdevPrintable(key);

        if (c != 0) {
            search_.push_back(c);
            refreshFilter();
            ensureSelection();
        }
    }

    // --------------------------------------------------------
    // Navigation
    // --------------------------------------------------------

    void moveSelection(int delta)
    {
        if (filtered_.empty()) {
            selected_ = -1;
            return;
        }

        if (selected_ < 0) {
            selected_ = 0;
        } else {
            selected_ =
                std::clamp(
                    selected_ + delta,
                    0,
                    static_cast<int>(
                        filtered_.size()) - 1);
        }

        selectionPulse_.start(
            0.0f,
            1.0f);
    }

    void moveHome()
    {
        if (!filtered_.empty()) {
            selected_ = 0;
            selectionPulse_.start(0.0f, 1.0f);
        }
    }

    void moveEnd()
    {
        if (!filtered_.empty()) {
            selected_ =
                static_cast<int>(
                    filtered_.size()) - 1;
            selectionPulse_.start(0.0f, 1.0f);
        }
    }

    void launchSelected()
    {
        if (selected_ < 0 ||
            selected_ >=
                static_cast<int>(
                    filtered_.size())) {
            return;
        }

        const AppItem& app =
            filtered_[selected_];

        launchDesktopFile(
            app.id,
            app.exec,
            app.desktopFile);
    }

    // --------------------------------------------------------
    // Pointer
    // --------------------------------------------------------

    void pointerMove(
        float x,
        float y)
    {
        pointerX_ = x;
        pointerY_ = y;

        hoveredTile_ = hitTile(x, y);
        hoveredApp_ = hitApp(x, y);

        for (auto& tile : tiles_) {
            tile.setHovered(false);
        }

        if (hoveredTile_ >= 0 &&
            hoveredTile_ <
                static_cast<int>(
                    tiles_.size())) {
            tiles_[hoveredTile_]
                .setHovered(true);
        }
    }

    void pointerDown(
        float x,
        float y)
    {
        pointerMove(x, y);

        if (hitSearch(x, y)) {
            setSearchFocused(true);
            return;
        }

        const int appIndex =
            hitApp(x, y);

        if (appIndex >= 0 &&
            appIndex <
                static_cast<int>(
                    filtered_.size())) {

            selected_ = appIndex;

            selectionPulse_.start(
                0.0f,
                1.0f);

            launchSelected();
            return;
        }

        const int tile =
            hitTile(x, y);

        if (tile >= 0) {
            activateTile(tile);
            return;
        }

        // Clicking the glass surface itself removes focus from search.
        setSearchFocused(false);
    }

    // --------------------------------------------------------
    // Hit testing
    // --------------------------------------------------------

    bool hitSearch(
        float x,
        float y) const
    {
        return
            x >= SEARCH_X &&
            x <= SEARCH_X + SEARCH_W &&
            y >= SEARCH_Y &&
            y <= SEARCH_Y + SEARCH_H;
    }

    int hitTile(
        float x,
        float y) const
    {
        const float startY =
            118.0f;

        for (int i = 0; i < 5; ++i) {
            const float col =
                static_cast<float>(i % 5);

            const float tx =
                34.0f +
                col *
                (TILE_W + TILE_GAP);

            if (x >= tx &&
                x <= tx + TILE_W &&
                y >= startY &&
                y <= startY + TILE_H) {
                return i;
            }
        }

        return -1;
    }

    int hitApp(
        float x,
        float y) const
    {
        const float baseY =
            272.0f;

        const float startX =
            34.0f;

        const float available =
            WINDOW_WIDTH -
            68.0f;

        const int columns =
            std::max(
                1,
                static_cast<int>(
                    std::floor(
                        (available + APP_GAP_X) /
                        (APP_W + APP_GAP_X))));

        for (
            int i = 0;
            i < static_cast<int>(
                    filtered_.size());
            ++i
        ) {
            const int row =
                i / columns;

            const int col =
                i % columns;

            const float xx =
                startX +
                col *
                (APP_W + APP_GAP_X);

            const float yy =
                baseY +
                row *
                (APP_H + APP_GAP_Y);

            if (x >= xx &&
                x <= xx + APP_W &&
                y >= yy &&
                y <= yy + APP_H) {
                return i;
            }
        }

        return -1;
    }

    // --------------------------------------------------------
    // Filter
    // --------------------------------------------------------

    void refreshFilter()
    {
        filtered_.clear();

        const std::string q =
            lowerCopy(search_);

        for (const AppItem& app :
             apps_) {

            if (q.empty()) {
                filtered_.push_back(app);
                continue;
            }

            const std::string name =
                lowerCopy(app.name);

            const std::string generic =
                lowerCopy(
                    app.genericName);

            const std::string comment =
                lowerCopy(
                    app.comment);

            if (name.find(q) !=
                    std::string::npos ||
                generic.find(q) !=
                    std::string::npos ||
                comment.find(q) !=
                    std::string::npos) {

                filtered_.push_back(app);
            }
        }

        if (selected_ >=
            static_cast<int>(
                filtered_.size())) {
            selected_ =
                filtered_.empty()
                    ? -1
                    : 0;
        }

        ensureSelection();
    }

    void ensureSelection()
    {
        if (!filtered_.empty() &&
            selected_ < 0) {
            selected_ = 0;
        }
    }

    // --------------------------------------------------------
    // Tiles
    // --------------------------------------------------------

    void activateTile(int index)
    {
        const std::string home =
            homeDir();

        switch (index) {
            case 0:
                spawnExec(
                    "xdg-open \"" +
                    home +
                    "/Documents\"");
                break;

            case 1:
                spawnExec(
                    "xdg-open \"" +
                    home +
                    "/Downloads\"");
                break;

            case 2:
                spawnExec(
                    "xdg-open \"" +
                    home +
                    "/Pictures\"");
                break;

            case 3:
                spawnExec(
                    "xdg-open \"" +
                    home +
                    "/Desktop\"");
                break;

            case 4:
                spawnExec(
                    "xdg-open \"" +
                    home +
                    "\"");
                break;
        }
    }

private:

    // Linux evdev key positions for a compact printable fallback.
    static char evdevPrintable(uint32_t key)
    {
        static constexpr char letters[] =
            "qwertyuiop"
            "asdfghjkl"
            "zxcvbnm";

        // Number row: 1..0
        static constexpr char numbers[] =
            "1234567890";

        // key=2 => 1
        if (key >= 2 && key <= 11) {
            return numbers[key - 2];
        }

        // Standard US keyboard positions.
        if (key >= 16 && key <= 25) {
            static constexpr char row[] = "qwertyuiop";
            return row[key - 16];
        }

        if (key >= 30 && key <= 38) {
            static constexpr char row[] = "asdfghjkl";
            return row[key - 30];
        }

        if (key >= 44 && key <= 50) {
            static constexpr char row[] = "zxcvbnm";
            return row[key - 44];
        }

        if (key == KeySpace) {
            return ' ';
        }

        return 0;
    }

public:

    float startupOpacity() const
    {
        return startupOpacity_.value();
    }

    float startupSlide() const
    {
        return startupSlide_.value();
    }

    float searchFocus() const
    {
        return searchFocus_.value();
    }

    float selectionPulse() const
    {
        return selectionPulse_.value();
    }

    float pointerX() const
    {
        return pointerX_;
    }

    float pointerY() const
    {
        return pointerY_;
    }

    const std::string& searchText() const
    {
        return search_;
    }

    const std::vector<AppItem>&
    apps() const
    {
        return filtered_;
    }

    int selectedIndex() const
    {
        return selected_;
    }

    int hoveredIndex() const
    {
        return hoveredApp_;
    }

    const UIButton& tileButton(int index) const
    {
        return tiles_.at(
            static_cast<size_t>(
                index));
    }

private:
    std::vector<AppItem> apps_;
    std::vector<AppItem> filtered_;

    std::vector<UIButton> tiles_;

    int selected_ = -1;
    int hoveredApp_ = -1;
    int hoveredTile_ = -1;

    bool searchFocused_ = false;

    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;

    float elapsed_ = 0.0f;

    std::string search_;

    Animation startupOpacity_{0.30f};
    Animation startupSlide_{0.26f};
    Animation searchFocus_{0.16f};
    Animation selectionPulse_{0.20f};
};

// ============================================================
// RENDERER
// ============================================================

class StartMenuRenderer {
public:

    bool init()
    {
        const SkImageInfo info =
            SkImageInfo::Make(
                WINDOW_WIDTH,
                WINDOW_HEIGHT,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType);

        surface_ =
            SkSurfaces::Raster(info);

        return surface_ != nullptr;
    }

    void render(
        const StartMenuUI& ui)
    {
        if (!surface_) {
            return;
        }

        SkCanvas* c =
            surface_->getCanvas();

        c->clear(
            AgroColor::white(245));

        const float opacity =
            ui.startupOpacity();

        const float slide =
            ui.startupSlide();

        GlassPanelStyle style;

        style.cornerRadius =
            PANEL_RADIUS;

        style.glassAlpha = 228;
        style.highlightAlpha = 72;
        style.shadowAlpha = 35;
        style.shadowOffsetY = 5.0f;

        drawGlassPanel(
            c,
            OUTER_MARGIN,
            slide + OUTER_MARGIN,
            WINDOW_WIDTH -
                OUTER_MARGIN * 2.0f,
            WINDOW_HEIGHT -
                OUTER_MARGIN * 2.0f,
            opacity,
            style);

        drawHeader(
            c,
            ui,
            opacity);

        drawTiles(
            c,
            ui,
            opacity);

        drawApps(
            c,
            ui,
            opacity);

        drawFooter(
            c,
            ui,
            opacity);
    }

    const uint32_t* pixels(
        int* stride) const
    {
        if (!surface_) {
            return nullptr;
        }

        SkPixmap pm;

        if (!surface_->peekPixels(&pm)) {
            return nullptr;
        }

        if (stride) {
            *stride =
                static_cast<int>(
                    pm.rowBytes() / 4);
        }

        return static_cast<
            const uint32_t*
        >(pm.addr());
    }

private:

    void drawHeader(
        SkCanvas* c,
        const StartMenuUI& ui,
        float opacity)
    {
        // Search field
        SkPaint searchBg;
        searchBg.setAntiAlias(true);

        searchBg.setColor(
            AgroColor::white(
                static_cast<int>(
                    (215.0f +
                     ui.searchFocus() * 25.0f) *
                    opacity)));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    SEARCH_X,
                    SEARCH_Y +
                        ui.startupSlide(),
                    SEARCH_W,
                    SEARCH_H),
                17.0f,
                17.0f),
            searchBg);

        SkPaint border;
        border.setAntiAlias(true);
        border.setStyle(
            SkPaint::kStroke_Style);
        border.setStrokeWidth(1.0f);

        border.setColor(
            AgroColor::ink(
                static_cast<int>(
                    (30.0f +
                     ui.searchFocus() * 45.0f) *
                    opacity)));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    SEARCH_X,
                    SEARCH_Y +
                        ui.startupSlide(),
                    SEARCH_W,
                    SEARCH_H),
                17.0f,
                17.0f),
            border);

        drawSearchGlyph(
            c,
            SEARCH_X + 23.0f,
            SEARCH_Y +
                ui.startupSlide() +
                SEARCH_H * 0.5f,
            opacity);

        SkFont f;
        f.setSize(14.0f);

        SkPaint tp;
        tp.setAntiAlias(true);

        const std::string label =
            ui.searchText().empty()
                ? "Search apps, settings and files"
                : ui.searchText();

        tp.setColor(
            ui.searchText().empty()
                ? AgroColor::ink(
                    static_cast<int>(
                        135.0f * opacity))
                : AgroColor::ink(
                    static_cast<int>(
                        220.0f * opacity)));

        c->drawString(
            label.c_str(),
            SEARCH_X + 49.0f,
            SEARCH_Y +
                ui.startupSlide() +
                32.0f,
            f,
            tp);

        // Small title at the right side gives the panel a
        // product-level identity without using fake account data.
        SkFont brand;
        brand.setSize(14.5f);

        SkPaint bp;
        bp.setAntiAlias(true);
        bp.setColor(
            AgroColor::ink(
                static_cast<int>(
                    175.0f * opacity)));

        c->drawString(
            "AgroOS",
            WINDOW_WIDTH -
                OUTER_MARGIN -
                78.0f,
            SEARCH_Y +
                ui.startupSlide() +
                32.0f,
            brand,
            bp);
    }

    void drawSearchGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.2f);
        p.setStrokeCap(
            SkPaint::kRound_Cap);

        p.setColor(
            AgroColor::ink(
                static_cast<int>(
                    185.0f * opacity)));

        c->drawCircle(
            cx,
            cy,
            6.0f,
            p);

        c->drawLine(
            cx + 4.5f,
            cy + 4.5f,
            cx + 10.0f,
            cy + 10.0f,
            p);
    }

    void drawTiles(
        SkCanvas* c,
        const StartMenuUI& ui,
        float opacity)
    {
        const float y =
            118.0f +
            ui.startupSlide();

        const char* titles[] = {
            "Documents",
            "Downloads",
            "Pictures",
            "Desktop",
            "Home"
        };

        const char* subtitles[] = {
            "Your files",
            "Received files",
            "Photos and media",
            "Desktop items",
            "Personal folder"
        };

        for (int i = 0; i < 5; ++i) {
            const float x =
                34.0f +
                i *
                (TILE_W + TILE_GAP);

            const UIButton& b =
                ui.tileButton(i);

            const bool hot =
                b.hover.value() > 0.01f;

            SkPaint bg;
            bg.setAntiAlias(true);

            bg.setColor(
                AgroColor::white(
                    static_cast<int>(
                        (105.0f +
                         50.0f *
                             b.hover.value()) *
                        opacity)));

            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        x,
                        y,
                        TILE_W,
                        TILE_H),
                    17.0f,
                    17.0f),
                bg);

            if (hot) {
                SkPaint glow;
                glow.setAntiAlias(true);
                glow.setStyle(
                    SkPaint::kStroke_Style);
                glow.setStrokeWidth(1.4f);
                glow.setColor(
                    AgroColor::core(
                        static_cast<int>(
                            80.0f *
                            b.hover.value() *
                            opacity)));

                c->drawRRect(
                    SkRRect::MakeRectXY(
                        SkRect::MakeXYWH(
                            x,
                            y,
                            TILE_W,
                            TILE_H),
                        17.0f,
                        17.0f),
                    glow);
            }

            drawTileGlyph(
                c,
                x + 29.0f,
                y + 37.0f,
                i,
                opacity);

            SkFont tf;
            tf.setSize(13.0f);

            SkPaint tp;
            tp.setAntiAlias(true);
            tp.setColor(
                AgroColor::ink(
                    static_cast<int>(
                        220.0f *
                        opacity)));

            c->drawString(
                titles[i],
                x + 57.0f,
                y + 31.0f,
                tf,
                tp);

            SkFont sf;
            sf.setSize(10.5f);

            SkPaint sp;
            sp.setAntiAlias(true);
            sp.setColor(
                AgroColor::ink(
                    static_cast<int>(
                        125.0f *
                        opacity)));

            c->drawString(
                subtitles[i],
                x + 57.0f,
                y + 49.0f,
                sf,
                sp);
        }
    }

    void drawTileGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        int index,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeJoin(
            SkPaint::kRound_Join);
        p.setStrokeCap(
            SkPaint::kRound_Cap);

        p.setColor(
            AgroColor::core(
                static_cast<int>(
                    205.0f * opacity)));

        if (index == 0) {
            // document
            SkPathBuilder doc;
            doc.moveTo(cx - 7, cy - 10);
            doc.lineTo(cx + 3, cy - 10);
            doc.lineTo(cx + 8, cy - 5);
            doc.lineTo(cx + 8, cy + 10);
            doc.lineTo(cx - 7, cy + 10);
            doc.close();
            c->drawPath(doc.detach(), p);

            c->drawLine(cx - 3, cy - 2,
                        cx + 4, cy - 2, p);
            c->drawLine(cx - 3, cy + 3,
                        cx + 4, cy + 3, p);
        }
        else if (index == 1) {
            // download
            c->drawLine(cx, cy - 9,
                        cx, cy + 6, p);
            c->drawLine(cx - 6, cy,
                        cx, cy + 6, p);
            c->drawLine(cx + 6, cy,
                        cx, cy + 6, p);
            c->drawLine(cx - 8, cy + 10,
                        cx + 8, cy + 10, p);
        }
        else if (index == 2) {
            // picture
            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        cx - 10,
                        cy - 8,
                        20,
                        16),
                    3,
                    3),
                p);
            c->drawCircle(
                cx + 4,
                cy - 3,
                2.2f,
                p);
            SkPathBuilder hill;
            hill.moveTo(cx - 7, cy + 5);
            hill.lineTo(cx - 1, cy - 1);
            hill.lineTo(cx + 3, cy + 3);
            hill.lineTo(cx + 6, cy);
            c->drawPath(
                hill.detach(),
                p);
        }
        else if (index == 3) {
            // desktop
            c->drawRect(
                SkRect::MakeXYWH(
                    cx - 10,
                    cy - 7,
                    20,
                    13),
                p);
            c->drawLine(
                cx - 4,
                cy + 9,
                cx + 4,
                cy + 9,
                p);
            c->drawLine(
                cx,
                cy + 6,
                cx,
                cy + 9,
                p);
        }
        else {
            // home
            SkPathBuilder house;
            house.moveTo(cx - 10, cy);
            house.lineTo(cx, cy - 9);
            house.lineTo(cx + 10, cy);
            house.lineTo(cx + 8, cy);
            house.lineTo(cx + 8, cy + 9);
            house.lineTo(cx - 8, cy + 9);
            house.close();
            c->drawPath(
                house.detach(),
                p);
        }
    }

    void drawApps(
        SkCanvas* c,
        const StartMenuUI& ui,
        float opacity)
    {
        const auto& apps =
            ui.apps();

        const float baseY =
            272.0f +
            ui.startupSlide();

        const float startX =
            34.0f;

        const float available =
            WINDOW_WIDTH -
            68.0f;

        const int columns =
            std::max(
                1,
                static_cast<int>(
                    std::floor(
                        (available +
                         APP_GAP_X) /
                        (APP_W +
                         APP_GAP_X))));

        // section title
        SkFont sectionFont;
        sectionFont.setSize(16.0f);

        SkPaint sectionPaint;
        sectionPaint.setAntiAlias(true);
        sectionPaint.setColor(
            AgroColor::ink(
                static_cast<int>(
                    220.0f *
                    opacity)));

        c->drawString(
            ui.searchText().empty()
                ? "All apps"
                : "Search results",
            startX,
            baseY - 20.0f,
            sectionFont,
            sectionPaint);

        for (
            int i = 0;
            i < static_cast<int>(
                    apps.size());
            ++i
        ) {
            const int row =
                i / columns;

            const int col =
                i % columns;

            const float x =
                startX +
                col *
                    (APP_W +
                     APP_GAP_X);

            const float y =
                baseY +
                row *
                    (APP_H +
                     APP_GAP_Y);

            const bool selected =
                ui.selectedIndex() == i;

            const bool hovered =
                ui.hoveredIndex() == i;

            SkPaint bg;
            bg.setAntiAlias(true);

            const int alpha =
                selected
                    ? 175
                    : hovered
                          ? 155
                          : 115;

            bg.setColor(
                AgroColor::white(
                    static_cast<int>(
                        alpha *
                        opacity)));

            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        x,
                        y,
                        APP_W,
                        APP_H),
                    18.0f,
                    18.0f),
                bg);

            if (selected || hovered) {
                SkPaint outline;
                outline.setAntiAlias(true);
                outline.setStyle(
                    SkPaint::kStroke_Style);
                outline.setStrokeWidth(
                    selected ? 1.8f : 1.2f);

                outline.setColor(
                    AgroColor::core(
                        static_cast<int>(
                            (selected
                                 ? 115.0f
                                 : 55.0f) *
                            opacity)));

                c->drawRRect(
                    SkRRect::MakeRectXY(
                        SkRect::MakeXYWH(
                            x,
                            y,
                            APP_W,
                            APP_H),
                        18.0f,
                        18.0f),
                    outline);
            }

            drawAppGlyph(
                c,
                x + APP_W * 0.5f,
                y + 33.0f,
                apps[i],
                opacity);

            SkFont nameFont;
            nameFont.setSize(11.5f);

            SkPaint namePaint;
            namePaint.setAntiAlias(true);
            namePaint.setColor(
                AgroColor::ink(
                    static_cast<int>(
                        225.0f *
                        opacity)));

            const std::string name =
                apps[i].name;

            c->drawString(
                name.c_str(),
                x + 10.0f,
                y + 69.0f,
                nameFont,
                namePaint);
        }
    }

    void drawAppGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        const AppItem& app,
        float opacity)
    {
        SkPaint plate;
        plate.setAntiAlias(true);
        plate.setStyle(
            SkPaint::kFill_Style);
        plate.setColor(
            AgroColor::core(
                static_cast<int>(
                    38.0f *
                    opacity)));

        c->drawCircle(
            cx,
            cy,
            23.0f,
            plate);

        // Generate a stable, human-friendly glyph from the app name.
        // This avoids fake SVG assets while still giving each app a
        // distinct visual identity.
        const std::string lower =
            lowerCopy(app.name);

        char glyph = '?';

        if (!app.name.empty()) {
            glyph =
                static_cast<char>(
                    std::toupper(
                        static_cast<unsigned char>(
                            app.name.front())));
        }

        if (lower.find("browser") !=
                std::string::npos ||
            lower.find("firefox") !=
                std::string::npos ||
            lower.find("chrome") !=
                std::string::npos ||
            lower.find("edge") !=
                std::string::npos) {
            drawBrowserGlyph(
                c,
                cx,
                cy,
                opacity);
            return;
        }

        if (lower.find("terminal") !=
                std::string::npos ||
            lower.find("console") !=
                std::string::npos) {
            drawTerminalGlyph(
                c,
                cx,
                cy,
                opacity);
            return;
        }

        if (lower.find("file") !=
                std::string::npos ||
            lower.find("explorer") !=
                std::string::npos) {
            drawFolderGlyph(
                c,
                cx,
                cy,
                opacity);
            return;
        }

        if (lower.find("settings") !=
                std::string::npos ||
            lower.find("control") !=
                std::string::npos) {
            drawSettingsGlyph(
                c,
                cx,
                cy,
                opacity);
            return;
        }

        SkFont f;
        f.setSize(22.0f);

        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(
            AgroColor::white(
                static_cast<int>(
                    245.0f *
                    opacity)));

        const char text[2] = {
            glyph,
            '\0'
        };

        c->drawString(
            text,
            cx - 8.0f,
            cy + 8.0f,
            f,
            p);
    }

    void drawBrowserGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setColor(
            AgroColor::white(
                static_cast<int>(
                    235.0f *
                    opacity)));

        c->drawCircle(
            cx,
            cy,
            11.0f,
            p);

        c->drawLine(
            cx - 11,
            cy,
            cx + 11,
            cy,
            p);

        c->drawOval(
            SkRect::MakeXYWH(
                cx - 5,
                cy - 11,
                10,
                22),
            p);
    }

    void drawTerminalGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeCap(
            SkPaint::kRound_Cap);
        p.setColor(
            AgroColor::white(
                static_cast<int>(
                    235.0f *
                    opacity)));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    cx - 12,
                    cy - 9,
                    24,
                    18),
                4,
                4),
            p);

        c->drawLine(
            cx - 7,
            cy - 2,
            cx - 2,
            cy + 2,
            p);

        c->drawLine(
            cx - 2,
            cy + 2,
            cx - 7,
            cy + 6,
            p);

        c->drawLine(
            cx + 2,
            cy + 6,
            cx + 7,
            cy + 6,
            p);
    }

    void drawFolderGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeJoin(
            SkPaint::kRound_Join);
        p.setColor(
            AgroColor::white(
                static_cast<int>(
                    235.0f *
                    opacity)));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    cx - 12,
                    cy - 8,
                    24,
                    17),
                4,
                4),
            p);

        c->drawLine(
            cx - 9,
            cy - 8,
            cx - 2,
            cy - 12,
            p);
    }

    void drawSettingsGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        float opacity)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(
            SkPaint::kStroke_Style);
        p.setStrokeWidth(2.2f);
        p.setColor(
            AgroColor::white(
                static_cast<int>(
                    235.0f *
                    opacity)));

        c->drawCircle(
            cx,
            cy,
            8.0f,
            p);

        c->drawCircle(
            cx,
            cy,
            3.0f,
            p);

        for (int i = 0; i < 8; ++i) {
            const float angle =
                i * 3.14159265f / 4.0f;

            const float x1 =
                cx +
                std::cos(angle) *
                    9.5f;

            const float y1 =
                cy +
                std::sin(angle) *
                    9.5f;

            const float x2 =
                cx +
                std::cos(angle) *
                    12.0f;

            const float y2 =
                cy +
                std::sin(angle) *
                    12.0f;

            c->drawLine(
                x1,
                y1,
                x2,
                y2,
                p);
        }
    }

    void drawFooter(
        SkCanvas* c,
        const StartMenuUI& ui,
        float opacity)
    {
        // Footer is intentionally minimal: real user data only.
        const std::string home =
            homeDir();

        SkFont f;
        f.setSize(11.5f);

        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(
            AgroColor::ink(
                static_cast<int>(
                    105.0f *
                    opacity)));

        const std::string label =
            "Home  •  " + home;

        c->drawString(
            label.c_str(),
            34.0f,
            WINDOW_HEIGHT - 30.0f,
            f,
            p);

        SkPaint state;
        state.setAntiAlias(true);
        state.setColor(
            AgroColor::accent(
                static_cast<int>(
                    180.0f *
                    opacity)));

        c->drawCircle(
            WINDOW_WIDTH - 42.0f,
            WINDOW_HEIGHT - 34.0f,
            5.0f,
            state);
    }

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
        " AGROOS START MENU\n"
        " Native C++17 + Skia + AgroUI + Wayland\n"
        " Real .desktop application discovery / no demo entries\n"
        "============================================================\n");

    agro::StartMenuUI ui;
    agro::StartMenuRenderer renderer;

    if (!renderer.init()) {
        std::fprintf(
            stderr,
            "AgroOS Start Menu: Skia initialization failed.\n");
        return 1;
    }

    AgroWaylandWindow* win =
        agro_wl_create(
            "AgroOS Start Menu",
            agro::WINDOW_WIDTH,
            agro::WINDOW_HEIGHT);

    if (!win) {
        std::fprintf(
            stderr,
            "AgroOS Start Menu: Wayland window creation failed.\n");
        return 1;
    }

    struct InputContext {
        agro::StartMenuUI* ui;
    } input{ &ui };

    agro_wl_set_input_callback(
        win,
        [](const AgroInputEvent* ev, void* user_data) {
            auto* ctx =
                static_cast<InputContext*>(
                    user_data);

            switch (ev->type) {
                case AGRO_INPUT_POINTER_MOTION:
                    ctx->ui->pointerMove(
                        static_cast<float>(ev->x),
                        static_cast<float>(ev->y));
                    break;

                case AGRO_INPUT_POINTER_BUTTON_DOWN:
                    if (ev->button == 1) {
                        ctx->ui->pointerDown(
                            static_cast<float>(ev->x),
                            static_cast<float>(ev->y));
                    }
                    break;

                case AGRO_INPUT_POINTER_BUTTON_UP:
                    break;

                case AGRO_INPUT_KEY_DOWN:
                    if (ev->key == agro::StartMenuUI::KeyEsc) {
                        ctx->ui->appendSearch(
                            agro::StartMenuUI::KeyEsc);
                    } else if (ev->key == agro::StartMenuUI::KeyEnter) {
                        ctx->ui->appendSearch(
                            agro::StartMenuUI::KeyEnter);
                    } else if (ev->key == agro::StartMenuUI::KeyUp) {
                        ctx->ui->moveSelection(-1);
                    } else if (ev->key == agro::StartMenuUI::KeyDown) {
                        ctx->ui->moveSelection(+1);
                    } else if (ev->key == agro::StartMenuUI::KeyHome) {
                        ctx->ui->moveHome();
                    } else if (ev->key == agro::StartMenuUI::KeyEnd) {
                        ctx->ui->moveEnd();
                    } else {
                        ctx->ui->appendSearch(ev->key);
                    }
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
            renderer.pixels(&stride);

        uint32_t* dst =
            agro_wl_begin_frame(win);

        if (src && dst) {
            const int w =
                agro_wl_width(win);

            const int h =
                agro_wl_height(win);

            const int copyW =
                std::min(
                    w,
                    agro::WINDOW_WIDTH);

            const int copyH =
                std::min(
                    h,
                    agro::WINDOW_HEIGHT);

            for (int y = 0;
                 y < copyH;
                 ++y) {

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

                for (int x = 0;
                     x < copyW;
                     ++x) {

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

            agro_wl_commit_frame(win);
        }
    }

    agro_wl_destroy(win);
    return 0;
}
