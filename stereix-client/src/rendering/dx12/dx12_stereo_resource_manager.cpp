#include "rendering/dx12/dx12_stereo_resource_manager.h"
#include "rendering/dx12/dx12_lifecycle_manager.h"
#include "core/subsystem_context.h"
#include "core/config_manager.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace vrinject {

DX12StereoResourceManager::~DX12StereoResourceManager() {
    Shutdown();
}

bool DX12StereoResourceManager::Initialize(ID3D12Device* device, DX12FenceManager* fenceManager, DX12PipelineStateCache* psoCache) {
    if (!device || !fenceManager || !psoCache) return false;

    m_device = device;
    m_fenceManager = fenceManager;
    m_psoCache = psoCache;

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateDescriptorHeaps()) return false;
    if (!CreateConstantBuffer()) return false;

    return true;
}

void DX12StereoResourceManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // We get the last signaled fence value and defer our releases
    uint64_t fenceValue = m_fenceManager ? m_fenceManager->GetLastSignaledValue() : 0;
    
    if (m_fenceManager) {
        if (m_leftEyeTexture) m_fenceManager->DeferRelease(m_leftEyeTexture, fenceValue);
        if (m_rightEyeTexture) m_fenceManager->DeferRelease(m_rightEyeTexture, fenceValue);
        if (m_constantBuffer) {
            if (m_mappedConstantBuffer) {
                m_constantBuffer->Unmap(0, nullptr);
                m_mappedConstantBuffer = nullptr;
            }
            m_fenceManager->DeferRelease(m_constantBuffer, fenceValue);
        }
        if (m_cbvSrvUavHeap) m_fenceManager->DeferRelease(m_cbvSrvUavHeap, fenceValue);
    }
    
    m_leftEyeTexture.Reset();
    m_rightEyeTexture.Reset();
    m_constantBuffer.Reset();
    m_cbvSrvUavHeap.Reset();
}

void DX12StereoResourceManager::SetTargetFormat(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_UNKNOWN || m_targetFormat == format) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_targetFormat = format;
    if (m_leftEyeTexture || m_rightEyeTexture) {
        if (m_fenceManager) {
            uint64_t fenceValue = m_fenceManager->GetLastSignaledValue();
            if (m_leftEyeTexture) m_fenceManager->DeferRelease(m_leftEyeTexture, fenceValue);
            if (m_rightEyeTexture) m_fenceManager->DeferRelease(m_rightEyeTexture, fenceValue);
        }
        m_leftEyeTexture.Reset();
        m_rightEyeTexture.Reset();
        if (m_currentWidth > 0 && m_currentHeight > 0) {
            CreateEyeTextures(m_currentWidth, m_currentHeight);
        }
    }
}

void DX12StereoResourceManager::SetTargetDimensions(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (m_targetWidth == width && m_targetHeight == height) return;
    m_targetWidth = width;
    m_targetHeight = height;
    HandleResize(width, height);
}

bool DX12StereoResourceManager::HandleResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == m_currentWidth && height == m_currentHeight) return true;

    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_currentWidth = width;
    m_currentHeight = height;

    // Defer old textures (null-checked m_fenceManager, HIGH-08)
    if (m_fenceManager) {
        uint64_t fenceValue = m_fenceManager->GetLastSignaledValue();
        if (m_leftEyeTexture) m_fenceManager->DeferRelease(m_leftEyeTexture, fenceValue);
        if (m_rightEyeTexture) m_fenceManager->DeferRelease(m_rightEyeTexture, fenceValue);
    }
    m_leftEyeTexture.Reset();
    m_rightEyeTexture.Reset();

    return CreateEyeTextures(width, height);
}

bool DX12StereoResourceManager::CreateEyeTextures(uint32_t width, uint32_t height) {
    if (!m_device || !m_cbvSrvUavHeap) return false;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    DXGI_FORMAT eyeFmt = (m_targetFormat != DXGI_FORMAT_UNKNOWN) ? m_targetFormat : Dx12LifecycleManager::Get().GetFormat();
    if (eyeFmt == DXGI_FORMAT_UNKNOWN) {
        eyeFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = eyeFmt;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, 
            D3D12_RESOURCE_STATE_COMMON, nullptr, 
            IID_PPV_ARGS(&m_leftEyeTexture)))) {
        return false;
    }
    m_leftEyeTexture->SetName(L"NexVR_LeftEyeTexture");

    if (FAILED(m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, 
            D3D12_RESOURCE_STATE_COMMON, nullptr, 
            IID_PPV_ARGS(&m_rightEyeTexture)))) {
        return false;
    }
    m_rightEyeTexture->SetName(L"NexVR_RightEyeTexture");

    // Recreate UAVs across all ring-buffered frame slots (HIGH-08)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else {
        uavDesc.Format = desc.Format;
    }
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = heapStart;
        uavHandle.ptr += static_cast<SIZE_T>(slot * DESCRIPTORS_PER_FRAME + 3) * m_descriptorSize;
        m_device->CreateUnorderedAccessView(m_leftEyeTexture.Get(), nullptr, &uavDesc, uavHandle);
        uavHandle.ptr += m_descriptorSize;
        m_device->CreateUnorderedAccessView(m_rightEyeTexture.Get(), nullptr, &uavDesc, uavHandle);
    }

    return true;
}

bool DX12StereoResourceManager::CreateDescriptorHeaps() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    // 3-frame ring buffer * (1 CBV + 2 SRV + 2 UAV) = 15 descriptors (eliminates in-flight GPU/CPU race, HIGH-08)
    heapDesc.NumDescriptors = MAX_FRAMES_IN_FLIGHT * DESCRIPTORS_PER_FRAME;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvSrvUavHeap)))) {
        return false;
    }
    m_cbvSrvUavHeap->SetName(L"NexVR_StereoDescriptorHeap");
    
    return true;
}

bool DX12StereoResourceManager::CreateConstantBuffer() {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(DX12StereoShaderConstants) * MAX_FRAMES_IN_FLIGHT; // 3 x 512-byte ring-buffered CBV slots (HIGH-08)
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, 
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, 
            IID_PPV_ARGS(&m_constantBuffer)))) {
        return false;
    }

    if (FAILED(m_constantBuffer->Map(0, nullptr, &m_mappedConstantBuffer))) {
        return false;
    }

    // Create ring-buffered CBVs (one 512-byte slot per frame in flight)
    const D3D12_GPU_VIRTUAL_ADDRESS gpuBase = m_constantBuffer->GetGPUVirtualAddress();
    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = gpuBase + static_cast<UINT64>(slot) * sizeof(DX12StereoShaderConstants);
        cbvDesc.SizeInBytes = sizeof(DX12StereoShaderConstants);

        D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = heapStart;
        cbvHandle.ptr += static_cast<SIZE_T>(slot * DESCRIPTORS_PER_FRAME) * m_descriptorSize;
        m_device->CreateConstantBufferView(&cbvDesc, cbvHandle);
    }

    return true;
}

void DX12StereoResourceManager::BindDescriptorHeaps(ID3D12GraphicsCommandList* commandList) {
    if (m_cbvSrvUavHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_cbvSrvUavHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
    }
}

#include <DirectXMath.h>

static Matrix4x4 InverseMatrix(const Matrix4x4& m) {
    DirectX::XMMATRIX dxm = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&m));
    DirectX::XMVECTOR det;
    DirectX::XMMATRIX invDxm = DirectX::XMMatrixInverse(&det, dxm);
    
    Matrix4x4 result;
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&result), invDxm);
    return result;
}

static bool SafeGetResourceDesc(ID3D12Resource* res, D3D12_RESOURCE_DESC& outDesc) {
    if (!res) return false;
    __try {
        outDesc = res->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCheckDepthReadable(ID3D12Resource* res) {
    if (!res) return false;
    D3D12_RESOURCE_DESC dDesc = {};
    if (!SafeGetResourceDesc(res, dDesc)) return false;
    bool denyShader = (dDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
    bool isTypelessOrFloat = (
        dDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
        dDesc.Format == DXGI_FORMAT_R32_TYPELESS ||
        dDesc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
        dDesc.Format == DXGI_FORMAT_R16_TYPELESS ||
        dDesc.Format == DXGI_FORMAT_R32_FLOAT ||
        dDesc.Format == DXGI_FORMAT_R16_UNORM
    );
    return (!denyShader && isTypelessOrFloat);
}

void DX12StereoResourceManager::UpdateFrameResources(
    ID3D12Device* device,
    const EyeView& leftEye,
    const EyeView& rightEye,
    const CameraSnapshot& cam,
    const DepthSnapshot& depth,
    bool shouldAttemptStereo,
    uint32_t frameSlot) 
{
    if (!m_device || !m_cbvSrvUavHeap) return;

    const uint32_t slot = frameSlot % MAX_FRAMES_IN_FLIGHT;

    // HandleResize has its own internal locking — call it before acquiring m_mutex
    // to avoid recursive lock deadlock
    uint32_t desiredWidth = m_targetWidth;
    uint32_t desiredHeight = m_targetHeight;

    if (desiredWidth == 0 || desiredHeight == 0) {
        if (cam.IsValid() && cam.resourceIdentity.width > 0 && cam.resourceIdentity.height > 0) {
            desiredWidth = cam.resourceIdentity.width;
            desiredHeight = cam.resourceIdentity.height;
        } else {
            desiredWidth = Dx12LifecycleManager::Get().GetWidth();
            desiredHeight = Dx12LifecycleManager::Get().GetHeight();
        }
    }

    if (desiredWidth > 0 && desiredHeight > 0) {
        if (desiredWidth != m_currentWidth || desiredHeight != m_currentHeight) {
            HandleResize(desiredWidth, desiredHeight);
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeFrameSlot = slot;

    // Update constant buffer data with exact reprojection parameters
    if (m_mappedConstantBuffer) {
        DX12StereoShaderConstants consts{};
        consts.inverseViewProj = InverseMatrix(cam.vp);
        consts.leftViewProj = leftEye.viewProjection;
        consts.rightViewProj = rightEye.viewProjection;
        consts.originalEyePos = cam.position;
        consts.leftEyePos = leftEye.eyePosition;
        consts.rightEyePos = rightEye.eyePosition;
        consts.width = m_currentWidth;
        consts.height = m_currentHeight;
        
        bool isDepthReadable = false;
        if (depth.IsValid() && depth.identity.nativeHandle) {
            isDepthReadable = SafeCheckDepthReadable(reinterpret_cast<ID3D12Resource*>(depth.identity.nativeHandle));
        }
        consts.shouldAttemptStereo = (shouldAttemptStereo && isDepthReadable) ? 1 : 0;
        const auto* cfg = SubsystemContext::Get().GetConfig();
        const auto& c = cfg ? cfg->GetConfig() : VRConfig{};
        consts.contrast = (c.contrast > 0.0f) ? c.contrast : 1.0f;
        consts.saturation = (c.saturation > 0.0f) ? c.saturation : 1.0f;
        consts.brightness = (c.brightness > 0.0f) ? c.brightness : 1.0f;
        consts.srgbCorrection = c.srgbCorrection ? 1 : 0;

        // Camera forward vector from view matrix
        Vector3 forward = { cam.view.m[0][2], cam.view.m[1][2], cam.view.m[2][2] };

        // P0.2: Curved HUD
        consts.curvedHudEnabled = c.curvedHud ? 1 : 0;
        consts.hudDistance = c.hudDistance;
        consts.hudCurvature = c.hudCurvature;

        // P1.2: Comfort vignette (dynamically computed from head yaw velocity)
        if (c.comfortVignette) {
            float yawDeg = atan2f(forward.x, forward.z) * (180.0f / 3.14159265f);
            consts.comfortVignetteRadius = m_vignetteCalculator.Update(yawDeg, c.vignetteOnset, c.vignetteStrength);
            consts.comfortVignetteFeather = c.vignetteStrength;
        } else {
            consts.comfortVignetteRadius = 1.0f; // 1.0 = fully open (no vignette)
            consts.comfortVignetteFeather = 0.0f;
        }

        // P1.1: Theater mode weight (driven by CutsceneDetector)
        if (c.cutsceneTheater) {
            consts.theaterModeWeight = m_cutsceneDetector.Update(forward, c.theaterCutThreshold);
        } else {
            consts.theaterModeWeight = 0.0f;
        }
        consts.theaterDistance = c.theaterDistance;

        // P2: Horizon lock (computed by roll correction smoothing)
        if (c.horizonLock) {
            consts.horizonRollCorrection = m_horizonLock.ComputeRollCorrection(cam.view, c.horizonLockStrength);
            consts.horizonLockStrength = c.horizonLockStrength;
        } else {
            consts.horizonRollCorrection = 0.0f;
            consts.horizonLockStrength = 0.0f;
        }

        for (int i = 0; i < 4; ++i) {
            consts.leftEyeFovTan[i] = leftEye.fovTan[i];
            consts.rightEyeFovTan[i] = rightEye.fovTan[i];
        }
        float aspect = (m_currentHeight > 0)
            ? (static_cast<float>(m_currentWidth) / static_cast<float>(m_currentHeight))
            : (16.0f / 9.0f);
        if (aspect < 0.5f || aspect > 4.0f) aspect = 16.0f / 9.0f;

        float gameTanX = 1.0f;
        float gameTanY = 1.0f / aspect;
        float p00 = std::abs(cam.projection.m[0][0]);
        float p11 = std::abs(cam.projection.m[1][1]);
        if (p00 > 0.25f && p00 < 5.0f && p11 > 0.25f && p11 < 5.0f) {
            gameTanX = (std::min)(1.85f, (std::max)(0.45f, 1.0f / p00));
            gameTanY = (std::min)(1.40f, (std::max)(0.25f, 1.0f / p11));
        }
        consts.gameTanHalfFovX = gameTanX;
        consts.gameTanHalfFovY = gameTanY;
        consts.stereoIpd = (c.ipd > 0.01f) ? c.ipd : 0.064f;
        consts.stereoConvergence = (c.convergence > 0.1f) ? c.convergence : 3.5f;
        consts.isReversedZ = cam.reversedZ ? 1u : 0u;
        consts.isLeftHanded = cam.isLeftHanded ? 1u : 0u;
        consts.nearPlane = 0.1f;
        consts.farPlane = 1000.0f;

        auto* dstSlotPtr = static_cast<uint8_t*>(m_mappedConstantBuffer) + static_cast<size_t>(slot) * sizeof(DX12StereoShaderConstants);
        memcpy(dstSlotPtr, &consts, sizeof(DX12StereoShaderConstants));
    }

    UpdateViews(cam, depth, slot);
}

void DX12StereoResourceManager::UpdateViews(const CameraSnapshot& cam, const DepthSnapshot& depth, uint32_t frameSlot) {
    if (!m_device || !m_cbvSrvUavHeap) return;

    // Offset to SRV section for the active ring-buffered frame slot (HIGH-08)
    const uint32_t slot = frameSlot % MAX_FRAMES_IN_FLIGHT;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(slot * DESCRIPTORS_PER_FRAME + 1) * m_descriptorSize; // Skip CBV

    ID3D12Resource* camRes = (cam.IsValid() && cam.resourceIdentity.nativeHandle) ? 
        reinterpret_cast<ID3D12Resource*>(cam.resourceIdentity.nativeHandle) : 
        Dx12LifecycleManager::Get().GetBackBuffer();

    DXGI_FORMAT colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (camRes) {
        D3D12_RESOURCE_DESC resDesc = {};
        bool camDescOk = false;
        __try {
            resDesc = camRes->GetDesc();
            camDescOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            camDescOk = false;
        }

        if (!camDescOk) {
            camRes = Dx12LifecycleManager::Get().GetBackBuffer();
            if (camRes) {
                __try {
                    resDesc = camRes->GetDesc();
                    camDescOk = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    camDescOk = false;
                }
            }
        }

        if (camDescOk) {
            colorFmt = (cam.IsValid() && cam.resourceIdentity.format != DXGI_FORMAT_UNKNOWN) ? 
                static_cast<DXGI_FORMAT>(cam.resourceIdentity.format) : resDesc.Format;
            if (colorFmt == DXGI_FORMAT_UNKNOWN) {
                colorFmt = Dx12LifecycleManager::Get().GetFormat();
            }
            
            switch (colorFmt) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    colorFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                    colorFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
                    break;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                    colorFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    break;
                case DXGI_FORMAT_R11G11B10_FLOAT:
                    colorFmt = DXGI_FORMAT_R11G11B10_FLOAT;
                    break;
                default:
                    break;
            }
            if (colorFmt == DXGI_FORMAT_UNKNOWN) {
                colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = colorFmt;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = resDesc.MipLevels > 0 ? resDesc.MipLevels : 1;
            __try {
                m_device->CreateShaderResourceView(camRes, &srvDesc, srvHandle);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    
    srvHandle.ptr += m_descriptorSize;

    ID3D12Resource* depthRes = (depth.IsValid() && depth.identity.nativeHandle) ? 
        reinterpret_cast<ID3D12Resource*>(depth.identity.nativeHandle) : 
        camRes;

    if (depthRes) {
        D3D12_RESOURCE_DESC resDesc = {};
        bool depthDescOk = false;
        __try {
            resDesc = depthRes->GetDesc();
            bool denyShader = (resDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
            bool isTypelessOrFloat = (
                resDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                resDesc.Format == DXGI_FORMAT_R32_TYPELESS ||
                resDesc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
                resDesc.Format == DXGI_FORMAT_R16_TYPELESS ||
                resDesc.Format == DXGI_FORMAT_R32_FLOAT ||
                resDesc.Format == DXGI_FORMAT_R16_UNORM
            );
            depthDescOk = (!denyShader && isTypelessOrFloat);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            depthDescOk = false;
        }

        if (!depthDescOk) {
            depthRes = camRes;
            if (camRes) {
                __try {
                    resDesc = camRes->GetDesc();
                    depthDescOk = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    depthDescOk = false;
                }
            }
        }

        if (depthDescOk) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = resDesc.MipLevels > 0 ? resDesc.MipLevels : 1;

            if (depthRes == camRes || !depth.IsValid()) {
                // When no valid depth texture exists, use the valid color format for the fallback SRV
                srvDesc.Format = (colorFmt != DXGI_FORMAT_UNKNOWN) ? colorFmt : resDesc.Format;
                if (srvDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                else if (srvDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            } else {
                DXGI_FORMAT actualFmt = (depth.identity.format != DXGI_FORMAT_UNKNOWN) ? 
                    static_cast<DXGI_FORMAT>(depth.identity.format) : resDesc.Format;
                
                switch (actualFmt) {
                    case DXGI_FORMAT_R32_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT:
                    case DXGI_FORMAT_R32_FLOAT:
                        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                        break;
                    case DXGI_FORMAT_R24G8_TYPELESS:
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
                        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                        break;
                    case DXGI_FORMAT_R32G8X24_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
                    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
                        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                        break;
                    case DXGI_FORMAT_R16_TYPELESS:
                    case DXGI_FORMAT_D16_UNORM:
                    case DXGI_FORMAT_R16_UNORM:
                        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
                        break;
                    default:
                        if (resDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || resDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                            srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                        } else if (resDesc.Format == DXGI_FORMAT_R16_TYPELESS || resDesc.Format == DXGI_FORMAT_D16_UNORM) {
                            srvDesc.Format = DXGI_FORMAT_R16_UNORM;
                        } else if (resDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || resDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
                            srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                        } else {
                            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                        }
                        break;
                }
            }
            __try {
                m_device->CreateShaderResourceView(depthRes, &srvDesc, srvHandle);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // If depth SRV creation fails (e.g. non-typeless D32_FLOAT with DENY_SHADER_RESOURCE),
                // fallback safely to camRes so the descriptor heap entry is always valid
                if (camRes && depthRes != camRes) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC fallbackDesc = {};
                    fallbackDesc.Format = colorFmt;
                    fallbackDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    fallbackDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    fallbackDesc.Texture2D.MipLevels = 1;
                    __try {
                        m_device->CreateShaderResourceView(camRes, &fallbackDesc, srvHandle);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }
        }
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12StereoResourceManager::GetCbvGpuHandle() const {
    if (!m_cbvSrvUavHeap) return {0};
    auto handle = m_cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(m_activeFrameSlot * DESCRIPTORS_PER_FRAME) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12StereoResourceManager::GetSrvGpuHandle() const {
    if (!m_cbvSrvUavHeap) return {0};
    auto handle = m_cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(m_activeFrameSlot * DESCRIPTORS_PER_FRAME + 1) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12StereoResourceManager::GetUavGpuHandle() const {
    if (!m_cbvSrvUavHeap) return {0};
    auto handle = m_cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(m_activeFrameSlot * DESCRIPTORS_PER_FRAME + 3) * m_descriptorSize;
    return handle;
}

} // namespace vrinject
