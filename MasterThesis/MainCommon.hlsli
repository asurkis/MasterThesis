#pragma once

#include "Shared.h"


struct TVertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    float3 Normal : NORMAL0;
    uint MeshletIndex : MESHLET_INDEX;
};
