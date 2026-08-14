// agroos-webos.cc
//
// AgroOS WebOS masaüstü penceresi.
//
// agroos-webui-latest.cc'den (installer/OOBE) FARKLI, AYRI bir binary.
// O dosyaya hiç dokunulmadı — hâlâ installer + OOBE + kurulum-sonrası ekran
// olarak aynen çalışıyor.
//
// Bu dosya CEF'i "windowless" (OSR) modda çalıştırır ama installer'ın
// aksine çıktıyı framebuffer'a değil, gerçek bir Wayland xdg_toplevel
// penceresine (agroos-compositor üzerinde) basar. Yani derste compositor
// üzerinde diğer pencerelerle (Skia UI dahil) birlikte var olabilen,
// taşınabilir/yeniden boyutlandırılabilir gerçek bir pencere olur.
//
// React tarafı: file:///agroos/webos/dist/index.html (Lovable'da üretilip
// VPS'te build alınan, sonra Colab'a çekilen statik SPA çıktısı).

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <memory>
#include <mutex>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/wrapper/cef_message_router.h"

// =============================================================================
// Wayland taraf: xdg_toplevel penceresi + wl_shm paylaşımlı buffer.
//
// ui/common/agro_appwindow.h'daki AppWindow ile AYNI deseni (registry,
// xdg_wm_base, xdg_toplevel, wl_shm çift-buffer) kullanır ama Skia'ya değil
// CEF'in ham BGRA piksel çıktısına bağlıdır — bu yüzden AppWindow'dan miras
// almak yerine kendi küçük Wayland katmanını taşır. agro_appwindow.h/.cpp'ye
// dokunulmadı.
// =============================================================================

namespace agrowebos {

// Anonim paylaşımlı bellek dosyası oluşturur (memfd_create varsa onu,
// yoksa mkstemp+unlink fallback'ini kullanır — Skia UI tarafında da aynı
// desen zaten var, burada bağımsız kopyası).
static int createAnonymousFile(size_t size) {
#if defined(SYS_memfd_create)
  {
    int fd = syscall(SYS_memfd_create, "agroos-webos-shm", 0);
    if (fd >= 0) {
      if (ftruncate(fd, size) == 0) return fd;
      close(fd);
    }
  }
#endif
  char path[] = "/tmp/agroos-webos-shm-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) return -1;
  unlink(path);
  if (ftruncate(fd, size) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

class WaylandWindow {
 public:
  WaylandWindow(const std::string& title, int width, int height)
      : title_(title), width_(width), height_(height) {}

  ~WaylandWindow() { shutdown(); }

  bool initialize() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
      fprintf(stderr, "[agroos-webos] wl_display_connect basarisiz — compositor calisiyor mu?\n");
      return false;
    }

    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener registryListener = {
        .global = registryGlobalCb,
        .global_remove = registryGlobalRemoveCb,
    };
    wl_registry_add_listener(registry_, &registryListener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !shm_ || !xdgWmBase_) {
      fprintf(stderr, "[agroos-webos] gerekli Wayland globalleri bulunamadi "
                       "(compositor=%p shm=%p xdg_wm_base=%p)\n",
              (void*)compositor_, (void*)shm_, (void*)xdgWmBase_);
      return false;
    }

    surface_ = wl_compositor_create_surface(compositor_);
    xdgSurface_ = xdg_wm_base_get_xdg_surface(xdgWmBase_, surface_);
    static const xdg_surface_listener xdgSurfaceListener = {
        .configure = xdgSurfaceConfigureCb,
    };
    xdg_surface_add_listener(xdgSurface_, &xdgSurfaceListener, this);

    xdgToplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    static const xdg_toplevel_listener xdgToplevelListener = {
        .configure = xdgToplevelConfigureCb,
        .close = xdgToplevelCloseCb,
    };
    xdg_toplevel_add_listener(xdgToplevel_, &xdgToplevelListener, this);
    xdg_toplevel_set_title(xdgToplevel_, title_.c_str());
    xdg_toplevel_set_app_id(xdgToplevel_, "agroos-webos");

    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);  // ilk configure'u bekle

    return true;
  }

  // CEF OnPaint'ten çağrılır. buffer BGRA32, width/height CEF view boyutu.
  // dirtyRects'i şimdilik yok sayıp her seferinde tam kareyi kopyalıyoruz —
  // ilk sürüm için basitlik/doğruluk önceliği; performans gerekiyorsa
  // sonra dirty-rect'e göre kısmi kopyaya geçilebilir.
  void presentFrame(const void* bgra, int width, int height) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (width != width_ || height != height_) {
      // CEF boyutu değişti (configure sonrası) — buffer'ı yeniden kur.
      width_ = width;
      height_ = height;
      destroyShmBuffer();
    }
    if (!shmData_) {
      if (!createShmBuffer(width_, height_)) return;
    }
    memcpy(shmData_, bgra, static_cast<size_t>(width_) * height_ * 4);
    wl_surface_attach(surface_, buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
    wl_surface_commit(surface_);
  }

  // Ana Wayland event döngüsünü pompalar. GLib/CEF döngüsüyle birlikte
  // dışarıdan periyodik çağrılır (bkz. main() içindeki CefDoMessageLoopWork
  // + wl_display_dispatch_pending kombinasyonu).
  void pumpEvents() {
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
  }

  int width() const { return width_; }
  int height() const { return height_; }
  bool shouldClose() const { return shouldClose_; }

  void shutdown() {
    destroyShmBuffer();
    if (xdgToplevel_) xdg_toplevel_destroy(xdgToplevel_);
    if (xdgSurface_) xdg_surface_destroy(xdgSurface_);
    if (surface_) wl_surface_destroy(surface_);
    if (xdgWmBase_) xdg_wm_base_destroy(xdgWmBase_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
    xdgToplevel_ = nullptr;
    xdgSurface_ = nullptr;
    surface_ = nullptr;
    xdgWmBase_ = nullptr;
    shm_ = nullptr;
    compositor_ = nullptr;
    registry_ = nullptr;
    display_ = nullptr;
  }

 private:
  bool createShmBuffer(int width, int height) {
    int stride = width * 4;
    size_t size = static_cast<size_t>(stride) * height;
    int fd = createAnonymousFile(size);
    if (fd < 0) {
      fprintf(stderr, "[agroos-webos] paylasimli bellek dosyasi olusturulamadi\n");
      return false;
    }
    shmData_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmData_ == MAP_FAILED) {
      close(fd);
      shmData_ = nullptr;
      return false;
    }
    shmSize_ = size;
    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int32_t>(size));
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);  // buffer mmap'i tuttugu icin fd'ye artik gerek yok
    static const wl_buffer_listener bufferListener = {
        .release = bufferReleaseCb,
    };
    wl_buffer_add_listener(buffer_, &bufferListener, this);
    return true;
  }

  void destroyShmBuffer() {
    if (buffer_) {
      wl_buffer_destroy(buffer_);
      buffer_ = nullptr;
    }
    if (shmData_) {
      munmap(shmData_, shmSize_);
      shmData_ = nullptr;
      shmSize_ = 0;
    }
  }

  // ---- Wayland callback'leri ----

  static void registryGlobalCb(void* data, wl_registry* registry, uint32_t id,
                                const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandWindow*>(data);
    std::string iface(interface);
    if (iface == wl_compositor_interface.name) {
      self->compositor_ = static_cast<wl_compositor*>(
          wl_registry_bind(registry, id, &wl_compositor_interface, 4));
    } else if (iface == wl_shm_interface.name) {
      self->shm_ = static_cast<wl_shm*>(
          wl_registry_bind(registry, id, &wl_shm_interface, 1));
    } else if (iface == xdg_wm_base_interface.name) {
      self->xdgWmBase_ = static_cast<xdg_wm_base*>(
          wl_registry_bind(registry, id, &xdg_wm_base_interface, 1));
      static const xdg_wm_base_listener wmBaseListener = {
          .ping = xdgWmBasePingCb,
      };
      xdg_wm_base_add_listener(self->xdgWmBase_, &wmBaseListener, self);
    }
  }

  static void registryGlobalRemoveCb(void*, wl_registry*, uint32_t) {}

  static void xdgWmBasePingCb(void*, xdg_wm_base* wmBase, uint32_t serial) {
    xdg_wm_base_pong(wmBase, serial);
  }

  static void xdgSurfaceConfigureCb(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    xdg_surface_ack_configure(xdgSurface, serial);
    auto* self = static_cast<WaylandWindow*>(data);
    self->configured_ = true;
  }

  static void xdgToplevelConfigureCb(void* data, xdg_toplevel*, int32_t width,
                                      int32_t height, wl_array*) {
    auto* self = static_cast<WaylandWindow*>(data);
    if (width > 0 && height > 0) {
      self->pendingWidth_ = width;
      self->pendingHeight_ = height;
    }
  }

  static void xdgToplevelCloseCb(void* data, xdg_toplevel*) {
    static_cast<WaylandWindow*>(data)->shouldClose_ = true;
  }

  static void bufferReleaseCb(void*, wl_buffer*) {
    // Tek buffer + senkron memcpy kullandığımız için burada özel bir şey
    // yapmaya gerek yok; buffer sürekli yeniden kullanılıyor.
  }

  std::string title_;
  int width_, height_;
  int pendingWidth_ = 0, pendingHeight_ = 0;
  bool configured_ = false;
  bool shouldClose_ = false;

  wl_display* display_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_compositor* compositor_ = nullptr;
  wl_shm* shm_ = nullptr;
  xdg_wm_base* xdgWmBase_ = nullptr;
  wl_surface* surface_ = nullptr;
  xdg_surface* xdgSurface_ = nullptr;
  xdg_toplevel* xdgToplevel_ = nullptr;

  wl_buffer* buffer_ = nullptr;
  void* shmData_ = nullptr;
  size_t shmSize_ = 0;

  std::mutex bufferMutex_;

 public:
  int takePendingWidth() { int w = pendingWidth_; pendingWidth_ = 0; return w; }
  int takePendingHeight() { int h = pendingHeight_; pendingHeight_ = 0; return h; }
};

}  // namespace agrowebos

// =============================================================================
// CEF taraf
// =============================================================================

namespace {

agrowebos::WaylandWindow* g_window = nullptr;

// agroos-webui-latest.cc'deki AgroRenderHandler ile AYNI rol, ama hedef
// framebuffer degil g_window->presentFrame(). Kod kopyalanmadi, sadece
// aynı CefRenderHandler arayüzü yeniden (bağımsız) uygulandı.
class WebosRenderHandler : public CefRenderHandler {
 public:
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
    int w = g_window ? g_window->width() : 1280;
    int h = g_window ? g_window->height() : 800;
    rect = CefRect(0, 0, w, h);
  }

  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               const RectList& dirtyRects, const void* buffer, int width,
               int height) override {
    if (type != PET_VIEW) return;
#if defined(AGROOS_TEST_PNG_DUMP)
    // Test modu: Wayland'a hic dokunmadan, CEF'in urettigi BGRA buffer'i
    // dogrudan bir PPM dosyasina yazar. Ekran/GPU/Wayland compositor
    // gerekmez, Colab'da da calisir. Sadece ilk birkac kareyi yazip
    // sonra durur (her karede diske yazmamak icin).
    if (dump_count_ < 3) {
      dumpFrameToPpm(buffer, width, height, dump_count_);
      dump_count_++;
      if (dump_count_ == 1) {
        fprintf(stderr, "[agroos-webos][TEST] ilk kare yazildi: %dx%d\n", width, height);
      }
    }
#else
    if (g_window) {
      g_window->presentFrame(buffer, width, height);
    }
#endif
  }

 private:
#if defined(AGROOS_TEST_PNG_DUMP)
  int dump_count_ = 0;

  // BGRA32 buffer'i basit bir PPM (P6) dosyasina yazar. PPM tercih
  // edildi cunku ekstra bir PNG kutuphanesi gerektirmez — herhangi bir
  // goruntu goruntuleyici (veya `convert frame_0.ppm frame_0.png`) PPM'i
  // acabilir.
  static void dumpFrameToPpm(const void* bgra, int width, int height, int index) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/agroos-webos-frame-%02d.ppm", index);
    FILE* f = fopen(path, "wb");
    if (!f) {
      fprintf(stderr, "[agroos-webos][TEST] %s yazilamadi\n", path);
      return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    const uint8_t* px = static_cast<const uint8_t*>(bgra);
    for (int i = 0; i < width * height; ++i) {
      // BGRA -> RGB (alfa atlanir, PPM alfa desteklemez)
      uint8_t b = px[i * 4 + 0];
      uint8_t g = px[i * 4 + 1];
      uint8_t r = px[i * 4 + 2];
      fputc(r, f);
      fputc(g, f);
      fputc(b, f);
    }
    fclose(f);
    fprintf(stderr, "[agroos-webos][TEST] kare yazildi: %s\n", path);
  }
#endif
  IMPLEMENT_REFCOUNTING(WebosRenderHandler);
};

// JS <-> C++ köprüsü. Şimdilik boş bırakıldı — gerçek native fonksiyonlara
// (dosya sistemi, ayarlar, pencere kontrolü) bağlanmadan önce
// ui/filemanager/filemanager_app.h ve ui/settings/settings_app.h'daki
// GERÇEK fonksiyon imzalarına bakılacak. Sahte/varsayımsal fonksiyon
// eklenmedi.
class WebosMessageHandler : public CefMessageRouterBrowserSide::Handler {
 public:
  bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
               int64_t query_id, const CefString& request, bool persistent,
               CefRefPtr<Callback> callback) override {
    // TODO: window.webos.* çağrıları buraya, gerçek C++ fonksiyonlarına
    // eşlenerek eklenecek. Şimdilik sadece log'la ve reddet, böylece
    // React tarafındaki mock fallback devreye girer.
    fprintf(stderr, "[agroos-webos] cefQuery (henuz bagli degil): %s\n",
            request.ToString().c_str());
    callback->Failure(0, "not_implemented");
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(WebosMessageHandler);
};

class WebosClient : public CefClient,
                     public CefLifeSpanHandler {
 public:
  explicit WebosClient(CefRefPtr<CefRenderHandler> renderHandler)
      : renderHandler_(renderHandler) {
    CefMessageRouterConfig config;
    config.js_query_function = "cefQuery";
    config.js_cancel_function = "cefQueryCancel";
    messageRouter_ = CefMessageRouterBrowserSide::Create(config);
    messageRouter_->AddHandler(new WebosMessageHandler(), false);
  }

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return renderHandler_; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browser_ = browser;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    messageRouter_->OnBeforeClose(browser);
    browser_ = nullptr;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefProcessId source_process,
                                 CefRefPtr<CefProcessMessage> message) override {
    return messageRouter_->OnProcessMessageReceived(browser, frame, source_process, message);
  }

 private:
  CefRefPtr<CefRenderHandler> renderHandler_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefMessageRouterBrowserSide> messageRouter_;
  IMPLEMENT_REFCOUNTING(WebosClient);
};

class WebosApp : public CefApp, public CefBrowserProcessHandler {
 public:
  explicit WebosApp(std::string url) : url_(std::move(url)) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    CefRefPtr<WebosRenderHandler> renderHandler = new WebosRenderHandler();
    client_ = new WebosClient(renderHandler);

    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless(0);

    CefBrowserSettings browserSettings;
    browserSettings.windowless_frame_rate = 60;

    CefBrowserHost::CreateBrowser(windowInfo, client_, url_, browserSettings, nullptr, nullptr);
  }

 private:
  std::string url_;
  CefRefPtr<WebosClient> client_;
  IMPLEMENT_REFCOUNTING(WebosApp);
};

}  // namespace

int main(int argc, char* argv[]) {
  CefMainArgs mainArgs(argc, argv);

  // dist/index.html'in gercek yolu. Normal modda AgroOS donanimindaki
  // sabit yol kullanilir. Test modunda (Colab gibi farkli bir dosya
  // duzeninde) AGROOS_WEBOS_URL ortam degiskeniyle override edilebilir,
  // boylece koda dokunmadan Colab'daki webos_dist/ yoluna isaret edilebilir.
  std::string url = "file:///agroos/webos/dist/index.html";
  if (const char* override_url = getenv("AGROOS_WEBOS_URL")) {
    url = override_url;
  }

  CefRefPtr<WebosApp> app(new WebosApp(url));

  int exitCode = CefExecuteProcess(mainArgs, app.get(), nullptr);
  if (exitCode >= 0) {
    return exitCode;
  }

#if defined(AGROOS_TEST_PNG_DUMP)
  // ---- TEST MODU: Wayland/compositor'a hic ihtiyac yok ----
  // Sadece CEF'i OSR modda baslatir, birkac kare bekler (WebosRenderHandler
  // OnPaint icinde bunlari /tmp/agroos-webos-frame-NN.ppm olarak yazar),
  // sonra kapanir. Colab dahil ekran/GPU olmayan her ortamda calisir.
  fprintf(stderr, "[agroos-webos][TEST] PNG dump test modu — Wayland baglanmiyor\n");

  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true;
  settings.multi_threaded_message_loop = false;

  // CEF, resources_dir_path/locales_dir_path acikca verilmezse calisma
  // dizinine gore arar — biz /tmp'ten calistirdigimiz icin bulamiyordu
  // (icu_util.cc: "Invalid file descriptor to ICU data received").
  // AGROOS_CEF_RESOURCES_DIR ortam degiskeniyle CEF_ROOT/Resources'i
  // acikca bildiriyoruz, boylece calisma dizininden bagimsiz calisir.
  if (const char* res_dir = getenv("AGROOS_CEF_RESOURCES_DIR")) {
    CefString(&settings.resources_dir_path) = res_dir;
    std::string locales = std::string(res_dir) + "/locales";
    CefString(&settings.locales_dir_path) = locales;
  }

  CefInitialize(mainArgs, settings, app.get(), nullptr);

  // ~3 saniye boyunca mesaj dongusunu pompala — sayfa yuklenip birkac
  // kare cizilsin diye. dist/index.html gercekten var olmali (bkz. url).
  for (int i = 0; i < 3000; ++i) {
    CefDoMessageLoopWork();
    usleep(1000);
  }

  CefShutdown();
  fprintf(stderr, "[agroos-webos][TEST] bitti — /tmp/agroos-webos-frame-*.ppm dosyalarina bakin\n");
  return 0;
#else
  // ---- NORMAL MOD: gercek Wayland xdg_toplevel penceresi ----
  g_window = new agrowebos::WaylandWindow("AgroOS", 1280, 800);
  if (!g_window->initialize()) {
    fprintf(stderr, "[agroos-webos] Wayland penceresi acilamadi — "
                     "agroos-compositor calisiyor mu?\n");
    delete g_window;
    return 1;
  }

  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true;
  settings.multi_threaded_message_loop = false;

  if (const char* res_dir = getenv("AGROOS_CEF_RESOURCES_DIR")) {
    CefString(&settings.resources_dir_path) = res_dir;
    std::string locales = std::string(res_dir) + "/locales";
    CefString(&settings.locales_dir_path) = locales;
  }

  CefInitialize(mainArgs, settings, app.get(), nullptr);

  // CEF'in kendi mesaj döngüsü ile Wayland event pompasını birlikte
  // çalıştırıyoruz: multi_threaded_message_loop=false olduğu için
  // CefRunMessageLoop() bloklar — bunun yerine kendi döngümüzü kurup
  // periyodik olarak CefDoMessageLoopWork() + wl_display event pompası
  // çağırıyoruz.
  while (!g_window->shouldClose()) {
    CefDoMessageLoopWork();
    g_window->pumpEvents();

    int pw = g_window->takePendingWidth();
    int ph = g_window->takePendingHeight();
    (void)pw;
    (void)ph;
    // TODO: pw/ph configure ile geldiğinde CEF tarafına
    // browser->GetHost()->WasResized() ile bildirilecek — browser
    // referansı WebosClient'ten dışarı taşınınca eklenecek.

    usleep(1000);  // ~1000 Hz üst sınır; CEF'in kendi frame_rate'i asıl sınırlayıcı
  }

  CefShutdown();
  g_window->shutdown();
  delete g_window;

  return 0;
#endif
}
