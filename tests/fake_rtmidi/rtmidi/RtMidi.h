/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.6: a fake RtMidi for tests/test_midiports.cpp - the "system" ports are a list the test changes
 * (plug in / pull out), send() calls the callback of the input that has that port open. */
#pragma once
#include <string>
#include <vector>
#include <algorithm>

struct RtMidiError { std::string m; const std::string &getMessage() const { return m; } };
struct RtMidi { enum Api { UNSPECIFIED }; };

namespace fake {
inline std::vector<std::string> &ports() { static std::vector<std::string> p; return p; }
struct In;
inline std::vector<In *> &opened() { static std::vector<In *> v; return v; }
}

class RtMidiIn {
public:
    typedef void (*RtMidiCallback)(double, std::vector<unsigned char> *, void *);
    RtMidiIn(RtMidi::Api = RtMidi::UNSPECIFIED, const std::string & = "") {}
    ~RtMidiIn() { closePort(); }
    unsigned getPortCount() { return (unsigned)fake::ports().size(); }
    std::string getPortName(unsigned i) { if (i >= fake::ports().size()) throw RtMidiError{ "bad port" }; return fake::ports()[i]; }
    void ignoreTypes(bool, bool, bool) {}
    void setCallback(RtMidiCallback cb, void *ud) { cb_ = cb; ud_ = ud; }
    void openPort(unsigned i, const std::string & = "") { name_ = getPortName(i); open_ = true; reg().push_back(this); }
    void closePort() { if (!open_) return; open_ = false; auto &r = reg(); r.erase(std::remove(r.begin(), r.end(), this), r.end()); }
    bool isPortOpen() const { return open_; }
    /* the test: a message from the device "name" */
    static int send(const std::string &name, std::vector<unsigned char> msg)
    {
        int n = 0;
        for (auto *in : reg()) if (in->name_ == name && in->cb_) { in->cb_(0.0, &msg, in->ud_); ++n; }
        return n;
    }
    static int open_count() { return (int)reg().size(); }
private:
    static std::vector<RtMidiIn *> &reg() { static std::vector<RtMidiIn *> r; return r; }
    RtMidiCallback cb_ = nullptr;
    void *ud_ = nullptr;
    std::string name_;
    bool open_ = false;
};
