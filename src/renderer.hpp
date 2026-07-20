#pragma once

#include "dx_common.hpp"
#include "environment.hpp"
#include "flow_volume.hpp"
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

    // One line of the top-right orders list: the polished, non-debug readout of one of a
    // level's required deliveries. `name` is the meat ("STEAK"), `band` the wanted
    // doneness in words ("medium rare to medium"), and `count` how many are wanted (shown
    // as an "x2" suffix when more than one). No progress or pass/fail state -- the list
    // just names the standing orders. Built by the caller from Objectives, so the renderer
    // stays free of the cook/level types.
    struct OrderCard {
        std::string name;
        std::string band;
        int count = 1;
    };

    // One meat on the top-left status rail: the always-on, player-facing readout of a
    // cooking piece of food -- the polished twin of the debug overlay's meat lines.
    // `name` is the loud header ("STEAK"), `band` the quiet caption naming its current
    // doneness ("medium rare"), and `temp` its current internal temperature preformatted
    // for display ("139F"), shown small beside the caption. The gauge draws `band_count`
    // segments and fills through `band_index`, the current doneness. `served` fades the
    // card back once the meat has been handed off. Built by the caller from Props, so the
    // renderer stays free of the cook types, exactly as OrderCard keeps it free of the
    // level types.
    struct MeatCard {
        std::string name;
        std::string band;
        std::string temp;
        int band_index = 0;
        int band_count = 1;
        bool served = false;
    };

    // Brings up the device, swapchain and every pipeline -- everything that lives
    // for the whole session, independent of which level is loaded. No scene geometry
    // is uploaded here; call LoadScene once the first level's Scene exists.
    void Initialize(HWND hwnd, UINT width, UINT height);
    // Uploads one level's models, textures and acceleration structures into the GPU
    // resources the frame draws from. Call after Initialize, and again after
    // ReleaseScene to swap in a different level. Assumes no scene is currently
    // loaded (ReleaseScene, or a fresh Initialize, left the slots empty).
    void LoadScene(const Scene& scene);
    // Frees the current level's models, textures and structures, so a different Scene
    // can be uploaded. Flushes the GPU first -- the frame in flight may still be
    // reading these -- so it is only for a between-levels swap, never mid-frame.
    // The device, swapchain, pipelines and shadow map outlive it; the font atlas
    // rides the scene upload and is simply re-loaded by the next LoadScene.
    void ReleaseScene();
    // Points the sun a level's way: re-aims the shadow map's orthographic light and
    // sets the direction the scene's direct term is lit from. The gradient sky
    // ignores it.
    void SetSunDirection(DirectX::XMFLOAT3 direction);
    // Sets the level's sky and lighting -- the sun colour, sky gradient, clouds,
    // ambient, fog and shafts every pass shades with. Just records the value; the
    // per-frame passes stamp it into their constant buffers each frame, so it takes
    // effect on the next Render.
    void SetEnvironment(const Environment& environment);
    // A level's grass field: a flat rectangle of procedurally grown blades. `center`
    // is its middle on the ground (y the height the blades stand on), `size` its extent
    // in metres (x by z), `color` the base blade colour (sRGB), and `wind` the world-
    // space breeze the blades sway in. Plain data a level fills, mirrored from the
    // toml's `grass` table.
    struct GrassPatch {
        DirectX::XMFLOAT3 center{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT2 size{20.0f, 20.0f};
        DirectX::XMFLOAT3 color{0.33f, 0.5f, 0.18f};
        float blade_height = 0.35f;
        float blade_width = 0.03f;
        DirectX::XMFLOAT2 wind{0.15f, 0.05f};
    };
    // Sets the level's grass, grown by the grass pass each frame; call on level load
    // like SetEnvironment. `obstacles` are the world's collider boxes (from the Scene):
    // the grass keeps clear of any that rise into the sward, so blades do not poke up
    // through the patio, benches or props. The renderer filters them by height itself --
    // the ground plane and high tree canopies are ignored -- so the caller can just hand
    // over every static collider and dynamic body it has. ClearGrass drops the field, so
    // a level with no grass leaves none of the previous level's lingering. Both are no-ops
    // in effect where the device has no mesh-shader support, since the pass never runs.
    void SetGrass(const GrassPatch& grass, std::span<const OrientedBox> obstacles);
    void ClearGrass();
    // Positions the NVIDIA Flow simulation box for the loaded level: `center` its middle in
    // world space, `half_extent` its half-size in metres. A level sets this around wherever
    // its fires burn (the grill grate, the fire pit) so their smoke is simulated; fire
    // outside the box is not. Call on level load, like SetEnvironment. Forwards to the Flow
    // subsystem, which repositions the grid if it is already running.
    void SetFlowRegion(DirectX::XMFLOAT3 center, float half_extent);
    void Resize(UINT width, UINT height);
    // `props` are the loose objects resting in the yard, drawn with the scene.
    // `highlight` is the one the player is aiming at, ringed with a glowing
    // outline; empty rings nothing. `viewmodel` and `held_props` are drawn last,
    // over a cleared depth buffer, so the player's arms and whatever they carry
    // are never sliced open by the wall they are standing against. `hud_prompt`
    // is the one centred line of HUD text laid over the finished frame; empty draws
    // nothing. `debug_lines` are the read-only debug overlay -- one line each,
    // anchored up from the bottom-left corner; empty draws no overlay. `orders` are the
    // bulleted order lines on the top-right rail; empty draws no rail. `meats` are the
    // always-on status cards on the top-left rail -- one per cooking meat, showing its
    // doneness and temperature; empty draws no rail.
    void Render(const Scene& scene, std::span<const MeshInstance> props,
                std::span<const MeshInstance> highlight, const ViewmodelPose& viewmodel,
                std::span<const MeshInstance> held_props,
                const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 camera_position,
                std::string_view hud_prompt, std::span<const std::string> debug_lines,
                std::span<const OrderCard> orders, std::span<const MeatCard> meats,
                const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection, float flow_dt,
                std::span<const FlowEmitter> flow_emitters,
                std::span<const DirectX::XMFLOAT4> droplets);
    // Draws the launch/pause menu as its own complete frame: a solid backdrop, a
    // `title`, and the vertical list of `entries` with the one at `selected` picked
    // out. Owns the swapchain buffer from clear to present, so the game loop calls
    // this *instead of* Render while the menu is up -- no level is stepped or drawn
    // behind it. Reads the font atlas from the heap's per-level region, so a level
    // must be loaded (its geometry simply is not drawn).
    void RenderMenu(std::string_view title, std::span<const std::string> entries, int selected);
    // Hit-tests the menu layout: the entry index whose row the client-space point
    // (x, y) falls in for a menu of `entry_count` entries, or -1 when the point is
    // over no entry. Uses the same metrics RenderMenu draws with, so hover and click
    // line up with what is on screen. Rows span the full width, so only y matters.
    int MenuEntryAt(int x, int y, int entry_count) const;

    // Draws the loading screen as its own complete frame, like RenderMenu: the menu
    // backdrop, a `title` ("LOADING") and a quieter `subtitle` (the level's name) beneath
    // it. Owns the swapchain buffer from clear to present. The game loop draws this once
    // and presents it, so it is the frame left on screen while the following blocking
    // level build stalls the thread. Reads the font atlas from the heap's per-level region,
    // so a level must be loaded (the outgoing one, whose geometry simply is not drawn).
    void RenderLoading(std::string_view title, std::string_view subtitle);

    // One line of the level-complete breakdown: an order's readout (e.g. "STEAK -- medium
    // rare to medium") and whether the turned-in tray met it. `met` colours the line so a
    // filled order and a missed one read apart at a glance. Built by the caller from
    // Objectives, keeping the renderer free of the level types.
    struct ResultLine {
        std::string text;
        bool met = false;
    };
    // Draws the level-complete results screen as its own complete frame, like RenderMenu:
    // a backdrop, a `title`, the order-by-order `lines` breakdown (green for met, red for
    // missed), and the selectable `actions` (Replay / Back to Menu) with the one at
    // `selected` picked out. `passed` tints the title -- amber-green on a clear, red on a
    // miss. Owns the swapchain from clear to present; the loop calls it instead of Render
    // while the results are up.
    void RenderResults(std::string_view title, bool passed, std::span<const ResultLine> lines,
                       std::span<const std::string> actions, int selected);
    // Hit-tests the results actions: the action index the client-space point (x, y) falls
    // in for `action_count` actions, or -1 when over none. Shares RenderResults's metrics
    // so hover and click line up. Rows span the full width, so only y matters.
    int ResultsActionAt(int x, int y, int action_count) const;
    // Draws the keybinds screen as its own complete frame, like RenderMenu: a backdrop,
    // a `title`, then one row per entry laid out in two columns -- `labels[i]` left,
    // `values[i]` right (the action's current key). The row at `selected` is picked out
    // in amber; while `capturing` is set, that row's value is replaced with a "press a
    // key" prompt. `labels` and `values` are parallel and equal length; a value may be
    // empty for a plain full-width row (the Reset / Back actions carry no key).
    void RenderKeybinds(std::string_view title, std::span<const std::string> labels,
                        std::span<const std::string> values, int selected, bool capturing);
    // Hit-tests the keybinds layout: the row index the client-space point (x, y) falls
    // in for `row_count` rows, or -1 when over none. Shares RenderKeybinds's metrics so
    // hover and click line up with what is drawn. Rows span the full width, so only y
    // matters.
    int KeybindRowAt(int x, int y, int row_count) const;
    void Shutdown();

    float AspectRatio() const;

    // True when the device reports mesh-shader tier 1 or better (D3D12 OPTIONS7).
    // Feature-level 12.0 hardware may or may not have this, so anything that wants
    // to take the mesh-shader path must check here and fall back otherwise.
    bool MeshShadersSupported() const { return mesh_shaders_supported_; }

private:
    // One draw: a slice of its model's index buffer, the base colour the glTF
    // material asked for, and the heap slots of the four textures that modulate it --
    // the base colour, its tangent-space normal map, the metallic-roughness map and
    // the occlusion map (each the white or flat-normal 1x1 default when the material
    // has none). The scene pixel shader reads them bindlessly via ResourceDescriptorHeap,
    // so these are plain descriptor-heap indices, not bound GPU handles.
    struct DrawPrimitive {
        DirectX::XMFLOAT4X4 transform;
        UINT first_index;
        UINT index_count;
        DirectX::XMFLOAT3 base_color;
        float metallic;
        float roughness;
        UINT base_color_texture;
        UINT normal_texture;
        UINT metallic_roughness_texture;
        UINT occlusion_texture;
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
    // `blas` is its bottom-level raytracing acceleration structure -- one geometry per
    // primitive, each baking that primitive's model-space transform in -- so the scene's
    // reflection rays can intersect this model. Built once at scene load, referenced by
    // the TLAS's per-instance entries; null until BuildAccelerationStructures runs.
    struct GpuModel {
        ComPtr<ID3D12Resource> vertex_buffer;
        D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
        ComPtr<ID3D12Resource> index_buffer;
        D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
        std::vector<DrawPrimitive> primitives;
        ComPtr<ID3D12Resource> blas;
        // Where this model's geometry and primitives live for the reflection hit shader:
        // the byte-address SRV slots of its vertex and index buffers, and the base of its
        // primitives in the RtPrimInfo array. Filled by BuildAccelerationStructures; used
        // each frame by UpdateTopLevelAS to build a hit-info entry for every instance of
        // this model, static or moving.
        UINT vb_srv = 0;
        UINT ib_srv = 0;
        UINT prim_base = 0;
    };

    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, UINT width, UINT height);
    void CreateRenderTargetViews();
    void CreateDepthBuffer();
    // The one shader-visible CBV/SRV/UAV heap, created once at startup. Its front
    // block holds the session SRVs that outlive a level swap (slot 0 the HDR scene
    // buffer, then depth/shadow/bloom/fluid); the per-level material, atlas, shadow
    // and raytracing descriptors follow, rewritten in place on each swap. Bound once and
    // never swapped mid-frame, which is what the scene pass's bindless fetches need.
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
    // The grass pass's mesh-shader pipeline and root signature. Built at startup only
    // where the device reports mesh-shader tier 1; on hardware without it the pass is
    // never created and RenderGrass never runs.
    void CreateGrassPipeline();
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
    // The volumetric cloud pass: a fullscreen pixel shader that marches a density slab
    // overhead, lit by a short march toward the sun, and composites it over the sky
    // where the scene depth is the far plane. Its own root signature (the camera and
    // sky, and the scene depth SRV) and a premultiplied-over blended PSO.
    void CreateCloudPipeline();
    // The lighter-fluid spray's three pipelines and their root signatures: the impostor
    // surface pass (nearest eye-depth + thickness, MRT), the separable bilateral depth
    // blur, and the dual-source composite into the HDR buffer. Also creates the per-frame
    // droplet upload buffer (a root SRV). The offscreen targets it draws into are made by
    // CreateFluidTargets, which is window-sized and so runs again on resize.
    void CreateFluidPipeline();
    // The screen-space fluid's offscreen render targets, recreated on resize like the HDR
    // buffer and the bloom mips: the surface depth, its blurred copy and the thickness,
    // each with a render-target view in rtv_heap_ and a shader-resource view in
    // engine_heap_.
    void CreateFluidTargets();
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

    // Builds the raytracing acceleration structures the scene pass reflects off: a
    // bottom-level AS per model (BuildAccelerationStructures fills each GpuModel::blas)
    // and one top-level AS over the yard's static instances, tlas_. Runs at scene load
    // after CreateSceneGeometry, on its own one-shot command list. The TLAS gets a
    // shader-visible SRV at tlas_descriptor_, which the scene pixel shader reads
    // bindlessly to trace reflection rays. Static for now -- rebuilt whole on a level
    // swap; moving props are not yet in the TLAS.
    void BuildAccelerationStructures(const Scene& scene);
    // Rebuilds the top-level AS for this frame: fills the current instance-descs region
    // with the yard's static instances plus the loose `props` (each pointing at its model's
    // BLAS with its world transform), fills the matching per-instance hit-info region, and
    // builds the TLAS on the render command list before the scene pass reads it. Called at
    // the top of Render, so carried and toppled props -- and the browning meat, whose tint
    // rides along -- reflect where they actually are this frame.
    void UpdateTopLevelAS(const Scene& scene, std::span<const MeshInstance> props);
    // Creates a default-heap buffer sized for an acceleration structure or its build
    // scratch (both need ALLOW_UNORDERED_ACCESS). Born layout-less like every other
    // buffer; the build is its first access.
    ComPtr<ID3D12Resource> CreateAsBuffer(UINT64 bytes, bool result);

    // The second, tiny pipeline: the alpha-blended, depth-free pass that draws
    // HUD text from the MSDF atlas. Builds its root signature, PSO and the
    // per-frame dynamic vertex buffer the glyph quads are written into.
    void CreateTextPipeline();
    // Loads the font's glyph metrics and uploads its atlas into the heap's per-level
    // region. Shares the scene upload's open command list and staging list.
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
    // bottom, and the `debug_lines` overlay anchored up from the bottom-left corner. Both
    // are packed into this frame's text region back-to-back before either is drawn,
    // so they never alias in the shared buffer the way two DrawText calls would (see
    // DrawMenu). A no-op when all are empty. Assumes the render target is bound. The
    // `orders` list, when non-empty, is packed into the same region and drawn as a
    // bulleted list in the top-right corner (see DrawObjectivesRail); the `meats` rail
    // likewise down the top-left corner (see DrawMeatRail).
    void DrawHud(std::string_view prompt, std::span<const std::string> debug_lines,
                 std::span<const OrderCard> orders, std::span<const MeatCard> meats);
    // One packed run of the HUD buffer: a vertex range that is either a solid fill (a
    // panel, a bullet dot) or a line of glyphs from `face`, tinted `color`. DrawHud and
    // its rails pack a list of these into the shared region and then draw them in order,
    // so nothing aliases and panels sit behind their text.
    struct HudRun {
        UINT first;
        UINT count;
        DirectX::XMFLOAT4 color;
        bool solid;     // A flat fill (panel/bullet) rather than glyphs.
        FontFace face;  // Which atlas the glyphs draw from (ignored when solid).
    };
    // Packs the top-right orders list into the shared text region, appending its panel,
    // bullet and label runs to `runs` and advancing `cursor`. Split out of DrawHud for
    // legibility only; it must pack into the same cursor so its quads never alias the
    // prompt's or the debug overlay's in this frame's buffer.
    void DrawObjectivesRail(std::span<const OrderCard> orders, std::vector<HudRun>& runs,
                            UINT& cursor);
    // Packs the top-left meats rail into the shared text region, appending its
    // panel/gauge/label runs to `runs` and advancing `cursor`. The mirror of
    // DrawObjectivesRail: left-anchored cards, one per cooking meat, each a name, a
    // doneness gauge filled through its current band, and the band named beneath. Split
    // out of DrawHud for legibility; packs into the same cursor so its quads never alias.
    void DrawMeatRail(std::span<const MeatCard> meats, std::vector<HudRun>& runs, UINT& cursor);
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
    // Packs the loading screen -- the amber title and a quieter subtitle beneath it --
    // into this frame's text region and draws it, the way DrawMenu does for the list. See
    // RenderLoading for the arguments; called by it once the target is cleared and bound.
    void DrawLoading(std::string_view title, std::string_view subtitle);
    // Packs the keybinds screen -- title plus two-column rows -- into this frame's text
    // region and draws it, the way DrawMenu does for the plain list. See RenderKeybinds
    // for the arguments; called by it once the render target is cleared and bound.
    void DrawKeybinds(std::string_view title, std::span<const std::string> labels,
                      std::span<const std::string> values, int selected, bool capturing);
    // Packs the results screen -- title, the coloured order breakdown, then the action
    // list -- into this frame's text region and draws it, the way DrawMenu does. See
    // RenderResults for the arguments; called by it once the target is cleared and bound.
    void DrawResults(std::string_view title, bool passed, std::span<const ResultLine> lines,
                     std::span<const std::string> actions, int selected);

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
    // `descriptor` of the SRV heap. `format` is sRGB for base-colour images
    // and plain _UNORM for data textures (normal, metallic-roughness, atlas).
    ComPtr<ID3D12Resource> UploadTexture(const Image& image, UINT descriptor, DXGI_FORMAT format,
                                         std::vector<ComPtr<ID3D12Resource>>& staging);

    // One draw per primitive of each instance's model, each under its own root
    // constants. `shadow_receive` is 1 for the world, which is shadowed by the
    // sun, and 0 for the viewmodel, which is not.
    void DrawInstances(std::span<const MeshInstance> instances,
                       const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 sun_direction,
                       float shadow_receive);

    // Fills the currently bound render target with the gradient sky, seen from
    // `camera_position` through `view_projection`. Draws no depth, so geometry
    // rendered afterward paints over it. Switches to the sky pipeline and root
    // signature; the caller restores whatever it needs next. Its radiance is left
    // linear for the HDR buffer, which the tonemap pass encodes. `time` is vestigial
    // (it once drifted the flat cloud layer).
    void DrawSky(const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 camera_position,
                 float time);

    // The shadow pass: draws every caster depth-only into the shadow map from the
    // sun's point of view, wrapped in the barriers that flip the map between
    // depth target and shader resource.
    void RenderShadowMap(const Scene& scene, std::span<const MeshInstance> props, float time);
    // Grows the level's grass field into the currently bound HDR target: dispatches the
    // mesh shader over the field's grid of cells, each group generating a pack of
    // blades, writing colour and depth. A no-op with no grass loaded or no mesh-shader
    // support. Binds its own pipeline and root signature; the caller restores the
    // scene's for whatever follows.
    void RenderGrass(const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 camera_position,
                     float time);
    // Steps and ray-marches the NVIDIA Flow fire/smoke sim into the HDR scene buffer. Runs
    // in the world pass while the scene depth is still a depth target and holds the yard, so
    // the smoke is occluded by the grill and the world; the arms clear depth afterward and
    // draw over it. Flow records into our command list and binds its own descriptor heap and
    // pipelines, so the caller restores the scene's heap, root signature, PSO and render
    // targets after -- the same bracket the cloud pass uses. A no-op with no emitters and
    // nothing already burning in the grid. `view` and `projection` are the camera's,
    // separately (Flow reconstructs rays from both); `dt` steps the sim.
    void RenderFlow(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection, float dt,
                    std::span<const FlowEmitter> emitters);
    // Draws the lighter-fluid spray as screen-space sphere impostors into the HDR buffer.
    // Uploads this frame's `droplets` (world centre + radius) into the mapped droplet
    // region, then one instanced draw of a billboard quad per droplet. Runs in the world
    // pass while the scene depth still holds the yard, so the beads occlude and are
    // occluded correctly; it writes depth too, so they bead against one another. Binds its
    // own pipeline and root signature; the caller restores the scene's. A no-op with no
    // droplets. `view` and `projection` are the camera's, for the billboard and depth.
    void RenderFluidSpray(std::span<const DirectX::XMFLOAT4> droplets,
                          const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection);
    // Casts the grass into the sun's shadow map: runs the amplification and mesh shaders
    // from the light's point of view with no pixel shader, writing the field's depth so
    // it shadows the ground and the props. Called from RenderShadowMap while the map is a
    // depth target. `time` drives the same wind sway, so the shadows match the blades. A
    // no-op with no grass or no mesh-shader support.
    void RenderGrassShadow(float time);
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

    // The one persistent, shader-visible CBV/SRV/UAV heap the game binds -- the
    // session SRVs (HDR buffer, depth, shadow, bloom, fluid) in its front block, the
    // per-level material/atlas/shadow/raytracing descriptors after them. See
    // CreateEngineDescriptorHeap and the kMaterialHeapBase / kSrvHeapSize constants in
    // the .cpp. Shaders index it directly (ResourceDescriptorHeap); a level swap
    // overwrites the per-level region rather than rebuilding the heap.
    ComPtr<ID3D12DescriptorHeap> engine_heap_;
    UINT engine_heap_size_ = 0;

    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList7> command_list_;

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;

    // The gradient-sky background pass. No vertex buffer, no textures: the pixel
    // shader reconstructs the view ray from the inverse view-projection handed in
    // as root constants.
    ComPtr<ID3D12RootSignature> sky_root_signature_;
    ComPtr<ID3D12PipelineState> sky_pipeline_state_;

    // The resolve pass: tonemaps the HDR scene buffer and encodes it to the
    // swapchain. Reads engine_heap_ slot 0; no vertex buffer (a fullscreen triangle
    // from SV_VertexID).
    ComPtr<ID3D12RootSignature> tonemap_root_signature_;
    ComPtr<ID3D12PipelineState> tonemap_pipeline_state_;

    // The volumetric sun-shaft pass, additively blended into the HDR buffer. Reads
    // the scene depth and shadow map from engine_heap_ slots 1 and 2.
    ComPtr<ID3D12RootSignature> light_shaft_root_signature_;
    ComPtr<ID3D12PipelineState> light_shaft_pipeline_state_;
    ComPtr<ID3D12RootSignature> cloud_root_signature_;
    ComPtr<ID3D12PipelineState> cloud_pipeline_state_;

    // The lighter-fluid spray, a screen-space fluid pipeline in three passes. Pass 1
    // billboards the droplets and writes nearest surface depth + accumulated thickness to
    // fluid_depth_/fluid_thickness_; pass 2 bilaterally smooths the depth into
    // fluid_depth_blur_; pass 3 reconstructs the surface and composites it into the HDR
    // buffer with a dual-source blend. The offscreen targets are window-sized (recreated on
    // resize) and their SRVs live in the persistent engine heap.
    ComPtr<ID3D12RootSignature> fluid_surface_root_signature_;
    ComPtr<ID3D12PipelineState> fluid_surface_pipeline_;
    ComPtr<ID3D12RootSignature> fluid_blur_root_signature_;
    ComPtr<ID3D12PipelineState> fluid_blur_pipeline_;
    ComPtr<ID3D12RootSignature> fluid_composite_root_signature_;
    ComPtr<ID3D12PipelineState> fluid_composite_pipeline_;
    ComPtr<ID3D12Resource> fluid_depth_;
    ComPtr<ID3D12Resource> fluid_depth_blur_;
    ComPtr<ID3D12Resource> fluid_thickness_;
    // The per-frame droplet buffer (world centre + radius), one mapped upload-heap region
    // per frame in flight, bound as a root SRV. No descriptor heap slot -- like the grass
    // obstacles, it rides a root descriptor.
    ComPtr<ID3D12Resource> fluid_droplets_;
    std::byte* fluid_droplets_mapped_ = nullptr;
    UINT fluid_droplets_stride_ = 0;

    // The bloom pyramid: its mip textures, one root signature and the downsample /
    // upsample PSOs. Level 0's SRV is what the resolve reads to add the glow.
    BloomTarget bloom_targets_[kBloomLevels];
    ComPtr<ID3D12RootSignature> bloom_root_signature_;
    ComPtr<ID3D12PipelineState> bloom_downsample_pipeline_state_;
    ComPtr<ID3D12PipelineState> bloom_upsample_pipeline_state_;

    // The pick-up outline pass. Shares the scene's vertex buffers and input
    // layout; only the root signature and PSO differ.
    ComPtr<ID3D12RootSignature> outline_root_signature_;
    ComPtr<ID3D12PipelineState> outline_pipeline_state_;

    // The grass pass: a mesh-shader pipeline (no vertex buffer -- the blades are grown
    // on the GPU) and its root signature, both null on hardware without mesh shaders,
    // where the pass never runs. grass_ is the field the current level set; grass_active_
    // stays false until a level with a `grass` table loads.
    ComPtr<ID3D12RootSignature> grass_root_signature_;
    ComPtr<ID3D12PipelineState> grass_pipeline_state_;
    // The depth-only variant of the grass pipeline: the same amplification and mesh
    // shaders with no pixel shader, run from the sun's point of view to write the
    // field's depth into the shadow map, so the grass casts onto the ground and props.
    ComPtr<ID3D12PipelineState> grass_shadow_pipeline_state_;
    GrassPatch grass_{};
    bool grass_active_ = false;
    // The world footprints the grass keeps clear of -- the patio, benches and props --
    // packed as XZ rectangles into an upload-heap constant buffer the amplification and
    // mesh shaders read (b2). Filled by SetGrass; grass_obstacle_count_ is how many of
    // the fixed array are live. Bound as a root CBV, so it needs no descriptor heap slot.
    ComPtr<ID3D12Resource> grass_obstacles_;
    std::byte* grass_obstacles_mapped_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS grass_obstacles_address_ = 0;
    UINT grass_obstacle_count_ = 0;

    // The shadow pass. The map lives in dsv_heap_ slot 1 as a depth view and in
    // engine_heap_ at shadow_descriptor_ as an SRV; the light view-projection is
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
    // the shaders once baked in, so the first frames before any level loads are
    // drawn as they always were.
    Environment environment_ = kDefaultEnvironment;
    // The NVIDIA Flow fire/smoke sim, run by RenderFlow each frame. Session-persistent like
    // the device and pipelines: it comes up in Initialize on our device/queue/fence and is
    // reset (not rebuilt) across level swaps.
    FlowVolume flow_;
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

    // The HUD text pass. The atlas SRV lives in the shared engine_heap_ at
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
    // engine_heap_ at mono_atlas_descriptor_, right after font_'s.
    ComPtr<ID3D12Resource> mono_atlas_texture_;
    UINT mono_atlas_descriptor_ = 0;
    UINT mono_atlas_width_ = 0;
    UINT mono_atlas_height_ = 0;
    Font mono_font_;
    ComPtr<ID3D12Resource> text_vertex_buffer_;
    std::byte* text_vertex_mapped_ = nullptr;

    // The GPU resources behind the per-level material descriptors (see engine_heap_,
    // which the SRVs live in). Held so they outlive the frames that sample them and
    // are freed together on a level swap.
    std::vector<ComPtr<ID3D12Resource>> textures_;

    std::vector<GpuModel> models_;

    // The top-level acceleration structure over the loaded level's static instances,
    // and the shader-visible SRV slot the scene pixel shader reads it from to trace
    // reflection rays. Rebuilt per level by BuildAccelerationStructures; the per-model
    // BLASes it references live on the GpuModels.
    // The top-level AS the reflection rays trace, rebuilt in place every frame by
    // UpdateTopLevelAS from the static yard plus the moving props. tlas_scratch_ is its
    // build scratch, tlas_instances_ the per-frame-in-flight instance descs it is built
    // from (kept mapped, one region per frame). A single result buffer is safe because the
    // queue serializes frames. rt_max_instances_ caps how many instances one frame may hold.
    ComPtr<ID3D12Resource> tlas_;
    ComPtr<ID3D12Resource> tlas_scratch_;
    ComPtr<ID3D12Resource> tlas_instances_;
    std::byte* tlas_instances_mapped_ = nullptr;
    UINT tlas_instances_stride_ = 0;
    UINT tlas_descriptor_ = 0;
    UINT rt_max_instances_ = 0;
    // The two structured buffers the reflection hit shader reads to shade what a ray hits:
    // per-instance geometry+tint info (rebuilt every frame with the props, so it is a
    // mapped per-frame-in-flight upload buffer with one SRV slot each) and the static
    // per-primitive index-range+colour info. Freed on a level swap.
    ComPtr<ID3D12Resource> rt_instance_info_;
    std::byte* rt_instance_info_mapped_ = nullptr;
    UINT rt_instance_info_stride_ = 0;
    UINT rt_instance_info_descriptor_[kFrameCount]{};
    ComPtr<ID3D12Resource> rt_prim_info_;
    UINT rt_prim_info_descriptor_ = 0;

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
    bool mesh_shaders_supported_ = false;
    bool initialized_ = false;
};
