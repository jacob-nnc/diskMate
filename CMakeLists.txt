cmake_minimum_required(VERSION 3.16)
project(DiskMate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "DiskMate 仅支持 Windows")
endif()

# 主程序（GUI，Win32 子系统）
add_executable(diskmate WIN32
  src/main.cpp
  src/filelist.cpp
  src/scan_engine.cpp
  src/scanner.cpp
  src/scanner_M.cpp
  src/treemap.cpp
  src/settings.cpp
  src/theme.cpp
  src/jsonlite.cpp
  src/utils.cpp
  app.rc
)
target_compile_definitions(diskmate PRIVATE UNICODE _UNICODE)
target_link_libraries(diskmate PRIVATE comctl32 shell32 gdi32 d2d1)
if(MINGW)
  target_link_options(diskmate PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# 扫描器自检工具（控制台，验证核心扫描逻辑）
add_executable(scan_test
  src/scan_test.cpp
  src/scanner.cpp
  src/utils.cpp
)
target_compile_definitions(scan_test PRIVATE UNICODE _UNICODE)
target_link_libraries(scan_test PRIVATE shell32)
if(MINGW)
  target_link_options(scan_test PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# 树图布局自检工具（控制台，渲染预览 BMP，联测扫描器）
add_executable(treemap_test
  src/treemap_test.cpp
  src/treemap.cpp
  src/scanner.cpp
  src/settings.cpp
  src/theme.cpp
  src/jsonlite.cpp
  src/utils.cpp
)
target_compile_definitions(treemap_test PRIVATE UNICODE _UNICODE)
target_link_libraries(treemap_test PRIVATE gdi32 shell32 d2d1)
if(MINGW)
  target_link_options(treemap_test PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# 新增：WebView2 版本（UI 用 HTML/CSS，C++ 只做扫描与桥接）
# WebView2 SDK 路径（用正斜杠，避免 CMake 把反斜杠当转义）
# 环境变量 DISKMATE_WEBVIEW2_DIR 可覆盖；默认取 NuGet 全局缓存里那个版本。
# 取不到就**只跳过 web 前端**，其它目标照常构建（方便别人 clone 下来就能编译）。
if(DEFINED ENV{DISKMATE_WEBVIEW2_DIR})
  file(TO_CMAKE_PATH "$ENV{DISKMATE_WEBVIEW2_DIR}" WEBVIEW2_DIR)
else()
  file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/1.0.2210.55/build/native" WEBVIEW2_DIR)
endif()
if(EXISTS "${WEBVIEW2_DIR}/include/WebView2.h" AND EXISTS "${WEBVIEW2_DIR}/x64/WebView2Loader.dll.lib")
add_executable(diskmate_web WIN32
  src/webview_host.cpp
  src/scanner.cpp
  src/scanner_M.cpp
  src/utils.cpp
  src/settings.cpp
  src/jsonlite.cpp
  app.rc
)
target_include_directories(diskmate_web PRIVATE "${WEBVIEW2_DIR}/include")
target_link_directories(diskmate_web PRIVATE "${WEBVIEW2_DIR}/x64")
target_compile_definitions(diskmate_web PRIVATE UNICODE _UNICODE)
# 用动态库版（WebView2Loader.dll.lib）：静态库是 MSVC 编译的，MinGW 链接会缺 __security_cookie
# 文件名是 WebView2Loader.dll.lib，-l 规则认不出，用完整路径链接
target_link_libraries(diskmate_web PRIVATE
  "${WEBVIEW2_DIR}/x64/WebView2Loader.dll.lib"
  ole32 oleaut32 shlwapi user32 gdi32 shell32)
if(MINGW)
  target_link_options(diskmate_web PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
# WebView2Loader.dll 必须与 exe 同目录
add_custom_command(TARGET diskmate_web POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WEBVIEW2_DIR}/x64/WebView2Loader.dll"
          "$<TARGET_FILE_DIR:diskmate_web>/WebView2Loader.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/web"
          "$<TARGET_FILE_DIR:diskmate_web>/web")
else()
  message(STATUS "跳过 diskmate_web：没找到 WebView2 SDK（期望 ${WEBVIEW2_DIR}/include/WebView2.h）")
  message(STATUS "  安装方法：nuget install Microsoft.Web.WebView2 -Version 1.0.2210.55 -OutputDirectory <dir>")
  message(STATUS "  或设置环境变量 DISKMATE_WEBVIEW2_DIR 指向 .../microsoft.web.webview2/<版本>/build/native")
endif()

# 扫描导出 JSON（控制台工具）：把 C:\ D:\ 等目录扫成 JSON 写文件
add_executable(dumpjson
  src/dumpjson.cpp
  src/scanner.cpp
  src/utils.cpp
)
target_compile_definitions(dumpjson PRIVATE UNICODE _UNICODE)
target_link_libraries(dumpjson PRIVATE shell32 user32)
if(MINGW)
  target_link_options(dumpjson PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# 递归文件大小列表工具（控制台）
add_executable(sizelist
  src/sizelist.cpp
  src/scanner.cpp
  src/utils.cpp
)
target_compile_definitions(sizelist PRIVATE UNICODE _UNICODE)
target_link_libraries(sizelist PRIVATE shell32 user32)
if(MINGW)
  target_link_options(sizelist PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# ============================================================================
# ImGui 原生前端（迁移骨架）：单进程 / D3D11 / 无脚本引擎
#   复用后端：scan_engine / scanner / scanner_M / treemap / settings / theme / jsonlite / utils
# ============================================================================
add_executable(diskmate_imgui WIN32
  src_imgui/main_imgui.cpp
  src_imgui/treemap_panel.cpp
  src_imgui/settings_modal.cpp
  src_imgui/shell_menu.cpp
  src_imgui/shared_prefs.cpp
  src/scan_engine.cpp
  src/scanner.cpp
  src/scanner_M.cpp
  src/treemap.cpp
  src/settings.cpp
  src/theme.cpp
  src/jsonlite.cpp
  src/utils.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp
  app.rc
)
target_include_directories(diskmate_imgui PRIVATE
  "${CMAKE_SOURCE_DIR}/third_party/imgui"
  "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
  "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(diskmate_imgui PRIVATE UNICODE _UNICODE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
# 注意：不要加 IMGUI_USE_32_BIT_INDEX —— 1.92/1.93 没有这个开关（ImDrawIdx 恒为
# unsigned short）；大网格靠 ImGuiBackendFlags_RendererHasVtxOffset 分段，
# 前端提交顶点时必须按 ≤64k 顶点分块（见 treemap_panel.cpp 的提交循环）。
target_link_libraries(diskmate_imgui PRIVATE d3d11 dxgi d3dcompiler d2d1 dwmapi shell32 gdi32 user32
  ole32 oleaut32 shlwapi comdlg32)
if(MINGW)
  target_link_options(diskmate_imgui PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
