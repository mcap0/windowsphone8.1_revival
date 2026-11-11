// One-time questions to the phone's GPU stack, answered in the log at the compositor's start (PLAN §75.4): can a
// texture of a Direct3D device of ours (the H.264 decoder's) be a GL texture of ANGLE's, so that pictures decoded on
// the GPU reach the compositor without passing through the CPU?

#include "config.h"

#include "Engine.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <WebCore/TextureMapperGLHeaders.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>

namespace Lumia {

void probeSharedVideoTextures()
{
    EGLDisplay display = eglGetCurrentDisplay();
    std::string extensions = eglQueryString(display, EGL_EXTENSIONS) ? eglQueryString(display, EGL_EXTENSIONS) : "";
    for (size_t at = 0; at < extensions.size(); at += 300)
        log("sonda video: EGL %s", extensions.substr(at, 300).c_str());

    // ANGLE's own device.
    EGLAttrib eglDevice = 0, angleDevice = 0;
    if (eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &eglDevice) && eglDevice
        && eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(eglDevice), EGL_D3D11_DEVICE_ANGLE, &angleDevice) && angleDevice) {
        auto* device = reinterpret_cast<ID3D11Device*>(angleDevice);
        ID3D11VideoDevice* video = nullptr;
        HRESULT hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video));
        log("sonda video: device di ANGLE livello %x, flag %x, ID3D11VideoDevice 0x%08X", static_cast<unsigned>(device->GetFeatureLevel()),
            device->GetCreationFlags(), static_cast<unsigned>(hr));
        if (video)
            video->Release();
    } else
        log("sonda video: device di ANGLE non ottenuto (0x%X)", eglGetError());

    // A device of ours, a texture others may open, and its handle.
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, 3, D3D11_SDK_VERSION, &device, &got, &context);
    if (FAILED(hr)) {
        log("sonda video: D3D11CreateDevice 0x%08X", static_cast<unsigned>(hr));
        return;
    }
    D3D11_TEXTURE2D_DESC desc = { };
    desc.Width = desc.Height = 64;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* texture = nullptr;
    hr = device->CreateTexture2D(&desc, nullptr, &texture);
    HANDLE handle = nullptr;
    HRESULT handleResult = E_FAIL;
    if (texture) {
        IDXGIResource* resource = nullptr;
        if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource)))) {
            handleResult = resource->GetSharedHandle(&handle);
            resource->Release();
        }
        // Blue 64, green 128, red 255: what the GL side must read back.
        ID3D11RenderTargetView* view = nullptr;
        if (SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view))) {
            const float colour[4] = { 1.0f, 128 / 255.0f, 64 / 255.0f, 1.0f };
            context->ClearRenderTargetView(view, colour);
            context->Flush();
            view->Release();
        }
    }
    log("sonda video: device nostro livello %x, texture condivisa 0x%08X, handle 0x%08X %p", static_cast<unsigned>(got), static_cast<unsigned>(hr),
        static_cast<unsigned>(handleResult), handle);

    EGLint configAttributes[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BIND_TO_TEXTURE_RGBA, EGL_TRUE, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttributes, &config, 1, &count) || !count) {
        log("sonda video: nessuna configurazione pbuffer RGBA legabile a texture (0x%X)", eglGetError());
        config = nullptr;
    }
    EGLint surfaceAttributes[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA, EGL_TEXTURE_TARGET, EGL_TEXTURE_2D, EGL_NONE };

    // Way A: ANGLE opens our texture.
    if (config && handle) {
        EGLSurface surface = eglCreatePbufferFromClientBuffer(display, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, handle, config, surfaceAttributes);
        EGLint error = eglGetError();
        if (surface == EGL_NO_SURFACE)
            log("sonda video: A, pbuffer dal nostro handle: no (0x%X)", error);
        else {
            GLuint id = 0, framebuffer = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            EGLBoolean bound = eglBindTexImage(display, surface, EGL_BACK_BUFFER);
            EGLint bindError = eglGetError();
            unsigned char pixel[4] = { };
            GLenum status = 0;
            if (bound) {
                glGenFramebuffers(1, &framebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
                status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status == GL_FRAMEBUFFER_COMPLETE)
                    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                eglReleaseTexImage(display, surface, EGL_BACK_BUFFER);
            }
            log("sonda video: A, pbuffer dal nostro handle: si; legato a texture %d (0x%X), framebuffer 0x%X, pixel letto %u %u %u %u (atteso 255 128 64 255)",
                bound, bindError, status, pixel[0], pixel[1], pixel[2], pixel[3]);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &id);
            eglDestroySurface(display, surface);
        }
    }

    // Way B: our device opens a texture of ANGLE's.
    if (config) {
        EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
        if (surface == EGL_NO_SURFACE)
            log("sonda video: B, pbuffer di ANGLE: no (0x%X)", eglGetError());
        else {
            void* angleHandle = nullptr;
            EGLBoolean asked = eglQuerySurfacePointerANGLE(display, surface, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, &angleHandle);
            EGLint error = eglGetError();
            HRESULT opened = E_FAIL;
            if (angleHandle) {
                ID3D11Texture2D* theirs = nullptr;
                opened = device->OpenSharedResource(angleHandle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&theirs));
                if (theirs)
                    theirs->Release();
            }
            log("sonda video: B, handle del pbuffer di ANGLE: %d (0x%X) %p, aperto dal nostro device 0x%08X", asked, error, angleHandle, static_cast<unsigned>(opened));
            eglDestroySurface(display, surface);
        }
    }
    if (texture)
        texture->Release();
    context->Release();
    device->Release();
}

} // namespace Lumia
