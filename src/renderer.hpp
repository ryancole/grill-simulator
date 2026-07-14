#pragma once

#include "dx_common.hpp"
#include "environment.hpp"
#include "font.hpp"
#include "scene.hpp"
#include "viewmodel.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Raw Direct3D 12. No abstraction layer, no engine: the swapchain, the command
// allocators and the fence are all right here on purpose.
class Renderer {
public:
    static constexpr UINT kFrameCount = 2;
    // Bloom pyramid depth: this many successively-halved mips. Public so the render
    // target and descriptor heaps, sized in the .cpp, can account for them.
    static constexpr UINT kBloomLevels = 6;

    // One order on the top-right objective rail: the polished, non-debug readout of a
    // level's win condition. `name` is the loud header ("STEAK"), `band` the quiet
    // caption naming the accepted doneness window ("medium rare to medium"). `filled`
    // of `count` are done, shown as pips. The gauge draws `band_count` segments and
    // brightens the inclusive window [band_min, band_max]. Built by the caller from
    // Objectives, so the renderer stays free of the cook/level types.
    struct OrderCard {
        std::string name;
        std::string band;
        int filled = 0;
        int count = 1;
        int band_min = 0;
        int band_max = 0;
        int band_count = 1;
        // The band the meat the player is acting on currently sits at, drawn as a live
        // playhead over this card's gauge so the target can be lined up by eye. -1 draws
        // no marker: nothing of this order's type is held or looked at.
        int marker_band = -1;
    };

    // Brings up the device, swapchain and every pipeline -- everything that lives
    // for the whole session, independent of which level is loaded. No scene geometry
    // is uploaded here; call LoadScene once the first level's Scene exists.
    void Initialize(HWND hwnd, UINT width, UINT height);
    // Uploads one level's models, textures and reflection probe into the GPU
    // resources the frame draws from. Call after Initialize, and again after
    // ReleaseScene to swap in a different level. Assumes no scene is currently
    // loaded (ReleaseScene, or a fresh Initialize, left the slots empty).
    void LoadScene(const Scene& scene);
    // Frees the current level's models, textures and probe, so a different Scene
    // can be uploaded. Flushes the GPU first -- the frame in flight may still be
    // reading these -- so it is only for a between-levels swap, never mid-frame.
    // The device, swapchain, pipelines and shadow map outlive it; the font atlas
    // rides the scene upload and is simply re-loaded by the next LoadScene.
    void ReleaseScene();
    // Points the sun a level's way: re-aims the shadow map's orthographic light and
    // sets the direction the scene's direct term is lit from. The gradient sky
    // ignores it. Call before LoadScene so the reflection probe captures this sun.
    void SetSunDirection(DirectX::XMFLOAT3 direction);
    // Sets the level's sky and lighting -- the sun colour, sky gradient, clouds,
    // ambient, fog and shafts every pass shades with. Just records the value; the
    // per-frame passes stamp it into their constant buffers each frame, so it takes
    // effect on the next Render. Call before LoadScene so the reflection probe bakes
    // the level's sky, exactly as SetSunDirection is called for its sun.
    void SetEnvironment(const Environment& environment);
    void Resize(UINT width, UINT height);
    // `props` are the loose objects resting in the yard, drawn with the scene.
    // `highlight` is the one the player is aiming at, ringed with a glowing
    // outline; empty rings nothing. `viewmodel` and `held_props` are drawn last,
    // over a cleared depth buffer, so the player's arms and whatever they carry
    // are never sliced open by the wall they are standing against. `hud_prompt`
    // is the one centred line of HUD text laid over the finished frame; empty draws
    // nothing. `debug_lines` are the read-only debug overlay -- one line each,
    // anchored down the top-left corner; empty draws no overlay. `orders` are the
    // polished objective cards on the top-right rail; empty draws no rail.
    void Render(const Scene& scene, std::span<const MeshInstance> props,
                std::span<const MeshInstance> highlight, const ViewmodelPose& viewmodel,
                std::span<const MeshInstance> held_props,
                const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 camera_position,
                std::string_view hud_prompt, std::span<const std::string> debug_lines,
                std::span<const OrderCard> orders);
    // Draws the launch/pause menu as its own complete frame: a solid backdrop, a
    // `title`, and the vertical list of `entries` with the one at `selected` picked
    // out. Owns the swapchain buffer from clear to present, so the game loop calls
    // this *instead of* Render while the menu is up -- no level is stepped or drawn
    // behind it. Reads the font atlas from the per-level texture heap, so a level
    // must be loaded (its geometry simply is not drawn).
    void RenderMenu(std::string_view title, std::span<const std::string> entries, int selected);
    // Hit-tests the menu layout: the entry index whose row the client-space point
    // (x, y) falls in for a menu of `entry_count` entries, or -1 when the point is
    // over no entry. Uses the same metrics RenderMenu draws with, so hover and click
    // line up with what is on screen. Rows span the full width, so only y matters.
    int MenuEntryAt(int x, int y, int entry_count) const;
    void Shutdown();

    float AspectRatio() const;

private:
    // One draw: a slice of its model's index buffer, the base colour the glTF
    // material asked for, the descriptor of the texture that modulates it, and the
    // descriptor of its tangent-space normal map (the flat 1x1 default when the
    // material has none).
    struct DrawPrimitive {
        DirectX::XMFLOAT4X4 transform;
        UINT first_index;
        UINT index_count;
        DirectX::XMFLOAT3 base_color;
        float metallic;
        float roughness;
        D3D12_GPU_DESCRIPTOR_HANDLE base_color_texture;
        D3D12_GPU_DESCRIPTOR_HANDLE normal_texture;
        D3D12_GPU_DESCRIPTOR_HANDLE metallic_roughness_texture;
        D3D12_GPU_DESCRIPTOR_HANDLE occlusion_texture;
    };

    // One level of the bloom pyramid: an HDR texture at some fraction of the window,
    // with its render-target view in rtv_heap_ and its shader-resource view in
    // engine_heap_. The chain is recreated on resize, since the sizes track the
    // window.
    struct BloomTarget {
        ComPtr<ID3D12Resource> texture;
        UINT width = 0;
        UINT height = 0;
    };

    // A Model, uploaded. The CPU-side Model that produced it is not needed again.
    struct GpuModel {
        ComPtr<ID3D12Resource> vertex_buffer;
        D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
        ComPtr<ID3D12Resource> index_buffer;
        D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
        std::vector<DrawPrimitive> primitives;
    };

    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, UINT width, UINT height);
    void CreateRenderTargetViews();
    void CreateDepthBuffer();
    // The shader-visible heap for session-level SRVs -- the ones that outlive a
    // level swap, kept apart from the per-level material heap (texture_heap_) that
    // ReleaseScene tears down. Created once at startup; slot 0 is the HDR scene
    // buffer, with room reserved for the post-process buffers to come.
    void CreateEngineDescriptorHeap();
    // The HDR scene buffer the whole world renders into, in linear light: an
    // R16G16B16A16_FLOAT target the size of the window, with a render-target view in
    // rtv_heap_ and a shader-resource view in engine_heap_. Recreated on resize like
    // the depth buffer; the tonemap pass reads it and writes the swapchain.
    void CreateHdrTarget();
    // The sun's depth buffer and everything the shadow pass draws it with: a
    // square R32_TYPELESS texture with a D32 depth view to render into and an
    // R32_FLOAT resource view to sample back, plus its own viewport. Fixed size,
    // so unlike the scene depth buffer it survives a window resize untouched.
    void CreateShadowMap();
    // The depth-only pipeline that fills the shadow map, its root signature, the
    // sun's orthographic light view-projection, and the per-frame constant buffer
    // that hands that matrix to the scene pass.
    void CreateShadowPipeline();
    // Rebuilds light_view_projection_ from sun_direction_: an orthographic box
    // aimed down the sun and centred on the yard. Called at startup and whenever
    // SetSunDirection re-aims the sun for a new level.
    void UpdateShadowProjection();
    void CreatePipeline();
    // The gradient-sky background: a fullscreen pass whose pixel shader turns each
    // pixel into a world-space view ray and samples the same analytic sky the
    // scene reflects. Its own tiny root signature (just the inverse view-projection
    // and the eye) and PSO with depth off.
    void CreateSkyPipeline();
    // The inverted-hull outline pass: its own root signature and PSO that grow a
    // mesh along its normals, cull the near faces and paint the far shell a flat
    // glowing colour. Depth-tests against the world but writes no depth.
    void CreateOutlinePipeline();
    // The resolve pass: a fullscreen pixel shader that tonemaps the linear HDR
    // scene buffer and encodes it to sRGB for the swapchain. Its own tiny root
    // signature (an exposure constant and the HDR buffer's SRV) and a PSO with depth
    // off, targeting the back buffer.
    void CreateTonemapPipeline();
    // The volumetric sun-shaft pass: a fullscreen pixel shader that marches the sun's
    // shadow map along each view ray and adds the scattered light into the HDR buffer
    // before the resolve. Its own root signature (the camera and light matrices, the
    // sun, and the depth + shadow SRVs) and an additively blended PSO.
    void CreateLightShaftPipeline();
    // The bloom pyramid's textures: kBloomLevels HDR targets, each half the size of
    // the one above, with their RTVs and SRVs. Recreated on resize like the HDR
    // buffer, since the sizes follow the window.
    void CreateBloomTargets();
    // The bloom pass pipelines: one root signature and two PSOs (the downsample,
    // which overwrites, and the upsample, which blends additively) over bloom.hlsl.
    void CreateBloomPipeline();
    // Runs the bloom chain on the finished HDR scene: downsample down the pyramid
    // thresholding the first level, then upsample and sum back up, leaving the glow
    // in bloom level 0 for the resolve to add in. Assumes the HDR buffer is a shader
    // resource and the engine heap is bound.
    void RenderBloom();
    // Uploads every model the scene holds, plus the 1x1 white texture that
    // stands in for a material with no texture of its own.
    void CreateSceneGeometry(const Scene& scene);

    // Renders the static yard into a cubemap once, from a fixed point at its
    // centre, so the scene pass can reflect the real fence and trees off a metal
    // rather than only the analytic sky. Six faces, each the scene lit by the
    // analytic sky (the capture pixel shader), with the gradient sky behind. Runs
    // after CreateSceneGeometry, on the same one-shot command list.
    void CaptureReflectionProbe(const Scene& scene);

    // The second, tiny pipeline: the alpha-blended, depth-free pass that draws
    // HUD text from the MSDF atlas. Builds its root signature, PSO and the
    // per-frame dynamic vertex buffer the glyph quads are written into.
    void CreateTextPipeline();
    // Loads the font's glyph metrics and uploads its atlas into the shared
    // texture heap. Shares the scene upload's open command list and staging list.
    void LoadFontAtlas(std::vector<ComPtr<ID3D12Resource>>& staging);
    // Loads one baked face -- its glyph CSV into `font` and its .png atlas into
    // `texture` at heap slot `descriptor`, recording the atlas pixel size in
    // `width`/`height`. Shares the open command list and `staging` list. Both HUD
    // faces (Inter, monospace) come up through this from LoadFontAtlas.
    void LoadFontFace(const std::filesystem::path& csv, const std::filesystem::path& png,
                      UINT descriptor, Font& font, ComPtr<ID3D12Resource>& texture, UINT& width,
                      UINT& height, std::vector<ComPtr<ID3D12Resource>>& staging);

    // One baked MSDF face for the text pass: its glyph metrics and the atlas it
    // samples (the SRV slot plus the texture's pixel size, needed for UVs and the
    // antialiasing range). LayoutLine and DrawTextRun take one, so the same code
    // draws either the Inter HUD face or the monospace debug face.
    struct FontFace {
        const Font* font;
        UINT atlas_descriptor;
        UINT atlas_width;
        UINT atlas_height;
    };
    FontFace HudFace() const { return {&font_, atlas_descriptor_, atlas_width_, atlas_height_}; }
    FontFace MonoFace() const {
        return {&mono_font_, mono_atlas_descriptor_, mono_atlas_width_, mono_atlas_height_};
    }
    // Draws the in-game HUD over the resolved frame: the centred `prompt` near the
    // bottom, and the `debug_lines` overlay anchored down the top-left corner. Both
    // are packed into this frame's text region back-to-back before either is drawn,
    // so they never alias in the shared buffer the way two DrawText calls would (see
    // DrawMenu). A no-op when all are empty. Assumes the render target is bound. The
    // `orders` rail, when non-empty, is packed into the same region and drawn as a
    // stack of cards down the top-right corner (see DrawObjectivesRail).
    void DrawHud(std::string_view prompt, std::span<const std::string> debug_lines,
                 std::span<const OrderCard> orders);
    // One packed run of the HUD buffer: a vertex range that is either a solid fill (a
    // panel, a gauge segment, a pip) or a line of glyphs from `face`, tinted `color`.
    // DrawHud and DrawObjectivesRail pack a list of these into the shared region and
    // then draw them in order, so nothing aliases and panels sit behind their text.
    struct HudRun {
        UINT first;
        UINT count;
        DirectX::XMFLOAT4 color;
        bool solid;     // A flat fill (panel/gauge/pip) rather than glyphs.
        FontFace face;  // Which atlas the glyphs draw from (ignored when solid).
    };
    // Packs the top-right objective rail into the shared text region, appending its
    // panel/gauge/pip/label runs to `runs` and advancing `cursor`. Split out of DrawHud
    // for legibility only; it must pack into the same cursor so its quads never alias
    // the prompt's or the debug overlay's in this frame's buffer.
    void DrawObjectivesRail(std::span<const OrderCard> orders, std::vector<HudRun>& runs,
                            UINT& cursor);
    // The pixel advance of `text` set from `face` at glyph height `pixel` -- the same
    // sum LayoutLine walks -- so a caller can right-anchor a line (left_px = right edge
    // minus this) or size a box to its text. Unknown glyphs contribute nothing.
    float TextWidth(const FontFace& face, std::string_view text, float pixel) const;
    // Draws the menu overlay -- the title and the highlighted list of entries --
    // into the current frame, over whatever backdrop the caller has cleared. All
    // lines are packed into this frame's text vertex region back-to-back and only
    // then drawn, so they never alias one another in the shared buffer the way
    // repeated DrawText calls would.
    void DrawMenu(std::string_view title, std::span<const std::string> entries, int selected);

    // Writes the glyph quads for one line into this frame's text vertex region,
    // beginning at vertex index `first`, and returns the new running vertex count.
    // `baseline` and `pixel` (the glyph height) are in pixels, origin top-left. It
    // only fills the buffer -- the caller picks the colour and issues the draw -- so
    // several lines can share one fill before any is drawn. Stops early if the region
    // fills; empty text writes nothing. `left_px` >= 0 left-anchors the line at that
    // x (the debug overlay); a negative value, the default, centres it horizontally
    // (the prompt and menu). `face` picks which baked font to lay the glyphs from.
    UINT LayoutLine(const FontFace& face, std::string_view text, float baseline, float pixel,
                    UINT first, float left_px = -1.0f);
    // Writes one solid rectangle (two triangles) into this frame's text vertex
    // region at vertex `first`, its corners the pixel box [x0,y0]-[x1,y1] (origin
    // top-left), and returns the new running vertex count. Drawn with the text
    // pipeline's solid mode, it backs the debug overlay with a translucent panel.
    // Stops without writing if the region is full.
    UINT LayoutSolidQuad(float x0, float y0, float x1, float y1, UINT first);
    // Binds the text pipeline, its root signature, a default atlas table and this
    // frame's text vertex buffer, ready for one or more DrawTextRun calls (each of
    // which rebinds the atlas to its own face).
    void BindTextPipeline();
    // Issues the shadow-then-fill draw pair for the run of `count` vertices starting
    // at `first`, drawn from `face`'s atlas and tinted `color` (the shadow is a
    // translucent black scaled by the fill's alpha). Assumes BindTextPipeline has run.
    void DrawTextRun(const FontFace& face, UINT first, UINT count, DirectX::XMFLOAT4 color);
    // Draws a run of solid-mode vertices (from LayoutSolidQuad) filled flat with
    // `color` -- the debug panel. No shadow pass; assumes BindTextPipeline has run.
    void DrawSolidRun(UINT first, UINT count, DirectX::XMFLOAT4 color);

    // Fills a default-heap buffer through a staging copy. The staging resource is
    // appended to `staging`, which the caller must keep alive until the GPU has
    // retired the copy. `access_after` is the buffer access the copy is followed by
    // -- vertex or index -- for the barrier that hands the buffer to the draw.
    ComPtr<ID3D12Resource> UploadBuffer(const void* data, UINT64 bytes,
                                        D3D12_BARRIER_ACCESS access_after,
                                        std::vector<ComPtr<ID3D12Resource>>& staging);
    // Uploads a mip chain and writes its shader resource view into slot
    // `descriptor` of the texture heap. `format` is sRGB for base-colour images
    // and plain _UNORM for data textures (normal, metallic-roughness, atlas).
    ComPtr<ID3D12Resource> UploadTexture(const Image& image, UINT descriptor, DXGI_FORMAT format,
                                         std::vector<ComPtr<ID3D12Resource>>& staging);
    D3D12_GPU_DESCRIPTOR_HANDLE TextureHandle(UINT descriptor) const;

    // One draw per primitive of each instance's model, each under its own root
    // constants. `shadow_receive` is 1 for the world, which is shadowed by the
    // sun, and 0 for the viewmodel, which is not.
    // `bind_probe` binds the reflection cubemap for the pass to sample. The
    // on-screen pass wants it; the probe-capture pass must not (it is filling that
    // cube and its pixel shader never reads it), so it passes false.
    void DrawInstances(std::span<const MeshInstance> instances,
                       const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 sun_direction,
                       float shadow_receive, bool bind_probe);

    // Fills the currently bound render target with the gradient sky, seen from
    // `camera_position` through `view_projection`. Draws no depth, so geometry
    // rendered afterward paints over it. Switches to the sky pipeline and root
    // signature; the caller restores whatever it needs next.
    // `capture` picks the pixel shader: the on-screen pass leaves its radiance
    // linear for the HDR buffer, the probe capture encodes to sRGB for the 8-bit
    // cube. `time` drifts the cloud layer; the capture passes 0 for a still probe.
    void DrawSky(const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 camera_position,
                 float time, bool capture);

    // The shadow pass: draws every caster depth-only into the shadow map from the
    // sun's point of view, wrapped in the barriers that flip the map between
    // depth target and shader resource.
    void RenderShadowMap(const Scene& scene, std::span<const MeshInstance> props);
    // One depth-only draw per primitive, under the shadow pipeline's lone root
    // constant: the caster's model-to-light-clip matrix.
    void DrawShadowCasters(std::span<const MeshInstance> instances);

    // Paints the glowing halo around each instance, with depth off so it wraps
    // every side evenly; `seconds` drives the pulse. The caller re-paints the
    // instance afterward to carve it back out of the halo. Assumes the render
    // target is still bound.
    void DrawOutlines(std::span<const MeshInstance> instances,
                      const DirectX::XMMATRIX& view_projection, float seconds);

    // Blocks until the GPU has retired every frame. Only for teardown, resize
    // and the one-off geometry upload; the steady-state path is MoveToNextFrame.
    void FlushGpu();
    // Signals the current frame and waits only if the frame we are about to
    // reuse is still in flight.
    void MoveToNextFrame();

    ComPtr<IDXGIFactory6> factory_;
    // Device10 and CommandList7 are the enhanced-barrier era: the device for
    // CreateCommittedResource3 (resources born with an explicit barrier layout), the
    // command list for Barrier(). Both inherit the older interfaces, so nothing else
    // that uses them has to change.
    ComPtr<ID3D12Device10> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;

    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_size_ = 0;
    ComPtr<ID3D12Resource> render_targets_[kFrameCount];

    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    UINT dsv_size_ = 0;
    ComPtr<ID3D12Resource> depth_stencil_;

    // The linear HDR scene buffer: the whole world draws into this, and the tonemap
    // pass resolves it to the swapchain. Its RTV is the last slot of rtv_heap_
    // (after the swapchain buffers); its SRV is slot 0 of engine_heap_.
    ComPtr<ID3D12Resource> hdr_target_;

    // The persistent, shader-visible heap for session-level SRVs (the HDR buffer,
    // and the post-process buffers to come). Kept separate from texture_heap_ so a
    // level swap, which rebuilds that per-level heap, never disturbs these.
    ComPtr<ID3D12DescriptorHeap> engine_heap_;
    UINT engine_heap_size_ = 0;

    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList7> command_list_;

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;

    // The gradient-sky background pass. No vertex buffer, no textures: the pixel
    // shader reconstructs the view ray from the inverse view-projection handed in
    // as root constants. `sky_capture_pipeline_state_` is the same pass encoding to
    // sRGB into the 8-bit probe cube, where the on-screen one leaves its radiance
    // linear for the HDR buffer.
    ComPtr<ID3D12RootSignature> sky_root_signature_;
    ComPtr<ID3D12PipelineState> sky_pipeline_state_;
    ComPtr<ID3D12PipelineState> sky_capture_pipeline_state_;

    // The resolve pass: tonemaps the HDR scene buffer and encodes it to the
    // swapchain. Reads engine_heap_ slot 0; no vertex buffer (a fullscreen triangle
    // from SV_VertexID).
    ComPtr<ID3D12RootSignature> tonemap_root_signature_;
    ComPtr<ID3D12PipelineState> tonemap_pipeline_state_;

    // The volumetric sun-shaft pass, additively blended into the HDR buffer. Reads
    // the scene depth and shadow map from engine_heap_ slots 1 and 2.
    ComPtr<ID3D12RootSignature> light_shaft_root_signature_;
    ComPtr<ID3D12PipelineState> light_shaft_pipeline_state_;

    // The bloom pyramid: its mip textures, one root signature and the downsample /
    // upsample PSOs. Level 0's SRV is what the resolve reads to add the glow.
    BloomTarget bloom_targets_[kBloomLevels];
    ComPtr<ID3D12RootSignature> bloom_root_signature_;
    ComPtr<ID3D12PipelineState> bloom_downsample_pipeline_state_;
    ComPtr<ID3D12PipelineState> bloom_upsample_pipeline_state_;

    // The reflection probe. `scene_capture_pipeline_state_` is the scene PSO with
    // the capture pixel shader (analytic sky, no cube read); it fills probe_cube_
    // through the six face RTVs in probe_rtv_heap_, depth-tested against
    // probe_depth_. The finished cube's SRV lives in texture_heap_ at
    // probe_descriptor_. All built once, at startup.
    ComPtr<ID3D12PipelineState> scene_capture_pipeline_state_;
    ComPtr<ID3D12Resource> probe_cube_;
    ComPtr<ID3D12DescriptorHeap> probe_rtv_heap_;
    ComPtr<ID3D12Resource> probe_depth_;
    UINT probe_descriptor_ = 0;

    // The pick-up outline pass. Shares the scene's vertex buffers and input
    // layout; only the root signature and PSO differ.
    ComPtr<ID3D12RootSignature> outline_root_signature_;
    ComPtr<ID3D12PipelineState> outline_pipeline_state_;

    // The shadow pass. The map lives in dsv_heap_ slot 1 as a depth view and in
    // texture_heap_ at shadow_descriptor_ as an SRV; the light view-projection is
    // fixed (the sun does not move) and rides to the scene pass in frame_constants_.
    ComPtr<ID3D12RootSignature> shadow_root_signature_;
    ComPtr<ID3D12PipelineState> shadow_pipeline_state_;
    ComPtr<ID3D12Resource> shadow_map_;
    UINT shadow_descriptor_ = 0;
    // The unit vector toward the sun, normalized. Set from the loaded level (default
    // to the backyard's in CreateShadowPipeline); drives both the shadow projection
    // below and the scene's direct-light term.
    DirectX::XMFLOAT3 sun_direction_{};
    DirectX::XMFLOAT4X4 light_view_projection_{};
    // The current level's sky and lighting, stamped into every pass's constant
    // buffer each frame (see the ApplyEnvironment overloads). Defaults to the look
    // the shaders once baked in, so the first frames before any level loads -- and
    // the reflection probe's own default capture -- are drawn as they always were.
    Environment environment_ = kDefaultEnvironment;
    // One aligned FrameConstants region per frame in flight, kept mapped and
    // rewritten each frame with the sun matrix and the current eye position. Per
    // frame because the camera moves, and buffered per frame so writing this
    // frame's copy cannot trample the one the GPU is still reading for the last.
    ComPtr<ID3D12Resource> frame_constants_;
    std::byte* frame_constants_mapped_ = nullptr;
    UINT frame_constants_stride_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS frame_constants_address_ = 0;
    D3D12_VIEWPORT shadow_viewport_{};
    D3D12_RECT shadow_scissor_{};

    // The HUD text pass. The atlas SRV lives in the shared texture_heap_ at
    // atlas_descriptor_; the vertex buffer is one upload-heap region per frame in
    // flight, kept mapped, rewritten with the frame's glyph quads.
    ComPtr<ID3D12RootSignature> text_root_signature_;
    ComPtr<ID3D12PipelineState> text_pipeline_state_;
    ComPtr<ID3D12Resource> atlas_texture_;
    UINT atlas_descriptor_ = 0;
    UINT atlas_width_ = 0;
    UINT atlas_height_ = 0;
    Font font_;
    // A second baked face, monospace, drawn only by the debug overlay so its columns
    // line up; the prompt and menu stay on font_ (Inter). Its atlas rides the same
    // texture_heap_ at mono_atlas_descriptor_, right after font_'s.
    ComPtr<ID3D12Resource> mono_atlas_texture_;
    UINT mono_atlas_descriptor_ = 0;
    UINT mono_atlas_width_ = 0;
    UINT mono_atlas_height_ = 0;
    Font mono_font_;
    ComPtr<ID3D12Resource> text_vertex_buffer_;
    std::byte* text_vertex_mapped_ = nullptr;

    // Shader-visible, and the only descriptor heap the game binds. Slot 0 is the
    // white texture; every glTF image follows it.
    ComPtr<ID3D12DescriptorHeap> texture_heap_;
    UINT texture_size_ = 0;
    std::vector<ComPtr<ID3D12Resource>> textures_;

    std::vector<GpuModel> models_;

    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_values_[kFrameCount]{};
    UINT frame_index_ = 0;

    // When the renderer came up, so the outline's glow can pulse against a
    // wall-clock rather than needing a delta threaded through Render.
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    UINT width_ = 0;
    UINT height_ = 0;
    bool allow_tearing_ = false;
    bool initialized_ = false;
};
