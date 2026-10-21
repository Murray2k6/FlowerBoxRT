#include "settings.h"

#include <algorithm>

namespace {

const wchar_t kRegKey[] = L"Software\\FlowerBoxRT";

struct Field {
    const wchar_t* name;
    bool Settings::*flag;
    int Settings::*number;
};

const Field kFields[] = {
    {L"Smooth", &Settings::smooth, nullptr},
    {L"Slanted", &Settings::slanted, nullptr},
    {L"Cycle", &Settings::cycle, nullptr},
    {L"Spin", &Settings::spin, nullptr},
    {L"Bloom", &Settings::bloom, nullptr},
    {L"TwoSided", &Settings::twoSided, nullptr},
    {L"Subdiv", nullptr, &Settings::subdiv},
    {L"ColorPick", nullptr, &Settings::colorMode},
    {L"ImageSize", nullptr, &Settings::imageSize},
    {L"Geom", nullptr, &Settings::shape},
    {L"Speed", nullptr, &Settings::speed},
    {L"RayTracing", &Settings::rayTracing, nullptr},
    {L"RTShadows", &Settings::rtShadows, nullptr},
    {L"RTAmbientOcclusion", &Settings::rtAmbientOcclusion, nullptr},
    {L"RTSamples", nullptr, &Settings::rtSamples},
    {L"RTBounces", nullptr, &Settings::rtBounces},
    {L"RTReflectivity", nullptr, &Settings::rtReflectivity},
    {L"Floor", &Settings::floor, nullptr},
    {L"MSAA", nullptr, &Settings::msaa},
    {L"FrameRate", nullptr, &Settings::frameRate},
};

}  // namespace

void Settings::Load() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (const Field& f : kFields) {
        DWORD value = 0, size = sizeof(value), type = 0;
        if (RegQueryValueExW(key, f.name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            Set(f.name, static_cast<int>(value));
        }
    }
    RegCloseKey(key);
    Clamp();
}

void Settings::Save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    for (const Field& f : kFields) {
        DWORD value = f.flag ? (this->*f.flag ? 1u : 0u) : static_cast<DWORD>(this->*f.number);
        RegSetValueExW(key, f.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }
    RegCloseKey(key);
}

bool Settings::Set(const wchar_t* name, int value) {
    for (const Field& f : kFields) {
        if (_wcsicmp(f.name, name) != 0) continue;
        if (f.flag)
            this->*f.flag = value != 0;
        else
            this->*f.number = value;
        return true;
    }
    return false;
}

void Settings::Clamp() {
    subdiv = std::clamp(subdiv, kSubdivMin, kSubdivMax);
    colorMode = std::clamp(colorMode, 0, 2);
    imageSize = std::clamp(imageSize, kImageSizeMin, kImageSizeMax);
    shape = std::clamp(shape, 0, kShapeCount - 1);
    speed = std::clamp(speed, kSpeedMin, kSpeedMax);
    rtSamples = std::clamp(rtSamples, kSamplesMin, kSamplesMax);
    rtBounces = std::clamp(rtBounces, kBouncesMin, kBouncesMax);
    rtReflectivity = std::clamp(rtReflectivity, kReflectivityMin, kReflectivityMax);
    if (msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) msaa = 4;
    frameRate = std::clamp(frameRate, 0, kFrameRateCount - 1);
}
