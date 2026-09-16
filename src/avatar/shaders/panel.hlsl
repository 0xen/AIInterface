// The text panel: a CPU-drawn BGRA bitmap (GdiCanvas) blitted as one quad.
// Each frame slot owns a region of the raw panel buffer at user binding 41:
// a 32-byte header {winW, winH, x, y, w, h, alphaBits, pad} then w*h BGRA
// pixels, so a static recording of this pass stays valid. The panel is
// composited with one uniform alpha (premultiplied output), which keeps GDI
// text and fills straightforward.

struct PushConstants {
    uint slot;
    uint pad;
};

#ifdef __spirv__
[[vk::push_constant]] PushConstants pc;
[[vk::binding(41, 0)]] RWByteAddressBuffer panel;
float4 rendClip(float4 clip) { return clip; }
#else
ConstantBuffer<PushConstants> pc : register(b0, space0);
[[vk::binding(41, 0)]] RWByteAddressBuffer panel : register(u0, space41);
float4 rendClip(float4 clip) { return float4(clip.x, -clip.y, clip.z, clip.w); }
#endif

static const uint kHeaderBytes = 32;

struct Header {
    uint winW, winH, x, y, w, h;
    float alpha;
};

Header loadHeader(uint base) {
    const uint4 a = panel.Load4(base);
    const uint4 b = panel.Load4(base + 16);
    Header h;
    h.winW = a.x; h.winH = a.y; h.x = a.z; h.y = a.w;
    h.w = b.x; h.h = b.y; h.alpha = asfloat(b.z);
    return h;
}

uint slotBase(uint slot) {
    // Slot regions are laid out back to back; the header of slot 0 carries
    // the region stride in `pad` (b.w) so the shader needs no extra constant.
    const uint stride = panel.Load(28);
    return slot * stride;
}

struct VSOutput {
    float4 position : SV_Position;
    nointerpolation uint base : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    const uint base = slotBase(pc.slot);
    const Header h = loadHeader(base);
    // Two triangles covering the panel rect, in Vulkan NDC (y down).
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    const float2 c = corners[vertexId];
    const float2 px = float2(h.x, h.y) + c * float2(h.w, h.h);
    const float2 ndc = px / float2(h.winW, h.winH) * 2.0 - 1.0;
    VSOutput o;
    o.position = rendClip(float4(ndc, 0.0, 1.0));
    o.base = base;
    return o;
}

float4 PSMain(VSOutput i) : SV_Target0 {
    const Header h = loadHeader(i.base);
    const int2 p = int2(i.position.xy) - int2(h.x, h.y);
    if (p.x < 0 || p.y < 0 || p.x >= (int)h.w || p.y >= (int)h.h) discard;
    const uint bgra = panel.Load(i.base + kHeaderBytes + (uint(p.y) * h.w + uint(p.x)) * 4);
    const float3 rgb = float3((bgra >> 16) & 0xFF, (bgra >> 8) & 0xFF, bgra & 0xFF) / 255.0;
    // The swapchain is sRGB: linearise the GDI colours so they land as drawn.
    const float3 lin = pow(rgb, 2.2);
    return float4(lin * h.alpha, h.alpha);
}
