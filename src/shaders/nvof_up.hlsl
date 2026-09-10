// ============================================================================
//  nvof_up.hlsl - upsample NVOFA grid-4 flow to full resolution, edge-aware.
//
//  NVOFA outputs a motion field at a coarse 4x4 grid (1080p -> 480x270).  A
//  naive nearest-neighbour upscale would smear one vector across every 4x4
//  block (blocky "Minecraft" edges).  A plain bilinear upscale blurs the motion
//  discontinuity at object silhouettes, dragging foreground/background flow
//  into each other.
//
//  This kernel does a joint-bilateral upsample:
//    weight = bilinear(spatial) * confidence(cost) * edge_stop(luma)
//  The luma term keeps the flow discontinuity aligned with the colour edge, and
//  the cost term down-weights grid samples the hardware flagged as unreliable.
// ============================================================================

Texture2D<int2>    tFlowG : register(t0);   // R16G16_SINT,  value/32 = pixels
Texture2D<float>   tCostG : register(t1);   // R8_UINT,      0..1, lower = more confident
Texture2D<float>   tLuma  : register(t2);   // R8_UNORM,     full-res luma guide
RWTexture2D<float2> rFlow : register(u0);   // R32G32_FLOAT, full-res flow

cbuffer Params : register(b0)
{
    uint  GridW, GridH;   // coarse grid size (ow, oh)
    uint  OutW,  OutH;    // full-res size (W, H)
    float SigmaLuma;      // edge-stop sigma in luma (0..1)
    float SigmaCost;      // confidence sigma in cost (0..1)
};

[numthreads(8, 8, 1)]
void cs_nvof_up(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= OutW || dt.y >= OutH) return;

    float scaleX = (float)OutW / (float)GridW;
    float scaleY = (float)OutH / (float)GridH;

    // output pixel centre in grid coordinates
    float gx = ((float)dt.x + 0.5f) / scaleX - 0.5f;
    float gy = ((float)dt.y + 0.5f) / scaleY - 0.5f;
    int   x0 = (int)floor(gx), y0 = (int)floor(gy);
    float fx = gx - (float)x0, fy = gy - (float)y0;

    float luma = tLuma[dt.xy];

    float2 acc = float2(0, 0);
    float  wsum = 1e-6f;

    [unroll]
    for (int yy = 0; yy <= 1; ++yy)
    [unroll]
    for (int xx = 0; xx <= 1; ++xx)
    {
        int cx = clamp(x0 + xx, 0, (int)GridW - 1);
        int cy = clamp(y0 + yy, 0, (int)GridH - 1);
        int2 g = int2(cx, cy);

        float w = (xx ? fx : 1.0f - fx) * (yy ? fy : 1.0f - fy);

        // confidence: cost 0 = best match -> high weight (R8_UINT reads 0..255)
        float cost = tCostG[g] / 255.0f;
        w *= exp(-cost * cost / (2.0f * SigmaCost * SigmaCost));

        // edge stop vs the guide sampled at this grid point's representative
        // full-res centre (aligns the flow edge with the colour edge)
        int2 rep = clamp(int2((int)(((float)cx + 0.5f) * scaleX),
                              (int)(((float)cy + 0.5f) * scaleY)),
                         int2(0, 0), int2((int)OutW - 1, (int)OutH - 1));
        float gl = tLuma[rep];
        float dl = luma - gl;
        w *= exp(-dl * dl / (2.0f * SigmaLuma * SigmaLuma));

        acc += w * ((float2)tFlowG[g] / 32.0f);
        wsum += w;
    }

    rFlow[dt.xy] = acc / wsum;
}

// ---- plain bilinear upsample of the cost field (scalar confidence) ----
Texture2D<float>   tCostOnly : register(t0);   // R8_UINT, 0..1
RWTexture2D<float> rCost     : register(u0);   // R32_FLOAT, full-res

cbuffer CostParams : register(b0)
{
    uint CGridW, CGridH, COutW, COutH;
};

[numthreads(8, 8, 1)]
void cs_cost_up(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= COutW || dt.y >= COutH) return;
    float sx = (float)COutW / (float)CGridW;
    float sy = (float)COutH / (float)CGridH;
    float gx = ((float)dt.x + 0.5f) / sx - 0.5f;
    float gy = ((float)dt.y + 0.5f) / sy - 0.5f;
    int x0 = (int)floor(gx), y0 = (int)floor(gy);
    float fx = gx - (float)x0, fy = gy - (float)y0;

    float acc = 0;
    [unroll]
    for (int yy = 0; yy <= 1; ++yy)
    [unroll]
    for (int xx = 0; xx <= 1; ++xx)
    {
        int cx = clamp(x0 + xx, 0, (int)CGridW - 1);
        int cy = clamp(y0 + yy, 0, (int)CGridH - 1);
        float w = (xx ? fx : 1.0f - fx) * (yy ? fy : 1.0f - fy);
        acc += w * (tCostOnly[int2(cx, cy)] / 255.0f);
    }
    rCost[dt.xy] = acc;
}
