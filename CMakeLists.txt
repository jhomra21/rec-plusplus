cmake_minimum_required(VERSION 3.22)
project(rec VERSION 0.1.0 LANGUAGES CXX)

add_library(rec STATIC)
target_compile_features(rec PUBLIC cxx_std_20)
target_include_directories(rec PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")
target_include_directories(rec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(APPLE)
    enable_language(OBJCXX)
    target_sources(rec PRIVATE src/platform/macos/recorder.mm src/platform/macos/stream.cpp src/core/rtmp.cpp)
    set_source_files_properties(src/platform/macos/recorder.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(rec PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(rec PUBLIC
        "-framework Foundation" "-framework ScreenCaptureKit" "-framework AVFoundation"
        "-framework CoreMedia" "-framework CoreVideo" "-framework CoreGraphics"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreAudio")
elseif(WIN32)
    target_sources(rec PRIVATE src/platform/windows/recorder.cpp src/platform/windows/stream.cpp src/core/rtmp.cpp)
    target_compile_definitions(rec PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(rec PRIVATE /permissive- /utf-8 /bigobj /EHsc)
    target_link_libraries(rec PUBLIC d3d11 dxgi mfplat mfreadwrite mfuuid ole32 windowsapp ws2_32)
else()
    target_sources(rec PRIVATE src/platform/unsupported.cpp)
endif()

add_executable(rec-cli src/main.cpp)
target_link_libraries(rec-cli PRIVATE rec)
set_target_properties(rec-cli PROPERTIES OUTPUT_NAME rec)
if(MSVC)
    target_compile_options(rec-cli PRIVATE /utf-8)
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
    target_link_libraries(rec-app PRIVATE rec comctl32 shell32 ole32 advapi32)
endif()

install(TARGETS rec rec-cli EXPORT recTargets)
install(DIRECTORY include/ DESTINATION include)
