#include "Common.hlsl"

StructuredBuffer<Vertex> Vertex : register(t0);

[shader("closesthit")] 
void ClosestHit(inout HitInfo payload, Attributes attrib) 
{
  float3 barycentrics = float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

  uint vertId = 3 * PrimitiveIndex();

  float3 hitColor = Vertex[vertId + 0].color * barycentrics.x +
      Vertex[vertId + 1].color * barycentrics.y +
      Vertex[vertId + 2].color * barycentrics.z;

  payload.colorAndDistance = float4(hitColor, RayTCurrent());
}
