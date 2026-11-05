cmake_minimum_required(VERSION 3.20)

# 版本号唯一来源：仓库根目录的 VERSION 文件
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OCRGUI_VERSION LIMIT_COUNT 1)
string(STRIP "${OCRGUI_VERSION}" OCRGUI_VERSION)
if (NOT OCRGUI_VERSION)
  message(FATAL_ERROR "VERSION 文件为空或不存在")
endif()

# 0.13.0 -> 0,13,0,0（Windows 资源文件需要 4 段数字）
set(OCRGUI_VERSION_QUAD "${OCRGUI_VERSION}.0")
string(REPLACE "." "," OCRGUI_VERSION_COMMA "${OCRGUI_VERSION_QUAD}")

project(ocrgui VERSION ${OCRGUI_VERSION} LANGUAGES CXX)

message(STATUS "ocrgui version: ${OCRGUI_VERSION}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# ---------- 第三方依赖 ----------
if (WIN32)
  set(ORT_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-win-x64-1.20.1")
  set(ORT_LIB_NAME "onnxruntime.lib")
else()
  set(ORT_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-linux-x64-1.20.1")
  set(ORT_LIB_NAME "libonnxruntime.so")
endif()
set(ORT_DIR "${ORT_DEFAULT_DIR}" CACHE PATH "ONNX Runtime 目录")

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
set(MUPDF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-1.25.0" CACHE PATH "MuPDF 目录")

# MuPDF 内嵌 CJK 字体占 32 MB，slim 库由 tools/slim_mupdf.ps1 生成（仅 Windows）
option(OCR_MUPDF_SLIM "Use libmupdf-slim.lib (embedded CJK fonts stripped, system fonts instead)" ON)
set(OCR_MUPDF_SLIM_ACTIVE OFF)
if (WIN32)
  set(MUPDF_LIB "${MUPDF_DIR}/lib/libmupdf.lib")
  set(MUPDF_SLIM_LIB "${MUPDF_DIR}/lib/libmupdf-slim.lib")
  if (OCR_MUPDF_SLIM AND EXISTS "${MUPDF_SLIM_LIB}")
    set(MUPDF_LIB "${MUPDF_SLIM_LIB}")
    set(OCR_MUPDF_SLIM_ACTIVE ON)
  elseif (OCR_MUPDF_SLIM)
    message(STATUS "libmupdf-slim.lib not found; using full libmupdf.lib (run tools/slim_mupdf.ps1 to save ~32 MB)")
  endif()
else()
  set(MUPDF_LIB "${MUPDF_DIR}/lib/libmupdf.a")
endif()

set(OCR_SOURCES
  src/main.cpp
  src/cli.cpp
  src/ocr/image.cpp
  src/ocr/engine.cpp
  src/util/http_download.cpp
  src/util/http_download_posix.cpp
  src/util/model_registry.cpp
  src/util/model_download.cpp
)
if (EXISTS "${MUPDF_DIR}/lib/libmupdf.lib" OR EXISTS "${MUPDF_DIR}/lib/libmupdf.a")
  message(STATUS "MuPDF: ${MUPDF_DIR}")
  list(APPEND OCR_SOURCES
    src/pdf/doc.cpp
    src/pdf/textlayer.cpp
    src/pdf/output_check.cpp
    src/pdf/ocr_pages.cpp
    src/pdf/preview.cpp
    src/pdf/lines_io.cpp
    src/pdf/system_fonts.cpp
  )
  if (OCR_MUPDF_SLIM_ACTIVE)
    list(APPEND OCR_SOURCES src/pdf/mupdf_font_stubs.cpp)
  endif()
else()
  message(WARNING "未找到 MuPDF 库（${MUPDF_DIR}/lib），PDF 渲染/文字层功能不可用")
endif()

if (WIN32)
  enable_language(RC)
  list(APPEND OCR_SOURCES resources/app.rc)
  add_executable(ocrgui WIN32 ${OCR_SOURCES})
else()
  add_executable(ocrgui ${OCR_SOURCES})
endif()

target_include_directories(ocrgui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/resources
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${ORT_DIR}/include
)

if (WIN32)
  target_link_libraries(ocrgui PRIVATE
    imgui_backend
    opengl32
    comdlg32
    shell32
    ole32
    winhttp
    bcrypt
    ${ORT_DIR}/lib/${ORT_LIB_NAME}
  )
else()
  find_package(OpenGL REQUIRED)
  find_package(CURL REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  target_link_libraries(ocrgui PRIVATE
    imgui_backend
    OpenGL::GL
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads
    ${ORT_DIR}/lib/${ORT_LIB_NAME}
  )
  # 运行时从 ORT 目录拷 so 到可执行文件旁边，并让加载器在该目录查找
  add_custom_command(TARGET ocrgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_DIR}/lib/libonnxruntime.so" "$<TARGET_FILE_DIR:ocrgui>"
    VERBATIM
  )
  set_target_properties(ocrgui PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
  )
  install(TARGETS ocrgui RUNTIME DESTINATION bin)
endif()

if (EXISTS "${MUPDF_DIR}/lib/libmupdf.lib" OR EXISTS "${MUPDF_DIR}/lib/libmupdf.a")
  target_compile_definitions(ocrgui PRIVATE OCRGUI_HAS_MUPDF=1)
  target_include_directories(ocrgui PRIVATE ${MUPDF_DIR}/include)
  target_link_libraries(ocrgui PRIVATE ${MUPDF_LIB})
  if (WIN32)
    target_link_libraries(ocrgui PRIVATE ws2_32 advapi32 shell32 user32 gdi32)
  else()
    # MuPDF 静态库依赖第三方库的静态包
    if (EXISTS "${MUPDF_DIR}/lib/libmupdf-third.a")
      target_link_libraries(ocrgui PRIVATE "${MUPDF_DIR}/lib/libmupdf-third.a")
    endif()
    target_link_libraries(ocrgui PRIVATE m dl)
  endif()
  if (OCR_MUPDF_SLIM_ACTIVE)
    target_compile_definitions(ocrgui PRIVATE OCR_MUPDF_SLIM=1)
  endif()
endif()

if (WIN32)
  # 运行期依赖：ORT dll 拷到 exe 旁边
  add_custom_command(TARGET ocrgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_DIR}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:ocrgui>"
  )
endif()
