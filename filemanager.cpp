// AgroOS File Manager
// Native C++17 + Skia + Wayland
// Real filesystem UI. No demo data.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <pwd.h>
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

// ------------------------------------------------------------
// Window / layout
// ------------------------------------------------------------

static constexpr int WINDOW_WIDTH  = 1366;
static constexpr int WINDOW_HEIGHT = 820;

static constexpr float TITLEBAR_H = 54.0f;
static constexpr float TOOLBAR_H = 54.0f;
static constexpr float SIDEBAR_W = 232.0f;
static constexpr float CONTENT_PAD = 18.0f;

static constexpr float ROW_H = 58.0f;
static constexpr float ROW_RADIUS = 10.0f;

static constexpr float HEADER_Y = TITLEBAR_H + TOOLBAR_H;
static constexpr float LIST_X = SIDEBAR_W + CONTENT_PAD;
static constexpr float LIST_W = WINDOW_WIDTH - LIST_X - CONTENT_PAD;

static constexpr int MAX_VISIBLE_ENTRIES = 3000;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static std::string homeDir()
{
    if (const char* h = std::getenv("HOME"); h && *h) {
        return h;
    }

    if (passwd* p = getpwuid(getuid()); p && p->pw_dir) {
        return p->pw_dir;
    }

    return "/";
}

static std::string lowerCopy(std::string s)
{
    std::transform(
        s.begin(), s.end(), s.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );
    return s;
}

static std::string bytesText(std::uintmax_t v)
{
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    const double tb = gb * 1024.0;

    char b[64];

    if (v >= static_cast<std::uintmax_t>(tb)) {
        std::snprintf(b, sizeof(b), "%.1f TB", v / tb);
    } else if (v >= static_cast<std::uintmax_t>(gb)) {
        std::snprintf(b, sizeof(b), "%.1f GB", v / gb);
    } else if (v >= static_cast<std::uintmax_t>(mb)) {
        std::snprintf(b, sizeof(b), "%.1f MB", v / mb);
    } else if (v >= static_cast<std::uintmax_t>(kb)) {
        std::snprintf(b, sizeof(b), "%.1f KB", v / kb);
    } else {
        std::snprintf(b, sizeof(b), "%llu B",
            static_cast<unsigned long long>(v));
    }

    return b;
}

static std::string mtimeText(const fs::file_time_type& ft)
{
    try {
        const auto systemTime =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() +
                std::chrono::system_clock::now());

        const std::time_t t =
            std::chrono::system_clock::to_time_t(systemTime);

        std::tm tmv{};
        localtime_r(&t, &tmv);

        char b[64];
        std::snprintf(
            b, sizeof(b),
            "%02d.%02d.%04d  %02d:%02d",
            tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900,
            tmv.tm_hour, tmv.tm_min);

        return b;
    } catch (...) {
        return "-";
    }
}

static void spawnOpen(const std::string& target)
{
    pid_t pid = fork();

    if (pid < 0) {
        return;
    }

    if (pid == 0) {
        execlp(
            "xdg-open",
            "xdg-open",
            target.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
}

static void spawnTerminal(const std::string& dir)
{
    const char* terminals[] = {
        "foot",
        "kitty",
        "alacritty",
        "konsole",
        "gnome-terminal"
    };

    for (const char* term : terminals) {
        if (access(("/usr/bin/" + std::string(term)).c_str(), X_OK) == 0 ||
            access(("/bin/" + std::string(term)).c_str(), X_OK) == 0) {

            pid_t pid = fork();

            if (pid == 0) {
                execlp(term, term, "--working-directory", dir.c_str(),
                       static_cast<char*>(nullptr));
                _exit(127);
            }

            return;
        }
    }

    spawnOpen(dir);
}

// ------------------------------------------------------------
// Data model
// ------------------------------------------------------------

enum class EntryKind {
    Folder,
    Image,
    Video,
    Audio,
    Archive,
    Document,
    Code,
    Generic
};

struct Entry {
    fs::path path;
    std::string name;
    std::string extension;
    std::string size;
    std::string modified;

    EntryKind kind = EntryKind::Generic;
    bool directory = false;
    bool hidden = false;
};

static EntryKind kindFor(const fs::path& p, bool directory)
{
    if (directory) {
        return EntryKind::Folder;
    }

    const std::string e = lowerCopy(p.extension().string());

    if (e == ".png" || e == ".jpg" || e == ".jpeg" ||
        e == ".webp" || e == ".gif" || e == ".bmp" ||
        e == ".svg") {
        return EntryKind::Image;
    }

    if (e == ".mp4" || e == ".mkv" || e == ".webm" ||
        e == ".avi" || e == ".mov") {
        return EntryKind::Video;
    }

    if (e == ".mp3" || e == ".wav" || e == ".ogg" ||
        e == ".flac" || e == ".m4a") {
        return EntryKind::Audio;
    }

    if (e == ".zip" || e == ".tar" || e == ".gz" ||
        e == ".bz2" || e == ".xz" || e == ".7z" ||
        e == ".rar") {
        return EntryKind::Archive;
    }

    if (e == ".pdf" || e == ".doc" || e == ".docx" ||
        e == ".odt" || e == ".txt" || e == ".md") {
        return EntryKind::Document;
    }

    if (e == ".c" || e == ".cc" || e == ".cpp" ||
        e == ".h" || e == ".hpp" || e == ".py" ||
        e == ".js" || e == ".ts" || e == ".rs" ||
        e == ".go" || e == ".sh" || e == ".qml") {
        return EntryKind::Code;
    }

    return EntryKind::Generic;
}

enum class SortMode {
    Name,
    Modified,
    Size
};

enum class SidebarId {
    Home,
    Desktop,
    Documents,
    Downloads,
    Pictures,
    Music,
    Videos,
    Root
};

struct SidebarItem {
    SidebarId id;
    const char* label;
    fs::path path;
};

// ------------------------------------------------------------
// FileManager model
// ------------------------------------------------------------

class FileManagerUI {
public:
    FileManagerUI()
    {
        currentPath_ = homeDir();

        toolbar_.resize(5);
        sidebar_.resize(8);

        startupOpacity_.start(0.0f, 1.0f);
        startupSlide_.start(12.0f, 0.0f);

        setupSidebar();
        refresh();
    }

    void setupSidebar()
    {
        const fs::path h(homeDir());

        sidebar_[0] = { SidebarId::Home, "Home", h };
        sidebar_[1] = { SidebarId::Desktop, "Desktop", h / "Desktop" };
        sidebar_[2] = { SidebarId::Documents, "Documents", h / "Documents" };
        sidebar_[3] = { SidebarId::Downloads, "Downloads", h / "Downloads" };
        sidebar_[4] = { SidebarId::Pictures, "Pictures", h / "Pictures" };
        sidebar_[5] = { SidebarId::Music, "Music", h / "Music" };
        sidebar_[6] = { SidebarId::Videos, "Videos", h / "Videos" };
        sidebar_[7] = { SidebarId::Root, "File System", "/" };
    }

    void update(float dt)
    {
        startupOpacity_.update(dt);
        startupSlide_.update(dt);
        searchFocus_.update(dt);
        selectionAnim_.update(dt);
        toastAnim_.update(dt);

        for (auto& b : toolbar_) {
            b.update(dt);
        }

        elapsed_ += dt;

        if (lastClickAge_ < 10.0f) {
            lastClickAge_ += dt;
        }
    }

    // --------------------------------------------------------
    // Filesystem
    // --------------------------------------------------------

    void refresh()
    {
        std::error_code ec;

        if (!fs::is_directory(currentPath_, ec)) {
            currentPath_ = homeDir();

            if (!fs::is_directory(currentPath_, ec)) {
                currentPath_ = "/";
            }
        }

        entries_.clear();

        try {
            fs::directory_iterator it(
                currentPath_,
                fs::directory_options::skip_permission_denied,
                ec);

            const fs::directory_iterator end;

            for (; it != end && !ec; it.increment(ec)) {
                const fs::directory_entry& de = *it;

                std::error_code e;

                const std::string name = de.path().filename().string();
                if (name.empty()) {
                    continue;
                }

                const bool hidden = !name.empty() && name[0] == '.';

                if (hidden && !showHidden_) {
                    continue;
                }

                Entry item;
                item.path = de.path();
                item.name = name;
                item.hidden = hidden;
                item.directory = de.is_directory(e);
                item.kind = kindFor(item.path, item.directory);

                if (!item.directory) {
                    std::error_code sizeEc;
                    const auto s = de.file_size(sizeEc);
                    item.size = sizeEc ? "-" : bytesText(s);
                }

                std::error_code timeEc;
                const auto mt = de.last_write_time(timeEc);
                item.modified = timeEc ? "-" : mtimeText(mt);

                item.extension = lowerCopy(item.path.extension().string());

                entries_.push_back(std::move(item));

                if (entries_.size() >= MAX_VISIBLE_ENTRIES) {
                    break;
                }
            }
        } catch (...) {
            // Keep a usable empty view if a directory cannot be read.
        }

        applyFilter();
        selected_ = -1;
    }

    void navigateTo(const fs::path& path, bool addHistory = true)
    {
        std::error_code ec;

        if (!fs::is_directory(path, ec)) {
            return;
        }

        fs::path abs = fs::absolute(path, ec);
        if (ec) {
            abs = path;
        }

        if (abs == currentPath_) {
            return;
        }

        if (addHistory) {
            history_.push_back(currentPath_);
        }

        currentPath_ = abs;
        search_.clear();
        refresh();
    }

    void goBack()
    {
        if (history_.empty()) {
            return;
        }

        const fs::path target = history_.back();
        history_.pop_back();

        navigateTo(target, false);
    }

    void goUp()
    {
        const fs::path parent = currentPath_.parent_path();

        if (parent.empty() || parent == currentPath_) {
            return;
        }

        navigateTo(parent);
    }

    void goHome()
    {
        navigateTo(homeDir());
    }

    void toggleHidden()
    {
        showHidden_ = !showHidden_;
        refresh();
    }

    void setSort(SortMode mode)
    {
        sort_ = mode;
        applyFilter();
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

    void appendSearch(char c)
    {
        if (!searchFocused_) {
            return;
        }

        if (c == 8) {
            if (!search_.empty()) {
                search_.pop_back();
                applyFilter();
            }
            return;
        }

        if (c == 27) {
            search_.clear();
            setSearchFocused(false);
            applyFilter();
            return;
        }

        if (c >= 32 && c < 127) {
            if (search_.size() < 200) {
                search_.push_back(c);
                applyFilter();
            }
        }
    }

    void clearSearch()
    {
        search_.clear();
        applyFilter();
    }

    // --------------------------------------------------------
    // Selection / actions
    // --------------------------------------------------------

    void pointerMove(float x, float y)
    {
        pointerX_ = x;
        pointerY_ = y;

        hovered_ = hitEntry(x, y);

        for (int i = 0; i < 5; ++i) {
            toolbar_[i].setHovered(hitToolbar(i, x, y));
        }
    }

    void pointerDown(float x, float y)
    {
        pointerMove(x, y);

        if (hitSearch(x, y)) {
            setSearchFocused(true);
            return;
        }

        for (int i = 0; i < 5; ++i) {
            if (hitToolbar(i, x, y)) {
                runToolbar(i);
                return;
            }
        }

        for (size_t i = 0; i < sidebar_.size(); ++i) {
            if (hitSidebar(static_cast<int>(i), x, y)) {
                navigateTo(sidebar_[i].path);
                return;
            }
        }

        const int idx = hitEntry(x, y);

        if (idx < 0) {
            selected_ = -1;
            return;
        }

        const bool dbl =
            selected_ == idx &&
            lastClickAge_ <= 0.36f &&
            std::hypot(
                x - lastClickX_,
                y - lastClickY_) < 12.0f;

        selected_ = idx;
        selectionAnim_.start(0.0f, 1.0f);

        lastClickX_ = x;
        lastClickY_ = y;
        lastClickAge_ = 0.0f;

        if (dbl) {
            activateSelected();
        }
    }

    void activateSelected()
    {
        if (selected_ < 0 ||
            selected_ >= static_cast<int>(filtered_.size())) {
            return;
        }

        const Entry& e = filtered_[selected_];

        if (e.directory) {
            navigateTo(e.path);
        } else {
            spawnOpen(e.path.string());
        }
    }

    void openContainingFolder()
    {
        if (selected_ < 0 ||
            selected_ >= static_cast<int>(filtered_.size())) {
            return;
        }

        const fs::path parent = filtered_[selected_].path.parent_path();
        if (!parent.empty()) {
            navigateTo(parent);
        }
    }

    void createFolder()
    {
        fs::path target = currentPath_ / "New Folder";
        std::error_code ec;

        if (fs::exists(target, ec)) {
            for (int i = 2; i < 10000; ++i) {
                target = currentPath_ / ("New Folder " + std::to_string(i));
                if (!fs::exists(target, ec)) {
                    break;
                }
            }
        }

        if (fs::create_directory(target, ec)) {
            toast("Folder created");
        } else {
            toast("Could not create folder");
        }

        refresh();
    }

    void deleteSelected()
    {
        if (selected_ < 0 ||
            selected_ >= static_cast<int>(filtered_.size())) {
            return;
        }

        const fs::path target = filtered_[selected_].path;
        std::error_code ec;

        const bool ok = fs::remove_all(target, ec) > 0 && !ec;

        toast(ok ? "Item deleted" : "Delete failed");
        refresh();
    }

    void revealTerminal()
    {
        spawnTerminal(currentPath_.string());
    }

    // --------------------------------------------------------
    // Hit testing
    // --------------------------------------------------------

    bool hitSearch(float x, float y) const
    {
        const float sx = LIST_X + 300.0f;
        const float sy = 7.0f;
        const float sw = WINDOW_WIDTH - sx - 18.0f;

        return x >= sx && x <= sx + sw &&
               y >= sy && y <= sy + 40.0f;
    }

    bool hitToolbar(int index, float x, float y) const
    {
        const float startX = LIST_X + 18.0f;
        const float top = TITLEBAR_H + 7.0f;
        const float size = 38.0f;
        const float gap = 7.0f;

        const float bx = startX + index * (size + gap);

        return x >= bx && x <= bx + size &&
               y >= top && y <= top + size;
    }

    bool hitSidebar(int index, float x, float y) const
    {
        if (index < 0 || index >= static_cast<int>(sidebar_.size())) {
            return false;
        }

        const float top = TITLEBAR_H + 12.0f;
        const float rowH = 42.0f;
        const float yy = top + index * rowH;

        return x >= 10.0f &&
               x <= SIDEBAR_W - 10.0f &&
               y >= yy &&
               y <= yy + rowH - 4.0f;
    }

    int hitEntry(float x, float y) const
    {
        if (x < LIST_X ||
            x > LIST_X + LIST_W ||
            y < HEADER_Y + 48.0f) {
            return -1;
        }

        const int row = static_cast<int>(
            (y - HEADER_Y - 48.0f) / ROW_H);

        if (row < 0 ||
            row >= static_cast<int>(filtered_.size())) {
            return -1;
        }

        return row;
    }

    // --------------------------------------------------------
    // State
    // --------------------------------------------------------

    void applyFilter()
    {
        filtered_.clear();

        const std::string q = lowerCopy(search_);

        for (const Entry& e : entries_) {
            if (!q.empty() &&
                lowerCopy(e.name).find(q) == std::string::npos) {
                continue;
            }

            filtered_.push_back(e);
        }

        std::sort(
            filtered_.begin(),
            filtered_.end(),
            [this](const Entry& a, const Entry& b) {

                if (a.directory != b.directory) {
                    return a.directory > b.directory;
                }

                if (sort_ == SortMode::Modified) {
                    return a.modified > b.modified;
                }

                if (sort_ == SortMode::Size) {
                    return a.size < b.size;
                }

                return lowerCopy(a.name) < lowerCopy(b.name);
            });
    }

    void runToolbar(int i)
    {
        switch (i) {
            case 0: goBack(); break;
            case 1: goUp(); break;
            case 2: refresh(); break;
            case 3: createFolder(); break;
            case 4: toggleHidden(); break;
        }
    }

    void toast(const std::string& message)
    {
        toast_ = message;
        toastAnim_.start(0.0f, 1.0f);
    }

public:
    float startupOpacity() const { return startupOpacity_.value(); }
    float startupSlide() const { return startupSlide_.value(); }
    float searchFocus() const { return searchFocus_.value(); }
    float selectionPulse() const { return selectionAnim_.value(); }
    float toastProgress() const { return toastAnim_.value(); }

    float pointerX() const { return pointerX_; }
    float pointerY() const { return pointerY_; }

    const fs::path& currentPath() const { return currentPath_; }
    const std::string& searchText() const { return search_; }

    const std::vector<Entry>& entries() const {
        return filtered_;
    }

    int selectedIndex() const { return selected_; }
    int hoveredIndex() const { return hovered_; }

    bool showHidden() const { return showHidden_; }
    const std::string& toastText() const { return toast_; }

    const UIButton& toolbarButton(int i) const {
        return toolbar_.at(static_cast<size_t>(i));
    }

    const std::vector<SidebarItem>& sidebarItems() const {
        static std::vector<SidebarItem> out = {
            { SidebarId::Home, "Home", fs::path(homeDir()) }
        };
        return out;
    }

private:
    fs::path currentPath_;
    std::vector<fs::path> history_;

    std::vector<Entry> entries_;
    std::vector<Entry> filtered_;

    std::vector<UIButton> toolbar_;
    std::vector<SidebarItem> sidebar_;

    int selected_ = -1;
    int hovered_ = -1;

    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;

    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;
    float lastClickAge_ = 99.0f;

    std::string search_;
    std::string toast_;

    bool showHidden_ = false;
    bool searchFocused_ = false;

    SortMode sort_ = SortMode::Name;

    Animation startupOpacity_{0.30f};
    Animation startupSlide_{0.26f};
    Animation searchFocus_{0.16f};
    Animation selectionAnim_{0.22f};
    Animation toastAnim_{0.35f};

    float elapsed_ = 0.0f;
};

// ------------------------------------------------------------
// Renderer
// ------------------------------------------------------------

class FileManagerRenderer {
public:
    bool init()
    {
        const SkImageInfo info = SkImageInfo::Make(
            WINDOW_WIDTH,
            WINDOW_HEIGHT,
            kRGBA_8888_SkColorType,
            kPremul_SkAlphaType);

        surface_ = SkSurfaces::Raster(info);
        return surface_ != nullptr;
    }

    void render(const FileManagerUI& ui)
    {
        if (!surface_) {
            return;
        }

        SkCanvas* c = surface_->getCanvas();
        c->clear(AgroColor::surface(255));

        drawChrome(c, ui);
        drawSidebar(c, ui);
        drawList(c, ui);
        drawToast(c, ui);
    }

    const uint32_t* pixels(int* stride) const
    {
        if (!surface_) {
            return nullptr;
        }

        SkPixmap pm;
        if (!surface_->peekPixels(&pm)) {
            return nullptr;
        }

        if (stride) {
            *stride = static_cast<int>(pm.rowBytes() / 4);
        }

        return static_cast<const uint32_t*>(pm.addr());
    }

private:
    static SkColor text() { return AgroColor::ink(235); }
    static SkColor secondary() { return AgroColor::ink(150); }
    static SkColor subtle() { return AgroColor::ink(88); }

    void drawChrome(SkCanvas* c, const FileManagerUI& ui)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(AgroColor::white(100));

        c->drawRect(
            SkRect::MakeXYWH(0, 0, WINDOW_WIDTH, TITLEBAR_H),
            p);

        p.setColor(AgroColor::ink(18));
        c->drawRect(
            SkRect::MakeXYWH(
                SIDEBAR_W,
                0,
                1.0f,
                WINDOW_HEIGHT),
            p);

        SkFont titleFont;
        titleFont.setSize(16.0f);

        SkPaint titlePaint;
        titlePaint.setAntiAlias(true);
        titlePaint.setColor(text());

        c->drawString(
            "Files",
            22.0f,
            33.0f,
            titleFont,
            titlePaint);

        // Address / breadcrumb
        const std::string path = ui.currentPath().string();

        SkFont pathFont;
        pathFont.setSize(12.5f);

        SkPaint pathPaint;
        pathPaint.setAntiAlias(true);
        pathPaint.setColor(secondary());

        c->drawString(
            path.c_str(),
            LIST_X + 18.0f,
            31.0f,
            pathFont,
            pathPaint);

        // Search
        const float sx = LIST_X + 300.0f;
        const float sy = 7.0f;
        const float sw = WINDOW_WIDTH - sx - 18.0f;

        SkPaint searchBg;
        searchBg.setAntiAlias(true);
        searchBg.setColor(
            AgroColor::white(
                static_cast<int>(
                    75.0f + ui.searchFocus() * 25.0f)));
        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    sx, sy, sw, 40.0f),
                12.0f, 12.0f),
            searchBg);

        SkPaint border;
        border.setAntiAlias(true);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(1.0f);
        border.setColor(
            AgroColor::ink(
                ui.searchFocus() > 0.5f ? 82 : 34));
        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    sx, sy, sw, 40.0f),
                12.0f, 12.0f),
            border);

        drawSearch(c, sx + 17.0f, sy + 20.0f);

        SkFont sf;
        sf.setSize(12.5f);

        SkPaint st;
        st.setAntiAlias(true);
        st.setColor(
            ui.searchText().empty() ?
                secondary() :
                text());

        const char* s =
            ui.searchText().empty() ?
                "Search in current folder" :
                ui.searchText().c_str();

        c->drawString(
            s,
            sx + 36.0f,
            sy + 25.0f,
            sf,
            st);

        // Toolbar
        const float startX = LIST_X + 18.0f;
        const float top = TITLEBAR_H + 7.0f;

        for (int i = 0; i < 5; ++i) {
            const float x = startX + i * 45.0f;
            const UIButton& b = ui.toolbarButton(i);

            SkPaint bg;
            bg.setAntiAlias(true);
            bg.setColor(
                AgroColor::white(
                    static_cast<int>(
                        34.0f + 40.0f * b.hover.value())));

            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        x, top, 38.0f, 38.0f),
                    10.0f, 10.0f),
                bg);

            drawToolbarIcon(
                c,
                x + 19.0f,
                top + 19.0f,
                i);
        }

        // Current folder status
        SkFont countFont;
        countFont.setSize(11.5f);

        SkPaint countPaint;
        countPaint.setAntiAlias(true);
        countPaint.setColor(secondary());

        const std::string count =
            std::to_string(ui.entries().size()) + " items";

        c->drawString(
            count.c_str(),
            LIST_X + 18.0f,
            HEADER_Y + 30.0f,
            countFont,
            countPaint);
    }

    void drawSearch(SkCanvas* c, float cx, float cy)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(1.8f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor(AgroColor::ink(170));

        c->drawCircle(cx, cy, 5.2f, p);
        c->drawLine(cx + 4.0f, cy + 4.0f,
                    cx + 8.0f, cy + 8.0f, p);
    }

    void drawToolbarIcon(
        SkCanvas* c,
        float cx,
        float cy,
        int i)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(AgroColor::ink(205));

        if (i == 0) {
            c->drawLine(cx + 7, cy, cx - 7, cy, p);
            c->drawLine(cx - 7, cy, cx, cy - 6, p);
            c->drawLine(cx - 7, cy, cx, cy + 6, p);
        } else if (i == 1) {
            c->drawLine(cx, cy + 7, cx, cy - 6, p);
            c->drawLine(cx, cy - 6, cx - 6, cy, p);
            c->drawLine(cx, cy - 6, cx + 6, cy, p);
        } else if (i == 2) {
            c->drawArc(
                SkRect::MakeXYWH(cx - 8, cy - 8, 16, 16),
                40, 290, false, p);
            c->drawLine(cx + 7, cy - 1, cx + 7, cy - 6, p);
            c->drawLine(cx + 7, cy - 6, cx + 2, cy - 6, p);
        } else if (i == 3) {
            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        cx - 9, cy - 5, 18, 12),
                    2.5f, 2.5f),
                p);
            c->drawLine(cx - 5, cy - 8, cx + 1, cy - 8, p);
            c->drawLine(cx, cy + 2, cx + 8, cy + 2, p);
            c->drawLine(cx + 4, cy - 2, cx + 4, cy + 6, p);
        } else {
            c->drawOval(
                SkRect::MakeXYWH(
                    cx - 9, cy - 6, 18, 12),
                p);
            c->drawCircle(cx, cy, 3.0f, p);
        }
    }

    void drawSidebar(
        SkCanvas* c,
        const FileManagerUI& ui)
    {
        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(AgroColor::white(75));

        c->drawRect(
            SkRect::MakeXYWH(
                0, TITLEBAR_H, SIDEBAR_W,
                WINDOW_HEIGHT - TITLEBAR_H),
            bg);

        const auto items = std::vector<SidebarItem>{
            { SidebarId::Home, "Home", fs::path(homeDir()) },
            { SidebarId::Desktop, "Desktop", fs::path(homeDir()) / "Desktop" },
            { SidebarId::Documents, "Documents", fs::path(homeDir()) / "Documents" },
            { SidebarId::Downloads, "Downloads", fs::path(homeDir()) / "Downloads" },
            { SidebarId::Pictures, "Pictures", fs::path(homeDir()) / "Pictures" },
            { SidebarId::Music, "Music", fs::path(homeDir()) / "Music" },
            { SidebarId::Videos, "Videos", fs::path(homeDir()) / "Videos" },
            { SidebarId::Root, "File System", fs::path("/") }
        };

        float y = TITLEBAR_H + 14.0f;

        for (const auto& item : items) {
            std::error_code ec;
            if (!fs::exists(item.path, ec)) {
                continue;
            }

            const bool active =
                fs::equivalent(
                    item.path,
                    ui.currentPath(),
                    ec);

            if (active) {
                SkPaint selected;
                selected.setAntiAlias(true);
                selected.setColor(AgroColor::accent(32));

                c->drawRRect(
                    SkRRect::MakeRectXY(
                        SkRect::MakeXYWH(
                            12, y, SIDEBAR_W - 24, 38),
                        11.0f, 11.0f),
                    selected);
            }

            drawSidebarGlyph(
                c,
                31.0f,
                y + 19.0f,
                item.id,
                active);

            SkFont f;
            f.setSize(13.5f);

            SkPaint tp;
            tp.setAntiAlias(true);
            tp.setColor(
                active ?
                    AgroColor::accent(230) :
                    text());

            c->drawString(
                item.label,
                53.0f,
                y + 24.0f,
                f,
                tp);

            y += 43.0f;
        }
    }

    void drawSidebarGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        SidebarId id,
        bool active)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(1.8f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(
            active ?
                AgroColor::accent(220) :
                AgroColor::ink(175));

        if (id == SidebarId::Home) {
            SkPathBuilder path;
            path.moveTo(cx - 9, cy);
            path.lineTo(cx, cy - 8);
            path.lineTo(cx + 9, cy);
            path.lineTo(cx + 7, cy);
            path.lineTo(cx + 7, cy + 8);
            path.lineTo(cx - 7, cy + 8);
            path.close();
            c->drawPath(path.detach(), p);
        } else if (id == SidebarId::Root) {
            c->drawCircle(cx, cy, 8, p);
            c->drawLine(cx - 6, cy, cx + 6, cy, p);
            c->drawLine(cx, cy - 6, cx, cy + 6, p);
        } else {
            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        cx - 9, cy - 6, 18, 13),
                    2.5f, 2.5f),
                p);
            c->drawLine(
                cx - 6, cy - 7,
                cx - 1, cy - 10,
                p);
            c->drawLine(
                cx - 1, cy - 10,
                cx + 4, cy - 10,
                p);
        }
    }

    void drawList(
        SkCanvas* c,
        const FileManagerUI& ui)
    {
        const auto& items = ui.entries();

        const float top = HEADER_Y + 48.0f;

        SkFont colFont;
        colFont.setSize(11.0f);

        SkPaint colPaint;
        colPaint.setAntiAlias(true);
        colPaint.setColor(subtle());

        c->drawString(
            "Name", LIST_X + 72, top - 14, colFont, colPaint);
        c->drawString(
            "Size", LIST_X + LIST_W - 220, top - 14, colFont, colPaint);
        c->drawString(
            "Modified", LIST_X + LIST_W - 112, top - 14, colFont, colPaint);

        if (items.empty()) {
            SkFont f;
            f.setSize(17.0f);

            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(text());

            c->drawString(
                ui.searchText().empty() ?
                    "This folder is empty" :
                    "No matching items",
                LIST_X + 24,
                top + 72,
                f,
                p);

            SkFont sf;
            sf.setSize(12.0f);

            SkPaint sp;
            sp.setAntiAlias(true);
            sp.setColor(secondary());

            c->drawString(
                ui.searchText().empty() ?
                    "There is nothing here to show." :
                    "Try another search term.",
                LIST_X + 24,
                top + 98,
                sf,
                sp);

            return;
        }

        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const Entry& e = items[i];

            const float y = top + i * ROW_H;
            const bool selected = ui.selectedIndex() == i;
            const bool hovered = ui.hoveredIndex() == i;

            if (selected || hovered) {
                SkPaint bg;
                bg.setAntiAlias(true);
                bg.setColor(
                    selected ?
                        AgroColor::accent(
                            static_cast<int>(
                                24 + 9 * ui.selectionPulse())) :
                        AgroColor::white(42));

                c->drawRRect(
                    SkRRect::MakeRectXY(
                        SkRect::MakeXYWH(
                            LIST_X,
                            y,
                            LIST_W,
                            ROW_H - 3),
                        ROW_RADIUS,
                        ROW_RADIUS),
                    bg);
            }

            drawEntryGlyph(
                c,
                LIST_X + 30,
                y + ROW_H * 0.5f,
                e);

            SkFont nf;
            nf.setSize(13.5f);

            SkPaint np;
            np.setAntiAlias(true);
            np.setColor(text());

            c->drawString(
                e.name.c_str(),
                LIST_X + 62,
                y + 26,
                nf,
                np);

            SkFont sf;
            sf.setSize(11.0f);

            SkPaint sp;
            sp.setAntiAlias(true);
            sp.setColor(secondary());

            const char* kind = e.directory ? "Folder" : "File";

            c->drawString(
                kind,
                LIST_X + 62,
                y + 43,
                sf,
                sp);

            c->drawString(
                e.size.c_str(),
                LIST_X + LIST_W - 220,
                y + 34,
                sf,
                sp);

            c->drawString(
                e.modified.c_str(),
                LIST_X + LIST_W - 112,
                y + 34,
                sf,
                sp);

            if (i + 1 < static_cast<int>(items.size())) {
                SkPaint divider;
                divider.setAntiAlias(true);
                divider.setColor(AgroColor::ink(18));

                c->drawRect(
                    SkRect::MakeXYWH(
                        LIST_X + 18,
                        y + ROW_H - 2,
                        LIST_W - 36,
                        1),
                    divider);
            }
        }
    }

    void drawEntryGlyph(
        SkCanvas* c,
        float cx,
        float cy,
        const Entry& e)
    {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(1.8f);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(
            e.directory ?
                AgroColor::accent(205) :
                AgroColor::ink(180));

        if (e.directory) {
            c->drawRRect(
                SkRRect::MakeRectXY(
                    SkRect::MakeXYWH(
                        cx - 11, cy - 7, 22, 15),
                    3, 3),
                p);
            c->drawLine(
                cx - 8, cy - 7,
                cx - 2, cy - 11,
                p);
            return;
        }

        SkPathBuilder doc;
        doc.moveTo(cx - 8, cy - 11);
        doc.lineTo(cx + 3, cy - 11);
        doc.lineTo(cx + 8, cy - 6);
        doc.lineTo(cx + 8, cy + 11);
        doc.lineTo(cx - 8, cy + 11);
        doc.close();

        c->drawPath(doc.detach(), p);

        c->drawLine(
            cx + 3, cy - 11,
            cx + 3, cy - 6,
            p);
        c->drawLine(
            cx + 3, cy - 6,
            cx + 8, cy - 6,
            p);
    }

    void drawToast(
        SkCanvas* c,
        const FileManagerUI& ui)
    {
        const float a =
            std::clamp(
                ui.toastProgress() < 0.5f ?
                    ui.toastProgress() * 2.0f :
                    2.0f - ui.toastProgress() * 2.0f,
                0.0f,
                1.0f);

        if (a <= 0.01f || ui.toastText().empty()) {
            return;
        }

        SkFont f;
        f.setSize(12.0f);

        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(
            AgroColor::ink(
                static_cast<int>(230 * a)));

        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(
            AgroColor::white(
                static_cast<int>(235 * a)));

        c->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(
                    WINDOW_WIDTH - 260,
                    WINDOW_HEIGHT - 62,
                    220,
                    38),
                12,
                12),
            bg);

        c->drawString(
            ui.toastText().c_str(),
            WINDOW_WIDTH - 240,
            WINDOW_HEIGHT - 38,
            f,
            p);
    }

    sk_sp<SkSurface> surface_;
};

} // namespace agro

// ------------------------------------------------------------
// main
// ------------------------------------------------------------

int main()
{
    std::printf(
        "============================================================\n"
        " AGROOS FILE MANAGER\n"
        " Native C++17 + Skia + Wayland\n"
        " REAL FILESYSTEM / NO DEMO DATA\n"
        "============================================================\n");

    agro::FileManagerUI ui;
    agro::FileManagerRenderer renderer;

    if (!renderer.init()) {
        std::fprintf(
            stderr,
            "AgroOS File Manager: Skia init failed.\n");
        return 1;
    }

    AgroWaylandWindow* win = agro_wl_create(
        "AgroOS Files",
        agro::WINDOW_WIDTH,
        agro::WINDOW_HEIGHT);

    if (!win) {
        std::fprintf(
            stderr,
            "AgroOS File Manager: Wayland window could not be created.\n");
        return 1;
    }

    struct InputContext {
        agro::FileManagerUI* ui;
    } input{ &ui };

    agro_wl_set_input_callback(
        win,
        [](const AgroInputEvent* ev, void* user) {
            auto* ctx =
                static_cast<InputContext*>(user);

            switch (ev->type) {
                case AGRO_INPUT_POINTER_MOTION:
                    ctx->ui->pointerMove(ev->x, ev->y);
                    break;

                case AGRO_INPUT_POINTER_BUTTON_DOWN:
                    if (ev->button == 1) {
                        ctx->ui->pointerDown(ev->x, ev->y);
                    }
                    break;

                case AGRO_INPUT_KEY_DOWN:
                    break;

                case AGRO_INPUT_KEY_UP:
                    break;

                case AGRO_INPUT_POINTER_BUTTON_UP:
                    break;

                case AGRO_INPUT_CLOSE_REQUEST:
                    break;
            }
        },
        &input);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    while (agro_wl_dispatch(win)) {
        const auto now = Clock::now();

        float dt =
            std::chrono::duration<float>(now - last).count();

        last = now;

        dt = std::clamp(
            dt,
            1.0f / 240.0f,
            1.0f / 15.0f);

        ui.update(dt);
        renderer.render(ui);

        int stride = 0;
        const uint32_t* src = renderer.pixels(&stride);
        uint32_t* dst = agro_wl_begin_frame(win);

        if (src && dst) {
            const int w = agro_wl_width(win);
            const int h = agro_wl_height(win);

            const int copyW =
                std::min(w, agro::WINDOW_WIDTH);

            const int copyH =
                std::min(h, agro::WINDOW_HEIGHT);

            for (int y = 0; y < copyH; ++y) {
                const uint32_t* s =
                    src + static_cast<size_t>(y) * stride;

                uint32_t* d =
                    dst + static_cast<size_t>(y) * w;

                for (int x = 0; x < copyW; ++x) {
                    const uint32_t p = s[x];

                    const uint32_t r = (p >> 0) & 0xff;
                    const uint32_t g = (p >> 8) & 0xff;
                    const uint32_t b = (p >> 16) & 0xff;
                    const uint32_t a = (p >> 24) & 0xff;

                    d[x] =
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
