#include "Util.hlsli"
#include "MainCommon.hlsli"

[maxvertexcount(3)]
void main(triangle TVertexOut input[3], inout TriangleStream<TVertexOut> os)
{
    uint i;
    TVertexOut verts[3];
    for (i = 0; i < 3; ++i)
        verts[i] = input[i];
    float4 oa = verts[0].PositionHS;
    float4 ob = verts[1].PositionHS;
    float4 oc = verts[2].PositionHS;
    float3 color = TriangleHeatmapColor(oa, ob, oc);
    for (i = 0; i < 3; ++i)
    {
        verts[i].DiffuseColor = color;
        os.Append(verts[i]);
    }
    os.RestartStrip();
}
