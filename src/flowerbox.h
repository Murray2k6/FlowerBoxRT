#pragma once

#include <cstdint>
#include <vector>

#include "settings.h"

// One triangle-list vertex. Positions and normals are in object space; the
// face normal follows the original's OpenGL front-face winding. col.w is 0
// for the flower box and 1 for the floor. prev is the position one frame
// earlier, for DLSS motion vectors.
struct Vertex {
    float pos[3];
    float nrm[3];
    float fnrm[3];
    float col[4];
    float prev[3];
};
static_assert(sizeof(Vertex) == 64, "Vertex layout must match the shaders");

struct Vec3 {
    float x, y, z;
};

// The 3D FlowerBox model: the shapes, bloom, colouring and spin of the
// original ssflwbox screen saver, advanced in animation steps instead of
// timer ticks.
class FlowerBox {
public:
    void Init(const Settings& s);
    // Advances the animation by a (fractional) number of original steps.
    void Advance(double steps);
    // Writes the current mesh; out must hold VertexCount() vertices.
    void Build(Vertex* out);
    uint32_t VertexCount() const { return m_vertexCount; }
    // Object-to-world matrix, row-major, for column vectors.
    void ModelMatrix(float m[4][4]) const;

private:
    struct Strip {
        int side;
        int row;
        std::vector<uint32_t> idx;
    };

    void MakeQuadSides(const float (*sides)[9], int count);
    void MakeTriangleSides(const float (*sides)[9], int count);
    void MakeCylinder();
    void MakeSpring();
    void CycleColors();
    int ColorIndex(const Strip& strip, int v) const;

    Settings m_settings;
    std::vector<Vec3> m_base, m_pts, m_nrm, m_prevPts;
    std::vector<float> m_vf;
    std::vector<Strip> m_strips;
    uint32_t m_vertexCount = 0;

    float m_minSf = 0, m_maxSf = 0, m_sfInc = 0, m_scale = 1;
    double m_sf = 0, m_sfDir = 1;
    double m_rotX = 0, m_rotY = 0, m_hue = 0;
    float m_palette[16][3] = {};
    int m_paletteCount = 1;
};
