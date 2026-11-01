// ocr-gui M0：Dear ImGui + GLFW 窗口骨架
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <commdlg.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

static std::string g_pdfPath;

static void OpenPdfDialog() {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"PDF \u6587\u4ef6 (*.pdf)\0*.pdf\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        int need = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(need > 0 ? need - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8.data(), need, nullptr, nullptr);
        g_pdfPath = utf8;
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
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
        ImGui::TextWrapped("%s", g_pdfPath.empty() ? "（未选择文件）" : g_pdfPath.c_str());
        ImGui::Separator();
        ImGui::SliderInt("线程数", &jobs, 1, 16);
        ImGui::SliderInt("DPI", &dpi, 150, 400);
        ImGui::SliderFloat("置信度阈值", &minScore, 0.1f, 0.9f, "%.2f");
        ImGui::Separator();
        if (!g_pdfPath.empty()) {
            if (ImGui::Button("开始识别", ImVec2(140, 0))) {
                // TODO(M3)：后台线程跑 OCR
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("开始识别", ImVec2(140, 0));
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("M0 骨架：窗口已就绪，OCR 引擎与 PDF 渲染待接入");
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
