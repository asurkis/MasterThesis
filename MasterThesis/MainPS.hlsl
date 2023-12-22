struct VertexOut
{
    float4 PositionHS : SV_Position;
    float3 PositionVS : POSITION0;
    float3 Normal : NORMAL0;
    uint   MeshletIndex : MESHLET_INDEX;
};

float4 main(VertexOut input) : SV_Target
{
    float  ambientIntensity = 0.1;
    float3 lightColor       = float3(1, 1, 1);
    float3 lightDir         = -normalize(float3(-1, -1, -10));

    uint   meshletIndex = input.MeshletIndex;
    float3 diffuseColor = float3(float(meshletIndex & 1), float(meshletIndex & 3) / 4, float(meshletIndex & 7) / 8);
    float  shininess    = 16.0;

    float3 normal = normalize(input.Normal);

    // Do some fancy Blinn-Phong shading!
    float  cosAngle  = saturate(dot(normal, lightDir));
    float3 viewDir   = -normalize(input.PositionVS);
    float3 halfAngle = normalize(lightDir + viewDir);

    float blinnTerm = saturate(dot(normal, halfAngle));
    blinnTerm       = cosAngle != 0.0 ? blinnTerm : 0.0;
    blinnTerm       = pow(blinnTerm, shininess);

    float3 finalColor = (cosAngle + blinnTerm + ambientIntensity) * diffuseColor;

    return float4(finalColor, 1);
}
