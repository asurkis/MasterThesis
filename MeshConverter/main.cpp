#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

#include <metis.h>
#include <tiny_gltf.h>

#define DBGOUT

#if IDXTYPEWIDTH == 32
#define IDX_C INT32_C
#elif IDXTYPEWIDTH == 64
#define IDX_C INT64_C
#else
#error "IDTYPEWIDTH not set"
#endif

using MeshEdge = std::pair<idx_t, idx_t>;

struct Vec3
{
    union {
        struct
        {
            float x, y, z;
        };
        struct
        {
            float m[3];
        };
    };
};

int main()
{
    tinygltf::Model    model;
    tinygltf::TinyGLTF tinyGltfCtx;
    std::string        err;
    std::string        warn;
    bool               success = tinyGltfCtx.LoadBinaryFromFile(&model, &err, &warn, "input.glb");
    if (!err.empty()) std::cerr << err << '\n';
    if (!warn.empty()) std::cerr << warn << '\n';
    if (!success) throw std::runtime_error("Fail");

    if (model.meshes.size() != 1) throw std::runtime_error("Should have one mesh");
    tinygltf::Mesh &mesh = model.meshes[0];

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

    tinygltf::Accessor   &positionAccessor   = model.accessors[positionIdx];
    tinygltf::BufferView &positionBufferView = model.bufferViews[positionAccessor.bufferView];
    tinygltf::Buffer     &positionBuffer     = model.buffers[positionBufferView.buffer];
    float *positionFloats = (float *)&positionBuffer.data[positionBufferView.byteOffset + positionAccessor.byteOffset];

    if (positionAccessor.type != TINYGLTF_TYPE_VEC3) throw std::runtime_error("Position is not vec3");
    if (positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        throw std::runtime_error("Position component type is not float");

    tinygltf::Accessor   &indexAccessor   = model.accessors[primitive.indices];
    tinygltf::BufferView &indexBufferView = model.bufferViews[indexAccessor.bufferView];
    tinygltf::Buffer     &indexBuffer     = model.buffers[indexBufferView.buffer];

    unsigned char *indexBytes = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];

    int indexComponentSize = 0;
    switch (indexAccessor.componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: indexComponentSize = 1; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: indexComponentSize = 2; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: indexComponentSize = 4; break;
    default: throw std::runtime_error("Unknown component type");
    }

    std::vector<idx_t> indices(indexAccessor.count);
    for (size_t indexId = 0; indexId < indexAccessor.count; ++indexId)
    {
        idx_t idx = 0;
        // little endian
        for (int indexByteOff = 0; indexByteOff < indexComponentSize; ++indexByteOff)
            idx += indexBytes[indexComponentSize * indexId + indexByteOff] << 8 * indexByteOff;
        indices[indexId] = idx;
    }

    std::map<MeshEdge, std::vector<idx_t>> trianglesByEdge;

    idx_t nTriangles = indexAccessor.count / 3;
    for (idx_t triangleId = 0; triangleId < nTriangles; ++triangleId)
    {
#ifdef DBGOUT
        std::cout << "[[" << triangleId << "]]:";
#endif

        idx_t vertexIdx[3] = {};
        for (int triangleVertexId = 0; triangleVertexId < 3; ++triangleVertexId)
        {
            idx_t idx                   = indices[IDX_C(3) * triangleId + triangleVertexId];
            vertexIdx[triangleVertexId] = idx;

#ifdef DBGOUT
            float *pPos = positionFloats + 3 * idx;

            Vec3 pos = {};
            for (int componentId = 0; componentId < 3; ++componentId) pos.m[componentId] = pPos[componentId];

            std::cout << "    [" << idx << "] = (" << pos.x << ", " << pos.y << ", " << pos.z << ")";
#endif
        }
#ifdef DBGOUT
        std::cout << '\n';
#endif

        for (int triangleEdgeId = 0; triangleEdgeId < 3; ++triangleEdgeId)
        {
            idx_t v = vertexIdx[triangleEdgeId];
            idx_t u = vertexIdx[(triangleEdgeId + 1) % 3];
            if (v == u) throw std::runtime_error("Collapsed triangle");
            if (v > u) std::swap(v, u);
            MeshEdge edge = {v, u};
            auto     iter = trianglesByEdge.try_emplace(edge);
            auto    &vec  = iter.first->second;
            if (!vec.empty() && vec[vec.size() - 1] == triangleId)
                throw std::runtime_error("Triangle with repeated edge?!");
            vec.push_back(triangleId);
        }
    }

#ifdef DBGOUT
    std::cout << "\nBy edge:\n";
    for (auto &byEdge : trianglesByEdge)
    {
        auto &edge = byEdge.first;
        auto &tris = byEdge.second;
        std::cout << "[" << edge.first << ", " << edge.second << "]: [";
        for (size_t i = 0; i < tris.size(); ++i)
        {
            if (i != 0) std::cout << ", ";
            std::cout << tris[i];
        }
        std::cout << "]\n";
    }
#endif

    idx_t              nvtxs = nTriangles;
    idx_t              ncon  = 1;
    std::vector<idx_t> xadj;
    xadj.reserve(nvtxs + 1);
    xadj.push_back(0);
    std::vector<idx_t> adjncy;
    for (idx_t triangleId = 0; triangleId < nTriangles; ++triangleId)
    {
        idx_t vertices[3];
        for (idx_t vertexId = 0; vertexId < 3; ++vertexId)
            vertices[vertexId] = indices[IDX_C(3) * triangleId + vertexId];
        for (idx_t edgeId = 0; edgeId < 3; ++edgeId)
        {
            idx_t v = vertices[edgeId];
            idx_t u = vertices[(edgeId + 1) % 3];
            if (v > u) std::swap(v, u);
            MeshEdge edge = {v, u};
            auto    &vec  = trianglesByEdge[edge];
            for (idx_t otherTriangle : vec)
            {
                if (otherTriangle == triangleId) continue;
                adjncy.push_back(otherTriangle);
            }
        }
        xadj.push_back(adjncy.size());
    }
    idx_t nparts = (nTriangles + 255) / 256;
    nparts       = std::max(nparts, IDX_C(2));

    idx_t              edgecut = 0;
    std::vector<idx_t> part(nvtxs, 0);

#ifdef DBGOUT
    std::cout << "\nAdjacency:\n";
    for (idx_t triangleId = 0; triangleId < nTriangles; ++triangleId)
    {
        std::cout << triangleId << ':';
        idx_t beg = xadj[triangleId];
        idx_t end = xadj[triangleId + 1];
        for (idx_t i = beg; i < end; ++i) std::cout << ' ' << adjncy[i];
        std::cout << '\n';
    }
#endif

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
                                          &nparts,
                                          nullptr,
                                          nullptr,
                                          options,
                                          &edgecut,
                                          part.data());
    if (metisResult != METIS_OK) throw std::runtime_error("METIS failed");

#ifdef DBGOUT
    for (idx_t triangleId = 0; triangleId < nTriangles; ++triangleId)
    {
        std::cout << "[[" << triangleId << "]] => " << part[triangleId] << '\n';
    }
#endif

    return 0;
}
