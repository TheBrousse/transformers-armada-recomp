// nr_device.h - P3 S3 (DRAFT for review): the narrow device interface under the native renderer's frame builder.
//
// The frame builder (nr_frame.cpp, API-neutral: the old d3d::render() body) owns everything that is about the
// GS - which targets exist and at which GS address, pass order, the field weave, glare/AA decisions, the draw
// loop, the picture's pacing. It talks to the GPU only through Device below. nr_d3d.cpp becomes the D3D12
// implementation of Device; a Vulkan backend implements the same ~30 calls (docs/RENDERER_BACKEND.md).
//
// Rules for the interface:
// - Handles are small integers the device hands out; 0 is "none". The builder never sees API objects.
// - Resource states are named by use (UseFor) and the device inserts the barriers / layout transitions; it
//   tracks each resource's current state itself (D3D12: the old stateOf / depthStateOf / transition).
// - Pipelines are named by PipelineDesc (gfx:: values from nr_backend.h); the device keeps its own cache and
//   on-disk library keyed however it likes (D3D12 keeps today's numeric pipelineKey, so the library survives).
// - Everything the builder records between beginFrame and endFrame is replayed per picture by the device (the
//   moment pictures: replay/predication stay inside the device; the builder marks the "extra picture" region).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "nr_backend.h" // gfx::BlendState, gfx::Compare, gfx::Format, gfx::WriteMask

namespace nr
{
    struct Texture; // (nr_types.h: a texture's texels as the capture made them)
}

namespace nr::gfx
{
    using TargetId = uint32_t;  // a colour (+ optional depth) render surface
    using TextureId = uint32_t; // a sampled texture, opaque (the device maps it to its own view); 0 = white
    constexpr TextureId kKeepTexture = ~0u; // bindTextures: leave that slot bound to what it was
    using QueryId = uint32_t;

    enum class UseFor : uint8_t
    {
        Draw,       // render target (+ depth write)
        Sample,     // pixel-shader read (colour, or depth for the depth fog)
        CopySource,
        CopyDest,
    };

    enum class DepthUse : uint8_t
    {
        Write,      // drawn with depth test/write
        ReadSample, // read-only depth bound while the shader samples it (the depth fog, the 3D probe)
    };

    struct TargetDesc
    {
        uint32_t width = 0, height = 0; // pixels
        Format colour = Format::RGBA8;
        bool depth = true;      // D32 alongside, also readable (the depth fog); false: colour alone
        bool shared = false;    // can be handed to the window (the picture)
    };

    // The passes of the shared shader set that are not the GS's draws: a full-screen triangle (drawn with draw(3): the
    // corners made from the vertex index, no vertex input) or, for Cloud, a quad per instance. Their constants through
    // bindDrawConstants, their inputs t0-t2 through bindTextures; each writes the format its target has.
    enum class Effect : uint8_t
    {
        None,
        Fxaa,        // FXAA 3.11 quality: the picture's copy (t0) into the picture (RGBA8)
        SmaaEdges,   // SMAA 1x luma edges of the picture's copy (t0) (RG8; the target cleared first: no edge, no pixel)
        SmaaWeights, // the blend weights from the edges (t0) (RGBA8). SMAA's area and search tables are the effect's own:
                     // the device binds them (t1, t2) with the pipeline - leave those two slots with kKeepTexture
        SmaaBlend,   // the picture's copy (t0) blended by the weights (t1) into the picture (RGBA8)
        Cloud,       // the warp tunnel's sparks: a strip of 4 a point, the points per instance (setVertices or setMesh,
                     // draw(4, 0, count)), added into the target
    };

    struct PipelineDesc
    {
        BlendState blend;
        bool depthTest = false;
        Compare depthFunc = Compare::Always;
        bool depthWrite = false;
        uint32_t writeMask = kWriteAll;
        bool picture = false;    // drawn into the picture (the presentation pass shaders)
        bool clip = false;       // the clipping geometry stage
        uint32_t variant = 0u;   // pixel-shader variant (shader bits from the draw)
        uint32_t alphaTest = 0u; // the AID / ATST combination
        Effect effect = Effect::None; // one of the effects: the fields above not used
    };

    // The frame's streaming rings, each its own (each a frame slot's part, started again with the frame): the draws'
    // constants (256-byte aligned entries; the D3D12 device copies the frame's part into video memory before the
    // frame runs), the passes' instance entries (a vertex stream), and vertices / indices made this frame.
    enum class RingKind : uint8_t
    {
        Constants,
        Instances,
        Vertices,
    };

    // Where per-draw data went in a ring: what the builder wrote, and where the GPU reads it.
    struct RingSpan
    {
        void *cpu = nullptr;
        uint64_t gpu = 0; // device address / offset, opaque to the builder (a byte offset from it stays in the span)
        uint32_t size = 0;
        uint32_t first = 0; // (RingKind::Instances: the entry's index - draw()'s firstInstance)
    };

    struct Rect
    {
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        // --- frame life (two slots in flight) ---
        virtual double waitFrameSlot() = 0;                     // blocks until a slot is free; ms waited
        virtual bool beginFrame(uint64_t tick) = 0;             // reset the slot's list, bind root signature/heaps
        virtual void beginExtraPictures() = 0;                  // from here, the region replayed per moment picture
        virtual void endFrame(uint32_t pictures) = 0;           // close, submit (once per picture), signal the slot

        // --- targets (the builder keys them by GS address; the device only owns the surfaces) ---
        virtual TargetId createTarget(const TargetDesc &d) = 0; // 0 = failed
        virtual bool resizeTarget(TargetId t, const TargetDesc &d) = 0; // new surfaces, same handle and views (cleared)
        virtual void releaseTarget(TargetId t) = 0;             // freed once the frames using it are done
        virtual void clearColour(TargetId t, const float rgba[4]) = 0;
        virtual void clearDepth(TargetId t, float value) = 0;
        virtual void use(TargetId t, UseFor how) = 0;           // barrier to that state (no-op if already there)
        virtual void useDepth(TargetId t, DepthUse how) = 0;
        virtual TextureId view(TargetId t, bool depth = false) = 0; // the target as a sampled texture
        virtual TextureId snapshot(TargetId t) = 0;             // a copy of it as it is now (a target read while drawn into)

        // --- textures ---
        virtual TextureId createTexture(uint32_t width, uint32_t height, Format f, uint32_t levels) = 0;
        virtual bool uploadTexture(TextureId t, uint32_t level, const void *texels, uint32_t rowBytes) = 0; // on this frame
        // The texture made and uploaded off the drawing thread when the device gets to it (D3D12: its copy queue), the
        // texels read then from `source` - gone by then: skipped; urgent: before the large images. textureReady tells.
        virtual bool uploadTextureLater(TextureId t, std::weak_ptr<const Texture> source, bool urgent) = 0;
        virtual bool textureReady(TextureId t) = 0;             // its upload has reached the GPU
        virtual void releaseTexture(TextureId t) = 0;

        // --- meshes (the game's geometry sets, cached across frames) ---
        virtual uint32_t createMesh(const void *vertices, uint32_t vertexBytes, const void *indices, uint32_t indexBytes) = 0;
        virtual void releaseMesh(uint32_t meshId) = 0;

        // --- streaming ring (constants, vertices of immediate draws, instances) ---
        virtual bool ringAlloc(RingKind kind, uint32_t size, uint32_t align, RingSpan &out) = 0; // false: that ring is full this frame

        // --- state ---
        virtual void setTarget(TargetId colour, bool withDepth) = 0;
        virtual void setViewport(float x, float y, float w, float h) = 0;
        virtual void setScissor(const Rect &r) = 0;
        virtual bool setPipeline(const PipelineDesc &p) = 0;    // a pipeline not made yet is made inside; false: none (the draw left out)
        virtual void setBlendFactor(const float rgba[4]) = 0;
        virtual void bindTextures(TextureId t0, TextureId t1, TextureId t2) = 0; // the three sampled slots (kKeepTexture: unchanged)
        virtual void bindDrawConstants(const RingSpan &c) = 0;  // b0
        virtual void bindPictureConstants(const RingSpan &c) = 0; // b1

        // --- draws ---
        virtual void setVertices(const RingSpan &v, uint32_t stride) = 0; // ring vertices (immediate draws); else setMesh
        virtual void setIndices(const RingSpan &indices) = 0;    // their 32-bit indices, in the ring (after setMesh: in place of its own)
        // The frame before's vertices of a mesh made anew each frame (moment pictures: place, colour, size - 20 bytes each),
        // a vertex stream of their own; an empty span: none.
        virtual void setPreviousVertices(const RingSpan &v) = 0;
        virtual void setMesh(uint32_t meshId) = 0;               // a cached mesh's vertex + index buffers
        virtual void draw(uint32_t vertices, uint32_t first, uint32_t instances = 1u, uint32_t firstInstance = 0u) = 0;
        virtual void drawIndexed(uint32_t indices, uint32_t first, uint32_t instances = 1u, uint32_t firstInstance = 0u) = 0;
        virtual void drawIndirect(uint64_t groundMeshId, uint32_t firstInstance) = 0; // the GPU grass of that ground (Mesh::id): counts from the compute pass
        virtual void dispatchGrass(const RingSpan &params, uint32_t groups) = 0;

        // --- copies ---
        virtual void copy(TargetId dst, const Rect &dstRect, TargetId src, const Rect &srcRect) = 0; // region, same format
        virtual void copyWhole(TargetId dst, TargetId src) = 0;

        // --- queries / stats ---
        virtual QueryId timestamp() = 0;                         // GPU time stamps (the profile, pacing)
        virtual void beginOcclusion(QueryId q) = 0;              // the 3D-on-screen probe / predication
        virtual void endOcclusion(QueryId q) = 0;
        virtual void predicate(QueryId q, bool skipIfZero) = 0;  // skip a target's draws in extra pictures
        virtual bool readQueries(uint64_t frameTick, double &gpuMs) = 0;

        // --- the picture ---
        virtual void publish(TargetId picture, uint32_t width, uint32_t height) = 0; // to the window: shared texture / readback
    };

    Device &device(); // the backend in use (D3D12 today)
}
