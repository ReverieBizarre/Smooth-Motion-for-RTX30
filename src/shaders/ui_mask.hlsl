// ============================================================================
//  ui_mask.hlsl - Pass-Level UI Mask Protection & Restoration Shader
//  Detects static / 2D UI (HUD, crosshair, text) and composites real frame
//  UI back onto the interpolated (VFI) frame to eliminate ghosting & jelly effects.
// ============================================================================

cbuffer UIMaskParams : register(b0)
{
    uint   Width;
    uint   Height;
    float  Sensitivity;       // Threshold for temporal delta (default ~0.08)
    float  BlendGain;         // Gain for UI mask alpha (default ~1.0)
    uint   Flags;             // bit 0: Enable UI Mask, bit 1: Heatmap Debug View, bit 2: Center Crosshair Boost
    float  CrosshairRadius;   // Radius in UV space (e.g. 0.035 for 1080p)
    float  CornerRadius;      // UV margin for corner HUDs (e.g. 0.22)
    float  Pad0;
};

Texture2D<float4>   t_currFrame : register(t0);   // Real current frame (I_N with UI)
Texture2D<float4>   t_prevFrame : register(t1);   // Real previous frame (I_{N-1} with UI)
Texture2D<float4>   t_genFrame  : register(t2);   // Synthesised/interpolated frame (I_gen)
RWTexture2D<float4> u_outFrame  : register(u0);   // Final composited output

// Helper: RGB to Rec.709 Luma
float GetLuma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    int2 pos = int2(id.xy);
    float4 currColor = t_currFrame.Load(int3(pos, 0));
    float4 prevColor = t_prevFrame.Load(int3(pos, 0));
    float4 genColor  = t_genFrame.Load(int3(pos, 0));

    // If UI Mask Protection is disabled, pass-through generated frame
    if ((Flags & 1) == 0)
    {
        u_outFrame[pos] = genColor;
        return;
    }

    float2 uv = float2(id.x + 0.5f, id.y + 0.5f) / float2(Width, Height);

    // 1. Temporal Color & Luma delta
    float3 colDiff = abs(currColor.rgb - prevColor.rgb);
    float maxDiff  = max(colDiff.r, max(colDiff.g, colDiff.b));
    float lumaCurr = GetLuma(currColor.rgb);
    float lumaPrev = GetLuma(prevColor.rgb);
    float lumaDiff = abs(lumaCurr - lumaPrev);

    // 2. High-Frequency Local Edge Gradient (Sobel-like 4-tap)
    float lumaL = GetLuma(t_currFrame.Load(int3(max(pos.x - 1, 0), pos.y, 0)).rgb);
    float lumaR = GetLuma(t_currFrame.Load(int3(min(pos.x + 1, (int)Width - 1), pos.y, 0)).rgb);
    float lumaU = GetLuma(t_currFrame.Load(int3(pos.x, max(pos.y - 1, 0), 0)).rgb);
    float lumaD = GetLuma(t_currFrame.Load(int3(pos.x, min(pos.y + 1, (int)Height - 1), 0)).rgb);
    float edgeGrad = (abs(lumaR - lumaL) + abs(lumaD - lumaU)) * 0.5f;

    // 3. Temporal Stability Score:
    // UI pixels have very small temporal change (maxDiff < Sensitivity)
    // while having sharp edges or high alpha/contrast.
    float staticScore = saturate(1.0f - (maxDiff / max(Sensitivity, 0.001f)));

    // 4. Region Priors:
    // a) Center Crosshair Boost
    float2 centerDist = (uv - float2(0.5f, 0.5f)) * float2((float)Width / (float)Height, 1.0f);
    float distSq = dot(centerDist, centerDist);
    float isCrosshairZone = ((Flags & 4) != 0 && distSq < (CrosshairRadius * CrosshairRadius)) ? 1.0f : 0.0f;

    // b) Edge & Corner HUD Prior (minimap, health, ammo)
    float isCornerZone = (uv.x < CornerRadius || uv.x > (1.0f - CornerRadius) ||
                          uv.y < CornerRadius || uv.y > (1.0f - CornerRadius)) ? 0.4f : 0.0f;

    // 5. Combined UI Mask Probability
    float uiProb = 0.0f;
    if ((Flags & 8) != 0)
    {
        // Ground-Truth Clean Pass Interception mode (RenoDX / DLSS 5 style)
        // currColor is Backbuffer (Scene + UI), prevColor is CleanScene (Scene only)
        float3 passDiff = abs(currColor.rgb - prevColor.rgb);
        float maxPassDiff = max(passDiff.r, max(passDiff.g, passDiff.b));
        float isDiff = (maxPassDiff > Sensitivity) ? 1.0f : 0.0f;
        uiProb = isDiff * saturate((maxPassDiff - Sensitivity) * 15.0f + 0.6f);
        if (isCrosshairZone > 0.0f && maxPassDiff > 0.015f)
            uiProb = 1.0f;
    }
    else
    {
        // Standard temporal difference mode (for standalone version.dll proxy)
        if (isCrosshairZone > 0.0f)
        {
            // In crosshair zone, protect all temporally stable pixels
            uiProb = staticScore;
        }
        else if (isCornerZone > 0.0f)
        {
            // In corner HUD zones (health, minimap, ammo), protect static elements & edges
            uiProb = staticScore * saturate(edgeGrad * 4.0f + 0.85f);
        }
        else
        {
            // General screen: require static invariance and edge contrast
            uiProb = staticScore * saturate(edgeGrad * 5.0f + 0.25f);
        }
    }

    float mask = saturate(uiProb * BlendGain);

    // 6. Debug Visualization Mode (Flags bit 1)
    if ((Flags & 2) != 0)
    {
        // Highlight UI mask as glowing neon red over darkened background
        float3 debugView = lerp(genColor.rgb * 0.4f, float3(1.0f, 0.1f, 0.15f), mask);
        u_outFrame[pos] = float4(debugView, 1.0f);
        return;
    }

    // 7. Reconstruction: Composite clean sharp UI from currColor over genColor
    float3 finalRgb = lerp(genColor.rgb, currColor.rgb, mask);
    u_outFrame[pos] = float4(finalRgb, currColor.a);
}
