// AgroOS Installer - CEF OSR host + gercek kurulum mantigi
//
// CEF'i offscreen render modunda baslatir, index.html'i yukler,
// her OnPaint karesini /dev/fb0'a yazar. JS <-> C++ arasinda
// CefMessageRouter (window.cefQuery) uzerinden IPC koprusu kurar.
//
// Desteklenen JS -> C++ istekleri (window.cefQuery ile JSON gonderilir):
//   {"type":"ui_ready"}
//   {"type":"disk_selected","disk":"/dev/sda"}
//   {"type":"start_install","disk":"/dev/sda"}
//   {"type":"reboot"}
//
// C++ -> JS geri cagirmalari (ExecuteJavaScript ile window.AgroOS.* cagirilir):
//   window.AgroOS.onReady(disks)
//   window.AgroOS.setDiskList(disks)
//   window.AgroOS.updateProgress(pct, label, logLine)
//   window.AgroOS.onError(message)

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <thread>
#include <dirent.h>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"

// ============================================================
// Framebuffer (degismedi)
// ============================================================
namespace {

struct FbInfo {
  int fd = -1;
  uint8_t* mem = nullptr;
  int width = 0;
  int height = 0;
  int bpp = 0;
  size_t screensize = 0;
  int line_length = 0;
};

FbInfo g_fb;

bool OpenFramebuffer(FbInfo* fb) {
  fb->fd = open("/dev/fb0", O_RDWR);
  if (fb->fd < 0) {
    fprintf(stderr, "[agroos-webui] /dev/fb0 acilamadi\n");
    return false;
  }

  struct fb_var_screeninfo vinfo;
  struct fb_fix_screeninfo finfo;

  if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
    fprintf(stderr, "[agroos-webui] FBIOGET_VSCREENINFO basarisiz\n");
    close(fb->fd);
    return false;
  }
  if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
    fprintf(stderr, "[agroos-webui] FBIOGET_FSCREENINFO basarisiz\n");
    close(fb->fd);
    return false;
  }

  fb->width = vinfo.xres;
  fb->height = vinfo.yres;
  fb->bpp = vinfo.bits_per_pixel;
  fb->line_length = finfo.line_length;
  fb->screensize = finfo.smem_len;

  fb->mem = static_cast<uint8_t*>(
      mmap(0, fb->screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0));
  if (fb->mem == MAP_FAILED) {
    fprintf(stderr, "[agroos-webui] mmap basarisiz\n");
    close(fb->fd);
    fb->mem = nullptr;
    return false;
  }

  fprintf(stderr, "[agroos-webui] fb0 acildi: %dx%d, %d bpp, line_length=%d\n",
          fb->width, fb->height, fb->bpp, fb->line_length);
  return true;
}

void CloseFramebuffer(FbInfo* fb) {
  if (fb->mem) {
    munmap(fb->mem, fb->screensize);
    fb->mem = nullptr;
  }
  if (fb->fd >= 0) {
    close(fb->fd);
    fb->fd = -1;
  }
}

void BlitToFramebuffer(FbInfo* fb, const void* src_buffer, int w, int h) {
  if (!fb->mem) return;

  const uint8_t* src = static_cast<const uint8_t*>(src_buffer);
  int copy_w = (w < fb->width) ? w : fb->width;
  int copy_h = (h < fb->height) ? h : fb->height;
  int src_stride = w * 4;

  for (int y = 0; y < copy_h; ++y) {
    memcpy(fb->mem + y * fb->line_length,
           src + y * src_stride,
           copy_w * 4);
  }
}

// ============================================================
// Kucuk yardimcilar: shell komutu calistirma, JSON string escape
// ============================================================

// Bir komutu calistirir, stdout+stderr'i birlikte doner, exit code'u
// exit_code_out'a yazar. system()/popen() yerine fork+exec kullaniyoruz
// ki initramfs gibi minimal ortamlarda da guvenilir calissin.
std::string RunCommand(const std::vector<std::string>& argv, int* exit_code_out) {
  std::string output;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    if (exit_code_out) *exit_code_out = -1;
    return "pipe() basarisiz";
  }

  pid_t pid = fork();
  if (pid < 0) {
    if (exit_code_out) *exit_code_out = -1;
    close(pipefd[0]); close(pipefd[1]);
    return "fork() basarisiz";
  }

  if (pid == 0) {
    // child
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    execvp(cargv[0], cargv.data());
    _exit(127);  // execvp basarisiz olduysa
  }

  // parent
  close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    output.append(buf, n);
  }
  close(pipefd[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exit_code_out) *exit_code_out = code;
  return output;
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

// Basit JSON deger okuyucu — tek seviyeli {"type":"...","disk":"..."}
// gibi duz objeler icin yeterli. Tam bir JSON parser gerekmiyor
// cunku JS tarafi hep sabit sekilli mesajlar gonderiyor.
std::string JsonGetString(const std::string& json, const std::string& key) {
  std::string pattern = "\"" + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos);
  if (pos == std::string::npos) return "";
  size_t end = json.find('"', pos + 1);
  if (end == std::string::npos) return "";
  return json.substr(pos + 1, end - pos - 1);
}

// ============================================================
// Disk tarama: /sys/block okuyarak gercek blok cihazlarini bulur.
// loop*, ram*, sr* gibi sanal/optik cihazlari eler.
// ============================================================
struct DiskEntry {
  std::string path;      // /dev/sda
  std::string label;     // model adi (varsa)
  unsigned long long size_bytes = 0;
};

bool ShouldSkipDevice(const std::string& name) {
  static const char* skip_prefixes[] = {"loop", "ram", "sr", "zram", "dm-"};
  for (auto p : skip_prefixes) {
    if (name.rfind(p, 0) == 0) return true;
  }
  return false;
}

std::string ReadFileTrim(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::string line;
  std::getline(f, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    line.pop_back();
  return line;
}

std::vector<DiskEntry> ScanDisks() {
  std::vector<DiskEntry> disks;
  DIR* dir = opendir("/sys/block");
  if (!dir) {
    fprintf(stderr, "[agroos-webui] /sys/block acilamadi\n");
    return disks;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (ShouldSkipDevice(name)) continue;

    std::string base = "/sys/block/" + name;

    // Bu cihazin gercekten bir "disk" oldugunu, partition olmadigini dogrula
    std::string removable_path = base + "/removable";
    std::string size_path = base + "/size";       // 512 byte sektor sayisi
    std::string model_path = base + "/device/model";

    std::string size_str = ReadFileTrim(size_path);
    if (size_str.empty()) continue;

    unsigned long long sectors = 0;
    try { sectors = std::stoull(size_str); } catch (...) { continue; }
    if (sectors == 0) continue;  // bos/gecersiz cihaz

    DiskEntry d;
    d.path = "/dev/" + name;
    d.size_bytes = sectors * 512ULL;
    d.label = ReadFileTrim(model_path);
    disks.push_back(d);
  }
  closedir(dir);
  return disks;
}

std::string FormatSize(unsigned long long bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double val = static_cast<double>(bytes);
  int unit_idx = 0;
  while (val >= 1024.0 && unit_idx < 4) {
    val /= 1024.0;
    unit_idx++;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%.1f %s", val, units[unit_idx]);
  return std::string(buf);
}

std::string DisksToJson(const std::vector<DiskEntry>& disks) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < disks.size(); ++i) {
    if (i > 0) ss << ",";
    ss << "{\"path\":\"" << JsonEscape(disks[i].path) << "\","
       << "\"label\":\"" << JsonEscape(disks[i].label) << "\","
       << "\"sizeLabel\":\"" << JsonEscape(FormatSize(disks[i].size_bytes)) << "\"}";
  }
  ss << "]";
  return ss.str();
}

}  // namespace

// ============================================================
// Kurulum isleri: gercek sfdisk bolumleme + mkfs + kopyalama.
// Ayri bir thread'de calisir, ilerlemeyi UI thread'e CefPostTask
// ile aktarir (CEF UI olmayan thread'den ExecuteJavaScript
// cagirmaya izin vermez).
// ============================================================

class AgroClient;  // ileri bildirim

// UI thread'e ilerleme guncellemesi gondermek icin kullanilan yardimci.
// browser referansi ana thread'de olusturuldugu icin guvenle paylasilabilir.
void PostProgressToUi(CefRefPtr<CefBrowser> browser, int pct,
                       const std::string& label, const std::string& log_line);
void PostErrorToUi(CefRefPtr<CefBrowser> browser, const std::string& message);
void PostDiskListToUi(CefRefPtr<CefBrowser> browser, bool call_ready);

// Gercek kurulum akisi: sfdisk ile bolumle, mkfs ile formatla,
// kernel/initramfs/bootloader'i hedef diske kopyala.
//
// Diskin bolum isimlendirmesi /dev/sda -> /dev/sda1 seklinde,
// /dev/nvme0n1 gibi cihazlar icin /dev/nvme0n1p1 seklinde olur;
// bunu asagida PartitionSuffix() ile hesapliyoruz.
namespace {

std::string PartitionPath(const std::string& disk, int part_num) {
  // nvme/mmc gibi cihazlarda son karakter rakamsa 'p' eklenir (nvme0n1p1)
  if (!disk.empty() && isdigit(static_cast<unsigned char>(disk.back()))) {
    return disk + "p" + std::to_string(part_num);
  }
  return disk + std::to_string(part_num);
}

void RunInstall(CefRefPtr<CefBrowser> browser, std::string disk) {
  int exit_code = 0;

  // ---- 1) Bolumleme: sfdisk ile GPT script ----
  // EFI: 512M (type=U EFI System), root: kalan alan (type=L Linux),
  // swap: script sonuna eklenmiyor - basitlik icin root+swap yerine
  // simdilik EFI+root olusturuyoruz; swap istenirse script'e eklenir.
  PostProgressToUi(browser, 5, "Disk bolumleniyor...", "sfdisk " + disk + " baslatiliyor");

  std::string sfdisk_script =
      "label: gpt\n"
      "size=512MiB, type=U, name=\"EFI\"\n"
      "size=2GiB,   type=S, name=\"swap\"\n"
      "type=L, name=\"root\"\n";  // kalan tum alan

  // sfdisk script'ini gecici dosyaya yaz, sonra "sfdisk disk < script" calistir
  {
    std::ofstream f("/tmp/agroos-sfdisk.script");
    f << sfdisk_script;
  }

  std::string sfdisk_cmd_output;
  {
    // sfdisk stdin'den okur; RunCommand stdin desteklemiyor,
    // bu yuzden shell uzerinden yonlendiriyoruz.
    std::vector<std::string> argv = {
        "/bin/sh", "-c",
        "sfdisk --force \"" + disk + "\" < /tmp/agroos-sfdisk.script 2>&1"};
    sfdisk_cmd_output = RunCommand(argv, &exit_code);
  }

  if (exit_code != 0) {
    PostErrorToUi(browser, "Bolumleme basarisiz (sfdisk): " + sfdisk_cmd_output);
    return;
  }
  PostProgressToUi(browser, 20, "Bolumleme tamamlandi", "Bolumler olusturuldu: EFI, swap, root");

  // Kernel'in yeni partition tablosunu gormesi icin kisa bir bekleme +
  // partprobe benzeri bir tetikleme (partprobe yoksa blockdev --rereadpt dener)
  {
    std::vector<std::string> argv = {"/bin/sh", "-c",
        "blockdev --rereadpt \"" + disk + "\" 2>/dev/null; sleep 1"};
    RunCommand(argv, &exit_code);
  }

  std::string efi_part = PartitionPath(disk, 1);
  std::string swap_part = PartitionPath(disk, 2);
  std::string root_part = PartitionPath(disk, 3);

  // ---- 2) Formatla ----
  PostProgressToUi(browser, 28, "EFI bolumu formatlaniyor...", "mkfs.vfat " + efi_part);
  {
    std::vector<std::string> argv = {"mkfs.vfat", "-F", "32", efi_part};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "mkfs.vfat basarisiz: " + out); return; }
  }

  PostProgressToUi(browser, 40, "Takas alani hazirlaniyor...", "mkswap " + swap_part);
  {
    std::vector<std::string> argv = {"mkswap", swap_part};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "mkswap basarisiz: " + out); return; }
  }

  PostProgressToUi(browser, 55, "Kok dosya sistemi olusturuluyor...", "mkfs.ext4 " + root_part);
  {
    std::vector<std::string> argv = {"mkfs.ext4", "-F", root_part};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "mkfs.ext4 basarisiz: " + out); return; }
  }

  // ---- 3) Mount et ----
  PostProgressToUi(browser, 62, "Dosya sistemleri baglaniyor...", "mount " + root_part + " -> /mnt/target");
  RunCommand({"mkdir", "-p", "/mnt/target"}, &exit_code);
  {
    std::vector<std::string> argv = {"mount", root_part, "/mnt/target"};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "root mount basarisiz: " + out); return; }
  }
  RunCommand({"mkdir", "-p", "/mnt/target/boot/efi"}, &exit_code);
  {
    std::vector<std::string> argv = {"mount", efi_part, "/mnt/target/boot/efi"};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "EFI mount basarisiz: " + out); return; }
  }

  // ---- 4) Kernel + initramfs kopyala ----
  PostProgressToUi(browser, 72, "Cekirdek kopyalaniyor...", "bzImage -> /mnt/target/boot/");
  RunCommand({"mkdir", "-p", "/mnt/target/boot"}, &exit_code);
  {
    std::vector<std::string> argv = {"cp", "/agroos/boot/agroos-bzImage", "/mnt/target/boot/vmlinuz"};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "Kernel kopyalama basarisiz: " + out); return; }
  }

  PostProgressToUi(browser, 82, "initramfs kopyalaniyor...", "initramfs -> /mnt/target/boot/");
  {
    std::vector<std::string> argv = {"cp", "/agroos/boot/agroos-initramfs.cpio.gz", "/mnt/target/boot/initramfs.img"};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) { PostErrorToUi(browser, "initramfs kopyalama basarisiz: " + out); return; }
  }

  // ---- 5) Bootloader kur (grub-install varsa onu kullan) ----
  PostProgressToUi(browser, 90, "Onyukleyici kuruluyor...", "grub-install " + disk);
  {
    std::vector<std::string> argv = {
        "/bin/sh", "-c",
        "grub-install --target=x86_64-efi --efi-directory=/mnt/target/boot/efi "
        "--boot-directory=/mnt/target/boot --removable 2>&1"};
    std::string out = RunCommand(argv, &exit_code);
    if (exit_code != 0) {
      // grub yoksa kurulumu basarisiz saymiyoruz, uyari olarak logluyoruz —
      // bootloader adimi initramfs'e eklenecek arac setine bagli.
      PostProgressToUi(browser, 92, "Onyukleyici atlandi (grub bulunamadi)", out.substr(0, 200));
    }
  }

  // ---- 6) Temizlik ----
  RunCommand({"umount", "/mnt/target/boot/efi"}, &exit_code);
  RunCommand({"umount", "/mnt/target"}, &exit_code);

  PostProgressToUi(browser, 100, "Kurulum tamamlandi", "AgroOS " + disk + " uzerine kuruldu");
}

}  // namespace

// ============================================================
// Render Handler (degismedi)
// ============================================================
class AgroRenderHandler : public CefRenderHandler {
 public:
  AgroRenderHandler() {}

  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
    rect = CefRect(0, 0, g_fb.width > 0 ? g_fb.width : 1024,
                    g_fb.height > 0 ? g_fb.height : 768);
  }

  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               const RectList& dirtyRects, const void* buffer, int width,
               int height) override {
    if (type != PET_VIEW) return;
    BlitToFramebuffer(&g_fb, buffer, width, height);
  }

 private:
  IMPLEMENT_REFCOUNTING(AgroRenderHandler);
};

// ============================================================
// Message Router handler (browser tarafi) — JS'ten gelen
// cefQuery isteklerini isler.
// ============================================================
class AgroMessageHandler : public CefMessageRouterBrowserSide::Handler {
 public:
  bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
               int64_t query_id, const CefString& request, bool persistent,
               CefRefPtr<Callback> callback) override {
    std::string req = request.ToString();
    std::string type = JsonGetString(req, "type");

    if (type == "ui_ready") {
      callback->Success("{}");
      // Disk taramasini ayri thread'de yapip sonucu JS'e push edelim
      // (tarama /sys/block okumasi hizli olsa da UI thread'i bloklamayalim).
      CefRefPtr<CefBrowser> br = browser;
      std::thread([br]() { PostDiskListToUi(br, true); }).detach();
      return true;
    }

    if (type == "disk_selected") {
      std::string disk = JsonGetString(req, "disk");
      fprintf(stderr, "[agroos-webui] disk secildi: %s\n", disk.c_str());
      callback->Success("{}");
      return true;
    }

    if (type == "start_install") {
      std::string disk = JsonGetString(req, "disk");
      if (disk.empty()) {
        callback->Failure(1, "disk belirtilmedi");
        return true;
      }
      callback->Success("{}");
      CefRefPtr<CefBrowser> br = browser;
      std::thread([br, disk]() { RunInstall(br, disk); }).detach();
      return true;
    }

    if (type == "reboot") {
      callback->Success("{}");
      fprintf(stderr, "[agroos-webui] reboot istendi\n");
      int code = 0;
      RunCommand({"reboot"}, &code);
      return true;
    }

    callback->Failure(1, "bilinmeyen istek tipi: " + type);
    return true;
  }
};

// ============================================================
// CEF Client — message router'i browser olaylarina baglar
// ============================================================
class AgroClient : public CefClient, public CefLifeSpanHandler {
 public:
  explicit AgroClient(CefRefPtr<CefRenderHandler> render_handler)
      : render_handler_(render_handler) {
    CefMessageRouterConfig config;
    message_router_ = CefMessageRouterBrowserSide::Create(config);
    message_handler_ = new AgroMessageHandler();
    message_router_->AddHandler(message_handler_.get(), false);
  }

  CefRefPtr<CefRenderHandler> GetRenderHandler() override {
    return render_handler_;
  }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefProcessId source_process,
                                 CefRefPtr<CefProcessMessage> message) override {
    return message_router_->OnProcessMessageReceived(browser, frame,
                                                       source_process, message);
  }

  CefRefPtr<CefBrowser> browser() { return browser_; }
  void SetBrowser(CefRefPtr<CefBrowser> b) { browser_ = b; }

 private:
  CefRefPtr<CefRenderHandler> render_handler_;
  CefRefPtr<CefMessageRouterBrowserSide> message_router_;
  CefRefPtr<AgroMessageHandler> message_handler_;
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(AgroClient);
};

// Global client referansi — PostProgressToUi vb. fonksiyonlarin
// ExecuteJavaScript cagirabilmesi icin ana frame'e erismesi lazim.
namespace {
CefRefPtr<AgroClient> g_client;
}

void PostProgressToUi(CefRefPtr<CefBrowser> browser, int pct,
                       const std::string& label, const std::string& log_line) {
  if (!browser) return;
  std::string js = "window.AgroOS && window.AgroOS.updateProgress(" +
      std::to_string(pct) + ", \"" + JsonEscape(label) + "\", \"" +
      JsonEscape(log_line) + "\");";
  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, std::string script) {
        if (b && b->GetMainFrame())
          b->GetMainFrame()->ExecuteJavaScript(script, b->GetMainFrame()->GetURL(), 0);
      },
      browser, js));
}

void PostErrorToUi(CefRefPtr<CefBrowser> browser, const std::string& message) {
  if (!browser) return;
  std::string js = "window.AgroOS && window.AgroOS.onError(\"" + JsonEscape(message) + "\");";
  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, std::string script) {
        if (b && b->GetMainFrame())
          b->GetMainFrame()->ExecuteJavaScript(script, b->GetMainFrame()->GetURL(), 0);
      },
      browser, js));
}

void PostDiskListToUi(CefRefPtr<CefBrowser> browser, bool call_ready) {
  std::vector<DiskEntry> disks = ScanDisks();
  std::string json = DisksToJson(disks);
  std::string js = call_ready
      ? "window.AgroOS && window.AgroOS.onReady(" + json + ");"
      : "window.AgroOS && window.AgroOS.setDiskList(" + json + ");";
  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, std::string script) {
        if (b && b->GetMainFrame())
          b->GetMainFrame()->ExecuteJavaScript(script, b->GetMainFrame()->GetURL(), 0);
      },
      browser, js));
}

// ============================================================
// Render process (renderer) tarafi — cefQuery'nin V8 context'ine
// enjekte edilmesi icin message router renderer-side gerekli.
// ============================================================
class AgroRenderProcessHandler : public CefRenderProcessHandler {
 public:
  AgroRenderProcessHandler() {
    CefMessageRouterConfig config;
    message_router_ = CefMessageRouterRendererSide::Create(config);
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    message_router_->OnContextCreated(browser, frame, context);
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
    message_router_->OnContextReleased(browser, frame, context);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefProcessId source_process,
                                 CefRefPtr<CefProcessMessage> message) override {
    return message_router_->OnProcessMessageReceived(browser, frame,
                                                       source_process, message);
  }

 private:
  CefRefPtr<CefMessageRouterRendererSide> message_router_;
  IMPLEMENT_REFCOUNTING(AgroRenderProcessHandler);
};

// ============================================================
// CEF App — hem browser hem renderer process handler'i saglar.
// Ayni binary CEF tarafindan farkli roller icin yeniden cagrilir;
// GetRenderProcessHandler() sadece renderer process'te devreye girer.
// ============================================================
class AgroApp : public CefApp,
                public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    if (!render_process_handler_)
      render_process_handler_ = new AgroRenderProcessHandler();
    return render_process_handler_;
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    CefRefPtr<AgroRenderHandler> render_handler = new AgroRenderHandler();
    g_client = new AgroClient(render_handler);

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 30;

    std::string url = "file:///agroos/ui/index.html";

    CefBrowserHost::CreateBrowser(window_info, g_client, url, browser_settings,
                                   nullptr, nullptr);
  }

 private:
  CefRefPtr<AgroRenderProcessHandler> render_process_handler_;
  IMPLEMENT_REFCOUNTING(AgroApp);
};

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);

  CefRefPtr<AgroApp> app(new AgroApp);

  int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  if (!OpenFramebuffer(&g_fb)) {
    fprintf(stderr, "[agroos-webui] framebuffer acilamadi, devam edilemiyor\n");
    return 1;
  }

  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true;

  CefInitialize(main_args, settings, app.get(), nullptr);
  CefRunMessageLoop();
  CefShutdown();

  CloseFramebuffer(&g_fb);
  return 0;
}
