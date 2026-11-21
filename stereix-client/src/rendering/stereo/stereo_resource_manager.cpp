#include "rendering/stereo/stereo_resource_manager.h"
#include "core/logger.h"
#include "core/seh_shield.h"
#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

// Include the compiled shader byte code
// This assumes CMake generated bin/shaders/stereo_reprojection_cs_dx11.h
#include "stereo_reprojection_cs_dx11.h"
#include "asw_shader_cs_dx11.h"

namespace vrinject {

StereoResourceManager::StereoResourceManager(ID3D11Device* device)
    : device_(device) {
}

StereoResourceManager::~StereoResourceManager() {
    ReleaseResources();
}

namespace {

// A depth-stencil texture cannot be read through an SRV in its typed form.
// Map the typed depth format to the typeless format a shadow copy must be
// created with, and to the format the SRV reads it back through.
DXGI_FORMAT TypelessDepthFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:      return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:           return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:         return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:           return DXGI_FORMAT_R16_TYPELESS;
        default:                                 return format;   // already colour-like
    }
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:           return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:         return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:           return DXGI_FORMAT_R16_UNORM;
        default:                                 return format;
    }
}

static bool SafeGetTextureDesc(ID3D11Texture2D* tex, D3D11_TEXTURE2D_DESC* outDesc) {
    if (!tex || !outDesc) return false;
    __try {
        tex->GetDesc(outDesc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const float* GetFallbackDepthRamp() {
    static float s_ramp[256 * 256];
    static bool s_inited = false;
    if (!s_inited) {
        for (uint32_t y = 0; y < 256; ++y) {
            float normY = static_cast<float>(y) / 255.0f;
            float val = 0.95f - 0.70f * normY;
            for (uint32_t x = 0; x < 256; ++x) {
                s_ramp[y * 256 + x] = val;
            }
        }
        s_inited = true;
    }
    return s_ramp;
}

} // namespace

void StereoResourceManager::ReleaseResources() {
    leftEyeTex_.Reset();
    rightEyeTex_.Reset();
    leftEyeUAV_.Reset();
    rightEyeUAV_.Reset();
    reprojectionShader_.Reset();
    aswShader_.Reset();
    constantBuffer_.Reset();
    linearSampler_.Reset();
    gameColorCopy_.Reset();
    gameDepthCopy_.Reset();
    gameColorSRV_.Reset();
    gameDepthSRV_.Reset();
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

bool StereoResourceManager::EnsureOneSourceView(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        DXGI_FORMAT shadowFormat,
        DXGI_FORMAT srvFormat,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& shadow,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
    if (!device_ || !context || !source) return false;
    if (!vrinject::seh::IsValidMemoryPointer(source)) return false;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    __try {
        source->GetDesc(&srcDesc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("StereoResourceManager: SEH intercepted accessing source texture %p (stale pointer).", source);
        return false;
    }

    if (srcDesc.Width == 0 || srcDesc.Height == 0) return false;

    // MSAA sources need ResolveSubresource, not CopyResource. Rather than
    // silently produce a wrong image, decline and let the caller fall back.
    if (srcDesc.SampleDesc.Count > 1) {
        LOG_WARN("StereoResourceManager: source is MSAA x%u, unsupported", srcDesc.SampleDesc.Count);
        return false;
    }

    // Always use shadow copy to avoid creating SRVs directly on the game's swapchain backbuffer,
    // which would hold persistent COM references and cause DXGI_ERROR_INVALID_CALL during ResizeBuffers.

    // Slow path: rebuild the shadow copy if absent or mismatched.
    bool rebuild = !shadow;
    if (shadow) {
        D3D11_TEXTURE2D_DESC have = {};
        shadow->GetDesc(&have);
        rebuild = have.Width != srcDesc.Width || have.Height != srcDesc.Height;
    }

    if (rebuild) {
        shadow.Reset();
        srv.Reset();

        D3D11_TEXTURE2D_DESC desc = srcDesc;
        desc.Format = (shadowFormat == DXGI_FORMAT_UNKNOWN) ? srcDesc.Format : shadowFormat;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &shadow))) {
            LOG_ERROR("StereoResourceManager: shadow texture creation failed (format %u)", desc.Format);
            shadow.Reset();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = (srvFormat == DXGI_FORMAT_UNKNOWN) ? desc.Format : srvFormat;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;

        if (FAILED(device_->CreateShaderResourceView(shadow.Get(), &viewDesc, &srv))) {
            LOG_ERROR("StereoResourceManager: shadow SRV creation failed (format %u)", viewDesc.Format);
            shadow.Reset();
            srv.Reset();
            return false;
        }
    }

    __try {
        context->CopyResource(shadow.Get(), source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("StereoResourceManager: SEH intercepted CopyResource from %p (stale or freed source).", source);
        return false;
    }

    return srv != nullptr;
}

bool StereoResourceManager::EnsureSourceViews(ID3D11DeviceContext* context,
                                              ID3D11Texture2D* gameColor,
                                              ID3D11Texture2D* gameDepth) {
    if (!gameColor) return false;

    const bool colorOk = EnsureOneSourceView(
        context, gameColor,
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
        gameColorCopy_, gameColorSRV_);

    bool depthOk = false;
    if (gameDepth && vrinject::seh::IsValidMemoryPointer(gameDepth)) {
        D3D11_TEXTURE2D_DESC depthDesc = {};
        if (SafeGetTextureDesc(gameDepth, &depthDesc)) {
            depthOk = EnsureOneSourceView(
                context, gameDepth,
                TypelessDepthFormat(depthDesc.Format), DepthSrvFormat(depthDesc.Format),
                gameDepthCopy_, gameDepthSRV_);
        } else {
            LOG_WARN("StereoResourceManager: SEH intercepted GetDesc on gameDepth %p", gameDepth);
        }
    }
    
    // Fallback: If depth buffer is not available or failed to lock safely,
    // generate a synthetic perspective depth ramp so stereo pass can still render with realistic 3D depth
    if (!depthOk) {
        if (!gameDepthCopy_ || !gameDepthSRV_) {
            const uint32_t fbW = 256;
            const uint32_t fbH = 256;
            const float* fbData = GetFallbackDepthRamp();

            D3D11_TEXTURE2D_DESC dummyDesc = {};
            dummyDesc.Width = fbW;
            dummyDesc.Height = fbH;
            dummyDesc.MipLevels = 1;
            dummyDesc.ArraySize = 1;
            dummyDesc.Format = DXGI_FORMAT_R32_FLOAT;
            dummyDesc.SampleDesc.Count = 1;
            dummyDesc.Usage = D3D11_USAGE_DEFAULT;
            dummyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            
            D3D11_SUBRESOURCE_DATA initData = { fbData, fbW * sizeof(float), 0 };
            if (SUCCEEDED(device_->CreateTexture2D(&dummyDesc, &initData, &gameDepthCopy_))) {
                device_->CreateShaderResourceView(gameDepthCopy_.Get(), nullptr, &gameDepthSRV_);
            }
        }
        depthOk = (gameDepthSRV_ != nullptr);
    }

    return colorOk && depthOk;
}

bool StereoResourceManager::Initialize(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!device_) return false;

    // Check if we already have resources with matching dimensions/format
    if (width_ == width && height_ == height && format_ == format && leftEyeTex_) {
        return true;
    }

    ReleaseResources();

    if (!CreateRenderTargets(width, height, format)) {
        LOG_ERROR("StereoResourceManager: Failed to create render targets");
        ReleaseResources();
        return false;
    }

    if (!LoadShaders()) {
        LOG_ERROR("StereoResourceManager: Failed to load stereo reprojection shader");
        ReleaseResources();
        return false;
    }

    if (!CreateConstantBuffer()) {
        LOG_ERROR("StereoResourceManager: Failed to create constant buffer");
        ReleaseResources();
        return false;
    }

    if (!CreateSamplerState()) {
        LOG_ERROR("StereoResourceManager: Failed to create linear sampler state");
        ReleaseResources();
        return false;
    }

    width_ = width;
    height_ = height;
    format_ = format;

    LOG_INFO("[DIAGNOSTICS] StereoResourceManager: Initialized stereo eye buffers %ux%u (Format %u)", width, height, format);
    return true;
}

bool StereoResourceManager::CreateRenderTargets(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    DXGI_FORMAT texFormat = format;
    DXGI_FORMAT uavFormat = format;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        texFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        texFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        texFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        uavFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        uavFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
        texFormat = DXGI_FORMAT_R10G10B10A2_TYPELESS;
        uavFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
        texFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
        uavFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else if (format == DXGI_FORMAT_UNKNOWN) {
        texFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    desc.Format = texFormat;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &leftEyeTex_);
    if (FAILED(hr)) {
        LOG_ERROR("StereoResourceManager: CreateTexture2D for leftEyeTex failed (format %u, hr=0x%X)", texFormat, hr);
        return false;
    }

    hr = device_->CreateTexture2D(&desc, nullptr, &rightEyeTex_);
    if (FAILED(hr)) {
        LOG_ERROR("StereoResourceManager: CreateTexture2D for rightEyeTex failed (format %u, hr=0x%X)", texFormat, hr);
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = uavFormat;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    hr = device_->CreateUnorderedAccessView(leftEyeTex_.Get(), &uavDesc, &leftEyeUAV_);
    if (FAILED(hr)) {
        LOG_ERROR("StereoResourceManager: CreateUnorderedAccessView for leftEyeUAV failed (format %u, hr=0x%X)", uavFormat, hr);
        return false;
    }

    hr = device_->CreateUnorderedAccessView(rightEyeTex_.Get(), &uavDesc, &rightEyeUAV_);
    if (FAILED(hr)) {
        LOG_ERROR("StereoResourceManager: CreateUnorderedAccessView for rightEyeUAV failed (format %u, hr=0x%X)", uavFormat, hr);
        return false;
    }

    return true;
}

bool StereoResourceManager::LoadShaders() {
    HRESULT hr = device_->CreateComputeShader(g_stereo_reprojection_DX11, sizeof(g_stereo_reprojection_DX11), nullptr, &reprojectionShader_);
    if (FAILED(hr)) {
        LOG_WARN("StereoResourceManager: Precompiled compute shader failed (hr=0x%X). Attempting dynamic HLSL compile fallback.", hr);
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        hr = D3DCompileFromFile(L"shaders/stereo_reprojection.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (SUCCEEDED(hr) && blob) {
            hr = device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &reprojectionShader_);
        }
        if (FAILED(hr)) return false;
    }
    hr = device_->CreateComputeShader(g_asw_shader_DX11, sizeof(g_asw_shader_DX11), nullptr, &aswShader_);
    return SUCCEEDED(hr);
}

bool StereoResourceManager::CreateConstantBuffer() {
    // Needs to be 16-byte aligned. 
    // In HLSL, the constant buffer will match StereoFrameContext fields we care about.
    // Let's make it 256 bytes for safety and flexibility.
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = 512; // Expanded for P0.2/P1.1/P1.2/P2 features (two 256-byte aligned chunks)
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbDesc.MiscFlags = 0;
    cbDesc.StructureByteStride = 0;

    HRESULT hr = device_->CreateBuffer(&cbDesc, nullptr, &constantBuffer_);
    return SUCCEEDED(hr);
}

bool StereoResourceManager::CreateSamplerState() {
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device_->CreateSamplerState(&desc, &linearSampler_);
    if (FAILED(hr)) {
        LOG_ERROR("StereoResourceManager: CreateSamplerState failed (hr=0x%X)", hr);
        return false;
    }
    return true;
}

} // namespace vrinject
