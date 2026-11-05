// PureOCR：无参数启动 GUI；有参数走 CLI（--pdf / --image ...）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include "resource.h"
#else
#include <unistd.h>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif
#include "stb_image_write.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/path_util.h"
#include "util/utf8_io.h"
#include "util/platform.h"
#include "util/http_download.h"
#include "util/model_download.h"
#include "util/model_registry.h"
#include "cli.h"
#ifdef PUREOCR_HAS_MUPDF
#include "pdf/doc.h"
#include "pdf/lines_io.h"
#include "pdf/ocr_pages.h"
#include "pdf/preview.h"
#endif

namespace {

std::string g_pdfPath;
std::string g_outPath;
std::string g_modelDir;
std::string g_pageSpec;

// OCR 运行状态（后台线程写，UI 线程读）
std::mutex g_mutex;
std::vector<std::string> g_log;
std::atomic<bool> g_running{false};
std::atomic<bool> g_done{false};
std::atomic<int> g_donePages{0};
std::atomic<int> g_totalPages{0};
std::string g_result;   // 完成后的结果摘要
std::thread g_worker;
float g_minScore = 0.55f;
float g_reviewScore = 0.75f;
double g_lastDpi = 300.0;
int g_threads = 2;
int g_dpi = 300;
bool g_switchToPreview = false;
#ifdef PUREOCR_SELFTEST
bool g_selfTest = false;
std::string g_shotPath;   // --shot <png>：渲染几帧后抓图（调试布局用）
int g_shotFrame = -1;
int g_shotW = 0;          // --shot <png> [w] [h]：可选，强制窗口尺寸便于按真实分辨率检查布局
int g_shotH = 0;
unsigned int g_shotFbo = 0;   // 离屏渲染，用于按真实分辨率检查布局
unsigned int g_shotTex = 0;
#endif
bool g_resume = false;  // 断点续跑
int g_presetIdx = -1;  // -1 = 未初始化，默认选「自动探测」
bool g_mergeRow = true;   // 合并同一行的碎片
bool g_maskOriginal = false;  // 用 OCR 文本替换原文（屏蔽原文字体）；默认关
bool g_darkTheme = true;  // 深色主题
std::string g_fixCsv;     // 复核 CSV（回灌用）
bool g_batchMode = false; // 输入是目录：批量处理该目录下的 PDF
bool g_recurse = false;   // 批量模式：含子目录
bool g_overwrite = false; // 批量模式：覆盖已存在的 <原名>_OCR.pdf

// ---- 模型在线下载（GUI）----
util::DownloadHandle g_dl;
std::thread g_dlThread;
std::atomic<bool> g_dlActive{false};
std::string g_dlError;    // 最近一次失败原因（受 g_mutex 保护）
int g_modelStamp = 0;     // 模型文件变化计数：用来刷新下拉里的「缺模型」标记
int g_dlSourceIdx = 0;    // 0=自动 1=官方 2=镜像（hf-mirror）
bool g_openDlPopup = false;  // 请求打开「缺少模型」弹窗

int DefaultPresetIndex() {
    int n = 0;
    const ocr::ModelPreset *list = ocr::ModelPresetList(&n);
    for (int i = 0; i < n; ++i) {
        if (std::string(list[i].id) == "auto") return i;
    }
    return 0;
}  // 模型预置（索引见 ocr::ModelPresetList）

const ocr::ModelPreset *CurrentPreset() {
    int n = 0;
    const ocr::ModelPreset *list = ocr::ModelPresetList(&n);
    if (g_presetIdx < 0) g_presetIdx = DefaultPresetIndex();
    if (g_presetIdx < 0 || g_presetIdx >= n) return list;
    return &list[g_presetIdx];
}
std::chrono::steady_clock::time_point g_startTime;

#ifdef PUREOCR_HAS_MUPDF
pdf::PagePreview g_preview;
int g_previewPage = 0;
float g_previewDpi = 120.0f;
bool g_showBoxes = true;
bool g_onlyLowConf = false;
std::vector<pdf::OcrPageLines> g_pageLines;
std::string g_texPath;
std::string g_texError;
int g_texPage = -1;
float g_texDpi = 0.0f;
unsigned g_tex = 0;
int g_texW = 0;
int g_texH = 0;
#endif

std::string ExeDir() { return util::ExeDir(); }

void PushLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_log.push_back(line);
    if (g_log.size() > 500) g_log.erase(g_log.begin(), g_log.begin() + 100);
}

std::string DefaultOutputPath(const std::string& input) {
    return util::InsertBeforeExtension(input, "_OCR");
}

bool IsDirPath(const std::string& path) {
    return util::IsDirectory(path);
}

/// 目录里待处理的 PDF（跳过 *_OCR.pdf 产物）；recurse=true 时含子目录。
std::vector<std::string> CollectPdfs(const std::string& folder, bool recurse) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::u8path(folder);
    auto take = [&](const std::filesystem::path& p) {
        if (!std::filesystem::is_regular_file(p, ec)) return;
        std::string ext = p.extension().u8string();
        for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (ext != ".pdf") return;
        std::string stem = p.stem().u8string();
        std::string tail = stem.size() >= 4 ? stem.substr(stem.size() - 4) : std::string();
        for (char& c : tail) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (tail == "_ocr") return;  // 自己就是产物
        out.push_back(p.u8string());
    };
    if (recurse) {
        for (std::filesystem::recursive_directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            take(it->path());
        }
    } else {
        for (std::filesystem::directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            take(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 侧栏显示用的计数：按 (目录, 是否递归) 记一份，避免每帧遍历目录
int BatchPdfCount(const std::string& folder, bool recurse) {
    static std::string key;
    static int cached = -1;
    const std::string k = folder + (recurse ? "#r" : "#f");
    if (k != key) {
        key = k;
        cached = static_cast<int>(CollectPdfs(folder, recurse).size());
    }
    return cached < 0 ? 0 : cached;
}

/// 选择包含 PDF 的文件夹（批量模式）
void OpenFolderDialog() {
#ifdef _WIN32
    BROWSEINFOW bi = {};
    bi.hwndOwner = GetActiveWindow();
    bi.lpszTitle = L"选择包含 PDF 的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = L"";
    if (SHGetPathFromIDListW(pidl, path)) {
        g_pdfPath = util::WideToUtf8(path);
        g_outPath.clear();  // 批量模式输出跟着每个源文件走
        g_batchMode = true;
        PushLog("选择文件夹：" + g_pdfPath);
        const int n = BatchPdfCount(g_pdfPath, g_recurse);
        PushLog("待处理 PDF：" + std::to_string(n) + " 个（已跳过 *_OCR.pdf）");
    }
    CoTaskMemFree(pidl);
#else
    // Linux：没有原生对话框后端，批量模式走「拖入文件夹」或命令行 --dir
    PushLog("Linux 版请把文件夹拖进窗口，或用命令行 --dir 指定目录");
#endif
}

bool FileExistsUtf8(const std::string& path) {
    return util::PathExists(path);
}

/// OCR 文本层要嵌入的字体：优先黑体，再退化到楷体 / 雅黑 / 宋体。
/// 以前这里写死 simhei.ttf，没装该字体的机器会直接"文本层初始化失败"。
std::string OcrFontPath() {
#ifdef _WIN32
    static const char* kCandidates[] = {"C:\\Windows\\Fonts\\simhei.ttf",
                                        "C:\\Windows\\Fonts\\simkai.ttf",
                                        "C:\\Windows\\Fonts\\msyh.ttc",
                                        "C:\\Windows\\Fonts\\simsun.ttc"};
#else
    static const char* kCandidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"};
#endif
    for (const char* c : kCandidates) {
        if (FileExistsUtf8(c)) return c;
    }
    return kCandidates[0];
}

/// Windows: %LOCALAPPDATA%\\pureocr\\settings.ini；Linux: $XDG_CONFIG_HOME/pureocr/settings.ini
std::string SettingsPath() {
    std::string dir = util::GetEnv("LOCALAPPDATA");
    if (dir.empty()) dir = util::GetEnv("XDG_CONFIG_HOME");
    if (dir.empty()) dir = util::GetEnv("HOME");
    if (dir.empty()) dir = ExeDir();
    const std::string cfgDir = dir + "/pureocr";
    util::MakeDirs(cfgDir);
    return cfgDir + "/settings.ini";
}

void LoadSettings() {
    const std::string path = SettingsPath();
    FILE* f = util::OpenUtf8(path, L"rb");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t rd = 0;
    while ((rd = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, rd);
    std::fclose(f);
    if (all.size() >= 3 && static_cast<unsigned char>(all[0]) == 0xEF) all.erase(0, 3);
    std::map<std::string, std::string> kv;
    size_t start = 0;
    while (start <= all.size()) {
        const size_t nl = all.find('\n', start);
        std::string line = all.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            kv[k] = line.substr(eq + 1);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    auto str = [&](const char* k) {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };
    auto num = [&](const char* k) { return std::atof(str(k).c_str()); };

    const std::string in = str("input");
    if (!in.empty() && FileExistsUtf8(in)) {
        g_pdfPath = in;
        if (IsDirPath(in)) {
            g_outPath.clear();  // 目录输入：输出跟着每个源文件走
        } else {
            const std::string out = str("output");
            g_outPath = (!out.empty() && FileExistsUtf8(out)) ? out : DefaultOutputPath(in);
        }
    }
    const std::string md = str("model_dir");
    if (!md.empty() && FileExistsUtf8(md)) g_modelDir = md;
    if (num("dpi") > 0) g_dpi = static_cast<int>(num("dpi"));
    if (num("threads") > 0) g_threads = static_cast<int>(num("threads"));
    if (num("min_score") > 0) g_minScore = static_cast<float>(num("min_score"));
    if (num("review_score") > 0) g_reviewScore = static_cast<float>(num("review_score"));
    if (kv.count("merge_row")) g_mergeRow = str("merge_row") != "0";
    if (kv.count("mask")) g_maskOriginal = str("mask") != "0";
    if (kv.count("recurse")) g_recurse = str("recurse") != "0";
    if (kv.count("overwrite")) g_overwrite = str("overwrite") != "0";
    if (kv.count("dark")) g_darkTheme = str("dark") != "0";
    if (kv.count("download_source")) g_dlSourceIdx = std::atoi(str("download_source").c_str());
    if (kv.count("pages")) g_pageSpec = str("pages");
    const std::string pid = str("preset");
    if (!pid.empty()) {
        int n = 0;
        const ocr::ModelPreset* list = ocr::ModelPresetList(&n);
        for (int i = 0; i < n; ++i) {
            if (pid == list[i].id) g_presetIdx = i;
        }
    }
}

void SaveSettings() {
    const std::string path = SettingsPath();
    FILE* f = util::OpenUtf8(path, L"wb");
    if (!f) return;
    const ocr::ModelPreset* mp = CurrentPreset();
    std::fprintf(f, "# PureOCR 设置（自动生成，可删）\n");
    std::fprintf(f, "input=%s\n", g_pdfPath.c_str());
    std::fprintf(f, "output=%s\n", g_outPath.c_str());
    std::fprintf(f, "model_dir=%s\n", g_modelDir.c_str());
    std::fprintf(f, "dpi=%d\n", g_dpi);
    std::fprintf(f, "threads=%d\n", g_threads);
    std::fprintf(f, "min_score=%.2f\n", static_cast<double>(g_minScore));
    std::fprintf(f, "review_score=%.2f\n", static_cast<double>(g_reviewScore));
    std::fprintf(f, "preset=%s\n", mp ? mp->id : "auto");
    std::fprintf(f, "merge_row=%d\n", g_mergeRow ? 1 : 0);
    std::fprintf(f, "mask=%d\n", g_maskOriginal ? 1 : 0);
    std::fprintf(f, "recurse=%d\n", g_recurse ? 1 : 0);
    std::fprintf(f, "overwrite=%d\n", g_overwrite ? 1 : 0);
    std::fprintf(f, "dark=%d\n", g_darkTheme ? 1 : 0);
    std::fprintf(f, "download_source=%d\n", g_dlSourceIdx);
    std::fprintf(f, "pages=%s\n", g_pageSpec.c_str());
    std::fclose(f);
}

// 界面缩放：窗口是物理像素渲染时按显示器缩放放大（150% 屏按 1.5 倍）
float g_uiScale = 1.0f;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontBrand = nullptr;

ImVec4 AccentColor() {
    return g_darkTheme ? ImVec4(0.36f, 0.58f, 1.00f, 1.0f) : ImVec4(0.15f, 0.41f, 0.92f, 1.0f);
}
ImVec4 AccentHover() {
    return g_darkTheme ? ImVec4(0.46f, 0.66f, 1.00f, 1.0f) : ImVec4(0.22f, 0.50f, 1.00f, 1.0f);
}
ImVec4 AccentActive() {
    return g_darkTheme ? ImVec4(0.28f, 0.47f, 0.90f, 1.0f) : ImVec4(0.11f, 0.32f, 0.76f, 1.0f);
}
ImVec4 MutedColor() {
    return g_darkTheme ? ImVec4(0.56f, 0.60f, 0.69f, 1.0f) : ImVec4(0.40f, 0.44f, 0.52f, 1.0f);
}

void ApplyTheme() {
    const float s = g_uiScale;
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;

    if (g_darkTheme) {
        ImGui::StyleColorsDark();
        c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.59f, 0.68f, 1.00f);
        c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.16f, 1.00f);
        c[ImGuiCol_Border] = ImVec4(0.19f, 0.22f, 0.28f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.21f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.22f, 0.29f, 1.00f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.27f, 0.35f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.17f, 0.20f, 0.27f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.29f, 0.40f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.36f, 0.52f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.18f, 0.23f, 0.32f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.29f, 0.42f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.35f, 0.52f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.58f, 1.00f, 1.00f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.46f, 0.66f, 1.00f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.42f, 0.62f, 1.00f, 1.00f);
        c[ImGuiCol_Separator] = ImVec4(0.19f, 0.22f, 0.28f, 1.00f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.36f, 0.58f, 1.00f, 1.00f);
    } else {
        ImGui::StyleColorsLight();
        c[ImGuiCol_Text] = ImVec4(0.12f, 0.14f, 0.19f, 1.00f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.46f, 0.54f, 1.00f);
        c[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        c[ImGuiCol_Border] = ImVec4(0.84f, 0.87f, 0.92f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(0.96f, 0.97f, 0.99f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.93f, 0.95f, 0.99f, 1.00f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.93f, 0.99f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.94f, 0.95f, 0.98f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.88f, 0.91f, 0.98f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.82f, 0.87f, 0.97f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.90f, 0.93f, 0.99f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.86f, 0.90f, 0.98f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.86f, 0.97f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.48f, 0.95f, 1.00f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.15f, 0.41f, 0.92f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.15f, 0.41f, 0.92f, 1.00f);
        c[ImGuiCol_Separator] = ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.93f, 0.95f, 0.97f, 1.00f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.79f, 0.86f, 1.00f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.15f, 0.41f, 0.92f, 1.00f);
    }

    st.WindowPadding = ImVec2(16.0f * s, 14.0f * s);
    st.FramePadding = ImVec2(11.0f * s, 7.0f * s);
    st.ItemSpacing = ImVec2(10.0f * s, 7.0f * s);
    st.ItemInnerSpacing = ImVec2(8.0f * s, 6.0f * s);
    st.CellPadding = ImVec2(8.0f * s, 4.0f * s);
    st.FrameRounding = 6.0f * s;
    st.GrabRounding = 5.0f * s;
    st.PopupRounding = 6.0f * s;
    st.ChildRounding = 10.0f * s;
    st.WindowRounding = 12.0f * s;
    st.ScrollbarSize = 12.0f * s;
    st.ScrollbarRounding = 6.0f * s;
    st.TabRounding = 6.0f * s;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;   // 浅色下输入框需要描边才看得出边界
    st.PopupBorderSize = 1.0f;
}

/// 把拖进来的 PDF 当作输入
void DropCallback(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) {
        std::string path(paths[i] ? paths[i] : "");
        if (path.empty()) continue;
        if (IsDirPath(path)) {  // 拖文件夹 = 批量模式
            g_pdfPath = path;
            g_outPath.clear();
            PushLog("拖入文件夹：" + path + "（批量模式）");
            return;
        }
        if (path.size() < 5) continue;
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (ext != ".pdf") continue;
        g_pdfPath = path;
        g_outPath = DefaultOutputPath(path);
        PushLog("拖入 PDF：" + path);
        return;
    }
}

/// 弹出一个系统文件选择框；Linux 用 zenity/kdialog，取不到就返回空串。
std::string PickPathDialog(const char* mode, const char* filterName, const char* pattern,
                           const std::string& initial) {
#ifdef _WIN32
    wchar_t file[1024] = L"";
    if (!initial.empty()) {
        const std::wstring w = util::Utf8ToWide(initial);
        wcsncpy_s(file, w.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    std::wstring wfilter = util::Utf8ToWide(std::string(filterName) + " (" + pattern + ")");
    wfilter.push_back(L'\0');
    wfilter += util::Utf8ToWide(pattern);
    wfilter.push_back(L'\0');
    wfilter.push_back(L'\0');
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    std::wstring wdef = util::Utf8ToWide(pattern);
    wdef.erase(0, std::min<size_t>(2, wdef.size()));  // "*.pdf" -> "pdf"
    ofn.lpstrDefExt = wdef.c_str();
    ofn.Flags = (std::string(mode) == "save") ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                                             : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    const BOOL ok = (std::string(mode) == "save") ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return std::string();
    return util::WideToUtf8(file);
#else
    std::string cmd;
    const std::string quoted = std::string("\"") + pattern + "\"";
    if (std::string(mode) == "save") {
        cmd = "zenity --file-selection --save --confirm-overwrite --file-filter=" + quoted;
        if (!initial.empty()) cmd += " --filename=\"" + initial + "\"";
    } else {
        cmd = "zenity --file-selection --file-filter=" + quoted;
        if (!initial.empty()) cmd += " --filename=\"" + initial + "\"";
    }
    cmd += " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::string();
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

/// 选择回灌用的复核 CSV
void PickFixCsvDialog() {
    const std::string picked = PickPathDialog("open", "CSV 文件", "*.csv", g_fixCsv);
    if (picked.empty()) return;
    g_fixCsv = picked;
    PushLog("已选择复核 CSV：" + g_fixCsv);
    PushLog("点「开始识别」应用修改（同参数下会命中缓存，很快）");
}

void OpenPdfDialog() {
    const std::string picked = PickPathDialog("open", "PDF 文件", "*.pdf", "");
    if (picked.empty()) return;
    g_pdfPath = picked;
    g_outPath = DefaultOutputPath(g_pdfPath);
}

void SavePdfDialog() {
    const std::string picked = PickPathDialog("save", "PDF 文件", "*.pdf", g_outPath);
    if (!picked.empty()) g_outPath = picked;
}

/// 用系统默认程序打开文件 / 文件管理器（跨平台）。
void OpenInFileManager(const std::string& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", util::Utf8ToWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

std::string ReviewCsvPath() {
    std::string base = g_outPath.empty() ? DefaultOutputPath(g_pdfPath) : g_outPath;
    const size_t dot = base.find_last_of('.');
    const size_t slash = base.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        base = base.substr(0, dot);
    }
    return base + "_复核.csv";
}

void ExportReviewCsv() {
    if (g_pageLines.empty()) {
        PushLog("没有可导出的识别结果（先跑一次识别）");
        return;
    }
    std::string path = ReviewCsvPath();
    const std::string picked = PickPathDialog("save", "CSV 文件", "*.csv", path);
    if (picked.empty()) return;
    path = picked;
    std::string err;
    if (!pdf::WriteLinesCsv(path, g_pageLines, g_lastDpi, g_reviewScore, &err)) {
        PushLog("导出 CSV 失败：" + err);
        return;
    }
    int total = 0;
    for (const auto& pg : g_pageLines) total += static_cast<int>(pg.lines.size());
    char buf[320];
    std::snprintf(buf, sizeof(buf), "已导出复核 CSV：%d 页 / %d 行（低于 %.2f 标为待复核）-> %s",
                  static_cast<int>(g_pageLines.size()), total,
                  static_cast<double>(g_reviewScore), path.c_str());
    PushLog(buf);
}

void OpenOutputFolder() {
    if (g_outPath.empty()) return;
    const std::string dir = util::ParentDir(g_outPath);
    if (dir.empty() || !std::filesystem::is_directory(std::filesystem::u8path(dir))) return;
    OpenInFileManager(dir);
}

// ---- 模型清单 / 缺件判断 / 下载（GUI 用）----

// registry.ini 只在该文件或模型目录变化时重新读，避免每帧 I/O。
const std::vector<util::ModelFile>& ModelRegistryCached() {
    static std::vector<util::ModelFile> cache;
    static std::string key;
    const std::string k = g_modelDir + "#" + std::to_string(g_modelStamp);
    if (k != key) {
        cache = util::LoadModelRegistry(g_modelDir);
        key = k;
    }
    return cache;
}

// 当前预设缺哪些模型（det/rec/cls 里不存在的）
std::vector<util::ModelFile> MissingModels() {
    std::vector<util::ModelFile> out;
    const ocr::ModelPreset* mp = CurrentPreset();
    if (!mp || g_modelDir.empty()) return out;
    const std::vector<util::ModelFile>& reg = ModelRegistryCached();
    for (const char* f : {mp->det, mp->rec, mp->cls}) {
        if (!f || !*f) continue;
        if (util::FileReady(util::JoinPath(g_modelDir, f))) continue;
        if (const util::ModelFile* info = util::FindModelFile(reg, f)) {
            out.push_back(*info);
        } else {
            util::ModelFile m;
            m.file = f;
            out.push_back(m);
        }
    }
    return out;
}

void StartModelDownload() {
    if (g_dlActive.load()) return;
    const std::vector<util::ModelFile> missing = MissingModels();
    if (missing.empty()) return;
    if (g_dlThread.joinable()) g_dlThread.join();  // 上一轮已经 join 过，这里只是保险

    const std::string dir = g_modelDir;
    const bool preferMirror = g_dlSourceIdx == 2;
    g_dl.received = 0;
    g_dl.total = 0;
    g_dl.cancel = false;
    g_dl.finished = false;
    g_dl.success = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_dlError.clear();
    }

    std::int64_t total = 0;
    for (const auto& m : missing) total += m.size;
    PushLog("开始下载模型：" + std::to_string(missing.size()) + " 个文件 / " + util::FormatBytes(total));

    g_dlActive = true;
    g_dlThread = std::thread([dir, missing, preferMirror]() {
        std::string err;
        const bool ok = util::FetchModels(dir, missing, preferMirror, &g_dl, &err);
        g_dl.success = ok;
        g_dl.finished = true;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_dlError = ok ? std::string() : err;
    });
}

void StartOcr() {
#ifdef PUREOCR_HAS_MUPDF
    if (g_running) return;
    {
        const std::vector<util::ModelFile> need = MissingModels();
        if (!need.empty()) {
            PushLog("模型不完整：还缺 " + std::to_string(need.size()) + " 个文件，请先点「下载缺失模型」");
            return;
        }
    }
    g_running = true;
    g_done = false;
    g_donePages = 0;
    g_totalPages = 0;
    g_startTime = std::chrono::steady_clock::now();
#ifdef PUREOCR_HAS_MUPDF
    g_pageLines.clear();
#endif
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_log.clear();
        g_result.clear();
    }
    // 上一轮的线程对象必须先 join 掉：给 joinable 的 std::thread 赋值会直接
    // std::terminate（症状就是「第一次跑完，再点开始就闪退」）
    if (g_worker.joinable()) g_worker.join();
#ifdef PUREOCR_HAS_MUPDF
    g_preview.Close();  // 释放上一轮对输出 PDF 的只读句柄，避免写同名文件被占用
    g_texPath.clear();
    g_texPage = -1;
#endif

    const std::string pdf = g_pdfPath;
    const bool batch = IsDirPath(pdf);
    g_batchMode = batch;
    std::vector<std::string> batchFiles;
    if (batch) {
        batchFiles = CollectPdfs(pdf, g_recurse);
        if (batchFiles.empty()) {
            PushLog("目录里没有可处理的 PDF（已跳过 *_OCR.pdf）：" + pdf);
            g_running = false;
            return;
        }
        PushLog("批量模式：" + pdf + "，共 " + std::to_string(batchFiles.size()) + " 个 PDF" +
                (g_recurse ? "（含子目录）" : "") +
                "；输出为每个源文件同目录的 <原名>_OCR.pdf");
    }
    const std::string out = batch ? std::string()
                                  : (g_outPath.empty() ? DefaultOutputPath(pdf) : g_outPath);
    std::vector<int> pages;
    if (!g_pageSpec.empty() && !pdf::ParsePageSpec(g_pageSpec, &pages)) {
        PushLog("页范围格式无效：" + g_pageSpec + "（例：1-10,15）");
        g_running = false;
        return;
    }
    const std::string pageSpecNote = g_pageSpec.empty() ? std::string("全部页") : g_pageSpec;
    const std::string modelDir = g_modelDir;
    const int threads = g_threads;
    const int dpi = g_dpi;
    const float minScore = g_minScore;
    g_lastDpi = static_cast<double>(dpi);
    SaveSettings();  // 每次开跑就落盘，异常退出也不丢参数

    g_worker = std::thread([=]() {
        // 单个文件跑一遍；summaryOut 带出结果摘要（失败时也写）
        auto runOne = [&](const std::string &inFile, const std::string &outFile,
                          std::string *summaryOut) -> bool {
            pdf::OcrRunOptions opt;
            opt.inputPath = inFile;
            opt.outputPath = outFile;
            opt.modelDir = modelDir;
            opt.fontPath = OcrFontPath();
            opt.dpi = dpi;
            opt.threads = threads;
            opt.minScore = minScore;
            opt.maskOriginal = g_maskOriginal;
            opt.resume = g_resume || !g_fixCsv.empty();
            const ocr::ModelPreset *mp = CurrentPreset();
            opt.detModel = mp->det;
            opt.recModel = mp->rec;
            opt.clsModel = mp->cls;
            opt.mergeGapRatio = g_mergeRow ? 0.8f : 0.0f;
            opt.pages = pages;
            opt.fixCsvPath = g_fixCsv;

            pdf::OcrRunStats stats;
            std::string err;
            PushLog("开始识别：" + inFile + "（" + pageSpecNote + "）");
            PushLog("输出：" + outFile);
            const bool ok = pdf::RunPdfOcr(
                opt,
                [](int done, int total, int page, int lines) {
                    if (!g_batchMode) {  // 批量模式的进度按「第几个文件」算
                        g_totalPages = total;
                        g_donePages = done;
                    }
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "[%d/%d] 第 %d 页：%d 行", done, total, page + 1,
                                  lines);
                    PushLog(buf);
                },
                &stats, &err);
#ifdef PUREOCR_HAS_MUPDF
            if (ok && !g_batchMode) {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_pageLines = std::move(stats.pageLines);
            }
#endif
            if (!ok) {
                PushLog("失败：" + err);
                if (summaryOut) *summaryOut = "失败：" + err;
                return false;
            }
            char buf[256];
            if (stats.maskedFonts > 0) {
                std::snprintf(buf, sizeof(buf),
                              "完成：%d 页 / %d 行，替换原文（屏蔽字体 %d 个），用时 %.1f 秒",
                              stats.pageCount, stats.lineCount, stats.maskedFonts, stats.seconds);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "完成：%d 页 / %d 行（原文保留，仅叠加隐形文字层），用时 %.1f 秒",
                              stats.pageCount, stats.lineCount, stats.seconds);
            }
            PushLog(buf);
            if (stats.stackedPages > 0) {
                char sbuf[256];
                std::snprintf(sbuf, sizeof(sbuf),
                              "%d 页原文本来就能复制，成品里原文与 OCR 文本并存（复制会看到两份）；"
                              "想只保留 OCR 文本请勾选「用 OCR 文本替换原文」",
                              stats.stackedPages);
                PushLog(sbuf);
            }
            if (stats.maskSkippedPartial) {
                PushLog("部分页处理：已自动跳过「用 OCR 文本替换原文」，未处理页的文字保持可复制");
            }
            if (!stats.verifyNote.empty()) {
                PushLog("注意：输出自检没能执行（" + stats.verifyNote + "）");
            } else if (stats.verifiedPages > 0) {
                char vbuf[96];
                std::snprintf(vbuf, sizeof(vbuf), "输出校验：%d 页内容完整", stats.verifiedPages);
                PushLog(vbuf);
            }
            if (stats.cachedPages > 0 || stats.ocrPages > 0) {
                char cbuf[160];
                std::snprintf(cbuf, sizeof(cbuf), "断点续跑：缓存命中 %d 页 / 新识别 %d 页",
                              stats.cachedPages, stats.ocrPages);
                PushLog(cbuf);
            }
            if (stats.cacheRemoved) PushLog("断点续跑缓存已清理（整篇已完成）");
            if (summaryOut) *summaryOut = buf;
            return true;
        };

        std::string summary;
        if (!batch) {
            runOne(pdf, out, &summary);
        } else {
            const auto bt0 = std::chrono::steady_clock::now();
            int okCount = 0, skipCount = 0, failCount = 0;
            std::vector<std::string> failedNames;
            g_totalPages = static_cast<int>(batchFiles.size());
            g_donePages = 0;
            for (size_t i = 0; i < batchFiles.size(); ++i) {
                const std::string &inFile = batchFiles[i];
                const std::string outFile = DefaultOutputPath(inFile);
                const size_t slash = inFile.find_last_of("\\/");
                const std::string name =
                    slash == std::string::npos ? inFile : inFile.substr(slash + 1);
                PushLog("[" + std::to_string(i + 1) + "/" + std::to_string(batchFiles.size()) + "] " +
                        name);
                if (!g_overwrite && FileExistsUtf8(outFile)) {
                    PushLog("跳过 " + name + "：已有同名 _OCR 产物（勾「覆盖已有结果」可重做）");
                    ++skipCount;
                } else {
                    std::string one;
                    if (runOne(inFile, outFile, &one)) {
                        ++okCount;
                    } else {
                        ++failCount;
                        failedNames.push_back(name);
                    }
                }
                g_donePages = static_cast<int>(i + 1);
            }
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - bt0).count();
            char b2[256];
            std::snprintf(b2, sizeof(b2),
                          "批量完成：成功 %d 个 / 跳过 %d 个 / 失败 %d 个，用时 %.1f 秒", okCount,
                          skipCount, failCount, secs);
            summary = b2;
            if (!failedNames.empty()) {
                std::string f = "失败的文件：";
                for (size_t i = 0; i < failedNames.size(); ++i) {
                    if (i) f += "、";
                    f += failedNames[i];
                }
                PushLog(f);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_result = summary;  // UI 线程在读，加锁避免读到半截字符串
        }
        g_done = true;
        g_running = false;
    });
#endif
}

/// g_result 的加锁快照（UI 线程每帧读取）
std::string ResultText() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_result;
}

#ifdef PUREOCR_HAS_MUPDF
/// 只读文档页数（同一路径只解析一次）；用于界面上显示「共 N 页」
int PageCountForPath(const std::string& path) {
    static std::string cachedPath;
    static int cachedCount = -1;
    if (path == cachedPath) return cachedCount < 0 ? 0 : cachedCount;
    cachedPath = path;
    cachedCount = -1;
    if (!path.empty()) {
        pdf::Document doc;
        std::string err;
        if (doc.Open(path, &err)) cachedCount = doc.PageCount();
    }
    return cachedCount < 0 ? 0 : cachedCount;
}
#endif

#ifdef PUREOCR_SELFTEST
// 离屏抓图用到的 3 个 FBO 入口（主程序不链接 GL loader，这里现取）
// GL 1.1 头里没有 FBO 常量，补上
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
typedef void(APIENTRY* PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
static PFN_glGenFramebuffers p_glGenFramebuffers = nullptr;
static PFN_glBindFramebuffer p_glBindFramebuffer = nullptr;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D = nullptr;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;
static void LoadShotFboFuncs() {
#if defined(_WIN32)
    p_glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
    p_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
    p_glFramebufferTexture2D =
        reinterpret_cast<PFN_glFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
    p_glCheckFramebufferStatus =
        reinterpret_cast<PFN_glCheckFramebufferStatus>(wglGetProcAddress("glCheckFramebufferStatus"));
#else
    p_glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(glfwGetProcAddress("glGenFramebuffers"));
    p_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(glfwGetProcAddress("glBindFramebuffer"));
    p_glFramebufferTexture2D =
        reinterpret_cast<PFN_glFramebufferTexture2D>(glfwGetProcAddress("glFramebufferTexture2D"));
    p_glCheckFramebufferStatus =
        reinterpret_cast<PFN_glCheckFramebufferStatus>(glfwGetProcAddress("glCheckFramebufferStatus"));
#endif
}

// 抓一帧画面存 PNG（这个环境没有可用桌面，只能自己读帧缓冲）
void MaybeCaptureShot(int w, int h) {
    static int frames = 0;
    if (g_shotFrame < 0 || g_shotPath.empty()) return;
    if (++frames < g_shotFrame) return;
    std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (g_shotFbo) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_shotFbo);
    } else {
        glReadBuffer(GL_BACK);
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const size_t row = static_cast<size_t>(w) * 4;
    std::vector<unsigned char> flipped(px.size());
    for (int y = 0; y < h; ++y) std::memcpy(&flipped[row * (h - 1 - y)], &px[row * y], row);
    stbi_write_png(g_shotPath.c_str(), w, h, 4, flipped.data(), static_cast<int>(row));
#ifdef _WIN32
    if (FILE* f = nullptr; fopen_s(&f, (g_shotPath + ".txt").c_str(), "wb") == 0 && f) {
#else
    if (FILE* f = std::fopen((g_shotPath + ".txt").c_str(), "wb")) {
#endif
        int ww = 0, wh = 0, fw = 0, fh = 0;
        GLFWwindow* win = glfwGetCurrentContext();
        glfwGetWindowSize(win, &ww, &wh);
        glfwGetFramebufferSize(win, &fw, &fh);
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(win, &sx, &sy);
        ImDrawData* dd = ImGui::GetDrawData();
        std::fprintf(f, "render=%dx%d window=%dx%d framebuffer=%dx%d contentScale=%.2fx%.2f\n",
                     w, h, ww, wh, fw, fh, sx, sy);
        std::fprintf(f, "drawData=%p lists=%d vtx=%d idx=%d glError=%d metricsVtx=%d\n", (void*)dd,
                     dd ? dd->CmdListsCount : -1, dd ? dd->TotalVtxCount : -1,
                     dd ? dd->TotalIdxCount : -1, (int)glGetError(), ImGui::GetIO().MetricsRenderVertices);
        std::fclose(f);
    }
    glfwSetWindowShouldClose(glfwGetCurrentContext(), 1);
}

// ---- 仅测试用（cmake -DCMAKE_CXX_FLAGS=/DPUREOCR_SELFTEST）：自动连跑两轮识别 ----
bool g_selfTestOk = false;

void SelfTestStep(GLFWwindow* window) {
    static int phase = 0;
    static int idleFrames = 0;
    if (phase == 0) {
        if (g_pdfPath.empty() || g_modelDir.empty()) {
            PushLog("[selftest] 缺少输入或模型目录，无法测试");
            glfwSetWindowShouldClose(window, 1);
            phase = 9;
            return;
        }
        PushLog("[selftest] 第 1 轮开始");
        StartOcr();
        phase = 1;
        return;
    }
    if (phase != 1 && phase != 2) return;
    if (g_running) {
        idleFrames = 0;
        return;
    }
    if (++idleFrames < 20) return;   // 等 UI 收尾几帧再点第二次
    idleFrames = 0;
    if (phase == 1) {
        PushLog("[selftest] 第 2 轮开始（这就是原来闪退的那一步）");
        StartOcr();
        phase = 2;
    } else {
        PushLog("[selftest] 两轮都完成，没有崩溃");
        g_selfTestOk = true;
        glfwSetWindowShouldClose(window, 1);
        phase = 3;
    }
}
#endif

std::string FmtDur(double sec) {
    if (sec < 0) sec = 0;
    const int total = static_cast<int>(sec + 0.5);
    if (total < 60) return std::to_string(total) + " 秒";
    const int m = total / 60;
    const int rs = total % 60;
    if (m < 60) return std::to_string(m) + " 分 " + std::to_string(rs) + " 秒";
    return std::to_string(m / 60) + " 时 " + std::to_string(m % 60) + " 分";
}

// ---------------- 通用小部件 ----------------
void SectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, 4.0f * g_uiScale));
    ImGui::TextColored(AccentColor(), "%s", text);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 1.0f * g_uiScale));
}


/// 单行显示，超宽自动截断成 "…"
void TextEllipsis(const char* text, ImVec4 color) {
    const float avail = ImGui::GetContentRegionAvail().x;
    std::string shown(text ? text : "");
    if (avail > 8.0f && ImGui::CalcTextSize(shown.c_str()).x > avail) {
        while (!shown.empty() && ImGui::CalcTextSize((shown + "…").c_str()).x > avail) {
            size_t i = shown.size();
            do {
                --i;
            } while (i > 0 && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80);
            shown.erase(i);
        }
        shown += "…";
    }
    ImGui::TextColored(color, "%s", shown.c_str());
}

void DrawLogPane(float height) {
    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(
        g_darkTheme ? IM_COL32(12, 14, 19, 255) : IM_COL32(250, 251, 253, 255));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_uiScale, 8.0f * g_uiScale));
    ImGui::BeginChild("##log", ImVec2(0, height),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        static bool stick = true;
        if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 4.0f) stick = false;
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_log.empty() && !g_running) {
            ImGui::TextColored(MutedColor(), "日志会显示在这里");
        }
        // 日志必须按宽度折行：以前超长一行直接被右边界裁掉，关键的报错尾段看不到
        ImGui::PushTextWrapPos(0.0f);
        for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        if (g_running) ImGui::TextUnformatted("……");
        ImGui::PopTextWrapPos();
        if (stick) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/// 左侧控制栏：参数区可滚动，底部固定「开始识别 + 进度 + 日志」
void DrawSidebar() {
    const float s = g_uiScale;
    const bool running = g_running.load();
    const std::string resultText = ResultText();

    // 参数区高度跟着内容走（上一帧量出来的），多出来的高度全给日志，避免侧栏中间一片空白
    static float sContentH = 0.0f;
    const float padY = ImGui::GetStyle().WindowPadding.y;
    const float availY = ImGui::GetContentRegionAvail().y;
    float bottomReserve = 46.0f * s + 12.0f * s + 130.0f * s;
    if (running) {
        bottomReserve += 34.0f * s;
    } else if (!resultText.empty()) {
        bottomReserve += 100.0f * s;
    }
    const float maxScrollH = std::max(160.0f * s, availY - bottomReserve);
    float scrollH = maxScrollH;
    if (sContentH > 0.0f) scrollH = std::min(sContentH + padY, maxScrollH);
    scrollH = std::max(scrollH, std::min(160.0f * s, maxScrollH));

    ImGui::BeginChild("##sideScroll", ImVec2(0, scrollH), ImGuiChildFlags_None);
    {
        // ---- 品牌行 ----
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float box = 40.0f * s;
        dl->AddRectFilled(bp, ImVec2(bp.x + box, bp.y + box), ImGui::GetColorU32(AccentColor()),
                          11.0f * s);
        if (g_fontBrand) {
            const char* brand = "OCR";
            const ImVec2 ts = g_fontBrand->CalcTextSizeA(g_fontBrand->FontSize, FLT_MAX, 0.0f, brand);
            dl->AddText(g_fontBrand, g_fontBrand->FontSize,
                        ImVec2(bp.x + (box - ts.x) * 0.5f, bp.y + (box - ts.y) * 0.5f),
                        IM_COL32(255, 255, 255, 255), brand);
        }
        ImGui::Dummy(ImVec2(box, box));
        ImGui::SameLine(0.0f, 12.0f * s);
        ImGui::BeginGroup();
        if (g_fontTitle) ImGui::PushFont(g_fontTitle);
        ImGui::TextUnformatted("PureOCR");
        if (g_fontTitle) ImGui::PopFont();
        ImGui::TextColored(MutedColor(), "扫描版 PDF → 可搜索文字");
        ImGui::EndGroup();

        // ---- 文件 ----
        SectionLabel("文件");
        const bool batchInput = IsDirPath(g_pdfPath);
        {
            // 等宽平分：以前左边用 -w 撑满，右边只剩 108px，「选择文件夹…」被挤在角落
            const float bgap = 8.0f * s;
            const float bw = std::floor((ImGui::GetContentRegionAvail().x - bgap) * 0.5f);
            ImGui::BeginDisabled(running);
            if (ImGui::Button("选择 PDF 文件…", ImVec2(bw, 40.0f * s))) OpenPdfDialog();
            ImGui::SameLine(0.0f, bgap);
            if (ImGui::Button("选择文件夹…", ImVec2(bw, 40.0f * s))) OpenFolderDialog();
            ImGui::EndDisabled();
        }
        if (batchInput) {
            TextEllipsis(("目录：" + g_pdfPath).c_str(), MutedColor());
            ImGui::Checkbox("含子目录", &g_recurse);
            ImGui::SameLine(0.0f, 16.0f * s);
            ImGui::Checkbox("覆盖已有结果", &g_overwrite);
            ImGui::TextColored(MutedColor(), "输出：每个源文件同目录的 <原名>_OCR.pdf");
        } else {
            if (g_pdfPath.empty()) {
                ImGui::TextColored(MutedColor(), "未选择（也可以把 PDF 或文件夹拖进窗口）");
            } else {
                TextEllipsis(g_pdfPath.c_str(), MutedColor());
            }
            if (!g_outPath.empty()) TextEllipsis(("→ " + g_outPath).c_str(), MutedColor());
            ImGui::BeginDisabled(running);
            if (ImGui::Button("另存为…", ImVec2(-FLT_MIN, 0.0f))) SavePdfDialog();
            ImGui::EndDisabled();
        }

        // ---- 识别设置 ----
        SectionLabel("识别设置");

        static char modelBuf[512] = "";
        if (modelBuf[0] == '\0' && !g_modelDir.empty()) {
            std::snprintf(modelBuf, sizeof(modelBuf), "%s", g_modelDir.c_str());
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(MutedColor(), "模型目录");
        ImGui::SameLine(112.0f * s);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##modeldir", modelBuf, sizeof(modelBuf))) g_modelDir = modelBuf;

        if (g_presetIdx < 0) g_presetIdx = DefaultPresetIndex();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(MutedColor(), "识别模型");
        ImGui::SameLine(112.0f * s);
        {
            int pc = 0;
            const ocr::ModelPreset* presets = ocr::ModelPresetList(&pc);
            static std::vector<std::string> labelStore;
            static std::vector<const char*> labels;
            static std::string labelKey;
            const std::string curKey = g_modelDir + "#" + std::to_string(g_modelStamp);
            if (labelKey != curKey || static_cast<int>(labels.size()) != pc) {
                labelKey = curKey;
                labelStore.clear();
                labels.clear();
                for (int i = 0; i < pc; ++i) {
                    std::string text = presets[i].label;
                    bool missing = false;
                    if (!g_modelDir.empty()) {
                        for (const char* f : {presets[i].det, presets[i].rec}) {
                            if (f && f[0] && !FileExistsUtf8(g_modelDir + "/" + f)) missing = true;
                        }
                    }
                    if (missing) text += "  [缺模型]";
                    labelStore.push_back(text);
                }
                for (const auto& text : labelStore) labels.push_back(text.c_str());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const int presetBefore = g_presetIdx;
            ImGui::Combo("##preset", &g_presetIdx, labels.data(), pc);
            if (g_presetIdx != presetBefore && !MissingModels().empty()) g_openDlPopup = true;
        }

        static char pageBuf[128] = "";
        static bool pageBufInit = false;
        if (!pageBufInit) {
            // 设置里可能存着上次的页范围，必须回显：否则输入框看着是空的，
            // 实际却只跑那几页（典型症状：10 页文件只识别了前 3 页）
            std::snprintf(pageBuf, sizeof(pageBuf), "%s", g_pageSpec.c_str());
            pageBufInit = true;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(MutedColor(), "页范围");
        ImGui::SameLine(112.0f * s);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##pages", "留空 = 全部，例 1-10,15", pageBuf, sizeof(pageBuf))) {
            g_pageSpec = pageBuf;
        }
#ifdef PUREOCR_HAS_MUPDF
        if (batchInput) {
            const int n = BatchPdfCount(g_pdfPath, g_recurse);
            ImGui::TextColored(MutedColor(), "共 %d 个 PDF%s，全部处理", n,
                               g_recurse ? "（含子目录）" : "");
        } else {
            const int pdfPages = PageCountForPath(g_pdfPath);
            std::vector<int> sel;
            const bool parsed = !g_pageSpec.empty() && pdf::ParsePageSpec(g_pageSpec, &sel);
            if (parsed && pdfPages > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "共 %d 页，只处理指定的 %d 页",
                                   pdfPages, static_cast<int>(sel.size()));
            } else if (pdfPages > 0) {
                ImGui::TextColored(MutedColor(), "共 %d 页，全部处理", pdfPages);
            }
        }
#endif
        if (!g_modelDir.empty()) {
            // 下载线程收尾：必须在这里 join，否则下一轮给 joinable 的 thread 赋值会 std::terminate
            if (g_dlActive.load() && g_dl.finished.load()) {
                if (g_dlThread.joinable()) g_dlThread.join();
                g_dlActive = false;
                g_dl.finished = false;
                ++g_modelStamp;
                std::string err;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    err = g_dlError;
                }
                PushLog(err.empty() ? "模型下载完成" : ("模型下载失败：" + err));
            }

            const std::vector<util::ModelFile> missing = MissingModels();
            if (!missing.empty()) {
                std::int64_t totalBytes = 0;
                for (const auto& m : missing) totalBytes += m.size;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.62f, 0.36f, 1.0f));
                ImGui::TextWrapped("缺 %d 个模型文件（%s）", static_cast<int>(missing.size()),
                                   util::FormatBytes(totalBytes).c_str());
                ImGui::PopStyleColor();
                if (g_dlActive.load()) {
                    const std::int64_t got = g_dl.received.load();
                    const std::int64_t tot = g_dl.total.load();
                    const float frac = tot > 0
                                           ? static_cast<float>(static_cast<double>(got) / static_cast<double>(tot))
                                           : 0.0f;
                    char overlay[128];
                    if (tot > 0) {
                        std::snprintf(overlay, sizeof(overlay), "%s / %s",
                                      util::FormatBytes(got).c_str(), util::FormatBytes(tot).c_str());
                    } else {
                        std::snprintf(overlay, sizeof(overlay), "%s", util::FormatBytes(got).c_str());
                    }
                    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);
                } else if (ImGui::Button("下载缺失模型…", ImVec2(-FLT_MIN, 0.0f))) {
                    g_openDlPopup = true;
                }
            }
        }

        // 两列参数：先画一行「标签 … 数值」，下一行再画两个滑块，左右列用 SameLine 对齐。
        // 两列共用同一行的 Y —— 不用 BeginGroup，也就不会出现右列掉半行的问题。
        // 坐标按「设备像素」取整：125%/150% 缩放下 1 逻辑像素 != 1 物理像素，
        // 只按逻辑像素取整，文字仍旧落在物理半像素上，看着发虚。
        const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
        const float pxUnit = (fbScale.x > 0.01f) ? fbScale.x : 1.0f;
        auto px = [pxUnit](float v) { return std::floor(v * pxUnit + 0.5f) / pxUnit; };

        const float gap = 14.0f * s;
        const float rowX = px(ImGui::GetCursorPosX());
        const float colW = px((ImGui::GetContentRegionAvail().x - gap) * 0.5f);
        const float col2X = px(rowX + colW + gap);
        // 每行开头都把 Y 也对齐到设备像素，否则整行文字会因为行高累积小数而发虚
        auto newRow = [&]() { ImGui::SetCursorPos(ImVec2(rowX, px(ImGui::GetCursorPosY()))); };

        // 标签行：左列「标签 …… 数值」，右列同理，数值右对齐到本列右边界
        auto labelRow = [&](const char* l1, const char* v1, const char* l2, const char* v2) {
            newRow();
            ImGui::TextColored(MutedColor(), "%s", l1);
            {
                const float xr = px(rowX + colW - ImGui::CalcTextSize(v1).x);
                if (xr > ImGui::GetCursorPosX() + 8.0f * s) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(xr);
                    ImGui::TextUnformatted(v1);
                }
            }
            ImGui::SameLine(col2X);
            ImGui::TextColored(MutedColor(), "%s", l2);
            {
                const float xr = px(col2X + colW - ImGui::CalcTextSize(v2).x);
                if (xr > ImGui::GetCursorPosX() + 8.0f * s) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(xr);
                    ImGui::TextUnformatted(v2);
                }
            }
        };

        char vb1[32], vb2[32];

        // 第 1 组：线程数 / 渲染 DPI
        std::snprintf(vb1, sizeof(vb1), "%d", g_threads);
        std::snprintf(vb2, sizeof(vb2), "%d", g_dpi);
        labelRow("线程数", vb1, "渲染 DPI", vb2);
        newRow();
        ImGui::SetNextItemWidth(colW);
        ImGui::SliderInt("##threads", &g_threads, 1, 16, "");
        ImGui::SameLine(col2X);
        ImGui::SetNextItemWidth(colW);
        ImGui::SliderInt("##dpi", &g_dpi, 150, 400, "");

        // 第 2 组：最低置信度 / 复核阈值
        std::snprintf(vb1, sizeof(vb1), "%.2f", static_cast<double>(g_minScore));
        std::snprintf(vb2, sizeof(vb2), "%.2f", static_cast<double>(g_reviewScore));
        labelRow("最低置信度", vb1, "复核阈值", vb2);
        newRow();
        ImGui::SetNextItemWidth(colW);
        ImGui::SliderFloat("##minscore", &g_minScore, 0.10f, 0.90f, "", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(col2X);
        ImGui::SetNextItemWidth(colW);
        ImGui::SliderFloat("##reviewscore", &g_reviewScore, 0.50f, 0.95f, "",
                           ImGuiSliderFlags_AlwaysClamp);

        // 勾选框行：同样按设备像素对齐
        newRow();
        ImGui::Checkbox("断点续跑", &g_resume);
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::SetCursorPosX(px(ImGui::GetCursorPosX()));
        ImGui::Checkbox("合并同行碎片", &g_mergeRow);
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::SetCursorPosX(px(ImGui::GetCursorPosX()));
        if (ImGui::Checkbox("深色主题", &g_darkTheme)) ApplyTheme();
        // 「替换原文」会改跨页共享的字体对象，单独一行给足说明，默认不勾。
        newRow();
        ImGui::Checkbox("用 OCR 文本替换原文", &g_maskOriginal);
        ImGui::SameLine(0.0f, 6.0f * s);
        ImGui::SetCursorPosX(px(ImGui::GetCursorPosX()));
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "勾选：屏蔽原文档的字体映射，复制/搜索只剩 OCR 文本。\n"
                "它会改写跨页共享的字体对象，只在「处理全部页」时生效。\n"
                "不勾选（默认）：原文与 OCR 文本并存，只往页面追加内容，最安全。");
        }
        sContentH = ImGui::GetCursorPosY();  // 记下参数区实际高度
    }
    ImGui::EndChild();

    // ---- 开始识别（固定在底部）----
    ImGui::Dummy(ImVec2(0, 2.0f * s));
    const bool canStart = !g_pdfPath.empty() && !running && !g_modelDir.empty();
    if (!canStart) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActive());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f * s);
    if (ImGui::Button(running ? "识别中……" : "开始识别", ImVec2(-FLT_MIN, 46.0f * s))) StartOcr();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (!canStart) ImGui::EndDisabled();

    const int total = g_totalPages.load();
    const int done = g_donePages.load();
    if (running && total > 0) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_startTime).count();
        const double eta = done > 0 ? elapsed / done * static_cast<double>(total - done) : 0.0;
        char overlay[176];
        std::snprintf(overlay, sizeof(overlay), "%d/%d 页 · 已用 %s · 剩余 %s", done, total,
                      FmtDur(elapsed).c_str(), done > 0 ? FmtDur(eta).c_str() : "计算中");
        ImGui::Dummy(ImVec2(0, 2.0f * s));
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total),
                           ImVec2(-FLT_MIN, 26.0f * s), overlay);
    } else if (!resultText.empty()) {
        const bool failed = resultText.rfind("失败", 0) == 0;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(failed ? ImVec4(0.95f, 0.45f, 0.42f, 1.0f)
                                  : ImVec4(0.35f, 0.72f, 0.45f, 1.0f),
                           "%s", resultText.c_str());
        ImGui::PopTextWrapPos();
        if (IsDirPath(g_pdfPath)) {
            if (ImGui::Button("打开输入目录", ImVec2(-FLT_MIN, 32.0f * s)) && !g_pdfPath.empty()) {
                OpenInFileManager(g_pdfPath);
            }
        } else if (!g_outPath.empty()) {
            const float bw = (ImGui::GetContentRegionAvail().x - 2.0f * 8.0f * s) / 3.0f;
            if (ImGui::Button("打开目录", ImVec2(bw, 32.0f * s))) OpenOutputFolder();
            ImGui::SameLine(0.0f, 8.0f * s);
            if (ImGui::Button("导出 CSV", ImVec2(bw, 32.0f * s))) ExportReviewCsv();
            ImGui::SameLine(0.0f, 8.0f * s);
            if (ImGui::Button("回灌 CSV", ImVec2(bw, 32.0f * s))) PickFixCsvDialog();
            if (!g_fixCsv.empty()) TextEllipsis(("已选：" + g_fixCsv).c_str(), MutedColor());
        }
    }

    // ---- 日志（占满剩余）----
    ImGui::Dummy(ImVec2(0, 8.0f * s));
    DrawLogPane(std::max(120.0f * s, ImGui::GetContentRegionAvail().y));
}
#ifdef PUREOCR_HAS_MUPDF
void UploadTexture(const std::vector<unsigned char>& rgba, int w, int h) {
    if (!g_tex) glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    g_texW = w;
    g_texH = h;
    g_texError.clear();
}

void CenterText(const char* text, ImVec4 color) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ts.x) * 0.5f);
    ImGui::TextColored(color, "%s", text);
}

/// 右侧预览区：页导航 + 常驻画布（竖版页面按高度铺满）
// ---- 缺少模型时的下载弹窗 ----
void DrawDownloadModal() {
    if (g_openDlPopup) {
        ImGui::OpenPopup("缺少模型###dlg");
        g_openDlPopup = false;
    }
    const float s = g_uiScale;
    ImGui::SetNextWindowSize(ImVec2(600.0f * s, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("缺少模型###dlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const std::vector<util::ModelFile> missing = MissingModels();
    if (missing.empty()) {
        ImGui::TextUnformatted("模型已经齐全，可以开始识别了。");
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(130.0f * s, 36.0f * s))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    std::int64_t totalBytes = 0;
    bool noUrl = false;
    for (const auto& m : missing) {
        totalBytes += m.size;
        if (m.url.empty()) noUrl = true;
    }

    const ocr::ModelPreset* mp = CurrentPreset();
    ImGui::TextWrapped("预设「%s」需要以下模型，本机还没有：", mp ? mp->label : "");
    ImGui::Spacing();
    for (const auto& m : missing) {
        ImGui::BulletText("%s", m.file.c_str());
        ImGui::SameLine();
        ImGui::TextColored(MutedColor(), "%s", util::FormatBytes(m.size).c_str());
    }
    ImGui::Spacing();
    ImGui::TextColored(MutedColor(), "保存到：%s", g_modelDir.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    if (g_dlActive.load()) {
        const std::int64_t got = g_dl.received.load();
        const std::int64_t tot = g_dl.total.load();
        const float frac =
            tot > 0 ? static_cast<float>(static_cast<double>(got) / static_cast<double>(tot)) : 0.0f;
        char overlay[128];
        if (tot > 0) {
            std::snprintf(overlay, sizeof(overlay), "%s / %s", util::FormatBytes(got).c_str(),
                          util::FormatBytes(tot).c_str());
        } else {
            std::snprintf(overlay, sizeof(overlay), "%s", util::FormatBytes(got).c_str());
        }
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);
        const std::string msg = g_dl.Message();
        if (!msg.empty()) ImGui::TextColored(MutedColor(), "%s", msg.c_str());
        ImGui::Spacing();
        if (ImGui::Button("取消下载", ImVec2(130.0f * s, 36.0f * s))) g_dl.cancel = true;
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(MutedColor(), "下载源");
        ImGui::SameLine(96.0f * s);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##dlsource", &g_dlSourceIdx, "自动（官方优先，失败自动换镜像）\0官方 HuggingFace\0镜像 hf-mirror\0");
        ImGui::Spacing();
        const std::string btn = "开始下载（" + util::FormatBytes(totalBytes) + "）";
        ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActive());
        if (ImGui::Button(btn.c_str(), ImVec2(230.0f * s, 42.0f * s))) StartModelDownload();
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("稍后再说", ImVec2(130.0f * s, 42.0f * s))) ImGui::CloseCurrentPopup();
        if (noUrl) {
            ImGui::Spacing();
            ImGui::TextWrapped("注意：其中有模型没有在线下载源，需要手动放到模型目录。");
        }
    }
    ImGui::EndPopup();
}

void DrawStage() {
    const float s = g_uiScale;
    if (g_pdfPath.empty()) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(0, std::max(24.0f, avail.y * 0.34f)));
        if (g_fontTitle) ImGui::PushFont(g_fontTitle);
        CenterText("等待 PDF", MutedColor());
        if (g_fontTitle) ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 8.0f * s));
        CenterText("点左侧「选择 PDF 文件」，或者把文件直接拖进窗口", MutedColor());
        return;
    }

    if (IsDirPath(g_pdfPath)) {
        ImGui::Dummy(ImVec2(0, 40.0f * s));
        if (g_fontTitle) ImGui::PushFont(g_fontTitle);
        CenterText("批量模式", MutedColor());
        if (g_fontTitle) ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 8.0f * s));
        CenterText("识别结果写入每个源文件同目录的 <原名>_OCR.pdf", MutedColor());
        CenterText("进度、逐文件结果看左侧日志", MutedColor());
        return;
    }

    const bool outReady = g_done && !g_outPath.empty() &&
                          std::filesystem::exists(std::filesystem::u8path(g_outPath));
    const std::string target = outReady ? g_outPath : g_pdfPath;

    std::string openErr;
    if (!g_preview.Open(target, &openErr)) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.42f, 1.0f), "打开失败：%s", openErr.c_str());
        return;
    }
    const int pageCount = g_preview.PageCount();
    if (pageCount <= 0) {
        ImGui::TextUnformatted("文档没有页面。");
        return;
    }
    if (g_previewPage > pageCount - 1) g_previewPage = pageCount - 1;
    if (g_previewPage < 0) g_previewPage = 0;

    // 本页识别结果（供叠加框 / 提示用）
    const pdf::OcrPageLines* pl = nullptr;
    for (const auto& pg : g_pageLines) {
        if (pg.page == g_previewPage) {
            pl = &pg;
            break;
        }
    }

    // ---- 工具条：第一行导航 ----
    const float rowH = 32.0f * s;
    if (ImGui::Button("<", ImVec2(36.0f * s, rowH)) && g_previewPage > 0) --g_previewPage;
    ImGui::SameLine(0.0f, 6.0f * s);
    {
        int pageNo = g_previewPage + 1;
        ImGui::SetNextItemWidth(150.0f * s);
        if (ImGui::SliderInt("##page", &pageNo, 1, pageCount, "第 %d 页")) g_previewPage = pageNo - 1;
    }
    ImGui::SameLine(0.0f, 6.0f * s);
    if (ImGui::Button(">", ImVec2(36.0f * s, rowH)) && g_previewPage < pageCount - 1) ++g_previewPage;
    ImGui::SameLine(0.0f, 12.0f * s);
    ImGui::TextColored(MutedColor(), "共 %d 页", pageCount);

    {
        const char* status = outReady ? "OCR 产物" : "原文件";
        char sbuf[64];
        std::snprintf(sbuf, sizeof(sbuf), "● %s", status);
        // 用「当前游标 + 剩余宽度」算右边界，直接用内容区最大值会被窗口 padding 带偏，
        // 结果整段文字画出可视区域，最后一个字被裁掉
        const ImVec2 ts = ImGui::CalcTextSize(sbuf);
        const float curX = ImGui::GetCursorScreenPos().x;
        const float targetX = std::floor(curX + ImGui::GetContentRegionAvail().x - ts.x);
        if (targetX > curX + 12.0f * s) {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(targetX, ImGui::GetCursorScreenPos().y));
            ImGui::TextColored(outReady ? ImVec4(0.30f, 0.68f, 0.42f, 1.0f) : MutedColor(), "%s",
                               sbuf);
        }
    }

    // ---- 工具条：第二行预览参数（窄窗口自动换行，避免控件被裁掉）----
    const float rowW = ImGui::GetContentRegionAvail().x;
    const bool wideRow = rowW > 520.0f * s;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(MutedColor(), "预览");
    ImGui::SameLine(0.0f, 8.0f * s);
    ImGui::SetNextItemWidth(std::max(120.0f * s, std::min(200.0f * s, rowW * 0.36f)));
    // 滑块 + 右侧数值：既有明显的把手可以拖，数字也不会被把手挡住
    ImGui::SliderFloat("##previewdpi", &g_previewDpi, 72.0f, 200.0f, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, 8.0f * s);
    ImGui::TextColored(MutedColor(), "%.0f dpi", g_previewDpi);
    if (wideRow) ImGui::SameLine(0.0f, 14.0f * s);
    ImGui::Checkbox("显示识别框", &g_showBoxes);
    if (wideRow) ImGui::SameLine(0.0f, 14.0f * s);
    ImGui::Checkbox("只看低置信度", &g_onlyLowConf);
    if (pl && !pl->lines.empty()) {
        int low = 0;
        for (const auto& ln : pl->lines) low += (ln.score < g_reviewScore) ? 1 : 0;
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::TextColored(MutedColor(), "本页 %d 行，低分 %d",
                           static_cast<int>(pl->lines.size()), low);
    }

    // ---- 页面图（页码 / DPI / 文件变化才重渲染）----
    if (g_texPath != target || g_texPage != g_previewPage || std::fabs(g_texDpi - g_previewDpi) > 0.5f) {
        std::vector<unsigned char> rgba;
        int tw = 0, th = 0;
        std::string rerr;
        if (g_preview.Render(g_previewPage, g_previewDpi, &rgba, &tw, &th, &rerr)) {
            UploadTexture(rgba, tw, th);
            g_texPath = target;
            g_texPage = g_previewPage;
            g_texDpi = g_previewDpi;
        } else {
            g_texError = rerr;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(
                                                g_darkTheme ? IM_COL32(8, 9, 12, 255)
                                                            : IM_COL32(228, 232, 240, 255)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * s, 16.0f * s));
    ImGui::BeginChild("##canvas", ImVec2(0, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        if (!g_texError.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.42f, 1.0f), "渲染失败：%s", g_texError.c_str());
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (g_tex && g_texW > 0 && g_texH > 0) {
            float scale =
                std::min(avail.x / static_cast<float>(g_texW), avail.y / static_cast<float>(g_texH));
            if (scale <= 0.0f) scale = 1.0f;
            const ImVec2 size(g_texW * scale, g_texH * scale);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - size.x) * 0.5f,
                                             origin.y + (avail.y - size.y) * 0.5f));
            ImGui::Image(static_cast<ImTextureID>(g_tex), size);

            const ImVec2 p0 = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p0.x + size.x + 1.0f, p0.y + size.y + 1.0f),
                        ImGui::GetColorU32(ImGuiCol_Border), 3.0f * s);

            if (pl && pl->width > 0 && pl->height > 0 && !pl->lines.empty()) {
                const float bx = size.x / static_cast<float>(pl->width);
                const float by = size.y / static_cast<float>(pl->height);
                if (g_showBoxes) {
                    for (const auto& ln : pl->lines) {
                        const bool low = ln.score < g_reviewScore;
                        if (g_onlyLowConf && !low) continue;
                        const ImU32 col = low ? IM_COL32(255, 96, 92, 200) : IM_COL32(64, 208, 120, 180);
                        dl->AddRect(ImVec2(p0.x + ln.x0 * bx, p0.y + ln.y0 * by),
                                    ImVec2(p0.x + ln.x1 * bx, p0.y + ln.y1 * by), col, 2.0f * s, 0,
                                    1.5f * s);
                    }
                }
                const ImVec2 m = ImGui::GetIO().MousePos;
                if (ImGui::IsItemHovered() && m.x >= p0.x && m.y >= p0.y && m.x <= p0.x + size.x &&
                    m.y <= p0.y + size.y) {
                    const float ix = (m.x - p0.x) / bx;
                    const float iy = (m.y - p0.y) / by;
                    for (const auto& ln : pl->lines) {
                        if (ix >= ln.x0 && ix <= ln.x1 && iy >= ln.y0 && iy <= ln.y1) {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(MutedColor(), "置信度 %.2f", ln.score);
                            ImGui::PushTextWrapPos(420.0f * s);
                            ImGui::TextUnformatted(ln.text.c_str());
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                            break;
                        }
                    }
                }
            }
        } else {
            ImGui::TextUnformatted("渲染中……");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
#endif

int RunGui() {
    g_modelDir = ExeDir() + "/models";
    LoadSettings();
    {
        // 全新安装（模型目录里一个模型都没有）时直接弹下载向导，省得用户对着「缺模型」发呆
        bool anyModel = false;
        for (const auto& m : ModelRegistryCached()) {
            if (util::FileReady(util::JoinPath(g_modelDir, m.file))) {
                anyModel = true;
                break;
            }
        }
        if (!anyModel) g_openDlPopup = true;
    }    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // 窗口默认占工作区 92%（封顶 1600x1080），并居中
    int mwx = 0, mwy = 0, mww = 1280, mwh = 800;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) glfwGetMonitorWorkarea(mon, &mwx, &mwy, &mww, &mwh);
    int winW = std::min(1600, static_cast<int>(mww * 0.92f));
    int winH = std::min(1080, static_cast<int>(mwh * 0.92f));

    if (winW < 900) winW = 900;
    if (winH < 620) winH = 620;
    const std::string windowTitle =
        std::string("PureOCR ") + PUREOCR_VERSION_STRING + " - 扫描 PDF 转可复制文字";
    GLFWwindow* window = glfwCreateWindow(winW, winH, windowTitle.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowPos(window, mwx + (mww - winW) / 2, mwy + (mwh - winH) / 2);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#ifdef _WIN32
    // 窗口 / 任务栏图标（资源里编进去的 .ico）
    if (HICON ico = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED))) {
        HWND hwnd = glfwGetWin32Window(window);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ico));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ico));
    }
#endif

    // UI 缩放：帧缓冲和窗口同样大小（物理像素渲染）时，按显示器缩放放大界面
    float csX = 1.0f, csY = 1.0f;
    glfwGetWindowContentScale(window, &csX, &csY);
    int ww0 = 0, wh0 = 0, fw0 = 0, fh0 = 0;
    glfwGetWindowSize(window, &ww0, &wh0);
    glfwGetFramebufferSize(window, &fw0, &fh0);
    const float fbScale = (ww0 > 0) ? static_cast<float>(fw0) / static_cast<float>(ww0) : 1.0f;
    g_uiScale = (fbScale > 1.01f) ? 1.0f : std::max(1.0f, csX);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ApplyTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetDropCallback(window, DropCallback);
    ImGui_ImplOpenGL3_Init("#version 330");

#ifdef PUREOCR_SELFTEST
    // 只用到几个 FBO 入口，用 wglGetProcAddress 现取，避免给主程序引入 GL loader
    if (g_shotW > 256 && g_shotH > 256) LoadShotFboFuncs();
#endif

#ifdef PUREOCR_SELFTEST
    // --shot 时用离屏 FBO 按指定分辨率渲染：远端桌面分辨率很小，直接开大窗口会被系统压回去
    if (g_shotW > 256 && g_shotH > 256) {
        glGenTextures(1, &g_shotTex);
        glBindTexture(GL_TEXTURE_2D, g_shotTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_shotW, g_shotH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glGenFramebuffers(1, &g_shotFbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_shotFbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_shotTex, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) g_shotFbo = 0;
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
#endif

    // 字体：优先微软雅黑，缺字/缺失时退回黑体；正文、标题、品牌各一份
#ifdef _WIN32
    const char* fontPath = FileExistsUtf8("C:\\Windows\\Fonts\\msyh.ttc")
                               ? "C:\\Windows\\Fonts\\msyh.ttc"
                               : "C:\\Windows\\Fonts\\simhei.ttf";
#else
    // 找一个能显示中文的字体；都没有就退回 ImGui 内置字体（会缺字，但不会崩）
    const char* fontPath = nullptr;
    static const char* kLinuxFonts[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* c : kLinuxFonts) {
        if (FileExistsUtf8(c)) { fontPath = c; break; }
    }
#endif
    // 常用简体字表里没有 "阈/渲" 之类，另外还要箭头、圆点、对勾等符号，统一补上
    static ImVector<ImWchar> glyphRanges;
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        // 常用符号整段打包（箭头 / 几何图形 / 杂项符号），以后文案里加箭头、星号也不会缺字
        static const ImWchar symbolRanges[] = {
            0x2190, 0x21FF,  // ← ↑ → ↓ 等箭头
            0x25A0, 0x25FF,  // ● ○ ◆ ◇ ■ □ ▲ 等几何图形
            0x2600, 0x27BF,  // ★ ☆ ✔ ✓ ✕ ✗ 等杂项符号
            0
        };
        builder.AddRanges(symbolRanges);
        builder.AddText("×·±°≈≤≥…—–");
        // 这 16 个字在「常用 2500 字」表之外，漏了会渲染成方框（看着就是乱码）：
        // 屏(屏蔽) 崩/溃(崩溃) 嵌(嵌入) 帧(帧) 楷 赋 轴 辑 逻 韩 馈 剔 兜 缀 桩
        builder.AddText("阈渲噪伽");
        builder.AddText("屏崩溃嵌帧楷赋轴辑逻韩馈剔兜缀桩");
        builder.BuildRanges(&glyphRanges);
    }
    const ImWchar* ranges = glyphRanges.Data;
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 1;
    cfg.RasterizerMultiply = 1.05f;
    ImFont* uiFont = nullptr;
    if (fontPath) {
        uiFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f * g_uiScale, &cfg, ranges);
        if (uiFont) {
            g_fontTitle = io.Fonts->AddFontFromFileTTF(fontPath, 21.0f * g_uiScale, &cfg, ranges);
            g_fontBrand = io.Fonts->AddFontFromFileTTF(fontPath, 17.0f * g_uiScale, &cfg, ranges);
        }
    }
    if (!uiFont) {
        uiFont = io.Fonts->AddFontDefault();
        g_fontTitle = nullptr;
        g_fontBrand = nullptr;
    }
    io.FontDefault = uiFont;


    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
#ifdef PUREOCR_SELFTEST
        if (g_selfTest) SelfTestStep(window);
#endif
        static int devFrames = 0;
        if (++devFrames % 60 == 0)        ImGui_ImplGlfw_NewFrame();
#ifdef PUREOCR_SELFTEST
        if (g_shotFbo) ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(g_shotW), static_cast<float>(g_shotH));
#endif
        ImGui::NewFrame();

        const float s = g_uiScale;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);

        const float rootW = ImGui::GetContentRegionAvail().x;
        const float sideW = std::min(std::clamp(rootW * 0.30f, 340.0f * s, 520.0f * s), rootW * 0.55f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 16.0f * s));
        ImGui::BeginChild("##sidebar", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawSidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 16.0f * s));
        ImGui::BeginChild("##stage", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
#ifdef PUREOCR_HAS_MUPDF
        DrawStage();
#else
        ImGui::TextUnformatted("此版本未包含 MuPDF，无法预览 PDF。");
#endif
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

#ifdef PUREOCR_HAS_MUPDF
        DrawDownloadModal();
#endif

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
#ifdef PUREOCR_SELFTEST
        if (g_shotFbo) {
            w = g_shotW;
            h = g_shotH;
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_shotFbo);
        }
#endif
        glViewport(0, 0, w, h);
        if (g_darkTheme) {
            glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        } else {
            glClearColor(0.94f, 0.94f, 0.96f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#ifdef PUREOCR_SELFTEST
        MaybeCaptureShot(w, h);
        if (g_shotFbo) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
        glfwSwapBuffers(window);
    }

    if (g_running) g_done = false;
    if (g_worker.joinable()) {
        // 关闭窗口时等待后台结束，避免进程被强制结束留下半成品
        for (int i = 0; i < 600 && g_running; ++i) {
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100 * 1000);
#endif
        }
        if (g_running) {
            g_worker.detach();
        } else {
            g_worker.join();
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    SaveSettings();
    glfwTerminate();
#ifdef PUREOCR_SELFTEST
    if (g_selfTest && !g_selfTestOk) return 3;
#endif
    return 0;
}

int RunApp(int argc, char** argv) {
#ifdef PUREOCR_SELFTEST
    if (argc > 2 && std::string(argv[1]) == "--shot") {
        g_shotPath = argv[2];
        g_shotFrame = 200;
        if (argc > 3) g_shotW = std::atoi(argv[3]);
        if (argc > 4) g_shotH = std::atoi(argv[4]);
        return RunGui();
    }
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        g_selfTest = true;
        return RunGui();
    }
#endif
    if (argc > 1) {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
        std::vector<char*> argvMut;
        argvMut.reserve(args.size());
        for (auto& a : args) argvMut.push_back(const_cast<char*>(a.c_str()));
        return RunCli(static_cast<int>(argvMut.size()), argvMut.data());
    }
    return RunGui();
}

}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) args.push_back(util::WideToUtf8(wargv[i]));
    if (wargv) LocalFree(wargv);

    if (args.size() > 1) {
        // 有参数 -> CLI 模式，需要把 stdout 接回父控制台。
        // 若被父进程重定向到文件/管道（>log.txt、| more），必须保留继承来的句柄。
        auto needsConsole = [](HANDLE h) {
            if (h == nullptr || h == INVALID_HANDLE_VALUE) return true;
            return GetFileType(h) == FILE_TYPE_CHAR;
        };
        const bool coutConsole = needsConsole(GetStdHandle(STD_OUTPUT_HANDLE));
        const bool cerrConsole = needsConsole(GetStdHandle(STD_ERROR_HANDLE));
        if (coutConsole || cerrConsole) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
            FILE* f = nullptr;
            if (coutConsole) freopen_s(&f, "CONOUT$", "w", stdout);
            if (cerrConsole) freopen_s(&f, "CONOUT$", "w", stderr);
            SetConsoleOutputCP(CP_UTF8);
        }
    }
    std::vector<char*> argvMut;
    argvMut.reserve(args.size());
    for (auto& a : args) argvMut.push_back(const_cast<char*>(a.c_str()));
    return RunApp(static_cast<int>(argvMut.size()), argvMut.data());
}
#else
int main(int argc, char** argv) { return RunApp(argc, argv); }
#endif
