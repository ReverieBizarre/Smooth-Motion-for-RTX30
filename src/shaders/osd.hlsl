// ============================================================================
//  osd.hlsl - Lightweight OSD Overlay Blending Shader
//  Alpha-blends the rasterized HUD / OSD surface onto the backbuffer.
// ============================================================================

cbuffer OSDParams : register(b0)
{
    uint  DstX;
    uint  DstY;
    uint  OverlayW;
    uint  OverlayH;
    float MasterAlpha;
    uint  BackbufferW;
    uint  BackbufferH;
    uint  Pad;
};

Texture2D<float4>   t_overlay    : register(t0);
RWTexture2D<float4> u_backbuffer : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OverlayW || id.y >= OverlayH)
        return;

    uint dstPx = DstX + id.x;
    uint dstPy = DstY + id.y;

    if (dstPx >= BackbufferW || dstPy >= BackbufferH)
        return;

    float4 src = t_overlay.Load(int3(id.xy, 0));
    float srcAlpha = src.a * MasterAlpha;

    if (srcAlpha <= 0.001f)
        return;

    float4 dst = u_backbuffer[int2(dstPx, dstPy)];
    float3 blended = lerp(dst.rgb, src.rgb, srcAlpha);
    u_backbuffer[int2(dstPx, dstPy)] = float4(blended, dst.a);
}
