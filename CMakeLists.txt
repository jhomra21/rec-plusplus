cmake_minimum_required(VERSION 3.22)
project(rec VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    # Release builds keep symbols, so the stacks of a hung app name its functions.
    add_compile_options($<$<CONFIG:Release>:/Z7>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

add_library(rec STATIC)
target_compile_features(rec PUBLIC cxx_std_20)
target_include_directories(rec PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")
target_include_directories(rec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(APPLE)
    enable_language(OBJCXX)
    target_sources(rec PRIVATE src/platform/macos/recorder.mm src/platform/macos/overlay.mm src/platform/macos/stream.cpp
        src/platform/macos/tls.cpp src/core/net.cpp src/core/rtmp.cpp src/core/replay.cpp)
    set_source_files_properties(src/platform/macos/recorder.mm src/platform/macos/overlay.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(rec PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(rec PUBLIC
        "-framework Foundation" "-framework ScreenCaptureKit" "-framework AVFoundation"
        "-framework CoreMedia" "-framework CoreVideo" "-framework CoreGraphics"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreAudio"
        "-framework CoreImage" "-framework Metal" "-framework MetalPerformanceShaders" "-framework Network" "-framework Security")
elseif(WIN32)
    target_sources(rec PRIVATE src/platform/windows/recorder.cpp src/platform/windows/stream.cpp src/platform/windows/tls.cpp
        src/platform/windows/overlay.cpp src/platform/windows/scaler.cpp
        src/core/mp4.cpp src/core/net.cpp src/core/rtmp.cpp src/core/replay.cpp)
    target_compile_definitions(rec PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(rec PRIVATE /permissive- /utf-8 /bigobj /EHsc)
    target_link_libraries(rec PUBLIC d3d11 dxgi d3dcompiler mf mfplat mfreadwrite mfuuid ole32 windowscodecs windowsapp
        ws2_32 secur32 crypt32 dwmapi)
else()
    target_sources(rec PRIVATE src/platform/unsupported.cpp)
endif()

add_executable(rec-cli src/main.cpp src/cli/options.cpp src/cli/engine.cpp src/cli/local_socket.cpp)
target_include_directories(rec-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rec-cli PRIVATE rec)
set_target_properties(rec-cli PROPERTIES OUTPUT_NAME rec)
if(MSVC)
    target_compile_options(rec-cli PRIVATE /utf-8)
    target_compile_definitions(rec-cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(rec-cli PRIVATE ws2_32)
    # Its import library would otherwise be rec.lib, the engine library's own path, and
    # MSBuild then skipped relinking rec.exe after engine-only changes.
    set_target_properties(rec-cli PROPERTIES ARCHIVE_OUTPUT_NAME rec-cli)
endif()
if(APPLE)
    # Privacy prompts read usage descriptions from the executable's embedded Info.plist.
    target_link_options(rec-cli PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/support/Info.plist")
    set_property(TARGET rec-cli APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/support/Info.plist")
endif()

# The desktop app: native controls on each platform over the shared session layer.
if(WIN32)
    add_executable(rec-app WIN32 src/app/windows/main.cpp src/app/session.cpp support/rec-app.manifest)
    target_include_directories(rec-app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(rec-app PRIVATE UNICODE _UNICODE)
    target_compile_options(rec-app PRIVATE /utf-8 /EHsc /permissive-)
    target_link_libraries(rec-app PRIVATE rec comctl32 shell32 ole32 oleacc advapi32)
elseif(APPLE)
    add_executable(rec-app MACOSX_BUNDLE src/app/macos/main.mm src/app/session.cpp)
    set_source_files_properties(src/app/macos/main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(rec-app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rec-app PRIVATE rec "-framework AppKit" "-framework Carbon" "-framework QuartzCore" "-framework Security")
    set_target_properties(rec-app PROPERTIES OUTPUT_NAME Rec
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/support/RecApp-Info.plist")
    # Sign the whole bundle so privacy permissions apply to Rec.app. macOS ties an ad-hoc
    # grant to the exact build, so every rebuild needs a new approval; a certificate identity
    # keeps the grant across rebuilds.
    set(REC_SIGN_IDENTITY "-" CACHE STRING "codesign identity for Rec.app; '-' signs ad-hoc")
    add_custom_command(TARGET rec-app POST_BUILD
        COMMAND codesign --force --sign "${REC_SIGN_IDENTITY}" --timestamp=none "$<TARGET_FILE_DIR:rec-app>/../.."
        VERBATIM)
endif()

install(TARGETS rec rec-cli EXPORT recTargets)
install(DIRECTORY include/ DESTINATION include)
