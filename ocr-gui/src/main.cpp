// ocr-gui：无参数启动 GUI；有参数走 CLI（--pdf / --image ...）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cli.h"
#ifdef OCRGUI_HAS_MUPDF
#include "pdf/ocr_pages.h"
#endif

namespace {

std::string g_pdfPath;
std::string g_outPath;
std::string g_modelDir;

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
int g_threads = 4;
int g_dpi = 300;

std::string WideToUtf8(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
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
    namespace fs = std::filesystem;
    fs::path p = fs::u8path(input);
    const std::string stem = p.stem().u8string();
    p.replace_filename(stem + u8"_可复制文字" + p.extension().u8string());
    return p.u8string();
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

void StartOcr() {
#ifdef OCRGUI_HAS_MUPDF
    if (g_running) return;
    g_running = true;
    g_done = false;
    g_donePages = 0;
    g_totalPages = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_log.clear();
        g_result.clear();
    }
    const std::string pdf = g_pdfPath;
    const std::string out = g_outPath.empty() ? DefaultOutputPath(pdf) : g_outPath;
    const std::string modelDir = g_modelDir;
    const int threads = g_threads;
    const int dpi = g_dpi;
    const float minScore = g_minScore;

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

        pdf::OcrRunStats stats;
        std::string err;
        PushLog("开始识别：" + pdf);
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
        if (!ok) {
            PushLog("失败：" + err);
            g_result = "失败：" + err;
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "完成：%d 页 / %d 行，屏蔽原字体 %d 个，用时 %.1f 秒",
                          stats.pageCount, stats.lineCount, stats.maskedFonts, stats.seconds);
            PushLog(buf);
            g_result = buf;
        }
        g_done = true;
        g_running = false;
    });
#endif
}

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

    char modelBuf[512];
    std::snprintf(modelBuf, sizeof(modelBuf), "%s", g_modelDir.c_str());

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("PDF OCR");
        if (ImGui::Button("打开 PDF...", ImVec2(140, 0)) && !g_running) OpenPdfDialog();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", g_pdfPath.empty() ? "（未选择文件）" : g_pdfPath.c_str());
        ImGui::TextWrapped("输出：%s", g_outPath.empty() ? "（自动）" : g_outPath.c_str());
        ImGui::Separator();

        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("模型目录", modelBuf, sizeof(modelBuf))) g_modelDir = modelBuf;
        ImGui::SliderInt("线程数", &g_threads, 1, 16);
        ImGui::SliderInt("DPI", &g_dpi, 150, 400);
        ImGui::SliderFloat("置信度阈值", &g_minScore, 0.1f, 0.9f, "%.2f");
        ImGui::Separator();

        const bool canStart = !g_pdfPath.empty() && !g_running && !g_modelDir.empty();
        if (!canStart) ImGui::BeginDisabled();
        if (ImGui::Button("开始识别", ImVec2(140, 0))) StartOcr();
        if (!canStart) ImGui::EndDisabled();

        const int total = g_totalPages.load();
        const int done = g_donePages.load();
        if (g_running && total > 0) {
            ImGui::SameLine();
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1, 0));
        } else if (!g_result.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(g_result.c_str());
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
