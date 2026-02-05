cmake_minimum_required(VERSION 3.16)

project(LiveAssistant VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ensure MSVC is used with standard library include paths.
# (Do not force -std:c++17 in CMAKE_CXX_FLAGS; CMAKE_CXX_STANDARD handles it.)

# MSVC specific settings to ensure standard library includes are found
if(MSVC)
    # Ensure we're using the correct MSVC runtime
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus")
endif()


# Windows specific defines for WinRT and Windows Graphics Capture
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)  # Windows 10
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    # Enable C++/WinRT
    add_definitions(-DCPPWINRT_VERSION="2.0.230207.1")
endif()

# Find Qt dependencies (add Qml/Quick for QML login, Multimedia for audio capture)
find_package(Qt6 COMPONENTS Core Widgets Qml Quick QuickWidgets WebSockets Multimedia)
qt_standard_project_setup()

# Third-party library paths
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(FFMPEG_DIR ${THIRD_PARTY_DIR}/ffmpeg)
set(OPENCV_DIR ${THIRD_PARTY_DIR}/opencv)
set(LIBCURL_DIR ${THIRD_PARTY_DIR}/libcurl)

# Include directories
include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${FFMPEG_DIR}/include)
include_directories(${OPENCV_DIR}/include)  # Parent directory containing opencv2
include_directories(${LIBCURL_DIR}/include)  # libcurl include directory

# Source files
file(GLOB_RECURSE APP_SOURCES "src/app/*.cpp")
file(GLOB_RECURSE APP_HEADERS "include/app/*.h")
list(APPEND APP_SOURCES 
    src/app/add_material_dialog.cpp 
    src/app/scene_item_row.cpp
    src/app/login_worker.cpp
    src/app/config.cpp
    src/app/encryption_utils.cpp
    src/app/login_service.cpp
)
file(GLOB_RECURSE COMMON_SOURCES "src/common/*.cpp")
file(GLOB_RECURSE COMMON_HEADERS "include/common/*.h")
file(GLOB_RECURSE SCENE_MANAGER_SOURCES
    "src/scene_manager/*.cpp"
)
file(GLOB_RECURSE SCENE_MANAGER_HEADERS "include/scene_manager/*.h")
file(GLOB_RECURSE VIDEO_ENGINE_SOURCES "src/video_engine/*.cpp")
file(GLOB_RECURSE AUDIO_ENGINE_SOURCES "src/audio_engine/*.cpp")
file(GLOB_RECURSE AUDIO_ENGINE_HEADERS "include/audio_engine/*.h")
file(GLOB_RECURSE ENCODER_SOURCES "src/encoder/*.cpp")
file(GLOB_RECURSE ENCODER_HEADERS "include/encoder/*.h")
file(GLOB_RECURSE STREAM_PUSHER_SOURCES "src/stream_pusher/*.cpp")
file(GLOB_RECURSE STREAM_PUSHER_HEADERS "include/stream_pusher/*.h")

# HTTP module sources
set(HTTP_SOURCES 
    "src/http/network_manager.cpp"
    "src/http/http_client.cpp"
    "src/http/client_service.cpp"
)
set(HTTP_HEADERS 
    "include/http/network_manager.h"
    "include/http/http_client.h"
    "include/http/client_service.h"
)

# UI files - let Qt handle them automatically
set(APP_UIS "src/app/main_window.ui" "src/app/live_list_window.ui" "src/app/camera_settings.ui" "src/app/screen_share.ui" "src/app/settings_dialog.ui" "src/app/exit_dialog.ui")

# Add QML and image resources (resources/qml.qrc)
qt_add_resources(RCC_QML_SOURCES resources/qml.qrc)
    
set(ALL_SOURCES
    ${APP_SOURCES}
    ${COMMON_SOURCES}
    ${HTTP_SOURCES}
    ${SCENE_MANAGER_SOURCES}
    ${VIDEO_ENGINE_SOURCES}
    ${AUDIO_ENGINE_SOURCES}
    ${ENCODER_SOURCES}
    ${STREAM_PUSHER_SOURCES}
)
list(APPEND ALL_SOURCES ${RCC_QML_SOURCES})

set(ALL_HEADERS
    ${APP_HEADERS}
    ${COMMON_HEADERS}
    ${HTTP_HEADERS}
    ${SCENE_MANAGER_HEADERS}
    ${AUDIO_ENGINE_HEADERS}
    ${ENCODER_HEADERS}
    ${STREAM_PUSHER_HEADERS}
)

# Create executable with AUTOMOC enabled (handles Q_OBJECT automatically)
qt_add_executable(LiveAssistant ${ALL_SOURCES} ${ALL_HEADERS} ${APP_UIS})

# Link libraries
target_link_libraries(LiveAssistant PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::WebSockets
    Qt6::Multimedia
    # FFmpeg libraries
    ${FFMPEG_DIR}/lib/x64/avformat.lib
    ${FFMPEG_DIR}/lib/x64/avcodec.lib
    ${FFMPEG_DIR}/lib/x64/swscale.lib
    ${FFMPEG_DIR}/lib/x64/avdevice.lib
    ${FFMPEG_DIR}/lib/x64/avutil.lib
    ${FFMPEG_DIR}/lib/x64/avfilter.lib
    ${FFMPEG_DIR}/lib/x64/swresample.lib
    # OpenCV libraries - using world library which includes all modules
    # Use Debug version for Debug build, Release version for Release build
    optimized ${OPENCV_DIR}/lib/x64/opencv_world4110.lib
    debug ${OPENCV_DIR}/lib/x64/opencv_world4110d.lib
    # libcurl library
    ${LIBCURL_DIR}/lib/x64/libcurl.lib
)
# Optionally link GraphicalEffects if available
if (TARGET Qt6::GraphicalEffects)
    target_link_libraries(LiveAssistant PRIVATE Qt6::GraphicalEffects)
    target_compile_definitions(LiveAssistant PRIVATE HAVE_QT_GRAPHICALEFFECTS=1)
endif()

# Windows specific libraries for WASAPI and screen capture
if(WIN32)
    target_link_libraries(LiveAssistant PRIVATE
        ole32.lib
        oleaut32.lib
        avrt.lib
        # DirectX and Windows Graphics Capture libraries
        d3d11.lib
        dxgi.lib
        dwmapi.lib
        windowsapp.lib
    )
endif()

# Let Qt handle the UI files automatically
set_target_properties(LiveAssistant PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    WIN32_EXECUTABLE ON
    MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
)

# Copy Qt DLLs to output directory (for Windows)
if(WIN32)
    qt_generate_deploy_app_script(
        TARGET LiveAssistant
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
    
    # Copy FFmpeg DLLs to output directory
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/lib/x64/*.dll")
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS}
        $<TARGET_FILE_DIR:LiveAssistant>
        COMMENT "Copying FFmpeg DLLs to output directory"
    )
    
    # Copy OpenCV DLLs to output directory
    # Use Debug version for Debug build, Release version for Release build
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${OPENCV_DIR}/lib/x64/opencv_world4110d.dll>
        $<$<CONFIG:Release>:${OPENCV_DIR}/lib/x64/opencv_world4110.dll>
        $<TARGET_FILE_DIR:LiveAssistant>
        COMMENT "Copying OpenCV DLLs to output directory"
    )
    
    # Copy libcurl DLLs to output directory
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBCURL_DIR}/lib/x64/libcurl.dll
        $<TARGET_FILE_DIR:LiveAssistant>
        COMMENT "Copying libcurl DLL to output directory"
    )
    
    # Copy cacert.pem to output directory
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:LiveAssistant>/resources/
        COMMENT "Creating resources directory"
    )
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/cacert.pem
        $<TARGET_FILE_DIR:LiveAssistant>/resources/
        COMMENT "Copying cacert.pem to output directory"
    )
endif()

# Suppress Vulkan warning
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "Suppress developer warnings")
