#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime
import shutil
import textwrap

TARGET = Path('/content/drive/MyDrive/AgroOS/ui/agroshell/agroshell.cpp')

CPP = r'''
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

#include <wayland-client.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"

#include "../agroui/agroui.h"
#include "../common/agro_wayland_client.h"

namespace agro {

// ============================================================
// MODERN TASKBAR MODEL
// ============================================================

class TaskbarUI {
public:
    static constexpr int Width = 1280;
    static constexpr int Height = 86;

    static constexpr float PanelMargin = 14.0f;
    static constexpr float PanelHeight = 64.0f;
    static constexpr float PanelRadius = 22.0f;

    static constexpr float LauncherSize = 46.0f;
    static constexpr float SearchWidth = 228.0f;
    static constexpr float SearchHeight = 44.0f;

    static constexpr float AppSize = 44.0f;
    static constexpr float AppGap = 8.0f;
    static constexpr int AppCount = 5;

    enum ButtonId : int {
        Launcher = 0,
        Files = 1,
        Browser = 2,
        Terminal = 3,
        Home = 4,
        Settings = 5
    };

    TaskbarUI() {
        buttons_.resize(AppCount + 1);
        layout();
        startupOpacity_.start(0.0f, 1.0f);
        startupSlide_.start(10.0f, 0.0f);
    }

    void layout() {
        auto& launcher = buttons_[Launcher];
        launcher.x = PanelMargin + 8.0f;
        launcher.y = 18.0f;
        launcher.width = LauncherSize;
        launcher.height = LauncherSize;

        const float searchX = launcher.x + launcher.width + 14.0f;
        const float appsX = searchX + SearchWidth + 16.0f;

        for (int i = 0; i < AppCount; ++i) {
            auto& b = buttons_[i + 1];
            b.x = appsX + static_cast<float>(i) * (AppSize + AppGap);
            b.y = 19.0f;
            b.width = AppSize;
            b.height = AppSize;
        }
    }

    void update(float dt) {
        startupOpacity_.update(dt);
        startupSlide_.update(dt);
        searchFocus_.update(dt);
        notificationPulse_.update(dt);
        for (auto& button : buttons_) button.update(dt);
        elapsed_ += dt;
    }

    void updatePointer(float x, float y) {
        const float localY = std::max(0.0f, y - startupSlide_.value());
        for (size_t i = 0; i < buttons_.size(); ++i) {
            setButtonHovered(i, inside(buttons_[i], x, localY));
        }
        setSearchFocused(hitSearch(x, localY));
    }

    bool hitTest(float x, float y, ButtonId id) const {
        const float localY = std::max(0.0f, y - startupSlide_.value());
        return inside(buttons_.at(static_cast<size_t>(id)), x, localY);
    }

    bool hitSearch(float x, float y) const {
        const float sx = buttons_[Launcher].x + LauncherSize + 14.0f;
        const float sy = 18.0f;
        return x >= sx && x <= sx + SearchWidth &&
               y >= sy && y <= sy + SearchHeight;
    }

    void setButtonHovered(size_t index, bool value) {
        if (index < buttons_.size()) buttons_[index].setHovered(value);
    }

    void setButtonPressed(size_t index, bool value) {
        if (index < buttons_.size()) buttons_[index].setPressed(value);
    }

    void setActive(ButtonId id) {
        for (size_t i = 1; i < buttons_.size(); ++i) {
            buttons_[i].active = static_cast<ButtonId>(i) == id;
        }
    }

    void setSearchFocused(bool focused) {
        searchFocused_ = focused;
        searchFocus_.start(searchFocus_.value(), focused ? 1.0f : 0.0f);
    }

    void pulseNotification() { notificationPulse_.start(0.0f, 1.0f); }

    float startupOpacity() const { return startupOpacity_.value(); }
    float startupSlide() const { return startupSlide_.value(); }
    float searchFocus() const { return searchFocus_.value(); }
    float notificationPulse() const { return notificationPulse_.value(); }
    float elapsed() const { return elapsed_; }

    const UIButton& button(size_t index) const { return buttons_.at(index); }

private:
    static bool inside(const UIButton& b, float x, float y) {
        return x >= b.x && x <= b.x + b.width &&
               y >= b.y && y <= b.y + b.height;
    }

    std::vector<UIButton> buttons_;
    Animation startupOpacity_{0.32f};
    Animation startupSlide_{0.28f};
    Animation searchFocus_{0.16f};
    Animation notificationPulse_{0.40f};
    bool searchFocused_ = false;
    float elapsed_ = 0.0f;
};

// ============================================================
// REAL ACTIONS - NO DEMO BUTTONS
// ============================================================

static void spawn(const char* program, const char* arg = nullptr) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (arg) execlp(program, program, arg, static_cast<char*>(nullptr));
        else execlp(program, program, static_cast<char*>(nullptr));
        _exit(127);
    }
}

static void openPath(const char* path) {
    if (access("/usr/bin/xdg-open", X_OK) == 0) {
        spawn("xdg-open", path);
    }
}

static void launchTerminal() {
    if (access("/usr/bin/foot", X_OK) == 0) spawn("foot");
    else if (access("/usr/bin/kitty", X_OK) == 0) spawn("kitty");
    else if (access("/usr/bin/alacritty", X_OK) == 0) spawn("alacritty");
    else if (access("/usr/bin/gnome-terminal", X_OK) == 0) spawn("gnome-terminal");
}

static void launchSettings() {
    if (access("/usr/bin/gnome-control-center", X_OK) == 0) spawn("gnome-control-center");
    else if (access("/usr/bin/xfce4-settings-manager", X_OK) == 0) spawn("xfce4-settings-manager");
    else if (access("/usr/bin/systemsettings", X_OK) == 0) spawn("systemsettings");
    else openPath("/usr/share/applications");
}

static void performAction(TaskbarUI::ButtonId id) {
    switch (id) {
        case TaskbarUI::Launcher:
            openPath("/usr/share/applications");
            break;
        case TaskbarUI::Files:
            openPath("/");
            break;
        case TaskbarUI::Browser:
            openPath("https://github.com/salih-ozt/Agro-OS");
            break;
        case TaskbarUI::Terminal:
            launchTerminal();
            break;
        case TaskbarUI::Home: {
            const char* home = std::getenv("HOME");
            openPath(home && *home ? home : "/home");
            break;
        }
        case TaskbarUI::Settings:
            launchSettings();
            break;
    }
}

// ============================================================
// SKIA RENDERER
// ============================================================

class AgroRenderer {
public:
    bool init() {
        const SkImageInfo info = SkImageInfo::Make(
            TaskbarUI::Width,
            TaskbarUI::Height,
            kRGBA_8888_SkColorType,
            kPremul_SkAlphaType
        );
        surface_ = SkSurfaces::Raster(info);
        return surface_ != nullptr;
    }

    void render(const TaskbarUI& ui) {
        if (!surface_) return;
        SkCanvas* canvas = surface_->getCanvas();
        canvas->clear(SkColorSetARGB(0, 0, 0, 0));
        drawTaskbar(canvas, ui);
    }

    const uint32_t* peekPixels(int* outStride) const {
        if (!surface_) return nullptr;
        SkPixmap pm;
        if (!surface_->peekPixels(&pm)) return nullptr;
        if (outStride) *outStride = static_cast<int>(pm.rowBytes() / 4);
        return static_cast<const uint32_t*>(pm.addr());
    }

private:
    void drawTaskbar(SkCanvas* canvas, const TaskbarUI& ui) {
        const float opacity = ui.startupOpacity();
        const float panelY = ui.startupSlide() + 7.0f;

        GlassPanelStyle style;
        style.cornerRadius = TaskbarUI::PanelRadius;

        drawGlassPanel(
            canvas,
            TaskbarUI::PanelMargin,
            panelY,
            TaskbarUI::Width - TaskbarUI::PanelMargin * 2.0f,
            TaskbarUI::PanelHeight,
            opacity,
            style
        );

        drawLauncher(canvas, ui, panelY, opacity);
        drawSearch(canvas, ui, panelY, opacity);

        for (int i = 0; i < TaskbarUI::AppCount; ++i) {
            drawAppButton(canvas, ui.button(i + 1), panelY, opacity, i);
        }

        drawSystemArea(canvas, ui, panelY, opacity);
    }

    void drawLauncher(SkCanvas* canvas, const TaskbarUI& ui, float y, float opacity) {
        const UIButton& b = ui.button(TaskbarUI::Launcher);
        const float s = b.scale();
        const float cx = b.x + b.width * 0.5f;
        const float cy = y + b.y + b.height * 0.5f;
        const float size = 44.0f * s;

        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(AgroColor::white(static_cast<int>((24.0f + b.hover.value() * 46.0f) * opacity)));
        canvas->drawRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeXYWH(cx - size * 0.5f, cy - size * 0.5f, size, size),
                13.0f, 13.0f
            ),
            bg
        );

        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setColor(AgroColor::accent(static_cast<int>(220.0f * opacity)));
        canvas->drawCircle(cx, cy, 7.5f, p);

        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setColor(AgroColor::white(static_cast<int>(180.0f * opacity)));
        canvas->drawCircle(cx, cy, 11.0f, p);
    }

    void drawSearch(SkCanvas* canvas, const TaskbarUI& ui, float y, float opacity) {
        const UIButton& launcher = ui.button(TaskbarUI::Launcher);
        const float x = launcher.x + launcher.width + 14.0f;
        const float yy = y + 18.0f;
        const float focus = ui.searchFocus();
        const float width = TaskbarUI::SearchWidth + 24.0f * focus;

        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(AgroColor::white(static_cast<int>((28.0f + focus * 22.0f) * opacity)));
        canvas->drawRRect(
            SkRRect::MakeRectXY(SkRect::MakeXYWH(x, yy, width, TaskbarUI::SearchHeight), 15.0f, 15.0f),
            bg
        );

        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor(AgroColor::ink(static_cast<int>(175.0f * opacity)));

        const float cx = x + 18.0f;
        const float cy = yy + TaskbarUI::SearchHeight * 0.5f;
        canvas->drawCircle(cx, cy, 5.5f, p);
        canvas->drawLine(cx + 4.0f, cy + 4.0f, cx + 9.0f, cy + 9.0f, p);

        SkFont font;
        font.setSize(13.5f);
        SkPaint text;
        text.setAntiAlias(true);
        text.setColor(AgroColor::ink(static_cast<int>((135.0f + focus * 55.0f) * opacity)));
        canvas->drawString("Search", x + 38.0f, yy + 27.0f, font, text);
    }

    void drawAppButton(SkCanvas* canvas, const UIButton& b, float y, float opacity, int index) {
        const float lift = -2.0f * b.hover.value();
        const float s = b.scale();
        const float cx = b.x + b.width * 0.5f;
        const float cy = y + b.y + b.height * 0.5f + lift;
        const float size = 42.0f * s;

        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(AgroColor::white(static_cast<int>((16.0f + b.hover.value() * 48.0f) * opacity)));
        canvas->drawRRect(
            SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - size * 0.5f, cy - size * 0.5f, size, size), 12.0f, 12.0f),
            bg
        );

        SkPaint icon;
        icon.setAntiAlias(true);
        switch (index) {
            case 0: drawFilesIcon(canvas, cx, cy, icon, opacity); break;
            case 1: drawBrowserIcon(canvas, cx, cy, icon, opacity); break;
            case 2: drawTerminalIcon(canvas, cx, cy, icon, opacity); break;
            case 3: drawHomeIcon(canvas, cx, cy, icon, opacity); break;
            case 4: drawSettingsIcon(canvas, cx, cy, icon, opacity); break;
        }

        if (b.active) {
            SkPaint indicator;
            indicator.setAntiAlias(true);
            indicator.setColor(AgroColor::accent(static_cast<int>(220.0f * opacity)));
            canvas->drawRoundRect(SkRect::MakeXYWH(cx - 6.0f, y + 65.0f, 12.0f, 2.5f), 1.25f, 1.25f, indicator);
        }
    }

    void drawFilesIcon(SkCanvas* canvas, float cx, float cy, SkPaint p, float opacity) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setColor(AgroColor::ink(static_cast<int>(210.0f * opacity)));
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - 10.0f, cy - 8.0f, 20.0f, 16.0f), 3.5f, 3.5f), p);
        canvas->drawLine(cx - 8.0f, cy - 4.0f, cx - 1.0f, cy - 4.0f, p);
    }

    void drawBrowserIcon(SkCanvas* canvas, float cx, float cy, SkPaint p, float opacity) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setColor(AgroColor::ink(static_cast<int>(210.0f * opacity)));
        canvas->drawCircle(cx, cy, 10.0f, p);
        canvas->drawLine(cx - 10.0f, cy, cx + 10.0f, cy, p);
        canvas->drawOval(SkRect::MakeXYWH(cx - 4.5f, cy - 10.0f, 9.0f, 20.0f), p);
    }

    void drawTerminalIcon(SkCanvas* canvas, float cx, float cy, SkPaint p, float opacity) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor(AgroColor::ink(static_cast<int>(210.0f * opacity)));
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - 11.0f, cy - 8.0f, 22.0f, 16.0f), 3.5f, 3.5f), p);
        canvas->drawLine(cx - 6.0f, cy - 2.0f, cx - 2.0f, cy + 1.0f, p);
        canvas->drawLine(cx - 2.0f, cy + 1.0f, cx - 6.0f, cy + 4.0f, p);
        canvas->drawLine(cx + 1.0f, cy + 4.0f, cx + 6.0f, cy + 4.0f, p);
    }

    void drawHomeIcon(SkCanvas* canvas, float cx, float cy, SkPaint p, float opacity) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(AgroColor::ink(static_cast<int>(210.0f * opacity)));

        SkPathBuilder path;
        path.moveTo(cx - 11.0f, cy);
        path.lineTo(cx, cy - 9.0f);
        path.lineTo(cx + 11.0f, cy);
        path.moveTo(cx - 8.0f, cy - 1.0f);
        path.lineTo(cx - 8.0f, cy + 9.0f);
        path.lineTo(cx + 8.0f, cy + 9.0f);
        path.lineTo(cx + 8.0f, cy - 1.0f);
        canvas->drawPath(path.detach(), p);
    }

    void drawSettingsIcon(SkCanvas* canvas, float cx, float cy, SkPaint p, float opacity) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor(AgroColor::ink(static_cast<int>(210.0f * opacity)));
        canvas->drawCircle(cx, cy, 8.0f, p);
        canvas->drawCircle(cx, cy, 3.0f, p);

        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
            const float x1 = cx + std::cos(a) * 9.5f;
            const float y1 = cy + std::sin(a) * 9.5f;
            const float x2 = cx + std::cos(a) * 12.0f;
            const float y2 = cy + std::sin(a) * 12.0f;
            canvas->drawLine(x1, y1, x2, y2, p);
        }
    }

    void drawSystemArea(SkCanvas* canvas, const TaskbarUI& ui, float y, float opacity) {
        const float width = 220.0f;
        const float x = TaskbarUI::Width - TaskbarUI::PanelMargin - 8.0f - width;
        const float yy = y + 18.0f;
        const float h = 44.0f;

        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(AgroColor::white(static_cast<int>(18.0f * opacity)));
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, yy, width, h), 14.0f, 14.0f), bg);

        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(1.8f);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor(AgroColor::ink(static_cast<int>(195.0f * opacity)));

        SkPathBuilder wifi;
        wifi.moveTo(x + 13.0f, yy + 18.0f);
        wifi.quadTo(x + 21.0f, yy + 10.0f, x + 29.0f, yy + 18.0f);
        canvas->drawPath(wifi.detach(), p);
        canvas->drawCircle(x + 21.0f, yy + 25.0f, 1.7f, p);

        const float pulse = ui.notificationPulse();
        canvas->drawCircle(x + 58.0f, yy + 21.5f, 6.0f, p);
        if (pulse > 0.01f) {
            SkPaint ring;
            ring.setAntiAlias(true);
            ring.setStyle(SkPaint::kStroke_Style);
            ring.setStrokeWidth(2.0f);
            ring.setColor(AgroColor::accent(static_cast<int>(190.0f * (1.0f - pulse) * opacity)));
            canvas->drawCircle(x + 58.0f, yy + 21.5f, 8.0f + pulse * 7.0f, ring);
        }

        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char timeText[16];
        std::snprintf(timeText, sizeof(timeText), "%02d:%02d", local.tm_hour, local.tm_min);

        SkFont font;
        font.setSize(14.0f);
        SkPaint text;
        text.setAntiAlias(true);
        text.setColor(AgroColor::ink(static_cast<int>(225.0f * opacity)));
        canvas->drawString(timeText, x + 95.0f, yy + 27.0f, font, text);
    }

    sk_sp<SkSurface> surface_;
};

} // namespace agro

int main() {
    std::printf(
        "============================================================\n"
        " AGROOS TASKBAR\n"
        " Native C++ + Skia + AgroUI + Wayland\n"
        "============================================================\n"
    );

    agro::TaskbarUI ui;
    agro::AgroRenderer renderer;

    if (!renderer.init()) {
        std::fprintf(stderr, "Skia renderer initialization failed.\n");
        return 1;
    }

    AgroWaylandWindow* win = agro_wl_create(
        "AgroOS Taskbar",
        agro::TaskbarUI::Width,
        agro::TaskbarUI::Height
    );

    if (!win) {
        std::fprintf(stderr, "AgroOS: Wayland penceresi olusturulamadi.\n");
        return 1;
    }

    struct InputContext {
        agro::TaskbarUI* ui;
        int pressedButton = -1;
    } input_ctx{&ui, -1};

    agro_wl_set_input_callback(
        win,
        [](const AgroInputEvent* ev, void* user_data) {
            auto* ctx = static_cast<InputContext*>(user_data);

            switch (ev->type) {
                case AGRO_INPUT_POINTER_MOTION:
                    ctx->ui->updatePointer(ev->x, ev->y);
                    break;

                case AGRO_INPUT_POINTER_BUTTON_DOWN:
                    if (ev->button != 1) break;
                    ctx->pressedButton = -1;
                    for (int id = 0; id <= agro::TaskbarUI::AppCount; ++id) {
                        const auto button = static_cast<agro::TaskbarUI::ButtonId>(id);
                        if (ctx->ui->hitTest(ev->x, ev->y, button)) {
                            ctx->pressedButton = id;
                            ctx->ui->setButtonPressed(static_cast<size_t>(id), true);
                            break;
                        }
                    }
                    if (ctx->pressedButton < 0) {
                        ctx->ui->setSearchFocused(ctx->ui->hitSearch(ev->x, ev->y));
                    }
                    break;

                case AGRO_INPUT_POINTER_BUTTON_UP:
                    if (ev->button != 1) break;
                    if (ctx->pressedButton >= 0) {
                        const int id = ctx->pressedButton;
                        ctx->ui->setButtonPressed(static_cast<size_t>(id), false);
                        ctx->ui->setActive(static_cast<agro::TaskbarUI::ButtonId>(id));
                        agro::performAction(static_cast<agro::TaskbarUI::ButtonId>(id));
                    }
                    ctx->pressedButton = -1;
                    break;

                case AGRO_INPUT_KEY_DOWN:
                case AGRO_INPUT_KEY_UP:
                    break;

                case AGRO_INPUT_CLOSE_REQUEST:
                    break;
            }
        },
        &input_ctx
    );

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    while (agro_wl_dispatch(win)) {
        const auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 15.0f);

        ui.update(dt);
        renderer.render(ui);

        int stride = 0;
        const uint32_t* src = renderer.peekPixels(&stride);
        uint32_t* dst = agro_wl_begin_frame(win);

        if (src && dst) {
            const int w = agro_wl_width(win);
            const int h = agro_wl_height(win);
            const int rowWidth = std::min(w, agro::TaskbarUI::Width);

            for (int y = 0; y < h; ++y) {
                const uint32_t* srow = src + static_cast<size_t>(y) * stride;
                uint32_t* drow = dst + static_cast<size_t>(y) * w;
                for (int x = 0; x < rowWidth; ++x) {
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
'''

if not TARGET.exists():
    raise SystemExit(f'Bulunamadi: {TARGET}')

backup = TARGET.with_suffix(TARGET.suffix + '.bak.' + datetime.now().strftime('%Y%m%d-%H%M%S'))
shutil.copy2(TARGET, backup)
TARGET.write_text(textwrap.dedent(CPP).lstrip(), encoding='utf-8')
print(f'Yedek: {backup}')
print(f'Guncellendi: {TARGET}')
print('agroshell.cpp modern, native-Skia UI ile degistirildi.')
