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

half4 main(float2 xy) {
    half4 c = child.eval(xy);
    c += child.eval(xy + float2(+in_blurOffset, +in_blurOffset));
    c += child.eval(xy + float2(+in_blurOffset, -in_blurOffset));
    c += child.eval(xy + float2(-in_blurOffset, -in_blurOffset));
    c += child.eval(xy + float2(-in_blurOffset, +in_blurOffset));
    c *= 0.2;

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
                                          const SkRect& blurRect) {
    LOG_ALWAYS_FATAL_IF(context == nullptr, "%s: Needs GPU context", __func__);
    LOG_ALWAYS_FATAL_IF(input == nullptr, "%s: Invalid input image", __func__);

    if (mCachedBlurredImage && mCachedInputUniqueID == input->uniqueID() &&
       mCachedBlurRadius == blurRadius && mCachedBlurRect == blurRect) {
        return mCachedBlurredImage;
    }

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

        uint32_t passCount = (uint32_t)std::ceil(tmpRadius / 3.0f);
        uint32_t numberOfPasses = std::clamp(passCount, 1u, 4u);
        float radiusByPasses = (numberOfPasses > 0) ? (tmpRadius / (float)numberOfPasses) : 0.0f;

        if (!mSurface1 || !mSurface2 || mSurface1->width() < info.width() || mSurface1->height() < info.height()) {
            mSurface1 = context->createRenderTarget(info);
            mSurface2 = context->createRenderTarget(info);
        }
        sk_sp<SkSurface> surface = mSurface1->makeSurface(info);
        sk_sp<SkSurface> surfaceTwo = mSurface2->makeSurface(info);
        LOG_ALWAYS_FATAL_IF(!surface || !surfaceTwo, "%s: Failed to create blur surfaces!",
                            __func__);

        sk_sp<SkImage> tmpBlur = nullptr;

        for (auto i = 0; i < numberOfPasses; i++) {
            blurBuilder.uniform("in_blurOffset") = (float)(i + 1) * radiusByPasses * kFastPathScale;
            tmpBlur = makeImage(surface.get(), &blurBuilder);
            blurBuilder.child("child") =
                    tmpBlur->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
            using std::swap;
            swap(surface, surfaceTwo);
        }
        mCachedInputUniqueID = input->uniqueID();
        mCachedBlurRadius = blurRadius;
        mCachedBlurRect = blurRect;
        mCachedBlurredImage = tmpBlur;

        return mCachedBlurredImage;

    } else {
        constexpr float kDownsampleScale = 0.25f;
        SkImageInfo info =
                input->imageInfo().makeWH(std::ceil(blurRect.width() * kDownsampleScale),
                                          std::ceil(blurRect.height() * kDownsampleScale));
        SkMatrix matrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        matrix.postScale(kDownsampleScale, kDownsampleScale);

        if (!mSurface1 || !mSurface2 || mSurface1->width() < info.width() || mSurface1->height() < info.height()) {
            mSurface1 = context->createRenderTarget(info);
            mSurface2 = context->createRenderTarget(info);
        }
        sk_sp<SkSurface> surface = mSurface1->makeSurface(info);
        sk_sp<SkSurface> surfaceTwo = mSurface2->makeSurface(info);
        LOG_ALWAYS_FATAL_IF(!surface || !surfaceTwo, "%s: Failed to create blur surfaces!",
                            __func__);
        sk_sp<SkImage> tmpBlur = nullptr;

        uint32_t passCount = (uint32_t)std::ceil(tmpRadius / 3.0f);
        uint32_t numberOfPasses = std::clamp(passCount, 2u, 8u);
        float radiusByPasses = (numberOfPasses > 0) ? (tmpRadius / (float)numberOfPasses) : 0.0f;

        blurBuilder.child("child") =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear, matrix);
        blurBuilder.uniform("in_blurOffset") = (float)(1) * radiusByPasses * kDownsampleScale;
        tmpBlur = makeImage(surface.get(), &blurBuilder);
        swap(surface, surfaceTwo);

        for (uint32_t i = 1; i < numberOfPasses; i++) {
            blurBuilder.child("child") =
                    tmpBlur->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
            blurBuilder.uniform("in_blurOffset") =
                    (float)(i + 1) * radiusByPasses * kDownsampleScale;
            tmpBlur = makeImage(surface.get(), &blurBuilder);
            using std::swap;
            swap(surface, surfaceTwo);
        }
        mCachedInputUniqueID = input->uniqueID();
        mCachedBlurRadius = blurRadius;
        mCachedBlurRect = blurRect;
        mCachedBlurredImage = tmpBlur;

        return mCachedBlurredImage;
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
