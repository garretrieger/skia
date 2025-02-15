/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm/gm.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkClipOp.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSize.h"
#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrRecordingContext.h"
#include "include/private/GrTypesPriv.h"
#include "include/private/SkColorData.h"
#include "src/core/SkCanvasPriv.h"
#include "src/core/SkClipOpPriv.h"
#include "src/core/SkClipStack.h"
#include "src/gpu/GrAppliedClip.h"
#include "src/gpu/GrCaps.h"
#include "src/gpu/GrClip.h"
#include "src/gpu/GrFragmentProcessor.h"
#include "src/gpu/GrPaint.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/GrSurfaceDrawContext.h"
#include "src/gpu/GrTextureProxy.h"
#include "src/gpu/GrUserStencilSettings.h"
#include "src/gpu/effects/GrDisableColorXP.h"
#include "src/gpu/effects/GrTextureEffect.h"
#include "tools/ToolUtils.h"

#include <utility>

class GrRecordingContext;

constexpr static SkIRect kDeviceRect = {0, 0, 600, 600};
constexpr static SkIRect kCoverRect = {50, 50, 550, 550};

namespace skiagm {

////////////////////////////////////////////////////////////////////////////////////////////////////

class WindowRectanglesBaseGM : public GM {
protected:
    virtual DrawResult onCoverClipStack(const SkClipStack&, SkCanvas*, SkString* errorMsg) = 0;

private:
    SkISize onISize() override { return SkISize::Make(kDeviceRect.width(), kDeviceRect.height()); }
    DrawResult onDraw(SkCanvas*, SkString* errorMsg) final;
};

DrawResult WindowRectanglesBaseGM::onDraw(SkCanvas* canvas, SkString* errorMsg) {
    ToolUtils::draw_checkerboard(canvas, 0xffffffff, 0xffc6c3c6, 25);

    SkClipStack stack;
    stack.clipRect(SkRect::MakeXYWH(370.75, 80.25, 149, 100), SkMatrix::I(),
                   kDifference_SkClipOp, false);
    stack.clipRect(SkRect::MakeXYWH(80.25, 420.75, 150, 100), SkMatrix::I(),
                   kDifference_SkClipOp, true);
    stack.clipRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(200, 200, 200, 200), 60, 45),
                    SkMatrix::I(), kDifference_SkClipOp, true);

    SkRRect nine;
    nine.setNinePatch(SkRect::MakeXYWH(550 - 30.25 - 100, 370.75, 100, 150), 12, 35, 23, 20);
    stack.clipRRect(nine, SkMatrix::I(), kDifference_SkClipOp, true);

    SkRRect complx;
    SkVector complxRadii[4] = {{6, 4}, {8, 12}, {16, 24}, {48, 32}};
    complx.setRectRadii(SkRect::MakeXYWH(80.25, 80.75, 100, 149), complxRadii);
    stack.clipRRect(complx, SkMatrix::I(), kDifference_SkClipOp, false);

    return this->onCoverClipStack(stack, canvas, errorMsg);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Draws a clip that will exercise window rectangles if they are supported.
 */
class WindowRectanglesGM : public WindowRectanglesBaseGM {
private:
    SkString onShortName() final { return SkString("windowrectangles"); }
    DrawResult onCoverClipStack(const SkClipStack&, SkCanvas*, SkString* errorMsg) final;
};

DrawResult WindowRectanglesGM::onCoverClipStack(const SkClipStack& stack, SkCanvas* canvas,
                                                SkString* errorMsg) {
    SkPaint paint;
    paint.setColor(0xff00aa80);

    // Set up the canvas's clip to match our SkClipStack.
    SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);
    for (const SkClipStack::Element* element = iter.next(); element; element = iter.next()) {
        SkClipOp op = element->getOp();
        bool isAA = element->isAA();
        switch (element->getDeviceSpaceType()) {
            case SkClipStack::Element::DeviceSpaceType::kShader:
                canvas->clipShader(element->refShader(), op);
                break;
            case SkClipStack::Element::DeviceSpaceType::kPath:
                canvas->clipPath(element->getDeviceSpacePath(), op, isAA);
                break;
            case SkClipStack::Element::DeviceSpaceType::kRRect:
                canvas->clipRRect(element->getDeviceSpaceRRect(), op, isAA);
                break;
            case SkClipStack::Element::DeviceSpaceType::kRect:
                canvas->clipRect(element->getDeviceSpaceRect(), op, isAA);
                break;
            case SkClipStack::Element::DeviceSpaceType::kEmpty:
                canvas->clipRect({ 0, 0, 0, 0 }, kIntersect_SkClipOp, false);
                break;
        }
    }

    canvas->drawRect(SkRect::Make(kCoverRect), paint);
    return DrawResult::kOk;
}

DEF_GM( return new WindowRectanglesGM(); )
}  // namespace skiagm

////////////////////////////////////////////////////////////////////////////////////////////////////

#if SK_GPU_V1
#include "src/gpu/GrReducedClip.h"
#include "src/gpu/GrStencilClip.h"

namespace skiagm {

constexpr static int kNumWindows = 8;

/**
 * Visualizes the mask (alpha or stencil) for a clip with several window rectangles. The purpose of
 * this test is to verify that window rectangles are being used during clip mask generation, and to
 * visualize where the window rectangles are placed.
 *
 * We use window rectangles when generating the clip mask because there is no need to invest time
 * defining those regions where window rectangles will be in effect during the actual draw anyway.
 *
 * This test works by filling the entire clip mask with a small checkerboard pattern before drawing
 * it, and then covering the mask with a solid color once it has been generated. The regions inside
 * window rectangles or outside the scissor should still have the initial checkerboard intact.
 */
class WindowRectanglesMaskGM : public WindowRectanglesBaseGM {
private:
    constexpr static int kMaskCheckerSize = 5;
    SkString onShortName() final { return SkString("windowrectangles_mask"); }
    DrawResult onCoverClipStack(const SkClipStack&, SkCanvas*, SkString* errorMsg) final;
    void visualizeAlphaMask(GrRecordingContext*, GrSurfaceDrawContext*,
                            const GrReducedClip&, GrPaint&&);
    void visualizeStencilMask(GrRecordingContext*, GrSurfaceDrawContext*,
                              const GrReducedClip&, GrPaint&&);
    void stencilCheckerboard(GrSurfaceDrawContext*, bool flip);
};

/**
 * This class clips a cover by an alpha mask. We use it to visualize the alpha clip mask.
 */
class AlphaOnlyClip final : public GrClip {
public:
    AlphaOnlyClip(GrSurfaceProxyView mask, int x, int y) : fMask(std::move(mask)), fX(x), fY(y) {}

private:
    SkIRect getConservativeBounds() const final {
        return SkIRect::MakeXYWH(fX, fY, fMask.width(), fMask.height());
    }
    Effect apply(GrRecordingContext* ctx, GrSurfaceDrawContext*, GrDrawOp*, GrAAType,
                 GrAppliedClip* out, SkRect* bounds) const override {
        GrSamplerState samplerState(GrSamplerState::WrapMode::kClampToBorder,
                                    GrSamplerState::Filter::kNearest);
        auto m = SkMatrix::Translate(-fX, -fY);
        auto subset = SkRect::Make(fMask.dimensions());
        auto domain = bounds->makeOffset(-fX, -fY).makeInset(0.5, 0.5);
        auto fp = GrTextureEffect::MakeSubset(fMask, kPremul_SkAlphaType, m, samplerState, subset,
                                              domain, *ctx->priv().caps());
        fp = GrFragmentProcessor::DeviceSpace(std::move(fp));
        out->addCoverageFP(std::move(fp));
        return Effect::kClipped;
    }
    GrSurfaceProxyView fMask;
    int fX;
    int fY;
};

/**
 * Makes a clip object that enforces the stencil clip bit. Used to visualize the stencil mask.
 */
static GrStencilClip make_stencil_only_clip(GrSurfaceDrawContext* rtc) {
    return GrStencilClip(rtc->dimensions(), SkClipStack::kEmptyGenID);
};

DrawResult WindowRectanglesMaskGM::onCoverClipStack(const SkClipStack& stack,
                                                    SkCanvas* canvas,
                                                    SkString* errorMsg) {
    auto rContext = canvas->recordingContext();
    GrSurfaceDrawContext* sdc = SkCanvasPriv::TopDeviceSurfaceDrawContext(canvas);

    if (!rContext || !sdc) {
        *errorMsg = kErrorMsg_DrawSkippedGpuOnly;
        return DrawResult::kSkip;
    }
    if (sdc->maxWindowRectangles() < kNumWindows) {
        *errorMsg = "Requires at least 8 window rectangles. "
                    "(Are you off FBO 0? Use sRGB to force offscreen rendering.)";
        return DrawResult::kSkip;
    }

    const GrReducedClip reducedClip(stack, SkRect::Make(kCoverRect), sdc->caps(), kNumWindows);

    GrPaint paint;
    if (sdc->numSamples() <= 1) {
        paint.setColor4f({ 0, 0.25f, 1, 1 });
        this->visualizeAlphaMask(rContext, sdc, reducedClip, std::move(paint));
    } else {
        paint.setColor4f({ 1, 0.25f, 0.25f, 1 });
        this->visualizeStencilMask(rContext, sdc, reducedClip, std::move(paint));
    }
    return DrawResult::kOk;
}

void WindowRectanglesMaskGM::visualizeAlphaMask(GrRecordingContext* ctx, GrSurfaceDrawContext* rtc,
                                                const GrReducedClip& reducedClip, GrPaint&& paint) {
    const int padRight = (kDeviceRect.right() - kCoverRect.right()) / 2;
    const int padBottom = (kDeviceRect.bottom() - kCoverRect.bottom()) / 2;
    auto maskRTC = GrSurfaceDrawContext::MakeWithFallback(
            ctx, GrColorType::kAlpha_8, nullptr, SkBackingFit::kExact,
            {kCoverRect.width() + padRight, kCoverRect.height() + padBottom}, SkSurfaceProps());
    if (!maskRTC) {
        return;
    }

    // Draw a checker pattern into the alpha mask so we can visualize the regions left untouched by
    // the clip mask generation.
    this->stencilCheckerboard(maskRTC.get(), true);
    maskRTC->clear(SK_PMColor4fWHITE);
    GrPaint stencilPaint;
    stencilPaint.setCoverageSetOpXPFactory(SkRegion::kDifference_Op, false);
    GrStencilClip stencilClip = make_stencil_only_clip(maskRTC.get());
    maskRTC->stencilRect(&stencilClip, &GrUserStencilSettings::kUnused, std::move(stencilPaint),
                         GrAA::kNo, SkMatrix::I(), SkRect::Make(maskRTC->dimensions()));
    reducedClip.drawAlphaClipMask(maskRTC.get());

    int x = kCoverRect.x() - kDeviceRect.x(),
        y = kCoverRect.y() - kDeviceRect.y();

    // Now visualize the alpha mask by drawing a rect over the area where it is defined. The regions
    // inside window rectangles or outside the scissor should still have the initial checkerboard
    // intact. (This verifies we didn't spend any time modifying those pixels in the mask.)
    AlphaOnlyClip alphaClip(maskRTC->readSurfaceView().makeSwizzle(GrSwizzle("aaaa")), x, y);
    rtc->drawRect(&alphaClip, std::move(paint), GrAA::kYes, SkMatrix::I(),
                  SkRect::Make(SkIRect::MakeXYWH(x, y, maskRTC->width(), maskRTC->height())));
}

void WindowRectanglesMaskGM::visualizeStencilMask(GrRecordingContext* ctx,
                                                  GrSurfaceDrawContext* rtc,
                                                  const GrReducedClip& reducedClip,
                                                  GrPaint&& paint) {
    if (ctx->abandoned()) {
        // GrReducedClip assumes the context hasn't been abandoned, which is reasonable since it is
        // only ever used if a draw op is made. Since this GM calls it directly, it has to be taken
        // into account.
        return;
    }
    // Draw a checker pattern into the stencil buffer so we can visualize the regions left untouched
    // by the clip mask generation.
    this->stencilCheckerboard(rtc, false);
    reducedClip.drawStencilClipMask(ctx, rtc);

    // Now visualize the stencil mask by covering the entire render target. The regions inside
    // window rectangles or outside the scissor should still have the initial checkerboard intact.
    // (This verifies we didn't spend any time modifying those pixels in the mask.)
    GrStencilClip clip = make_stencil_only_clip(rtc);
    rtc->drawPaint(&clip, std::move(paint), SkMatrix::I());
}

void WindowRectanglesMaskGM::stencilCheckerboard(GrSurfaceDrawContext* rtc, bool flip) {
    constexpr static GrUserStencilSettings kSetClip(
        GrUserStencilSettings::StaticInit<
        0,
        GrUserStencilTest::kAlways,
        0,
        GrUserStencilOp::kSetClipBit,
        GrUserStencilOp::kKeep,
        0>()
    );

    rtc->clearStencilClip(SkIRect::MakeSize(rtc->dimensions()), false);

    for (int y = 0; y < kDeviceRect.height(); y += kMaskCheckerSize) {
        for (int x = (y & 1) == flip ? 0 : kMaskCheckerSize;
             x < kDeviceRect.width(); x += 2 * kMaskCheckerSize) {
            SkIRect checker = SkIRect::MakeXYWH(x, y, kMaskCheckerSize, kMaskCheckerSize);
            GrPaint paint;
            paint.setXPFactory(GrDisableColorXPFactory::Get());
            rtc->stencilRect(nullptr, &kSetClip, std::move(paint), GrAA::kNo, SkMatrix::I(),
                             SkRect::Make(checker));
        }
    }
}

DEF_GM( return new WindowRectanglesMaskGM(); )
}  // namespace skiagm

#endif // SK_GPU_V1

