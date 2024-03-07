#include "MainCommon.hlsli"

float4 DebugNormal(float3 normal)
{
    return float4(0.5 * normal + (0.5).xxx, 1);
}

float4 main(TVertexOut input) : SV_Target
{
    // return DebugNormal(input.Normal);
    
    float ambientIntensity = 0.1;
    float3 lightColor = float3(1, 1, 1);
    float3 lightDir = -normalize(float3(-1, -1, -10));

    float shininess = 16.0;

    float3 normal = normalize(input.Normal);

    // Do some fancy Blinn-Phong shading!
    float cosAngle = saturate(dot(normal, lightDir));
    float3 viewDir = -normalize(input.PositionVS);
    float3 halfAngle = normalize(lightDir + viewDir);

    float blinnTerm = saturate(dot(normal, halfAngle));
    blinnTerm = cosAngle != 0.0 ? blinnTerm : 0.0;
    blinnTerm = pow(blinnTerm, shininess);

    float3 finalColor = (cosAngle + blinnTerm + ambientIntensity) * input.DiffuseColor;
    
    return float4(finalColor, 1);
}
