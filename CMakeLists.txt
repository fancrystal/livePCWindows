cmake_minimum_required(VERSION 3.16)

project(LiveAssistant VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ensure MSVC is used with standard library include paths.
# (Do not force -std:c++17 in CMAKE_CXX_FLAGS; CMAKE_CXX_STANDARD handles it.)

# MSVC specific settings to ensure standard library includes are found
if(MSVC)
    # Ensure we're using the correct MSVC runtime
    # /EHa: 启用 SEH 异常与 C++ 异常的混合处理，使 _set_se_translator 能捕获 D3D11 驱动崩溃
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /EHa")
endif()


# Windows specific defines for WinRT and Windows Graphics Capture
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)  # Windows 10
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    # Enable C++/WinRT
    add_definitions(-DCPPWINRT_VERSION="2.0.230207.1")
endif()

option(LIVEASSISTANT_DEPLOY_QT_ON_BUILD "Run windeployqt after every LiveAssistant build" OFF)

# Find Qt dependencies (add Qml/Quick for QML login, Multimedia for audio capture, MultimediaWidgets for QVideoWidget, PrintSupport for QCustomPlot)
find_package(Qt6 COMPONENTS Core Widgets Qml Quick QuickWidgets WebSockets Multimedia MultimediaWidgets PrintSupport WebEngineWidgets Concurrent OpenGL OpenGLWidgets)
qt_standard_project_setup()

# Third-party library paths
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(FFMPEG_DIR ${THIRD_PARTY_DIR}/ffmpeg)
set(OPENCV_DIR ${THIRD_PARTY_DIR}/opencv)
set(LIBCURL_DIR ${THIRD_PARTY_DIR}/libcurl)
set(VLC_DIR ${THIRD_PARTY_DIR}/vlc)

# Include directories
include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${FFMPEG_DIR}/include)
include_directories(${OPENCV_DIR}/include)  # Parent directory containing opencv2
include_directories(${LIBCURL_DIR}/include)  # libcurl include directory
include_directories(${THIRD_PARTY_DIR}/qcustomplot)  # QCustomPlot for audio visualization
include_directories(${THIRD_PARTY_DIR}/kiss_fft)     # KissFFT for audio spectrum analysis
include_directories(${VLC_DIR}/include)   # VLC include directory

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
list(FILTER ENCODER_SOURCES EXCLUDE REGEX ".*qsv_device\\.cpp$")
list(FILTER ENCODER_SOURCES EXCLUDE REGEX ".*qsv_encoder\\.cpp$")
file(GLOB_RECURSE ENCODER_HEADERS "include/encoder/*.h")
list(FILTER ENCODER_HEADERS EXCLUDE REGEX ".*qsv_device\\.h$")
list(FILTER ENCODER_HEADERS EXCLUDE REGEX ".*qsv_encoder\\.h$")
file(GLOB_RECURSE STREAM_PUSHER_SOURCES "src/stream_pusher/*.cpp")
file(GLOB_RECURSE STREAM_PUSHER_HEADERS "include/stream_pusher/*.h")
file(GLOB_RECURSE MEDIA_PIPELINE_SOURCES "src/media_pipeline/*.cpp")
file(GLOB_RECURSE MEDIA_PIPELINE_HEADERS "include/media_pipeline/*.h")

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
set(APP_UIS "src/app/main_window.ui" "src/app/live_list_window.ui" "src/app/camera_settings.ui" "src/app/screen_share.ui" "src/app/settings_dialog.ui" "src/app/exit_dialog.ui" "src/app/device_check_dialog.ui")

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
    ${MEDIA_PIPELINE_SOURCES}
    ${THIRD_PARTY_DIR}/qcustomplot/qcustomplot.cpp
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
    ${MEDIA_PIPELINE_HEADERS}
)

# Windows 图标资源文件
if(WIN32)
    set(APP_ICON_RESOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
endif()

# Create executable with AUTOMOC enabled (handles Q_OBJECT automatically)
qt_add_executable(LiveAssistant ${ALL_SOURCES} ${ALL_HEADERS} ${APP_UIS} ${APP_ICON_RESOURCE})

# Add KissFFT C source files directly to the main project
# Set source file properties to compile as C
set_source_files_properties(${THIRD_PARTY_DIR}/kiss_fft/kiss_fft.c PROPERTIES LANGUAGE C)
set_source_files_properties(${THIRD_PARTY_DIR}/kiss_fft/kiss_fftr.c PROPERTIES LANGUAGE C)
target_sources(LiveAssistant PRIVATE
    ${THIRD_PARTY_DIR}/kiss_fft/kiss_fft.c
    ${THIRD_PARTY_DIR}/kiss_fft/kiss_fftr.c
)

# Link libraries
target_link_libraries(LiveAssistant PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::WebSockets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::PrintSupport
    Qt6::WebEngineWidgets
    Qt6::Concurrent
    Qt6::OpenGL
    Qt6::OpenGLWidgets
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
    # VLC libraries
    ${VLC_DIR}/lib/x64/libvlc.lib
    ${VLC_DIR}/lib/x64/libvlccore.lib
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
        opengl32.lib
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

    # Deploy Qt runtime files directly into the build output directory only
    # when explicitly requested, so normal edit/build cycles stay quiet.
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    if(WINDEPLOYQT_EXECUTABLE)
        set(LIVEASSISTANT_WINDEPLOYQT_ARGS
            "$<$<CONFIG:Debug>:--debug>"
            "$<$<NOT:$<CONFIG:Debug>>:--release>"
            --no-3danimation
            --no-3dcore
            --no-3dextras
            --no-3dinput
            --no-3dlogic
            --no-3dquick
            --no-3dquickanimation
            --no-3dquickextras
            --no-3dquickinput
            --no-3dquickrender
            --no-3dquickscene2d
            --no-3drender
            --no-quick3d
            --no-quick3dassetimport
            --no-quick3dassetutils
            --no-quick3deffects
            --no-quick3dhelpers
            --no-quick3dhelpersimpl
            --no-quick3diblbaker
            --no-quick3dparticleeffects
            --no-quick3dparticles
            --no-quick3dphysics
            --no-quick3dphysicshelpers
            --no-quick3druntimerender
            --no-quick3dutils
            --dir "$<TARGET_FILE_DIR:LiveAssistant>"
            --qmldir "${CMAKE_SOURCE_DIR}/qml"
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:LiveAssistant>"
        )

        add_custom_target(deploy_qt_runtime
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${LIVEASSISTANT_WINDEPLOYQT_ARGS}
            COMMAND ${CMAKE_COMMAND} -E rm -f
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DAnimation.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DCore.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DExtras.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DInput.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DLogic.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DQuickScene2D.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DRender.dll"
                    "$<TARGET_FILE_DIR:LiveAssistant>/Qt6Quick3DUtils.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    "$<TARGET_FILE_DIR:LiveAssistant>/geometryloaders"
                    "$<TARGET_FILE_DIR:LiveAssistant>/sceneparsers"
                    "$<TARGET_FILE_DIR:LiveAssistant>/renderers"
                    "$<TARGET_FILE_DIR:LiveAssistant>/renderplugins"
            DEPENDS LiveAssistant
            COMMENT "Deploying Qt runtime files with windeployqt"
            COMMAND_EXPAND_LISTS
        )

        if(LIVEASSISTANT_DEPLOY_QT_ON_BUILD)
            add_custom_command(
                TARGET LiveAssistant POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${LIVEASSISTANT_WINDEPLOYQT_ARGS}
                COMMAND ${CMAKE_COMMAND} -E rm -f
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DAnimation.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DCore.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DExtras.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DInput.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DLogic.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DQuickScene2D.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt63DRender.dll"
                        "$<TARGET_FILE_DIR:LiveAssistant>/Qt6Quick3DUtils.dll"
                COMMAND ${CMAKE_COMMAND} -E rm -rf
                        "$<TARGET_FILE_DIR:LiveAssistant>/geometryloaders"
                        "$<TARGET_FILE_DIR:LiveAssistant>/sceneparsers"
                        "$<TARGET_FILE_DIR:LiveAssistant>/renderers"
                        "$<TARGET_FILE_DIR:LiveAssistant>/renderplugins"
                COMMENT "Deploying Qt runtime files with windeployqt"
                COMMAND_EXPAND_LISTS
            )
        endif()
    else()
        message(WARNING "windeployqt not found; Qt runtime files will not be deployed to the build output directory")
    endif()
    
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
    
    # Copy VLC DLLs to output directory
    file(GLOB VLC_DLLS "${VLC_DIR}/lib/x64/*.dll")
    add_custom_command(
        TARGET LiveAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VLC_DLLS}
        $<TARGET_FILE_DIR:LiveAssistant>
        COMMENT "Copying VLC DLLs to output directory"
    )
    
    # Copy VLC plugins directory if exists
    if(EXISTS "${VLC_DIR}/lib/x64/plugins")
        add_custom_command(
            TARGET LiveAssistant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${VLC_DIR}/lib/x64/plugins"
            $<TARGET_FILE_DIR:LiveAssistant>/plugins
            COMMENT "Copying VLC plugins directory"
        )
    endif()
    
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
