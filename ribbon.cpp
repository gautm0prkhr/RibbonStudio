// ============================================================================
//  Ribbon 2.1 - A native Windows 11 music sketchpad & step sequencer
//  Single-file Win32 + WinMM C++17 studio. MIT licensed.
//
//  Features:
//    - 8 synthesised tracks (Kick, Snare, Hat, Clap, Bass, Keys, Pad, Lead)
//    - Per-track Volume, Pan, Resonant Filter Tone, Decay, Mute & Solo
//    - 4 switchable patterns (A/B/C/D) + Song Chain mode + Swing groove
//    - Master Stereo Reverb, Tempo-Synced Ping-Pong Delay & Soft Limiter
//    - Hybrid Note Selection:
//        * Right-click any melodic step -> Floating 12-key Piano Popover
//        * Vertical drag or Mouse Wheel on melodic steps -> Live Pitch Glide
//        * Root Key selector + Custom Note Pool Editor (12-key toggle strip)
//    - Step Accents, Paint-Drag, Undo (Ctrl+Z), Built-in Presets, Smart Groove
//    - Live Oscilloscope, Stereo Peak Meter, 16-bit 44.1 kHz Stereo WAV Export
//    - Windows 11 Dark Title Bar, Per-Monitor V2 DPI, .rib File Association
//
//  Build (MSVC x64 Native Tools):
//    rc ribbon.rc
//    cl /std:c++17 /EHsc /O2 /W3 ribbon.cpp ribbon.res user32.lib gdi32.lib winmm.lib comdlg32.lib shell32.lib /Fe:Ribbon.exe
//
//  Build (MinGW-w64):
//    windres ribbon.rc -O coff -o ribbon.res
//    g++ -std=c++17 -O2 -mwindows -municode ribbon.cpp ribbon.res -o Ribbon.exe -lwinmm -lgdi32 -luser32 -lcomdlg32 -lshell32
// ============================================================================

#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

// ---------------------------------------------------------------------------
//  Constants, Scales, and Project Data
// ---------------------------------------------------------------------------

static const int    SR         = 44100;
static const int    STEPS      = 16;
static const int    MAXCH      = 8;
static const int    MAXPAT     = 4;
static const int    MAXVOICE   = 64;
static const int    MAXUNDO    = 32;
static const int    MIN_OFF    = -24;
static const int    MAX_OFF    = 35;  // 5 full octaves (C..B) relative to track root
static const double PI         = 3.14159265358979323846;
static const double PI2        = 6.28318530717958647692;

enum Kind { KICK, SNARE, HAT, CLAP, BASS, KEYS, PAD, LEAD, NKIND };

static const wchar_t* kKindName[NKIND] = {
    L"Kick", L"Snare", L"Hat", L"Clap", L"Bass", L"Keys", L"Pad", L"Lead"
};

static const COLORREF kKindColor[NKIND] = {
    RGB(255, 122, 154), // Kick  - rose
    RGB(246, 194, 118), // Snare - amber
    RGB(118, 218, 198), // Hat   - mint
    RGB(198, 158, 255), // Clap  - violet
    RGB(122, 178, 255), // Bass  - sky blue
    RGB(244, 224, 196), // Keys  - warm cream
    RGB(178, 146, 255), // Pad   - lavender
    RGB(255, 150, 206)  // Lead  - pink
};

static const wchar_t* kNoteNames[12] = {
    L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
};

static inline bool isDrum(int kind) { return kind <= CLAP; }

enum ScaleType { SC_MINOR, SC_MAJOR, SC_PENTA, SC_PHRYG, SC_FREE, SC_CUSTOM, NSCALE };
static const wchar_t* kScaleName[NSCALE] = {
    L"Minor", L"Major", L"Penta", L"Phryg", L"Free", L"Custom"
};

static const int kScaleIntervals[5][12] = {
    { 0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1 }, // Natural Minor
    { 0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1 }, // Major
    { 0, 3, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1 }, // Minor Pentatonic
    { 0, 1, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1 }, // Phrygian
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }      // Free (Chromatic)
};

static int scaleLength(int sc) {
    if (sc < 0 || sc >= 5) return 12;
    int n = 0;
    while (n < 12 && kScaleIntervals[sc][n] >= 0) ++n;
    return n;
}

static unsigned buildScaleMask(int sc, int rootKey) {
    if (sc < 0 || sc >= 5) return 0xFFFu;
    unsigned mask = 0;
    int len = scaleLength(sc);
    int rk = ((rootKey % 12) + 12) % 12;
    for (int i = 0; i < len; ++i) {
        int pc = (kScaleIntervals[sc][i] + rk) % 12;
        mask |= (1u << pc);
    }
    return mask ? mask : 0xFFFu;
}

static int countMaskBits(unsigned m) {
    int n = 0;
    for (int i = 0; i < 12; ++i)
        if (m & (1u << i)) ++n;
    return n;
}

static int channelRootMidi(int kind) {
    if (kind == BASS) return 36; // C2
    if (kind == PAD)  return 48; // C3
    if (kind == KEYS) return 60; // C4
    return 60;                   // Lead C4
}

static int channelBaseOct(int kind) {
    return (channelRootMidi(kind) / 12) - 1;
}

static void midiNoteName(int midi, wchar_t* out, size_t n) {
    int note = ((midi % 12) + 12) % 12;
    int oct = (midi / 12) - 1;
    swprintf(out, n, L"%s%d", kNoteNames[note], oct);
}

struct PatternTrack {
    unsigned    steps;       // bit i = active
    unsigned    accents;     // bit i = accented velocity
    signed char off[STEPS];  // semitone offset from channel root (C)
};

struct Pattern {
    PatternTrack tr[MAXCH];
};

struct ChannelMix {
    int   kind;
    bool  on;      // audible (not muted)
    bool  solo;    // soloed
    float vol;     // 0..1
    float pan;     // -1..1
    float tone;    // 0..1 (filter cutoff)
    float decay;   // 0..1 (envelope decay multiplier)
};

struct Project {
    int        tempo;      // 40..240 BPM
    float      swing;      // 0..0.60
    int        scale;      // ScaleType
    int        rootKey;    // 0..11 (C..B)
    unsigned   customMask; // 12-bit allowed pitch classes for SC_CUSTOM
    float      master;     // 0..1
    float      reverbMix;  // 0..1
    float      delayMix;   // 0..1
    int        curPat;     // 0..3 (A..D)
    bool       songMode;   // chain A->B->C->D
    ChannelMix ch[MAXCH];
    Pattern    pat[MAXPAT];
};

static unsigned effectiveNoteMask(const Project& p) {
    if (p.scale == SC_CUSTOM) {
        unsigned m = p.customMask & 0xFFFu;
        return m ? m : 1u;
    }
    return buildScaleMask(p.scale, p.rootKey);
}

static bool inActiveScale(const Project& p, int off) {
    int pc = ((off % 12) + 12) % 12;
    return (effectiveNoteMask(p) & (1u << pc)) != 0;
}

static int stepNoteOffset(const Project& p, int cur, int dir, bool chromatic) {
    if (chromatic || p.scale == SC_FREE) {
        return std::clamp(cur + (dir > 0 ? 1 : -1), MIN_OFF, MAX_OFF);
    }
    int next = cur;
    for (int tries = 0; tries < 24; ++tries) {
        next += (dir > 0 ? 1 : -1);
        if (next > MAX_OFF) return MAX_OFF;
        if (next < MIN_OFF) return MIN_OFF;
        if (inActiveScale(p, next)) return next;
    }
    return std::clamp(cur + (dir > 0 ? 1 : -1), MIN_OFF, MAX_OFF);
}

static Project              g_proj;
static Pattern              g_clipboard;
static bool                 g_hasClipboard = false;
static std::vector<Project> g_undoStack;

static int  g_sel          = 0;
static bool g_playing      = false;
static int  g_presetIdx    = 0;

static wchar_t g_toastText[128] = L"";
static DWORD   g_toastUntil     = 0;

static void setToast(const wchar_t* msg, DWORD ms = 2400) {
    wcsncpy(g_toastText, msg, 127);
    g_toastText[127] = 0;
    g_toastUntil = GetTickCount() + ms;
}

static bool isChannelAudible(const Project& p, int c) {
    bool anySolo = false;
    for (int i = 0; i < MAXCH; ++i) {
        if (p.ch[i].solo) { anySolo = true; break; }
    }
    if (anySolo) return p.ch[c].solo;
    return p.ch[c].on;
}

// ---------------------------------------------------------------------------
//  Presets & Groove Generator
// ---------------------------------------------------------------------------

static const wchar_t* kPresetNames[] = {
    L"Lo-Fi Chill", L"Trap Bounce", L"Synthwave", L"House Groove", L"Blank Canvas"
};
static const int kNumPresets = 5;

static void resetMixDefaults(Project& p) {
    p.tempo      = 92;
    p.swing      = 0.14f;
    p.scale      = SC_MINOR;
    p.rootKey    = 0; // C
    p.customMask = buildScaleMask(SC_MINOR, 0);
    p.master     = 0.84f;
    p.reverbMix  = 0.24f;
    p.delayMix   = 0.18f;
    p.curPat     = 0;
    p.songMode   = false;
    for (int i = 0; i < MAXCH; ++i) {
        p.ch[i].kind  = i;
        p.ch[i].on    = true;
        p.ch[i].solo  = false;
        p.ch[i].pan   = 0.0f;
        p.ch[i].tone  = 0.72f;
        p.ch[i].decay = 0.50f;
        switch (i) {
        case KICK:  p.ch[i].vol = 0.88f; p.ch[i].tone = 0.65f; p.ch[i].decay = 0.52f; break;
        case SNARE: p.ch[i].vol = 0.78f; p.ch[i].tone = 0.75f; p.ch[i].decay = 0.45f; break;
        case HAT:   p.ch[i].vol = 0.58f; p.ch[i].pan = -0.18f; p.ch[i].tone = 0.82f; p.ch[i].decay = 0.35f; break;
        case CLAP:  p.ch[i].vol = 0.66f; p.ch[i].pan =  0.14f; p.ch[i].tone = 0.76f; p.ch[i].decay = 0.48f; break;
        case BASS:  p.ch[i].vol = 0.80f; p.ch[i].tone = 0.55f; p.ch[i].decay = 0.55f; break;
        case KEYS:  p.ch[i].vol = 0.68f; p.ch[i].pan = -0.22f; p.ch[i].tone = 0.70f; p.ch[i].decay = 0.58f; break;
        case PAD:   p.ch[i].vol = 0.46f; p.ch[i].pan =  0.18f; p.ch[i].tone = 0.52f; p.ch[i].decay = 0.78f; break;
        case LEAD:  p.ch[i].vol = 0.60f; p.ch[i].pan =  0.08f; p.ch[i].tone = 0.78f; p.ch[i].decay = 0.46f; break;
        }
    }
    memset(p.pat, 0, sizeof(p.pat));
}

static void loadPreset(Project& p, int idx) {
    resetMixDefaults(p);
    auto setS = [&](int pat, int ch, int step, int off = 0, bool acc = false) {
        p.pat[pat].tr[ch].steps |= (1u << step);
        if (acc) p.pat[pat].tr[ch].accents |= (1u << step);
        p.pat[pat].tr[ch].off[step] = (signed char)off;
    };

    if (idx == 0) {
        // Lo-Fi Chill (90 BPM, C Minor)
        p.tempo = 90; p.swing = 0.22f; p.scale = SC_MINOR; p.rootKey = 0;
        p.reverbMix = 0.30f; p.delayMix = 0.22f;
        int kA[] = {0, 6, 10}; for (int s : kA) setS(0, KICK, s, 0, s == 0);
        setS(0, SNARE, 4, 0, true); setS(0, SNARE, 12, 0, true);
        setS(0, CLAP, 12);
        for (int s = 0; s < 16; s += 2) setS(0, HAT, s, 0, (s % 4) == 0);
        setS(0, BASS, 0, 0, true); setS(0, BASS, 6, 0); setS(0, BASS, 8, 3); setS(0, BASS, 12, -2);
        setS(0, KEYS, 2, 7); setS(0, KEYS, 7, 10); setS(0, KEYS, 10, 12, true); setS(0, KEYS, 14, 7);
        setS(0, PAD, 0, 0); setS(0, PAD, 8, 3);
        p.pat[1] = p.pat[0];
        setS(1, KICK, 14);
        setS(1, LEAD, 0, 12, true); setS(1, LEAD, 3, 10); setS(1, LEAD, 6, 7); setS(1, LEAD, 10, 15, true); setS(1, LEAD, 14, 12);
    } else if (idx == 1) {
        // Trap Bounce (138 BPM, C Phrygian)
        p.tempo = 138; p.swing = 0.08f; p.scale = SC_PHRYG; p.rootKey = 0;
        p.reverbMix = 0.24f; p.delayMix = 0.20f;
        p.ch[BASS].decay = 0.78f; p.ch[BASS].tone = 0.45f;
        int kA[] = {0, 3, 10, 14}; for (int s : kA) setS(0, KICK, s, 0, s == 0);
        setS(0, SNARE, 8, 0, true);
        setS(0, CLAP, 8, 0, true);
        for (int s = 0; s < 16; ++s) {
            if (s % 2 == 0 || s == 5 || s == 13 || s == 15) setS(0, HAT, s, 0, s == 0 || s == 8);
        }
        setS(0, BASS, 0, 0, true); setS(0, BASS, 3, 0); setS(0, BASS, 10, 1, true); setS(0, BASS, 14, 7);
        setS(0, KEYS, 0, 12); setS(0, KEYS, 6, 13); setS(0, KEYS, 12, 7);
        setS(0, PAD, 0, 0);
        p.pat[1] = p.pat[0];
        setS(1, LEAD, 2, 12); setS(1, LEAD, 4, 13, true); setS(1, LEAD, 7, 15); setS(1, LEAD, 11, 13); setS(1, LEAD, 14, 12);
    } else if (idx == 2) {
        // Synthwave (112 BPM, C Minor)
        p.tempo = 112; p.swing = 0.0f; p.scale = SC_MINOR; p.rootKey = 0;
        p.reverbMix = 0.38f; p.delayMix = 0.30f;
        p.ch[LEAD].tone = 0.85f; p.ch[BASS].tone = 0.62f;
        for (int s = 0; s < 16; s += 4) setS(0, KICK, s, 0, true);
        setS(0, SNARE, 4, 0, true); setS(0, SNARE, 12, 0, true);
        for (int s = 2; s < 16; s += 4) setS(0, HAT, s, 0, true);
        int bNotes[8] = {0, 0, 0, 0, -4, -4, -2, -2};
        for (int i = 0; i < 8; ++i) setS(0, BASS, i * 2, bNotes[i], (i % 2) == 0);
        setS(0, PAD, 0, 0); setS(0, PAD, 8, -4);
        setS(0, LEAD, 0, 12, true); setS(0, LEAD, 3, 15); setS(0, LEAD, 6, 14); setS(0, LEAD, 8, 10, true); setS(0, LEAD, 12, 12);
        p.pat[1] = p.pat[0];
        setS(1, KEYS, 1, 19); setS(1, KEYS, 5, 15); setS(1, KEYS, 9, 17); setS(1, KEYS, 13, 14);
    } else if (idx == 3) {
        // House Groove (124 BPM, C Minor Penta)
        p.tempo = 124; p.swing = 0.16f; p.scale = SC_PENTA; p.rootKey = 0;
        p.reverbMix = 0.22f; p.delayMix = 0.24f;
        for (int s = 0; s < 16; s += 4) setS(0, KICK, s, 0, true);
        setS(0, CLAP, 4, 0, true); setS(0, CLAP, 12, 0, true);
        for (int s = 2; s < 16; s += 4) setS(0, HAT, s, 0, true);
        setS(0, HAT, 7); setS(0, HAT, 15);
        setS(0, BASS, 0, 0, true); setS(0, BASS, 3, 3); setS(0, BASS, 6, 5); setS(0, BASS, 10, 7, true); setS(0, BASS, 14, 3);
        setS(0, KEYS, 2, 12); setS(0, KEYS, 6, 15); setS(0, KEYS, 11, 10); setS(0, KEYS, 14, 12);
        p.pat[1] = p.pat[0];
        setS(1, SNARE, 14); setS(1, SNARE, 15, 0, true);
    }
    p.customMask = buildScaleMask(p.scale, p.rootKey);
}

// ---------------------------------------------------------------------------
//  DSP Engine (Self-contained so live audio & WAV export never collide)
// ---------------------------------------------------------------------------

struct Voice {
    bool   active;
    int    kind;
    float  pan;
    double phase;
    double phase2;
    double phase3;
    double inc;
    double env;
    double decayRate;
    double amp;
    double age;
    double cutoffHz;
    double res;
    double svfLow;
    double svfBand;
    double hpState;
    bool   accent;
};

struct StereoDelay {
    static const int MAX_BUF = SR * 2;
    std::vector<float> bufL;
    std::vector<float> bufR;
    int    writePos;
    double dampL;
    double dampR;

    StereoDelay() : bufL(MAX_BUF, 0.0f), bufR(MAX_BUF, 0.0f), writePos(0), dampL(0.0), dampR(0.0) {}

    void clear() {
        std::fill(bufL.begin(), bufL.end(), 0.0f);
        std::fill(bufR.begin(), bufR.end(), 0.0f);
        writePos = 0; dampL = dampR = 0.0;
    }

    void process(double inL, double inR, int delaySamples, double mix, double& outL, double& outR) {
        int dL = std::clamp(delaySamples, 64, MAX_BUF - 2);
        int dR = std::clamp((delaySamples * 3) / 4, 64, MAX_BUF - 2);
        int rL = (writePos - dL + MAX_BUF) % MAX_BUF;
        int rR = (writePos - dR + MAX_BUF) % MAX_BUF;

        double tapL = bufL[rL];
        double tapR = bufR[rR];

        dampL += (tapL - dampL) * 0.35;
        dampR += (tapR - dampR) * 0.35;

        bufL[writePos] = (float)std::clamp(inL + dampR * 0.42, -2.0, 2.0);
        bufR[writePos] = (float)std::clamp(inR + dampL * 0.42, -2.0, 2.0);
        writePos = (writePos + 1) % MAX_BUF;

        outL = inL + tapL * mix * 0.72;
        outR = inR + tapR * mix * 0.72;
    }
};

struct StereoReverb {
    static const int NCOMB = 4;
    static const int NALLP = 2;
    struct Comb {
        std::vector<float> buf;
        int    idx = 0;
        double damp = 0.0;
        double fb = 0.78;
        void init(int len, double feedback) { buf.assign(len, 0.0f); idx = 0; damp = 0.0; fb = feedback; }
        double tick(double x) {
            double y = buf[idx];
            damp = y * 0.74 + damp * 0.26;
            buf[idx] = (float)std::clamp(x + damp * fb, -4.0, 4.0);
            if (++idx >= (int)buf.size()) idx = 0;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        int idx = 0;
        void init(int len) { buf.assign(len, 0.0f); idx = 0; }
        double tick(double x) {
            double b = buf[idx];
            double y = -x + b;
            buf[idx] = (float)std::clamp(x + b * 0.5, -4.0, 4.0);
            if (++idx >= (int)buf.size()) idx = 0;
            return y;
        }
    };

    Comb    cL[NCOMB], cR[NCOMB];
    Allpass aL[NALLP], aR[NALLP];

    StereoReverb() {
        const int clen[NCOMB] = { 1557, 1617, 1491, 1422 };
        const int alen[NALLP] = { 225, 556 };
        for (int i = 0; i < NCOMB; ++i) {
            cL[i].init(clen[i], 0.80);
            cR[i].init(clen[i] + 29, 0.80);
        }
        for (int i = 0; i < NALLP; ++i) {
            aL[i].init(alen[i]);
            aR[i].init(alen[i] + 23);
        }
    }

    void clear() {
        for (int i = 0; i < NCOMB; ++i) {
            std::fill(cL[i].buf.begin(), cL[i].buf.end(), 0.0f); cL[i].damp = 0.0;
            std::fill(cR[i].buf.begin(), cR[i].buf.end(), 0.0f); cR[i].damp = 0.0;
        }
        for (int i = 0; i < NALLP; ++i) {
            std::fill(aL[i].buf.begin(), aL[i].buf.end(), 0.0f);
            std::fill(aR[i].buf.begin(), aR[i].buf.end(), 0.0f);
        }
    }

    void process(double inL, double inR, double mix, double& outL, double& outR) {
        double mono = (inL + inR) * 0.18;
        double sL = 0.0, sR = 0.0;
        for (int i = 0; i < NCOMB; ++i) {
            sL += cL[i].tick(mono);
            sR += cR[i].tick(mono);
        }
        for (int i = 0; i < NALLP; ++i) {
            sL = aL[i].tick(sL);
            sR = aR[i].tick(sR);
        }
        outL = inL + sL * mix * 0.52;
        outR = inR + sR * mix * 0.52;
    }
};

struct SynthEngine {
    Voice        voices[MAXVOICE];
    StereoDelay  delay;
    StereoReverb reverb;
    unsigned     noiseState;

    float        scopeBuf[256];
    int          scopePos;
    float        meterL;
    float        meterR;
    float        chPulse[MAXCH];

    SynthEngine() : noiseState(2463534242u), scopePos(0), meterL(0.0f), meterR(0.0f) {
        clearVoices();
        memset(scopeBuf, 0, sizeof(scopeBuf));
        memset(chPulse, 0, sizeof(chPulse));
    }

    void clearVoices() {
        for (int i = 0; i < MAXVOICE; ++i) voices[i].active = false;
    }

    void resetAll() {
        clearVoices();
        delay.clear();
        reverb.clear();
        meterL = meterR = 0.0f;
        memset(chPulse, 0, sizeof(chPulse));
    }

    inline double noise() {
        unsigned x = noiseState;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        noiseState = x ? x : 1u;
        return (double)noiseState / 2147483648.0 - 1.0;
    }

    void spawn(int kind, double midiNote, double amp, float pan, float tone, float decayKnob, bool accent) {
        int slot = -1;
        double minEnv = 1e9;
        int steal = 0;
        for (int i = 0; i < MAXVOICE; ++i) {
            if (!voices[i].active) { slot = i; break; }
            if (voices[i].env < minEnv) { minEnv = voices[i].env; steal = i; }
        }
        if (slot < 0) slot = steal;

        Voice& v = voices[slot];
        memset(&v, 0, sizeof(Voice));
        v.active = true;
        v.kind   = kind;
        v.pan    = std::clamp(pan, -1.0f, 1.0f);
        v.accent = accent;
        v.amp    = amp * (accent ? 1.32 : 1.0);
        v.env    = 1.0;

        const double f0 = 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
        const double dMul = 2.2 - 1.75 * std::clamp((double)decayKnob, 0.0, 1.0);
        v.cutoffHz = 220.0 * std::pow(55.0, std::clamp((double)tone, 0.0, 1.0));
        v.res = 0.55;

        switch (kind) {
        case KICK:  v.inc = 155.0;  v.decayRate = 5.8 * dMul;  v.res = 0.70; break;
        case SNARE: v.inc = 195.0;  v.decayRate = 11.5 * dMul; v.res = 0.70; break;
        case HAT:   v.inc = 3800.0; v.decayRate = 34.0 * dMul; v.res = 0.80; break;
        case CLAP:  v.inc = 950.0;  v.decayRate = 13.5 * dMul; v.res = 0.65; break;
        case BASS:  v.inc = f0;     v.decayRate = 2.4 * dMul;  v.res = 0.48; break;
        case KEYS:  v.inc = f0;     v.decayRate = 1.9 * dMul;  v.res = 0.65; break;
        case PAD:   v.inc = f0;     v.decayRate = 0.75 * dMul; v.res = 0.68; break;
        case LEAD:  v.inc = f0;     v.decayRate = 2.8 * dMul;  v.res = 0.42; break;
        }
        if (kind < MAXCH) chPulse[kind] = 1.0f;
    }

    double renderVoice(Voice& v) {
        if (!v.active) return 0.0;
        const double t = v.age;
        double out = 0.0;

        switch (v.kind) {
        case KICK: {
            v.inc = 44.0 + 130.0 * std::exp(-t * 36.0);
            double sub = std::sin(v.phase);
            double click = (t < 0.004) ? (1.0 - t / 0.004) * 0.45 * noise() : 0.0;
            out = std::tanh(sub * 1.65) * 0.95 + click;
            break;
        }
        case SNARE: {
            double n = noise();
            double hp = n - v.hpState;
            v.hpState += hp * 0.25;
            double bodyEnv = std::exp(-t * 28.0);
            double body = std::sin(v.phase) * bodyEnv * 0.58;
            out = hp * 0.68 + body;
            break;
        }
        case HAT: {
            double n = noise();
            double hp = n - v.hpState;
            v.hpState += hp * 0.14;
            double metal = ((v.phase < PI) ? 1.0 : -1.0) * 0.18
                         + ((v.phase2 < PI) ? 1.0 : -1.0) * 0.14
                         + ((v.phase3 < PI) ? 1.0 : -1.0) * 0.12;
            out = (hp * 0.72 + metal * 0.45) * 0.85;
            break;
        }
        case CLAP: {
            double n = noise();
            double hp = n - v.hpState;
            v.hpState += hp * 0.28;
            double burst = 0.22;
            if (t < 0.011) burst = 1.0;
            else if (t > 0.022 && t < 0.033) burst = 0.92;
            else if (t > 0.044 && t < 0.056) burst = 0.85;
            out = hp * burst * 0.95;
            break;
        }
        case BASS: {
            double norm = v.phase / PI2;
            double tri = (norm < 0.5) ? (4.0 * norm - 1.0) : (3.0 - 4.0 * norm);
            double saw = 2.0 * (v.phase2 / PI2) - 1.0;
            double sub = std::sin(v.phase);
            out = sub * 0.55 + tri * 0.30 + saw * 0.24;
            break;
        }
        case KEYS: {
            double tine = std::exp(-t * 14.0) * 0.65;
            double mod = std::sin(v.phase * 4.0) * tine;
            out = std::sin(v.phase + mod) * 0.62
                + std::sin(v.phase2) * 0.24
                + std::sin(v.phase * 2.0) * 0.12;
            break;
        }
        case PAD: {
            double atk = std::min(1.0, t / 0.08);
            double s1 = std::sin(v.phase);
            double s2 = std::sin(v.phase2);
            double s3 = std::sin(v.phase3);
            out = (s1 * 0.46 + s2 * 0.38 + s3 * 0.26) * atk;
            break;
        }
        case LEAD: {
            double sq  = (v.phase < PI) ? 0.55 : -0.55;
            double saw = (2.0 * (v.phase2 / PI2) - 1.0) * 0.45;
            out = sq + saw;
            break;
        }
        }

        double envMod = isDrum(v.kind) ? 1.0 : (0.35 + 0.65 * v.env);
        double fc = std::clamp(v.cutoffHz * envMod * (v.accent ? 1.25 : 1.0), 70.0, 14500.0);
        double f = 2.0 * std::sin(PI * fc / (double)SR);
        double q = std::clamp(v.res, 0.35, 1.0);
        double hpF = out - v.svfLow - q * v.svfBand;
        v.svfBand += f * hpF;
        v.svfLow  += f * v.svfBand;
        double filtered = (v.kind == HAT) ? (out * 0.65 + hpF * 0.35) : v.svfLow;

        double inc1 = v.inc;
        double inc2 = (v.kind == HAT) ? v.inc * 1.37 : v.inc * 1.006;
        double inc3 = (v.kind == PAD) ? v.inc * 1.4983 : v.inc * 1.73;

        v.phase  += PI2 * inc1 / (double)SR; if (v.phase  >= PI2) v.phase  -= PI2 * std::floor(v.phase  / PI2);
        v.phase2 += PI2 * inc2 / (double)SR; if (v.phase2 >= PI2) v.phase2 -= PI2 * std::floor(v.phase2 / PI2);
        v.phase3 += PI2 * inc3 / (double)SR; if (v.phase3 >= PI2) v.phase3 -= PI2 * std::floor(v.phase3 / PI2);

        v.age += 1.0 / (double)SR;
        v.env -= v.decayRate / (double)SR;
        if (v.env <= 0.0) {
            v.active = false;
            return 0.0;
        }

        double gain = v.env * v.env * v.amp;
        return filtered * gain;
    }

    void triggerStep(const Project& p, int patIdx, int step) {
        const Pattern& pat = p.pat[patIdx];
        for (int c = 0; c < MAXCH; ++c) {
            if (!isChannelAudible(p, c)) continue;
            if (!(pat.tr[c].steps & (1u << step))) continue;
            int kind = p.ch[c].kind;
            double midi = isDrum(kind) ? 60.0 : (double)(channelRootMidi(kind) + pat.tr[c].off[step]);
            bool acc = (pat.tr[c].accents & (1u << step)) != 0;
            spawn(kind, midi, 0.56 * p.ch[c].vol, p.ch[c].pan, p.ch[c].tone, p.ch[c].decay, acc);
        }
    }

    void renderFrame(const Project& p, double& outL, double& outR, bool updateTelemetry) {
        double dryL = 0.0, dryR = 0.0;
        double fxSendL = 0.0, fxSendR = 0.0;

        for (int i = 0; i < MAXVOICE; ++i) {
            if (!voices[i].active) continue;
            int k = voices[i].kind;
            double s = renderVoice(voices[i]);
            if (s == 0.0) continue;
            double pan = voices[i].pan * 0.5 + 0.5;
            double gL = std::cos(pan * PI * 0.5);
            double gR = std::sin(pan * PI * 0.5);
            dryL += s * gL;
            dryR += s * gR;
            double sendAmt = (k == KICK) ? 0.04 : (k == BASS) ? 0.12 : (k == HAT) ? 0.25 : 0.72;
            fxSendL += s * gL * sendAmt;
            fxSendR += s * gR * sendAmt;
        }

        double wetL = dryL, wetR = dryR;
        if (p.delayMix > 0.005f) {
            int spb = (int)(60.0 / (double)std::max(40, p.tempo) / 4.0 * SR);
            double dOutL = 0.0, dOutR = 0.0;
            delay.process(fxSendL, fxSendR, spb * 3, p.delayMix, dOutL, dOutR);
            wetL += (dOutL - fxSendL);
            wetR += (dOutR - fxSendR);
        }
        if (p.reverbMix > 0.005f) {
            double rOutL = 0.0, rOutR = 0.0;
            reverb.process(fxSendL, fxSendR, p.reverbMix, rOutL, rOutR);
            wetL += (rOutL - fxSendL);
            wetR += (rOutR - fxSendR);
        }

        outL = std::tanh(wetL * 1.15) * 0.94 * p.master;
        outR = std::tanh(wetR * 1.15) * 0.94 * p.master;

        if (updateTelemetry) {
            float aL = (float)std::fabs(outL);
            float aR = (float)std::fabs(outR);
            meterL = (aL > meterL) ? aL : meterL * 0.9994f;
            meterR = (aR > meterR) ? aR : meterR * 0.9994f;
            scopeBuf[scopePos] = (float)((outL + outR) * 0.5);
            scopePos = (scopePos + 1) & 255;
        }
    }
};

static int calcStepSamples(int tempo, float swing, int stepIndex) {
    double base = 60.0 / (double)std::clamp(tempo, 40, 240) / 4.0 * (double)SR;
    double sw = std::clamp((double)swing, 0.0, 0.60);
    double factor = ((stepIndex % 2) == 0) ? (1.0 + sw * 0.45) : (1.0 - sw * 0.45);
    return std::max(256, (int)(base * factor));
}

// ---------------------------------------------------------------------------
//  Real-time WinMM Audio Engine (Event-driven worker thread)
// ---------------------------------------------------------------------------

static const int NUM_BUFS    = 3;
static const int BUF_SAMPLES = 1024;

static CRITICAL_SECTION g_cs;
static SynthEngine      g_liveEngine;
static HWAVEOUT         g_wave        = NULL;
static HANDLE           g_audioEvent  = NULL;
static HANDLE           g_audioThread = NULL;
static volatile bool    g_audioRun    = false;
static WAVEHDR          g_hdr[NUM_BUFS];
static short            g_pcm[NUM_BUFS][BUF_SAMPLES * 2];

static int              g_smpLeft     = 0;
static int              g_stepCursor  = 0;
static int              g_playPat     = 0;
static volatile LONG    g_displayStep = -1;
static volatile LONG    g_displayPat  = 0;

static void fillAudioBuffer(short* dst, int frames) {
    int pos = 0;
    while (pos < frames) {
        int chunk = std::min(256, frames - pos);
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < chunk; ++i) {
            if (g_playing) {
                if (--g_smpLeft <= 0) {
                    int patIdx = g_proj.songMode ? g_playPat : g_proj.curPat;
                    g_liveEngine.triggerStep(g_proj, patIdx, g_stepCursor);
                    InterlockedExchange(&g_displayStep, g_stepCursor);
                    InterlockedExchange(&g_displayPat, patIdx);
                    g_smpLeft = calcStepSamples(g_proj.tempo, g_proj.swing, g_stepCursor);
                    g_stepCursor = (g_stepCursor + 1) % STEPS;
                    if (g_stepCursor == 0 && g_proj.songMode) {
                        g_playPat = (g_playPat + 1) % MAXPAT;
                    }
                }
            }
            double l = 0.0, r = 0.0;
            g_liveEngine.renderFrame(g_proj, l, r, true);
            dst[(pos + i) * 2]     = (short)(std::clamp(l, -1.0, 1.0) * 32000.0);
            dst[(pos + i) * 2 + 1] = (short)(std::clamp(r, -1.0, 1.0) * 32000.0);
        }
        for (int c = 0; c < MAXCH; ++c) g_liveEngine.chPulse[c] *= 0.90f;
        LeaveCriticalSection(&g_cs);
        pos += chunk;
    }
}

static DWORD WINAPI audioThreadProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (g_audioRun) {
        WaitForSingleObject(g_audioEvent, 40);
        if (!g_audioRun) break;
        for (int b = 0; b < NUM_BUFS; ++b) {
            if (g_hdr[b].dwFlags & WHDR_DONE) {
                fillAudioBuffer(g_pcm[b], BUF_SAMPLES);
                waveOutWrite(g_wave, &g_hdr[b], sizeof(WAVEHDR));
            }
        }
    }
    return 0;
}

static bool initAudioEngine() {
    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = SR;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 4;
    wf.nAvgBytesPerSec = SR * 4;

    g_audioEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_audioEvent) return false;

    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, (DWORD_PTR)g_audioEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(g_audioEvent);
        g_audioEvent = NULL;
        return false;
    }

    for (int b = 0; b < NUM_BUFS; ++b) {
        memset(&g_hdr[b], 0, sizeof(WAVEHDR));
        memset(g_pcm[b], 0, sizeof(g_pcm[b]));
        g_hdr[b].lpData         = (LPSTR)g_pcm[b];
        g_hdr[b].dwBufferLength = BUF_SAMPLES * 4;
        waveOutPrepareHeader(g_wave, &g_hdr[b], sizeof(WAVEHDR));
        waveOutWrite(g_wave, &g_hdr[b], sizeof(WAVEHDR));
    }

    g_audioRun = true;
    g_audioThread = CreateThread(NULL, 0, audioThreadProc, NULL, 0, NULL);
    return g_audioThread != NULL;
}

static void shutdownAudioEngine() {
    if (!g_wave) return;
    g_audioRun = false;
    if (g_audioEvent) SetEvent(g_audioEvent);
    if (g_audioThread) {
        WaitForSingleObject(g_audioThread, 2000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    waveOutReset(g_wave);
    for (int b = 0; b < NUM_BUFS; ++b) waveOutUnprepareHeader(g_wave, &g_hdr[b], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    g_wave = NULL;
    if (g_audioEvent) { CloseHandle(g_audioEvent); g_audioEvent = NULL; }
}

static void togglePlay() {
    EnterCriticalSection(&g_cs);
    if (!g_playing) {
        g_smpLeft    = 1;
        g_stepCursor = 0;
        g_playPat    = g_proj.songMode ? 0 : g_proj.curPat;
        g_playing    = true;
    } else {
        g_playing = false;
        g_liveEngine.clearVoices();
        InterlockedExchange(&g_displayStep, -1);
    }
    LeaveCriticalSection(&g_cs);
}

static void pushUndo() {
    if (g_undoStack.size() >= MAXUNDO) g_undoStack.erase(g_undoStack.begin());
    g_undoStack.push_back(g_proj);
}

static void performUndo() {
    if (g_undoStack.empty()) { setToast(L"Nothing to undo"); return; }
    EnterCriticalSection(&g_cs);
    g_proj = g_undoStack.back();
    g_undoStack.pop_back();
    LeaveCriticalSection(&g_cs);
    setToast(L"Undid last edit");
}

static void previewChannelStep(int ch, int step) {
    EnterCriticalSection(&g_cs);
    int kind = g_proj.ch[ch].kind;
    double midi = isDrum(kind) ? 60.0 : (double)(channelRootMidi(kind) + g_proj.pat[g_proj.curPat].tr[ch].off[step]);
    bool acc = (g_proj.pat[g_proj.curPat].tr[ch].accents & (1u << step)) != 0;
    g_liveEngine.spawn(kind, midi, 0.56 * g_proj.ch[ch].vol, g_proj.ch[ch].pan, g_proj.ch[ch].tone, g_proj.ch[ch].decay, acc);
    LeaveCriticalSection(&g_cs);
}

static void previewMidiNote(int ch, int midi, bool accent = false) {
    EnterCriticalSection(&g_cs);
    int useCh = isDrum(ch) ? KEYS : ch;
    int kind = g_proj.ch[useCh].kind;
    g_liveEngine.spawn(kind, (double)midi, 0.54 * g_proj.ch[useCh].vol,
                       g_proj.ch[useCh].pan, g_proj.ch[useCh].tone, g_proj.ch[useCh].decay, accent);
    LeaveCriticalSection(&g_cs);
}

static void generateSmartGroove(int ch) {
    pushUndo();
    EnterCriticalSection(&g_cs);
    PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[ch];
    tr.steps = 0; tr.accents = 0;
    memset(tr.off, 0, sizeof(tr.off));
    unsigned seed = GetTickCount() ^ (unsigned)(ch * 2654435761u);
    auto rnd = [&]() -> unsigned { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };

    if (ch == KICK) {
        tr.steps |= 1u; tr.accents |= 1u;
        for (int s = 2; s < 16; ++s) {
            int prob = (s == 8 || s == 10) ? 55 : (s % 2 == 0) ? 28 : 14;
            if ((int)(rnd() % 100) < prob) tr.steps |= (1u << s);
        }
    } else if (ch == SNARE || ch == CLAP) {
        tr.steps |= (1u << 4) | (1u << 12);
        tr.accents |= (1u << 4) | (1u << 12);
        if (rnd() % 3 == 0) tr.steps |= (1u << 15);
    } else if (ch == HAT) {
        for (int s = 0; s < 16; ++s) {
            int prob = (s % 2 == 0) ? 85 : 32;
            if ((int)(rnd() % 100) < prob) {
                tr.steps |= (1u << s);
                if (s % 4 == 0) tr.accents |= (1u << s);
            }
        }
    } else {
        unsigned mask = effectiveNoteMask(g_proj);
        std::vector<int> pool;
        for (int i = 0; i < 12; ++i) {
            int pc = (g_proj.rootKey + i) % 12;
            if (mask & (1u << pc)) pool.push_back(pc);
        }
        if (pool.empty()) pool.push_back(0);
        int density = (ch == PAD) ? 2 : (ch == BASS) ? 5 : 6;
        for (int i = 0; i < density; ++i) {
            int s = (ch == PAD) ? (i * 8) : (int)(rnd() % 16);
            tr.steps |= (1u << s);
            if (i == 0) tr.accents |= (1u << s);
            int pc = (i == 0) ? pool[0] : pool[rnd() % pool.size()];
            int oct = (ch == LEAD && (rnd() % 2)) ? 12 : 0;
            tr.off[s] = (signed char)std::clamp(pc + oct, MIN_OFF, MAX_OFF);
        }
    }
    LeaveCriticalSection(&g_cs);
    wchar_t buf[64];
    swprintf(buf, 64, L"Generated groove for %s", kKindName[ch]);
    setToast(buf);
}

// ---------------------------------------------------------------------------
//  UI Layout, Theme, Popover State, and Rendering
// ---------------------------------------------------------------------------

static const int WIN_W   = 1180;
static const int WIN_H   = 710;
static const int MIN_W   = 980;
static const int MIN_H   = 640;

static const int TOP_H   = 68;
static const int RAIL_W  = 198;
static const int FOOT_H  = 26;
static const int ROW_H   = 36;
static const int LABEL_W = 78;
static const int CELL_H  = 28;

static const int POP_W   = 296;
static const int POP_H   = 164;

static HWND  g_main      = NULL;
static HFONT g_font      = NULL;
static HFONT g_fontBold  = NULL;
static HFONT g_fontBig   = NULL;
static HFONT g_fontSmall = NULL;
static HFONT g_fontTiny  = NULL;
static RECT  g_client    = { 0, 0, WIN_W, WIN_H };

// Colors (Refined Ribbon Dark Plum Palette)
static const COLORREF C_BG       = RGB(20, 17, 26);
static const COLORREF C_TOP      = RGB(29, 25, 38);
static const COLORREF C_PANEL    = RGB(26, 22, 34);
static const COLORREF C_CARD     = RGB(31, 27, 41);
static const COLORREF C_CARD_HI  = RGB(42, 36, 56);
static const COLORREF C_CELL     = RGB(36, 31, 47);
static const COLORREF C_BEAT     = RGB(46, 40, 60);
static const COLORREF C_LINE     = RGB(58, 51, 74);
static const COLORREF C_CREAM    = RGB(244, 236, 226);
static const COLORREF C_MUTED    = RGB(158, 150, 176);
static const COLORREF C_ROSE     = RGB(255, 122, 154);
static const COLORREF C_MINT     = RGB(118, 218, 198);
static const COLORREF C_AMBER    = RGB(246, 194, 118);

enum Hot {
    HOT_NONE, HOT_PLAY, HOT_TEMPO, HOT_SWING, HOT_PAT, HOT_SONG,
    HOT_PRESET, HOT_OPEN, HOT_SAVE, HOT_EXPORT, HOT_MASTER,
    HOT_CHANNEL, HOT_RAIL_MUTE, HOT_RAIL_SOLO, HOT_STEP,
    HOT_VOL, HOT_PAN, HOT_TONE, HOT_DECAY, HOT_MUTE, HOT_SOLO,
    HOT_CLEAR_CH, HOT_GROOVE_CH,
    HOT_REVERB, HOT_DELAY, HOT_ROOT_KEY, HOT_SCALE, HOT_NOTE_POOL,
    HOT_COPY, HOT_PASTE, HOT_UNDO
};

enum DragMode {
    DRAG_NONE, DRAG_VOL, DRAG_PAN, DRAG_TONE, DRAG_DECAY,
    DRAG_MASTER, DRAG_REVERB, DRAG_DELAY, DRAG_TEMPO, DRAG_SWING,
    DRAG_PAINT_ON, DRAG_PAINT_OFF, DRAG_STEP_MAYBE, DRAG_PITCH
};

enum PopMode { POP_NONE, POP_STEP_NOTE, POP_NOTE_POOL };

enum PopHot {
    PHOT_NONE, PHOT_KEY, PHOT_OCT_DN, PHOT_OCT_UP, PHOT_ACC, PHOT_CLEAR,
    PHOT_P_MIN, PHOT_P_MAJ, PHOT_P_PNT, PHOT_P_ALL, PHOT_DONE
};

struct PopoverState {
    bool    open   = false;
    PopMode mode   = POP_NONE;
    int     ch     = KEYS;
    int     step   = 0;
    int     oct    = 4; // displayed octave for POP_STEP_NOTE
    RECT    rect   = { 0, 0, 0, 0 };
    PopHot  hot    = PHOT_NONE;
    int     hotPC  = -1;
};

static PopoverState g_pop;

static Hot      g_hot          = HOT_NONE;
static int      g_hotIdx       = -1;
static int      g_hotStep      = -1;
static DragMode g_drag         = DRAG_NONE;
static int      g_dragX0       = 0;
static int      g_dragY0       = 0;
static int      g_dragTempo0   = 92;
static float    g_dragSwing0   = 0.0f;
static int      g_dragCh       = 0;
static int      g_dragStep     = 0;
static int      g_dragOff0     = 0;
static int      g_dragLastStep = 0;

static DWORD    g_lastWheelTick = 0;
static int      g_lastWheelCh   = -1;
static int      g_lastWheelStep = -1;

// Layout rects
static RECT playRect()   { return { 202, 16, 238, 52 }; }
static RECT tempoRect()  { return { 248, 10, 320, 58 }; }
static RECT swingRect()  { return { 326, 10, 394, 58 }; }
static RECT patRect(int i) { int x = 404 + i * 28; return { x, 18, x + 25, 50 }; }
static RECT songRect()   { return { 520, 18, 574, 50 }; }

static RECT masterRect() { return { g_client.right - 134, 28, g_client.right - 34, 44 }; }
static RECT meterRect()  { return { g_client.right - 28, 14, g_client.right - 12, 54 }; }
static RECT expRect()    { return { g_client.right - 200, 18, g_client.right - 144, 50 }; }
static RECT saveRect()   { return { g_client.right - 252, 18, g_client.right - 204, 50 }; }
static RECT openRect()   { return { g_client.right - 304, 18, g_client.right - 256, 50 }; }
static RECT presetRect() { return { g_client.right - 394, 18, g_client.right - 310, 50 }; }

static int gridTop()  { return TOP_H + 30; }
static int gridLeft() { return RAIL_W + 12 + LABEL_W; }
static int cellW() {
    int avail = g_client.right - gridLeft() - 14;
    int w = avail / STEPS;
    return w < 16 ? 16 : w;
}

static RECT cellRect(int ch, int step) {
    int x = gridLeft() + step * cellW();
    int y = gridTop() + ch * ROW_H;
    return { x, y, x + cellW() - 4, y + CELL_H };
}

static RECT chanRowRect(int ch) {
    int y = TOP_H + 34 + ch * 46;
    return { 8, y, RAIL_W - 8, y + 40 };
}
static RECT railMuteRect(int ch) {
    RECT r = chanRowRect(ch);
    return { r.right - 48, r.top + 10, r.right - 28, r.top + 30 };
}
static RECT railSoloRect(int ch) {
    RECT r = chanRowRect(ch);
    return { r.right - 25, r.top + 10, r.right - 5, r.top + 30 };
}

static int inspTop() { return gridTop() + MAXCH * ROW_H + 14; }
static int inspBottom() { return g_client.bottom - FOOT_H - 12; }

static RECT cardTrackRect() {
    int x0 = RAIL_W + 12;
    int total = g_client.right - 12 - x0;
    int w = (int)(total * 0.43);
    return { x0, inspTop(), x0 + w, inspBottom() };
}
static RECT cardFxRect() {
    RECT c1 = cardTrackRect();
    int x0 = c1.right + 10;
    int total = g_client.right - 12 - (RAIL_W + 12);
    int w = (int)(total * 0.28);
    return { x0, inspTop(), x0 + w, inspBottom() };
}
static RECT cardScopeRect() {
    RECT c2 = cardFxRect();
    return { c2.right + 10, inspTop(), g_client.right - 12, inspBottom() };
}

static RECT trackSliderRect(int idx) {
    RECT c = cardTrackRect();
    int y = c.top + 48 + idx * 34;
    return { c.left + 72, y, c.right - 52, y + 16 };
}
static RECT inspClearRect()  { RECT c = cardTrackRect(); return { c.right - 188, c.top + 12, c.right - 138, c.top + 34 }; }
static RECT inspGrooveRect() { RECT c = cardTrackRect(); return { c.right - 133, c.top + 12, c.right - 74,  c.top + 34 }; }
static RECT inspMuteRect()   { RECT c = cardTrackRect(); return { c.right - 68,  c.top + 12, c.right - 42,  c.top + 34 }; }
static RECT inspSoloRect()   { RECT c = cardTrackRect(); return { c.right - 38,  c.top + 12, c.right - 12,  c.top + 34 }; }

static RECT fxSliderRect(int idx) {
    RECT c = cardFxRect();
    int y = c.top + 48 + idx * 34;
    return { c.left + 68, y, c.right - 44, y + 16 };
}
// Sleek 3-pill Key / Scale / Notes row (fits cleanly even at MIN_W)
static RECT rootKeyBtnRect() {
    RECT c = cardFxRect();
    return { c.left + 14, c.top + 118, c.left + 56, c.top + 144 };
}
static RECT scaleBtnRect() {
    RECT c = cardFxRect();
    return { c.left + 62, c.top + 118, c.right - 66, c.top + 144 };
}
static RECT notePoolBtnRect() {
    RECT c = cardFxRect();
    return { c.right - 60, c.top + 118, c.right - 14, c.top + 144 };
}
static RECT utilBtnRect(int idx) {
    RECT c = cardFxRect();
    int w = (c.right - c.left - 28 - 12) / 3;
    int x = c.left + 14 + idx * (w + 6);
    return { x, c.top + 152, x + w, c.top + 178 };
}

// ---------------------------------------------------------------------------
//  Floating 12-Key Popover Geometry & Actions
// ---------------------------------------------------------------------------

static const int kWhitePC[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int kBlackPC[5] = { 1, 3, 6, 8, 10 };
static const int kBlackBoundary[5] = { 1, 2, 4, 5, 6 };

static RECT popWhiteKeyRect(int wIdx) {
    int kbL = g_pop.rect.left + 8;
    int kbT = g_pop.rect.top + 40;
    return { kbL + wIdx * 40, kbT, kbL + wIdx * 40 + 39, kbT + 96 };
}

static RECT popBlackKeyRect(int bIdx) {
    int kbL = g_pop.rect.left + 8;
    int kbT = g_pop.rect.top + 40;
    int cx = kbL + kBlackBoundary[bIdx] * 40;
    return { cx - 13, kbT, cx + 13, kbT + 58 };
}

// Step-mode popover header controls
static RECT popOctDnRect()  { return { g_pop.rect.right - 150, g_pop.rect.top + 7, g_pop.rect.right - 126, g_pop.rect.top + 29 }; }
static RECT popOctValRect() { return { g_pop.rect.right - 125, g_pop.rect.top + 7, g_pop.rect.right - 91,  g_pop.rect.top + 29 }; }
static RECT popOctUpRect()  { return { g_pop.rect.right - 90,  g_pop.rect.top + 7, g_pop.rect.right - 66,  g_pop.rect.top + 29 }; }
static RECT popAccRect()    { return { g_pop.rect.right - 60,  g_pop.rect.top + 7, g_pop.rect.right - 30,  g_pop.rect.top + 29 }; }
static RECT popClearRect()  { return { g_pop.rect.right - 26,  g_pop.rect.top + 7, g_pop.rect.right - 8,   g_pop.rect.top + 29 }; }

// Pool-mode popover header controls
static RECT popPoolPresetRect(int idx) {
    // 0=Min, 1=Maj, 2=Pnt, 3=All
    const int xOff[4] = { 184, 150, 116, 82 };
    const int w[4]    = { 30,  30,  30,  28 };
    return { g_pop.rect.right - xOff[idx], g_pop.rect.top + 7, g_pop.rect.right - xOff[idx] + w[idx], g_pop.rect.top + 29 };
}
static RECT popDoneRect()   { return { g_pop.rect.right - 48,  g_pop.rect.top + 7, g_pop.rect.right - 8,   g_pop.rect.top + 29 }; }

static RECT calcClampedPopRect(RECT anchor, bool preferAbove) {
    int x = (anchor.left + anchor.right) / 2 - POP_W / 2;
    int y = preferAbove ? (anchor.top - POP_H - 8) : (anchor.bottom + 8);
    if (preferAbove && y < TOP_H + 6) y = anchor.bottom + 8;
    if (!preferAbove && y + POP_H > g_client.bottom - FOOT_H - 6) y = anchor.top - POP_H - 8;

    int minX = RAIL_W + 10;
    int maxX = std::max(minX, (int)g_client.right - POP_W - 10);
    int minY = TOP_H + 6;
    int maxY = std::max(minY, (int)g_client.bottom - FOOT_H - POP_H - 6);
    x = std::clamp(x, minX, maxX);
    y = std::clamp(y, minY, maxY);
    return { x, y, x + POP_W, y + POP_H };
}

static void openStepNotePopover(int ch, int step) {
    if (isDrum(ch)) return;
    g_sel = ch;
    int curOff = 0;
    EnterCriticalSection(&g_cs);
    curOff = g_proj.pat[g_proj.curPat].tr[ch].off[step];
    LeaveCriticalSection(&g_cs);

    int midi = channelRootMidi(ch) + curOff;
    int baseOct = channelBaseOct(ch);
    int oct = std::clamp((midi / 12) - 1, baseOct - 2, baseOct + 2);

    g_pop.open  = true;
    g_pop.mode  = POP_STEP_NOTE;
    g_pop.ch    = ch;
    g_pop.step  = step;
    g_pop.oct   = oct;
    g_pop.rect  = calcClampedPopRect(cellRect(ch, step), true);
    g_pop.hot   = PHOT_NONE;
    g_pop.hotPC = -1;
}

static void openNotePoolPopover() {
    g_pop.open  = true;
    g_pop.mode  = POP_NOTE_POOL;
    g_pop.rect  = calcClampedPopRect(scaleBtnRect(), true);
    g_pop.hot   = PHOT_NONE;
    g_pop.hotPC = -1;
}

static void closePopover() {
    g_pop.open  = false;
    g_pop.mode  = POP_NONE;
    g_pop.hot   = PHOT_NONE;
    g_pop.hotPC = -1;
}

static void assignStepPitchClass(int pc, bool closeAfter) {
    pushUndo();
    int midi = (g_pop.oct + 1) * 12 + pc;
    int off = std::clamp(midi - channelRootMidi(g_pop.ch), MIN_OFF, MAX_OFF);
    EnterCriticalSection(&g_cs);
    PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[g_pop.ch];
    tr.steps |= (1u << g_pop.step);
    tr.off[g_pop.step] = (signed char)off;
    LeaveCriticalSection(&g_cs);
    previewChannelStep(g_pop.ch, g_pop.step);

    wchar_t nbuf[16], msg[64];
    midiNoteName(channelRootMidi(g_pop.ch) + off, nbuf, 16);
    swprintf(msg, 64, L"%s step %d -> %s", kKindName[g_pop.ch], g_pop.step + 1, nbuf);
    setToast(msg, 1600);
    if (closeAfter) closePopover();
}

static void togglePoolNote(int pc) {
    pushUndo();
    bool turnedOn = false;
    bool rejected = false;
    EnterCriticalSection(&g_cs);
    if (g_proj.scale != SC_CUSTOM) {
        g_proj.customMask = effectiveNoteMask(g_proj);
        g_proj.scale = SC_CUSTOM;
    }
    unsigned bit = 1u << pc;
    if (g_proj.customMask & bit) {
        if (countMaskBits(g_proj.customMask) <= 1) {
            rejected = true;
        } else {
            g_proj.customMask &= ~bit;
        }
    } else {
        g_proj.customMask |= bit;
        turnedOn = true;
    }
    LeaveCriticalSection(&g_cs);

    if (rejected) {
        setToast(L"Keep at least 1 note active in your note pool");
    } else if (turnedOn) {
        int previewCh = isDrum(g_sel) ? KEYS : g_sel;
        previewMidiNote(previewCh, channelRootMidi(previewCh) + pc, false);
    }
}

// Drawing helpers
static inline bool inRect(RECT r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static COLORREF mixColor(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(bl, 0, 255));
}

static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void roundRectFill(HDC dc, RECT r, int rad, COLORREF bg, COLORREF border, int borderW = 1) {
    HBRUSH b = CreateSolidBrush(bg);
    HPEN   p = CreatePen(PS_SOLID, borderW, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void drawText(HDC dc, RECT r, const wchar_t* s, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

static void drawSlider(HDC dc, RECT r, float t, COLORREF accent, bool bipolar = false) {
    t = std::clamp(t, 0.0f, 1.0f);
    roundRectFill(dc, r, 8, RGB(18, 15, 24), C_LINE);
    int innerW = std::max(1, (int)(r.right - r.left - 4));
    RECT f = { r.left + 2, r.top + 2, r.left + 2, r.bottom - 2 };
    if (bipolar) {
        int mid = r.left + 2 + innerW / 2;
        int pos = r.left + 2 + (int)(innerW * t);
        f.left  = std::min(mid, pos);
        f.right = std::max(mid, pos);
        if (f.right - f.left < 3) { f.left = mid - 1; f.right = mid + 2; }
    } else {
        f.right = r.left + 2 + (int)(innerW * t);
    }
    if (f.right > f.left) {
        HBRUSH b = CreateSolidBrush(accent);
        HPEN   p = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, f.left, f.top, f.right, f.bottom, 6, 6);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(p);
    }
    int tx = r.left + 2 + (int)(innerW * t);
    RECT thumb = { tx - 3, r.top + 1, tx + 4, r.bottom - 1 };
    roundRectFill(dc, thumb, 4, C_CREAM, accent);
}

static void paintPopover(HDC dc, const Project& p) {
    if (!g_pop.open) return;

    RECT pr = g_pop.rect;
    // Soft layered drop shadow
    RECT sh2 = { pr.left - 4, pr.top + 4, pr.right + 6, pr.bottom + 8 };
    roundRectFill(dc, sh2, 14, RGB(11, 9, 16), RGB(11, 9, 16));
    RECT sh1 = { pr.left - 2, pr.top + 2, pr.right + 3, pr.bottom + 4 };
    roundRectFill(dc, sh1, 12, RGB(15, 12, 22), RGB(15, 12, 22));

    COLORREF accent = (g_pop.mode == POP_STEP_NOTE) ? kKindColor[g_pop.ch] : C_AMBER;
    roundRectFill(dc, pr, 12, RGB(34, 29, 46), accent, 1);

    unsigned mask = effectiveNoteMask(p);
    wchar_t buf[64];

    if (g_pop.mode == POP_STEP_NOTE) {
        const PatternTrack& tr = p.pat[p.curPat].tr[g_pop.ch];
        bool isOn  = (tr.steps & (1u << g_pop.step)) != 0;
        bool isAcc = (tr.accents & (1u << g_pop.step)) != 0;
        int curMidi = channelRootMidi(g_pop.ch) + tr.off[g_pop.step];
        int curPC   = ((curMidi % 12) + 12) % 12;
        int curOct  = (curMidi / 12) - 1;

        wchar_t nbuf[16];
        midiNoteName(curMidi, nbuf, 16);
        swprintf(buf, 64, L"%s S%d  %s", kKindName[g_pop.ch], g_pop.step + 1, isOn ? nbuf : L"--");
        RECT titleR = { pr.left + 10, pr.top + 6, pr.right - 154, pr.top + 30 };
        drawText(dc, titleR, buf, accent, g_fontBold);

        // Oct - / Oct N / Oct +
        RECT dnR = popOctDnRect();
        roundRectFill(dc, dnR, 5, (g_pop.hot == PHOT_OCT_DN) ? C_CARD_HI : RGB(44, 38, 58), C_LINE);
        drawText(dc, dnR, L"-", C_CREAM, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT ovR = popOctValRect();
        swprintf(buf, 32, L"Oct %d", g_pop.oct);
        drawText(dc, ovR, buf, C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT upR = popOctUpRect();
        roundRectFill(dc, upR, 5, (g_pop.hot == PHOT_OCT_UP) ? C_CARD_HI : RGB(44, 38, 58), C_LINE);
        drawText(dc, upR, L"+", C_CREAM, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Accent toggle
        RECT acR = popAccRect();
        COLORREF acBg = isAcc ? C_AMBER : ((g_pop.hot == PHOT_ACC) ? C_CARD_HI : RGB(44, 38, 58));
        roundRectFill(dc, acR, 5, acBg, isAcc ? C_CREAM : C_LINE);
        drawText(dc, acR, L"Acc", isAcc ? RGB(28, 20, 12) : C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Clear step
        RECT clR = popClearRect();
        roundRectFill(dc, clR, 5, (g_pop.hot == PHOT_CLEAR) ? RGB(92, 42, 58) : RGB(50, 34, 46), C_LINE);
        drawText(dc, clR, L"x", C_ROSE, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Draw 7 white keys
        for (int w = 0; w < 7; ++w) {
            int pc = kWhitePC[w];
            RECT kr = popWhiteKeyRect(w);
            bool inSc   = (mask & (1u << pc)) != 0;
            bool isSel  = isOn && (curPC == pc) && (curOct == g_pop.oct);
            bool isHov  = (g_pop.hot == PHOT_KEY && g_pop.hotPC == pc);

            COLORREF bg = isSel ? accent : (inSc ? RGB(236, 228, 220) : RGB(154, 146, 166));
            if (isHov && !isSel) bg = mixColor(bg, accent, 0.35f);
            COLORREF bd = isHov ? C_CREAM : RGB(28, 24, 38);
            roundRectFill(dc, kr, 5, bg, bd);

            if (inSc && !isSel && p.scale != SC_FREE) {
                RECT dot = { (kr.left + kr.right) / 2 - 2, kr.bottom - 24, (kr.left + kr.right) / 2 + 3, kr.bottom - 19 };
                fill(dc, dot, accent);
            }
            RECT lbl = { kr.left, kr.bottom - 18, kr.right, kr.bottom - 3 };
            drawText(dc, lbl, kNoteNames[pc], isSel ? RGB(24, 16, 28) : RGB(44, 38, 56), g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Draw 5 black keys on top
        for (int b = 0; b < 5; ++b) {
            int pc = kBlackPC[b];
            RECT kr = popBlackKeyRect(b);
            bool inSc   = (mask & (1u << pc)) != 0;
            bool isSel  = isOn && (curPC == pc) && (curOct == g_pop.oct);
            bool isHov  = (g_pop.hot == PHOT_KEY && g_pop.hotPC == pc);

            COLORREF bg = isSel ? accent : (inSc ? RGB(42, 36, 56) : RGB(24, 21, 32));
            if (isHov && !isSel) bg = mixColor(bg, accent, 0.42f);
            COLORREF bd = isHov ? C_CREAM : (isSel ? C_CREAM : RGB(18, 15, 24));
            roundRectFill(dc, kr, 5, bg, bd);

            if (inSc && !isSel && p.scale != SC_FREE) {
                RECT dot = { (kr.left + kr.right) / 2 - 2, kr.bottom - 20, (kr.left + kr.right) / 2 + 3, kr.bottom - 15 };
                fill(dc, dot, accent);
            }
            RECT lbl = { kr.left, kr.bottom - 15, kr.right, kr.bottom - 2 };
            drawText(dc, lbl, kNoteNames[pc], isSel ? RGB(24, 16, 28) : (inSc ? C_CREAM : C_MUTED), g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        RECT ft = { pr.left + 10, pr.bottom - 22, pr.right - 10, pr.bottom - 4 };
        drawText(dc, ft, L"Click: pick   Shift+Click: audition   Keys A-J: play", C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (g_pop.mode == POP_NOTE_POOL) {
        int activeCount = countMaskBits(mask);
        swprintf(buf, 64, L"Note Pool (%d)", activeCount);
        RECT titleR = { pr.left + 10, pr.top + 6, pr.right - 188, pr.top + 30 };
        drawText(dc, titleR, buf, C_AMBER, g_fontBold);

        const wchar_t* pLbl[4] = { L"Min", L"Maj", L"Pnt", L"All" };
        const int pSc[4] = { SC_MINOR, SC_MAJOR, SC_PENTA, SC_FREE };
        const PopHot pH[4] = { PHOT_P_MIN, PHOT_P_MAJ, PHOT_P_PNT, PHOT_P_ALL };
        for (int i = 0; i < 4; ++i) {
            RECT r = popPoolPresetRect(i);
            bool active = (p.scale == pSc[i]);
            COLORREF bg = active ? RGB(66, 54, 42) : ((g_pop.hot == pH[i]) ? C_CARD_HI : RGB(44, 38, 58));
            roundRectFill(dc, r, 5, bg, active ? C_AMBER : C_LINE);
            drawText(dc, r, pLbl[i], active ? C_AMBER : C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        RECT dnR = popDoneRect();
        roundRectFill(dc, dnR, 5, (g_pop.hot == PHOT_DONE) ? RGB(54, 88, 84) : RGB(42, 70, 68), C_MINT);
        drawText(dc, dnR, L"Done", C_MINT, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // White keys (toggle active notes)
        for (int w = 0; w < 7; ++w) {
            int pc = kWhitePC[w];
            RECT kr = popWhiteKeyRect(w);
            bool on    = (mask & (1u << pc)) != 0;
            bool isHov = (g_pop.hot == PHOT_KEY && g_pop.hotPC == pc);

            COLORREF bg = on ? RGB(238, 230, 222) : RGB(58, 52, 72);
            if (isHov) bg = mixColor(bg, C_AMBER, 0.28f);
            COLORREF bd = isHov ? C_CREAM : (on ? C_AMBER : RGB(34, 29, 44));
            roundRectFill(dc, kr, 5, bg, bd);

            if (on) {
                RECT bar = { kr.left + 8, kr.bottom - 24, kr.right - 8, kr.bottom - 20 };
                fill(dc, bar, (pc == p.rootKey) ? C_ROSE : C_MINT);
            }
            RECT lbl = { kr.left, kr.bottom - 18, kr.right, kr.bottom - 3 };
            drawText(dc, lbl, kNoteNames[pc], on ? RGB(28, 22, 36) : C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Black keys (toggle active notes)
        for (int b = 0; b < 5; ++b) {
            int pc = kBlackPC[b];
            RECT kr = popBlackKeyRect(b);
            bool on    = (mask & (1u << pc)) != 0;
            bool isHov = (g_pop.hot == PHOT_KEY && g_pop.hotPC == pc);

            COLORREF bg = on ? RGB(74, 62, 92) : RGB(24, 20, 32);
            if (isHov) bg = mixColor(bg, C_AMBER, 0.35f);
            COLORREF bd = isHov ? C_CREAM : (on ? C_AMBER : RGB(18, 15, 24));
            roundRectFill(dc, kr, 5, bg, bd);

            if (on) {
                RECT bar = { kr.left + 5, kr.bottom - 20, kr.right - 5, kr.bottom - 16 };
                fill(dc, bar, (pc == p.rootKey) ? C_ROSE : C_MINT);
            }
            RECT lbl = { kr.left, kr.bottom - 15, kr.right, kr.bottom - 2 };
            drawText(dc, lbl, kNoteNames[pc], on ? C_CREAM : RGB(92, 86, 108), g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        RECT ft = { pr.left + 10, pr.bottom - 22, pr.right - 10, pr.bottom - 4 };
        drawText(dc, ft, L"Toggle exact notes for wheel, pitch-drag & Groove", C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void paintUI(HDC dc) {
    Project p;
    float scopeSnap[256];
    int scopeHead = 0;
    float mL = 0.0f, mR = 0.0f;
    float pulse[MAXCH];
    EnterCriticalSection(&g_cs);
    p = g_proj;
    memcpy(scopeSnap, g_liveEngine.scopeBuf, sizeof(scopeSnap));
    scopeHead = g_liveEngine.scopePos;
    mL = g_liveEngine.meterL;
    mR = g_liveEngine.meterR;
    memcpy(pulse, g_liveEngine.chPulse, sizeof(pulse));
    LeaveCriticalSection(&g_cs);

    int shownStep = (int)g_displayStep;
    int shownPat  = (int)g_displayPat;
    int editPat   = p.curPat;

    fill(dc, g_client, C_BG);

    // ---- Top Transport Bar ----
    RECT topR = { 0, 0, g_client.right, TOP_H };
    fill(dc, topR, C_TOP);
    RECT topLine = { 0, TOP_H - 1, g_client.right, TOP_H };
    fill(dc, topLine, C_LINE);

    RECT bR = { 16, 10, 190, 38 };
    drawText(dc, bR, L"Ribbon 2.1", C_CREAM, g_fontBig);
    RECT sR = { 18, 38, 190, 56 };
    drawText(dc, sR, L"win11 studio sketchpad", C_MUTED, g_fontTiny);

    RECT pr = playRect();
    COLORREF playBg = g_playing ? RGB(255, 148, 174) : (g_hot == HOT_PLAY ? RGB(255, 138, 166) : C_ROSE);
    roundRectFill(dc, pr, 36, playBg, C_ROSE);
    {
        HBRUSH b = CreateSolidBrush(RGB(28, 15, 22));
        HPEN   pen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        if (g_playing) {
            Rectangle(dc, pr.left + 12, pr.top + 11, pr.left + 16, pr.top + 25);
            Rectangle(dc, pr.left + 20, pr.top + 11, pr.left + 24, pr.top + 25);
        } else {
            POINT tri[3] = {
                { pr.left + 14, pr.top + 10 },
                { pr.left + 14, pr.top + 26 },
                { pr.left + 26, pr.top + 18 }
            };
            Polygon(dc, tri, 3);
        }
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(pen);
    }

    wchar_t buf[128];
    RECT tr = tempoRect();
    roundRectFill(dc, tr, 8, (g_hot == HOT_TEMPO) ? C_CARD_HI : C_CARD, C_LINE);
    RECT tl = { tr.left + 6, tr.top + 4, tr.right - 6, tr.top + 18 };
    drawText(dc, tl, L"TEMPO", C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT tv = { tr.left + 4, tr.top + 18, tr.right - 4, tr.bottom - 4 };
    swprintf(buf, 64, L"%d BPM", p.tempo);
    drawText(dc, tv, buf, C_CREAM, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT swr = swingRect();
    roundRectFill(dc, swr, 8, (g_hot == HOT_SWING) ? C_CARD_HI : C_CARD, C_LINE);
    RECT swl = { swr.left + 6, swr.top + 4, swr.right - 6, swr.top + 18 };
    drawText(dc, swl, L"SWING", C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT swv = { swr.left + 4, swr.top + 18, swr.right - 4, swr.bottom - 4 };
    swprintf(buf, 64, L"%d%%", (int)(p.swing * 100.0f + 0.5f));
    drawText(dc, swv, buf, C_MINT, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* patLbl[4] = { L"A", L"B", L"C", L"D" };
    for (int i = 0; i < MAXPAT; ++i) {
        RECT r = patRect(i);
        bool isCur = (i == editPat);
        bool isPlayingPat = (g_playing && p.songMode && i == shownPat);
        bool hov = (g_hot == HOT_PAT && g_hotIdx == i);
        COLORREF bg = isCur ? RGB(66, 48, 86) : (hov ? C_CARD_HI : C_CARD);
        COLORREF bd = isPlayingPat ? C_MINT : (isCur ? C_ROSE : C_LINE);
        roundRectFill(dc, r, 6, bg, bd, isPlayingPat ? 2 : 1);
        drawText(dc, r, patLbl[i], isCur ? C_CREAM : C_MUTED, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT sgr = songRect();
    roundRectFill(dc, sgr, 6, p.songMode ? RGB(44, 72, 70) : (g_hot == HOT_SONG ? C_CARD_HI : C_CARD), p.songMode ? C_MINT : C_LINE);
    drawText(dc, sgr, L"SONG", p.songMode ? C_MINT : C_MUTED, g_fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, presetRect(), 7, (g_hot == HOT_PRESET) ? C_CARD_HI : RGB(39, 34, 52), C_LINE);
    drawText(dc, presetRect(), kPresetNames[g_presetIdx], C_AMBER, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, openRect(), 7, (g_hot == HOT_OPEN) ? C_CARD_HI : RGB(37, 32, 48), C_LINE);
    drawText(dc, openRect(), L"Open", C_CREAM, g_fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, saveRect(), 7, (g_hot == HOT_SAVE) ? C_CARD_HI : RGB(37, 32, 48), C_LINE);
    drawText(dc, saveRect(), L"Save", C_CREAM, g_fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, expRect(), 7, (g_hot == HOT_EXPORT) ? RGB(82, 42, 60) : RGB(64, 34, 48), C_ROSE);
    drawText(dc, expRect(), L"WAV", RGB(255, 224, 232), g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT ml = { masterRect().left, 10, masterRect().right, 24 };
    swprintf(buf, 64, L"MASTER  %d%%", (int)(p.master * 100.0f + 0.5f));
    drawText(dc, ml, buf, C_MUTED, g_fontTiny);
    drawSlider(dc, masterRect(), p.master, C_CREAM);

    RECT mr = meterRect();
    roundRectFill(dc, mr, 4, RGB(15, 13, 20), C_LINE);
    int mh = mr.bottom - mr.top - 4;
    int hL = std::clamp((int)(mL * mh), 0, mh);
    int hR = std::clamp((int)(mR * mh), 0, mh);
    if (hL > 0) {
        RECT bL = { mr.left + 2, mr.bottom - 2 - hL, mr.left + 7, mr.bottom - 2 };
        fill(dc, bL, (mL > 0.88f) ? C_ROSE : C_MINT);
    }
    if (hR > 0) {
        RECT bR = { mr.left + 9, mr.bottom - 2 - hR, mr.right - 2, mr.bottom - 2 };
        fill(dc, bR, (mR > 0.88f) ? C_ROSE : C_MINT);
    }

    // ---- Left Channel Rail ----
    RECT rail = { 0, TOP_H, RAIL_W, g_client.bottom - FOOT_H };
    fill(dc, rail, C_PANEL);
    RECT railLine = { RAIL_W - 1, TOP_H, RAIL_W, g_client.bottom - FOOT_H };
    fill(dc, railLine, C_LINE);

    RECT rh = { 12, TOP_H + 8, RAIL_W - 12, TOP_H + 26 };
    drawText(dc, rh, L"TRACKS (1-8)", C_MUTED, g_fontTiny);

    for (int c = 0; c < MAXCH; ++c) {
        RECT r = chanRowRect(c);
        bool sel = (c == g_sel);
        bool aud = isChannelAudible(p, c);
        bool hov = (g_hot == HOT_CHANNEL && g_hotIdx == c);
        roundRectFill(dc, r, 8, sel ? RGB(44, 38, 58) : (hov ? RGB(36, 31, 48) : C_CARD), sel ? kKindColor[c] : C_LINE);

        RECT sw = { r.left + 6, r.top + 8, r.left + 10, r.bottom - 8 };
        fill(dc, sw, aud ? kKindColor[c] : RGB(75, 68, 88));
        if (pulse[c] > 0.12f) {
            RECT led = { r.left + 6, r.top + 8, r.left + 11, r.bottom - 8 };
            fill(dc, led, C_CREAM);
        }

        RECT nm = { r.left + 18, r.top + 4, r.right - 52, r.top + 22 };
        drawText(dc, nm, kKindName[c], aud ? C_CREAM : C_MUTED, g_fontBold);
        RECT sub = { r.left + 18, r.top + 21, r.right - 52, r.bottom - 3 };
        drawText(dc, sub, isDrum(c) ? L"drum" : L"synth", C_MUTED, g_fontTiny);

        RECT rm = railMuteRect(c);
        bool muted = !p.ch[c].on;
        roundRectFill(dc, rm, 5, muted ? C_ROSE : RGB(33, 28, 44), C_LINE);
        drawText(dc, rm, L"M", muted ? RGB(28, 14, 20) : C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT rs = railSoloRect(c);
        bool solo = p.ch[c].solo;
        roundRectFill(dc, rs, 5, solo ? C_AMBER : RGB(33, 28, 44), C_LINE);
        drawText(dc, rs, L"S", solo ? RGB(28, 20, 10) : C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- Step Sequencer Grid ----
    int gtop = gridTop();
    int gleft = gridLeft();
    int cw = cellW();

    for (int s = 0; s < STEPS; ++s) {
        int x = gleft + s * cw;
        RECT hr = { x, TOP_H + 6, x + cw - 4, TOP_H + 24 };
        bool isCurPlay = (g_playing && s == shownStep);
        if (isCurPlay) {
            RECT glow = { x, TOP_H + 22, x + cw - 4, TOP_H + 26 };
            fill(dc, glow, C_CREAM);
        }
        if ((s % 4) == 0) {
            swprintf(buf, 16, L"%d", (s / 4) + 1);
            drawText(dc, hr, buf, isCurPlay ? C_CREAM : C_MUTED, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            drawText(dc, hr, L".", isCurPlay ? C_ROSE : RGB(90, 82, 106), g_fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    const Pattern& curP = p.pat[editPat];
    for (int c = 0; c < MAXCH; ++c) {
        int y = gtop + c * ROW_H;
        bool aud = isChannelAudible(p, c);
        RECT lr = { RAIL_W + 12, y, RAIL_W + 12 + LABEL_W - 6, y + CELL_H };
        drawText(dc, lr, kKindName[c], (c == g_sel) ? C_CREAM : C_MUTED, (c == g_sel) ? g_fontBold : g_fontSmall);

        for (int s = 0; s < STEPS; ++s) {
            RECT r = cellRect(c, s);
            bool on      = (curP.tr[c].steps & (1u << s)) != 0;
            bool acc     = (curP.tr[c].accents & (1u << s)) != 0;
            bool beat    = (s % 4) == 0;
            bool playCol = (g_playing && s == shownStep && (!p.songMode || editPat == shownPat));
            bool hov     = (!g_pop.open && g_hot == HOT_STEP && g_hotIdx == c && g_hotStep == s);
            bool popTgt  = (g_pop.open && g_pop.mode == POP_STEP_NOTE && g_pop.ch == c && g_pop.step == s);

            COLORREF bg = on ? kKindColor[c] : (beat ? C_BEAT : C_CELL);
            if (!aud) bg = on ? RGB(74, 68, 86) : RGB(28, 25, 36);
            else if (on && !acc) bg = mixColor(bg, RGB(24, 20, 32), 0.14f);
            else if (!on && hov) bg = mixColor(bg, kKindColor[c], 0.16f);

            COLORREF bd = playCol ? C_CREAM : (popTgt ? C_CREAM : (acc && on ? C_CREAM : (hov ? mixColor(C_LINE, kKindColor[c], 0.55f) : (beat ? RGB(74, 66, 92) : C_LINE))));
            roundRectFill(dc, r, 6, bg, bd, (playCol || popTgt || (acc && on)) ? 2 : 1);

            if (on && !isDrum(c)) {
                // Subtle accent top highlight
                if (acc) {
                    RECT bar = { r.left + 6, r.top + 2, r.right - 6, r.top + 4 };
                    fill(dc, bar, C_CREAM);
                }
                // Subtle pitch-height micro-bar along bottom edge
                float pitchFrac = (float)(curP.tr[c].off[s] - MIN_OFF) / (float)(MAX_OFF - MIN_OFF);
                int maxW = std::max(4, (int)(r.right - r.left - 10));
                int barW = std::max(3, (int)(maxW * std::clamp(pitchFrac, 0.0f, 1.0f)));
                RECT pbar = { r.left + 5, r.bottom - 4, r.left + 5 + barW, r.bottom - 2 };
                fill(dc, pbar, RGB(28, 20, 36));

                wchar_t nbuf[16];
                midiNoteName(channelRootMidi(c) + curP.tr[c].off[s], nbuf, 16);
                RECT trTxt = { r.left, r.top, r.right, r.bottom - 2 };
                drawText(dc, trTxt, nbuf, RGB(24, 18, 30), g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (on && acc) {
                RECT dot = { (r.left + r.right) / 2 - 2, (r.top + r.bottom) / 2 - 2,
                             (r.left + r.right) / 2 + 3, (r.top + r.bottom) / 2 + 3 };
                fill(dc, dot, RGB(28, 18, 26));
            }
        }
    }

    // ---- Bottom Inspector Cards ----
    RECT c1 = cardTrackRect();
    roundRectFill(dc, c1, 10, C_CARD, C_LINE);
    const ChannelMix& selCh = p.ch[g_sel];

    RECT c1t = { c1.left + 14, c1.top + 10, c1.right - 194, c1.top + 36 };
    swprintf(buf, 64, L"%s  (%s)", kKindName[g_sel], isDrum(g_sel) ? L"drum" : L"synth");
    drawText(dc, c1t, buf, kKindColor[g_sel], g_fontBold);

    roundRectFill(dc, inspClearRect(), 6, (g_hot == HOT_CLEAR_CH) ? C_CARD_HI : RGB(40, 34, 52), C_LINE);
    drawText(dc, inspClearRect(), L"Clear", C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, inspGrooveRect(), 6, (g_hot == HOT_GROOVE_CH) ? RGB(50, 46, 72) : RGB(42, 38, 60), C_MINT);
    drawText(dc, inspGrooveRect(), L"Groove", C_MINT, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, inspMuteRect(), 6, !selCh.on ? C_ROSE : RGB(40, 34, 52), C_LINE);
    drawText(dc, inspMuteRect(), L"M", !selCh.on ? RGB(28, 14, 20) : C_CREAM, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    roundRectFill(dc, inspSoloRect(), 6, selCh.solo ? C_AMBER : RGB(40, 34, 52), C_LINE);
    drawText(dc, inspSoloRect(), L"S", selCh.solo ? RGB(28, 20, 10) : C_CREAM, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* sLbl[4] = { L"Volume", L"Pan", L"Tone", L"Decay" };
    float sVal[4] = { selCh.vol, (selCh.pan + 1.0f) * 0.5f, selCh.tone, selCh.decay };
    COLORREF sCol[4] = { kKindColor[g_sel], C_MINT, C_AMBER, RGB(198, 158, 255) };
    for (int i = 0; i < 4; ++i) {
        RECT sr = trackSliderRect(i);
        RECT lb = { c1.left + 14, sr.top - 2, sr.left - 6, sr.bottom + 2 };
        drawText(dc, lb, sLbl[i], C_MUTED, g_fontSmall);
        drawSlider(dc, sr, sVal[i], sCol[i], i == 1);
        RECT vr = { sr.right + 6, sr.top - 2, c1.right - 8, sr.bottom + 2 };
        if (i == 1) {
            int pv = (int)(selCh.pan * 100.0f);
            if (std::abs(pv) < 4) wcscpy(buf, L"C");
            else if (pv < 0) swprintf(buf, 32, L"L%d", -pv);
            else swprintf(buf, 32, L"R%d", pv);
        } else {
            swprintf(buf, 32, L"%d%%", (int)(sVal[i] * 100.0f + 0.5f));
        }
        drawText(dc, vr, buf, C_CREAM, g_fontTiny, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // Card 2: Master FX & Key / Scale / Custom Notes
    RECT c2 = cardFxRect();
    roundRectFill(dc, c2, 10, C_CARD, C_LINE);
    RECT c2t = { c2.left + 14, c2.top + 10, c2.right - 14, c2.top + 36 };
    drawText(dc, c2t, L"Master FX & Notes", C_CREAM, g_fontBold);

    const wchar_t* fxLbl[2] = { L"Reverb", L"Delay" };
    float fxVal[2] = { p.reverbMix, p.delayMix };
    COLORREF fxCol[2] = { RGB(178, 146, 255), C_MINT };
    for (int i = 0; i < 2; ++i) {
        RECT sr = fxSliderRect(i);
        RECT lb = { c2.left + 14, sr.top - 2, sr.left - 6, sr.bottom + 2 };
        drawText(dc, lb, fxLbl[i], C_MUTED, g_fontSmall);
        drawSlider(dc, sr, fxVal[i], fxCol[i]);
        RECT vr = { sr.right + 6, sr.top - 2, c2.right - 8, sr.bottom + 2 };
        swprintf(buf, 32, L"%d%%", (int)(fxVal[i] * 100.0f + 0.5f));
        drawText(dc, vr, buf, C_CREAM, g_fontTiny, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // Key | Scale | Notes pill row
    RECT rkR = rootKeyBtnRect();
    bool keyDim = (p.scale == SC_FREE || p.scale == SC_CUSTOM);
    roundRectFill(dc, rkR, 6, (g_hot == HOT_ROOT_KEY) ? C_CARD_HI : RGB(40, 35, 54), C_LINE);
    drawText(dc, rkR, keyDim ? L"--" : kNoteNames[p.rootKey % 12], keyDim ? C_MUTED : C_CREAM, g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT scR = scaleBtnRect();
    roundRectFill(dc, scR, 6, (g_hot == HOT_SCALE) ? C_CARD_HI : RGB(40, 35, 54), (p.scale == SC_FREE) ? C_MINT : C_LINE);
    if (p.scale == SC_CUSTOM) {
        swprintf(buf, 64, L"Custom (%d)", countMaskBits(effectiveNoteMask(p)));
    } else {
        swprintf(buf, 64, L"%s", kScaleName[p.scale]);
    }
    drawText(dc, scR, buf, (p.scale == SC_FREE) ? C_MINT : C_AMBER, g_fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT npR = notePoolBtnRect();
    bool poolOpen = (g_pop.open && g_pop.mode == POP_NOTE_POOL);
    roundRectFill(dc, npR, 6, poolOpen ? RGB(64, 52, 42) : ((g_hot == HOT_NOTE_POOL) ? C_CARD_HI : RGB(42, 37, 58)), poolOpen ? C_AMBER : C_LINE);
    drawText(dc, npR, L"Notes", poolOpen ? C_AMBER : C_CREAM, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* uLbl[3] = { L"Copy Pat", L"Paste", L"Undo" };
    const Hot uHot[3] = { HOT_COPY, HOT_PASTE, HOT_UNDO };
    for (int i = 0; i < 3; ++i) {
        RECT ur = utilBtnRect(i);
        bool dim = (i == 1 && !g_hasClipboard) || (i == 2 && g_undoStack.empty());
        roundRectFill(dc, ur, 6, (!dim && g_hot == uHot[i]) ? C_CARD_HI : RGB(37, 32, 48), C_LINE);
        drawText(dc, ur, uLbl[i], dim ? RGB(92, 86, 108) : C_CREAM, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Card 3: Live Oscilloscope
    RECT c3 = cardScopeRect();
    roundRectFill(dc, c3, 10, C_CARD, C_LINE);
    RECT c3t = { c3.left + 14, c3.top + 10, c3.right - 14, c3.top + 34 };
    drawText(dc, c3t, L"Output Scope", C_CREAM, g_fontBold);

    RECT scBox = { c3.left + 14, c3.top + 42, c3.right - 14, c3.bottom - 34 };
    roundRectFill(dc, scBox, 8, RGB(16, 14, 22), C_LINE);

    int midY = (scBox.top + scBox.bottom) / 2;
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(34, 30, 44));
    HGDIOBJ oldP = SelectObject(dc, gridPen);
    MoveToEx(dc, scBox.left + 4, midY, NULL);
    LineTo(dc, scBox.right - 4, midY);
    SelectObject(dc, oldP);
    DeleteObject(gridPen);

    HPEN wavePen = CreatePen(PS_SOLID, 2, C_ROSE);
    oldP = SelectObject(dc, wavePen);
    int sw = std::max(1, (int)(scBox.right - scBox.left - 8));
    int sh = std::max(1, (int)(scBox.bottom - scBox.top - 10)) / 2;
    for (int i = 0; i < 256; ++i) {
        int idx = (scopeHead + i) & 255;
        int x = scBox.left + 4 + (i * sw) / 255;
        int y = midY - (int)(std::clamp(scopeSnap[idx], -1.0f, 1.0f) * sh);
        if (i == 0) MoveToEx(dc, x, y, NULL);
        else LineTo(dc, x, y);
    }
    SelectObject(dc, oldP);
    DeleteObject(wavePen);

    RECT scHint = { c3.left + 14, c3.bottom - 28, c3.right - 14, c3.bottom - 6 };
    drawText(dc, scHint,
             isDrum(g_sel) ? L"Alt+Click: accent   Drag: paint"
                           : L"Right-Click: piano   Drag up/dn or Wheel: pitch",
             C_MUTED, g_fontTiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ---- Floating Pitch-Glide Pill (during vertical drag on an active step) ----
    if (g_drag == DRAG_PITCH && !isDrum(g_dragCh)) {
        RECT cr = cellRect(g_dragCh, g_dragStep);
        int cx = (cr.left + cr.right) / 2;
        int py = std::max(TOP_H + 8, (int)cr.top - 30);
        RECT pill = { cx - 28, py, cx + 28, py + 24 };
        roundRectFill(dc, pill, 10, RGB(24, 20, 34), kKindColor[g_dragCh], 2);
        wchar_t nbuf[16];
        midiNoteName(channelRootMidi(g_dragCh) + curP.tr[g_dragCh].off[g_dragStep], nbuf, 16);
        drawText(dc, pill, nbuf, kKindColor[g_dragCh], g_fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- Floating 12-Key Popover (on top of grid & cards) ----
    paintPopover(dc, p);

    // ---- Footer ----
    RECT foot = { 0, g_client.bottom - FOOT_H, g_client.right, g_client.bottom };
    fill(dc, foot, RGB(16, 14, 22));
    RECT ft = { 12, foot.top, g_client.right - 12, foot.bottom };
    if (g_toastText[0] && GetTickCount() < g_toastUntil) {
        drawText(dc, ft, g_toastText, C_MINT, g_fontSmall);
    } else {
        swprintf(buf, 128,
            L"%s  |  Pat %s%s  |  Right-Click cell: Piano  |  Drag vertical / Wheel: Pitch  |  Ctrl+Z: Undo",
            g_playing ? L"Playing" : L"Ready",
            patLbl[editPat],
            p.songMode ? L" (Song A-D)" : L"");
        drawText(dc, ft, buf, C_MUTED, g_fontTiny);
    }
}

static void redraw() {
    if (!g_main) return;
    InvalidateRect(g_main, NULL, FALSE);
}

// ---------------------------------------------------------------------------
//  Hit Testing & Slider Helpers
// ---------------------------------------------------------------------------

static void hitTestPopover(int x, int y, PopHot& hot, int& pc) {
    hot = PHOT_NONE; pc = -1;
    if (!g_pop.open || !inRect(g_pop.rect, x, y)) return;

    if (g_pop.mode == POP_STEP_NOTE) {
        if (inRect(popOctDnRect(), x, y)) { hot = PHOT_OCT_DN; return; }
        if (inRect(popOctUpRect(), x, y)) { hot = PHOT_OCT_UP; return; }
        if (inRect(popAccRect(), x, y))   { hot = PHOT_ACC; return; }
        if (inRect(popClearRect(), x, y)) { hot = PHOT_CLEAR; return; }
    } else if (g_pop.mode == POP_NOTE_POOL) {
        const PopHot pH[4] = { PHOT_P_MIN, PHOT_P_MAJ, PHOT_P_PNT, PHOT_P_ALL };
        for (int i = 0; i < 4; ++i) {
            if (inRect(popPoolPresetRect(i), x, y)) { hot = pH[i]; return; }
        }
        if (inRect(popDoneRect(), x, y)) { hot = PHOT_DONE; return; }
    }

    // Black keys take priority over white keys
    for (int b = 0; b < 5; ++b) {
        if (inRect(popBlackKeyRect(b), x, y)) { hot = PHOT_KEY; pc = kBlackPC[b]; return; }
    }
    for (int w = 0; w < 7; ++w) {
        if (inRect(popWhiteKeyRect(w), x, y)) { hot = PHOT_KEY; pc = kWhitePC[w]; return; }
    }
}

static void hitTest(int x, int y, Hot& hot, int& idx, int& step) {
    hot = HOT_NONE; idx = -1; step = -1;
    if (inRect(playRect(), x, y))   { hot = HOT_PLAY; return; }
    if (inRect(tempoRect(), x, y))  { hot = HOT_TEMPO; return; }
    if (inRect(swingRect(), x, y))  { hot = HOT_SWING; return; }
    for (int i = 0; i < MAXPAT; ++i)
        if (inRect(patRect(i), x, y)) { hot = HOT_PAT; idx = i; return; }
    if (inRect(songRect(), x, y))   { hot = HOT_SONG; return; }
    if (inRect(presetRect(), x, y)) { hot = HOT_PRESET; return; }
    if (inRect(openRect(), x, y))   { hot = HOT_OPEN; return; }
    if (inRect(saveRect(), x, y))   { hot = HOT_SAVE; return; }
    if (inRect(expRect(), x, y))    { hot = HOT_EXPORT; return; }
    if (inRect(masterRect(), x, y)) { hot = HOT_MASTER; return; }

    for (int c = 0; c < MAXCH; ++c) {
        if (inRect(railMuteRect(c), x, y)) { hot = HOT_RAIL_MUTE; idx = c; return; }
        if (inRect(railSoloRect(c), x, y)) { hot = HOT_RAIL_SOLO; idx = c; return; }
        if (inRect(chanRowRect(c), x, y))  { hot = HOT_CHANNEL; idx = c; return; }
    }
    for (int c = 0; c < MAXCH; ++c) {
        for (int s = 0; s < STEPS; ++s) {
            if (inRect(cellRect(c, s), x, y)) { hot = HOT_STEP; idx = c; step = s; return; }
        }
    }

    if (inRect(inspClearRect(), x, y))  { hot = HOT_CLEAR_CH; return; }
    if (inRect(inspGrooveRect(), x, y)) { hot = HOT_GROOVE_CH; return; }
    if (inRect(inspMuteRect(), x, y))   { hot = HOT_MUTE; return; }
    if (inRect(inspSoloRect(), x, y))   { hot = HOT_SOLO; return; }

    if (inRect(trackSliderRect(0), x, y)) { hot = HOT_VOL; return; }
    if (inRect(trackSliderRect(1), x, y)) { hot = HOT_PAN; return; }
    if (inRect(trackSliderRect(2), x, y)) { hot = HOT_TONE; return; }
    if (inRect(trackSliderRect(3), x, y)) { hot = HOT_DECAY; return; }

    if (inRect(fxSliderRect(0), x, y))  { hot = HOT_REVERB; return; }
    if (inRect(fxSliderRect(1), x, y))  { hot = HOT_DELAY; return; }
    if (inRect(rootKeyBtnRect(), x, y)) { hot = HOT_ROOT_KEY; return; }
    if (inRect(scaleBtnRect(), x, y))   { hot = HOT_SCALE; return; }
    if (inRect(notePoolBtnRect(), x, y)){ hot = HOT_NOTE_POOL; return; }
    if (inRect(utilBtnRect(0), x, y))   { hot = HOT_COPY; return; }
    if (inRect(utilBtnRect(1), x, y))   { hot = HOT_PASTE; return; }
    if (inRect(utilBtnRect(2), x, y))   { hot = HOT_UNDO; return; }
}

static float sliderFrac(RECT r, int x) {
    float t = (float)(x - r.left) / (float)std::max(1L, r.right - r.left);
    return std::clamp(t, 0.0f, 1.0f);
}

static void applySliderDrag(DragMode dm, int x) {
    EnterCriticalSection(&g_cs);
    switch (dm) {
    case DRAG_VOL:    g_proj.ch[g_sel].vol   = sliderFrac(trackSliderRect(0), x); break;
    case DRAG_PAN:    g_proj.ch[g_sel].pan   = sliderFrac(trackSliderRect(1), x) * 2.0f - 1.0f; break;
    case DRAG_TONE:   g_proj.ch[g_sel].tone  = sliderFrac(trackSliderRect(2), x); break;
    case DRAG_DECAY:  g_proj.ch[g_sel].decay = sliderFrac(trackSliderRect(3), x); break;
    case DRAG_MASTER: g_proj.master          = sliderFrac(masterRect(), x); break;
    case DRAG_REVERB: g_proj.reverbMix       = sliderFrac(fxSliderRect(0), x); break;
    case DRAG_DELAY:  g_proj.delayMix        = sliderFrac(fxSliderRect(1), x); break;
    default: break;
    }
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
//  File I/O (Backward-compatible RIBBON 1 & 2 + Offline WAV Export)
// ---------------------------------------------------------------------------

static bool loadProjectFromPath(const wchar_t* path) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;

    wchar_t head[32] = L"";
    int version = 0;
    if (fwscanf(f, L"%31s %d", head, &version) != 2 || wcscmp(head, L"RIBBON") != 0) {
        fclose(f);
        return false;
    }

    Project np;
    resetMixDefaults(np);
    bool hasKeyLine = false;
    wchar_t key[32];
    while (fwscanf(f, L"%31s", key) == 1) {
        if (wcscmp(key, L"tempo") == 0) {
            int t = 92; if (fwscanf(f, L"%d", &t) == 1) np.tempo = std::clamp(t, 40, 240);
        } else if (wcscmp(key, L"swing") == 0) {
            double s = 0.0; if (fwscanf(f, L"%lf", &s) == 1) np.swing = (float)std::clamp(s, 0.0, 0.60);
        } else if (wcscmp(key, L"scale") == 0) {
            int sc = 0; if (fwscanf(f, L"%d", &sc) == 1) np.scale = std::clamp(sc, 0, NSCALE - 1);
        } else if (wcscmp(key, L"key") == 0) {
            int rk = 0; unsigned cm = 0x5ADu;
            if (fwscanf(f, L"%d %u", &rk, &cm) == 2) {
                np.rootKey = ((rk % 12) + 12) % 12;
                np.customMask = (cm & 0xFFFu) ? (cm & 0xFFFu) : 1u;
                hasKeyLine = true;
            }
        } else if (wcscmp(key, L"master") == 0) {
            double m = 0.84; if (fwscanf(f, L"%lf", &m) == 1) np.master = (float)std::clamp(m, 0.0, 1.0);
        } else if (wcscmp(key, L"fx") == 0) {
            double rv = 0.24, dl = 0.18;
            if (fwscanf(f, L"%lf %lf", &rv, &dl) == 2) {
                np.reverbMix = (float)std::clamp(rv, 0.0, 1.0);
                np.delayMix  = (float)std::clamp(dl, 0.0, 1.0);
            }
        } else if (wcscmp(key, L"mode") == 0) {
            int cp = 0, sm = 0;
            if (fwscanf(f, L"%d %d", &cp, &sm) == 2) {
                np.curPat = std::clamp(cp, 0, MAXPAT - 1);
                np.songMode = (sm != 0);
            }
        } else if (wcscmp(key, L"mix") == 0) {
            int idx = -1, on = 1, solo = 0;
            double vol = 0.75, pan = 0.0, tone = 0.7, dec = 0.5;
            if (fwscanf(f, L"%d %d %d %lf %lf %lf %lf", &idx, &on, &solo, &vol, &pan, &tone, &dec) == 7) {
                if (idx >= 0 && idx < MAXCH) {
                    np.ch[idx].on    = (on != 0);
                    np.ch[idx].solo  = (solo != 0);
                    np.ch[idx].vol   = (float)std::clamp(vol, 0.0, 1.0);
                    np.ch[idx].pan   = (float)std::clamp(pan, -1.0, 1.0);
                    np.ch[idx].tone  = (float)std::clamp(tone, 0.0, 1.0);
                    np.ch[idx].decay = (float)std::clamp(dec, 0.0, 1.0);
                }
            }
        } else if (wcscmp(key, L"pat") == 0) {
            int pi = 0, ch = 0;
            unsigned st = 0, ac = 0;
            if (fwscanf(f, L"%d %d %u %u", &pi, &ch, &st, &ac) == 4) {
                if (pi >= 0 && pi < MAXPAT && ch >= 0 && ch < MAXCH) {
                    np.pat[pi].tr[ch].steps   = st;
                    np.pat[pi].tr[ch].accents = ac;
                    for (int s = 0; s < STEPS; ++s) {
                        int o = 0;
                        if (fwscanf(f, L"%d", &o) == 1) np.pat[pi].tr[ch].off[s] = (signed char)std::clamp(o, MIN_OFF, MAX_OFF);
                    }
                }
            }
        } else if (wcscmp(key, L"ch") == 0) {
            int idx = -1, kind = 0;
            unsigned st = 0;
            double vol = 0.75, pan = 0.0;
            if (fwscanf(f, L"%d %d %lf %lf %u", &idx, &kind, &vol, &pan, &st) == 5) {
                if (idx >= 0 && idx < MAXCH) {
                    np.ch[idx].vol = (float)std::clamp(vol, 0.0, 1.0);
                    np.ch[idx].pan = (float)std::clamp(pan, -1.0, 1.0);
                    np.pat[0].tr[idx].steps = st;
                    for (int s = 0; s < STEPS; ++s) {
                        int o = 0;
                        if (fwscanf(f, L"%d", &o) == 1) np.pat[0].tr[idx].off[s] = (signed char)std::clamp(o, MIN_OFF, MAX_OFF);
                    }
                }
            }
        }
    }
    fclose(f);

    if (!hasKeyLine) np.customMask = buildScaleMask(np.scale, np.rootKey);
    closePopover();
    pushUndo();
    EnterCriticalSection(&g_cs);
    g_proj = np;
    LeaveCriticalSection(&g_cs);
    setToast(L"Project loaded");
    return true;
}

static void saveProject() {
    wchar_t path[MAX_PATH] = L"track.rib";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_main;
    ofn.lpstrFilter = L"Ribbon project (*.rib)\0*.rib\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"rib";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    EnterCriticalSection(&g_cs);
    Project p = g_proj;
    LeaveCriticalSection(&g_cs);

    FILE* f = _wfopen(path, L"w");
    if (!f) { MessageBoxW(g_main, L"Could not write project file.", L"Ribbon", MB_ICONWARNING); return; }

    fwprintf(f, L"RIBBON 2\n");
    fwprintf(f, L"tempo %d\n", p.tempo);
    fwprintf(f, L"swing %.3f\n", p.swing);
    fwprintf(f, L"scale %d\n", p.scale);
    fwprintf(f, L"key %d %u\n", p.rootKey, p.customMask & 0xFFFu);
    fwprintf(f, L"master %.3f\n", p.master);
    fwprintf(f, L"fx %.3f %.3f\n", p.reverbMix, p.delayMix);
    fwprintf(f, L"mode %d %d\n", p.curPat, p.songMode ? 1 : 0);
    for (int c = 0; c < MAXCH; ++c) {
        fwprintf(f, L"mix %d %d %d %.3f %.3f %.3f %.3f\n",
                 c, p.ch[c].on ? 1 : 0, p.ch[c].solo ? 1 : 0,
                 p.ch[c].vol, p.ch[c].pan, p.ch[c].tone, p.ch[c].decay);
    }
    for (int pi = 0; pi < MAXPAT; ++pi) {
        for (int c = 0; c < MAXCH; ++c) {
            fwprintf(f, L"pat %d %d %u %u", pi, c, p.pat[pi].tr[c].steps, p.pat[pi].tr[c].accents);
            for (int s = 0; s < STEPS; ++s) fwprintf(f, L" %d", (int)p.pat[pi].tr[c].off[s]);
            fwprintf(f, L"\n");
        }
    }
    fclose(f);
    setToast(L"Saved project (.rib)");
}

static void openProject() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_main;
    ofn.lpstrFilter = L"Ribbon project (*.rib)\0*.rib\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!loadProjectFromPath(path)) {
        MessageBoxW(g_main, L"Could not open that Ribbon project.", L"Ribbon", MB_ICONWARNING);
    }
}

static void exportWav() {
    wchar_t path[MAX_PATH] = L"ribbon_track.wav";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_main;
    ofn.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    EnterCriticalSection(&g_cs);
    Project p = g_proj;
    LeaveCriticalSection(&g_cs);

    std::unique_ptr<SynthEngine> eng(new SynthEngine());
    const int numBars = 4;
    const int totalSteps = numBars * STEPS;
    int activeFrames = 0;
    for (int s = 0; s < totalSteps; ++s) activeFrames += calcStepSamples(p.tempo, p.swing, s % STEPS);
    const int tailFrames = SR * 2;
    const int totalFrames = activeFrames + tailFrames;

    std::vector<short> out((size_t)totalFrames * 2);
    int curStep = 0;
    int smpLeft = 0;
    for (int i = 0; i < totalFrames; ++i) {
        if (curStep < totalSteps && --smpLeft <= 0) {
            int patIdx = p.songMode ? ((curStep / STEPS) % MAXPAT) : p.curPat;
            eng->triggerStep(p, patIdx, curStep % STEPS);
            smpLeft = calcStepSamples(p.tempo, p.swing, curStep % STEPS);
            ++curStep;
        }
        double l = 0.0, r = 0.0;
        eng->renderFrame(p, l, r, false);
        out[(size_t)i * 2]     = (short)(std::clamp(l, -1.0, 1.0) * 32000.0);
        out[(size_t)i * 2 + 1] = (short)(std::clamp(r, -1.0, 1.0) * 32000.0);
    }

    FILE* f = _wfopen(path, L"wb");
    if (!f) { MessageBoxW(g_main, L"Could not write WAV file.", L"Ribbon", MB_ICONWARNING); return; }

    unsigned dataBytes = (unsigned)(out.size() * sizeof(short));
    unsigned riffSize  = 36u + dataBytes;
    unsigned fmtSize = 16, rate = SR, byteRate = SR * 4;
    unsigned short fmt = 1, chans = 2, align = 4, bits = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&chans, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(out.data(), sizeof(short), out.size(), f);
    fclose(f);

    setToast(p.songMode ? L"Exported 4-bar Song (A-D) WAV" : L"Exported 4-bar Pattern WAV", 3500);
    MessageBoxW(g_main,
                p.songMode ? L"WAV exported (Full Song A-D + FX tail)." : L"WAV exported (4 bars + FX tail).",
                L"Ribbon 2.1", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
//  Windows 11 Dark Title Bar & Per-Monitor V2 DPI
// ---------------------------------------------------------------------------

static void enableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL (WINAPI *FnSetDpiCtx)(HANDLE);
    auto fn = (FnSetDpiCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (fn) {
        fn((HANDLE)(LONG_PTR)-4);
    }
}

static void applyWin11DarkTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *FnDwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = (FnDwmSetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        BOOL dark = TRUE;
        fn(hwnd, 20, &dark, sizeof(dark));
        fn(hwnd, 19, &dark, sizeof(dark));
        COLORREF cap = C_TOP;
        fn(hwnd, 35, &cap, sizeof(cap));
    }
    FreeLibrary(dwm);
}

// ---------------------------------------------------------------------------
//  Window Procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        GetClientRect(hwnd, &g_client);
        applyWin11DarkTitleBar(hwnd);
        SetTimer(hwnd, 1, 25, NULL);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = MIN_W;
        mm->ptMinTrackSize.y = MIN_H;
        return 0;
    }

    case WM_SIZE:
        GetClientRect(hwnd, &g_client);
        if (g_pop.open) {
            if (g_pop.mode == POP_STEP_NOTE) g_pop.rect = calcClampedPopRect(cellRect(g_pop.ch, g_pop.step), true);
            else if (g_pop.mode == POP_NOTE_POOL) g_pop.rect = calcClampedPopRect(scaleBtnRect(), true);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        int w = std::max(1L, g_client.right);
        int h = std::max(1L, g_client.bottom);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        paintUI(mem);
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        if (g_drag != DRAG_NONE) {
            if (g_drag == DRAG_TEMPO) {
                int t = std::clamp(g_dragTempo0 + (g_dragY0 - y) / 2, 40, 240);
                EnterCriticalSection(&g_cs); g_proj.tempo = t; LeaveCriticalSection(&g_cs);
            } else if (g_drag == DRAG_SWING) {
                float s = std::clamp(g_dragSwing0 + (float)(g_dragY0 - y) * 0.004f, 0.0f, 0.60f);
                EnterCriticalSection(&g_cs); g_proj.swing = s; LeaveCriticalSection(&g_cs);
            } else if (g_drag == DRAG_STEP_MAYBE) {
                int dx = x - g_dragX0;
                int dy = g_dragY0 - y;
                Hot h2; int c2, s2;
                hitTest(x, y, h2, c2, s2);
                if (std::abs(dy) >= 6 && std::abs(dy) >= std::abs(dx)) {
                    // Vertical drag -> pitch glide!
                    pushUndo();
                    g_drag = DRAG_PITCH;
                    g_dragLastStep = 0;
                    SetCursor(LoadCursor(NULL, IDC_SIZENS));
                } else if (h2 == HOT_STEP && (c2 != g_dragCh || s2 != g_dragStep)) {
                    // Horizontal drag into another cell -> paint off!
                    pushUndo();
                    EnterCriticalSection(&g_cs);
                    unsigned m0 = 1u << g_dragStep;
                    g_proj.pat[g_proj.curPat].tr[g_dragCh].steps   &= ~m0;
                    g_proj.pat[g_proj.curPat].tr[g_dragCh].accents &= ~m0;
                    unsigned m2 = 1u << s2;
                    g_proj.pat[g_proj.curPat].tr[c2].steps   &= ~m2;
                    g_proj.pat[g_proj.curPat].tr[c2].accents &= ~m2;
                    LeaveCriticalSection(&g_cs);
                    g_drag = DRAG_PAINT_OFF;
                }
            } else if (g_drag == DRAG_PITCH) {
                int stepsDelta = (g_dragY0 - y) / 10;
                if (stepsDelta != g_dragLastStep) {
                    bool chrom = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    int dir = (stepsDelta > g_dragLastStep) ? 1 : -1;
                    int count = std::abs(stepsDelta - g_dragLastStep);
                    bool changed = false;
                    EnterCriticalSection(&g_cs);
                    PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[g_dragCh];
                    int cur = tr.off[g_dragStep];
                    for (int k = 0; k < count; ++k) cur = stepNoteOffset(g_proj, cur, dir, chrom);
                    if (cur != tr.off[g_dragStep]) {
                        tr.off[g_dragStep] = (signed char)cur;
                        changed = true;
                    }
                    LeaveCriticalSection(&g_cs);
                    g_dragLastStep = stepsDelta;
                    if (changed) previewChannelStep(g_dragCh, g_dragStep);
                }
            } else if (g_drag == DRAG_PAINT_ON || g_drag == DRAG_PAINT_OFF) {
                Hot h2; int c2, s2;
                hitTest(x, y, h2, c2, s2);
                if (h2 == HOT_STEP) {
                    EnterCriticalSection(&g_cs);
                    unsigned mask = 1u << s2;
                    bool wasOn = (g_proj.pat[g_proj.curPat].tr[c2].steps & mask) != 0;
                    if (g_drag == DRAG_PAINT_ON && !wasOn) {
                        g_proj.pat[g_proj.curPat].tr[c2].steps |= mask;
                        g_sel = c2;
                        LeaveCriticalSection(&g_cs);
                        previewChannelStep(c2, s2);
                    } else if (g_drag == DRAG_PAINT_OFF && wasOn) {
                        g_proj.pat[g_proj.curPat].tr[c2].steps   &= ~mask;
                        g_proj.pat[g_proj.curPat].tr[c2].accents &= ~mask;
                        LeaveCriticalSection(&g_cs);
                    } else {
                        LeaveCriticalSection(&g_cs);
                    }
                }
            } else {
                applySliderDrag(g_drag, x);
            }
            redraw();
            return 0;
        }

        if (g_pop.open) {
            PopHot ph; int ppc;
            hitTestPopover(x, y, ph, ppc);
            if (ph != g_pop.hot || ppc != g_pop.hotPC) {
                g_pop.hot = ph;
                g_pop.hotPC = ppc;
                SetCursor(LoadCursor(NULL, (ph != PHOT_NONE) ? IDC_HAND : IDC_ARROW));
                redraw();
            }
            return 0;
        }

        Hot hot; int idx, step;
        hitTest(x, y, hot, idx, step);
        if (hot != g_hot || idx != g_hotIdx || step != g_hotStep) {
            g_hot = hot; g_hotIdx = idx; g_hotStep = step;
            HCURSOR cur = LoadCursor(NULL, IDC_ARROW);
            if (hot == HOT_TEMPO || hot == HOT_SWING) cur = LoadCursor(NULL, IDC_SIZENS);
            else if (hot != HOT_NONE) cur = LoadCursor(NULL, IDC_HAND);
            SetCursor(cur);
            redraw();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU)  & 0x8000) != 0;

        // Handle clicks when floating 12-key popover is open
        if (g_pop.open) {
            if (!inRect(g_pop.rect, x, y)) {
                closePopover();
                redraw();
                return 0;
            }
            PopHot ph; int pc;
            hitTestPopover(x, y, ph, pc);
            if (g_pop.mode == POP_STEP_NOTE) {
                int baseOct = channelBaseOct(g_pop.ch);
                if (ph == PHOT_KEY && pc >= 0) {
                    assignStepPitchClass(pc, !shift);
                } else if (ph == PHOT_OCT_DN) {
                    g_pop.oct = std::max(baseOct - 2, g_pop.oct - 1);
                } else if (ph == PHOT_OCT_UP) {
                    g_pop.oct = std::min(baseOct + 2, g_pop.oct + 1);
                } else if (ph == PHOT_ACC) {
                    pushUndo();
                    EnterCriticalSection(&g_cs);
                    PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[g_pop.ch];
                    tr.steps |= (1u << g_pop.step);
                    tr.accents ^= (1u << g_pop.step);
                    LeaveCriticalSection(&g_cs);
                    previewChannelStep(g_pop.ch, g_pop.step);
                } else if (ph == PHOT_CLEAR) {
                    pushUndo();
                    EnterCriticalSection(&g_cs);
                    PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[g_pop.ch];
                    tr.steps   &= ~(1u << g_pop.step);
                    tr.accents &= ~(1u << g_pop.step);
                    LeaveCriticalSection(&g_cs);
                    closePopover();
                }
            } else if (g_pop.mode == POP_NOTE_POOL) {
                if (ph == PHOT_KEY && pc >= 0) {
                    togglePoolNote(pc);
                } else if (ph == PHOT_P_MIN || ph == PHOT_P_MAJ || ph == PHOT_P_PNT || ph == PHOT_P_ALL) {
                    int sc = (ph == PHOT_P_MIN) ? SC_MINOR : (ph == PHOT_P_MAJ) ? SC_MAJOR : (ph == PHOT_P_PNT) ? SC_PENTA : SC_FREE;
                    pushUndo();
                    EnterCriticalSection(&g_cs);
                    g_proj.scale = sc;
                    g_proj.customMask = buildScaleMask(sc, g_proj.rootKey);
                    LeaveCriticalSection(&g_cs);
                } else if (ph == PHOT_DONE) {
                    closePopover();
                }
            }
            redraw();
            return 0;
        }

        Hot hot; int idx, step;
        hitTest(x, y, hot, idx, step);

        switch (hot) {
        case HOT_PLAY:   togglePlay(); break;
        case HOT_SAVE:   saveProject(); break;
        case HOT_OPEN:   openProject(); break;
        case HOT_EXPORT: exportWav(); break;
        case HOT_TEMPO:
            g_drag = DRAG_TEMPO; g_dragY0 = y; g_dragTempo0 = g_proj.tempo;
            SetCapture(hwnd); break;
        case HOT_SWING:
            g_drag = DRAG_SWING; g_dragY0 = y; g_dragSwing0 = g_proj.swing;
            SetCapture(hwnd); break;
        case HOT_PAT:
            EnterCriticalSection(&g_cs); g_proj.curPat = idx; LeaveCriticalSection(&g_cs);
            break;
        case HOT_SONG:
            EnterCriticalSection(&g_cs); g_proj.songMode = !g_proj.songMode; LeaveCriticalSection(&g_cs);
            setToast(g_proj.songMode ? L"Song Mode ON (chains A -> B -> C -> D)" : L"Pattern Loop Mode");
            break;
        case HOT_PRESET:
            pushUndo();
            g_presetIdx = (g_presetIdx + 1) % kNumPresets;
            EnterCriticalSection(&g_cs);
            loadPreset(g_proj, g_presetIdx);
            LeaveCriticalSection(&g_cs);
            {
                wchar_t b[64];
                swprintf(b, 64, L"Loaded preset: %s", kPresetNames[g_presetIdx]);
                setToast(b);
            }
            break;
        case HOT_CHANNEL:
            g_sel = idx;
            previewChannelStep(idx, 0);
            break;
        case HOT_RAIL_MUTE:
            EnterCriticalSection(&g_cs); g_proj.ch[idx].on = !g_proj.ch[idx].on; LeaveCriticalSection(&g_cs);
            break;
        case HOT_RAIL_SOLO:
            EnterCriticalSection(&g_cs); g_proj.ch[idx].solo = !g_proj.ch[idx].solo; LeaveCriticalSection(&g_cs);
            break;
        case HOT_STEP: {
            g_sel = idx;
            unsigned mask = 1u << step;
            EnterCriticalSection(&g_cs);
            PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[idx];
            bool wasOn = (tr.steps & mask) != 0;
            int origOff = tr.off[step];
            LeaveCriticalSection(&g_cs);

            if (alt) {
                pushUndo();
                EnterCriticalSection(&g_cs);
                g_proj.pat[g_proj.curPat].tr[idx].steps   |= mask;
                g_proj.pat[g_proj.curPat].tr[idx].accents ^= mask;
                LeaveCriticalSection(&g_cs);
                previewChannelStep(idx, step);
            } else if (!wasOn) {
                pushUndo();
                EnterCriticalSection(&g_cs);
                g_proj.pat[g_proj.curPat].tr[idx].steps |= mask;
                LeaveCriticalSection(&g_cs);
                g_drag = DRAG_PAINT_ON;
                SetCapture(hwnd);
                previewChannelStep(idx, step);
            } else if (!isDrum(idx)) {
                // Active melodic step: click toggles off, vertical drag glides pitch, horizontal drag paints off
                g_drag     = DRAG_STEP_MAYBE;
                g_dragX0   = x;
                g_dragY0   = y;
                g_dragCh   = idx;
                g_dragStep = step;
                g_dragOff0 = origOff;
                SetCapture(hwnd);
            } else {
                pushUndo();
                EnterCriticalSection(&g_cs);
                g_proj.pat[g_proj.curPat].tr[idx].steps   &= ~mask;
                g_proj.pat[g_proj.curPat].tr[idx].accents &= ~mask;
                LeaveCriticalSection(&g_cs);
                g_drag = DRAG_PAINT_OFF;
                SetCapture(hwnd);
            }
            break;
        }
        case HOT_VOL:    g_drag = DRAG_VOL;    applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_PAN:    g_drag = DRAG_PAN;    applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_TONE:   g_drag = DRAG_TONE;   applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_DECAY:  g_drag = DRAG_DECAY;  applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_MASTER: g_drag = DRAG_MASTER; applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_REVERB: g_drag = DRAG_REVERB; applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_DELAY:  g_drag = DRAG_DELAY;  applySliderDrag(g_drag, x); SetCapture(hwnd); break;
        case HOT_MUTE:
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].on = !g_proj.ch[g_sel].on; LeaveCriticalSection(&g_cs);
            break;
        case HOT_SOLO:
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].solo = !g_proj.ch[g_sel].solo; LeaveCriticalSection(&g_cs);
            break;
        case HOT_CLEAR_CH:
            pushUndo();
            EnterCriticalSection(&g_cs);
            memset(&g_proj.pat[g_proj.curPat].tr[g_sel], 0, sizeof(PatternTrack));
            LeaveCriticalSection(&g_cs);
            setToast(L"Cleared channel");
            break;
        case HOT_GROOVE_CH:
            generateSmartGroove(g_sel);
            break;
        case HOT_ROOT_KEY:
            EnterCriticalSection(&g_cs);
            g_proj.rootKey = (g_proj.rootKey + 1) % 12;
            if (g_proj.scale == SC_CUSTOM || g_proj.scale == SC_FREE) g_proj.scale = SC_MINOR;
            LeaveCriticalSection(&g_cs);
            {
                wchar_t b[64];
                swprintf(b, 64, L"Key set to %s %s", kNoteNames[g_proj.rootKey], kScaleName[g_proj.scale]);
                setToast(b);
            }
            break;
        case HOT_SCALE:
            EnterCriticalSection(&g_cs);
            g_proj.scale = (g_proj.scale + 1) % NSCALE;
            LeaveCriticalSection(&g_cs);
            {
                wchar_t b[64];
                if (g_proj.scale == SC_FREE) wcscpy(b, L"Scale: Free (all 12 notes unlocked)");
                else if (g_proj.scale == SC_CUSTOM) swprintf(b, 64, L"Scale: Custom (%d notes) - click Notes to edit", countMaskBits(effectiveNoteMask(g_proj)));
                else swprintf(b, 64, L"Scale: %s %s", kNoteNames[g_proj.rootKey], kScaleName[g_proj.scale]);
                setToast(b);
            }
            break;
        case HOT_NOTE_POOL:
            openNotePoolPopover();
            break;
        case HOT_COPY:
            EnterCriticalSection(&g_cs);
            g_clipboard = g_proj.pat[g_proj.curPat];
            g_hasClipboard = true;
            LeaveCriticalSection(&g_cs);
            setToast(L"Copied current pattern");
            break;
        case HOT_PASTE:
            if (g_hasClipboard) {
                pushUndo();
                EnterCriticalSection(&g_cs);
                g_proj.pat[g_proj.curPat] = g_clipboard;
                LeaveCriticalSection(&g_cs);
                setToast(L"Pasted pattern");
            }
            break;
        case HOT_UNDO:
            performUndo();
            break;
        default: break;
        }
        redraw();
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_drag == DRAG_STEP_MAYBE) {
            // Simple click without dragging -> toggle step off
            pushUndo();
            EnterCriticalSection(&g_cs);
            unsigned mask = 1u << g_dragStep;
            g_proj.pat[g_proj.curPat].tr[g_dragCh].steps   &= ~mask;
            g_proj.pat[g_proj.curPat].tr[g_dragCh].accents &= ~mask;
            LeaveCriticalSection(&g_cs);
            g_drag = DRAG_NONE;
            ReleaseCapture();
            redraw();
        } else if (g_drag != DRAG_NONE) {
            g_drag = DRAG_NONE;
            ReleaseCapture();
            redraw();
        }
        return 0;

    case WM_MBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_pop.open) { closePopover(); redraw(); return 0; }
        Hot hot; int idx, step;
        hitTest(x, y, hot, idx, step);
        if (hot == HOT_STEP) {
            pushUndo();
            g_sel = idx;
            unsigned mask = 1u << step;
            EnterCriticalSection(&g_cs);
            g_proj.pat[g_proj.curPat].tr[idx].steps   |= mask;
            g_proj.pat[g_proj.curPat].tr[idx].accents ^= mask;
            LeaveCriticalSection(&g_cs);
            previewChannelStep(idx, step);
            redraw();
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        if (g_pop.open) {
            if (inRect(g_pop.rect, x, y)) {
                PopHot ph; int pc;
                hitTestPopover(x, y, ph, pc);
                if (g_pop.mode == POP_STEP_NOTE && ph == PHOT_KEY && pc >= 0) {
                    // Right-click a piano key -> audition & assign without closing
                    assignStepPitchClass(pc, false);
                    redraw();
                }
                return 0;
            }
            closePopover();
        }

        Hot hot; int idx, step;
        hitTest(x, y, hot, idx, step);
        if (hot == HOT_STEP) {
            g_sel = idx;
            if (!isDrum(idx)) {
                openStepNotePopover(idx, step);
            } else {
                pushUndo();
                EnterCriticalSection(&g_cs);
                g_proj.pat[g_proj.curPat].tr[idx].steps   &= ~(1u << step);
                g_proj.pat[g_proj.curPat].tr[idx].accents &= ~(1u << step);
                LeaveCriticalSection(&g_cs);
            }
            redraw();
        } else if (hot == HOT_SCALE || hot == HOT_ROOT_KEY) {
            openNotePoolPopover();
            redraw();
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int dir = (delta > 0) ? 1 : -1;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);

        if (g_pop.open && g_pop.mode == POP_STEP_NOTE && inRect(g_pop.rect, pt.x, pt.y)) {
            int baseOct = channelBaseOct(g_pop.ch);
            g_pop.oct = std::clamp(g_pop.oct + dir, baseOct - 2, baseOct + 2);
            redraw();
            return 0;
        }

        Hot hot; int idx, step;
        hitTest(pt.x, pt.y, hot, idx, step);

        if (hot == HOT_TEMPO) {
            EnterCriticalSection(&g_cs);
            g_proj.tempo = std::clamp(g_proj.tempo + dir * 2, 40, 240);
            LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_SWING) {
            EnterCriticalSection(&g_cs);
            g_proj.swing = std::clamp(g_proj.swing + dir * 0.03f, 0.0f, 0.60f);
            LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_ROOT_KEY) {
            EnterCriticalSection(&g_cs);
            g_proj.rootKey = (g_proj.rootKey + (dir > 0 ? 1 : 11)) % 12;
            if (g_proj.scale == SC_CUSTOM || g_proj.scale == SC_FREE) g_proj.scale = SC_MINOR;
            LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_STEP) {
            DWORD now = GetTickCount();
            if (idx != g_lastWheelCh || step != g_lastWheelStep || (now - g_lastWheelTick) > 650) {
                pushUndo();
            }
            g_lastWheelCh = idx; g_lastWheelStep = step; g_lastWheelTick = now;

            if (ctrl) {
                unsigned mask = 1u << step;
                EnterCriticalSection(&g_cs);
                PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[idx];
                tr.steps |= mask;
                if (dir > 0) tr.accents |= mask;
                else         tr.accents &= ~mask;
                LeaveCriticalSection(&g_cs);
                previewChannelStep(idx, step);
            } else if (!isDrum(idx)) {
                EnterCriticalSection(&g_cs);
                PatternTrack& tr = g_proj.pat[g_proj.curPat].tr[idx];
                tr.steps |= (1u << step);
                tr.off[step] = (signed char)stepNoteOffset(g_proj, tr.off[step], dir, shift);
                LeaveCriticalSection(&g_cs);
                previewChannelStep(idx, step);
            }
        } else if (hot == HOT_VOL) {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].vol = std::clamp(g_proj.ch[g_sel].vol + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_PAN) {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].pan = std::clamp(g_proj.ch[g_sel].pan + dir * 0.1f, -1.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_TONE) {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].tone = std::clamp(g_proj.ch[g_sel].tone + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_DECAY) {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].decay = std::clamp(g_proj.ch[g_sel].decay + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_REVERB) {
            EnterCriticalSection(&g_cs); g_proj.reverbMix = std::clamp(g_proj.reverbMix + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_DELAY) {
            EnterCriticalSection(&g_cs); g_proj.delayMix = std::clamp(g_proj.delayMix + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        } else if (hot == HOT_MASTER) {
            EnterCriticalSection(&g_cs); g_proj.master = std::clamp(g_proj.master + dir * 0.05f, 0.0f, 1.0f); LeaveCriticalSection(&g_cs);
        }
        redraw();
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

        if (wp == VK_ESCAPE) {
            if (g_pop.open) { closePopover(); redraw(); return 0; }
            if (g_playing)  { togglePlay();   redraw(); return 0; }
            return 0;
        }

        // Keyboard piano input while Step Note Popover is open
        if (g_pop.open && g_pop.mode == POP_STEP_NOTE && !ctrl) {
            int baseOct = channelBaseOct(g_pop.ch);
            if (wp == VK_UP)   { g_pop.oct = std::min(baseOct + 2, g_pop.oct + 1); redraw(); return 0; }
            if (wp == VK_DOWN) { g_pop.oct = std::max(baseOct - 2, g_pop.oct - 1); redraw(); return 0; }
            static const WPARAM kPianoVK[12] = { 'A', 'W', 'S', 'E', 'D', 'F', 'T', 'G', 'Y', 'H', 'U', 'J' };
            for (int pc = 0; pc < 12; ++pc) {
                if (wp == kPianoVK[pc]) {
                    assignStepPitchClass(pc, false);
                    redraw();
                    return 0;
                }
            }
        }

        if (wp == VK_SPACE && !ctrl) { togglePlay(); redraw(); return 0; }
        if (ctrl && wp == 'S')       { saveProject(); return 0; }
        if (ctrl && wp == 'O')       { openProject(); redraw(); return 0; }
        if (ctrl && wp == 'E')       { exportWav(); return 0; }
        if (ctrl && wp == 'Z')       { performUndo(); redraw(); return 0; }
        if (ctrl && wp == 'C') {
            EnterCriticalSection(&g_cs);
            g_clipboard = g_proj.pat[g_proj.curPat]; g_hasClipboard = true;
            LeaveCriticalSection(&g_cs);
            setToast(L"Copied pattern"); redraw(); return 0;
        }
        if (ctrl && wp == 'V' && g_hasClipboard) {
            pushUndo();
            EnterCriticalSection(&g_cs);
            g_proj.pat[g_proj.curPat] = g_clipboard;
            LeaveCriticalSection(&g_cs);
            setToast(L"Pasted pattern"); redraw(); return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) {
            int step = shift ? 5 : 1;
            EnterCriticalSection(&g_cs);
            g_proj.tempo = std::clamp(g_proj.tempo + (wp == VK_UP ? step : -step), 40, 240);
            LeaveCriticalSection(&g_cs);
            redraw(); return 0;
        }
        if (wp >= '1' && wp <= '8') { g_sel = (int)(wp - '1'); redraw(); return 0; }
        if (wp >= VK_F1 && wp <= VK_F4) {
            EnterCriticalSection(&g_cs); g_proj.curPat = (int)(wp - VK_F1); LeaveCriticalSection(&g_cs);
            redraw(); return 0;
        }
        if (wp == VK_TAB) {
            EnterCriticalSection(&g_cs); g_proj.songMode = !g_proj.songMode; LeaveCriticalSection(&g_cs);
            redraw(); return 0;
        }
        if (!ctrl && wp == 'M') {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].on = !g_proj.ch[g_sel].on; LeaveCriticalSection(&g_cs);
            redraw(); return 0;
        }
        if (!ctrl && wp == 'S') {
            EnterCriticalSection(&g_cs); g_proj.ch[g_sel].solo = !g_proj.ch[g_sel].solo; LeaveCriticalSection(&g_cs);
            redraw(); return 0;
        }
        if (wp == VK_DELETE) {
            pushUndo();
            EnterCriticalSection(&g_cs);
            memset(&g_proj.pat[g_proj.curPat].tr[g_sel], 0, sizeof(PatternTrack));
            LeaveCriticalSection(&g_cs);
            setToast(L"Cleared channel"); redraw(); return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_playing = false;
        shutdownAudioEngine();
        if (g_font)      DeleteObject(g_font);
        if (g_fontBold)  DeleteObject(g_fontBold);
        if (g_fontBig)   DeleteObject(g_fontBig);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        if (g_fontTiny)  DeleteObject(g_fontTiny);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  Entry Point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    enableDpiAwareness();
    InitializeCriticalSection(&g_cs);
    loadPreset(g_proj, 0);

    if (cmdLine && cmdLine[0]) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc >= 2 && argv[1][0]) loadProjectFromPath(argv[1]);
            LocalFree(argv);
        }
    }

    initAudioEngine();

    g_font      = CreateFontW(-15, 0, 0, 0, FW_NORMAL,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold  = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBig   = CreateFontW(-23, 0, 0, 0, FW_BOLD,     0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontTiny  = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    HICON appIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    if (!appIcon) appIcon = LoadIconW(NULL, IDI_APPLICATION);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(C_BG);
    wc.hIcon         = appIcon;
    wc.hIconSm       = appIcon;
    wc.lpszClassName = L"RibbonStudioWindow";
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_main = CreateWindowExW(0, L"RibbonStudioWindow", L"Ribbon 2.1",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top,
                             NULL, NULL, inst, NULL);
    if (!g_main) {
        shutdownAudioEngine();
        DeleteCriticalSection(&g_cs);
        return 1;
    }

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(&g_cs);
    return (int)msg.wParam;
}
