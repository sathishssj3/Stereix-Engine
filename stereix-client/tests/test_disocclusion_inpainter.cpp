// test_disocclusion_inpainter.cpp — Enterprise tests for DirectML Disocclusion Inpainter
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <numeric>
#include "ai/disocclusion_inpainter.h"

using namespace vrinject::ai;

class DisocclusionInpainterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Instantiate with DirectML enabled (will utilize GPU if available or fallback cleanly)
        inpainter = std::make_unique<DisocclusionInpainter>(true, 0, L"models/disocclusion_inpainter.onnx");
    }

    std::unique_ptr<DisocclusionInpainter> inpainter;
};

TEST_F(DisocclusionInpainterTest, InitializesCleanlyWithoutCrash) {
    ASSERT_NE(inpainter, nullptr);
    // Should be valid (loaded neural model or initialized safe fallback mode)
    EXPECT_TRUE(inpainter->IsValid() || inpainter->IsSimulated());
}

TEST_F(DisocclusionInpainterTest, ModelDimensionsMatchExpectedArchitecture) {
    ASSERT_NE(inpainter, nullptr);
    EXPECT_EQ(inpainter->GetModelInputWidth(), 480);
    EXPECT_EQ(inpainter->GetModelInputHeight(), 270);
    EXPECT_EQ(inpainter->GetInputChannels(), 4);
    EXPECT_TRUE(inpainter->HasStaticDimensions());
}

TEST_F(DisocclusionInpainterTest, FillsDisocclusionHoleWithContext) {
    const int width = 480;
    const int height = 270;
    const size_t planeSize = static_cast<size_t>(width) * height;

    // Fill scene with uniform background color 0.7f
    std::vector<float> rgbInput(3 * planeSize, 0.7f);
    std::vector<float> maskInput(planeSize, 0.0f);

    // Create a 60x60 disocclusion hole in the center
    const int holeXStart = 210;
    const int holeXEnd = 270;
    const int holeYStart = 105;
    const int holeYEnd = 165;

    for (int y = holeYStart; y < holeYEnd; ++y) {
        for (int x = holeXStart; x < holeXEnd; ++x) {
            size_t idx = y * width + x;
            maskInput[idx] = 1.0f; // Disoccluded hole
            rgbInput[idx] = 0.0f; // Missing data
            rgbInput[planeSize + idx] = 0.0f;
            rgbInput[2 * planeSize + idx] = 0.0f;
        }
    }

    std::vector<float> rgbOutput(3 * planeSize, 0.0f);
    bool ok = inpainter->Inpaint(rgbInput.data(), maskInput.data(), rgbOutput.data(), width, height);
    EXPECT_TRUE(ok);

    // Verify non-hole pixels retained 100% fidelity
    size_t outsidePixel = 10 * width + 10;
    EXPECT_NEAR(rgbOutput[outsidePixel], 0.7f, 0.01f);
    EXPECT_NEAR(rgbOutput[planeSize + outsidePixel], 0.7f, 0.01f);
    EXPECT_NEAR(rgbOutput[2 * planeSize + outsidePixel], 0.7f, 0.01f);

    // Verify hole pixels were inpainted with valid non-zero surrounding context
    size_t holeCenter = (holeYStart + 30) * width + (holeXStart + 30);
    EXPECT_GT(rgbOutput[holeCenter], 0.1f);
    EXPECT_GT(rgbOutput[planeSize + holeCenter], 0.1f);
    EXPECT_GT(rgbOutput[2 * planeSize + holeCenter], 0.1f);
}

TEST_F(DisocclusionInpainterTest, PreservesNonHolePixelsWithFidelity) {
    const int width = 64;
    const int height = 64;
    const size_t planeSize = width * height;

    std::vector<float> rgbInput(3 * planeSize, 0.45f);
    // Mask is all zero (no disocclusions)
    std::vector<float> maskInput(planeSize, 0.0f);
    std::vector<float> rgbOutput(3 * planeSize, 0.0f);

    bool ok = inpainter->Inpaint(rgbInput.data(), maskInput.data(), rgbOutput.data(), width, height);
    EXPECT_TRUE(ok);

    // With zero mask, output should exactly preserve rgbInput
    for (size_t i = 0; i < 3 * planeSize; ++i) {
        EXPECT_NEAR(rgbOutput[i], rgbInput[i], 0.001f);
    }
}

TEST_F(DisocclusionInpainterTest, HandlesArbitraryResolutionDynamicResizing) {
    // Non-native arbitrary dimensions
    const int width = 128;
    const int height = 96;
    const size_t planeSize = width * height;

    std::vector<float> rgbInput(3 * planeSize, 0.5f);
    std::vector<float> maskInput(planeSize, 0.0f);
    maskInput[48 * width + 64] = 1.0f; // Single hole pixel

    std::vector<float> rgbOutput(3 * planeSize, 0.0f);
    bool ok = inpainter->Inpaint(rgbInput.data(), maskInput.data(), rgbOutput.data(), width, height);
    EXPECT_TRUE(ok);

    for (size_t i = 0; i < 3 * planeSize; ++i) {
        EXPECT_GE(rgbOutput[i], 0.0f);
        EXPECT_LE(rgbOutput[i], 1.0f);
    }
}

TEST_F(DisocclusionInpainterTest, InpaintRGBAPackedPixels) {
    const int width = 32;
    const int height = 32;
    const size_t pixelCount = width * height;

    // 50% grey image with alpha = 255
    std::vector<uint8_t> rgbaInput(pixelCount * 4, 128);
    for (size_t i = 0; i < pixelCount; ++i) {
        rgbaInput[i * 4 + 3] = 255;
    }

    // Set hole pixel in center with alpha = 0 and black color
    size_t centerIdx = 16 * width + 16;
    rgbaInput[centerIdx * 4] = 0;
    rgbaInput[centerIdx * 4 + 1] = 0;
    rgbaInput[centerIdx * 4 + 2] = 0;
    rgbaInput[centerIdx * 4 + 3] = 0; // Hole marker

    std::vector<uint8_t> rgbaOutput(pixelCount * 4, 0);
    // Calling InpaintRGBA with nullptr mask will auto-derive mask from alpha == 0
    bool ok = inpainter->InpaintRGBA(rgbaInput.data(), nullptr, rgbaOutput.data(), width, height);
    EXPECT_TRUE(ok);

    // Non-hole pixel should retain its color
    EXPECT_NEAR(static_cast<int>(rgbaOutput[0]), 128, 2);
    EXPECT_EQ(rgbaOutput[3], 255);

    // Hole pixel should be filled with non-zero color
    EXPECT_GT(static_cast<int>(rgbaOutput[centerIdx * 4]), 20);
}

TEST_F(DisocclusionInpainterTest, ZeroCrashNullAndInvalidDimensionGuards) {
    std::vector<float> dummy(100, 0.5f);

    // Null input
    EXPECT_FALSE(inpainter->Inpaint(nullptr, dummy.data(), dummy.data(), 10, 10));

    // Null output
    EXPECT_FALSE(inpainter->Inpaint(dummy.data(), dummy.data(), nullptr, 10, 10));

    // Zero / negative dimensions
    EXPECT_FALSE(inpainter->Inpaint(dummy.data(), dummy.data(), dummy.data(), 0, 10));
    EXPECT_FALSE(inpainter->Inpaint(dummy.data(), dummy.data(), dummy.data(), 10, -5));

    // Null RGBA packed buffers
    std::vector<uint8_t> dummyRGBA(400, 128);
    EXPECT_FALSE(inpainter->InpaintRGBA(nullptr, dummy.data(), dummyRGBA.data(), 10, 10));
    EXPECT_FALSE(inpainter->InpaintRGBA(dummyRGBA.data(), dummy.data(), nullptr, 10, 10));
    EXPECT_FALSE(inpainter->InpaintRGBA(dummyRGBA.data(), dummy.data(), dummyRGBA.data(), -1, 10));
}
