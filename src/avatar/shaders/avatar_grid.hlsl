// The 2D pixel avatar (M2.1): one triangle covering the avatar band whose
// pixel shader floors into a cell grid read out of a per-slot storage buffer.
//
// Why a buffer and not a texture: the engine's sampled images are not
// update-after-bind, so a grid uploaded as an image would need a waitIdle
// around every frame's update. User storage buffers at bindings 40+ are
// update-after-bind and HostVisible, so the CPU writes this slot's region
// after waitFrameSlot and nothing stalls. The engine binds them as RAW UAVs
// on D3D12 (register u0 in the binding's space) — a StructuredBuffer at t0
// makes CreateGraphicsPipelineState fail with 0x80070057.
//
// Nearest-neighbour is by construction: the grid is placed at an integer
// scale and an integer origin (both computed on the CPU, see
// avatar_renderer.cpp) and this shader turns a pixel into a cell with an
// integer division. There is no sampler anywhere in the path, so there is
// nothing left that could interpolate.

struct PushConstants {
    uint slot;
    uint pad;
};

#ifdef __spirv__
[[vk::push_constant]] PushConstants pc;
[[vk::binding(40, 0)]] RWByteAddressBuffer grids;
#else
ConstantBuffer<PushConstants> pc : register(b0, space0);
[[vk::binding(40, 0)]] RWByteAddressBuffer grids : register(u0, space40);
#endif

// C++ twin: the layout constants in avatar_renderer.h. Each slot is a
// 32-byte header followed by the base layer and then the overlay layer, both
// sized for the 64x64 maximum so a grid-size change never reallocates.
// Cells are packed RGBA8, one uint each, row-major at the LIVE grid width.
static const uint kMaxCells = 64 * 64;
static const uint kHeaderBytes = 32;
static const uint kLayerBytes = kMaxCells * 4;
static const uint kSlotBytes = kHeaderBytes + 2 * kLayerBytes;

// The swapchain view is sRGB, so the hardware encodes what this shader
// writes; alpha is not encoded. Cell bytes are already in the encoded
// domain — that is what an authored RGBA8 colour is — so the premultiply
// below is srgbToLinear(encoded * a): the encode half of the round trip is
// already done by the art. Getting this wrong is what
// M1.5 documented: srgb() is concave, so premultiplying in linear makes the
// avatar arrive at full strength a third of the way into the fade.
float3 srgbToLinear(float3 c) {
    const float3 lo = c / 12.92;
    const float3 hi = pow(abs(c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float4 unpackRgba8(uint v) {
    const uint4 bytes = uint4(v, v >> 8, v >> 16, v >> 24) & 0xFFu;
    return float4(bytes) / 255.0;
}

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    // Fullscreen triangle over the band's viewport. Coverage is
    // orientation-independent and cull mode is NONE in the engine, so this
    // needs no rendClip() flip; the pixel shader works from SV_Position.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint base = pc.slot * kSlotBytes;
    const uint4 h0 = grids.Load4(base);       // width, height, scale, originX
    const uint4 h1 = grids.Load4(base + 16);  // originY, fade, pad, pad
    const uint gridW = h0.x;
    const uint gridH = h0.y;
    const uint scale = max(h0.z, 1u);
    const float fade = asfloat(h1.y);

    // SV_Position is at the pixel centre, so the truncation lands on the
    // pixel's own index; the viewport starts at the band's top-left, so
    // these are band pixels already.
    const int lx = int(position.x) - asint(h0.w);
    const int ly = int(position.y) - asint(h1.x);
    if (lx < 0 || ly < 0) discard;
    const uint gx = uint(lx) / scale;
    const uint gy = uint(ly) / scale;
    if (gx >= gridW || gy >= gridH) discard;
    const uint cell = (gy * gridW + gx) * 4;

    const float4 b = unpackRgba8(grids.Load(base + kHeaderBytes + cell));
    const float4 o = unpackRgba8(grids.Load(base + kHeaderBytes + kLayerBytes + cell));
    // Overlay over base, kept premultiplied throughout so the composite and
    // the fade below are the same kind of number. Two layers rather than one
    // so an accessory never destroys the body art underneath it.
    const float3 premul = o.rgb * o.a + b.rgb * b.a * (1.0 - o.a);
    const float alpha = (o.a + b.a * (1.0 - o.a)) * fade;
    if (alpha <= 0.0) discard;  // transparent cell: the desktop shows through
    // This pass does not blend, it writes the cleared (0,0,0,0) swapchain
    // directly, and the window's alpha is what the desktop is seen through —
    // so the premultiply has to happen here, and in the encoded domain.
    return float4(srgbToLinear(premul * fade), alpha);
}
