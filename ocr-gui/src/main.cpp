// ocr-gui：无参数启动 GUI；有参数走 CLI（--pdf / --image ...）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/path_util.h"
#include "cli.h"
#ifdef OCRGUI_HAS_MUPDF
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
bool g_resume = false;  // 断点续跑
int g_presetIdx = -1;  // -1 = 未初始化，默认选「自动探测」
bool g_mergeRow = true;  // 合并同一行的碎片

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
    return util::InsertBeforeExtension(input, "_可复制文字");
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

void StartOcr() {
#ifdef OCRGUI_HAS_MUPDF
    if (g_running) return;
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

    g_worker = std::thread([=]() {
        pdf::OcrRunOptions opt;
        opt.inputPath = pdf;
        opt.outputPath = out;
        opt.modelDir = modelDir;
        opt.fontPath = "C:\\Windows\\Fonts\\simhei.ttf";
        opt.dpi = dpi;
        opt.threads = threads;
        opt.minScore = minScore;
        opt.maskOriginal = true;
        opt.resume = g_resume;
        const ocr::ModelPreset *mp = CurrentPreset();
        opt.detModel = mp->det;
        opt.recModel = mp->rec;
        opt.clsModel = mp->cls;
        opt.mergeGapRatio = g_mergeRow ? 0.8f : 0.0f;
        opt.pages = pages;

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
        if (!ok) {
            PushLog("失败：" + err);
            g_result = "失败：" + err;
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "完成：%d 页 / %d 行，屏蔽原字体 %d 个，用时 %.1f 秒",
                          stats.pageCount, stats.lineCount, stats.maskedFonts, stats.seconds);
            PushLog(buf);
            g_result = buf;
            if (stats.cachedPages > 0 || stats.ocrPages > 0) {
                char cbuf[160];
                std::snprintf(cbuf, sizeof(cbuf), "断点续跑：缓存命中 %d 页 / 新识别 %d 页",
                              stats.cachedPages, stats.ocrPages);
                PushLog(cbuf);
            }
        }
        g_done = true;
        g_running = false;
    });
#endif
}

std::string FmtDur(double sec) {
    if (sec < 0) sec = 0;
    const int total = static_cast<int>(sec + 0.5);
    if (total < 60) return std::to_string(total) + " 秒";
    const int m = total / 60;
    const int rs = total % 60;
    if (m < 60) return std::to_string(m) + " 分 " + std::to_string(rs) + " 秒";
    return std::to_string(m / 60) + " 时 " + std::to_string(m % 60) + " 分";
}

/// 「识别」页
void DrawOcrTab() {
    if (ImGui::Button("打开 PDF...", ImVec2(140, 0)) && !g_running) OpenPdfDialog();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", g_pdfPath.empty() ? "（未选择文件）" : g_pdfPath.c_str());
    ImGui::TextWrapped("输出：%s", g_outPath.empty() ? "（自动）" : g_outPath.c_str());
    ImGui::SameLine();
    if (!g_running) {
        if (ImGui::SmallButton("另存为...")) SavePdfDialog();
    }
    ImGui::Separator();

    static char modelBuf[512] = "";
    if (modelBuf[0] == '\0' && !g_modelDir.empty()) {
        std::snprintf(modelBuf, sizeof(modelBuf), "%s", g_modelDir.c_str());
    }
    ImGui::SetNextItemWidth(360);
    if (ImGui::InputText("模型目录", modelBuf, sizeof(modelBuf))) g_modelDir = modelBuf;
    if (g_presetIdx < 0) g_presetIdx = DefaultPresetIndex();
    {
        int pc = 0;
        const ocr::ModelPreset *presets = ocr::ModelPresetList(&pc);
        static std::vector<const char *> labels;
        if (static_cast<int>(labels.size()) != pc) {
            labels.clear();
            for (int i = 0; i < pc; ++i) labels.push_back(presets[i].label);
        }
        ImGui::SetNextItemWidth(240);
        ImGui::Combo("模型", &g_presetIdx, labels.data(), pc);
        ImGui::SameLine();
        ImGui::Checkbox("合并同行碎片", &g_mergeRow);
        ImGui::SameLine();
        ImGui::TextDisabled("（换模型会自动让断点缓存失效）");
    }
    ImGui::SetNextItemWidth(360);
    static char pageBuf[128] = "";
    if (ImGui::InputText("页范围", pageBuf, sizeof(pageBuf))) g_pageSpec = pageBuf;
    ImGui::SameLine();
    ImGui::TextDisabled("留空=全部，例：1-10,15");
    ImGui::SliderInt("线程数", &g_threads, 1, 16);
    ImGui::SameLine();
    ImGui::Checkbox("断点续跑", &g_resume);
    ImGui::SameLine();
    ImGui::TextDisabled("中断后重跑会跳过已完成页（同一输出）");
    ImGui::SliderInt("DPI", &g_dpi, 150, 400);
    ImGui::SliderFloat("置信度阈值", &g_minScore, 0.1f, 0.9f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("复核阈值", &g_reviewScore, 0.5f, 0.95f, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("低于此分数标为待复核");
    ImGui::Separator();

    const bool canStart = !g_pdfPath.empty() && !g_running && !g_modelDir.empty();
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("开始识别", ImVec2(140, 0))) StartOcr();
    if (!canStart) ImGui::EndDisabled();

    const int total = g_totalPages.load();
    const int done = g_donePages.load();
    if (g_running && total > 0) {
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - g_startTime)
                                   .count();
        const double eta =
            done > 0 ? elapsed / done * static_cast<double>(total - done) : 0.0;
        char overlay[160];
        std::snprintf(overlay, sizeof(overlay), "%d/%d 页  已用 %s  剩余约 %s", done, total,
                      FmtDur(elapsed).c_str(), done > 0 ? FmtDur(eta).c_str() : "计算中");
        ImGui::SameLine();
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1, 0),
                           overlay);
    } else if (!g_result.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g_result.c_str());
        if (!g_outPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("打开输出目录")) OpenOutputFolder();
            ImGui::SameLine();
            if (ImGui::SmallButton("查看识别框")) g_switchToPreview = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("导出复核 CSV")) ExportReviewCsv();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("日志");
    ImGui::BeginChild("log", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        if (g_running) ImGui::TextUnformatted("……");
    }
    ImGui::EndChild();
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

/// 「预览」页：渲染页面并叠加 OCR 识别框，用来检查文字层有没有错位
void DrawPreviewTab() {
    if (g_pdfPath.empty()) {
        ImGui::TextUnformatted("先在「识别」页打开一个 PDF。");
        return;
    }
    const bool outReady = g_done && !g_outPath.empty() &&
                          std::filesystem::exists(std::filesystem::u8path(g_outPath));
    const std::string target = outReady ? g_outPath : g_pdfPath;

    std::string openErr;
    if (!g_preview.Open(target, &openErr)) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "打开失败：%s", openErr.c_str());
        return;
    }
    const int pageCount = g_preview.PageCount();
    if (pageCount <= 0) {
        ImGui::TextUnformatted("文档没有页面。");
        return;
    }
    if (g_previewPage > pageCount - 1) g_previewPage = pageCount - 1;
    if (g_previewPage < 0) g_previewPage = 0;

    if (outReady) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "预览 OCR 产物：%s", target.c_str());
    } else {
        ImGui::TextWrapped("预览原文件：%s", target.c_str());
    }

    int pageNo = g_previewPage + 1;
    ImGui::SetNextItemWidth(170);
    if (ImGui::SliderInt("页码", &pageNo, 1, pageCount)) g_previewPage = pageNo - 1;
    ImGui::SameLine();
    if (ImGui::SmallButton("上一页")) {
        if (g_previewPage > 0) --g_previewPage;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("下一页")) {
        if (g_previewPage < pageCount - 1) ++g_previewPage;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("预览 DPI", &g_previewDpi, 72.0f, 200.0f, "%.0f");
    ImGui::Checkbox("显示识别框", &g_showBoxes);
    ImGui::SameLine();
    ImGui::Checkbox("只看低置信度", &g_onlyLowConf);

    // 页号 / DPI / 文件变化时才重新渲染
    if (g_texPath != target || g_texPage != g_previewPage ||
        std::fabs(g_texDpi - g_previewDpi) > 0.5f) {
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        std::string rerr;
        if (g_preview.Render(g_previewPage, g_previewDpi, &rgba, &w, &h, &rerr)) {
            UploadTexture(rgba, w, h);
            g_texPath = target;
            g_texPage = g_previewPage;
            g_texDpi = g_previewDpi;
        } else {
            g_texError = rerr;
        }
    }
    if (!g_texError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "渲染失败：%s", g_texError.c_str());
    }

    const pdf::OcrPageLines* pl = nullptr;
    for (const auto& pg : g_pageLines) {
        if (pg.page == g_previewPage) {
            pl = &pg;
            break;
        }
    }
    if (pl && !pl->lines.empty()) {
        int lowCount = 0;
        for (const auto& ln : pl->lines) {
            if (ln.score < g_reviewScore) ++lowCount;
        }
        ImGui::Text("识别行 %d（其中低于 %.2f 的 %d 行）", static_cast<int>(pl->lines.size()),
                    g_reviewScore, lowCount);
    } else if (!g_done) {
        ImGui::TextDisabled("该页暂无识别结果（尚未识别）。");
    }

    ImGui::BeginChild("preview_canvas", ImVec2(0, 0), true);
    if (g_tex && g_texW > 0 && g_texH > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 32.0f) avail.x = 32.0f;
        if (avail.y < 32.0f) avail.y = 32.0f;
        float scale =
            std::min(avail.x / static_cast<float>(g_texW), avail.y / static_cast<float>(g_texH));
        if (scale <= 0.0f) scale = 1.0f;
        const ImVec2 size(g_texW * scale, g_texH * scale);
        ImGui::Image(static_cast<ImTextureID>(g_tex), size);
        if (pl && pl->width > 0 && pl->height > 0 && !pl->lines.empty()) {
            const ImVec2 p0 = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float bx = size.x / static_cast<float>(pl->width);
            const float by = size.y / static_cast<float>(pl->height);
            if (g_showBoxes) {
                for (const auto& ln : pl->lines) {
                    const bool low = ln.score < g_reviewScore;
                    if (g_onlyLowConf && !low) continue;
                    dl->AddRect(ImVec2(p0.x + ln.x0 * bx, p0.y + ln.y0 * by),
                                ImVec2(p0.x + ln.x1 * bx, p0.y + ln.y1 * by),
                                low ? IM_COL32(255, 90, 90, 230) : IM_COL32(70, 220, 130, 180),
                                0.0f, 0, 1.0f);
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
                        ImGui::Text("置信度 %.2f", ln.score);
                        ImGui::TextUnformatted(ln.text.c_str());
                        ImGui::EndTooltip();
                        break;
                    }
                }
            }
        }
    } else {
        ImGui::TextUnformatted("渲染中……");
    }
    ImGui::EndChild();
}
#endif
int RunGui() {
    g_modelDir = ExeDir() + "\\models";
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1100, 760, "PDF OCR", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f, &cfg,
                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon());


    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        static int devFrames = 0;
        if (++devFrames % 60 == 0)        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("PDF OCR", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ImGui::BeginTabBar("main_tabs")) {
            if (ImGui::BeginTabItem("识别")) {
                DrawOcrTab();
                ImGui::EndTabItem();
            }
#ifdef OCRGUI_HAS_MUPDF
            const ImGuiTabItemFlags previewFlags =
                g_switchToPreview ? ImGuiTabItemFlags_SetSelected : 0;
            g_switchToPreview = false;
            if (ImGui::BeginTabItem("预览", nullptr, previewFlags)) {
                DrawPreviewTab();
                ImGui::EndTabItem();
            }
#endif
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (g_running) g_done = false;
    if (g_worker.joinable()) {
        // 关闭窗口时等待后台结束，避免进程被强制结束留下半成品
        for (int i = 0; i < 600 && g_running; ++i) Sleep(100);
        if (g_worker.joinable()) g_worker.detach();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
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
