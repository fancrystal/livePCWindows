#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <winrt/base.h>   // winrt::com_ptr
#include <d3d11.h>
#include <cstdint>

namespace live_assistant {

class GpuCompositor;

// 零拷贝 GPU 预览 Widget：WGL_NV_DX_interop(2) + QOpenGLWidget
//
// 将 GpuCompositor 的 D3D11 Render Target（BGRA）直接映射为 OpenGL 纹理并显示，
// 完全消除 CPU 读回（无 QImage 拷贝，无 glTexImage2D 上传）。
//
// 实现路径：
//   1. 创建一个带 D3D11_RESOURCE_MISC_SHARED 的 "display texture"
//   2. 每帧通过 CopyResource 将 GpuCompositor RT → display texture（GPU Copy）
//   3. 通过 WGL_NV_DX_interop(2) 注册 display texture 为 OpenGL 纹理
//   4. paintGL(): lock → 绘制全屏四边形 → unlock
//
// 使用：
//   auto* widget = new GpuCanvasWidget(parent);
//   widget->set_gpu_compositor(gpu_compositor_ptr);
//   // 每帧结束后调用：
//   widget->request_update();
class GpuCanvasWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    explicit GpuCanvasWidget(QWidget* parent = nullptr);
    ~GpuCanvasWidget() override;

    // 设置 GpuCompositor（可在 initialize 前或后调用）
    void set_gpu_compositor(GpuCompositor* compositor);

    // 通知 Widget 有新帧可显示（线程安全，可从任意线程调用）
    void request_update();

    bool is_interop_available() const { return interop_initialized_; }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    // ── WGL interop 初始化 ────────────────────────────────────────────────────
    bool load_wgl_functions();
    bool check_wgl_extension(const char* name) const;
    bool init_wgl_device();

    // ── 纹理管理 ──────────────────────────────────────────────────────────────
    bool ensure_display_texture(int w, int h);   // 懒创建/重建 display texture
    bool register_display_texture();              // 注册为 GL 纹理（interop）
    void unregister_display_texture();            // 注销并释放 GL 纹理

    // ── GL 资源 ───────────────────────────────────────────────────────────────
    bool create_shader_program();
    bool create_quad_vao();
    void paint_fullscreen_quad();

    // ── 成员 ─────────────────────────────────────────────────────────────────
    GpuCompositor* compositor_ = nullptr;

    // D3D11 display texture（带 SHARED flag，可被 WGL interop 注册）
    winrt::com_ptr<ID3D11Texture2D> display_texture_;
    int display_tex_width_  = 0;
    int display_tex_height_ = 0;

    // OpenGL 资源
    GLuint gl_texture_  = 0;
    GLuint gl_program_  = 0;
    GLuint gl_vao_      = 0;
    GLuint gl_vbo_      = 0;

    // WGL interop 资源
    HANDLE wgl_device_      = nullptr;  // wglDXOpenDeviceNV 返回
    HANDLE wgl_tex_handle_  = nullptr;  // wglDXRegisterObjectNV 返回

    bool interop_initialized_ = false;
    bool interop2_supported_  = false;  // WGL_NV_DX_interop2（无需 SHARED flag）

    // ── WGL 函数指针（运行时从 wglGetProcAddress 加载）──────────────────────
    using PFNDXOPENDEVICE            = HANDLE (WINAPI*)(void*);
    using PFNDXCLOSEDEVICE           = BOOL   (WINAPI*)(HANDLE);
    using PFNDXREGISTEROBJECT        = HANDLE (WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
    using PFNDXUNREGISTEROBJECT      = BOOL   (WINAPI*)(HANDLE, HANDLE);
    using PFNDXLOCKOBJECTS           = BOOL   (WINAPI*)(HANDLE, GLint, HANDLE*);
    using PFNDXUNLOCKOBJECTS         = BOOL   (WINAPI*)(HANDLE, GLint, HANDLE*);
    using PFNDXSETRESOURCESHAREHANDLE = BOOL  (WINAPI*)(void*, HANDLE);
    using PFNWGLGETEXTENSIONSSTRING  = const char* (WINAPI*)(HDC);

    PFNDXOPENDEVICE            pfnDXOpenDeviceNV_             = nullptr;
    PFNDXCLOSEDEVICE           pfnDXCloseDeviceNV_            = nullptr;
    PFNDXREGISTEROBJECT        pfnDXRegisterObjectNV_         = nullptr;
    PFNDXUNREGISTEROBJECT      pfnDXUnregisterObjectNV_       = nullptr;
    PFNDXLOCKOBJECTS           pfnDXLockObjectsNV_            = nullptr;
    PFNDXUNLOCKOBJECTS         pfnDXUnlockObjectsNV_          = nullptr;
    PFNDXSETRESOURCESHAREHANDLE pfnDXSetResourceShareHandleNV_ = nullptr;
    PFNWGLGETEXTENSIONSSTRING  pfnWglGetExtensionsStringARB_  = nullptr;

    // WGL_ACCESS_READ_ONLY_NV = 0x0000
    static constexpr GLenum WGL_ACCESS_READ_ONLY_NV  = 0x0000;
};

} // namespace live_assistant
