// ocr-gui：无参数启动 GUI；有参数走 CLI（--pdf / --image ...）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "resource.h"
#include "stb_image_write.h"

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
#include "util/http_download.h"
#include "util/model_download.h"
#include "util/model_registry.h"
#include "cli.h"
#ifdef OCRGUI_HAS_MUPDF
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
float g_minScore = 0.5f;
float g_reviewScore = 0.75f;
double g_lastDpi = 300.0;
int g_threads = 4;
int g_dpi = 300;
bool g_switchToPreview = false;
#ifdef OCRGUI_SELFTEST
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

#ifdef OCRGUI_HAS_MUPDF
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

std::string WideToUtf8(const wchar_t* s) {
    if (!s) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::vector<char> buf(static_cast<size_t>(n));  // 含结尾 0，避免写越界
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.data(), n, nullptr, nullptr);
    return std::string(buf.data(), static_cast<size_t>(n - 1));
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(n));  // 含结尾 0，避免写越界
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), n);
    return std::wstring(buf.data(), static_cast<size_t>(n - 1));
}

std::string ExeDir() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string dir = WideToUtf8(exe);
    const size_t p = dir.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : dir.substr(0, p);
}

void PushLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_log.push_back(line);
    if (g_log.size() > 500) g_log.erase(g_log.begin(), g_log.begin() + 100);
}

std::string DefaultOutputPath(const std::string& input) {
    return util::InsertBeforeExtension(input, "_OCR");
}

bool FileExistsUtf8(const std::string& path) {
    return GetFileAttributesW(Utf8ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// OCR 文本层要嵌入的字体：优先黑体，再退化到楷体 / 雅黑 / 宋体。
/// 以前这里写死 simhei.ttf，没装该字体的机器会直接"文本层初始化失败"。
std::string OcrFontPath() {
    static const char* kCandidates[] = {"C:\\Windows\\Fonts\\simhei.ttf",
                                        "C:\\Windows\\Fonts\\simkai.ttf",
                                        "C:\\Windows\\Fonts\\msyh.ttc",
                                        "C:\\Windows\\Fonts\\simsun.ttc"};
    for (const char* c : kCandidates) {
        if (FileExistsUtf8(c)) return c;
    }
    return kCandidates[0];
}

/// %LOCALAPPDATA%\ocrgui\settings.ini
std::string SettingsPath() {
    std::string dir;
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "LOCALAPPDATA") == 0 && buf) {
        dir = buf;
        free(buf);
    }
    if (dir.empty()) dir = ExeDir();
    CreateDirectoryW(Utf8ToWide(dir + "\\ocrgui").c_str(), nullptr);
    return dir + "\\ocrgui\\settings.ini";
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
        const std::string out = str("output");
        g_outPath = (!out.empty() && FileExistsUtf8(out)) ? out : DefaultOutputPath(in);
    }
    const std::string md = str("model_dir");
    if (!md.empty() && FileExistsUtf8(md)) g_modelDir = md;
    if (num("dpi") > 0) g_dpi = static_cast<int>(num("dpi"));
    if (num("threads") > 0) g_threads = static_cast<int>(num("threads"));
    if (num("min_score") > 0) g_minScore = static_cast<float>(num("min_score"));
    if (num("review_score") > 0) g_reviewScore = static_cast<float>(num("review_score"));
    if (kv.count("merge_row")) g_mergeRow = str("merge_row") != "0";
    if (kv.count("mask")) g_maskOriginal = str("mask") != "0";
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
    std::fprintf(f, "# ocrgui 设置（自动生成，可删）\n");
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

/// 选择回灌用的复核 CSV
void PickFixCsvDialog() {
    wchar_t file[1024] = L"";
    if (!g_fixCsv.empty()) wcsncpy_s(file, Utf8ToWide(g_fixCsv).c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    g_fixCsv = WideToUtf8(file);
    PushLog("已选择复核 CSV：" + g_fixCsv);
    PushLog("点「开始识别」应用修改（同参数下会命中缓存，很快）");
}

void OpenPdfDialog() {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        g_pdfPath = WideToUtf8(file);
        g_outPath = DefaultOutputPath(g_pdfPath);
    }
}

void SavePdfDialog() {
    wchar_t file[1024] = L"";
    if (!g_outPath.empty()) {
        const std::wstring w = Utf8ToWide(g_outPath);
        wcsncpy_s(file, w.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"pdf";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) g_outPath = WideToUtf8(file);
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
    wchar_t file[1024] = L"";
    const std::wstring w = Utf8ToWide(path);
    wcsncpy_s(file, w.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    path = WideToUtf8(file);
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
    ShellExecuteW(nullptr, L"open", Utf8ToWide(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
#ifdef OCRGUI_HAS_MUPDF
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
#ifdef OCRGUI_HAS_MUPDF
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
#ifdef OCRGUI_HAS_MUPDF
    g_preview.Close();  // 释放上一轮对输出 PDF 的只读句柄，避免写同名文件被占用
    g_texPath.clear();
    g_texPage = -1;
#endif

    const std::string pdf = g_pdfPath;
    const std::string out = g_outPath.empty() ? DefaultOutputPath(pdf) : g_outPath;
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
        pdf::OcrRunOptions opt;
        opt.inputPath = pdf;
        opt.outputPath = out;
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
        PushLog("开始识别：" + pdf + "（" + pageSpecNote + "）");
        PushLog("输出：" + out);
        const bool ok = pdf::RunPdfOcr(
            opt,
            [](int done, int total, int page, int lines) {
                g_totalPages = total;
                g_donePages = done;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "[%d/%d] 第 %d 页：%d 行", done, total, page + 1, lines);
                PushLog(buf);
            },
            &stats, &err);
#ifdef OCRGUI_HAS_MUPDF
        if (ok) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pageLines = std::move(stats.pageLines);
        }
#endif
        std::string summary;
        if (!ok) {
            PushLog("失败：" + err);
            summary = "失败：" + err;
        } else {
            char buf[256];
            if (stats.maskedFonts > 0) {
                std::snprintf(buf, sizeof(buf), "完成：%d 页 / %d 行，替换原文（屏蔽字体 %d 个），用时 %.1f 秒",
                              stats.pageCount, stats.lineCount, stats.maskedFonts, stats.seconds);
            } else {
                std::snprintf(buf, sizeof(buf), "完成：%d 页 / %d 行（原文保留，仅叠加隐形文字层），用时 %.1f 秒",
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
            summary = buf;
            if (stats.cachedPages > 0 || stats.ocrPages > 0) {
                char cbuf[160];
                std::snprintf(cbuf, sizeof(cbuf), "断点续跑：缓存命中 %d 页 / 新识别 %d 页",
                              stats.cachedPages, stats.ocrPages);
                PushLog(cbuf);
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

#ifdef OCRGUI_HAS_MUPDF
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

#ifdef OCRGUI_SELFTEST
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
    p_glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
    p_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
    p_glFramebufferTexture2D =
        reinterpret_cast<PFN_glFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
    p_glCheckFramebufferStatus =
        reinterpret_cast<PFN_glCheckFramebufferStatus>(wglGetProcAddress("glCheckFramebufferStatus"));
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
    if (FILE* f = nullptr; fopen_s(&f, (g_shotPath + ".txt").c_str(), "wb") == 0 && f) {
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

// ---- 仅测试用（cmake -DCMAKE_CXX_FLAGS=/DOCRGUI_SELFTEST）：自动连跑两轮识别 ----
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

void FieldLabel(const char* text) { ImGui::TextColored(MutedColor(), "%s", text); }

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
        for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        if (g_running) ImGui::TextUnformatted("……");
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
        ImGui::TextUnformatted("PDF OCR");
        if (g_fontTitle) ImGui::PopFont();
        ImGui::TextColored(MutedColor(), "扫描版 PDF → 可搜索文字");
        ImGui::EndGroup();

        // ---- 文件 ----
        SectionLabel("文件");
        {
            const float saveW = 100.0f * s;
            ImGui::BeginDisabled(running);
            if (ImGui::Button("选择 PDF 文件...", ImVec2(-saveW - 8.0f * s, 40.0f * s))) {
                OpenPdfDialog();
            }
            ImGui::SameLine(0.0f, 8.0f * s);
            if (ImGui::Button("另存为…", ImVec2(saveW, 40.0f * s))) SavePdfDialog();
            ImGui::EndDisabled();
        }
        if (g_pdfPath.empty()) {
            ImGui::TextColored(MutedColor(), "未选择（也可以把 PDF 直接拖进窗口）");
        } else {
            TextEllipsis(g_pdfPath.c_str(), MutedColor());
        }
        if (!g_outPath.empty()) TextEllipsis(("→ " + g_outPath).c_str(), MutedColor());

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
                            if (f && f[0] && !FileExistsUtf8(g_modelDir + "\\" + f)) missing = true;
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
#ifdef OCRGUI_HAS_MUPDF
        {
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

        const float gap = 14.0f * s;
        const float halfW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;

        ImGui::BeginGroup();
        FieldLabel("线程数");
        ImGui::SetNextItemWidth(halfW);
        ImGui::SliderInt("##threads", &g_threads, 1, 16);
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, gap);
        ImGui::BeginGroup();
        FieldLabel("渲染 DPI");
        ImGui::SetNextItemWidth(halfW);
        ImGui::SliderInt("##dpi", &g_dpi, 150, 400);
        ImGui::EndGroup();

        ImGui::BeginGroup();
        FieldLabel("最低置信度");
        ImGui::SetNextItemWidth(halfW);
        ImGui::SliderFloat("##minscore", &g_minScore, 0.10f, 0.90f, "%.2f");
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, gap);
        ImGui::BeginGroup();
        FieldLabel("复核阈值");
        ImGui::SetNextItemWidth(halfW);
        ImGui::SliderFloat("##reviewscore", &g_reviewScore, 0.50f, 0.95f, "%.2f");
        ImGui::EndGroup();

        ImGui::Checkbox("断点续跑", &g_resume);
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::Checkbox("合并同行碎片", &g_mergeRow);
        ImGui::SameLine(0.0f, 16.0f * s);
        if (ImGui::Checkbox("深色主题", &g_darkTheme)) ApplyTheme();
        // 「替换原文」会改跨页共享的字体对象，单独一行给足说明，默认不勾。
        ImGui::Checkbox("用 OCR 文本替换原文", &g_maskOriginal);
        ImGui::SameLine(0.0f, 6.0f * s);
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
        if (!g_outPath.empty()) {
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
#ifdef OCRGUI_HAS_MUPDF
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
        const float tw = ImGui::CalcTextSize("● ").x + ImGui::CalcTextSize(status).x + 4.0f * s;
        const float x = ImGui::GetWindowContentRegionMax().x - tw;
        if (x > ImGui::GetCursorPosX() + 12.0f * s) {
            ImGui::SameLine(x);
            ImGui::TextColored(outReady ? ImVec4(0.30f, 0.68f, 0.42f, 1.0f) : MutedColor(), "● %s",
                               status);
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
    g_modelDir = ExeDir() + "\\models";
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
    GLFWwindow* window = glfwCreateWindow(winW, winH, "PDF OCR - 扫描 PDF 转可复制文字", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowPos(window, mwx + (mww - winW) / 2, mwy + (mwh - winH) / 2);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // 窗口 / 任务栏图标（资源里编进去的 .ico）
    if (HICON ico = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED))) {
        HWND hwnd = glfwGetWin32Window(window);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ico));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ico));
    }

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

#ifdef OCRGUI_SELFTEST
    // 只用到几个 FBO 入口，用 wglGetProcAddress 现取，避免给主程序引入 GL loader
    if (g_shotW > 256 && g_shotH > 256) LoadShotFboFuncs();
#endif

#ifdef OCRGUI_SELFTEST
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
    const char* fontPath = FileExistsUtf8("C:\\Windows\\Fonts\\msyh.ttc")
                               ? "C:\\Windows\\Fonts\\msyh.ttc"
                               : "C:\\Windows\\Fonts\\simhei.ttf";
    // 常用简体字表里没有 "阈/渲" 之类，另外还要箭头、圆点、对勾等符号，统一补上
    static ImVector<ImWchar> glyphRanges;
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.AddText("→←↑↓●○◆◇✔✓✕✗×·±°≈≤≥…—–");
        builder.AddText("阈渲噪伽");
        builder.BuildRanges(&glyphRanges);
    }
    const ImWchar* ranges = glyphRanges.Data;
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 1;
    cfg.RasterizerMultiply = 1.05f;
    ImFont* uiFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f * g_uiScale, &cfg, ranges);
    if (!uiFont) uiFont = io.Fonts->AddFontDefault();
    io.FontDefault = uiFont;
    g_fontTitle = io.Fonts->AddFontFromFileTTF(fontPath, 21.0f * g_uiScale, &cfg, ranges);
    g_fontBrand = io.Fonts->AddFontFromFileTTF(fontPath, 17.0f * g_uiScale, &cfg, ranges);


    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
#ifdef OCRGUI_SELFTEST
        if (g_selfTest) SelfTestStep(window);
#endif
        static int devFrames = 0;
        if (++devFrames % 60 == 0)        ImGui_ImplGlfw_NewFrame();
#ifdef OCRGUI_SELFTEST
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
#ifdef OCRGUI_HAS_MUPDF
        DrawStage();
#else
        ImGui::TextUnformatted("此版本未包含 MuPDF，无法预览 PDF。");
#endif
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

#ifdef OCRGUI_HAS_MUPDF
        DrawDownloadModal();
#endif

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
#ifdef OCRGUI_SELFTEST
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
#ifdef OCRGUI_SELFTEST
        MaybeCaptureShot(w, h);
        if (g_shotFbo) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
        glfwSwapBuffers(window);
    }

    if (g_running) g_done = false;
    if (g_worker.joinable()) {
        // 关闭窗口时等待后台结束，避免进程被强制结束留下半成品
        for (int i = 0; i < 600 && g_running; ++i) Sleep(100);
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
#ifdef OCRGUI_SELFTEST
    if (g_selfTest && !g_selfTestOk) return 3;
#endif
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
#ifdef OCRGUI_SELFTEST
    if (wargv && argc > 2 && WideToUtf8(wargv[1]) == "--shot") {
        g_shotPath = WideToUtf8(wargv[2]);
        g_shotFrame = 200;
        if (argc > 3) g_shotW = _wtoi(wargv[3]);
        if (argc > 4) g_shotH = _wtoi(wargv[4]);
        LocalFree(wargv);
        return RunGui();
    }
    g_selfTest = wargv && argc > 1 && WideToUtf8(wargv[1]) == "--selftest";
    if (g_selfTest) {
        LocalFree(wargv);
        return RunGui();
    }
#endif
    if (wargv && argc > 1) {
        // 有参数 -> CLI 模式，需要把 stdout 接回父控制台
        std::vector<std::string> args;
        args.reserve(argc);
        for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(wargv[i]));
        LocalFree(wargv);
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return RunCli(static_cast<int>(argv.size()), argv.data());
    }
    if (wargv) LocalFree(wargv);
    return RunGui();
}
