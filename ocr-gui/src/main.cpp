// ocr-gui：无参数启动 GUI；带参数走 CLI（--image ...）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"

namespace {

std::string g_pdfPath;

std::string WideToUtf8(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

void OpenPdfDialog() {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"PDF \u6587\u4ef6 (*.pdf)\0*.pdf\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) g_pdfPath = WideToUtf8(file);
}

int RunGui() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1200, 800, "PDF OCR", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
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

    int jobs = 4, dpi = 300;
    float minScore = 0.5f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("PDF OCR");
        if (ImGui::Button("打开 PDF...", ImVec2(140, 0))) OpenPdfDialog();
        ImGui::TextWrapped("%s", g_pdfPath.empty() ? "\uff08\u672a\u9009\u62e9\u6587\u4ef6\uff09" : g_pdfPath.c_str());
        ImGui::Separator();
        ImGui::SliderInt("\u7ebf\u7a0b\u6570", &jobs, 1, 16);
        ImGui::SliderInt("DPI", &dpi, 150, 400);
        ImGui::SliderFloat("\u7f6e\u4fe1\u5ea6\u9608\u503c", &minScore, 0.1f, 0.9f, "%.2f");
        ImGui::Separator();
        if (!g_pdfPath.empty()) {
            if (ImGui::Button("\u5f00\u59cb\u8bc6\u522b", ImVec2(140, 0))) {
                // TODO(M3)：后台线程跑 OCR
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("\u5f00\u59cb\u8bc6\u522b", ImVec2(140, 0));
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("M0 \u9aa8\u67b6\uff1a\u7a97\u53e3\u5c31\u7eea\uff0cOCR \u5f15\u64ce\u5df2\u5c31\u7eea\uff08CLI \u53ef\u7528\uff09");
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
        // 有参数 -> CLI 模式（需要把 stdout 接回父控制台）
        std::vector<std::string> args;
        args.reserve(argc);
        for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(wargv[i]));
        LocalFree(wargv);
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return RunCli(static_cast<int>(argv.size()), argv.data());
    }
    if (wargv) LocalFree(wargv);
    return RunGui();
}
