#pragma once

#include "Util.hlsli"

#define WAVE_SIZE 32
#define GROUP_SIZE_AS WAVE_SIZE

struct TPayload
{
    float4 Position;
    TMeshlet Meshlets[GROUP_SIZE_AS];
    float4 AdditionalInfo[GROUP_SIZE_AS];
    uint MeshletIndex[GROUP_SIZE_AS];
};

#define ROOT_SIG                                                                                                       \
    "CBV(b0),"                                                                                                         \
    "RootConstants(b1, num32bitconstants=2),"                                                                          \
    "SRV(t0),"                                                                                                         \
    "SRV(t1),"                                                                                                         \
    "SRV(t2),"                                                                                                         \
    "SRV(t3)"

ConstantBuffer<TMainCB> MainCB : register(b0);
ConstantBuffer<TMesh> MeshInfo : register(b1);

StructuredBuffer<TVertex> Vertices : register(t0);
StructuredBuffer<uint> Primitives : register(t1);
StructuredBuffer<TMeshlet> Meshlets : register(t2);
StructuredBuffer<TBoundingBox> MeshletBoxes : register(t3);
