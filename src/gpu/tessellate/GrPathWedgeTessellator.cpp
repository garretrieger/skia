/*
 * Copyright 2021 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/tessellate/GrPathWedgeTessellator.h"

#include "src/gpu/GrResourceProvider.h"
#include "src/gpu/geometry/GrPathUtils.h"
#include "src/gpu/geometry/GrWangsFormula.h"
#include "src/gpu/tessellate/GrCullTest.h"
#include "src/gpu/tessellate/shaders/GrPathTessellationShader.h"

namespace {

constexpr static float kPrecision = GrTessellationShader::kLinearizationPrecision;

// Parses out each contour in a path and tracks the midpoint. Example usage:
//
//   SkTPathContourParser parser;
//   while (parser.parseNextContour()) {
//       SkPoint midpoint = parser.currentMidpoint();
//       for (auto [verb, pts] : parser.currentContour()) {
//           ...
//       }
//   }
//
class MidpointContourParser {
public:
    MidpointContourParser(const SkPath& path)
            : fPath(path)
            , fVerbs(SkPathPriv::VerbData(fPath))
            , fNumRemainingVerbs(fPath.countVerbs())
            , fPoints(SkPathPriv::PointData(fPath))
            , fWeights(SkPathPriv::ConicWeightData(fPath)) {}
    // Advances the internal state to the next contour in the path. Returns false if there are no
    // more contours.
    bool parseNextContour() {
        bool hasGeometry = false;
        for (; fVerbsIdx < fNumRemainingVerbs; ++fVerbsIdx) {
            switch (fVerbs[fVerbsIdx]) {
                case SkPath::kMove_Verb:
                    if (!hasGeometry) {
                        fMidpoint = fPoints[fPtsIdx];
                        fMidpointWeight = 1;
                        this->advance();
                        ++fPtsIdx;
                        continue;
                    }
                    return true;
                default:
                    continue;
                case SkPath::kLine_Verb:
                    ++fPtsIdx;
                    break;
                case SkPath::kConic_Verb:
                    ++fWtsIdx;
                    [[fallthrough]];
                case SkPath::kQuad_Verb:
                    fPtsIdx += 2;
                    break;
                case SkPath::kCubic_Verb:
                    fPtsIdx += 3;
                    break;
            }
            fMidpoint += fPoints[fPtsIdx - 1];
            ++fMidpointWeight;
            hasGeometry = true;
        }
        return hasGeometry;
    }

    // Allows for iterating the current contour using a range-for loop.
    SkPathPriv::Iterate currentContour() {
        return SkPathPriv::Iterate(fVerbs, fVerbs + fVerbsIdx, fPoints, fWeights);
    }

    SkPoint currentMidpoint() { return fMidpoint * (1.f / fMidpointWeight); }

private:
    void advance() {
        fVerbs += fVerbsIdx;
        fNumRemainingVerbs -= fVerbsIdx;
        fVerbsIdx = 0;
        fPoints += fPtsIdx;
        fPtsIdx = 0;
        fWeights += fWtsIdx;
        fWtsIdx = 0;
    }

    const SkPath& fPath;

    const uint8_t* fVerbs;
    int fNumRemainingVerbs = 0;
    int fVerbsIdx = 0;

    const SkPoint* fPoints;
    int fPtsIdx = 0;

    const float* fWeights;
    int fWtsIdx = 0;

    SkPoint fMidpoint;
    int fMidpointWeight;
};

// Writes out wedge patches, chopping as necessary so none require more segments than are supported
// by the hardware.
class WedgeWriter {
public:
    WedgeWriter(const SkRect& cullBounds, const SkMatrix& viewMatrix, int maxSegments)
            : fCullTest(cullBounds, viewMatrix)
            , fVectorXform(viewMatrix)
            , fMaxSegments_pow2(maxSegments * maxSegments)
            , fMaxSegments_pow4(fMaxSegments_pow2 * fMaxSegments_pow2) {
    }

    SK_ALWAYS_INLINE void writeFlatWedge(const GrShaderCaps& shaderCaps,
                                         GrVertexChunkBuilder* chunker, SkPoint p0, SkPoint p1,
                                         SkPoint midpoint) {
        if (GrVertexWriter vertexWriter = chunker->appendVertex()) {
            GrPathUtils::writeLineAsCubic(p0, p1, &vertexWriter);
            vertexWriter.write(midpoint);
            vertexWriter.write(GrVertexWriter::If(!shaderCaps.infinitySupport(),
                                                  GrTessellationShader::kCubicCurveType));
        }
    }

    SK_ALWAYS_INLINE void writeQuadraticWedge(const GrShaderCaps& shaderCaps,
                                              GrVertexChunkBuilder* chunker, const SkPoint p[3],
                                              SkPoint midpoint) {
        float numSegments_pow4 = GrWangsFormula::quadratic_pow4(kPrecision, p, fVectorXform);
        if (numSegments_pow4 > fMaxSegments_pow4) {
            this->chopAndWriteQuadraticWedges(shaderCaps, chunker, p, midpoint);
            return;
        }
        if (GrVertexWriter vertexWriter = chunker->appendVertex()) {
            GrPathUtils::writeQuadAsCubic(p, &vertexWriter);
            vertexWriter.write(midpoint);
            vertexWriter.write(GrVertexWriter::If(!shaderCaps.infinitySupport(),
                                                  GrTessellationShader::kCubicCurveType));
        }
        fNumFixedSegments_pow4 = std::max(numSegments_pow4, fNumFixedSegments_pow4);
    }

    SK_ALWAYS_INLINE void writeConicWedge(const GrShaderCaps& shaderCaps,
                                          GrVertexChunkBuilder* chunker, const SkPoint p[3],
                                          float w, SkPoint midpoint) {
        float numSegments_pow2 = GrWangsFormula::conic_pow2(kPrecision, p, w, fVectorXform);
        if (GrWangsFormula::conic_pow2(kPrecision, p, w, fVectorXform) > fMaxSegments_pow2) {
            this->chopAndWriteConicWedges(shaderCaps, chunker, {p, w}, midpoint);
            return;
        }
        if (GrVertexWriter vertexWriter = chunker->appendVertex()) {
            GrTessellationShader::WriteConicPatch(p, w, &vertexWriter);
            vertexWriter.write(midpoint);
            vertexWriter.write(GrVertexWriter::If(!shaderCaps.infinitySupport(),
                                                  GrTessellationShader::kConicCurveType));
        }
        fNumFixedSegments_pow4 = std::max(numSegments_pow2 * numSegments_pow2,
                                          fNumFixedSegments_pow4);
    }

    SK_ALWAYS_INLINE void writeCubicWedge(const GrShaderCaps& shaderCaps,
                                          GrVertexChunkBuilder* chunker, const SkPoint p[4],
                                          SkPoint midpoint) {
        float numSegments_pow4 = GrWangsFormula::cubic_pow4(kPrecision, p, fVectorXform);
        if (numSegments_pow4 > fMaxSegments_pow4) {
            this->chopAndWriteCubicWedges(shaderCaps, chunker, p, midpoint);
            return;
        }
        if (GrVertexWriter vertexWriter = chunker->appendVertex()) {
            vertexWriter.writeArray(p, 4);
            vertexWriter.write(midpoint);
            vertexWriter.write(GrVertexWriter::If(!shaderCaps.infinitySupport(),
                                                  GrTessellationShader::kCubicCurveType));
        }
        fNumFixedSegments_pow4 = std::max(numSegments_pow4, fNumFixedSegments_pow4);
    }

    int numFixedSegments_pow4() const { return fNumFixedSegments_pow4; }

private:
    void chopAndWriteQuadraticWedges(const GrShaderCaps& shaderCaps, GrVertexChunkBuilder* chunker,
                                     const SkPoint p[3], SkPoint midpoint) {
        SkPoint chops[5];
        SkChopQuadAtHalf(p, chops);
        for (int i = 0; i < 2; ++i) {
            const SkPoint* q = chops + i*2;
            if (fCullTest.areVisible3(q)) {
                this->writeQuadraticWedge(shaderCaps, chunker, q, midpoint);
            } else {
                this->writeFlatWedge(shaderCaps, chunker, q[0], q[2], midpoint);
            }
        }
    }

    void chopAndWriteConicWedges(const GrShaderCaps& shaderCaps, GrVertexChunkBuilder* chunker,
                                 const SkConic& conic, SkPoint midpoint) {
        SkConic chops[2];
        if (!conic.chopAt(.5, chops)) {
            return;
        }
        for (int i = 0; i < 2; ++i) {
            if (fCullTest.areVisible3(chops[i].fPts)) {
                this->writeConicWedge(shaderCaps, chunker, chops[i].fPts, chops[i].fW, midpoint);
            } else {
                this->writeFlatWedge(shaderCaps, chunker, chops[i].fPts[0], chops[i].fPts[2],
                                     midpoint);
            }
        }
    }

    void chopAndWriteCubicWedges(const GrShaderCaps& shaderCaps, GrVertexChunkBuilder* chunker,
                                 const SkPoint p[4], SkPoint midpoint) {
        SkPoint chops[7];
        SkChopCubicAtHalf(p, chops);
        for (int i = 0; i < 2; ++i) {
            const SkPoint* c = chops + i*3;
            if (fCullTest.areVisible4(c)) {
                this->writeCubicWedge(shaderCaps, chunker, c, midpoint);
            } else {
                this->writeFlatWedge(shaderCaps, chunker, c[0], c[3], midpoint);
            }
        }
    }

    GrCullTest fCullTest;
    GrVectorXform fVectorXform;
    const float fMaxSegments_pow2;
    const float fMaxSegments_pow4;

    // If using fixed count, this is the max number of curve segments we need to draw per instance.
    float fNumFixedSegments_pow4 = 1;
};

}  // namespace


GrPathTessellator* GrPathWedgeTessellator::Make(SkArenaAlloc* arena, const SkMatrix& viewMatrix,
                                                const SkPMColor4f& color, int numPathVerbs,
                                                const GrPipeline& pipeline, const GrCaps& caps) {
    using PatchType = GrPathTessellationShader::PatchType;
    GrPathTessellationShader* shader;
    if (caps.shaderCaps()->tessellationSupport() &&
        caps.shaderCaps()->infinitySupport() &&  // The hw tessellation shaders use infinity.
        !pipeline.usesVaryingCoords() &&  // Our tessellation back door doesn't handle varyings.
        numPathVerbs >= caps.minPathVerbsForHwTessellation()) {
        shader = GrPathTessellationShader::MakeHardwareTessellationShader(arena, viewMatrix, color,
                                                                          PatchType::kWedges);
    } else {
        shader = GrPathTessellationShader::MakeMiddleOutFixedCountShader(*caps.shaderCaps(), arena,
                                                                         viewMatrix, color,
                                                                         PatchType::kWedges);
    }
    return arena->make([=](void* objStart) {
        return new(objStart) GrPathWedgeTessellator(shader);
    });
}

GR_DECLARE_STATIC_UNIQUE_KEY(gFixedCountVertexBufferKey);
GR_DECLARE_STATIC_UNIQUE_KEY(gFixedCountIndexBufferKey);

void GrPathWedgeTessellator::prepare(GrMeshDrawTarget* target, const SkRect& cullBounds,
                                     const SkPath& path,
                                     const BreadcrumbTriangleList* breadcrumbTriangleList) {
    SkASSERT(!breadcrumbTriangleList);
    SkASSERT(fVertexChunkArray.empty());

    const GrShaderCaps& shaderCaps = *target->caps().shaderCaps();

    // Over-allocate enough wedges for 1 in 4 to chop.
    int maxWedges = GrPathTessellator::MaxSegmentsInPath(path);
    int wedgeAllocCount = (maxWedges * 5 + 3) / 4;  // i.e., ceil(maxWedges * 5/4)
    if (!wedgeAllocCount) {
        return;
    }
    size_t patchStride = fShader->willUseTessellationShaders() ? fShader->vertexStride() * 5
                                                               : fShader->instanceStride();
    GrVertexChunkBuilder chunker(target, &fVertexChunkArray, patchStride, wedgeAllocCount);

    int maxSegments;
    if (fShader->willUseTessellationShaders()) {
        maxSegments = shaderCaps.maxTessellationSegments();
    } else {
        maxSegments = GrPathTessellationShader::kMaxFixedCountSegments;
    }

    WedgeWriter wedgeWriter(cullBounds, fShader->viewMatrix(), maxSegments);
    MidpointContourParser parser(path);
    while (parser.parseNextContour()) {
        SkPoint midpoint = parser.currentMidpoint();
        SkPoint startPoint = {0, 0};
        SkPoint lastPoint = startPoint;
        for (auto [verb, pts, w] : parser.currentContour()) {
            switch (verb) {
                case SkPathVerb::kMove:
                    startPoint = lastPoint = pts[0];
                    break;
                case SkPathVerb::kClose:
                    break;  // Ignore. We can assume an implicit close at the end.
                case SkPathVerb::kLine:
                    wedgeWriter.writeFlatWedge(shaderCaps, &chunker, pts[0], pts[1], midpoint);
                    lastPoint = pts[1];
                    break;
                case SkPathVerb::kQuad:
                    wedgeWriter.writeQuadraticWedge(shaderCaps, &chunker, pts, midpoint);
                    lastPoint = pts[2];
                    break;
                case SkPathVerb::kConic:
                    wedgeWriter.writeConicWedge(shaderCaps, &chunker, pts, *w, midpoint);
                    lastPoint = pts[2];
                    break;
                case SkPathVerb::kCubic:
                    wedgeWriter.writeCubicWedge(shaderCaps, &chunker, pts, midpoint);
                    lastPoint = pts[3];
                    break;
            }
        }
        if (lastPoint != startPoint) {
            wedgeWriter.writeFlatWedge(shaderCaps, &chunker, lastPoint, startPoint, midpoint);
        }
    }

    if (!fShader->willUseTessellationShaders()) {
        // log2(n) == log16(n^4).
        int fixedResolveLevel = GrWangsFormula::nextlog16(wedgeWriter.numFixedSegments_pow4());
        int numCurveTriangles =
                GrPathTessellationShader::NumCurveTrianglesAtResolveLevel(fixedResolveLevel);
        // Emit 3 vertices per curve triangle, plus 3 more for the fan triangle.
        fFixedIndexCount = numCurveTriangles * 3 + 3;

        GR_DEFINE_STATIC_UNIQUE_KEY(gFixedCountVertexBufferKey);

        fFixedCountVertexBuffer = target->resourceProvider()->findOrMakeStaticBuffer(
                GrGpuBufferType::kVertex,
                GrPathTessellationShader::SizeOfVertexBufferForMiddleOutWedges(),
                gFixedCountVertexBufferKey,
                GrPathTessellationShader::InitializeVertexBufferForMiddleOutWedges);

        GR_DEFINE_STATIC_UNIQUE_KEY(gFixedCountIndexBufferKey);

        fFixedCountIndexBuffer = target->resourceProvider()->findOrMakeStaticBuffer(
                GrGpuBufferType::kIndex,
                GrPathTessellationShader::SizeOfIndexBufferForMiddleOutWedges(),
                gFixedCountIndexBufferKey,
                GrPathTessellationShader::InitializeIndexBufferForMiddleOutWedges);
    }
}

void GrPathWedgeTessellator::draw(GrOpFlushState* flushState) const {
    if (fShader->willUseTessellationShaders()) {
        for (const GrVertexChunk& chunk : fVertexChunkArray) {
            flushState->bindBuffers(nullptr, nullptr, chunk.fBuffer);
            flushState->draw(chunk.fCount * 5, chunk.fBase * 5);
        }
    } else {
        SkASSERT(fShader->hasInstanceAttributes());
        for (const GrVertexChunk& chunk : fVertexChunkArray) {
            flushState->bindBuffers(fFixedCountIndexBuffer, chunk.fBuffer, fFixedCountVertexBuffer);
            flushState->drawIndexedInstanced(fFixedIndexCount, 0, chunk.fCount, chunk.fBase, 0);
        }
    }
}
