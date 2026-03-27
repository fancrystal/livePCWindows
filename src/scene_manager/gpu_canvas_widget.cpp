#define NOMINMAX
#include "scene_manager/gpu_canvas_widget.h"
#include "scene_manager/gpu_compositor.h"
#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"

#include <dxgi.h>   // IDXGIResource::GetSharedHandle
#include <QOpenGLShaderProgram>
#include <QMetaObject>

// Windows WGL header (provides wglGetCurrentDC / wglGetProcAddress)
#include <wingdi.h>
#include <GL/gl.h>

namespace live_assistant {

// ── GLSL 着色器 ────────────────────────────────────────────────────────────────
static const char* kVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 pos;
out vec2 uv;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    // D3D11 UV: (0,0)=top-left; OpenGL UV: (0,0)=bottom-left → Y 翻转
    uv = vec2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
}
)";

static const char* kFragSrc = R"(
#version 330 core
uniform sampler2D tex;
in vec2 uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(tex, uv);
    // DXGI_FORMAT_B8G8R8A8_UNORM: D3D11 存储为 BGRA，
    // WGL interop 将其暴露为 GL RGBA，需要交换 R/B
    fragColor = vec4(c.b, c.g, c.r, c.a);
}
)";

// ── 全屏四边形顶点（triangle strip）──────────────────────────────────────────
static const float kQuadVerts[] = {
    -1.f, -1.f,
     1.f, -1.f,
    -1.f,  1.f,
     1.f,  1.f,
};

// ─────────────────────────────────────────────────────────────────────────────

GpuCanvasWidget::GpuCanvasWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    // 强制独立 OpenGL surface（避免与 Qt 的 QOpenGLWidget 共享上下文时的 state 问题）
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(fmt);
}

GpuCanvasWidget::~GpuCanvasWidget()
{
    // 确保在 GL context 存在时清理
    makeCurrent();
    unregister_display_texture();
    if (gl_vao_)     { glDeleteVertexArrays(1, &gl_vao_);  gl_vao_ = 0; }
    if (gl_vbo_)     { glDeleteBuffers(1, &gl_vbo_);       gl_vbo_ = 0; }
    if (gl_program_) { glDeleteProgram(gl_program_);       gl_program_ = 0; }
    if (wgl_device_ && pfnDXCloseDeviceNV_) {
        pfnDXCloseDeviceNV_(wgl_device_);
        wgl_device_ = nullptr;
    }
    doneCurrent();
}

void GpuCanvasWidget::set_gpu_compositor(GpuCompositor* compositor)
{
    compositor_ = compositor;
}

void GpuCanvasWidget::request_update()
{
    // 线程安全：QOpenGLWidget::update() 必须在 GUI 线程调用
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

// ── initializeGL ──────────────────────────────────────────────────────────────
void GpuCanvasWidget::initializeGL()
{
    if (!initializeOpenGLFunctions()) {
        LOG_ERROR("[GpuCanvasWidget] Failed to initialize OpenGL 3.3 Core functions");
        return;
    }

    if (!load_wgl_functions()) {
        LOG_ERROR("[GpuCanvasWidget] Failed to load WGL extension functions — interop unavailable");
        goto build_gl_resources;
    }

    if (!init_wgl_device()) {
        LOG_WARNING("[GpuCanvasWidget] WGL DX interop device init failed");
        goto build_gl_resources;
    }

    interop_initialized_ = true;
    LOG_INFO("[GpuCanvasWidget] WGL_NV_DX_interop" +
             std::string(interop2_supported_ ? "2" : "") + " initialized");

build_gl_resources:
    if (!create_shader_program()) {
        LOG_ERROR("[GpuCanvasWidget] Shader compilation failed");
    }
    if (!create_quad_vao()) {
        LOG_ERROR("[GpuCanvasWidget] VAO creation failed");
    }

    glClearColor(0.f, 0.f, 0.f, 1.f);
}

void GpuCanvasWidget::resizeGL(int /*w*/, int /*h*/)
{
    // viewport 由 Qt 自动设置
}

// ── paintGL ───────────────────────────────────────────────────────────────────
void GpuCanvasWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (!compositor_ || !compositor_->is_initialized()) return;
    if (!gl_program_ || !gl_vao_) return;

    // ── 1. 懒创建 / 重建 display texture ─────────────────────────────────────
    int cw = compositor_->canvas_width();
    int ch = compositor_->canvas_height();
    if (cw != display_tex_width_ || ch != display_tex_height_) {
        if (interop_initialized_) unregister_display_texture();
        if (!ensure_display_texture(cw, ch)) return;
        if (interop_initialized_ && !register_display_texture()) {
            interop_initialized_ = false;  // interop 注册失败，回退仅 CopyResource 路径
        }
    }

    if (!display_texture_) return;

    // ── 2. GPU Copy: GpuCompositor RT → display texture ──────────────────────
    auto& shared = SharedD3D11Device::instance();
    ID3D11Texture2D* src = compositor_->output_texture();
    if (src) {
        shared.context()->CopyResource(display_texture_.get(), src);
    }

    if (!interop_initialized_ || !wgl_tex_handle_) return;

    // ── 3. Lock（D3D11 释放所有权，OpenGL 获取所有权）────────────────────────
    if (!pfnDXLockObjectsNV_(wgl_device_, 1, &wgl_tex_handle_)) {
        LOG_WARNING("[GpuCanvasWidget] wglDXLockObjectsNV failed");
        return;
    }

    // ── 4. 绘制全屏四边形 ────────────────────────────────────────────────────
    paint_fullscreen_quad();

    // ── 5. Unlock（OpenGL 释放所有权，D3D11 可再次写入）─────────────────────
    pfnDXUnlockObjectsNV_(wgl_device_, 1, &wgl_tex_handle_);
}

// ── WGL interop 初始化 ────────────────────────────────────────────────────────

bool GpuCanvasWidget::load_wgl_functions()
{
#define LOAD(fn, name) \
    fn = reinterpret_cast<decltype(fn)>(wglGetProcAddress(name)); \
    if (!fn) { LOG_WARNING("[GpuCanvasWidget] wglGetProcAddress(\"" name "\") returned nullptr"); return false; }

    LOAD(pfnWglGetExtensionsStringARB_, "wglGetExtensionsStringARB");
    LOAD(pfnDXOpenDeviceNV_,            "wglDXOpenDeviceNV");
    LOAD(pfnDXCloseDeviceNV_,           "wglDXCloseDeviceNV");
    LOAD(pfnDXRegisterObjectNV_,        "wglDXRegisterObjectNV");
    LOAD(pfnDXUnregisterObjectNV_,      "wglDXUnregisterObjectNV");
    LOAD(pfnDXLockObjectsNV_,           "wglDXLockObjectsNV");
    LOAD(pfnDXUnlockObjectsNV_,         "wglDXUnlockObjectsNV");
    // wglDXSetResourceShareHandleNV is optional (needed for interop v1 only)
    pfnDXSetResourceShareHandleNV_ =
        reinterpret_cast<PFNDXSETRESOURCESHAREHANDLE>(wglGetProcAddress("wglDXSetResourceShareHandleNV"));
#undef LOAD
    return true;
}

bool GpuCanvasWidget::check_wgl_extension(const char* name) const
{
    if (!pfnWglGetExtensionsStringARB_) return false;
    const char* exts = pfnWglGetExtensionsStringARB_(wglGetCurrentDC());
    if (!exts) return false;
    return std::string(exts).find(name) != std::string::npos;
}

bool GpuCanvasWidget::init_wgl_device()
{
    auto& shared = SharedD3D11Device::instance();
    if (!shared.device()) {
        LOG_ERROR("[GpuCanvasWidget] SharedD3D11Device not available");
        return false;
    }

    interop2_supported_ = check_wgl_extension("WGL_NV_DX_interop2");
    LOG_INFO("[GpuCanvasWidget] WGL_NV_DX_interop2 supported: " +
             std::string(interop2_supported_ ? "yes" : "no"));

    wgl_device_ = pfnDXOpenDeviceNV_(shared.device());
    if (!wgl_device_) {
        LOG_ERROR("[GpuCanvasWidget] wglDXOpenDeviceNV failed");
        return false;
    }
    return true;
}

// ── display texture 管理 ──────────────────────────────────────────────────────

bool GpuCanvasWidget::ensure_display_texture(int w, int h)
{
    auto& shared = SharedD3D11Device::instance();
    if (!shared.device()) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = static_cast<UINT>(w);
    desc.Height         = static_cast<UINT>(h);
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc     = {1, 0};
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // D3D11_RESOURCE_MISC_SHARED 使 WGL_NV_DX_interop v1 可以通过 shared handle 注册
    desc.MiscFlags      = D3D11_RESOURCE_MISC_SHARED;

    display_texture_ = nullptr;
    HRESULT hr = shared.device()->CreateTexture2D(&desc, nullptr, display_texture_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCanvasWidget] CreateTexture2D (display) failed: hr=" + std::to_string(hr));
        return false;
    }
    display_tex_width_  = w;
    display_tex_height_ = h;
    LOG_INFO("[GpuCanvasWidget] Display texture created: " +
             std::to_string(w) + "x" + std::to_string(h));
    return true;
}

bool GpuCanvasWidget::register_display_texture()
{
    if (!display_texture_ || !wgl_device_) return false;

    // ── 创建 OpenGL 纹理对象 ────────────────────────────────────────────────
    if (!gl_texture_) glGenTextures(1, &gl_texture_);

    // ── 如果 interop2 不支持，走 v1 路径：需要 shared handle ────────────────
    if (!interop2_supported_) {
        if (!pfnDXSetResourceShareHandleNV_) {
            LOG_ERROR("[GpuCanvasWidget] WGL_NV_DX_interop v1 requires wglDXSetResourceShareHandleNV, not found");
            return false;
        }
        // 获取 D3D11 纹理的 shared handle（通过 IDXGIResource）
        winrt::com_ptr<IDXGIResource> dxgi_res;
        HRESULT hr = display_texture_->QueryInterface(
            __uuidof(IDXGIResource),
            reinterpret_cast<void**>(dxgi_res.put()));
        if (FAILED(hr)) {
            LOG_ERROR("[GpuCanvasWidget] QueryInterface(IDXGIResource) failed: hr=" + std::to_string(hr));
            return false;
        }
        HANDLE share_handle = nullptr;
        hr = dxgi_res->GetSharedHandle(&share_handle);
        if (FAILED(hr) || !share_handle) {
            LOG_ERROR("[GpuCanvasWidget] GetSharedHandle failed: hr=" + std::to_string(hr));
            return false;
        }
        // 向 WGL 注册 shared handle
        if (!pfnDXSetResourceShareHandleNV_(display_texture_.get(), share_handle)) {
            LOG_ERROR("[GpuCanvasWidget] wglDXSetResourceShareHandleNV failed");
            return false;
        }
    }

    // ── 注册 D3D11 纹理为 OpenGL 纹理 ────────────────────────────────────────
    wgl_tex_handle_ = pfnDXRegisterObjectNV_(
        wgl_device_,
        display_texture_.get(),
        gl_texture_,
        GL_TEXTURE_2D,
        WGL_ACCESS_READ_ONLY_NV);

    if (!wgl_tex_handle_) {
        LOG_ERROR("[GpuCanvasWidget] wglDXRegisterObjectNV failed");
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
        return false;
    }
    LOG_INFO("[GpuCanvasWidget] D3D11 display texture registered as GL texture (interop" +
             std::string(interop2_supported_ ? "2" : "v1") + ")");
    return true;
}

void GpuCanvasWidget::unregister_display_texture()
{
    if (wgl_tex_handle_ && wgl_device_ && pfnDXUnregisterObjectNV_) {
        pfnDXUnregisterObjectNV_(wgl_device_, wgl_tex_handle_);
        wgl_tex_handle_ = nullptr;
    }
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
    display_texture_ = nullptr;
    display_tex_width_ = display_tex_height_ = 0;
}

// ── GL 资源创建 ───────────────────────────────────────────────────────────────

bool GpuCanvasWidget::create_shader_program()
{
    // 手动编译，避免 QOpenGLShaderProgram 引入额外依赖
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(shader, 512, nullptr, log);
            LOG_ERROR("[GpuCanvasWidget] Shader compile error: " + std::string(log));
            glDeleteShader(shader); return 0;
        }
        return shader;
    };

    GLuint vs = compile(GL_VERTEX_SHADER,   kVertSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragSrc);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }

    gl_program_ = glCreateProgram();
    glAttachShader(gl_program_, vs);
    glAttachShader(gl_program_, fs);
    glLinkProgram(gl_program_);
    glDeleteShader(vs); glDeleteShader(fs);

    GLint ok = 0; glGetProgramiv(gl_program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(gl_program_, 512, nullptr, log);
        LOG_ERROR("[GpuCanvasWidget] Program link error: " + std::string(log));
        glDeleteProgram(gl_program_); gl_program_ = 0; return false;
    }
    return true;
}

bool GpuCanvasWidget::create_quad_vao()
{
    glGenVertexArrays(1, &gl_vao_);
    glGenBuffers(1, &gl_vbo_);
    glBindVertexArray(gl_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return true;
}

void GpuCanvasWidget::paint_fullscreen_quad()
{
    glUseProgram(gl_program_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(gl_program_, "tex"), 0);

    glBindVertexArray(gl_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

} // namespace live_assistant
