#include <Common.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

#include <metis.h>
#include <tiny_gltf.h>

using namespace DirectX;

constexpr bool DBGOUT = false;

#if IDXTYPEWIDTH == 32
#define IDX_C INT32_C
#elif IDXTYPEWIDTH == 64
#define IDX_C INT64_C
#else
#error "IDTYPEWIDTH not set"
#endif

using MeshEdge = std::pair<idx_t, idx_t>;

int main()
{
    tinygltf::Model    inModel;
    tinygltf::TinyGLTF tinyGltfCtx;
    std::string        err;
    std::string        warn;
    bool               success = tinyGltfCtx.LoadBinaryFromFile(&inModel, &err, &warn, "input.glb");
    if (!err.empty()) std::cerr << err << '\n';
    if (!warn.empty()) std::cerr << warn << '\n';
    if (!success) throw std::runtime_error("Fail");

    if (inModel.meshes.size() != 1) throw std::runtime_error("Should have one mesh");
    tinygltf::Mesh &mesh = inModel.meshes[0];

    if (mesh.primitives.size() != 1) throw std::runtime_error("Should be one primitive");
    tinygltf::Primitive &primitive = mesh.primitives[0];
    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) throw std::runtime_error("Only triangles are supported");

    int positionIdx = -1;
    int normalIdx   = -1;
    for (auto &&pair : primitive.attributes)
    {
        if (pair.first == "POSITION")
        {
            if (positionIdx != -1) throw std::runtime_error("Two positions found");
            positionIdx = pair.second;
        }
        else if (pair.first == "NORMAL")
        {
            if (normalIdx != -1) throw std::runtime_error("Two normals found");
            normalIdx = pair.second;
        }
    }
    if (positionIdx == -1) throw std::runtime_error("No position found");

    tinygltf::Accessor   &positionAccessor   = inModel.accessors[positionIdx];
    tinygltf::BufferView &positionBufferView = inModel.bufferViews[positionAccessor.bufferView];
    tinygltf::Buffer     &positionBuffer     = inModel.buffers[positionBufferView.buffer];

    tinygltf::Accessor   &normalAccessor   = inModel.accessors[normalIdx];
    tinygltf::BufferView &normalBufferView = inModel.bufferViews[normalAccessor.bufferView];
    tinygltf::Buffer     &normalBuffer     = inModel.buffers[normalBufferView.buffer];

    size_t positionStride = positionAccessor.ByteStride(positionBufferView);
    size_t normalStride   = normalAccessor.ByteStride(normalBufferView);

    if constexpr (DBGOUT) std::cout << "nPositions = " << positionAccessor.count << std::endl;
    if (positionAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Position is not vec3");
    if (positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        throw std::runtime_error("Position component type is not float");

    if (normalAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Normal is not vec3");
    if (normalAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        throw std::runtime_error("Normal component type is not float");

    std::vector<Vertex> vertices;
    vertices.reserve(positionAccessor.count);
    XMVECTOR posMin;
    XMVECTOR posMax;

    for (size_t iVert = 0; iVert < positionAccessor.count; ++iVert)
    {
        size_t offPosition = positionBufferView.byteOffset;
        offPosition += positionAccessor.byteOffset;
        offPosition += positionStride * iVert;

        size_t offNormal = normalBufferView.byteOffset;
        offNormal += normalAccessor.byteOffset;
        offNormal += normalStride * iVert;
        unsigned char *pPosition = &positionBuffer.data[offPosition];
        unsigned char *pNormal   = &normalBuffer.data[offNormal];

        Vertex vert   = {};
        vert.Position = *reinterpret_cast<float3 *>(pPosition);
        vert.Normal   = *reinterpret_cast<float3 *>(pNormal);
        vertices.push_back(vert);

        XMVECTOR pos = XMLoadFloat3(&vert.Position);
        if (iVert == 0)
        {
            posMin = pos;
            posMax = pos;
        }
        else
        {
            posMin = XMVectorMin(posMin, pos);
            posMax = XMVectorMax(posMax, pos);
        }
    }

    tinygltf::Accessor   &indexAccessor   = inModel.accessors[primitive.indices];
    tinygltf::BufferView &indexBufferView = inModel.bufferViews[indexAccessor.bufferView];
    tinygltf::Buffer     &indexBuffer     = inModel.buffers[indexBufferView.buffer];

    unsigned char *indexBytes = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];

    int indexComponentSize = 0;
    switch (indexAccessor.componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: indexComponentSize = 1; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: indexComponentSize = 2; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: indexComponentSize = 4; break;
    default: throw std::runtime_error("Unknown component type");
    }

    if (indexComponentSize != indexAccessor.ByteStride(indexBufferView)) throw std::runtime_error("Non packed indices");

    std::vector<idx_t> indices(indexAccessor.count);
    for (size_t iiVert = 0; iiVert < indexAccessor.count; ++iiVert)
    {
        idx_t iVert = 0;
        // little endian
        for (int indexByteOff = 0; indexByteOff < indexComponentSize; ++indexByteOff)
            iVert += (idx_t)indexBytes[indexComponentSize * iiVert + indexByteOff] << 8 * indexByteOff;
        indices[iiVert] = iVert;
    }

    std::map<MeshEdge, std::vector<idx_t>> edgeTriangles;

    size_t nTriangles = indexAccessor.count / 3;
    for (size_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
    {
        if constexpr (DBGOUT) std::cout << "T[" << iTriangle << "]:";

        idx_t vertexIdx[3] = {};
        for (int iTriVert = 0; iTriVert < 3; ++iTriVert)
        {
            idx_t iVert         = indices[IDX_C(3) * iTriangle + iTriVert];
            vertexIdx[iTriVert] = iVert;

            if constexpr (DBGOUT)
            {
                float3 pos = vertices[iVert].Position;
                std::cout << "    V[" << iVert << "] = (" << pos.x << ", " << pos.y << ", " << pos.z << ")";
            }
        }
        if constexpr (DBGOUT) std::cout << '\n';

        for (int iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
        {
            idx_t v = vertexIdx[iTriEdge];
            idx_t u = vertexIdx[(iTriEdge + 1) % 3];
            if (v == u) throw std::runtime_error("Collapsed triangle");
            if (v > u) std::swap(v, u);
            MeshEdge edge = {v, u};
            auto     iter = edgeTriangles.try_emplace(edge);
            auto    &vec  = iter.first->second;
            if (!vec.empty() && vec[vec.size() - 1] == iTriangle)
                throw std::runtime_error("Triangle with repeated edge?!");
            vec.push_back(idx_t(iTriangle));
        }
    }

    if constexpr (DBGOUT)
    {
        std::cout << "\nBy edge:\n";
        for (auto &[edge, tris] : edgeTriangles)
        {
            std::cout << "V[" << edge.first << ", " << edge.second << "]: T[";
            for (size_t i = 0; i < tris.size(); ++i)
            {
                if (i != 0) std::cout << ", ";
                std::cout << tris[i];
            }
            std::cout << "]\n";
        }
    }

    // Готовим структуру для первого вызова METIS
    constexpr uint TARGET_PRIMITIVES = MESHLET_MAX_PRIMITIVES * 7 / 8;

    idx_t nParts = idx_t((nTriangles + TARGET_PRIMITIVES - 1) / TARGET_PRIMITIVES);
    nParts       = std::max(nParts, IDX_C(2));
    idx_t nvtxs  = nTriangles;

    idx_t              ncon = 1;
    std::vector<idx_t> xadj;
    xadj.reserve(size_t(nvtxs) + 1);
    xadj.push_back(0);
    std::vector<idx_t> adjncy;
    std::vector<idx_t> adjwgt;

    XMVECTOR maxDiff = posMax - posMin;
    float    maxLen  = XMVectorGetX(XMVector3Length(maxDiff)) * (1.0f + FLT_EPSILON);

    for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
    {
        idx_t vertexIndices[3] = {};
        for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            vertexIndices[iTriVert] = indices[IDX_C(3) * iTriangle + iTriVert];
        for (idx_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
        {
            idx_t iVert = vertexIndices[iTriEdge];
            idx_t jVert = vertexIndices[(iTriEdge + 1) % 3];
            if (iVert > jVert) std::swap(iVert, jVert);
            MeshEdge edge = {iVert, jVert};
            auto    &vec  = edgeTriangles[edge];

            float3   posi  = vertices[iVert].Position;
            float3   posj  = vertices[jVert].Position;
            XMVECTOR posiv = XMLoadFloat3(&posi);
            XMVECTOR posjv = XMLoadFloat3(&posj);
            float    len   = XMVectorGetX(XMVector3Length(posjv - posiv));

            for (idx_t iOtherTriangle : vec)
            {
                if (iOtherTriangle == iTriangle) continue;
                adjncy.push_back(iOtherTriangle);
                adjwgt.push_back(idx_t(IDX_C(0x7FFFFFFF) * len / maxLen));
            }
        }
        xadj.push_back(idx_t(adjncy.size()));
    }
    for (idx_t i = nTriangles; i < nvtxs; ++i) xadj.push_back(idx_t(adjncy.size()));

    idx_t              edgecut = 0;
    std::vector<idx_t> trianglePart(nvtxs, 0);

    if constexpr (DBGOUT)
    {
        std::cout << "\nAdjacency:\n";
        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
        {
            std::cout << iTriangle << ':';
            idx_t beg = xadj[iTriangle];
            idx_t end = xadj[size_t(iTriangle) + 1];
            for (idx_t i = beg; i < end; ++i) std::cout << ' ' << adjncy[i];
            std::cout << '\n';
        }
    }

    idx_t options[METIS_NOPTIONS] = {};
    METIS_SetDefaultOptions(options);
    options[METIS_OPTION_NUMBERING] = 0;

    // Для разделения кроме первой фазы нужно будет
    // использовать графы
    int metisResult = METIS_PartGraphRecursive(&nvtxs,
                                               &ncon,
                                               xadj.data(),
                                               adjncy.data(),
                                               nullptr,
                                               nullptr,
                                               adjwgt.data(),
                                               &nParts,
                                               nullptr,
                                               nullptr,
                                               options,
                                               &edgecut,
                                               trianglePart.data());
    if (metisResult != METIS_OK) throw std::runtime_error("METIS failed");

    if constexpr (DBGOUT)
    {
        std::cout << "\nClusterization:\n";
        for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
            std::cout << "T[" << iTriangle << "] => " << trianglePart[iTriangle] << '\n';
    }

    ModelCPU outModel;

    // Вершины остаются без изменений
    outModel.Vertices = vertices;

    std::vector<std::vector<idx_t>>     partTriangles(nParts);
    std::vector<std::map<idx_t, idx_t>> partVertexMap(nParts);

    for (idx_t iTriangle = 0; iTriangle < nTriangles; ++iTriangle)
    {
        idx_t iPart = trianglePart[iTriangle];
        partTriangles[iPart].push_back(iTriangle);

        auto &verts = partVertexMap[iPart];
        for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
        {
            idx_t iVert = indices[IDX_C(3) * iTriangle + iTriVert];
            verts.try_emplace(iVert, idx_t(verts.size()));
        }
    }

    if constexpr (true)
    {
        for (size_t iPart = 0; iPart < nParts; ++iPart)
        {
            auto &part = partTriangles[iPart];
            if (part.size() > MESHLET_MAX_PRIMITIVES)
                std::cout << "Part[" << iPart << "].size() = " << part.size() << "\n";
        }
    }

    outModel.Meshlets.reserve(nParts);
    for (idx_t iPart = 0; iPart < nParts; ++iPart)
    {
        auto &vertMap   = partVertexMap[iPart];
        auto &triangles = partTriangles[iPart];

        MeshletDesc meshlet = {};
        meshlet.VertOffset  = outModel.GlobalIndices.size();
        meshlet.VertCount   = vertMap.size();
        meshlet.PrimOffset  = outModel.Primitives.size();
        meshlet.PrimCount   = triangles.size();
        outModel.Meshlets.push_back(meshlet);

        std::vector<idx_t> vertMapRev(vertMap.size());
        for (auto &[iVert, iLocVert] : vertMap) vertMapRev[iLocVert] = iVert;
        for (idx_t iVert : vertMapRev) outModel.GlobalIndices.push_back(iVert);

        for (idx_t iTriangle : triangles)
        {
            uint prim = 0;
            for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                idx_t iVert    = indices[IDX_C(3) * iTriangle + iTriVert];
                idx_t iLocVert = vertMap[iVert];
                prim |= iLocVert << (10 * iTriVert);
            }
            outModel.Primitives.push_back(prim);
        }
    }

    MeshDesc outMesh      = {};
    outMesh.MeshletCount  = nParts;
    outMesh.MeshletOffset = 0;
    outModel.Meshes.push_back(outMesh);

    outModel.SaveToFile("../MasterThesis/model.bin");

    if constexpr (DBGOUT)
    {
        std::cout << "\nOut model:\nVertices:\n";
        for (size_t iVert = 0; iVert < outModel.Vertices.size(); ++iVert)
        {
            auto &vert = outModel.Vertices[iVert];
            std::cout << "V[" << iVert << "] = (" << vert.Position.x << ", " << vert.Position.y << ", "
                      << vert.Position.z << ")\n";
        }

        std::cout << "\nGlobal indices:\n";
        for (size_t ii = 0; ii < outModel.GlobalIndices.size(); ++ii)
        {
            uint i = outModel.GlobalIndices[ii];
            std::cout << "GI[" << ii << "] = " << i << "\n";
        }

        std::cout << "\nTriangles:\n";
        for (size_t iTriangle = 0; iTriangle < outModel.Primitives.size(); ++iTriangle)
        {
            uint triCode = outModel.Primitives[iTriangle];
            std::cout << "T[" << iTriangle << "] = L[";
            for (uint iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                if (iTriVert != 0) std::cout << ", ";
                std::cout << ((triCode >> (10 * iTriVert)) & 0x3FF);
            }
            std::cout << "]\n";
        }

        std::cout << "\nMeshlets:\n";
        for (size_t iMeshlet = 0; iMeshlet < outModel.Meshlets.size(); ++iMeshlet)
        {
            MeshletDesc &meshlet = outModel.Meshlets[iMeshlet];
            std::cout << "M[" << iMeshlet << "] = { VertOffset: " << meshlet.VertOffset
                      << ", VertCount: " << meshlet.VertCount << ", PrimOffset: " << meshlet.PrimOffset
                      << ", PrimCount: " << meshlet.PrimCount << "}\n";
        }

        std::cout << "\nMeshlets unwrapped:\n";
        for (size_t iMeshlet = 0; iMeshlet < outModel.Meshlets.size(); ++iMeshlet)
        {
            std::cout << "M[" << iMeshlet << "]:\n";
            MeshletDesc &meshlet = outModel.Meshlets[iMeshlet];
            for (uint iMeshletTriangle = 0; iMeshletTriangle < meshlet.PrimCount; ++iMeshletTriangle)
            {
                uint iTriangle = meshlet.PrimOffset + iMeshletTriangle;
                std::cout << "    T[" << iTriangle << "] = V[";
                uint triangleCode = outModel.Primitives[iTriangle];
                for (uint iTriVert = 0; iTriVert < 3; ++iTriVert)
                {
                    if (iTriVert != 0) std::cout << ", ";
                    uint iLocVert = (triangleCode >> (10 * iTriVert)) & 0x3FF;
                    uint iVert    = outModel.GlobalIndices[size_t(meshlet.VertOffset) + iLocVert];
                    std::cout << iVert;
                }
                std::cout << "]\n";
            }
        }
    }

    return 0;
}
