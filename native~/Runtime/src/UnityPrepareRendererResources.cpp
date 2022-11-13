#include "UnityPrepareRendererResources.h"

#include "TextureLoader.h"
#include "UnityLifetime.h"
#include "UnityTransforms.h"

#include <Cesium3DTilesSelection/GltfUtilities.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/Transforms.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionMeshPrimitiveExtFeatureMetadata.h>
#include <CesiumGltf/ExtensionModelExtFeatureMetadata.h>
#include <CesiumUtility/ScopeGuard.h>

#include <DotNet/CesiumForUnity/Cesium3DTileset.h>
#include <DotNet/CesiumForUnity/CesiumGeoreference.h>
#include <DotNet/CesiumForUnity/CesiumGlobeAnchor.h>
#include <DotNet/CesiumForUnity/CesiumMetadata.h>
#include <DotNet/System/Array1.h>
#include <DotNet/System/Object.h>
#include <DotNet/System/String.h>
#include <DotNet/System/Text/Encoding.h>
#include <DotNet/Unity/Collections/Allocator.h>
#include <DotNet/Unity/Collections/LowLevel/Unsafe/NativeArrayUnsafeUtility.h>
#include <DotNet/Unity/Collections/NativeArray1.h>
#include <DotNet/Unity/Collections/NativeArrayOptions.h>
#include <DotNet/UnityEngine/Application.h>
#include <DotNet/UnityEngine/Debug.h>
#include <DotNet/UnityEngine/FilterMode.h>
#include <DotNet/UnityEngine/HideFlags.h>
#include <DotNet/UnityEngine/Material.h>
#include <DotNet/UnityEngine/Matrix4x4.h>
#include <DotNet/UnityEngine/Mesh.h>
#include <DotNet/UnityEngine/MeshCollider.h>
#include <DotNet/UnityEngine/MeshData.h>
#include <DotNet/UnityEngine/MeshDataArray.h>
#include <DotNet/UnityEngine/MeshFilter.h>
#include <DotNet/UnityEngine/MeshRenderer.h>
#include <DotNet/UnityEngine/MeshTopology.h>
#include <DotNet/UnityEngine/Object.h>
#include <DotNet/UnityEngine/Physics.h>
#include <DotNet/UnityEngine/Quaternion.h>
#include <DotNet/UnityEngine/Rendering/IndexFormat.h>
#include <DotNet/UnityEngine/Rendering/MeshUpdateFlags.h>
#include <DotNet/UnityEngine/Rendering/SubMeshDescriptor.h>
#include <DotNet/UnityEngine/Rendering/VertexAttributeDescriptor.h>
#include <DotNet/UnityEngine/Resources.h>
#include <DotNet/UnityEngine/Texture.h>
#include <DotNet/UnityEngine/TextureWrapMode.h>
#include <DotNet/UnityEngine/Transform.h>
#include <DotNet/UnityEngine/Vector2.h>
#include <DotNet/UnityEngine/Vector3.h>
#include <DotNet/UnityEngine/Vector4.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <algorithm>

using namespace Cesium3DTilesSelection;
using namespace CesiumForUnityNative;
using namespace CesiumGeometry;
using namespace CesiumGeospatial;
using namespace CesiumGltf;
using namespace CesiumUtility;
using namespace DotNet;

namespace {

template <typename TDest, typename TSource>
void setTriangles(
    const Unity::Collections::NativeArray1<TDest>& dest,
    const AccessorView<TSource>& source) {
  assert(dest.Length() == source.size());

  TDest* triangles = static_cast<TDest*>(
      Unity::Collections::LowLevel::Unsafe::NativeArrayUnsafeUtility::
          GetUnsafeBufferPointerWithoutChecks(dest));

  for (int64_t i = 0; i < source.size(); ++i) {
    triangles[i] = source[i];
  }
}

int32_t countPrimitives(const CesiumGltf::Model& model) {
  int32_t numberOfPrimitives = 0;
  model.forEachPrimitiveInScene(
      -1,
      [&numberOfPrimitives](
          const Model& gltf,
          const Node& node,
          const Mesh& mesh,
          const MeshPrimitive& primitive,
          const glm::dmat4& transform) { ++numberOfPrimitives; });
  return numberOfPrimitives;
}

/**
 * @brief The result after populating Unity mesh data with loaded glTF content. 
 */
struct MeshDataResult {
  UnityEngine::MeshDataArray meshDataArray;
  std::vector<CesiumPrimitiveInfo> primitiveInfos;
};

uint32_t packColorChannel(float c) {
  return c >= 1.0f ? 255 : static_cast<uint32_t>(std::floor(256.0f * c));
}

uint32_t packColor(float r, float g, float b, float a) {
  return 
      (packColorChannel(r) << 24) | 
      (packColorChannel(g) << 16) | 
      (packColorChannel(b) << 8) | 
      packColorChannel(a);
}

void populateMeshDataArray(
    MeshDataResult& meshDataResult,
    const TileLoadResult& tileLoadResult) {
  const CesiumGltf::Model* pModel =
      std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);
  if (!pModel)
    return;

  size_t meshDataInstance = 0;

  meshDataResult.primitiveInfos.reserve(countPrimitives(*pModel));

  pModel->forEachPrimitiveInScene(
      -1,
      [&meshDataResult, &meshDataInstance](
          const Model& gltf,
          const Node& node,
          const Mesh& mesh,
          const MeshPrimitive& primitive,
          const glm::dmat4& transform) {
        UnityEngine::MeshData meshData = meshDataResult.meshDataArray[meshDataInstance++];
        CesiumPrimitiveInfo& primitiveInfo = 
            meshDataResult.primitiveInfos.emplace_back();

        using namespace DotNet::UnityEngine;
        using namespace DotNet::UnityEngine::Rendering;
        using namespace DotNet::Unity::Collections;
        using namespace DotNet::Unity::Collections::LowLevel::Unsafe;

        // TODO: might have to change this limit
        const int MAX_ATTRIBUTES = 8;
        VertexAttributeDescriptor descriptor[MAX_ATTRIBUTES];

        // Interleave all attributes into single stream.
        std::int32_t numberOfAttributes = 0;
        std::int32_t streamIndex = 0;

        if (primitive.indices < 0) {
          // TODO: support non-indexed primitives.
          return;
        }

        auto positionAccessorIt = primitive.attributes.find("POSITION");
        if (positionAccessorIt == primitive.attributes.end()) {
          // This primitive doesn't have a POSITION semantic, ignore it.
          return;
        }

        int32_t positionAccessorID = positionAccessorIt->second;
        AccessorView<UnityEngine::Vector3> positionView(
            gltf,
            positionAccessorID);
        if (positionView.status() != AccessorViewStatus::Valid) {
          // TODO: report invalid accessor
          return;
        }

        assert(numberOfAttributes < MAX_ATTRIBUTES);
        descriptor[numberOfAttributes].attribute = VertexAttribute::Position;
        descriptor[numberOfAttributes].format = VertexAttributeFormat::Float32;
        descriptor[numberOfAttributes].dimension = 3;
        descriptor[numberOfAttributes].stream = streamIndex;
        ++numberOfAttributes;


        // Add the NORMAL attribute, if it exists.
        auto normalAccessorIt = primitive.attributes.find("NORMAL");
        AccessorView<UnityEngine::Vector3> normalView =
            normalAccessorIt != primitive.attributes.end()
                ? AccessorView<UnityEngine::Vector3>(
                      gltf,
                      normalAccessorIt->second)
                : AccessorView<UnityEngine::Vector3>();

        if (normalView.status() == AccessorViewStatus::Valid &&
            normalView.size() >= positionView.size()) {
          assert(numberOfAttributes < MAX_ATTRIBUTES);
          descriptor[numberOfAttributes].attribute = VertexAttribute::Normal;
          descriptor[numberOfAttributes].format =
              VertexAttributeFormat::Float32;
          descriptor[numberOfAttributes].dimension = 3;
          descriptor[numberOfAttributes].stream = streamIndex;
          ++numberOfAttributes;
        }

        
        // Add the COLOR_0 attribute, if it exists.
        // It may originally be a vec3 or vec4 attribute, but we
        // will pack it into a Color32 in both cases.
        auto colorAccessorIt = primitive.attributes.find("COLOR_0");
        AccessorView<UnityEngine::Vector3> colorViewVec3 =
            colorAccessorIt != primitive.attributes.end()
                ? AccessorView<UnityEngine::Vector3>(
                      gltf,
                      normalAccessorIt->second)
                : AccessorView<UnityEngine::Vector3>();
        AccessorView<UnityEngine::Vector4> colorViewVec4 =
            colorAccessorIt != primitive.attributes.end()
                ? AccessorView<UnityEngine::Vector4>(
                      gltf,
                      normalAccessorIt->second)
                : AccessorView<UnityEngine::Vector4>();

        if ((colorViewVec3.status() == AccessorViewStatus::Valid &&
             colorViewVec3.size() >= positionView.size()) || 
            (colorViewVec4.status() == AccessorViewStatus::Valid &&
             colorViewVec4.size() >= positionView.size())) {
          assert(numberOfAttributes < MAX_ATTRIBUTES);
          descriptor[numberOfAttributes].attribute = VertexAttribute::Color;
          descriptor[numberOfAttributes].format = VertexAttributeFormat::UInt32;
          // TODO: check if this dimension count is correct -
          // my current understanding is we pack the color into
          // a single int32, so 1 dimension...?
          // Maybe 4 dimensions of SInt8?
          descriptor[numberOfAttributes].dimension = 1;
          descriptor[numberOfAttributes].stream = streamIndex;
          ++numberOfAttributes;
        }

        constexpr int MAX_TEX_COORDS = 8;
        int numTexCoords = 0;
        AccessorView<UnityEngine::Vector2> texCoordViews[MAX_TEX_COORDS];

        // Add all texture coordinate sets TEXCOORD_i
        for (int i = 0; i < 8 && numTexCoords < MAX_TEX_COORDS; ++i) {
          // TODO: Only add texture coordinates that are needed.
          // E.g., might not need UV coords for metadata.

          // Build accessor view for glTF attribute.
          auto texCoordAccessorIt = primitive.attributes.find("TEXCOORD_" + std::to_string(i));
          if (texCoordAccessorIt == primitive.attributes.end()) {
            continue;
          }

          AccessorView<UnityEngine::Vector2> texCoordView(
              gltf,
              texCoordAccessorIt->second);
          if (texCoordView.status() != AccessorViewStatus::Valid &&
              texCoordView.size() >= positionView.size()) {
            // TODO: report invalid accessor?
            continue;
          }

          texCoordViews[numTexCoords] = texCoordView;
          primitiveInfo.uvIndexMap[i] = numTexCoords;
          
          // Build Unity descriptor for this attribute.
          assert(numberOfAttributes < MAX_ATTRIBUTES);

          descriptor[numberOfAttributes].attribute = (VertexAttribute)((int)VertexAttribute::TexCoord0 + numTexCoords);
          descriptor[numberOfAttributes].format = VertexAttributeFormat::Float32;
          descriptor[numberOfAttributes].dimension = 2;
          descriptor[numberOfAttributes].stream = streamIndex;

          ++numTexCoords;
          ++numberOfAttributes;
        }


        // Add all texture coordinate sets _CESIUMOVERLAY_i
        for (int i = 0; i < 8 && numTexCoords < MAX_TEX_COORDS; ++i) {
          // Build accessor view for glTF attribute.
          auto overlayAccessorIt = primitive.attributes.find("_CESIUMOVERLAY_" + std::to_string(i));
          if (overlayAccessorIt == primitive.attributes.end()) {
            continue;
          }

          AccessorView<UnityEngine::Vector2> overlayTexCoordView(
              gltf,
              overlayAccessorIt->second);
          if (overlayTexCoordView.status() != AccessorViewStatus::Valid &&
              overlayTexCoordView.size() >= positionView.size()) {
            // TODO: report invalid accessor?
            continue;
          }
          
          texCoordViews[numTexCoords] = overlayTexCoordView;
          primitiveInfo.rasterOverlayUvIndexMap[i] = numTexCoords;

          // Build Unity descriptor for this attribute.
          assert(numberOfAttributes < MAX_ATTRIBUTES);

          descriptor[numberOfAttributes].attribute = (VertexAttribute)((int)VertexAttribute::TexCoord0 + numTexCoords);
          descriptor[numberOfAttributes].format = VertexAttributeFormat::Float32;
          descriptor[numberOfAttributes].dimension = 2;
          descriptor[numberOfAttributes].stream = streamIndex;

          ++numTexCoords;
          ++numberOfAttributes;
        }


        System::Array1<VertexAttributeDescriptor> attributes(
            numberOfAttributes);
        for (int32_t i = 0; i < numberOfAttributes; ++i) {
          attributes.Item(i, descriptor[i]);
        }

        meshData.SetVertexBufferParams(positionView.size(), attributes);


        // TODO: double check this is safe!!
        NativeArray1<uint8_t> nativeVertexBuffer = 
            meshData.GetVertexData<uint8_t>(streamIndex);
        uint8_t* pWritePos = static_cast<uint8_t*>(
            NativeArrayUnsafeUtility::GetUnsafeBufferPointerWithoutChecks(
              nativeVertexBuffer));
        
        // Since the vertex buffer is dynamically interleaved, we don't have a 
        // convenient struct to represent the vertex data.
        // The vertex layout will be as follows:
        // 1. position
        // 2. normals (skip if N/A)
        // 3. vertex colors (skip if N/A) 
        // 4. texcoords (first all TEXCOORD_i, then all _CESIUMOVERLAY_i)
        for (int64_t i = 0; i < positionView.size(); ++i) {
          *reinterpret_cast<Vector3*>(pWritePos) = positionView[i];
          pWritePos += sizeof(Vector3);

          if (normalView.status() == AccessorViewStatus::Valid) {
            *reinterpret_cast<Vector3*>(pWritePos) = normalView[i];
            pWritePos += sizeof(Vector3);
          }

          if (colorViewVec3.status() == AccessorViewStatus::Valid) {
            const Vector3& color = colorViewVec3[i];
            *reinterpret_cast<uint32_t*>(pWritePos) = packColor(color.x, color.y, color.z, 1.0f);
            pWritePos += sizeof(uint32_t);
          } else if (colorViewVec4.status() == AccessorViewStatus::Valid) {
            const Vector4& color = colorViewVec4[i];
            *reinterpret_cast<uint32_t*>(pWritePos) = packColor(color.x, color.y, color.z, color.w);
            pWritePos += sizeof(uint32_t);
          }

          for (uint32_t texCoordIndex = 0; texCoordIndex < numTexCoords; ++texCoordIndex) {
            *reinterpret_cast<Vector2*>(pWritePos) = texCoordViews[texCoordIndex][i];
            pWritePos += sizeof(Vector2);            
          }
        }

        // TODO: previously when there were more normals / texcoords then positions,
        // we just filled the vertex data with 0s. Now we don't add them at all, and
        // instead consider those attributes as "invalid". Which is the actual desired
        // behavior?

        int32_t indexCount = 0;

        AccessorView<uint8_t> indices8(gltf, primitive.indices);
        if (indices8.status() == AccessorViewStatus::Valid) {
          indexCount = indices8.size();
          meshData.SetIndexBufferParams(indexCount, IndexFormat::UInt16);
          setTriangles(meshData.GetIndexData<std::uint16_t>(), indices8);
        }

        AccessorView<uint16_t> indices16(gltf, primitive.indices);
        if (indices16.status() == AccessorViewStatus::Valid) {
          indexCount = indices16.size();
          meshData.SetIndexBufferParams(indexCount, IndexFormat::UInt16);
          setTriangles(meshData.GetIndexData<std::uint16_t>(), indices16);
        }

        AccessorView<uint32_t> indices32(gltf, primitive.indices);
        if (indices32.status() == AccessorViewStatus::Valid) {
          indexCount = indices32.size();
          meshData.SetIndexBufferParams(indexCount, IndexFormat::UInt32);
          setTriangles(meshData.GetIndexData<std::uint32_t>(), indices32);
        }

        meshData.subMeshCount(1);

        // TODO: use sub-meshes for glTF primitives, instead of a separate mesh
        // for each.
        SubMeshDescriptor subMeshDescriptor{};
        subMeshDescriptor.topology = MeshTopology::Triangles;
        subMeshDescriptor.indexStart = 0;
        subMeshDescriptor.indexCount = indexCount;
        subMeshDescriptor.baseVertex = 0;

        // These are calculated automatically by SetSubMesh
        subMeshDescriptor.firstVertex = 0;
        subMeshDescriptor.vertexCount = 0;

        meshData.SetSubMesh(0, subMeshDescriptor, MeshUpdateFlags::Default);

        // if (createPhysicsMeshes) {
        //   UnityEngine::MeshCollider meshCollider =
        //       primitiveGameObject.AddComponent<UnityEngine::MeshCollider>();
        //   meshCollider.sharedMesh(unityMesh);
        // }
      });
}

/**
 * @brief The result of the async part of mesh loading. 
 */
struct LoadThreadResult {
  System::Array1<UnityEngine::Mesh> meshes;
  std::vector<CesiumPrimitiveInfo> primitiveInfos{};
};
} // namespace

UnityPrepareRendererResources::UnityPrepareRendererResources(
    const UnityEngine::GameObject& tileset)
    : _tileset(tileset) {}

CesiumAsync::Future<TileLoadResultAndRenderResources>
UnityPrepareRendererResources::prepareInLoadThread(
    const CesiumAsync::AsyncSystem& asyncSystem,
    TileLoadResult&& tileLoadResult,
    const glm::dmat4& transform,
    const std::any& rendererOptions) {
  CesiumGltf::Model* pModel =
      std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);
  if (!pModel)
    return asyncSystem.createResolvedFuture(
        TileLoadResultAndRenderResources{std::move(tileLoadResult), nullptr});

  int32_t numberOfPrimitives = countPrimitives(*pModel);

  struct IntermediateLoadThreadResult {
    MeshDataResult meshDataResult;
    TileLoadResult tileLoadResult;
  };

  return asyncSystem
      .runInMainThread([numberOfPrimitives]() {
        // Allocate a MeshDataArray for the primitives.
        // Unfortunately, this must be done on the main thread.
        return UnityEngine::Mesh::AllocateWritableMeshData(numberOfPrimitives);
      })
      .thenInWorkerThread(
          [tileLoadResult = std::move(tileLoadResult)](
              UnityEngine::MeshDataArray&& meshDataArray) mutable {
            // Free the MeshDataArray if something goes wrong.
            ScopeGuard sg([&meshDataArray]() { meshDataArray.Dispose(); });

            MeshDataResult meshDataResult {
              std::move(meshDataArray),
              {}};
            populateMeshDataArray(meshDataResult, tileLoadResult);

            // We're returning the MeshDataArray, so don't free it.
            sg.release();
            return IntermediateLoadThreadResult{
                std::move(meshDataResult),
                std::move(tileLoadResult)};
          })
      .thenInMainThread(
          [asyncSystem, tileset = this->_tileset](
              IntermediateLoadThreadResult&& workerResult) mutable {
            bool shouldCreatePhysicsMeshes = false;
            bool shouldShowTilesInHierarchy = false;

            DotNet::CesiumForUnity::Cesium3DTileset tilesetComponent =
                tileset.GetComponent<DotNet::CesiumForUnity::Cesium3DTileset>();
            if (tilesetComponent != nullptr) {
              shouldCreatePhysicsMeshes =
                  tilesetComponent.createPhysicsMeshes();
              shouldShowTilesInHierarchy = 
                  tilesetComponent.showTilesInHierarchy();
            }

            const UnityEngine::MeshDataArray& meshDataArray = 
                workerResult.meshDataResult.meshDataArray;

            // Create meshes and populate them from the MeshData created in
            // the worker thread. Sadly, this must be done in the main
            // thread, too.
            System::Array1<UnityEngine::Mesh> meshes(meshDataArray.Length());
            for (int32_t i = 0, len = meshes.Length(); i < len; ++i) {
              UnityEngine::Mesh unityMesh{};

              // Don't let Unity unload this mesh during the time in between
              // when we create it and when we attach it to a GameObject.
              if (shouldShowTilesInHierarchy) {
                unityMesh.hideFlags(UnityEngine::HideFlags::HideAndDontSave);
              } else {
                unityMesh.hideFlags(UnityEngine::HideFlags::HideAndDontSave | UnityEngine::HideFlags::HideInHierarchy);
              }

              meshes.Item(i, unityMesh);
            }

            // TODO: Validate indices in the worker thread, and then ask Unity
            // not to do it here by setting
            // MeshUpdateFlags::DontValidateIndices.
            UnityEngine::Mesh::ApplyAndDisposeWritableMeshData(
                meshDataArray,
                meshes,
                UnityEngine::Rendering::MeshUpdateFlags::Default);

            // TODO: we should be able to do this in the worker thread, even if
            // we have to do it manually.
            for (int32_t i = 0, len = meshes.Length(); i < len; ++i) {
              meshes[i].RecalculateBounds();
            }

            if (shouldCreatePhysicsMeshes) {
              // Baking physics meshes takes awhile, so do that in a
              // worker thread.
              std::int32_t len = meshes.Length();
              std::vector<std::int32_t> instanceIDs(meshes.Length());
              for (int32_t i = 0; i < len; ++i) {
                instanceIDs[i] = meshes[i].GetInstanceID();
              }

              return asyncSystem.runInWorkerThread(
                  [workerResult = std::move(workerResult),
                   instanceIDs = std::move(instanceIDs),
                   meshes = std::move(meshes)]() mutable {
                    for (std::int32_t instanceID : instanceIDs) {
                      UnityEngine::Physics::BakeMesh(instanceID, false);
                    }

                    LoadThreadResult* pResult = new LoadThreadResult{
                        std::move(meshes),
                        std::move(workerResult.meshDataResult.primitiveInfos)};
                    return TileLoadResultAndRenderResources{
                        std::move(workerResult.tileLoadResult),
                        pResult};
                  });
            } else {
              LoadThreadResult* pResult = new LoadThreadResult{
                  std::move(meshes),
                  std::move(workerResult.meshDataResult.primitiveInfos)};
              return asyncSystem.createResolvedFuture(
                  TileLoadResultAndRenderResources{
                      std::move(workerResult.tileLoadResult),
                      pResult});
            }
          });
}


void* UnityPrepareRendererResources::prepareInMainThread(
    Cesium3DTilesSelection::Tile& tile,
    void* pLoadThreadResult_) {
  std::unique_ptr<LoadThreadResult> pLoadThreadResult(static_cast<LoadThreadResult*>(pLoadThreadResult_));

  const System::Array1<UnityEngine::Mesh>& meshes = pLoadThreadResult->meshes;
  const std::vector<CesiumPrimitiveInfo>& primitiveInfos = pLoadThreadResult->primitiveInfos;

  const Cesium3DTilesSelection::TileContent& content = tile.getContent();
  const Cesium3DTilesSelection::TileRenderContent* pRenderContent =
      content.getRenderContent();
  if (!pRenderContent) {
    return nullptr;
  }

  const Model& model = pRenderContent->getModel();

  std::string name = "glTF";
  auto urlIt = model.extras.find("Cesium3DTiles_TileUrl");
  if (urlIt != model.extras.end()) {
    name = urlIt->second.getStringOrDefault("glTF");
  }

  DotNet::CesiumForUnity::Cesium3DTileset tilesetComponent =
      this->_tileset.GetComponent<DotNet::CesiumForUnity::Cesium3DTileset>();

  auto pModelGameObject =
      std::make_unique<UnityEngine::GameObject>(System::String(name));
  
  if (tilesetComponent.showTilesInHierarchy()) {
    pModelGameObject->hideFlags(UnityEngine::HideFlags::DontSave);
  } else {
    pModelGameObject->hideFlags(UnityEngine::HideFlags::DontSave | UnityEngine::HideFlags::HideInHierarchy);
  }
  
  pModelGameObject->transform().parent(this->_tileset.transform());
  pModelGameObject->SetActive(false);

  glm::dmat4 tileTransform = tile.getTransform();
  tileTransform = GltfUtilities::applyRtcCenter(model, tileTransform);
  tileTransform = GltfUtilities::applyGltfUpAxisTransform(model, tileTransform);

  DotNet::CesiumForUnity::CesiumGeoreference georeferenceComponent =
      this->_tileset
          .GetComponentInParent<DotNet::CesiumForUnity::CesiumGeoreference>();

  const LocalHorizontalCoordinateSystem* pCoordinateSystem = nullptr;
  if (georeferenceComponent != nullptr) {
    pCoordinateSystem =
        &georeferenceComponent.NativeImplementation().getCoordinateSystem();
  }

  UnityEngine::Material opaqueMaterial = tilesetComponent.opaqueMaterial();
  if (opaqueMaterial == nullptr) {
    opaqueMaterial = UnityEngine::Resources::Load<UnityEngine::Material>(
        System::String("CesiumDefaultTilesetMaterial"));
  }

  const bool createPhysicsMeshes = tilesetComponent.createPhysicsMeshes();
  const bool showTilesInHierarchy = tilesetComponent.showTilesInHierarchy();

  size_t meshIndex = 0;

  DotNet::CesiumForUnity::CesiumMetadata pMetadataComponent = nullptr;
  if (model.getExtension<ExtensionModelExtFeatureMetadata>()) {
    pMetadataComponent =
        pModelGameObject
            ->GetComponentInParent<DotNet::CesiumForUnity::CesiumMetadata>();
    if (pMetadataComponent == nullptr) {
      pMetadataComponent =
          pModelGameObject->transform()
              .parent()
              .gameObject()
              .AddComponent<DotNet::CesiumForUnity::CesiumMetadata>();
    }
  }

  model.forEachPrimitiveInScene(
      -1,
      [&meshes,
       &primitiveInfos,
       &pModelGameObject,
       &tileTransform,
       &meshIndex,
       opaqueMaterial,
       pCoordinateSystem,
       createPhysicsMeshes,
       showTilesInHierarchy,
       &pMetadataComponent](
          const Model& gltf,
          const Node& node,
          const Mesh& mesh,
          const MeshPrimitive& primitive,
          const glm::dmat4& transform) {
        const CesiumPrimitiveInfo& primitiveInfo = primitiveInfos[meshIndex];
        UnityEngine::Mesh unityMesh = meshes[meshIndex++];
        if (unityMesh == nullptr) {
          // This indicates Unity destroyed the mesh already, which really
          // shouldn't happen.
          return;
        }

        if (primitive.indices < 0) {
          // TODO: support non-indexed primitives.
          return;
        }

        auto positionAccessorIt = primitive.attributes.find("POSITION");
        if (positionAccessorIt == primitive.attributes.end()) {
          // This primitive doesn't have a POSITION semantic, ignore it.
          return;
        }

        int32_t positionAccessorID = positionAccessorIt->second;
        AccessorView<UnityEngine::Vector3> positionView(
            gltf,
            positionAccessorID);
        if (positionView.status() != AccessorViewStatus::Valid) {
          // TODO: report invalid accessor
          return;
        }

        int64_t primitiveIndex = &mesh.primitives[0] - &primitive;
        UnityEngine::GameObject primitiveGameObject(
            System::String("Primitive " + std::to_string(primitiveIndex)));
        if (showTilesInHierarchy) {
          primitiveGameObject.hideFlags(UnityEngine::HideFlags::DontSave);
        } else {
          primitiveGameObject.hideFlags(UnityEngine::HideFlags::DontSave | UnityEngine::HideFlags::HideInHierarchy);
        }

        primitiveGameObject.transform().parent(pModelGameObject->transform());

        glm::dmat4 fixedToUnity =
            pCoordinateSystem
                ? pCoordinateSystem->getEcefToLocalTransformation()
                : glm::dmat4(1.0);

        glm::dmat4 transformToEcef = tileTransform * transform;
        glm::dvec3 ecefPosition = glm::dvec3(transformToEcef[3]);

        glm::dmat4 transformToUnity = fixedToUnity * transformToEcef;

        glm::dvec3 translation = glm::dvec3(transformToUnity[3]);

        RotationAndScale rotationAndScale =
            UnityTransforms::matrixToRotationAndScale(
                glm::dmat3(transformToUnity));

        primitiveGameObject.transform().position(UnityEngine::Vector3{
            float(translation.x),
            float(translation.y),
            float(translation.z)});
        primitiveGameObject.transform().rotation(
            UnityTransforms::toUnity(rotationAndScale.rotation));
        primitiveGameObject.transform().localScale(
            UnityTransforms::toUnity(rotationAndScale.scale));

        CesiumForUnity::CesiumGlobeAnchor anchor =
            primitiveGameObject
                .AddComponent<CesiumForUnity::CesiumGlobeAnchor>();
        anchor.detectTransformChanges(false);
        anchor.SetPositionEarthCenteredEarthFixed(
            ecefPosition.x,
            ecefPosition.y,
            ecefPosition.z);

        UnityEngine::MeshFilter meshFilter =
            primitiveGameObject.AddComponent<UnityEngine::MeshFilter>();
        UnityEngine::MeshRenderer meshRenderer =
            primitiveGameObject.AddComponent<UnityEngine::MeshRenderer>();

        UnityEngine::Material material =
            UnityEngine::Object::Instantiate(opaqueMaterial);
        meshRenderer.material(material);

        const Material* pMaterial =
            Model::getSafe(&gltf.materials, primitive.material);
        if (pMaterial) {
          if (pMaterial->pbrMetallicRoughness) {
            const std::optional<TextureInfo>& baseColorTexture =
                pMaterial->pbrMetallicRoughness->baseColorTexture;
            if (baseColorTexture) {
              auto texCoordIndexIt = primitiveInfo.uvIndexMap.find(baseColorTexture->texCoord);
              if (texCoordIndexIt != primitiveInfo.uvIndexMap.end()) {
                UnityEngine::Texture texture =
                    TextureLoader::loadTexture(gltf, baseColorTexture->index);
                if (texture != nullptr) {
                  material.SetTexture(System::String("_baseColorTexture"), texture);
                  material.SetFloat(System::String("_baseColorTextureCoordinateIndex"), static_cast<float>(texCoordIndexIt->second));

                  const std::vector<double>& baseColorFactorSrc = pMaterial->pbrMetallicRoughness->baseColorFactor;

                  // TODO: double check that the gltf base color factor is in RGBA order
                  // TODO: do these scale factors need to consider sRGB?
                  // If so, we might want to use material.SetColor
                  UnityEngine::Vector4 baseColorFactor;
                  baseColorFactor.x = baseColorFactorSrc.size() > 0 ? static_cast<float>(baseColorFactorSrc[0]) : 1.0f;
                  baseColorFactor.y = baseColorFactorSrc.size() > 1 ? static_cast<float>(baseColorFactorSrc[1]) : 1.0f;
                  baseColorFactor.z = baseColorFactorSrc.size() > 2 ? static_cast<float>(baseColorFactorSrc[2]) : 1.0f;
                  baseColorFactor.w = baseColorFactorSrc.size() > 3 ? static_cast<float>(baseColorFactorSrc[3]) : 1.0f;
                  material.SetVector(System::String("_baseColorFactor"), baseColorFactor);

                  material.EnableKeyword(System::String("_HASBASECOLOR_ON"));
                }
              }
            }

            const std::optional<TextureInfo>& metallicRoughness = 
                pMaterial->pbrMetallicRoughness->metallicRoughnessTexture;
            if (metallicRoughness) {
              auto texCoordIndexIt = primitiveInfo.uvIndexMap.find(metallicRoughness->texCoord);
              if (texCoordIndexIt != primitiveInfo.uvIndexMap.end()) {
                UnityEngine::Texture texture =
                    TextureLoader::loadTexture(gltf, metallicRoughness->index);
                if (texture != nullptr) {
                  material.SetTexture(System::String("_metallicRoughnessTexture"), texture);
                  material.SetFloat(System::String("_metallicRoughnessTextureCoordinateIndex"), static_cast<float>(texCoordIndexIt->second));

                  UnityEngine::Vector4 metallicRoughnessFactor;
                  metallicRoughnessFactor.x = pMaterial->pbrMetallicRoughness->metallicFactor;
                  metallicRoughnessFactor.y = pMaterial->pbrMetallicRoughness->roughnessFactor;
                  material.SetVector(System::String("_metallicRoughnessFactor"), metallicRoughnessFactor);
                  
                  material.EnableKeyword(System::String("_HASMETALLICROUGHNESS_ON"));
                }
              }
            }
          }

          if (pMaterial->normalTexture) {
            auto texCoordIndexIt = primitiveInfo.uvIndexMap.find(pMaterial->normalTexture->texCoord);
            if (texCoordIndexIt != primitiveInfo.uvIndexMap.end()) {
              UnityEngine::Texture texture = 
                  TextureLoader::loadTexture(gltf, pMaterial->normalTexture->index);
              if (texture != nullptr) {
                material.SetTexture(System::String("_normalMapTexture"), texture);
                material.SetFloat(System::String("_normalMapTextureCoordinateIndex"), static_cast<float>(texCoordIndexIt->second));
                material.SetFloat(System::String("_normalMapScale"), static_cast<float>(pMaterial->normalTexture->scale));

                material.EnableKeyword(System::String("_HASNORMALMAP_ON"));
              }
            }
          }

          if (pMaterial->occlusionTexture) {
            auto texCoordIndexIt = primitiveInfo.uvIndexMap.find(pMaterial->occlusionTexture->texCoord);
            if (texCoordIndexIt != primitiveInfo.uvIndexMap.end()) {
              UnityEngine::Texture texture = 
                  TextureLoader::loadTexture(gltf, pMaterial->occlusionTexture->index);
              if (texture != nullptr) {
                material.SetTexture(System::String("_occlusionTexture"), texture);
                material.SetFloat(System::String("_occlusionTextureCoordinateIndex"), static_cast<float>(texCoordIndexIt->second));
                material.SetFloat(System::String("_occlusionStrength"), static_cast<float>(pMaterial->occlusionTexture->strength));

                material.EnableKeyword(System::String("_HASOCCLUSION_ON"));
              }
            }
          }

          if (pMaterial->emissiveTexture) {
            auto texCoordIndexIt = primitiveInfo.uvIndexMap.find(pMaterial->emissiveTexture->texCoord);
            if (texCoordIndexIt != primitiveInfo.uvIndexMap.end()) {
              UnityEngine::Texture texture = 
                  TextureLoader::loadTexture(gltf, pMaterial->emissiveTexture->index);
              if (texture != nullptr) {
                material.SetTexture(System::String("_emissiveTexture"), texture);
                material.SetFloat(System::String("_emissiveTextureCoordinateIndex"), static_cast<float>(texCoordIndexIt->second));

                const std::vector<double>& emissiveFactorSrc = pMaterial->emissiveFactor;

                UnityEngine::Vector4 emissiveFactor;
                emissiveFactor.x = emissiveFactorSrc.size() > 0 ? static_cast<float>(emissiveFactorSrc[0]) : 0.0f;
                emissiveFactor.y = emissiveFactorSrc.size() > 1 ? static_cast<float>(emissiveFactorSrc[1]) : 0.0f;
                emissiveFactor.z = emissiveFactorSrc.size() > 2 ? static_cast<float>(emissiveFactorSrc[2]) : 0.0f;
                material.SetVector(System::String("_emissiveFactor"), emissiveFactor);

                material.EnableKeyword(System::String("_HASEMISSIVE_ON"));
              }
            }
          }
        }

        // TODO: Actually prevent interleaving more than 3 overlay UVs.
        uint32_t overlayCount = std::min(primitiveInfo.rasterOverlayUvIndexMap.size(), 3ull);
        for (uint32_t i = 0; i < overlayCount; ++i) {
          auto texCoordIndexIt = primitiveInfo.rasterOverlayUvIndexMap.find(i);
          if (texCoordIndexIt != primitiveInfo.rasterOverlayUvIndexMap.end()) {
            material.SetFloat(
                System::String("_overlay" + std::to_string(i) + "TextureCoordinateIndex"),
                static_cast<float>(texCoordIndexIt->second));
          }
        }

        switch (overlayCount) {
          case 0:
          material.EnableKeyword(System::String("_OVERLAYCOUNT_NONE"));
          break;
          case 1:
          material.DisableKeyword(System::String("_OVERLAYCOUNT_NONE"));
          material.EnableKeyword(System::String("_OVERLAYCOUNT_ONE"));
          break;
          case 2:
          material.DisableKeyword(System::String("_OVERLAYCOUNT_NONE"));
          material.EnableKeyword(System::String("_OVERLAYCOUNT_TWO"));
          break;
          // case 3:
          default:
          material.DisableKeyword(System::String("_OVERLAYCOUNT_NONE"));
          material.EnableKeyword(System::String("_OVERLAYCOUNT_THREE"));
        };

        meshFilter.sharedMesh(unityMesh);

        if (createPhysicsMeshes) {
          // This should not trigger mesh baking for physics, because the meshes
          // were already baked in the worker thread.
          UnityEngine::MeshCollider meshCollider =
              primitiveGameObject.AddComponent<UnityEngine::MeshCollider>();
          meshCollider.sharedMesh(unityMesh);
        }
        const ExtensionMeshPrimitiveExtFeatureMetadata* pMetadata =
            primitive.getExtension<ExtensionMeshPrimitiveExtFeatureMetadata>();
        if (pMetadata) {
          pMetadataComponent.NativeImplementation().loadMetadata(
              primitiveGameObject.transform().GetInstanceID(),
              &gltf,
              &primitive);
        }
      });

  CesiumGltfGameObject* pCesiumGameObject = new CesiumGltfGameObject {
    std::move(pModelGameObject),
    std::move(pLoadThreadResult->primitiveInfos)
  };

  return pCesiumGameObject;
}

void UnityPrepareRendererResources::free(
    Cesium3DTilesSelection::Tile& tile,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept {
  if (pMainThreadResult) {
    std::unique_ptr<CesiumGltfGameObject> pCesiumGameObject(
        static_cast<CesiumGltfGameObject*>(pMainThreadResult));

    auto pMetadataComponent =
        pCesiumGameObject->pGameObject
            ->GetComponentInParent<DotNet::CesiumForUnity::CesiumMetadata>();
    if (pMetadataComponent != nullptr) {
      for (int32_t i = 0, len = pCesiumGameObject->pGameObject->transform().childCount(); i < len;
           ++i) {
        pMetadataComponent.NativeImplementation().unloadMetadata(
            pCesiumGameObject->pGameObject->transform().GetChild(i).GetInstanceID());
      }
    }

    UnityLifetime::Destroy(*pCesiumGameObject->pGameObject);
  }
}

void* UnityPrepareRendererResources::prepareRasterInLoadThread(
    CesiumGltf::ImageCesium& image,
    const std::any& rendererOptions) {
  return nullptr;
}

void* UnityPrepareRendererResources::prepareRasterInMainThread(
    Cesium3DTilesSelection::RasterOverlayTile& rasterTile,
    void* pLoadThreadResult) {
  auto pTexture = std::make_unique<UnityEngine::Texture>(
      TextureLoader::loadTexture(rasterTile.getImage()));
  pTexture->wrapMode(UnityEngine::TextureWrapMode::Clamp);
  pTexture->filterMode(UnityEngine::FilterMode::Trilinear);
  pTexture->anisoLevel(16);
  return pTexture.release();
}

void UnityPrepareRendererResources::freeRaster(
    const Cesium3DTilesSelection::RasterOverlayTile& rasterTile,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept {
  if (pMainThreadResult) {
    std::unique_ptr<UnityEngine::Texture> pTexture(
        static_cast<UnityEngine::Texture*>(pMainThreadResult));
    UnityLifetime::Destroy(*pTexture);
  }
}

void UnityPrepareRendererResources::attachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t overlayTextureCoordinateID,
    const Cesium3DTilesSelection::RasterOverlayTile& rasterTile,
    void* pMainThreadRendererResources,
    const glm::dvec2& translation,
    const glm::dvec2& scale) {
  const Cesium3DTilesSelection::TileContent& content = tile.getContent();
  const Cesium3DTilesSelection::TileRenderContent* pRenderContent =
      content.getRenderContent();
  if (!pRenderContent) {
    return;
  }

  CesiumGltfGameObject* pCesiumGameObject = static_cast<CesiumGltfGameObject*>(
      pRenderContent->getRenderResources());
  UnityEngine::Texture* pTexture =
      static_cast<UnityEngine::Texture*>(pMainThreadRendererResources);
  if (!pCesiumGameObject || !pCesiumGameObject->pGameObject || !pTexture)
    return;

  // TODO: Can we count on the order of primitives in the transform chain
  // to match the order of primitives using gltf->forEachPrimitive??
  uint32_t primitiveIndex = 0;

  UnityEngine::Transform transform = pCesiumGameObject->pGameObject->transform();
  for (int32_t i = 0, len = transform.childCount(); i < len; ++i) {
    UnityEngine::Transform childTransform = transform.GetChild(i);
    if (childTransform == nullptr)
      continue;

    UnityEngine::GameObject child = childTransform.gameObject();
    if (child == nullptr)
      continue;

    UnityEngine::MeshRenderer meshRenderer =
        child.GetComponent<UnityEngine::MeshRenderer>();
    if (meshRenderer == nullptr)
      continue;

    UnityEngine::Material material = meshRenderer.sharedMaterial();
    if (material == nullptr)
      continue;

    const CesiumPrimitiveInfo& primitiveInfo = pCesiumGameObject->primitiveInfos[primitiveIndex++];
    auto texCoordIndexIt = primitiveInfo.rasterOverlayUvIndexMap.find(overlayTextureCoordinateID);
    if (texCoordIndexIt == primitiveInfo.rasterOverlayUvIndexMap.end()) {
      // The associated UV coords for this overlay are missing.
      // TODO: log warning?
      continue;
    }

    std::string overlayIndexStr = std::to_string(overlayTextureCoordinateID);
    material.SetTexture(System::String("_overlay" + overlayIndexStr + "Texture"), *pTexture);

    UnityEngine::Vector4 translationAndScale{
        float(translation.x),
        float(translation.y),
        float(scale.x),
        float(scale.y)};
    material.SetVector(
        System::String("_overlay" + overlayIndexStr + "TranslationAndScale"),
        translationAndScale);
  }
}

void UnityPrepareRendererResources::detachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t overlayTextureCoordinateID,
    const Cesium3DTilesSelection::RasterOverlayTile& rasterTile,
    void* pMainThreadRendererResources) noexcept {
  const Cesium3DTilesSelection::TileContent& content = tile.getContent();
  const Cesium3DTilesSelection::TileRenderContent* pRenderContent =
      content.getRenderContent();
  if (!pRenderContent) {
    return;
  }

  CesiumGltfGameObject* pCesiumGameObject = static_cast<CesiumGltfGameObject*>(
      pRenderContent->getRenderResources());
  UnityEngine::Texture* pTexture =
      static_cast<UnityEngine::Texture*>(pMainThreadRendererResources);
  if (!pCesiumGameObject || !pCesiumGameObject->pGameObject || !pTexture) 
    return;

  UnityEngine::Transform transform = pCesiumGameObject->pGameObject->transform();
  for (int32_t i = 0, len = transform.childCount(); i < len; ++i) {
    UnityEngine::Transform childTransform = transform.GetChild(i);
    if (childTransform == nullptr)
      continue;

    UnityEngine::GameObject child = childTransform.gameObject();
    if (child == nullptr)
      continue;

    UnityEngine::MeshRenderer meshRenderer =
        child.GetComponent<UnityEngine::MeshRenderer>();
    if (meshRenderer == nullptr)
      continue;

    UnityEngine::Material material = meshRenderer.sharedMaterial();
    if (material == nullptr)
      continue;

    material.SetTexture(
        System::String("_overlay" + std::to_string(overlayTextureCoordinateID) + "Texture"),
        UnityEngine::Texture(nullptr));
  }
}
