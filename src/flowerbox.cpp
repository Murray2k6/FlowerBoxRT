#include "flowerbox.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Per-step increments of the original (ssflwbox frame routine).
constexpr double kSpinXPerStep = 3.0;  // degrees
constexpr double kSpinYPerStep = 2.0;  // degrees
constexpr double kHuePerStep = 2.5 * kPi / 180.0;

// Side tables from the original: origin, U edge, V edge.
const float kCubeSides[6][9] = {
    {-0.5f, -0.5f, 0.5f, 1, 0, 0, 0, 1, 0},  {0.5f, -0.5f, -0.5f, -1, 0, 0, 0, 1, 0},
    {0.5f, 0.5f, -0.5f, -1, 0, 0, 0, 0, 1},  {-0.5f, -0.5f, -0.5f, 1, 0, 0, 0, 0, 1},
    {0.5f, -0.5f, -0.5f, 0, 1, 0, 0, 0, 1},  {-0.5f, 0.5f, -0.5f, 0, -1, 0, 0, 0, 1},
};
const float kTetraSides[4][9] = {
    {-0.5f, -0.216506f, 0.288675f, 1, 0, 0, 0, 0.866025f, -0.288675f},
    {0, -0.216506f, -0.57735f, -0.5f, 0, 0.866025f, 0.25f, 0.866025f, 0.144337f},
    {0.5f, -0.216506f, 0.288675f, -0.5f, 0, -0.866025f, -0.25f, 0.866025f, 0.144337f},
    {0.5f, -0.216506f, 0.288675f, -1, 0, 0, 0, 0, -0.866025f},
};
const float kPyramidSides[8][9] = {
    {-0.5f, 0, 0.5f, 1, 0, 0, 0, 0.5f, -0.5f},   {0.5f, 0, 0.5f, -1, 0, 0, 0, -0.5f, -0.5f},
    {0.5f, 0, 0.5f, 0, 0, -1, -0.5f, 0.5f, 0},   {0.5f, 0, -0.5f, 0, 0, 1, -0.5f, -0.5f, 0},
    {0.5f, 0, -0.5f, -1, 0, 0, 0, 0.5f, 0.5f},   {-0.5f, 0, -0.5f, 1, 0, 0, 0, -0.5f, 0.5f},
    {-0.5f, 0, -0.5f, 0, 0, 1, 0.5f, 0.5f, 0},   {-0.5f, 0, 0.5f, 0, 0, -1, 0.5f, -0.5f, 0},
};

// Bloom range, step and sphere scale per shape, from the original init routines.
struct ShapeParams {
    float minSf, maxSf, sfInc, scale;
};
const ShapeParams kShapeParams[kShapeCount] = {
    {-1.1f, 5.1f, 0.05f, 2.0f},   // cube
    {-1.1f, 5.2f, 0.05f, 3.75f},  // tetrahedron
    {-1.1f, 5.2f, 0.05f, 3.0f},   // pyramids
    {-2.5f, 8.5f, 0.05f, 2.1f},   // cylinder
    {-2.2f, 0.2f, 0.05f, 1.0f},   // spring
};

// Default colours, from the original's data section.
const float kCheckerColors[16][3] = {
    {1, 0, 0}, {0, 1, 0},       {0, 1, 0},       {0, 0, 1},       {0, 0, 1},       {1, 0, 1},
    {1, 0, 1}, {0, 1, 1},       {0, 1, 1},       {1, 1, 0},       {1, 1, 0},       {0.5f, 0.5f, 1},
    {0.5f, 0.5f, 1}, {1, 0.5f, 0.5f}, {1, 0.5f, 0.5f}, {1, 0, 0},
};
const float kSideColors[8][3] = {
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 0}, {0.5f, 0.5f, 1}, {1, 0.5f, 0.5f},
};

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(const Vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-20f) return {0, 0, 1};
    return {v.x / len, v.y / len, v.z / len};
}
Vec3 SidePoint(const float* side, float a, float b) {
    return {side[0] + side[3] * a + side[6] * b, side[1] + side[4] * a + side[7] * b,
            side[2] + side[5] * a + side[8] * b};
}

}  // namespace

void FlowerBox::Init(const Settings& s) {
    m_settings = s;
    m_base.clear();
    m_strips.clear();

    switch (s.shape) {
        case kShapeTetrahedron: MakeTriangleSides(kTetraSides, 4); break;
        case kShapePyramids: MakeTriangleSides(kPyramidSides, 8); break;
        case kShapeCylinder: MakeCylinder(); break;
        case kShapeSpring: MakeSpring(); break;
        default: MakeQuadSides(kCubeSides, 6); break;
    }

    const ShapeParams& p = kShapeParams[s.shape];
    m_minSf = p.minSf;
    m_maxSf = p.maxSf;
    m_sfInc = p.sfInc;
    m_scale = p.scale;

    // Per-point bloom factor: at sf = 0 the point sits on the shape, at
    // sf = 1 on a sphere of radius 1/scale, beyond that it folds through.
    m_vf.resize(m_base.size());
    for (size_t i = 0; i < m_base.size(); ++i) {
        const Vec3& b = m_base[i];
        float len = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z) * m_scale;
        m_vf[i] = len > 1e-20f ? (1.0f - len) / len : 0.0f;
    }
    m_pts.resize(m_base.size());
    m_nrm.resize(m_base.size());
    m_prevPts.clear();

    m_vertexCount = 0;
    for (const Strip& st : m_strips) m_vertexCount += static_cast<uint32_t>(st.idx.size() - 2) * 3;

    m_sf = 0;
    m_sfDir = 1;
    m_rotX = m_rotY = 0;
    m_hue = 0;

    switch (s.colorMode) {
        case kColorChecker:
            m_paletteCount = 16;
            for (int i = 0; i < 16; ++i)
                for (int c = 0; c < 3; ++c) m_palette[i][c] = kCheckerColors[i][c];
            break;
        case kColorPerSide:
            m_paletteCount = 8;
            for (int i = 0; i < 8; ++i)
                for (int c = 0; c < 3; ++c) m_palette[i][c] = kSideColors[i][c];
            break;
        default:
            m_paletteCount = 1;
            m_palette[0][0] = m_palette[0][1] = m_palette[0][2] = 1.0f;
            break;
    }
    if (s.cycle) CycleColors();
}

void FlowerBox::MakeQuadSides(const float (*sides)[9], int count) {
    const int n = m_settings.subdiv;
    const uint32_t perSide = static_cast<uint32_t>((n + 1) * (n + 1));
    for (int s = 0; s < count; ++s) {
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n; ++j)
                m_base.push_back(SidePoint(sides[s], static_cast<float>(i) / n, static_cast<float>(j) / n));
        const uint32_t base = s * perSide;
        for (int i = 0; i < n; ++i) {
            Strip st{s, i, {}};
            for (int j = 0; j <= n; ++j) {
                st.idx.push_back(base + i * (n + 1) + j);
                st.idx.push_back(base + (i + 1) * (n + 1) + j);
            }
            m_strips.push_back(std::move(st));
        }
    }
}

void FlowerBox::MakeTriangleSides(const float (*sides)[9], int count) {
    const int n = m_settings.subdiv;
    for (int s = 0; s < count; ++s) {
        const uint32_t base = static_cast<uint32_t>(m_base.size());
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n - i; ++j)
                m_base.push_back(SidePoint(sides[s], static_cast<float>(i) / n + static_cast<float>(j) / (2 * n),
                                           static_cast<float>(j) / n));
        uint32_t rowStart = base;
        for (int r = 0; r < n; ++r) {
            const uint32_t rowLen = static_cast<uint32_t>(n + 1 - r);
            Strip st{s, r, {}};
            st.idx.push_back(rowStart);
            for (uint32_t k = 0; k + 1 < rowLen; ++k) {
                st.idx.push_back(rowStart + rowLen + k);
                st.idx.push_back(rowStart + 1 + k);
            }
            m_strips.push_back(std::move(st));
            rowStart += rowLen;
        }
    }
}

void FlowerBox::MakeCylinder() {
    const int n = m_settings.subdiv;
    for (int i = 0; i < n; ++i) {
        double theta = 2.0 * kPi * i / n;
        float x = static_cast<float>(0.5 * std::cos(theta)), z = static_cast<float>(0.5 * std::sin(theta));
        for (int k = 0; k <= n; ++k) m_base.push_back({x, static_cast<float>(k) / n - 0.5f, z});
    }
    for (int r = 0; r < n; ++r) {
        Strip st{0, r, {}};
        for (int k = 0; k <= n; ++k) {
            st.idx.push_back(r == n - 1 ? static_cast<uint32_t>(k) : static_cast<uint32_t>((r + 1) * (n + 1) + k));
            st.idx.push_back(static_cast<uint32_t>(r * (n + 1) + k));
        }
        m_strips.push_back(std::move(st));
    }
}

void FlowerBox::MakeSpring() {
    const int n = m_settings.subdiv;
    const int rings = 4 * n + 1;
    for (int r = 0; r < rings; ++r) {
        double t = static_cast<double>(r) / (rings - 1);
        double phi = 4.0 * kPi * r / (rings - 1);
        double radius = 0.5 - 0.2 * t;
        float frame[9] = {
            static_cast<float>(radius * std::cos(phi)), static_cast<float>(t - 0.5), static_cast<float>(radius * std::sin(phi)),
            static_cast<float>(0.1 * std::cos(phi)),    0.0f,                        static_cast<float>(0.1 * std::sin(phi)),
            0.0f,                                       0.1f,                        0.0f,
        };
        for (int k = 0; k < n; ++k) {
            double psi = 2.0 * kPi * k / n;
            m_base.push_back(SidePoint(frame, static_cast<float>(std::cos(psi)), static_cast<float>(std::sin(psi))));
        }
    }
    for (int s = 0; s < n; ++s) {
        Strip st{0, s, {}};
        for (int r = 0; r < rings; ++r) {
            st.idx.push_back(static_cast<uint32_t>(r * n + s));
            st.idx.push_back(static_cast<uint32_t>(s == n - 1 ? r * n : r * n + s + 1));
        }
        m_strips.push_back(std::move(st));
    }
}

void FlowerBox::Advance(double steps) {
    if (steps <= 0) return;
    if (m_settings.spin) {
        m_rotX = std::fmod(m_rotX + kSpinXPerStep * steps, 360.0);
        m_rotY = std::fmod(m_rotY + kSpinYPerStep * steps, 360.0);
    }
    if (m_settings.bloom) {
        // The original reversed direction whenever sf left [min, max];
        // reflecting at the bounds is the continuous form of that.
        m_sf += m_sfDir * m_sfInc * steps;
        for (int guard = 0; guard < 64 && (m_sf > m_maxSf || m_sf < m_minSf); ++guard) {
            if (m_sf > m_maxSf) m_sf = 2.0 * m_maxSf - m_sf;
            else m_sf = 2.0 * m_minSf - m_sf;
            m_sfDir = -m_sfDir;
        }
    }
    if (m_settings.cycle) {
        m_hue = std::fmod(m_hue + kHuePerStep * steps, 2.0 * kPi);
        CycleColors();
    }
}

void FlowerBox::CycleColors() {
    // Evenly spaced hues at full saturation, as the original's colour cycle.
    const double step = 2.0 * kPi / m_paletteCount;
    double h = m_hue;
    for (int i = 0; i < m_paletteCount; ++i, h += step) {
        double x = h * 6.0 / (2.0 * kPi);
        int whole = static_cast<int>(x);
        float t = static_cast<float>(x - whole), q = 1.0f - t;
        float* c = m_palette[i];
        switch (whole % 6) {
            case 0: c[0] = 1; c[1] = t; c[2] = 0; break;
            case 1: c[0] = q; c[1] = 1; c[2] = 0; break;
            case 2: c[0] = 0; c[1] = 1; c[2] = t; break;
            case 3: c[0] = 0; c[1] = q; c[2] = 1; break;
            case 4: c[0] = t; c[1] = 0; c[2] = 1; break;
            default: c[0] = 1; c[1] = 0; c[2] = q; break;
        }
    }
}

int FlowerBox::ColorIndex(const Strip& strip, int v) const {
    switch (m_settings.colorMode) {
        case kColorChecker: return strip.side * 2 + ((((v + (m_settings.slanted ? 1 : 0)) / 2) + strip.row) & 1);
        case kColorPerSide: return strip.side;
        default: return 0;
    }
}

void FlowerBox::Build(Vertex* out) {
    const float sf = static_cast<float>(m_sf);
    for (size_t i = 0; i < m_base.size(); ++i) {
        float f = sf * m_vf[i] + 1.0f;
        m_pts[i] = {m_base[i].x * f, m_base[i].y * f, m_base[i].z * f};
        m_nrm[i] = {0, 0, 0};
    }

    // Vertex normals: area-weighted strip triangle normals, as the original.
    for (const Strip& st : m_strips) {
        for (size_t k = 0; k + 2 < st.idx.size(); ++k) {
            uint32_t a = st.idx[k], b = st.idx[k + 1], c = st.idx[k + 2];
            Vec3 n = Cross(Sub(m_pts[b], m_pts[a]), Sub(m_pts[c], m_pts[a]));
            if (k & 1) n = {-n.x, -n.y, -n.z};
            for (uint32_t p : {a, b, c}) {
                m_nrm[p].x += n.x;
                m_nrm[p].y += n.y;
                m_nrm[p].z += n.z;
            }
        }
    }
    for (Vec3& n : m_nrm) n = Normalize(n);
    if (m_prevPts.size() != m_pts.size()) m_prevPts = m_pts;

    Vertex* v = out;
    for (const Strip& st : m_strips) {
        for (size_t k = 0; k + 2 < st.idx.size(); ++k) {
            // Strip triangle k in front-face order, as GL_TRIANGLE_STRIP draws it.
            size_t order[3] = {k, k + 1, k + 2};
            if (k & 1) {
                order[0] = k + 1;
                order[1] = k;
            }
            const Vec3& p0 = m_pts[st.idx[order[0]]];
            const Vec3 fn = Normalize(Cross(Sub(m_pts[st.idx[order[1]]], p0), Sub(m_pts[st.idx[order[2]]], p0)));
            // Flat shading takes the colour of the strip's last (provoking) vertex.
            const int flatColor = ColorIndex(st, static_cast<int>(k + 2));
            for (size_t corner : order) {
                uint32_t p = st.idx[corner];
                const Vec3& n = m_settings.smooth ? m_nrm[p] : fn;
                const float* c = m_palette[m_settings.smooth ? ColorIndex(st, static_cast<int>(corner)) : flatColor];
                *v++ = {{m_pts[p].x, m_pts[p].y, m_pts[p].z},
                        {n.x, n.y, n.z},
                        {fn.x, fn.y, fn.z},
                        {c[0], c[1], c[2], 0.0f},
                        {m_prevPts[p].x, m_prevPts[p].y, m_prevPts[p].z}};
            }
        }
    }
    m_prevPts = m_pts;
}

void FlowerBox::ModelMatrix(float m[4][4]) const {
    // glRotate(x) then glRotate(y): M = Rx * Ry.
    const double ax = m_rotX * kPi / 180.0, ay = m_rotY * kPi / 180.0;
    const float cx = static_cast<float>(std::cos(ax)), sx = static_cast<float>(std::sin(ax));
    const float cy = static_cast<float>(std::cos(ay)), sy = static_cast<float>(std::sin(ay));
    const float r[4][4] = {
        {cy, 0, sy, 0},
        {sx * sy, cx, -sx * cy, 0},
        {-cx * sy, sx, cx * cy, 0},
        {0, 0, 0, 1},
    };
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = r[i][j];
}
