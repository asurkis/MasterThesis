#pragma once

#include "Util.hlsli"

struct TVertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    uint MeshletIndex : MESHLET_INDEX;
};
