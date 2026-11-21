#pragma once

#include "ai/ai_model_loader.h"
#include <vector>
#include <mutex>

namespace vrinject {
namespace ai {

class DisocclusionInpainter : public NexVR::AI::AiModelLoader {
public:
    static DisocclusionInpainter& GetInstance(bool useDirectML = true, int deviceId = 0);

    DisocclusionInpainter(bool useDirectML = true, int deviceId = 0, const std::wstring& modelPath = L"models/disocclusion_inpainter.onnx");
    ~DisocclusionInpainter() override = default;

    // Runs neural inpainting on the provided RGB image and mask buffer
    // rgbInput: float array [3 * width * height] normalized 0.0 to 1.0 (RGB planar)
    // maskInput: float array [1 * width * height] (1.0 = hole to fill, 0.0 = valid pixel). If nullptr, treated as all 0.0.
    // rgbOutput: float array [3 * width * height] receives the inpainted RGB image
    bool Inpaint(
        const float* rgbInput,
        const float* maskInput,
        float* rgbOutput,
        int width,
        int height
    );

    // Convenience method for packed RGBA pixels
    // rgbaInput: uint8 array [4 * width * height] [R, G, B, A]
    // maskInput: float array [1 * width * height] (optional; if nullptr, alpha == 0 in rgbaInput is treated as hole)
    // rgbaOutput: uint8 array [4 * width * height] receives inpainted RGBA
    bool InpaintRGBA(
        const uint8_t* rgbaInput,
        const float* maskInput,
        uint8_t* rgbaOutput,
        int width,
        int height
    );

    int GetModelInputWidth() const { return m_modelInputWidth; }
    int GetModelInputHeight() const { return m_modelInputHeight; }
    int GetInputChannels() const { return m_inputChannels; }
    bool HasStaticDimensions() const { return m_hasStaticDimensions; }

private:
    void QueryModelDimensions();

    std::mutex m_mutex;
    int m_modelInputWidth = 480;
    int m_modelInputHeight = 270;
    int m_inputChannels = 4;
    size_t m_inputCount = 1;
    bool m_hasStaticDimensions = true;
};

} // namespace ai
} // namespace vrinject
