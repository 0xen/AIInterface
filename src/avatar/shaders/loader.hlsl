// The loading animation's four cubes: a full-screen triangle whose pixel
// shader intersects four analytic boxes and keeps the nearest hit.
//
// Why not cube geometry, when cube.hlsl already draws instanced cubes: this
// app draws no scene batch, so the engine attaches no depth buffer, and four
// meshes that genuinely pass through one another would resolve by draw order
// — the later instance painting over the earlier one wherever they overlap.
// One ray per pixel resolves the union exactly, for free, and the triangle
// covers the whole window the overlay wants anyway.
//
// Everything per-frame rides push constants (no storage buffer, so no
// binding and no per-slot region to write): the overlay recorder is
// re-recorded every frame, unlike the frame passes, so that is safe here.
// The cubes never rotate, so the boxes stay axis-aligned and the whole
// intersection is one slab test.

struct PushConstants {
    float2 res;    // viewport pixels
    float extent;  // cube half-size, world units
    float opacity; // whole-loader fade for the handoff to the avatar
    float4 centre; // each cube's position along the shared slide axis
    float4 level;  // each cube's greyscale level (linear; the view is sRGB)
};

#ifdef __spirv__
[[vk::push_constant]] PushConstants pc;
#else
ConstantBuffer<PushConstants> pc : register(b0, space0);
#endif

// World half-width mapped onto half the window's width, so the animation is
// sized by the window's width and the tall panel below it does not stretch it.
static const float kScale = 1.32;
// The cubes are not the whole loading screen: the caption and the progress
// bar hang below them. Lifting the cubes by a fifth of the half-width
// (~36 px) centres that whole assembly on the window instead of the cubes
// alone, which otherwise leaves the top of the window conspicuously empty.
static const float kLift = 0.20;
static const float3 kLight = float3(0.36, 0.86, 0.36);
// Looking down the diagonal: three faces of every cube are visible, which is
// what gives a static, unrotating cube its solidity.
static const float3 kEyeDir = float3(0.62, 0.46, 1.0);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    // Fullscreen triangle. Coverage is orientation-independent and cull mode
    // is NONE in the engine, so this needs no rendClip() flip; the pixel
    // shader works from SV_Position instead.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// Nearest entry point of the axis-aligned box centred at (cx, 0, 0), plus its
// face normal. Returns false when the ray misses or the box is behind.
bool hitBox(float3 o, float3 d, float cx, float e, out float t, out float3 n) {
    const float3 c = float3(cx, 0.0, 0.0);
    const float3 inv = 1.0 / d;
    const float3 t0 = (c - e - o) * inv;
    const float3 t1 = (c + e - o) * inv;
    const float3 lo = min(t0, t1);
    const float3 hi = max(t0, t1);
    t = max(max(lo.x, lo.y), lo.z);
    const float far = min(min(hi.x, hi.y), hi.z);
    // The slab whose entry is the latest is the face that was hit; it faces
    // back along the ray.
    n = normalize(step(t - 1e-5, lo) * -sign(d));
    return far >= max(t, 0.0);
}

// One ray through pixel `pix`. rgb is the shaded surface, a is 1 on a hit.
float4 sampleAt(float2 pix) {
    float2 q = float2(1.0, -1.0) * (pix - 0.5 * pc.res) / (0.5 * pc.res.x);
    // Subtracted, not added: q is the ray's origin offset, so raising the
    // origin lowers what the pixel sees.
    q.y -= kLift;
    const float3 f = -normalize(kEyeDir);
    const float3 right = normalize(cross(f, float3(0.0, 1.0, 0.0)));
    const float3 up = cross(right, f);
    // Orthographic: a static cube keeps its proportions wherever it slides,
    // so nothing about the loop depends on where in the window it is.
    const float3 o = normalize(kEyeDir) * 8.0 + (q.x * right + q.y * up) * kScale;

    float best = 1e9;
    float3 bestN = float3(0.0, 0.0, 0.0);
    float bestLevel = 0.0;
    [unroll]
    for (uint i = 0; i < 4; ++i) {
        float t;
        float3 n;
        if (hitBox(o, f, pc.centre[i], pc.extent, t, n) && t < best) {
            best = t;
            bestN = n;
            bestLevel = pc.level[i];
        }
    }
    if (best > 1e8) return float4(0.0, 0.0, 0.0, 0.0);
    const float diffuse = saturate(dot(bestN, normalize(kLight)));
    return float4(bestLevel * (0.24 + 0.76 * diffuse).xxx, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target0 {
    // 2x2 supersampling: the window is only 360 px wide and a hard-edged
    // analytic box would otherwise stair-step badly along its silhouette.
    float4 acc = 0.0;
    [unroll]
    for (int y = 0; y < 2; ++y) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            acc += sampleAt(pos.xy + float2(x - 0.5, y - 0.5) * 0.5);
        }
    }
    if (acc.a <= 0.0) discard;
    // Straight alpha: the engine's blend is SRC_ALPHA / INV_SRC_ALPHA for
    // colour and ONE / INV_SRC_ALPHA for alpha, so coverage composites the
    // silhouette over the scrim and accumulates into the window's alpha.
    // The handoff fade scales coverage rather than colour: dimming the cubes
    // towards black would sink them into the scrim instead of dissolving them,
    // and would still claim the window's alpha from the avatar underneath.
    const float a = acc.a * 0.25 * pc.opacity;
    if (a <= 0.0) discard;
    return float4(acc.rgb / acc.a, a);
}
