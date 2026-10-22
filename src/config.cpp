// Reads the settings from the [TrueDrawFPS] section of TrueDrawFPS.ini, next to the plugin.
// The settings and their defaults are listed in the README.
#include "tdfps.h"

bool optCamera, optObjects, optWheels, optUI, optParticles, optProbe;
int frameLatency, replayMode;
uint32_t gateMask;
float lookahead; // 0 to 1 step, see blendT()

void loadConfig(const char* ini) {
    auto get = [&](const char* key, int def) { return GetPrivateProfileIntA("TrueDrawFPS", key, def, ini); };
    logOn = get("Log", 0) != 0;
    frameLatency = get("FrameLatency", 1);
    optCamera = get("SmoothCamera", 1) != 0;
    optObjects = get("SmoothObjects", 1) != 0;
    optWheels = get("SmoothWheels", 1) != 0;
    optUI = get("SmoothUI", 1) != 0; // UI animations every frame (ui.cpp) and the blended map camera (blend/view.cpp)
    optParticles = get("SmoothParticles", 1) != 0;
    optProbe = get("Probe", 0) != 0;
    replayMode = get("InputReplay", 0);
    char s[32];
    GetPrivateProfileStringA("TrueDrawFPS", "Lookahead", "0", s, sizeof s, ini);
    lookahead = (float)atof(s);
    lookahead = lookahead < 0 ? 0 : lookahead > 1 ? 1 : lookahead;
    GetPrivateProfileStringA("TrueDrawFPS", "UpdateGates", "7FFFFFF", s, sizeof s, ini);
    gateMask = strtoul(s, nullptr, 16);
}
