// BGRA to RGBA conversion compute shader
// Each thread processes one pixel

RWTexture2D<unorm float4> inputTexture : register(u0);
RWTexture2D<unorm float4> outputTexture : register(u1);

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    outputTexture.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    // Read BGRA from input
    unorm float4 bgra = inputTexture[DTid.xy];

    // Write RGBA to output (swap R and B channels)
    outputTexture[DTid.xy] = float4(bgra.b, bgra.g, bgra.r, bgra.a);
}