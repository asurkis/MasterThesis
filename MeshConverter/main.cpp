#include <Common.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

#include <metis.h>
#include <tiny_gltf.h>

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
    for (auto &&pair : primitive.attributes)
    {
        if (pair.first != "POSITION") continue;
        if (positionIdx != -1) throw std::runtime_error("Two positions found");
        positionIdx = pair.second;
    }
    if (positionIdx == -1) throw std::runtime_error("No position found");

    tinygltf::Accessor   &positionAccessor   = inModel.accessors[positionIdx];
    tinygltf::BufferView &positionBufferView = inModel.bufferViews[positionAccessor.bufferView];
    tinygltf::Buffer     &positionBuffer     = inModel.buffers[positionBufferView.buffer];

    size_t positionStride = positionBufferView.byteStride;
    if (positionStride == 0) positionStride = sizeof(float3);
    size_t nPositions = positionBufferView.byteLength / positionStride;
    if constexpr (DBGOUT) std::cout << "nPositions = " << nPositions << std::endl;
    unsigned char *positionBytes = &positionBuffer.data[positionBufferView.byteOffset + positionAccessor.byteOffset];

    if (positionAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Position is not vec3");
    if (positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        throw std::runtime_error("Position component type is not float");

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
            iVert += indexBytes[indexComponentSize * iiVert + indexByteOff] << 8 * indexByteOff;
        indices[iiVert] = iVert;
    }

    std::map<MeshEdge, std::vector<idx_t>> edgeTriangles;

    size_t nTriangles = indexAccessor.count / 3;
    for (size_t iTri = 0; iTri < nTriangles; ++iTri)
    {
        if constexpr (DBGOUT) std::cout << "T[" << iTri << "]:";

        idx_t vertexIdx[3] = {};
        for (int iTriVert = 0; iTriVert < 3; ++iTriVert)
        {
            idx_t iVert         = indices[IDX_C(3) * iTri + iTriVert];
            vertexIdx[iTriVert] = iVert;

            if constexpr (DBGOUT)
            {
                unsigned char *pVert = positionBytes + positionStride * iVert;
                float3         pos   = *reinterpret_cast<float3 *>(pVert);
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
            if (!vec.empty() && vec[vec.size() - 1] == iTri) throw std::runtime_error("Triangle with repeated edge?!");
            vec.push_back(idx_t(iTri));
        }
    }

    if constexpr (DBGOUT)
    {
        std::cout << "\nBy edge:\n";
        for (auto &[edge, tris] : edgeTriangles)
        {
            std::cout << "(V[" << edge.first << "], V[" << edge.second << "]): [";
            for (size_t i = 0; i < tris.size(); ++i)
            {
                if (i != 0) std::cout << ", ";
                std::cout << "T[" << tris[i] << "]";
            }
            std::cout << "]\n";
        }
    }

    // Готовим структуру для первого вызова METIS
    idx_t              nvtxs = idx_t(nTriangles);
    idx_t              ncon  = 1;
    std::vector<idx_t> xadj;
    xadj.reserve(size_t(nvtxs) + 1);
    xadj.push_back(0);
    std::vector<idx_t> adjncy;
    for (idx_t iTri = 0; iTri < nTriangles; ++iTri)
    {
        idx_t vertices[3] = {};
        for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert) vertices[iTriVert] = indices[IDX_C(3) * iTri + iTriVert];
        for (idx_t iTriEdge = 0; iTriEdge < 3; ++iTriEdge)
        {
            idx_t v = vertices[iTriEdge];
            idx_t u = vertices[(iTriEdge + 1) % 3];
            if (v > u) std::swap(v, u);
            MeshEdge edge = {v, u};
            auto    &vec  = edgeTriangles[edge];
            for (idx_t iOtherTri : vec)
            {
                if (iOtherTri == iTri) continue;
                adjncy.push_back(iOtherTri);
            }
        }
        xadj.push_back(idx_t(adjncy.size()));
    }
    idx_t nParts = idx_t((nTriangles + 127) / 128);
    nParts       = std::max(nParts, IDX_C(2));

    idx_t              edgecut = 0;
    std::vector<idx_t> trianglePart(nvtxs, 0);

    if constexpr (DBGOUT)
    {
        std::cout << "\nAdjacency:\n";
        for (idx_t iTri = 0; iTri < nTriangles; ++iTri)
        {
            std::cout << iTri << ':';
            idx_t beg = xadj[iTri];
            idx_t end = xadj[size_t(iTri) + 1];
            for (idx_t i = beg; i < end; ++i) std::cout << ' ' << adjncy[i];
            std::cout << '\n';
        }
    }

    idx_t options[METIS_NOPTIONS] = {};
    METIS_SetDefaultOptions(options);
    options[METIS_OPTION_NUMBERING] = 0;

    // Для разделения кроме первой фазы нужно будет
    // использовать графы
    int metisResult = METIS_PartGraphKway(&nvtxs,
                                          &ncon,
                                          xadj.data(),
                                          adjncy.data(),
                                          nullptr,
                                          nullptr,
                                          nullptr,
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
        for (idx_t iTri = 0; iTri < nTriangles; ++iTri)
            std::cout << "T[" << iTri << "] => " << trianglePart[iTri] << '\n';
    }

    ModelCPU outModel;

    // Вершины остаются без изменений
    outModel.Vertices.reserve(nPositions);
    for (size_t iVert = 0; iVert < nPositions; ++iVert)
    {
        unsigned char *pVert = positionBytes + positionStride * iVert;
        Vertex         vert  = {};
        vert.Position        = *reinterpret_cast<float3 *>(pVert);
        outModel.Vertices.push_back(vert);
    }

    std::vector<std::vector<idx_t>>     partTriangles(nParts);
    std::vector<std::map<idx_t, idx_t>> partVertexMap(nParts);

    for (idx_t iTri = 0; iTri < nTriangles; ++iTri)
    {
        idx_t iPart = trianglePart[iTri];
        partTriangles[iPart].push_back(iTri);

        auto &verts = partVertexMap[iPart];
        for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
        {
            idx_t iVert = indices[IDX_C(3) * iTri + iTriVert];
            verts.try_emplace(iVert, idx_t(verts.size()));
        }
    }

    outModel.Meshlets.reserve(nParts);
    for (idx_t iPart = 0; iPart < nParts; ++iPart)
    {
        auto &vertMap = partVertexMap[iPart];
        auto &tris    = partTriangles[iPart];

        MeshletDesc meshlet = {};
        meshlet.VertOffset  = outModel.GlobalIndices.size();
        meshlet.VertCount   = vertMap.size();
        meshlet.PrimOffset  = outModel.LocalIndices.size();
        meshlet.PrimCount   = tris.size();
        outModel.Meshlets.push_back(meshlet);

        for (auto &[iGlobVert, iLocVert] : vertMap) outModel.GlobalIndices.push_back(iGlobVert);
        for (idx_t iTri : tris)
        {
            uint prim = 0;
            for (idx_t iTriVert = 0; iTriVert < 3; ++iTriVert)
            {
                idx_t iGlobVert = indices[IDX_C(3) * iTri + iTriVert];
                idx_t iLocVert  = vertMap[iGlobVert];
                prim            = 1024 * prim + iLocVert;
            }
            outModel.LocalIndices.push_back(prim);
        }
    }

    MeshDesc outMesh      = {};
    outMesh.MeshletCount  = nParts;
    outMesh.MeshletOffset = 0;
    outModel.Meshes.push_back(outMesh);

    outModel.SaveToFile("output.bin");

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

        std::cout << "\nLocal indices:\n";
        for (size_t ii = 0; ii < outModel.LocalIndices.size(); ++ii)
        {
            uint i = outModel.LocalIndices[ii];
            std::cout << "LI[" << ii << "] = " << i << "\n";
        }

        std::cout << "\nMeshlets:\n";
        for (size_t iMeshlet = 0; iMeshlet < outModel.Meshlets.size(); ++iMeshlet)
        {
            MeshletDesc &meshlet = outModel.Meshlets[iMeshlet];
            std::cout << "M[" << iMeshlet << "] = { VertOffset: " << meshlet.VertOffset
                      << ", VertCount: " << meshlet.VertCount << ", PrimOffset: " << meshlet.PrimOffset
                      << ", PrimCount: " << meshlet.PrimCount << "}\n";
        }
    }

    return 0;
}
