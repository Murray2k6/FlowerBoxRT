// 3D FlowerBox RT shaders. build.cmd compiles:
//   VSMain  vs_6_0
//   PSMain  ps_6_0                                    classic lighting, any D3D12 GPU
//   PSMain  ps_6_5 -D RAYTRACING=1                    inline ray tracing (DXR 1.1)
//   PSMain  ... -D MOTION_VECTORS=1                   both again with DLSS motion vectors

#define F_TWO_SIDED 1u
#define F_SHADOWS   2u
#define F_AO        4u
#define F_DEBUG     8u
#define F_TEMPORAL  16u  // vary ray samples per frame (DLSS accumulates them)

cbuffer Frame : register(b0) {
    row_major float4x4 viewProj;
    row_major float4x4 model;
    row_major float4x4 prevModel;
    float3 eyePos;
    float lightRadius;
    float3 lightPos;
    float reflectivity;
    float floorY;
    float floorFade;
    uint flags;
    uint samples;
    uint bounces;
    float aoRadius;
    float floorReflectivity;
    uint frameIndex;
};

// Per floating square: sub-pixel jitter and where the square is in the target.
cbuffer View : register(b1) {
    float2 jitterNdc;
    float2 viewOrigin;
    float viewSize;
    float3 viewPad;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 fnrm : FACENORMAL;
    float4 col : COLOR;
    float3 prev : PREVPOSITION;
};

struct PSIn {
    float4 svpos : SV_Position;
    float3 wpos : WORLDPOS;
    float3 nrm : NORMAL;
    nointerpolation float3 fnrm : FACENORMAL;
    float4 col : COLOR;
    float4 curClip : CURCLIP;
    float4 prevClip : PREVCLIP;
};

PSIn VSMain(VSIn v) {
    PSIn o;
    bool isFloor = v.col.w > 0.5;
    float3 wp = isFloor ? v.pos : mul(model, float4(v.pos, 1)).xyz;
    float3 prevWp = isFloor ? v.prev : mul(prevModel, float4(v.prev, 1)).xyz;
    o.wpos = wp;
    o.nrm = isFloor ? v.nrm : mul((float3x3)model, v.nrm);
    o.fnrm = isFloor ? v.fnrm : mul((float3x3)model, v.fnrm);
    o.col = v.col;
    o.curClip = mul(viewProj, float4(wp, 1));
    o.prevClip = mul(viewProj, float4(prevWp, 1));
    o.svpos = o.curClip;
    o.svpos.xy += jitterNdc * o.svpos.w;
    return o;
}

// The original's fixed-function lighting: light 0 at (2, 2, 10), white
// diffuse and specular, global ambient 0.2 times material ambient 0.2,
// material specular 0.8 with shininess 30, infinite viewer.
float3 ClassicLight(float3 P, float3 N, float3 albedo, float shadow, float ao) {
    float3 L = normalize(lightPos - P);
    float ndl = dot(N, L);
    float3 c = 0.04 * ao;
    if (ndl > 0) {
        float3 H = normalize(L + float3(0, 0, 1));
        c += shadow * (ndl * albedo * lerp(1.0, ao, 0.5) + 0.8 * pow(saturate(dot(N, H)), 30.0));
    }
    return saturate(c);
}

// The floor fades with distance from the box...
float FloorFade(float3 P) {
    return exp(-dot(P.xz, P.xz) / (floorFade * floorFade));
}

// ...and towards the edges of its floating square, so it has no hard edge.
float FloorVignette(float2 pixel) {
    float2 uv = (pixel - viewOrigin) / viewSize;
    // Centred low in the square, where the floor and the reflection are.
    return 1 - smoothstep(0.6, 0.98, length((uv - float2(0.5, 0.6)) * float2(2.0, 2.4)));
}

#ifdef RAYTRACING

struct Vtx {
    float3 pos;
    float3 nrm;
    float3 fnrm;
    float4 col;
    float3 prev;
};

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<Vtx> verts : register(t1);

static const uint kBoxInstance = 0;
// Rays start this far off the surface (the shape is about one unit across)
// so a triangle does not shadow itself.
static const float kRayOffset = 2e-3;
static const float kRayTMin = 1e-3;

// With two-sided off the box is one-sided, as with the original's back-face
// culling: a triangle only exists seen from its front. A ray looking for a
// surface (eye, reflection) accepts triangles facing it; a ray towards light
// (shadow, ambient occlusion) is blocked only by triangles whose front faces
// that light, since light arriving at a triangle's back passes through.
bool AcceptCandidate(uint instance, uint prim, float3 objectDir, bool towardsLight) {
    if (instance != kBoxInstance || (flags & F_TWO_SIDED)) return true;
    float d = dot(verts[prim * 3].fnrm, objectDir);
    return towardsLight ? d > 0 : d < 0;
}

float Visible(float3 origin, float3 dir, float tMax) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = kRayTMin;
    r.TMax = tMax;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AcceptCandidate(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateObjectRayDirection(), true))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

struct Hit {
    bool hit;
    bool isFloor;
    float3 P;
    float3 N;
    float3 Ng;
    float3 albedo;
};

Hit TraceClosest(float3 origin, float3 dir) {
    RayQuery<RAY_FLAG_NONE> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = kRayTMin;
    r.TMax = 100;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AcceptCandidate(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateObjectRayDirection(), false))
            q.CommitNonOpaqueTriangleHit();
    }
    Hit h = (Hit)0;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;
    h.hit = true;
    h.P = origin + dir * q.CommittedRayT();
    if (q.CommittedInstanceID() != kBoxInstance) {
        h.isFloor = true;
        h.N = h.Ng = float3(0, 1, 0);
        h.albedo = 0.2;
        return h;
    }
    uint prim = q.CommittedPrimitiveIndex();
    float2 bc = q.CommittedTriangleBarycentrics();
    Vtx a = verts[prim * 3], b = verts[prim * 3 + 1], c = verts[prim * 3 + 2];
    float w0 = 1 - bc.x - bc.y;
    float3x3 o2w = (float3x3)q.CommittedObjectToWorld3x4();
    h.N = normalize(mul(o2w, a.nrm * w0 + b.nrm * bc.x + c.nrm * bc.y));
    h.Ng = normalize(mul(o2w, a.fnrm));
    h.albedo = a.col.rgb * w0 + b.col.rgb * bc.x + c.col.rgb * bc.y;
    if (dot(h.Ng, dir) > 0) {  // back face, lit from its own side (two-sided lighting)
        h.N = -h.N;
        h.Ng = -h.Ng;
    }
    return h;
}

uint Hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Low-discrepancy 2D sequence, rotated per pixel.
float2 Sample2D(uint i, float2 rotation) {
    return frac(float2(0.75487766624669276, 0.56984029099805327) * (i + 1) + rotation);
}

void Basis(float3 n, out float3 t, out float3 b) {
    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

float SoftShadow(float3 origin, float3 P, uint count, float2 rotation) {
    float3 toLight = normalize(lightPos - P);
    float3 t, b;
    Basis(toLight, t, b);
    float sum = 0;
    for (uint s = 0; s < count; ++s) {
        float2 u = Sample2D(s, rotation);
        float r = sqrt(u.x) * lightRadius;
        float a = 6.2831853 * u.y;
        float3 target = lightPos + (t * cos(a) + b * sin(a)) * r;
        float3 d = target - origin;
        float len = length(d);
        sum += Visible(origin, d / len, len);
    }
    return sum / count;
}

float AmbientOcclusion(float3 origin, float3 N, float3 Ng, uint count, float2 rotation) {
    float3 t, b;
    Basis(N, t, b);
    float open = 0;
    for (uint s = 0; s < count; ++s) {
        float2 u = Sample2D(s + 7, rotation.yx);
        float r = sqrt(u.x), a = 6.2831853 * u.y;
        float3 d = normalize(t * (r * cos(a)) + b * (r * sin(a)) + N * sqrt(max(0.0, 1 - u.x)));
        if (dot(d, Ng) < 0) d = reflect(d, Ng);
        open += Visible(origin, d, aoRadius);
    }
    return open / count;
}

float Fresnel(float3 N, float3 V) {
    return 0.25 + 0.75 * pow(1 - saturate(dot(N, V)), 5);
}

float3 ShadeHit(Hit h) {
    float3 origin = h.P + h.Ng * kRayOffset;
    float vis = 1;
    if ((flags & F_SHADOWS) && dot(h.N, lightPos - h.P) > 0) {
        float3 d = lightPos - origin;
        float len = length(d);
        vis = Visible(origin, d / len, len);
    }
    float3 c = ClassicLight(h.P, h.N, h.albedo, vis, 1);
    return h.isFloor ? c * FloorFade(h.P) : c;
}

#endif  // RAYTRACING

struct PSOut {
    float4 color : SV_Target0;
#ifdef MOTION_VECTORS
    float2 motion : SV_Target1;  // previous minus current position, in UV units
#endif
};

PSOut PSMain(PSIn i) {
    bool isFloor = i.col.w > 0.5;
    float3 P = i.wpos;
    float3 Ng = normalize(i.fnrm);
    bool front = dot(Ng, eyePos - P) >= 0;
    if (!isFloor && !front && !(flags & F_TWO_SIDED)) discard;
    float3 N = normalize(i.nrm);
    if (!front) {
        N = -N;
        Ng = -Ng;
    }
    float3 albedo = i.col.rgb;

#ifdef RAYTRACING
    float3 origin = P + Ng * kRayOffset;
    uint2 px = uint2(i.svpos.xy);
    uint seed = Hash(px.x * 7919u + px.y * 104729u + ((flags & F_TEMPORAL) ? frameIndex * 2654435761u : 0u));
    float2 rotation = float2(seed & 0xFFFF, seed >> 16) / 65536.0;

    float vis = 1, ao = 1;
    if ((flags & F_SHADOWS) && dot(N, lightPos - P) > 0) vis = SoftShadow(origin, P, samples, rotation);
    if (flags & F_AO) ao = AmbientOcclusion(origin, N, Ng, samples, rotation);
    float3 color = ClassicLight(P, N, albedo, vis, ao);

    if (bounces > 0) {
        float3 V = normalize(eyePos - P);
        float3 throughput = (isFloor ? floorReflectivity : reflectivity) * Fresnel(N, V);
        float3 o = origin;
        float3 d = reflect(-V, N);
        if (dot(d, Ng) < 0) d = reflect(d, Ng);
        for (uint b = 0; b < bounces && any(throughput > 0.004); ++b) {
            Hit h = TraceClosest(o, d);
            if (!h.hit) break;
            color += throughput * ShadeHit(h);
            throughput *= (h.isFloor ? floorReflectivity : reflectivity) * Fresnel(h.N, -d);
            o = h.P + h.Ng * kRayOffset;
            d = reflect(d, h.N);
            if (dot(d, h.Ng) < 0) d = reflect(d, h.Ng);
        }
    }
    if (flags & F_DEBUG) color = float3(vis, ao, 0);  // FLOWERBOX_RT_VIEW: red = lit, green = open sky
#else
    float3 color = ClassicLight(P, N, albedo, 1, 1);
#endif

    if (isFloor) color *= FloorFade(P) * FloorVignette(i.svpos.xy);

    PSOut o;
    o.color = float4(saturate(color), 1);
#ifdef MOTION_VECTORS
    float2 cur = i.curClip.xy / i.curClip.w, prev = i.prevClip.xy / i.prevClip.w;
    o.motion = (prev - cur) * float2(0.5, -0.5);
#endif
    return o;
}
