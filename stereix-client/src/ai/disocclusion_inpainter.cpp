#include "ai/disocclusion_inpainter.h"
#include "core/logger.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <onnxruntime_cxx_api.h>

namespace vrinject {
namespace ai {

static void BilinearResizePlanarRGB(
    const float* src, int srcW, int srcH,
    float* dst, int dstW, int dstH)
{
    float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);

    size_t srcPlane = static_cast<size_t>(srcW) * srcH;
    size_t dstPlane = static_cast<size_t>(dstW) * dstH;

    for (int c = 0; c < 3; ++c) {
        const float* srcC = src + c * srcPlane;
        float* dstC = dst + c * dstPlane;

        for (int dy = 0; dy < dstH; ++dy) {
            float sy = (dy + 0.5f) * scaleY - 0.5f;
            int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
            int y1 = std::clamp(y0 + 1, 0, srcH - 1);
            float fy = sy - std::floor(sy);

            for (int dx = 0; dx < dstW; ++dx) {
                float sx = (dx + 0.5f) * scaleX - 0.5f;
                int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
                int x1 = std::clamp(x0 + 1, 0, srcW - 1);
                float fx = sx - std::floor(sx);

                float p00 = srcC[y0 * srcW + x0];
                float p10 = srcC[y0 * srcW + x1];
                float p01 = srcC[y1 * srcW + x0];
                float p11 = srcC[y1 * srcW + x1];

                float top = p00 + fx * (p10 - p00);
                float bottom = p01 + fx * (p11 - p01);
                dstC[dy * dstW + dx] = top + fy * (bottom - top);
            }
        }
    }
}

static void BilinearResizeMask(
    const float* src, int srcW, int srcH,
    float* dst, int dstW, int dstH)
{
    float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);

    for (int dy = 0; dy < dstH; ++dy) {
        float sy = (dy + 0.5f) * scaleY - 0.5f;
        int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
        int y1 = std::clamp(y0 + 1, 0, srcH - 1);
        float fy = sy - std::floor(sy);

        for (int dx = 0; dx < dstW; ++dx) {
            float sx = (dx + 0.5f) * scaleX - 0.5f;
            int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
            int x1 = std::clamp(x0 + 1, 0, srcW - 1);
            float fx = sx - std::floor(sx);

            float p00 = src[y0 * srcW + x0];
            float p10 = src[y0 * srcW + x1];
            float p01 = src[y1 * srcW + x0];
            float p11 = src[y1 * srcW + x1];

            float top = p00 + fx * (p10 - p00);
            float bottom = p01 + fx * (p11 - p01);
            dst[dy * dstW + dx] = top + fy * (bottom - top);
        }
    }
}

static void ApplyFallbackBlend(
    const float* rgbInput,
    const float* maskInput,
    float* rgbOutput,
    int width,
    int height)
{
    size_t planeSize = static_cast<size_t>(width) * height;
    size_t totalElements = 3 * planeSize;

    std::memcpy(rgbOutput, rgbInput, totalElements * sizeof(float));

    if (!maskInput) return;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = y * width + x;
            if (maskInput[idx] > 0.5f) { // Hole detected
                // Search left for nearest valid neighbor pixel to stretch
                int foundIdx = -1;
                for (int i = 1; i < width && (x - i) >= 0; ++i) {
                    size_t checkIdx = y * width + (x - i);
                    if (maskInput[checkIdx] <= 0.5f) {
                        foundIdx = static_cast<int>(checkIdx);
                        break;
                    }
                }
                // If not found to the left, search right
                if (foundIdx < 0) {
                    for (int i = 1; i < width && (x + i) < width; ++i) {
                        size_t checkIdx = y * width + (x + i);
                        if (maskInput[checkIdx] <= 0.5f) {
                            foundIdx = static_cast<int>(checkIdx);
                            break;
                        }
                    }
                }
                // If not found in row, search vertically
                if (foundIdx < 0) {
                    for (int i = 1; i < height && (y - i) >= 0; ++i) {
                        size_t checkIdx = (y - i) * width + x;
                        if (maskInput[checkIdx] <= 0.5f) {
                            foundIdx = static_cast<int>(checkIdx);
                            break;
                        }
                    }
                }
                if (foundIdx >= 0) {
                    rgbOutput[idx] = rgbInput[foundIdx];
                    rgbOutput[planeSize + idx] = rgbInput[planeSize + foundIdx];
                    rgbOutput[2 * planeSize + idx] = rgbInput[2 * planeSize + foundIdx];
                }
            }
        }
    }
}

DisocclusionInpainter& DisocclusionInpainter::GetInstance(bool useDirectML, int deviceId) {
    static DisocclusionInpainter instance(useDirectML, deviceId);
    return instance;
}

DisocclusionInpainter::DisocclusionInpainter(bool useDirectML, int deviceId, const std::wstring& modelPath)
    : NexVR::AI::AiModelLoader(modelPath, useDirectML, deviceId) {
    QueryModelDimensions();
}

void DisocclusionInpainter::QueryModelDimensions() {
    if (!m_session) return;
    try {
        m_inputCount = m_session->GetInputCount();
        auto typeInfo = m_session->GetInputTypeInfo(0);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> shape = tensorInfo.GetShape();
        if (shape.size() >= 4) {
            int64_t c = shape[1];
            int64_t h = shape[2];
            int64_t w = shape[3];
            m_inputChannels = (c > 0) ? static_cast<int>(c) : 4;
            if (h > 0 && w > 0) {
                m_modelInputHeight = static_cast<int>(h);
                m_modelInputWidth = static_cast<int>(w);
                m_hasStaticDimensions = true;
            } else {
                m_modelInputHeight = 270;
                m_modelInputWidth = 480;
                m_hasStaticDimensions = false;
            }
        }
    } catch (...) {
        m_modelInputHeight = 270;
        m_modelInputWidth = 480;
        m_inputChannels = 4;
        m_inputCount = 1;
        m_hasStaticDimensions = true;
    }
}

bool DisocclusionInpainter::Inpaint(
    const float* rgbInput,
    const float* maskInput,
    float* rgbOutput,
    int width,
    int height
) {
    if (!rgbInput || !rgbOutput || width <= 0 || height <= 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);

    size_t planeSize = static_cast<size_t>(width) * height;
    size_t totalElements = 3 * planeSize;

    // Fast CPU/Shader fallback path if ONNX session is uninitialized or in simulated mode
    if (!m_session || m_isSimulated) {
        ApplyFallbackBlend(rgbInput, maskInput, rgbOutput, width, height);
        return true;
    }

    try {
        int inferW = m_hasStaticDimensions ? m_modelInputWidth : width;
        int inferH = m_hasStaticDimensions ? m_modelInputHeight : height;
        size_t inferPlaneSize = static_cast<size_t>(inferW) * inferH;
        bool needsResize = (width != inferW || height != inferH);

        const float* inputRgbData = rgbInput;
        std::vector<float> resizedRgb;
        if (needsResize) {
            resizedRgb.resize(3 * inferPlaneSize);
            BilinearResizePlanarRGB(rgbInput, width, height, resizedRgb.data(), inferW, inferH);
            inputRgbData = resizedRgb.data();
        }

        std::vector<float> defaultMask;
        const float* inputMaskData = maskInput;
        std::vector<float> resizedMask;
        if (!inputMaskData) {
            defaultMask.assign(inferPlaneSize, 0.0f);
            inputMaskData = defaultMask.data();
        } else if (needsResize) {
            resizedMask.resize(inferPlaneSize);
            BilinearResizeMask(maskInput, width, height, resizedMask.data(), inferW, inferH);
            inputMaskData = resizedMask.data();
        }

        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::AllocatorWithDefaultOptions allocator;

        std::vector<float> neuralOutput(3 * inferPlaneSize);
        int64_t outputDims[4] = { 1, 3, inferH, inferW };
        auto outputNameAlloc = m_session->GetOutputNameAllocated(0, allocator);
        const char* outputNames[] = { outputNameAlloc.get() };
        Ort::Value outputTensor = Ort::Value::CreateTensor<float>(
            memInfo, neuralOutput.data(), 3 * inferPlaneSize, outputDims, 4);

        if (m_inputCount == 1 && m_inputChannels == 4) {
            // 4-channel packed tensor [1, 4, inferH, inferW] (R, G, B, Mask)
            std::vector<float> packedInput(4 * inferPlaneSize);
            std::memcpy(packedInput.data(), inputRgbData, 3 * inferPlaneSize * sizeof(float));
            std::memcpy(packedInput.data() + 3 * inferPlaneSize, inputMaskData, inferPlaneSize * sizeof(float));

            int64_t inputDims[4] = { 1, 4, inferH, inferW };
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, packedInput.data(), 4 * inferPlaneSize, inputDims, 4);

            auto inputNameAlloc = m_session->GetInputNameAllocated(0, allocator);
            const char* inputNames[] = { inputNameAlloc.get() };

            m_session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, &outputTensor, 1);
        } else if (m_inputCount >= 2) {
            // 2-input model [input_image: (1, 3, H, W), input_mask: (1, 1, H, W)]
            int64_t imageDims[4] = { 1, 3, inferH, inferW };
            int64_t maskDims[4] = { 1, 1, inferH, inferW };

            Ort::Value inputImageTensor = Ort::Value::CreateTensor<float>(
                memInfo, const_cast<float*>(inputRgbData), 3 * inferPlaneSize, imageDims, 4);
            Ort::Value inputMaskTensor = Ort::Value::CreateTensor<float>(
                memInfo, const_cast<float*>(inputMaskData), inferPlaneSize, maskDims, 4);

            auto inName0 = m_session->GetInputNameAllocated(0, allocator);
            auto inName1 = m_session->GetInputNameAllocated(1, allocator);
            const char* inputNames[] = { inName0.get(), inName1.get() };
            Ort::Value inputTensors[] = { std::move(inputImageTensor), std::move(inputMaskTensor) };

            m_session->Run(Ort::RunOptions{nullptr}, inputNames, inputTensors, 2, outputNames, &outputTensor, 1);
        } else {
            // 1-input RGB model [1, 3, inferH, inferW]
            int64_t imageDims[4] = { 1, 3, inferH, inferW };
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, const_cast<float*>(inputRgbData), 3 * inferPlaneSize, imageDims, 4);
            auto inName0 = m_session->GetInputNameAllocated(0, allocator);
            const char* inputNames[] = { inName0.get() };

            m_session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, &outputTensor, 1);
        }

        // Upscale neural prediction if processed at reduced tensor resolution
        std::vector<float> upscaledOutput;
        const float* filledSource = neuralOutput.data();
        if (needsResize) {
            upscaledOutput.resize(totalElements);
            BilinearResizePlanarRGB(neuralOutput.data(), inferW, inferH, upscaledOutput.data(), width, height);
            filledSource = upscaledOutput.data();
        }

        // Composite neural inpainting into hole regions, preserving 100% fidelity on non-hole pixels
        if (maskInput) {
            for (size_t i = 0; i < planeSize; ++i) {
                float m = std::clamp(maskInput[i], 0.0f, 1.0f);
                rgbOutput[i] = (1.0f - m) * rgbInput[i] + m * filledSource[i];
                rgbOutput[planeSize + i] = (1.0f - m) * rgbInput[planeSize + i] + m * filledSource[planeSize + i];
                rgbOutput[2 * planeSize + i] = (1.0f - m) * rgbInput[2 * planeSize + i] + m * filledSource[2 * planeSize + i];
            }
        } else {
            std::memcpy(rgbOutput, filledSource, totalElements * sizeof(float));
        }

        return true;
    } catch (const Ort::Exception& e) {
        LOG_WARN("DisocclusionInpainter: Neural inference exception (%s). Engaging heuristic blend fallback.", e.what());
        ApplyFallbackBlend(rgbInput, maskInput, rgbOutput, width, height);
        return true;
    } catch (...) {
        LOG_WARN("DisocclusionInpainter: Unknown exception during neural inpainting. Engaging fallback.");
        ApplyFallbackBlend(rgbInput, maskInput, rgbOutput, width, height);
        return true;
    }
}

bool DisocclusionInpainter::InpaintRGBA(
    const uint8_t* rgbaInput,
    const float* maskInput,
    uint8_t* rgbaOutput,
    int width,
    int height
) {
    if (!rgbaInput || !rgbaOutput || width <= 0 || height <= 0) return false;

    size_t planeSize = static_cast<size_t>(width) * height;
    std::vector<float> planarRGB(3 * planeSize);
    std::vector<float> derivedMask;

    float* rPlane = planarRGB.data();
    float* gPlane = planarRGB.data() + planeSize;
    float* bPlane = planarRGB.data() + 2 * planeSize;

    const float* activeMask = maskInput;
    if (!activeMask) {
        // Derive hole mask from alpha channel: alpha == 0 marks an empty disocclusion hole
        derivedMask.resize(planeSize, 0.0f);
        for (size_t i = 0; i < planeSize; ++i) {
            uint8_t a = rgbaInput[i * 4 + 3];
            derivedMask[i] = (a == 0) ? 1.0f : 0.0f;
        }
        activeMask = derivedMask.data();
    }

    // Convert packed RGBA8 [0..255] to planar RGB [0.0..1.0]
    for (size_t i = 0; i < planeSize; ++i) {
        size_t srcIdx = i * 4;
        rPlane[i] = static_cast<float>(rgbaInput[srcIdx]) / 255.0f;
        gPlane[i] = static_cast<float>(rgbaInput[srcIdx + 1]) / 255.0f;
        bPlane[i] = static_cast<float>(rgbaInput[srcIdx + 2]) / 255.0f;
    }

    std::vector<float> planarOutput(3 * planeSize);
    if (!Inpaint(planarRGB.data(), activeMask, planarOutput.data(), width, height)) {
        return false;
    }

    const float* outR = planarOutput.data();
    const float* outG = planarOutput.data() + planeSize;
    const float* outB = planarOutput.data() + 2 * planeSize;

    // Convert planar RGB back to packed RGBA8 [0..255]
    for (size_t i = 0; i < planeSize; ++i) {
        size_t dstIdx = i * 4;
        rgbaOutput[dstIdx] = static_cast<uint8_t>(std::clamp(outR[i] * 255.0f + 0.5f, 0.0f, 255.0f));
        rgbaOutput[dstIdx + 1] = static_cast<uint8_t>(std::clamp(outG[i] * 255.0f + 0.5f, 0.0f, 255.0f));
        rgbaOutput[dstIdx + 2] = static_cast<uint8_t>(std::clamp(outB[i] * 255.0f + 0.5f, 0.0f, 255.0f));
        rgbaOutput[dstIdx + 3] = rgbaInput[dstIdx + 3]; // Preserve original alpha
    }

    return true;
}

} // namespace ai
} // namespace vrinject
