#pragma once

#include <windows.h>

enum ColorMode { kColorChecker = 0, kColorPerSide = 1, kColorOne = 2 };
enum ShapeKind { kShapeCube, kShapeTetrahedron, kShapePyramids, kShapeCylinder, kShapeSpring, kShapeCount };
enum FrameRateMode { kFrameRate30, kFrameRate60, kFrameRateVSync, kFrameRateCount };

constexpr int kSubdivMin = 2, kSubdivMax = 40;
constexpr int kImageSizeMin = 0, kImageSizeMax = 100;
constexpr int kSpeedMin = 10, kSpeedMax = 300;
constexpr int kSamplesMin = 1, kSamplesMax = 16;
constexpr int kBouncesMin = 0, kBouncesMax = 4;
constexpr int kReflectivityMin = 0, kReflectivityMax = 100;

struct Settings {
    // The original 3D FlowerBox options, with the original defaults.
    bool smooth = true;
    bool slanted = false;
    bool cycle = false;
    bool spin = true;
    bool bloom = true;
    bool twoSided = false;
    int subdiv = 10;
    int colorMode = kColorPerSide;
    int imageSize = 55;
    int shape = kShapeCube;

    // Speed in percent of the original pace (32 animation steps per second).
    int speed = 100;

    // Ray tracing, used only when the graphics card supports DXR 1.1.
    bool rayTracing = true;
    bool rtShadows = true;
    bool rtAmbientOcclusion = true;
    int rtSamples = 4;
    int rtBounces = 2;
    int rtReflectivity = 40;

    bool floor = false;
    int msaa = 4;
    int frameRate = kFrameRate60;

    void Load();
    void Save() const;
    void Clamp();
    // Sets a value by its registry name; returns false for an unknown name.
    bool Set(const wchar_t* name, int value);
};
