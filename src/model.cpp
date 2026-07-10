#include "model.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/dxmath_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <variant>

using namespace DirectX;

namespace {

// glTF is right-handed with +Y up and -Z forward. This game is left-handed with
// +Z north. A mirror through Z carries one into the other, and because a mirror
// is its own inverse, the same matrix undoes it.
//
// Three consequences, all paid for on the way in:
//
//   * A position or a direction has its Z negated.
//   * A transform M becomes S*M*S: a point is mirrored into glTF space,
//     transformed there, and mirrored back.
//   * Every triangle is reversed. A mirror negates the cross product
//     (v1 - v0) x (v2 - v0), while the outward normal it is supposed to equal
//     only has its Z negated -- so the two now disagree, and swapping two
//     corners of the triangle brings them back together. That invariant,
//     "cross(v1 - v0, v2 - v0) is the outward normal", is the one
//     MakeUnitCubeModel maintains by hand, and it is what makes back-face
//     culling work without anyone checking a winding order.
XMMATRIX MirrorZ() { return XMMatrixScaling(1.0f, 1.0f, -1.0f); }

XMFLOAT3 MirrorZ(XMFLOAT3 v) { return {v.x, v.y, -v.z}; }

// fastgltf holds a matrix as 16 column-major floats; XMFLOAT4X4 reads 16 floats
// as rows. That mismatch is exactly a transpose, and a transpose is exactly what
// turns glTF's column-vector matrix (p' = M*p) into DirectXMath's row-vector one
// (p' = p*M). So this copy is the whole conversion. Adding a transpose here
// would be the bug, not the fix.
XMFLOAT4X4 ToRowMajor(const fastgltf::math::fmat4x4& matrix) {
    XMFLOAT4X4 result;
    static_assert(sizeof(result) == sizeof(matrix));
    std::memcpy(&result, matrix.data(), sizeof(result));
    return result;
}

// A glTF-space transform, in the game's space and in DirectXMath's convention.
XMFLOAT4X4 ToGameSpace(const fastgltf::math::fmat4x4& gltf_transform) {
    const XMFLOAT4X4 row_major = ToRowMajor(gltf_transform);
    const XMMATRIX mirror = MirrorZ();

    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result, mirror * XMLoadFloat4x4(&row_major) * mirror);
    return result;
}

// A node whose name ends in this is drawn but never collided with.
//
// Colliders are derived from geometry, one axis-aligned box per node, and that
// is what a prop assembled out of parts wants -- until two of those parts are
// stacked. Then the lower one's top face is a surface the player can stand on,
// buried inside the prop where nobody can see it. The cooler's body ends where
// its lid begins, and without this a player descending past its corner lands on
// the body's lid-height rim and hovers there.
//
// So the cooler's body is the whole cooler, and its lid is a lip that sits over
// the top of it and collides with nothing. glTF has no standard way to say
// "decorative"; a name suffix is what engines settle on (Unreal's UCX_, Godot's
// -noimp), and it survives a round trip through Blender's outliner. The
// alternative, a custom `extras` field, would drag simdjson's headers in here to
// read one bool.
constexpr std::string_view kDecorativeSuffix = "_nocollide";

const fastgltf::Accessor& FindAccessor(const fastgltf::Asset& asset,
                                       const fastgltf::Primitive& primitive,
                                       std::string_view attribute) {
    const auto* found = primitive.findAttribute(attribute);
    if (found == primitive.attributes.cend()) {
        throw std::runtime_error("glTF primitive has no " + std::string(attribute));
    }
    return asset.accessors[found->accessorIndex];
}

bool HasAttribute(const fastgltf::Primitive& primitive, std::string_view attribute) {
    return primitive.findAttribute(attribute) != primitive.attributes.cend();
}

// The raw bytes behind an image, wherever glTF chose to put them: inline in a
// .glb's binary chunk, in a data: URI, or in a file beside a .gltf. The parser is
// asked to pull the last two into memory, so by the time we are here they are
// all just spans.
std::span<const std::byte> ImageBytes(const fastgltf::Asset& asset, const fastgltf::Image& image) {
    auto whole_buffer = [](const fastgltf::Buffer& buffer) -> std::span<const std::byte> {
        return std::visit(
            fastgltf::visitor{
                [](const fastgltf::sources::Array& array) -> std::span<const std::byte> {
                    return {array.bytes.data(), array.bytes.size()};
                },
                [](const fastgltf::sources::ByteView& bytes) -> std::span<const std::byte> {
                    return {bytes.bytes.data(), bytes.bytes.size()};
                },
                [](const auto&) -> std::span<const std::byte> {
                    throw std::runtime_error("glTF image lives in a buffer that was never loaded");
                }},
            buffer.data);
    };

    return std::visit(
        fastgltf::visitor{
            [&](const fastgltf::sources::BufferView& source) -> std::span<const std::byte> {
                const fastgltf::BufferView& view = asset.bufferViews[source.bufferViewIndex];
                return whole_buffer(asset.buffers[view.bufferIndex])
                    .subspan(view.byteOffset, view.byteLength);
            },
            [](const fastgltf::sources::Array& array) -> std::span<const std::byte> {
                return {array.bytes.data(), array.bytes.size()};
            },
            [](const fastgltf::sources::ByteView& bytes) -> std::span<const std::byte> {
                return {bytes.bytes.data(), bytes.bytes.size()};
            },
            [](const auto&) -> std::span<const std::byte> {
                throw std::runtime_error("glTF image has an unsupported data source");
            }},
        image.data);
}

// The bounds glTF already computed, mirrored into the game's space. The mirror
// makes the smaller Z the larger one, so the two trade places.
Aabb AccessorBounds(const fastgltf::Accessor& accessor) {
    if (!accessor.min || !accessor.max || accessor.min->size() < 3 || accessor.max->size() < 3) {
        throw std::runtime_error("glTF POSITION accessor carries no min/max bounds");
    }

    // The spec allows POSITION to be a normalised integer, which fastgltf would
    // hand back as int64 bounds. Nothing this game loads does that, and silently
    // reading the wrong union member is worse than saying so.
    auto component = [](const fastgltf::AccessorBoundsArray& bounds, std::size_t i) {
        if (!bounds.isType<double>()) {
            throw std::runtime_error("glTF POSITION accessor is not float");
        }
        return static_cast<float>(bounds.get<double>(i));
    };

    const float min_z = component(*accessor.min, 2);
    const float max_z = component(*accessor.max, 2);

    Aabb bounds{};
    bounds.min = {component(*accessor.min, 0), component(*accessor.min, 1), -max_z};
    bounds.max = {component(*accessor.max, 0), component(*accessor.max, 1), -min_z};
    return bounds;
}

void LoadImages(const fastgltf::Asset& asset, Model& model) {
    model.images.reserve(asset.images.size());
    for (const fastgltf::Image& image : asset.images) {
        model.images.push_back(DecodeImage(ImageBytes(asset, image)));
    }
}

void LoadMaterials(const fastgltf::Asset& asset, Model& model) {
    model.materials.reserve(asset.materials.size());
    for (const fastgltf::Material& source : asset.materials) {
        Material material{};
        material.base_color = {source.pbrData.baseColorFactor.x(),
                               source.pbrData.baseColorFactor.y(),
                               source.pbrData.baseColorFactor.z()};

        if (source.pbrData.baseColorTexture) {
            const fastgltf::Texture& texture =
                asset.textures[source.pbrData.baseColorTexture->textureIndex];
            if (!texture.imageIndex) {
                throw std::runtime_error("glTF base colour texture names no image");
            }
            material.base_color_image = static_cast<int>(*texture.imageIndex);
        }

        model.materials.push_back(material);
    }
}

// Flattens the node hierarchy. Every node the default scene can reach comes out
// with its transform composed all the way up to the root. A node no scene names
// keeps the identity it was built with, which is what an unused skin's joints
// would get -- harmless, because nothing draws them.
void LoadNodes(const fastgltf::Asset& asset, Model& model) {
    model.nodes.resize(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        model.nodes[i].name = std::string(asset.nodes[i].name);
        XMStoreFloat4x4(&model.nodes[i].to_model, XMMatrixIdentity());
    }

    const std::size_t scene_index = asset.defaultScene.value_or(0);
    if (scene_index >= asset.scenes.size()) {
        throw std::runtime_error("glTF names no scene to draw");
    }

    // The recursion is as deep as the node tree, which no sane asset makes deep.
    // An explicit stack would buy nothing but noise.
    auto descend = [&](std::size_t index, const fastgltf::math::fmat4x4& parent_to_model,
                       int parent, auto&& self) -> void {
        const fastgltf::Node& node = asset.nodes[index];
        const fastgltf::math::fmat4x4 to_model =
            fastgltf::getTransformMatrix(node, parent_to_model);

        model.nodes[index].to_model = ToGameSpace(to_model);
        model.nodes[index].parent = parent;

        for (const std::size_t child : node.children) {
            self(child, to_model, static_cast<int>(index), self);
        }
    };

    for (const std::size_t root : asset.scenes[scene_index].nodeIndices) {
        descend(root, fastgltf::math::fmat4x4(), -1, descend);
    }
}

void LoadSkins(const fastgltf::Asset& asset, Model& model) {
    model.skins.reserve(asset.skins.size());
    for (const fastgltf::Skin& source : asset.skins) {
        Skin skin{};
        skin.joints.assign(source.joints.begin(), source.joints.end());

        if (source.inverseBindMatrices) {
            const fastgltf::Accessor& accessor = asset.accessors[*source.inverseBindMatrices];
            if (accessor.count != skin.joints.size()) {
                throw std::runtime_error("glTF skin needs one inverse bind matrix per joint");
            }
            skin.inverse_bind.reserve(accessor.count);
            fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(
                asset, accessor, [&](const fastgltf::math::fmat4x4& matrix) {
                    // An inverse bind matrix maps model space into a joint's bind
                    // space. Both ends are mirrored, so it takes the same
                    // sandwich a node transform takes.
                    skin.inverse_bind.push_back(ToGameSpace(matrix));
                });
        } else {
            // The spec's default: every joint already sits at the origin in bind
            // pose, so the matrix that undoes the bind is the identity.
            skin.inverse_bind.resize(skin.joints.size());
            for (XMFLOAT4X4& matrix : skin.inverse_bind) {
                XMStoreFloat4x4(&matrix, XMMatrixIdentity());
            }
        }

        model.skins.push_back(std::move(skin));
    }
}

// Reads JOINTS_0 and WEIGHTS_0 for one primitive. Whatever component types the
// file used -- joints as bytes or shorts, weights as floats or as normalised
// integers -- fastgltf converts them on the way out.
void LoadSkinVertices(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
                      std::size_t first_vertex, Model& model) {
    model.skin_vertices.resize(model.vertices.size());

    fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(
        asset, FindAccessor(asset, primitive, "JOINTS_0"),
        [&](fastgltf::math::u16vec4 joints, std::size_t i) {
            for (std::size_t component = 0; component < 4; ++component) {
                model.skin_vertices[first_vertex + i].joints[component] = joints[component];
            }
        });

    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
        asset, FindAccessor(asset, primitive, "WEIGHTS_0"),
        [&](fastgltf::math::fvec4 weights, std::size_t i) {
            model.skin_vertices[first_vertex + i].weights = {weights.x(), weights.y(),
                                                             weights.z(), weights.w()};
        });
}

void LoadPrimitive(const fastgltf::Asset& asset, const fastgltf::Node& node,
                   const fastgltf::Primitive& source, const XMFLOAT4X4& to_model, Model& model) {
    if (source.type != fastgltf::PrimitiveType::Triangles) {
        throw std::runtime_error("glTF primitive is not a triangle list");
    }
    if (!source.indicesAccessor) {
        // Options::GenerateMeshIndices has already invented these for any
        // primitive that shipped without them, so this really is malformed.
        throw std::runtime_error("glTF primitive has no indices");
    }
    if (!HasAttribute(source, "NORMAL")) {
        // A primitive with no normals would be lit as though every face pointed
        // straight up. Better to say so than to draw it wrong.
        throw std::runtime_error("glTF primitive has no NORMAL attribute");
    }

    const fastgltf::Accessor& positions = FindAccessor(asset, source, "POSITION");

    Primitive primitive{};
    primitive.transform = to_model;
    primitive.bounds = AccessorBounds(positions);
    primitive.first_index = static_cast<std::uint32_t>(model.indices.size());
    primitive.material = source.materialIndex ? static_cast<int>(*source.materialIndex) : -1;
    primitive.skin = node.skinIndex ? static_cast<int>(*node.skinIndex) : -1;
    primitive.collides = !std::string_view(node.name).ends_with(kDecorativeSuffix);

    const std::size_t first_vertex = model.vertices.size();
    model.vertices.resize(first_vertex + positions.count);

    fastgltf::iterateAccessorWithIndex<XMFLOAT3>(
        asset, positions, [&](XMFLOAT3 position, std::size_t i) {
            model.vertices[first_vertex + i].position = MirrorZ(position);
        });

    fastgltf::iterateAccessorWithIndex<XMFLOAT3>(
        asset, FindAccessor(asset, source, "NORMAL"), [&](XMFLOAT3 normal, std::size_t i) {
            model.vertices[first_vertex + i].normal = MirrorZ(normal);
        });

    if (HasAttribute(source, "TEXCOORD_0")) {
        // glTF puts the UV origin at the top left, exactly where Direct3D puts
        // it, so nothing needs flipping.
        fastgltf::iterateAccessorWithIndex<XMFLOAT2>(
            asset, FindAccessor(asset, source, "TEXCOORD_0"), [&](XMFLOAT2 uv, std::size_t i) {
                model.vertices[first_vertex + i].uv = uv;
            });
    } else {
        // An untextured material samples a 1x1 white texture, where every
        // coordinate reads the same texel.
        for (std::size_t i = 0; i < positions.count; ++i) {
            model.vertices[first_vertex + i].uv = {0.0f, 0.0f};
        }
    }

    if (HasAttribute(source, "JOINTS_0") && HasAttribute(source, "WEIGHTS_0")) {
        LoadSkinVertices(asset, source, first_vertex, model);
    } else if (!model.skin_vertices.empty()) {
        // An earlier primitive was skinned. Keep the two arrays parallel.
        model.skin_vertices.resize(model.vertices.size());
    }

    const fastgltf::Accessor& indices = asset.accessors[*source.indicesAccessor];
    if (indices.count % 3 != 0) {
        throw std::runtime_error("glTF triangle list has an index count that is not a multiple of 3");
    }
    primitive.index_count = static_cast<std::uint32_t>(indices.count);
    model.indices.resize(primitive.first_index + indices.count);

    // Whatever width the file used -- byte, short or int -- arrives as a uint32.
    // Corner 0 of each triangle stays put and corners 1 and 2 swap, which is the
    // winding reversal the Z mirror owes. Every index is also lifted past the
    // vertices already in the model, so one buffer holds every primitive.
    fastgltf::iterateAccessorWithIndex<std::uint32_t>(
        asset, indices, [&](std::uint32_t index, std::size_t i) {
            const std::size_t corner = i % 3;
            const std::size_t reversed = corner == 0 ? i : i - corner + (3 - corner);
            model.indices[primitive.first_index + reversed] =
                static_cast<std::uint32_t>(first_vertex) + index;
        });

    model.primitives.push_back(primitive);
}

} // namespace

Model LoadGltfModel(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        throw std::runtime_error("Cannot read " + path.string() + ": " +
                                 std::string(fastgltf::getErrorMessage(data.error())));
    }

    // LoadExternalBuffers and LoadExternalImages pull a .gltf's side-car .bin and
    // .png files into memory. A .glb carries both in its binary chunk and needs
    // neither, but nothing here should care which of the two it was handed.
    fastgltf::Parser parser;
    auto asset = parser.loadGltf(data.get(), path.parent_path(),
                                 fastgltf::Options::LoadExternalBuffers |
                                     fastgltf::Options::LoadExternalImages |
                                     fastgltf::Options::GenerateMeshIndices);
    if (asset.error() != fastgltf::Error::None) {
        throw std::runtime_error("Cannot parse " + path.string() + ": " +
                                 std::string(fastgltf::getErrorMessage(asset.error())));
    }

    Model model{};
    LoadImages(asset.get(), model);
    LoadMaterials(asset.get(), model);
    LoadNodes(asset.get(), model);
    LoadSkins(asset.get(), model);

    // A mesh is drawn once per node that references it, under that node's
    // transform, so the four legs of the grill share one leg mesh.
    for (std::size_t i = 0; i < asset->nodes.size(); ++i) {
        const fastgltf::Node& node = asset->nodes[i];
        if (!node.meshIndex) {
            continue;
        }
        for (const fastgltf::Primitive& primitive : asset->meshes[*node.meshIndex].primitives) {
            LoadPrimitive(asset.get(), node, primitive, model.nodes[i].to_model, model);
        }
    }

    // A vertex that no skinned primitive claimed still has the zeroed weights it
    // was resized with, and a vertex weighted to nothing collapses onto the
    // origin. Binding it wholly to joint 0 at least leaves it where it is.
    for (SkinVertex& skin : model.skin_vertices) {
        if (skin.weights.x + skin.weights.y + skin.weights.z + skin.weights.w <= 0.0f) {
            skin.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        }
    }

    if (model.primitives.empty()) {
        throw std::runtime_error(path.string() + " holds no drawable geometry");
    }
    return model;
}

Model MakeUnitCubeModel() {
    constexpr XMFLOAT3 kFaceNormals[] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
                                         {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
                                         {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};

    // v grows downward across a texture and upward across the tangent frame,
    // which is why the corners walk V backwards.
    constexpr XMFLOAT2 kCornerUvs[] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};

    Model model{};

    for (const XMFLOAT3& face_normal : kFaceNormals) {
        const XMVECTOR normal = XMLoadFloat3(&face_normal);

        // Any vector that is not parallel to the normal seeds the face's tangent
        // frame. Build u and v so that cross(u, v) == normal.
        const XMVECTOR seed = std::abs(face_normal.y) > 0.5f ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                                                             : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(seed, normal));
        const XMVECTOR v = XMVector3Cross(normal, u);

        // Direct3D's default rasteriser treats a triangle as front facing when
        // cross(v1 - v0, v2 - v0) points at the camera. Emitting the corners in
        // this order makes that cross product the outward normal, so every face
        // of the cube is front facing from outside and back-face culling works.
        const XMVECTOR center = XMVectorScale(normal, 0.5f);
        const XMVECTOR half_u = XMVectorScale(u, 0.5f);
        const XMVECTOR half_v = XMVectorScale(v, 0.5f);
        const XMVECTOR corners[] = {
            XMVectorSubtract(XMVectorSubtract(center, half_u), half_v),
            XMVectorSubtract(XMVectorAdd(center, half_u), half_v),
            XMVectorAdd(XMVectorAdd(center, half_u), half_v),
            XMVectorAdd(XMVectorSubtract(center, half_u), half_v),
        };

        const auto base = static_cast<std::uint32_t>(model.vertices.size());
        for (int corner = 0; corner < 4; ++corner) {
            Vertex vertex{};
            XMStoreFloat3(&vertex.position, corners[corner]);
            vertex.normal = face_normal;
            vertex.uv = kCornerUvs[corner];
            model.vertices.push_back(vertex);
        }

        for (const std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
            model.indices.push_back(base + offset);
        }
    }

    Primitive primitive{};
    XMStoreFloat4x4(&primitive.transform, XMMatrixIdentity());
    primitive.index_count = static_cast<std::uint32_t>(model.indices.size());
    primitive.bounds.min = {-0.5f, -0.5f, -0.5f};
    primitive.bounds.max = {0.5f, 0.5f, 0.5f};
    model.primitives.push_back(primitive);

    return model;
}
