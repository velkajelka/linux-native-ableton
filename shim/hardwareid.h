// Helpers for shim/licensing.cpp. Plain C++, no Qt and no libudev, so
// tests/hardware-id.sh builds them alone.
#pragma once
#include <string>

// The source (mountinfo field after the filesystem type) of the last mount at
// mountPoint in a /proc/self/mountinfo text, with mountinfo's octal escapes
// (\040 for a space) decoded; empty when there is no such mount.
std::string mountSource(const std::string &mountinfo, const char *mountPoint);

// Where Live's account sign-in (Ableton account token) goes instead of
// /data/settings/secrets: <XDG_CONFIG_HOME or HOME/.config>/Ableton/settings/secrets.
// Empty when neither variable is usable.
std::string secretsFile(const char *home, const char *xdgConfigHome);
