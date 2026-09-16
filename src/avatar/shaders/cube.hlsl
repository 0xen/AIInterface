// Avatar placeholder: one lit cube generated from SV_VertexID (no vertex
// buffer). Per-frame data rides a per-slot storage buffer at the engine's
// first user binding (40); the push constant only carries the slot, so a
// static recording of this pass stays valid.
//
// The binding/clip-space seam mirrors Renderer/assets/shaders/backend.hlsli
// (inlined here so this file compiles without an include path into the
// engine tree). The engine binds user storage buffers as RAW UAVs on D3D12
// (register u0 in the binding's space), so the frame record is read through
// a byte-address buffer on both backends. Cull mode is NONE in the engine,
// so back faces are dropped in the pixel shader instead of relying on a
// depth attachment.

struct PushConstants {
    uint slot;
    uint pad;
};

#ifdef __spirv__
[[vk::push_constant]] PushConstants pc;
[[vk::binding(40, 0)]] RWByteAddressBuffer frames;
float4 rendClip(float4 clip) { return clip; }
#else
ConstantBuffer<PushConstants> pc : register(b0, space0);
[[vk::binding(40, 0)]] RWByteAddressBuffer frames : register(u0, space40);
float4 rendClip(float4 clip) { return float4(clip.x, -clip.y, clip.z, clip.w); }
#endif

// C++ twin: CubeFrame in main.cpp (160 bytes: mvp, model, cameraPos, tint).
// The matrices are column-major float[16]; loading the four columns as ROWS
// gives the transpose, so positions transform with mul(vector, m).
struct CubeFrameT {
    float4x4 mvpT;
    float4x4 modelT;
    float4 cameraPos; // world space, w unused
    float4 tint;      // linear rgb; w is the M1.5 handoff fade (1 = fully present)
};

float4x4 loadMatrixT(uint offset) {
    return float4x4(asfloat(frames.Load4(offset + 0)), asfloat(frames.Load4(offset + 16)),
                    asfloat(frames.Load4(offset + 32)), asfloat(frames.Load4(offset + 48)));
}

CubeFrameT loadFrame(uint slot) {
    const uint base = slot * 160;
    CubeFrameT f;
    f.mvpT = loadMatrixT(base + 0);
    f.modelT = loadMatrixT(base + 64);
    f.cameraPos = asfloat(frames.Load4(base + 128));
    f.tint = asfloat(frames.Load4(base + 144));
    return f;
}

// The swapchain view is sRGB, so the hardware encodes whatever this shader
// writes. Alpha is not encoded, so anything that has to agree with alpha —
// the premultiply below — has to cross into the encoded domain by hand.
// lerp/step rather than a ternary: a vector condition in `?:` is an error
// under HLSL 2021, which is what the engine's dxc rule compiles with.
float3 linearToSrgb(float3 c) {
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

float3 srgbToLinear(float3 c) {
    const float3 lo = c / 12.92;
    const float3 hi = pow(abs(c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR0;
    nointerpolation float facing : TEXCOORD0;
    nointerpolation float alpha : TEXCOORD1;
};

static const float3 kCorners[8] = {
    float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
    float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1),
};
// Six faces, two triangles each, indices into kCorners.
static const uint kIndices[36] = {
    4, 5, 6, 4, 6, 7, // +Z
    1, 0, 3, 1, 3, 2, // -Z
    5, 1, 2, 5, 2, 6, // +X
    0, 4, 7, 0, 7, 3, // -X
    7, 6, 2, 7, 2, 3, // +Y
    0, 1, 5, 0, 5, 4, // -Y
};
static const float3 kNormals[6] = {
    float3(0, 0, 1), float3(0, 0, -1), float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0),
};
static const float3 kFaceTints[6] = {
    float3(1.00, 0.55, 0.20), float3(0.20, 0.60, 1.00), float3(0.95, 0.30, 0.45),
    float3(0.35, 0.85, 0.45), float3(0.98, 0.85, 0.30), float3(0.65, 0.45, 0.95),
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    const CubeFrameT f = loadFrame(pc.slot);
    const uint face = vertexId / 6;
    const float3 local = kCorners[kIndices[vertexId]] * 0.5;
    const float3 world = mul(float4(local, 1.0), f.modelT).xyz;
    const float3 normal = normalize(mul(kNormals[face], (float3x3)f.modelT));
    VSOutput o;
    o.position = rendClip(mul(float4(local, 1.0), f.mvpT));
    o.normal = normal;
    o.color = kFaceTints[face] * f.tint.rgb;
    o.facing = dot(normal, normalize(f.cameraPos.xyz - world));
    o.alpha = f.tint.w;
    return o;
}

float4 PSMain(VSOutput i) : SV_Target0 {
    if (i.facing < 0.0) discard; // back face: a convex cube needs no depth buffer
    const float3 lightDir = normalize(float3(0.4, 0.8, 0.6));
    const float diffuse = saturate(dot(normalize(i.normal), lightDir));
    const float3 rgb = i.color * (0.30 + 0.70 * diffuse);
    // Premultiplied, and the window's alpha is what the desktop is seen
    // through: at tint.w < 1 (the M1.5 handoff) the cube is genuinely
    // translucent rather than blended against a black that is not there. This
    // pass does not blend, it writes the cleared (0,0,0,0) swapchain directly,
    // so the premultiply has to happen here.
    //
    // And it has to happen in the ENCODED domain. The view is sRGB, so the
    // hardware stores srgb(rgb * a) — but the desktop compositor blends the
    // stored bytes, where premultiplied means srgb(rgb) * a. srgb() is concave,
    // so the naive product is far brighter than it should be and the cube
    // arrives at full strength a third of the way into the fade.
    const float3 premul = srgbToLinear(linearToSrgb(rgb) * i.alpha);
    return float4(premul, i.alpha);
}
