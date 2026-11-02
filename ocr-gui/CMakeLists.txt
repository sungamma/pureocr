cmake_minimum_required(VERSION 3.20)
project(ocrgui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# ---------- 第三方依赖 ----------
set(ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/onnxruntime-win-x64-1.20.1" CACHE PATH "ONNX Runtime 目录")

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.3.9
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_backend STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_backend PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_backend PUBLIC glfw)

# ---------- 主程序 ----------
set(MUPDF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mupdf-1.25.0" CACHE PATH "MuPDF 目录")

set(OCR_SOURCES
  src/main.cpp
  src/cli.cpp
  src/ocr/image.cpp
  src/ocr/engine.cpp
)
if (EXISTS "${MUPDF_DIR}/lib/libmupdf.lib")
  message(STATUS "MuPDF: ${MUPDF_DIR}")
  list(APPEND OCR_SOURCES
    src/pdf/doc.cpp
    src/pdf/textlayer.cpp
    src/pdf/ocr_pages.cpp
  )
else()
  message(WARNING "未找到 ${MUPDF_DIR}/lib/libmupdf.lib，PDF 渲染/文字层功能不可用")
endif()

add_executable(ocrgui WIN32 ${OCR_SOURCES})
target_include_directories(ocrgui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${ORT_DIR}/include
)
target_link_libraries(ocrgui PRIVATE
  imgui_backend
  opengl32
  comdlg32
  ${ORT_DIR}/lib/onnxruntime.lib
)

if (EXISTS "${MUPDF_DIR}/lib/libmupdf.lib")
  target_compile_definitions(ocrgui PRIVATE OCRGUI_HAS_MUPDF=1)
  target_include_directories(ocrgui PRIVATE ${MUPDF_DIR}/include)
  target_link_libraries(ocrgui PRIVATE ${MUPDF_DIR}/lib/libmupdf.lib)
endif()

# 运行期依赖：ORT dll 拷到 exe 旁边
add_custom_command(TARGET ocrgui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${ORT_DIR}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:ocrgui>"
)
