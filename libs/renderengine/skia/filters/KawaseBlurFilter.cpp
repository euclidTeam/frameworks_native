/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "KawaseBlurFilter.h"
#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkImageInfo.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/trace.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>
#include <algorithm>
#include <cmath>

namespace android {
namespace renderengine {
namespace skia {

KawaseBlurFilter::KawaseBlurFilter() : BlurFilter() {
    SkString blurString(
            R"(
uniform shader child;
uniform float in_blurOffset;

half3 rgb2hsv(half3 c) {
    half4 K = half4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    half4 p = c.g < c.b ? half4(c.bg, K.wz) : half4(c.gb, K.xy);
    half4 q = c.r < p.x ? half4(p.xyw, c.r) : half4(c.r, p.yzx);
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return half3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

half3 hsv2rgb(half3 c) {
    half4 K = half4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    half3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

half4 main(float2 xy) {
    half4 c = child.eval(xy);
    c += child.eval(xy + float2(+in_blurOffset, +in_blurOffset));
    c += child.eval(xy + float2(+in_blurOffset, -in_blurOffset));
    c += child.eval(xy + float2(-in_blurOffset, -in_blurOffset));
    c += child.eval(xy + float2(-in_blurOffset, +in_blurOffset));
    c *= 0.2;

    half3 hsv = rgb2hsv(c.rgb);
    hsv.y *= 1.0;
    c.rgb = hsv2rgb(hsv);

    return c;
})");

    auto [blurEffect, error] = SkRuntimeEffect::MakeForShader(blurString);
    if (!blurEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", error.c_str());
    }
    mBlurEffect = std::move(blurEffect);
}

// Draws the given runtime shader on a GPU (Ganesh) surface and returns the result as an
// SkImage.
static sk_sp<SkImage> makeImage(SkSurface* surface, SkRuntimeShaderBuilder* builder) {
    sk_sp<SkShader> shader = builder->makeShader(nullptr);
    if (!shader) {
        return nullptr;
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    paint.setBlendMode(SkBlendMode::kSrc);
    surface->getCanvas()->drawPaint(paint);
    return surface->makeTemporaryImage();
}

sk_sp<SkImage> KawaseBlurFilter::generate(SkiaGpuContext* context, const uint32_t blurRadius,
                                          const sk_sp<SkImage> input,
                                          const SkRect& blurRect) const {
    LOG_ALWAYS_FATAL_IF(context == nullptr, "%s: Needs GPU context", __func__);
    LOG_ALWAYS_FATAL_IF(input == nullptr, "%s: Invalid input image", __func__);

    if (blurRadius == 0) {
        return input;
    }

    SkRuntimeShaderBuilder blurBuilder(mBlurEffect);
    SkSamplingOptions linear(SkFilterMode::kLinear, SkMipmapMode::kNone);
    float tmpRadius = (float)blurRadius;

    constexpr float kFastPathThreshold = 12.0f;

    if (tmpRadius <= kFastPathThreshold) {
        constexpr float kFastPathScale = 0.25f;
        SkImageInfo info =
                input->imageInfo().makeWH(std::ceil(blurRect.width() * kFastPathScale),
                                          std::ceil(blurRect.height() * kFastPathScale));
        SkMatrix matrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        matrix.postScale(kFastPathScale, kFastPathScale);

        blurBuilder.child("child") =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear, matrix);

        uint32_t numberOfPasses = std::clamp((uint32_t)std::ceil(tmpRadius / 3.0f), 1u, 4u);
        float radiusByPasses = (numberOfPasses > 0) ? (tmpRadius / (float)numberOfPasses) : 0.0f;

        sk_sp<SkSurface> surface = context->createRenderTarget(info);
        sk_sp<SkSurface> surfaceTwo = context->createRenderTarget(info);
        sk_sp<SkImage> tmpBlur = nullptr;

        for (auto i = 0; i < numberOfPasses; i++) {
            blurBuilder.uniform("in_blurOffset") = (float)(i + 1) * radiusByPasses * kFastPathScale;
            tmpBlur = makeImage(surface.get(), &blurBuilder);
            blurBuilder.child("child") =
                    tmpBlur->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
            using std::swap;
            swap(surface, surfaceTwo);
        }
        return tmpBlur;

    } else {
        constexpr float kHiResScale = 0.5f;
        SkImageInfo hiResInfo =
                input->imageInfo().makeWH(std::ceil(blurRect.width() * kHiResScale),
                                          std::ceil(blurRect.height() * kHiResScale));
        SkMatrix hiResMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        hiResMatrix.postScale(kHiResScale, kHiResScale);

        blurBuilder.child("child") =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear, hiResMatrix);
        blurBuilder.uniform("in_blurOffset") = 1.0f * kHiResScale;

        sk_sp<SkSurface> hiResSurface = context->createRenderTarget(hiResInfo);
        LOG_ALWAYS_FATAL_IF(!hiResSurface, "%s: Failed to create hi-res surface!", __func__);
        sk_sp<SkImage> hiResBlur = makeImage(hiResSurface.get(), &blurBuilder);

        constexpr float kLoResScale = 0.25f;
        SkImageInfo loResInfo =
                input->imageInfo().makeWH(std::ceil(blurRect.width() * kLoResScale),
                                          std::ceil(blurRect.height() * kLoResScale));
        sk_sp<SkSurface> loResSurface = context->createRenderTarget(loResInfo);
        LOG_ALWAYS_FATAL_IF(!loResSurface, "%s: Failed to create lo-res surface!", __func__);

        SkPaint downsamplePaint;
        loResSurface->getCanvas()->drawImageRect(
                hiResBlur, SkRect::Make(loResInfo.dimensions()),
                SkSamplingOptions({1.0f / 3.0f, 1.0f / 3.0f}), &downsamplePaint);
        sk_sp<SkImage> tmpBlur = loResSurface->makeImageSnapshot();

        uint32_t numberOfPasses = std::clamp((uint32_t)std::ceil(tmpRadius / 3.0f), 2u, 8u);
        float radiusByPasses = (numberOfPasses > 0) ? (tmpRadius / (float)numberOfPasses) : 0.0f;

        if (numberOfPasses > 1) {
            sk_sp<SkSurface> surfaceTwo = context->createRenderTarget(loResInfo);
            LOG_ALWAYS_FATAL_IF(!surfaceTwo, "%s: Failed to create second blur surface!",
                                __func__);

            for (auto i = 0; i < numberOfPasses; i++) {
                LOG_ALWAYS_FATAL_IF(tmpBlur == nullptr, "%s: tmpBlur is null for pass %d",
                                    __func__, i);
                blurBuilder.child("child") =
                        tmpBlur->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
                blurBuilder.uniform("in_blurOffset") =
                        (float)(i + 1) * radiusByPasses * kLoResScale;
                tmpBlur = makeImage(loResSurface.get(), &blurBuilder);
                using std::swap;
                swap(loResSurface, surfaceTwo);
            }
        }
        return tmpBlur;
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
