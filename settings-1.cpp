#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdexcept>
#include <linux/memfd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"

#if __has_include("xdg-shell-client-protocol.h")
#include "xdg-shell-client-protocol.h"
#elif __has_include("xdg-shell-protocol.h")
#include "xdg-shell-protocol.h"
#else
#error "xdg-shell-client-protocol.h or xdg-shell-protocol.h is required"
#endif

namespace agro {

struct Color {
    uint8_t r, g, b, a;
    SkColor sk() const { return SkColorSetARGB(a, r, g, b); }
};

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

static constexpr Color BG{232, 244, 255, 255};
static constexpr Color WINDOW{248, 251, 255, 245};
static constexpr Color PANEL{241, 247, 253, 230};
static constexpr Color CARD{255, 255, 255, 238};
static constexpr Color TEXT{25, 38, 52, 255};
static constexpr Color MUTED{102, 119, 136, 255};
static constexpr Color BORDER{205, 219, 232, 210};
static constexpr Color ACCENT{25, 132, 232, 255};
static constexpr Color ACCENT_SOFT{218, 238, 255, 255};
static constexpr Color GREEN{43, 165, 91, 255};
static constexpr Color PURPLE{116, 92, 214, 255};
static constexpr Color ORANGE{235, 151, 55, 255};
static constexpr Color RED{226, 75, 82, 255};
static constexpr Color CYAN{37, 173, 193, 255};

class Painter {
public:
    explicit Painter(SkCanvas* canvas) : canvas_(canvas) {}

    void fill(SkColor color) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(color);
        paint_.setMaskFilter(nullptr);
        canvas_->drawPaint(paint_);
    }

    void rect(Rect r, SkColor color, float radius = 0.0f, float alpha = 1.0f) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(withAlpha(color, static_cast<uint8_t>(SkColorGetA(color) * alpha)));
        paint_.setMaskFilter(nullptr);
        if (radius > 0.0f) {
            canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), radius, radius), paint_);
        } else {
            canvas_->drawRect(r.x, r.y, r.w, r.h, paint_);
        }
    }

    void stroke(Rect r, SkColor color, float width = 1.0f, float radius = 0.0f) {
        paint_.setStyle(SkPaint::kStroke_Style);
        paint_.setStrokeWidth(width);
        paint_.setColor(color);
        paint_.setMaskFilter(nullptr);
        if (radius > 0.0f) {
            canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), radius, radius), paint_);
        } else {
            canvas_->drawRect(r.x, r.y, r.w, r.h, paint_);
        }
    }

    void line(float x1, float y1, float x2, float y2, SkColor color, float width = 1.0f) {
        paint_.setStyle(SkPaint::kStroke_Style);
        paint_.setStrokeWidth(width);
        paint_.setStrokeCap(SkPaint::kRound_Cap);
        paint_.setColor(color);
        paint_.setMaskFilter(nullptr);
        canvas_->drawLine(x1, y1, x2, y2, paint_);
    }

    void circle(float cx, float cy, float radius, SkColor color) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(color);
        paint_.setMaskFilter(nullptr);
        canvas_->drawCircle(cx, cy, radius, paint_);
    }

    void text(const std::string& value, float x, float baseline, float size,
              SkColor color, bool bold = false) {
        SkFont font(typeface_, size);
        font.setEdging(SkFont::Edging::kAntiAlias);
        font.setSubpixel(true);
        if (bold) font.setTypeface(boldTypeface_);
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(color);
        paint_.setMaskFilter(nullptr);
        canvas_->drawString(value.c_str(), x, baseline, font, paint_);
    }

    float textWidth(const std::string& value, float size, bool bold = false) const {
        SkFont font(bold ? boldTypeface_ : typeface_, size);
        return font.measureText(value.c_str(), value.size(), SkTextEncoding::kUTF8);
    }

    void shadowRect(Rect r, float radius) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(SkColorSetARGB(40, 15, 35, 65));
        paint_.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 16.0f, 0.0f));
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y + 7, r.w, r.h), radius, radius), paint_);
        paint_.setMaskFilter(nullptr);
    }

    void iconLaptop(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kStroke_Style);
        paint_.setStrokeWidth(2.0f);
        paint_.setColor(c);
        canvas_->drawRoundRect(SkRect::MakeXYWH(cx - s * .38f, cy - s * .31f, s * .76f, s * .52f), 3, 3, paint_);
        canvas_->drawLine(cx - s * .48f, cy + s * .28f, cx + s * .48f, cy + s * .28f, paint_);
        canvas_->drawLine(cx - s * .35f, cy + s * .28f, cx + s * .35f, cy + s * .28f, paint_);
    }

    void iconWifi(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kStroke_Style);
        paint_.setStrokeWidth(2.3f);
        paint_.setStrokeCap(SkPaint::kRound_Cap);
        paint_.setColor(c);
        SkPath p;
        p.moveTo(cx - s*.38f, cy - s*.02f);
        p.quadTo(cx, cy - s*.34f, cx + s*.38f, cy - s*.02f);
        canvas_->drawPath(p, paint_);
        p.reset();
        p.moveTo(cx - s*.22f, cy + s*.12f);
        p.quadTo(cx, cy - s*.08f, cx + s*.22f, cy + s*.12f);
        canvas_->drawPath(p, paint_);
        circle(cx, cy + s*.22f, s*.045f, c);
    }

    void iconDevices(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kStroke_Style);
        paint_.setStrokeWidth(2.0f);
        paint_.setColor(c);
        canvas_->drawRoundRect(SkRect::MakeXYWH(cx - s*.45f, cy - s*.28f, s*.58f, s*.42f), 3, 3, paint_);
        canvas_->drawRoundRect(SkRect::MakeXYWH(cx - s*.02f, cy - s*.18f, s*.38f, s*.54f), 4, 4, paint_);
        canvas_->drawLine(cx - s*.28f, cy + s*.20f, cx + s*.08f, cy + s*.20f, paint_);
    }

    void iconPalette(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(c);
        SkPath p;
        p.moveTo(cx, cy - s*.42f);
        p.cubicTo(cx-s*.44f, cy-s*.42f, cx-s*.48f, cy+s*.36f, cx-s*.02f, cy+s*.40f);
        p.cubicTo(cx+s*.16f, cy+s*.42f, cx+s*.18f, cy+s*.26f, cx+s*.08f, cy+s*.19f);
        p.cubicTo(cx-s*.02f, cy+s*.12f, cx+s*.04f, cy-s*.03f, cx+s*.16f, cy-s*.04f);
        p.cubicTo(cx+s*.44f, cy-s*.05f, cx+s*.38f, cy-s*.42f, cx, cy-s*.42f);
        canvas_->drawPath(p, paint_);
        circle(cx-s*.18f, cy-s*.15f, s*.045f, BG.sk());
        circle(cx-s*.02f, cy-s*.25f, s*.045f, BG.sk());
        circle(cx+s*.14f, cy-s*.16f, s*.045f, BG.sk());
    }

    void iconLayers(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kFill_Style);
        paint_.setColor(c);
        SkPath p;
        p.moveTo(cx, cy-s*.40f); p.lineTo(cx+s*.42f, cy-s*.16f); p.lineTo(cx, cy+s*.08f); p.lineTo(cx-s*.42f, cy-s*.16f); p.close();
        canvas_->drawPath(p, paint_);
        p.reset(); p.moveTo(cx-s*.35f, cy-s*.02f); p.lineTo(cx, cy+s*.20f); p.lineTo(cx+s*.35f, cy-s*.02f);
        p.lineTo(cx+s*.35f, cy+s*.10f); p.lineTo(cx, cy+s*.32f); p.lineTo(cx-s*.35f, cy+s*.10f); p.close();
        canvas_->drawPath(p, paint_);
    }

    void iconUsers(float cx, float cy, float s, SkColor c) {
        circle(cx-s*.15f, cy-s*.18f, s*.16f, c);
        circle(cx+s*.22f, cy-s*.12f, s*.12f, c);
        paint_.setStyle(SkPaint::kStroke_Style); paint_.setStrokeWidth(2); paint_.setColor(c);
        canvas_->drawArc(SkRect::MakeXYWH(cx-s*.40f, cy+s*.02f, s*.58f, s*.38f), 200, 140, false, paint_);
        canvas_->drawArc(SkRect::MakeXYWH(cx-s*.02f, cy+s*.04f, s*.55f, s*.30f), 205, 130, false, paint_);
    }

    void iconClock(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kStroke_Style); paint_.setStrokeWidth(2.1f); paint_.setColor(c);
        canvas_->drawCircle(cx, cy, s*.35f, paint_);
        canvas_->drawLine(cx, cy, cx, cy-s*.20f, paint_);
        canvas_->drawLine(cx, cy, cx+s*.17f, cy+s*.10f, paint_);
    }

    void iconAccessibility(float cx, float cy, float s, SkColor c) {
        circle(cx, cy-s*.20f, s*.10f, c);
        paint_.setStyle(SkPaint::kStroke_Style); paint_.setStrokeWidth(2); paint_.setColor(c);
        canvas_->drawLine(cx, cy-s*.08f, cx, cy+s*.26f, paint_);
        canvas_->drawLine(cx-s*.25f, cy+s*.03f, cx+s*.25f, cy+s*.03f, paint_);
        canvas_->drawLine(cx, cy+s*.25f, cx-s*.18f, cy+s*.42f, paint_);
        canvas_->drawLine(cx, cy+s*.25f, cx+s*.18f, cy+s*.42f, paint_);
    }

    void iconShield(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kFill_Style); paint_.setColor(c);
        SkPath p;
        p.moveTo(cx, cy-s*.45f);
        p.lineTo(cx+s*.34f, cy-s*.28f);
        p.lineTo(cx+s*.28f, cy+s*.16f);
        p.quadTo(cx, cy+s*.46f, cx-s*.28f, cy+s*.16f);
        p.lineTo(cx-s*.34f, cy-s*.28f);
        p.close();
        canvas_->drawPath(p, paint_);
        paint_.setStyle(SkPaint::kStroke_Style); paint_.setStrokeWidth(2); paint_.setColor(WINDOW.sk());
        canvas_->drawLine(cx-s*.13f, cy, cx-s*.02f, cy+s*.12f, paint_);
        canvas_->drawLine(cx-s*.02f, cy+s*.12f, cx+s*.18f, cy-s*.12f, paint_);
    }

    void iconGame(float cx, float cy, float s, SkColor c) {
        paint_.setStyle(SkPaint::kStroke_Style); paint_.setStrokeWidth(2); paint_.setColor(c);
        canvas_->drawRoundRect(SkRect::MakeXYWH(cx-s*.43f, cy-s*.23f, s*.86f, s*.46f), 8, 8, paint_);
        canvas_->drawLine(cx-s*.25f, cy, cx-s*.05f, cy, paint_);
        canvas_->drawLine(cx-s*.15f, cy-s*.10f, cx-s*.15f, cy+s*.10f, paint_);
        circle(cx+s*.22f, cy-s*.05f, s*.035f, c);
        circle(cx+s*.30f, cy+s*.06f, s*.035f, c);
    }

    void iconDisplay(float cx, float cy, float s, SkColor c) { iconLaptop(cx, cy, s, c); }

private:
    static SkColor withAlpha(SkColor c, uint8_t a) { return SkColorSetARGB(a, SkColorGetR(c), SkColorGetG(c), SkColorGetB(c)); }
    SkCanvas* canvas_ = nullptr;
    SkPaint paint_;
    sk_sp<SkTypeface> typeface_ = SkTypeface::MakeFromName("Inter", SkFontStyle::Normal());
    sk_sp<SkTypeface> boldTypeface_ = SkTypeface::MakeFromName("Inter", SkFontStyle::Bold());
};

struct Category {
    std::string name;
    std::string desc;
    Color accent;
    int icon = 0;
};

class SettingsUI {
public:
    SettingsUI()
        : categories_({
            {"System", "Display, sound, notifications, power", ACCENT, 0},
            {"Display", "Brightness, scale, wallpaper", CYAN, 1},
            {"Personalization", "Background, colors, desktop skin", PURPLE, 2},
            {"Sound", "Volume, output, input", ORANGE, 3},
            {"Network", "Wi-Fi, VPN, connections", GREEN, 4},
            {"About", "AgroOS version and device", RED, 5}
        }) {}

    void resize(int w, int h) {
        width_ = std::max(900, w);
        height_ = std::max(620, h);
    }

    void render(SkCanvas* canvas) {
        Painter p(canvas);
        p.fill(BG.sk());

        const float inset = 0.0f;
        p.shadowRect({inset + 4, inset + 6, float(width_) - 8, float(height_) - 12}, 18);
        p.rect({0, 0, float(width_), float(height_)}, WINDOW.sk(), 18);

        renderTitlebar(p);
        renderSidebar(p);
        renderContent(p);
        renderWindowButtons(p);
    }

    void pointerMove(float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
        hoverCategory_ = categoryAt(x, y);
        hoverButton_ = titleButtonAt(x, y);
        hoverControl_ = controlAt(x, y);
    }

    bool pointerDown(float x, float y, uint32_t serial, wl_seat* seat, xdg_toplevel* toplevel) {
        mouseX_ = x;
        mouseY_ = y;
        const TitleButton tb = titleButtonAt(x, y);
        if (tb != TitleButton::None) {
            pressedButton_ = tb;
            return true;
        }

        const int cat = categoryAt(x, y);
        if (cat >= 0) {
            activeCategory_ = cat;
            searchFocus_ = false;
            return true;
        }

        if (controlAt(x, y) == Control::Search) {
            searchFocus_ = true;
            return true;
        }

        if (controlAt(x, y) == Control::Switch) {
            notifications_ = !notifications_;
            dirty_ = true;
            return true;
        }

        if (controlAt(x, y) == Control::Slider) {
            brightness_ = std::clamp((x - 615.0f) / 430.0f, 0.0f, 1.0f);
            dirty_ = true;
            draggingSlider_ = true;
            return true;
        }

        if (seat && toplevel && y < 48.0f && tb == TitleButton::None) {
            xdg_toplevel_move(toplevel, seat, serial);
            return true;
        }
        return false;
    }

    void pointerUp(float x, float y) {
        if (draggingSlider_) draggingSlider_ = false;
        const TitleButton tb = titleButtonAt(x, y);
        if (pressedButton_ != TitleButton::None && pressedButton_ == tb) {
            action_ = pressedButton_;
        }
        pressedButton_ = TitleButton::None;
    }

    void pointerScroll(float delta) {
        if (activeCategory_ == 0) {
            contentScroll_ = std::clamp(contentScroll_ + delta * 35.0f, 0.0f, 180.0f);
            dirty_ = true;
        }
    }

    void keyDown(uint32_t key) {
        if (!searchFocus_) {
            if (key == KEY_ESC) action_ = TitleButton::Close;
            return;
        }

        if (key == KEY_BACKSPACE) {
            if (!search_.empty()) search_.pop_back();
            dirty_ = true;
            return;
        }
        if (key == KEY_ENTER) {
            searchFocus_ = false;
            return;
        }
        if (key == KEY_ESC) {
            searchFocus_ = false;
            search_.clear();
            dirty_ = true;
            return;
        }
        const char c = keyToChar(key);
        if (c) {
            search_.push_back(c);
            dirty_ = true;
        }
    }

    TitleButton takeAction() {
        const TitleButton a = action_;
        action_ = TitleButton::None;
        return a;
    }

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    bool sliderDragging() const { return draggingSlider_; }

    void dragSlider(float x) {
        if (!draggingSlider_) return;
        brightness_ = std::clamp((x - 615.0f) / 430.0f, 0.0f, 1.0f);
        dirty_ = true;
    }

    enum class TitleButton { None, Minimize, Maximize, Close };
    enum class Control { None, Search, Switch, Slider };

private:
    int width_ = 1180;
    int height_ = 760;
    int activeCategory_ = 0;
    int hoverCategory_ = -1;
    TitleButton hoverButton_ = TitleButton::None;
    TitleButton pressedButton_ = TitleButton::None;
    TitleButton action_ = TitleButton::None;
    Control hoverControl_ = Control::None;
    bool searchFocus_ = false;
    bool notifications_ = true;
    bool nightLight_ = false;
    bool bluetooth_ = true;
    bool draggingSlider_ = false;
    bool dirty_ = true;
    float brightness_ = 0.73f;
    float contentScroll_ = 0.0f;
    float mouseX_ = 0;
    float mouseY_ = 0;
    std::string search_;
    std::vector<Category> categories_;

    int categoryAt(float x, float y) const {
        if (x > 260.0f) return -1;
        const float top = 96.0f;
        const float row = 66.0f;
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            if (Rect{18.0f, top + i * row, 224.0f, 56.0f}.contains(x, y)) return i;
        }
        return -1;
    }

    TitleButton titleButtonAt(float x, float y) const {
        if (y > 46.0f) return TitleButton::None;
        if (Rect{width_ - 136.0f, 0, 42, 46}.contains(x, y)) return TitleButton::Minimize;
        if (Rect{width_ - 94.0f, 0, 42, 46}.contains(x, y)) return TitleButton::Maximize;
        if (Rect{width_ - 52.0f, 0, 52, 46}.contains(x, y)) return TitleButton::Close;
        return TitleButton::None;
    }

    Control controlAt(float x, float y) const {
        if (Rect{width_ - 420.0f, 68.0f, 300.0f, 42.0f}.contains(x, y)) return Control::Search;
        if (activeCategory_ == 0 && Rect{618.0f, 286.0f, 70.0f, 34.0f}.contains(x, y)) return Control::Switch;
        if (activeCategory_ == 0 && Rect{570.0f, 420.0f, 500.0f, 36.0f}.contains(x, y)) return Control::Slider;
        return Control::None;
    }

    char keyToChar(uint32_t key) const {
        if (key >= KEY_A && key <= KEY_Z) return static_cast<char>('a' + key - KEY_A);
        if (key >= KEY_1 && key <= KEY_9) return static_cast<char>('1' + key - KEY_1);
        if (key == KEY_0) return '0';
        if (key == KEY_SPACE) return ' ';
        if (key == KEY_MINUS) return '-';
        return 0;
    }

    void renderTitlebar(Painter& p) {
        p.rect({0, 0, float(width_), 46}, SkColorSetARGB(238, 252, 254, 255), 18);
        p.line(0, 45.5f, float(width_), 45.5f, BORDER.sk(), 1);
        p.circle(24, 23, 9, ACCENT.sk());
        p.iconLayers(24, 23, 12, WINDOW.sk());
        p.text("Settings", 46, 29, 16, TEXT.sk(), true);

        p.rect({width_ - 136.0f, 0, 42, 46}, hoverButton_ == TitleButton::Minimize ? ACCENT_SOFT.sk() : WINDOW.sk());
        p.rect({width_ - 94.0f, 0, 42, 46}, hoverButton_ == TitleButton::Maximize ? ACCENT_SOFT.sk() : WINDOW.sk());
        p.rect({width_ - 52.0f, 0, 52, 46}, hoverButton_ == TitleButton::Close ? SkColorSetARGB(255, 255, 232, 234) : WINDOW.sk());
        p.line(width_ - 118, 23, width_ - 104, 23, MUTED.sk(), 1.8f);
        p.stroke({float(width_) - 80, 16, 12, 12}, MUTED.sk(), 1.6f, 2);
        p.line(width_ - 36, 16, width_ - 23, 30, hoverButton_ == TitleButton::Close ? RED.sk() : MUTED.sk(), 1.8f);
        p.line(width_ - 23, 16, width_ - 36, 30, hoverButton_ == TitleButton::Close ? RED.sk() : MUTED.sk(), 1.8f);
    }

    void renderSidebar(Painter& p) {
        p.rect({0, 46, 260, float(height_) - 46}, PANEL.sk());
        p.line(259.5f, 46, 259.5f, float(height_), BORDER.sk(), 1);
        p.text("AgroOS", 24, 82, 13, MUTED.sk(), true);
        p.text("Settings", 24, 104, 28, TEXT.sk(), true);

        float y = 122;
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            const bool active = i == activeCategory_;
            const bool hover = i == hoverCategory_;
            if (active || hover) {
                p.rect({14, y, 232, 52}, active ? ACCENT_SOFT.sk() : SkColorSetARGB(180, 229, 241, 251), 12);
            }
            p.circle(40, y + 26, 16, categories_[i].accent.sk());
            drawCategoryIcon(p, i, 40, y + 26, 22, WINDOW.sk());
            p.text(categories_[i].name, 68, y + 23, 15, TEXT.sk(), active);
            p.text(shortDesc(i), 68, y + 41, 10.5f, MUTED.sk(), false);
            y += 66;
        }

        p.rect({18, float(height_) - 88, 224, 54}, SkColorSetARGB(160, 255, 255, 255), 14);
        p.circle(42, height_ - 61, 15, ACCENT_SOFT.sk());
        p.iconShield(42, height_ - 61, 20, ACCENT.sk());
        p.text("AgroOS 0.1", 66, height_ - 64, 13, TEXT.sk(), true);
        p.text("Secure desktop session", 66, height_ - 46, 10.5f, MUTED.sk());
    }

    std::string shortDesc(int i) const {
        switch (i) {
            case 0: return "Display, sound, power";
            case 1: return "Brightness and wallpaper";
            case 2: return "Colors and desktop";
            case 3: return "Volume and devices";
            case 4: return "Wi-Fi and VPN";
            default: return "System information";
        }
    }

    void drawCategoryIcon(Painter& p, int i, float cx, float cy, float s, SkColor c) {
        switch (i) {
            case 0: p.iconLaptop(cx, cy, s, c); break;
            case 1: p.iconDisplay(cx, cy, s, c); break;
            case 2: p.iconPalette(cx, cy, s, c); break;
            case 3: p.iconLayers(cx, cy, s, c); break;
            case 4: p.iconWifi(cx, cy, s, c); break;
            default: p.iconShield(cx, cy, s, c); break;
        }
    }

    void renderContent(Painter& p) {
        const float left = 260.0f;
        p.rect({left, 46, float(width_) - left, float(height_) - 46}, SkColorSetARGB(205, 246, 250, 255));

        const float right = float(width_) - 32.0f;
        p.text(pageTitle(), left + 34, 94, 31, TEXT.sk(), true);
        p.text(pageSubtitle(), left + 34, 120, 13.5f, MUTED.sk());

        p.rect({right - 300, 68, 300, 42}, SkColorSetARGB(245, 255, 255, 255), 12);
        p.stroke({right - 300, 68, 300, 42}, BORDER.sk(), 1, 12);
        drawSearchIcon(p, right - 278, 89, 15, MUTED.sk());
        p.text(searchFocus_ && search_.empty() ? "Type to search" : (search_.empty() ? "Find the setting" : search_), right - 255, 94, 13, search_.empty() ? MUTED.sk() : TEXT.sk());

        if (activeCategory_ == 0) renderSystem(p, left);
        else renderSecondary(p, left);
    }

    std::string pageTitle() const {
        return categories_[activeCategory_].name;
    }

    std::string pageSubtitle() const {
        switch (activeCategory_) {
            case 0: return "Display, sound, notifications, power";
            case 1: return "Control brightness, scale and wallpaper";
            case 2: return "Choose colors, icons and desktop style";
            case 3: return "Manage sound output and input devices";
            case 4: return "Connect to networks and VPN services";
            default: return "AgroOS version, hardware and runtime information";
        }
    }

    void renderSystem(Painter& p, float left) {
        const float x = left + 34;
        const float w = float(width_) - x - 32;
        const float y = 150 - contentScroll_;

        card(p, {x, y, w, 112});
        p.circle(x + 38, y + 38, 25, ACCENT_SOFT.sk());
        p.iconLaptop(x + 38, y + 38, 34, ACCENT.sk());
        p.text("Your device", x + 78, y + 34, 17, TEXT.sk(), true);
        p.text("AgroOS Desktop", x + 78, y + 57, 12, MUTED.sk());
        p.text("Wayland session • Skia renderer", x + 78, y + 80, 11.5f, MUTED.sk());
        p.rect({x + w - 142, y + 30, 112, 42}, ACCENT_SOFT.sk(), 11);
        p.text("Open info", x + w - 115, y + 56, 12.5f, ACCENT.sk(), true);

        card(p, {x, y + 130, w, 154});
        p.text("Notifications", x + 24, y + 162, 18, TEXT.sk(), true);
        p.text("Show desktop and system notifications", x + 24, y + 184, 12, MUTED.sk());
        drawSwitch(p, x + w - 90, y + 150, notifications_, ACCENT.sk());
        p.text("Night light", x + 24, y + 230, 16, TEXT.sk(), true);
        p.text("Reduce blue light during evening hours", x + 24, y + 250, 12, MUTED.sk());
        drawSwitch(p, x + w - 90, y + 216, nightLight_, PURPLE.sk());

        card(p, {x, y + 302, w, 178});
        p.text("Display", x + 24, y + 334, 18, TEXT.sk(), true);
        p.text("Brightness", x + 24, y + 357, 12, MUTED.sk());
        p.text(std::to_string(static_cast<int>(brightness_ * 100)) + "%", x + w - 70, y + 357, 12, ACCENT.sk(), true);
        drawSlider(p, x + 24, y + 378, w - 48, brightness_, ACCENT.sk());
        p.text("Scale", x + 24, y + 432, 12, MUTED.sk());
        p.rect({x + 24, y + 448, 170, 34}, WINDOW.sk(), 9);
        p.text("100%", x + 42, y + 470, 12.5f, TEXT.sk());
        p.text("Resolution", x + 245, y + 432, 12, MUTED.sk());
        p.rect({x + 245, y + 448, 240, 34}, WINDOW.sk(), 9);
        p.text("1280 × 800", x + 263, y + 470, 12.5f, TEXT.sk());
    }

    void renderSecondary(Painter& p, float left) {
        const float x = left + 34;
        const float w = float(width_) - x - 32;
        const float y = 150;
        card(p, {x, y, w, 128});
        p.circle(x + 44, y + 48, 25, categories_[activeCategory_].accent.sk());
        drawCategoryIcon(p, activeCategory_, x + 44, y + 48, 34, WINDOW.sk());
        p.text(categories_[activeCategory_].name + " overview", x + 82, y + 42, 19, TEXT.sk(), true);
        p.text(categories_[activeCategory_].desc, x + 82, y + 65, 12, MUTED.sk());
        p.text("This panel is ready for the next AgroOS settings module.", x + 82, y + 89, 11.5f, MUTED.sk());

        card(p, {x, y + 148, (w - 16) * .5f, 176});
        card(p, {x + (w - 16) * .5f + 16, y + 148, (w - 16) * .5f, 176});
        p.text("Quick settings", x + 24, y + 181, 17, TEXT.sk(), true);
        p.text("Common controls will live here.", x + 24, y + 204, 12, MUTED.sk());
        p.rect({x + 24, y + 230, 130, 38}, ACCENT_SOFT.sk(), 10);
        p.text("Open panel", x + 45, y + 255, 12, ACCENT.sk(), true);
        p.text("Details", x + (w - 16) * .5f + 40, y + 181, 17, TEXT.sk(), true);
        p.text("Settings are stored per-user.", x + (w - 16) * .5f + 40, y + 204, 12, MUTED.sk());
        p.rect({x + (w - 16) * .5f + 40, y + 230, 130, 38}, SkColorSetARGB(220, 247, 247, 249), 10);
        p.text("Learn more", x + (w - 16) * .5f + 58, y + 255, 12, TEXT.sk(), true);
    }

    void card(Painter& p, Rect r) {
        p.shadowRect(r, 14);
        p.rect(r, CARD.sk(), 14);
        p.stroke(r, BORDER.sk(), 1, 14);
    }

    void drawSwitch(Painter& p, float x, float y, bool on, SkColor color) {
        p.rect({x, y, 62, 34}, on ? color : SkColorSetARGB(235, 214, 224, 233), 17);
        p.circle(on ? x + 45 : x + 17, y + 17, 12, SkColorSetARGB(255, 255, 255, 255));
    }

    void drawSlider(Painter& p, float x, float y, float w, float value, SkColor color) {
        p.rect({x, y, w, 6}, SkColorSetARGB(255, 218, 228, 237), 3);
        p.rect({x, y, w * value, 6}, color, 3);
        p.circle(x + w * value, y + 3, 10, color);
        p.circle(x + w * value, y + 3, 5, WINDOW.sk());
    }

    void drawSearchIcon(Painter& p, float cx, float cy, float s, SkColor c) {
        p.stroke({cx - s*.38f, cy - s*.38f, s*.65f, s*.65f}, c, 1.8f, s*.32f);
        p.line(cx + s*.16f, cy + s*.16f, cx + s*.42f, cy + s*.42f, c, 1.8f);
    }

public:
    static int iconWidth() { return 0; }
};

class WaylandSettingsApp {
public:
    bool init() {
        display_ = wl_display_connect(nullptr);
        if (!display_) return false;
        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &registryListener, this);
        if (wl_display_roundtrip(display_) < 0) return false;
        if (!compositor_ || !shm_ || !seat_ || !wmBase_) return false;

        wl_seat_add_listener(seat_, &seatListener, this);
        xdg_wm_base_add_listener(wmBase_, &wmBaseListener, this);
        if (wl_display_roundtrip(display_) < 0) return false;

        surface_ = wl_compositor_create_surface(compositor_);
        xdgSurface_ = xdg_wm_base_get_xdg_surface(wmBase_, surface_);
        xdg_surface_add_listener(xdgSurface_, &xdgSurfaceListener, this);
        toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
        xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
        xdg_toplevel_set_title(toplevel_, "AgroOS Settings");
        xdg_toplevel_set_app_id(toplevel_, "agroos-settings");

        wl_surface_commit(surface_);
        wl_display_roundtrip(display_);
        return true;
    }

    int run() {
        while (!running_) {
            if (wl_display_dispatch(display_) < 0) return 1;
        }
        return 0;
    }

private:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    xdg_wm_base* wmBase_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    wl_buffer* buffer_ = nullptr;
    void* pixels_ = nullptr;
    size_t stride_ = 0;
    size_t bytes_ = 0;
    int fd_ = -1;
    bool bufferBusy_ = false;
    bool configured_ = false;
    bool running_ = false;
    bool pointerInside_ = false;
    uint32_t lastSerial_ = 0;
    int width_ = 1180;
    int height_ = 760;
    SettingsUI ui_;
    sk_sp<SkSurface> skiaSurface_;

    static constexpr wl_registry_listener registryListener = {
        registryGlobal,
        registryRemove
    };

    static constexpr wl_seat_listener seatListener = {
        seatCapabilities,
        seatName
    };

    static constexpr xdg_wm_base_listener wmBaseListener = {
        wmPing
    };

    static constexpr xdg_surface_listener xdgSurfaceListener = {
        surfaceConfigure
    };

    template <typename T, typename = void>
    struct HasConfigureBounds : std::false_type {};

    template <typename T>
    struct HasConfigureBounds<T, std::void_t<decltype(((T*)nullptr)->configure_bounds)>> : std::true_type {};

    template <typename T, typename = void>
    struct HasWmCapabilities : std::false_type {};

    template <typename T>
    struct HasWmCapabilities<T, std::void_t<decltype(((T*)nullptr)->wm_capabilities)>> : std::true_type {};

    static xdg_toplevel_listener makeToplevelListener() {
        xdg_toplevel_listener l{};
        l.configure = toplevelConfigure;
        l.close = toplevelClose;
        if constexpr (HasConfigureBounds<xdg_toplevel_listener>::value) l.configure_bounds = toplevelConfigureBounds;
        if constexpr (HasWmCapabilities<xdg_toplevel_listener>::value) l.wm_capabilities = toplevelWmCapabilities;
        return l;
    }

    inline static xdg_toplevel_listener toplevelListener = makeToplevelListener();

    static constexpr wl_pointer_listener pointerListener = {
        pointerEnter,
        pointerLeave,
        pointerMotion,
        pointerButton,
        pointerAxis,
        pointerFrame,
        pointerAxisSource,
        pointerAxisStop,
        pointerAxisDiscrete
    };

    static constexpr wl_buffer_listener bufferListener = {
        bufferRelease
    };

    static void registryGlobal(void* data, wl_registry* registry, uint32_t name,
                               const char* interface, uint32_t version) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->shm_ = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
            self->wmBase_ = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
        }
    }

    static void registryRemove(void*, wl_registry*, uint32_t) {}

    static void seatCapabilities(void* data, wl_seat* seat, uint32_t caps) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
            self->pointer_ = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(self->pointer_, &pointerListener, self);
        }
        if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self->pointer_) {
            wl_pointer_destroy(self->pointer_);
            self->pointer_ = nullptr;
        }
    }

    static void seatName(void*, wl_seat*, const char*) {}

    static void wmPing(void*, xdg_wm_base* base, uint32_t serial) {
        xdg_wm_base_pong(base, serial);
    }

    static void surfaceConfigure(void* data, xdg_surface* surface, uint32_t serial) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        xdg_surface_ack_configure(surface, serial);
        if (!self->configured_) {
            self->configured_ = true;
            self->createBuffer();
            self->draw();
        } else if (self->bufferBusy_) {
            self->drawWhenReleased_ = true;
        }
    }

    static void toplevelConfigure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        if (w > 0) self->width_ = w;
        if (h > 0) self->height_ = h;
        self->ui_.resize(self->width_, self->height_);
        if (self->configured_ && !self->bufferBusy_) {
            self->recreateBuffer();
            self->draw();
        } else {
            self->resizePending_ = true;
        }
    }

    static void toplevelClose(void* data, xdg_toplevel*) {
        static_cast<WaylandSettingsApp*>(data)->running_ = true;
    }

    static void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
    static void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}

    static void pointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        if (surface != self->surface_) return;
        self->lastSerial_ = serial;
        self->pointerInside_ = true;
        self->lastX_ = wl_fixed_to_double(sx);
        self->lastY_ = wl_fixed_to_double(sy);
        self->ui_.pointerMove(self->lastX_, self->lastY_);
        self->requestRedraw();
    }

    static void pointerLeave(void* data, wl_pointer*, uint32_t serial, wl_surface*) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        self->lastSerial_ = serial;
        self->pointerInside_ = false;
    }

    static void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        const float x = wl_fixed_to_double(sx);
        const float y = wl_fixed_to_double(sy);
        self->lastX_ = x;
        self->lastY_ = y;
        self->ui_.pointerMove(x, y);
        if (self->ui_.sliderDragging()) self->ui_.dragSlider(x);
        self->requestRedraw();
    }

    static void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        self->lastSerial_ = serial;
        if (button != BTN_LEFT) return;
        const float x = self->lastX_;
        const float y = self->lastY_;
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            self->ui_.pointerDown(x, y, serial, self->seat_, self->toplevel_);
            self->requestRedraw();
        } else {
            self->ui_.pointerUp(x, y);
            const auto action = self->ui_.takeAction();
            if (action == SettingsUI::TitleButton::Close) {
                self->running_ = true;
                return;
            }
            if (action == SettingsUI::TitleButton::Minimize) {
                xdg_toplevel_set_minimized(self->toplevel_);
            } else if (action == SettingsUI::TitleButton::Maximize) {
                if (self->maximized_) {
                    xdg_toplevel_unset_maximized(self->toplevel_);
                    self->maximized_ = false;
                } else {
                    xdg_toplevel_set_maximized(self->toplevel_);
                    self->maximized_ = true;
                }
            }
            self->requestRedraw();
        }
    }

    static void pointerAxis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) self->ui_.pointerScroll(static_cast<float>(wl_fixed_to_double(value)));
        self->requestRedraw();
    }

    static void pointerFrame(void*, wl_pointer*) {}
    static void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
    static void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, uint32_t, int32_t) {}

    static void bufferRelease(void* data, wl_buffer* buffer) {
        auto* self = static_cast<WaylandSettingsApp*>(data);
        self->bufferBusy_ = false;
        if (self->resizePending_) {
            self->recreateBuffer();
            self->resizePending_ = false;
        }
        if (self->drawWhenReleased_ || self->ui_.dirty()) {
            self->drawWhenReleased_ = false;
            self->draw();
        }
    }

    void requestRedraw() {
        if (!bufferBusy_) draw();
    }

    void draw() {
        if (!configured_ || !skiaSurface_ || bufferBusy_) return;
        ui_.clearDirty();
        SkCanvas* canvas = skiaSurface_->getCanvas();
        ui_.render(canvas);
        canvas->flush();
        wl_surface_attach(surface_, buffer_, 0, 0);
        wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
        wl_surface_commit(surface_);
        bufferBusy_ = true;
    }

    void createBuffer() {
        recreateBuffer();
    }

    void recreateBuffer() {
        if (buffer_) {
            wl_buffer_destroy(buffer_);
            buffer_ = nullptr;
        }
        if (pixels_) {
            munmap(pixels_, bytes_);
            pixels_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }

        stride_ = static_cast<size_t>(width_) * 4;
        bytes_ = stride_ * static_cast<size_t>(height_);
        fd_ = createAnonymousFile(bytes_);
        if (fd_ < 0) throw std::runtime_error("memfd/shm allocation failed");
        pixels_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (pixels_ == MAP_FAILED) {
            pixels_ = nullptr;
            throw std::runtime_error("mmap failed");
        }

        wl_shm_pool* pool = wl_shm_create_pool(shm_, fd_, static_cast<int>(bytes_));
        buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, static_cast<int>(stride_), WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        wl_buffer_add_listener(buffer_, &bufferListener, this);

        const SkImageInfo info = SkImageInfo::Make(width_, height_, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        skiaSurface_ = SkSurfaces::WrapPixels(info, pixels_, stride_);
    }

    static int createAnonymousFile(size_t size) {
#ifdef MFD_CLOEXEC
        int fd = memfd_create("agroos-settings", MFD_CLOEXEC);
#else
        char path[] = "/tmp/agroos-settings-XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) unlink(path);
#endif
        if (fd < 0) return -1;
        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    float lastX_ = 0.0f;
    float lastY_ = 0.0f;
    bool drawWhenReleased_ = false;
    bool resizePending_ = false;
    bool maximized_ = false;
};

} // namespace agro

int main() {
    try {
        agro::WaylandSettingsApp app;
        if (!app.init()) return 1;
        return app.run();
    } catch (...) {
        return 2;
    }
}
