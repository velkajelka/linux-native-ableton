// PipeWire devices listed in Live's audio device menus (pwdevices.cpp).
#pragma once
#include <string>

constexpr int kFirstVirtualCard = 100;

struct VirtualDevice {
    std::string node; // PipeWire node name; empty: the desktop's default devices
    std::string name;
    bool playback, capture;
    unsigned playbackChannels, captureChannels;
};

bool virtualDevicesEnabled();
bool virtualDevice(int card, VirtualDevice &device);

// PipeWire's graph cycle in frames at the graph's own rate (pwgraph.cpp),
// with the rate out when known. 0 = unknown: the graph follows the streams
// (or PipeWire did not answer), and no floor is needed.
unsigned pipewireGraphTiming(unsigned *rate);
