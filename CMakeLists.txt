cmake_minimum_required(VERSION 3.20)

project(MasterTerm VERSION 0.1.134 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(MASTERSSH_VCPKG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
list(PREPEND CMAKE_PREFIX_PATH "${MASTERSSH_VCPKG_PREFIX}")
find_package(libssh2 CONFIG REQUIRED)
get_filename_component(MASTERTERM_LIBSSH2_PREFIX
    "${libssh2_DIR}/../../.." ABSOLUTE)
set(MASTERTERM_RUNTIME_DLLS)
foreach(runtime_name
        libssh2.dll libcrypto-3-x64.dll libssl-3-x64.dll zlib.dll z.dll)
    if (EXISTS "${MASTERTERM_LIBSSH2_PREFIX}/bin/${runtime_name}")
        list(APPEND MASTERTERM_RUNTIME_DLLS
            "${MASTERTERM_LIBSSH2_PREFIX}/bin/${runtime_name}")
    endif()
endforeach()

# MasterTerm uses the MSVC STL from the current compiler toolset (mutexes,
# threads and filesystem). Do not rely on a target PC's potentially older
# Visual C++ redistributable: an older MSVCP140.dll can load successfully but
# still crash on the first STL synchronization call. Ship the matching x64
# CRT beside every executable so Windows selects the tested app-local runtime.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)
set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
include(InstallRequiredSystemLibraries)
foreach(runtime_path IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    if (EXISTS "${runtime_path}")
        list(APPEND MASTERTERM_RUNTIME_DLLS "${runtime_path}")
    endif()
endforeach()
list(REMOVE_DUPLICATES MASTERTERM_RUNTIME_DLLS)

set(MASTERTERM_WEBVIEW2_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/package-1.0.4078.44/build/native"
    CACHE PATH "WebView2 SDK native root")
if (NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The WebView2 build of MasterTerm requires Windows and MSVC")
endif()
if (NOT EXISTS "${MASTERTERM_WEBVIEW2_ROOT}/include/WebView2.h"
    OR NOT EXISTS "${MASTERTERM_WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib")
    message(FATAL_ERROR "WebView2 SDK was not found at ${MASTERTERM_WEBVIEW2_ROOT}")
endif()

add_executable(MasterTerm WIN32
    src/main.cpp
    src/SshSession.h
    src/SshSession.cpp
    src/SerialSession.h
    src/SerialSession.cpp
    src/LocalShellSession.h
    src/LocalShellSession.cpp
    src/SshProxyRelay.cpp
    src/SshLibrary.h
    src/SshTunnel.cpp
    src/SshTunnel.h
    src/NativeProcess.h
    src/NativeProcess.cpp
    src/NativeJson.h
    src/NativeJsonDom.h
    src/NativeBase64.h
    src/SftpTransferProtocol.h
    src/SftpTransferProtocol.cpp
    src/WebViewBackend.h
    src/WebViewBackend.cpp
    src/WebViewHost.h
    src/WebViewHost.cpp
    src/WebViewWindow.h
    src/WebViewWindow.cpp
    src/RdpSession.h
    src/RdpSession.cpp
)

# CMake's automatic RC rule can misparse localized MSVC include output when
# generating Ninja files. Compile the icon resource explicitly, then give the
# resulting .res file to link.exe.
find_program(MASTERTERM_RC_COMPILER NAMES rc.exe REQUIRED)
set(MASTERTERM_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/MasterTerm.rc")
set(MASTERTERM_ICON_RES "${CMAKE_CURRENT_BINARY_DIR}/MasterTerm-icon.res")
add_custom_command(
    OUTPUT "${MASTERTERM_ICON_RES}"
    COMMAND "${MASTERTERM_RC_COMPILER}" /nologo
        /fo "${MASTERTERM_ICON_RES}" "${MASTERTERM_ICON_RC}"
    DEPENDS "${MASTERTERM_ICON_RC}"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/MasterTerm.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/MasterTerm.manifest"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/MasterTerm_MasterCore_Logo.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/app.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/vendor/xterm/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/vendor/xterm/xterm.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/vendor/xterm/xterm.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/vendor/xterm-fit/addon-fit.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/vendor/xterm-fit/LICENSE"
    VERBATIM)
add_custom_target(MasterTermIconResource DEPENDS "${MASTERTERM_ICON_RES}")
add_dependencies(MasterTerm MasterTermIconResource)
target_link_libraries(MasterTerm PRIVATE "${MASTERTERM_ICON_RES}")

# Keep this dependency explicit. Some localized MSVC/Ninja environments fail
# to parse /showIncludes output, which can otherwise leave the host compiled
# against an older backend declaration.
set_property(SOURCE
    src/WebViewHost.cpp
    src/WebViewWindow.cpp
    APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WebViewBackend.h")

# The localized MSVC /showIncludes output is not always parsed by Ninja.
# These sources allocate session objects directly, so stale objects compiled
# against an older class layout can under-allocate and corrupt the heap.
set_property(SOURCE
    src/WebViewBackend.cpp
    src/masterterm_cli.cpp
    APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SshSession.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SerialSession.h")

# The same localized include scanning issue can hide changes in the native
# protocol/string headers used by the WebView backend. Keep these direct
# dependencies explicit so protocol changes never reuse an old backend object.
set_property(SOURCE src/WebViewBackend.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeJson.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeJsonDom.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeKnownHosts.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeMetrics.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeConfigFile.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeFile.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NativeString.h")
set_property(SOURCE src/main.cpp src/WebViewWindow.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WebViewWindow.h")
set_property(SOURCE src/WebViewBackend.cpp src/LocalShellSession.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/LocalShellSession.h")

add_executable(MasterTermSftpWorker
    src/sftp_worker.cpp
    src/NativeString.h
    src/SshProxyRelay.cpp
    src/SshProxyRelay.h
    resources/icons/MasterTermSftpWorker.rc)
add_dependencies(MasterTerm MasterTermSftpWorker)
add_executable(MasterTerm-cli
    src/masterterm_cli.cpp
    src/NativeJsonDom.h
    src/SshSession.cpp
    src/SshProxyRelay.cpp
    src/SshProxyRelay.h
    src/SshSession.h
    src/SshLibrary.h)
target_compile_definitions(MasterTerm-cli PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    MASTERTERM_VERSION="${PROJECT_VERSION}")
target_compile_options(MasterTerm-cli PRIVATE /utf-8)
target_compile_definitions(MasterTermSftpWorker PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermSftpWorker PRIVATE /utf-8)
target_link_options(MasterTermSftpWorker PRIVATE /MANIFEST:NO)
add_executable(MasterTermNativeProcessTest EXCLUDE_FROM_ALL
    tests/NativeProcessTest.cpp
    src/NativeProcess.cpp
    src/NativeProcess.h)
target_include_directories(MasterTermNativeProcessTest PRIVATE src)
target_compile_features(MasterTermNativeProcessTest PRIVATE cxx_std_20)
target_compile_definitions(MasterTermNativeProcessTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeProcessTest PRIVATE /utf-8 /UNDEBUG)
add_executable(MasterTermNativeJsonTest EXCLUDE_FROM_ALL
    tests/NativeJsonTest.cpp
    src/NativeJson.h
    src/SftpTransferProtocol.h
    src/SftpTransferProtocol.cpp)
target_include_directories(MasterTermNativeJsonTest PRIVATE src)
target_compile_features(MasterTermNativeJsonTest PRIVATE cxx_std_17)
target_compile_options(MasterTermNativeJsonTest PRIVATE /utf-8 /UNDEBUG)
add_executable(MasterTermNativeFileTest EXCLUDE_FROM_ALL
    tests/NativeFileTest.cpp
    src/NativeConfigFile.h
    src/NativeFile.h)
target_include_directories(MasterTermNativeFileTest PRIVATE src)
target_compile_features(MasterTermNativeFileTest PRIVATE cxx_std_17)
target_compile_definitions(MasterTermNativeFileTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeFileTest PRIVATE /utf-8 /UNDEBUG)
add_executable(MasterTermNativeSerialTest EXCLUDE_FROM_ALL
    tests/NativeSerialSessionTest.cpp
    src/SerialSession.cpp
    src/SerialSession.h)
target_include_directories(MasterTermNativeSerialTest PRIVATE src)
target_compile_features(MasterTermNativeSerialTest PRIVATE cxx_std_20)
target_compile_definitions(MasterTermNativeSerialTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeSerialTest PRIVATE /utf-8 /UNDEBUG)
add_executable(MasterTermNativeMonitorTest EXCLUDE_FROM_ALL
    tests/NativeMonitorTest.cpp
    src/NativeKnownHosts.h
    src/NativeMetrics.h)
target_include_directories(MasterTermNativeMonitorTest PRIVATE src)
target_compile_features(MasterTermNativeMonitorTest PRIVATE cxx_std_17)
target_compile_definitions(MasterTermNativeMonitorTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeMonitorTest PRIVATE /utf-8 /UNDEBUG)
add_executable(MasterTermNativeDiagnosticLogTest EXCLUDE_FROM_ALL
    tests/NativeDiagnosticLogTest.cpp
    src/DiagnosticLog.h
    src/NativeDataDir.h)
target_include_directories(MasterTermNativeDiagnosticLogTest PRIVATE src)
target_compile_features(MasterTermNativeDiagnosticLogTest PRIVATE cxx_std_20)
target_compile_definitions(MasterTermNativeDiagnosticLogTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeDiagnosticLogTest PRIVATE /utf-8 /UNDEBUG)
add_test(NAME MasterTermNativeDiagnosticLogTest
    COMMAND MasterTermNativeDiagnosticLogTest)
set_tests_properties(MasterTermNativeDiagnosticLogTest PROPERTIES
    ENVIRONMENT "MASTERSSH_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/diaglog-test-data")
add_executable(MasterTermNativeCryptoTest EXCLUDE_FROM_ALL
    tests/NativeCryptoTest.cpp
    src/NativeBase64.h
    src/NativeCrypto.h)
target_include_directories(MasterTermNativeCryptoTest PRIVATE src)
target_compile_features(MasterTermNativeCryptoTest PRIVATE cxx_std_20)
target_compile_definitions(MasterTermNativeCryptoTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativeCryptoTest PRIVATE /utf-8 /UNDEBUG)
target_link_libraries(MasterTermNativeCryptoTest PRIVATE crypt32)
add_test(NAME MasterTermNativeCryptoTest
    COMMAND MasterTermNativeCryptoTest)

add_executable(MasterTermNativeHashTest EXCLUDE_FROM_ALL
    tests/NativeHashTest.cpp
    src/NativeHash.h)
target_include_directories(MasterTermNativeHashTest PRIVATE src)
target_compile_features(MasterTermNativeHashTest PRIVATE cxx_std_17)
target_compile_options(MasterTermNativeHashTest PRIVATE /utf-8 /UNDEBUG)
target_link_libraries(MasterTermNativeHashTest PRIVATE bcrypt)

add_executable(MasterTermNativePathTextTest EXCLUDE_FROM_ALL
    tests/NativePathTextTest.cpp
    src/NativeString.h)
target_include_directories(MasterTermNativePathTextTest PRIVATE src)
target_compile_features(MasterTermNativePathTextTest PRIVATE cxx_std_17)
target_compile_definitions(MasterTermNativePathTextTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MasterTermNativePathTextTest PRIVATE /utf-8 /UNDEBUG)
add_test(NAME MasterTermNativePathTextTest
    COMMAND MasterTermNativePathTextTest)

target_compile_features(MasterTerm PRIVATE cxx_std_20)
target_compile_definitions(MasterTerm PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    MASTERTERM_VERSION="${PROJECT_VERSION}"
    MASTERTERM_VERSION_W=L"${PROJECT_VERSION}")
target_compile_options(MasterTerm PRIVATE /utf-8)
target_link_options(MasterTerm PRIVATE /MANIFEST:NO)
target_include_directories(MasterTerm PRIVATE
    "${MASTERTERM_WEBVIEW2_ROOT}/include"
    "${MASTERTERM_WEBVIEW2_ROOT}/include-winrt")
target_link_libraries(MasterTerm PRIVATE
    libssh2::libssh2 ws2_32 winhttp crypt32 bcrypt
    gdiplus
    ole32 shell32 shlwapi oleacc uiautomationcore windowsapp
    "${MASTERTERM_WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib")
file(GLOB_RECURSE MASTERTERM_WEB_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/*")
add_custom_target(MasterTermWebAssets
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/web"
        "${CMAKE_CURRENT_BINARY_DIR}/web"
    DEPENDS ${MASTERTERM_WEB_ASSETS}
    VERBATIM)
add_dependencies(MasterTerm MasterTermWebAssets)
if (MASTERTERM_RUNTIME_DLLS)
    add_custom_command(TARGET MasterTerm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MASTERTERM_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:MasterTerm>"
        COMMAND_EXPAND_LISTS)
endif()

target_link_libraries(MasterTermSftpWorker PRIVATE libssh2::libssh2)
target_link_libraries(MasterTermSftpWorker PRIVATE ws2_32)
target_link_libraries(MasterTerm-cli PRIVATE libssh2::libssh2 ws2_32 advapi32 bcrypt)

# Headless full-screen terminal regression runs the vendored xterm engine
# under Node and feeds vim/top/less-style VT streams (alternate screen,
# resize, scrolling, chunked and malformed input).
find_program(MASTERTERM_NODE_EXECUTABLE NAMES node)
if (MASTERTERM_NODE_EXECUTABLE)
    add_test(NAME MasterTermFullscreenTerminalRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fullscreen-terminal-regression.js")
    add_test(NAME MasterTermSplitLayoutRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/split-layout-regression.js")
add_test(NAME MasterTermRdpSwitchRegression
    COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/rdp-switch-regression.js")
add_test(NAME MasterTermOverlaySshRegression
    COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/overlay-ssh-regression.js")
    add_test(NAME MasterTermProcessLifecycleRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/process-lifecycle-regression.js")
    add_test(NAME MasterTermSidebarAutoHideRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/sidebar-autohide-regression.js")
    add_test(NAME MasterTermLogRedactionCheck
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/log-redaction-check.js")
    add_test(NAME MasterTermCloudSchemaRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/cloud-schema-regression.js")
    add_test(NAME MasterTermCloudDiffRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/cloud-diff-regression.js")
    add_test(NAME MasterTermTransferQueueRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/transfer-queue-regression.js")
    add_test(NAME MasterTermTransferResumeRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/transfer-resume-regression.js")
    add_test(NAME MasterTermTransferConflictRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/transfer-conflict-regression.js")
    add_test(NAME MasterTermTransferCenterRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/transfer-center-regression.js")
    add_test(NAME MasterTermSpecialPathRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/special-path-regression.js")
    add_test(NAME MasterTermRdpQualityRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/rdp-quality-regression.js")
    add_test(NAME MasterTermServerOrderRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/server-order-regression.js")
    add_test(NAME MasterTermConnectionViewRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/connection-view-regression.js")
    add_test(NAME MasterTermFeatureEnhancementsRegression
        COMMAND "${MASTERTERM_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/feature-enhancements-regression.js")

endif()

# Real WebView2 end-to-end test (T1-3): boots MasterTerm with an isolated
# data directory, drives the production UI over CDP (settings dialog, real
# SSH + SFTP, ConPTY, split layout, RDP failure lifecycle, exit) and saves
# screenshots/logs on failure.  Skips with exit 0 when no SSH credentials
# are available so CI machines can run it empty.
find_program(MASTERTERM_POWERSHELL_EXECUTABLE NAMES powershell)
if (MASTERTERM_POWERSHELL_EXECUTABLE)
    add_test(NAME MasterTermWebViewE2E
        COMMAND "${MASTERTERM_POWERSHELL_EXECUTABLE}"
                -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/webview-e2e.ps1"
                -BuildDir "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(MasterTermWebViewE2E PROPERTIES TIMEOUT 600)
endif()
