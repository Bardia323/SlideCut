// SlideCut — minimal hardcut slideshow maker.
// Images -> draggable timeline -> mp4 (H.264/AAC, Instagram-ready) via ffmpeg.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <future>
#include <map>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <memory>
#include <string>
#include <sstream>
#include <functional>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"                // ImRect, BeginDragDropTargetCustom
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "stb_image.h"
#include "miniaudio.h"

// ---------------------------------------------------------------- utilities

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring BaseName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
static std::wstring DirName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}
static std::wstring StemOf(const std::wstring& p) {
    std::wstring b = BaseName(p);
    size_t d = b.find_last_of(L'.');
    return d == std::wstring::npos ? b : b.substr(0, d);
}
static std::wstring LowerExt(const std::wstring& p) {
    size_t d = p.find_last_of(L'.');
    size_t s = p.find_last_of(L"\\/");     // a dot inside a folder name is not an extension
    if (d == std::wstring::npos || (s != std::wstring::npos && s > d)) return {};
    std::wstring e = p.substr(d + 1);
    for (auto& ch : e) ch = (wchar_t)towlower(ch);
    return e;
}

// Trim a source name down to something safe for a filename: no reserved
// characters, no runs of separators, no trailing dots/spaces (Windows drops them).
static std::wstring SanitizeStem(std::wstring s, size_t maxLen = 48) {
    std::wstring out;
    for (wchar_t ch : s) {
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
        if ((ch == L'-' || ch == L' ' || ch == L'_') && !out.empty() &&
            (out.back() == L'-' || out.back() == L' ' || out.back() == L'_')) continue;
        out += ch;
    }
    while (!out.empty() && (out.front() == L' ' || out.front() == L'-')) out.erase(out.begin());
    if (out.size() > maxLen) out.resize(maxLen);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.' || out.back() == L'-'))
        out.pop_back();
    return out;
}

// Special Elite lives in assets/ next to the exe (or one/two levels up when running
// straight out of a build dir). Empty result = font missing; text cards fall back.
static const wchar_t* TITLE_FONT_FILE = L"SpecialElite-Regular.ttf";

static std::wstring AssetPath(const wchar_t* name) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const wchar_t* rel[] = { L"assets\\", L"..\\assets\\", L"..\\..\\assets\\" };
    for (const wchar_t* r : rel) {
        std::wstring p = dir + r + name;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

// ------------------------------------------------------------------- d3d11

static ID3D11Device*           g_d3dDevice = nullptr;
static ID3D11DeviceContext*    g_d3dContext = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static ID3D11ShaderResourceView* CreateTextureRGBA(const unsigned char* px, int w, int h) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { px, (UINT)(w * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_d3dDevice->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    g_d3dDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

// ----------------------------------------------------------------- project

struct LoadedImage {                       // produced on worker thread
    std::vector<unsigned char> px;         // RGBA, downscaled for display
    int w = 0, h = 0;                      // display size
    int srcW = 0, srcH = 0;                // original size (for export canvas)
    bool ok = false;
};

// Information gathered in the background about a clip's video file, plus the
// small low-fps jpeg ladder used for scrubbing instead of seeking the real file.
struct Song;

struct VideoSource {
    std::wstring path;
    std::wstring proxyDir;                 // holds %06d.jpg at PROXY_FPS
    double  duration = 0;                  // full source length, seconds
    int     w = 0, h = 0;                  // source pixel size
    double  fps = 0;                       // source frame rate
    double  startTime = 0;                 // container start time; -ss counts from it
    bool    hasAudio = false;
    std::atomic<bool> probed{ false };     // duration/size known
    std::atomic<bool> ready{ false };      // proxy extraction finished
    std::atomic<int>  framesOnDisk{ 0 };   // grows while extracting
    std::map<int, ID3D11ShaderResourceView*> cache;   // frame index -> texture
    std::atomic<int> cacheGen{ 0 };        // bumped when the cache is dropped
    int     lastIdx = 0;                   // last frame asked for, drawing thread only
    size_t  cacheBytes = 0;
    size_t  frameBytes = 0;                // uploaded size of one proxy frame
    std::atomic<bool> building{ false };
    float   aspect = 1.0f;
    // The clip's own audio, reduced to min/max pairs for the timeline strip. The
    // pairs live in the decoded Song itself - a second copy of them is megabytes
    // per hour of footage for nothing.
    std::atomic<bool>  apeaksReady{ false };
    double  apeakRate = 200.0;             // buckets per second
    std::shared_ptr<Song> audio;           // full audio for playback
};

static int g_uidNext = 1;                  // stable per-clip id, used for filter labels

// A colour grade, per shot and once more over the whole film. Contrast and
// saturation are stored as offsets from 1 so that a zeroed constant buffer — which
// is what every other pass hands the shader — means "leave the picture alone".
struct Grade {
    float bright = 0.0f;                   // -1 .. 1, added
    float contrast = 1.0f;                 // 0 .. 2, pivoted on mid grey
    float sat = 1.0f;                      // 0 .. 2, 0 is monochrome
    float temp = 0.0f;                     // -1 cool .. 1 warm
    bool  mono = false;                    // hard override of sat
    bool  On() const {
        return mono || fabsf(bright) > 1e-4f || fabsf(contrast - 1.0f) > 1e-4f
            || fabsf(sat - 1.0f) > 1e-4f || fabsf(temp) > 1e-4f;
    }
};

// How a clip fills a canvas its own aspect does not match. Per clip, base or layer.
enum { LFIT_INSIDE = 0, LFIT_FILL, LFIT_BLUR, LFIT_BLACK };

// Filling the canvas throws away whatever does not fit. This is which part of the
// clip is kept: a 3x3 grid read left to right, top row first, so 0 is the top-left
// corner and 4 is the middle.
enum { LANCHOR_CENTER = 4 };
static const char* LANCHOR_ITEMS[9] = {
    "top left", "top", "top right",
    "left", "centre", "right",
    "bottom left", "bottom", "bottom right" };
// 0 keeps the left/top edge, 0.5 the middle, 1 the right/bottom edge.
static void AnchorFrac(int a, float* ax, float* ay) {
    if (a < 0 || a > 8) a = LANCHOR_CENTER;
    *ax = (float)(a % 3) * 0.5f;
    *ay = (float)(a / 3) * 0.5f;
}
static const char* LFIT_ITEMS = "Inside\0" "Fill\0" "Blur bed\0" "Black bed\0";

// A clip can swap the projector for a screen of its own. LOOK_PROJECTOR keeps the
// film treatment; the tube looks bypass the plate entirely, since a television set
// has no gate to sit in.
enum { LOOK_PROJECTOR, LOOK_CRT, LOOK_CCTV, LOOK_CCTV_CRT };
// Not a look a clip can pick: LookAtTime's answer where nothing is on screen at all.
static const int LOOK_NONE = -1;
static const char* LOOK_ITEMS = "Projector\0CRT\0CCTV\0CCTV + CRT\0";

struct Clip {
    int          uid = g_uidNext++;
    enum Kind { Image, Text, Video, Nest } kind = Image;
    std::wstring path;
    std::string  label;                    // utf8 basename for UI
    double       duration = 3.0;           // seconds on the timeline
    int          srcW = 0, srcH = 0;
    ID3D11ShaderResourceView* tex = nullptr;
    float        texAspect = 1.0f;
    std::future<LoadedImage> pending;      // valid while decoding
    // text-card fields (kind == Text)
    std::string  text;                     // utf8, '\n' separated lines
    float        textScale = 0.13f;        // cap height as a fraction of frame height
    // video fields (kind == Video)
    std::shared_ptr<VideoSource> vid;
    double       trimIn = 0.0;             // seconds into the source
    bool         reversed = false;         // play this shot backwards
    int          group = 0;                // 0 = loose, else a group id
    int          nest = 0;                 // kind == Nest: the sequence it stands for
    bool         skip = false;             // muted: the film runs straight past it
    int          look = LOOK_PROJECTOR;    // which screen this shot plays on
    bool         look43 = false;           // crop that screen to real 4:3 glass
    // Export only, never saved: what a range render cut off the head and tail of a
    // sequence. It still renders whole on its own clock and is trimmed after, so a
    // screen's clock and everything inside stay where the preview has them.
    double       nestHead = 0.0, nestTail = 0.0;
    Grade        grade;                    // this shot's own colour
    double       xfade = 0.0;              // dissolve into the next shot, seconds
    bool         useAudio = true;          // mix this clip's own audio into the export
    float        volume = 1.0f;            // multiplier for the clip's own audio

    // ---- placement on an overlay track (ignored on the base track, which packs)
    double       start = 0.0;              // timeline seconds where this clip begins
    int          lblend = 0;               // index into LAYER_MODES
    float        lopacity = 1.0f;
    // How this clip meets a canvas its own aspect does not match - a base cut and an
    // overlay layer both use it. "Inside" leaves the gaps clear, so on a layer the cut
    // underneath shows through them and on a base cut the projector wall does. The
    // other three fill the gaps: by cropping the picture, or by putting an opaque bed
    // - blurred or black - behind it, which hides whatever is under without cropping
    // anything. Fill is the default: it covers, and nothing peeks out at the sides.
    int          lfit = LFIT_FILL;
    int          lanchor = LANCHOR_CENTER;  // which part survives a fill crop

    // ---- text overlay (any clip kind; Text cards use it for placement too)
    bool         ovlOn = false;
    std::string  ovlText;                  // utf8, '\n' separated
    float        ovlScale = 0.07f;         // cap height as a fraction of frame height
    float        ovlX = 0.5f, ovlY = 0.5f; // normalised centre of the text block
    float        ovlCol[3] = { 1.0f, 1.0f, 1.0f };
    float        ovlAlpha = 1.0f;
    bool         ovlShadow = true;

    // ---- double exposure: a second source blended over this clip
    bool         dxOn = false;
    std::wstring dxPath;
    std::string  dxLabel;
    bool         dxIsVideo = false;
    int          dxBlend = 0;              // index into BLEND_MODES
    float        dxAmount = 0.5f;
    double       dxTrimIn = 0.0;
    std::shared_ptr<VideoSource> dxVid;    // when dxIsVideo
    ID3D11ShaderResourceView* dxTex = nullptr;   // still preview texture
    float        dxAspect = 1.0f;
    std::future<LoadedImage> dxPending;
};

// ffmpeg blend=all_mode names, in UI order.
static const char* BLEND_MODES[] = { "screen", "lighten", "overlay", "multiply",
                                     "softlight", "difference", "addition", "darken" };
// Overlay-track compositing modes. "normal" is plain alpha, the rest are the same
// ffmpeg blend modes the double exposure uses.
static const wchar_t* LAYER_MODES_W[] = { L"normal", L"screen", L"lighten", L"overlay",
                                          L"multiply", L"softlight", L"difference",
                                          L"addition", L"darken" };
static const char* LAYER_ITEMS =
    "Normal\0" "Screen\0" "Lighten\0" "Overlay\0" "Multiply\0" "Soft light\0"
    "Difference\0" "Addition\0" "Darken\0";

static const wchar_t* BLEND_MODES_W[] = { L"screen", L"lighten", L"overlay", L"multiply",
                                          L"softlight", L"difference", L"addition", L"darken" };
static const char* BLEND_ITEMS =
    "Screen\0" "Lighten\0" "Overlay\0" "Multiply\0"
    "Soft light\0" "Difference\0" "Addition\0" "Darken\0";

struct Song {
    int     uid = g_uidNext++;
    int     group = 0;
    std::wstring path;
    std::string  label;
    double  offset = 0.0;                  // timeline position of the block start (can be < 0)
    double  duration = 0.0;
    double  trimStart = 0.0;               // seconds into the source where the block begins
    double  trimEnd = 0.0;                 // seconds into the source where the block ends
    std::vector<float> pcm;                // interleaved stereo f32 @ 48k
    std::vector<float> peaks;              // min,max pairs per bucket
    int     framesPerPeak = 1024;
    bool    loaded = false;
    bool    reversed = false;
    int     fx = 0;                        // the block's own chain (AFX_*), under the track's
    float   fxMix = 1.0f;
    // A texture block has no file. 1 = noise bed: its chain run on silence, heard on
    // its own track. 2 = chain region: while it plays, its chain runs over `target`.
    int     gen = 0;
    int     target = -1;                   // chain region: the audio track it runs over
    int     tex = 0;                       // noise bed: which texture it plays (TEX_*)
    float   volume = 1.0f;                 // the block's own level, after its chain, under the track's
    float   fadeIn = 0.0f;                 // seconds of ramp up from the block's start
    float   fadeOut = 0.0f;                // seconds of ramp down into the block's end
};

// A block's fade level at timeline time t. Linear ramps that begin no earlier than
// timeline 0, so the export's afade (which cannot start partway up) matches.
static float SongFadeGain(const Song& s, double t) {
    double len = s.trimEnd - s.trimStart, g = 1.0;
    if (s.fadeIn > 0.001f) {
        double a = s.offset > 0 ? s.offset : 0.0, b = s.offset + s.fadeIn;
        if (t < b && b > a) g = std::min(g, (t - a) / (b - a));
    }
    if (s.fadeOut > 0.001f) {
        double e = s.offset + len, a = std::max(e - s.fadeOut, 0.0);
        if (t > a && e > a) g = std::min(g, (e - t) / (e - a));
    }
    return g < 0 ? 0.0f : (float)g;
}
// A texture block's source is endless; this is how far its out-point can be pulled.
static const double GEN_MAX = 3600.0;

static const int SAMPLE_RATE = 48000;

// ------------------------------------------------------------------ audio fx
// A chain a track can be run through: a cinematic dialogue polish (shape, a
// squeeze, a touch of room), a telephone futz (band-limited, mono, driven) and
// an AM radio (wider band than the phone, hissing, broadcast-squashed,
// drifting).
// The preview mixer runs it sample by sample and the export asks ffmpeg for the
// same shape, so what you hear is what lands in the file.
enum { AFX_NONE = 0, AFX_CINE, AFX_PHONE, AFX_PHONE_CINE, AFX_AM, AFX_AM_CINE,
       AFX_COUNT };
static const char* kAfxNames[AFX_COUNT] = { "none", "cinematic", "telephone",
                                            "cinematic + telephone", "am radio",
                                            "cinematic + am radio" };

// RBJ biquad, transposed direct form II, two channels of state.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = { 0, 0 }, z2[2] = { 0, 0 };
    void Set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
    void Reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    inline float Run(int ch, float x) {
        float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void HighPass(float f, float q) {
        float w = 6.2831853f * f / SAMPLE_RATE, c = cosf(w), s = sinf(w);
        float al = s / (2 * q);
        Set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void LowPass(float f, float q) {
        float w = 6.2831853f * f / SAMPLE_RATE, c = cosf(w), s = sinf(w);
        float al = s / (2 * q);
        Set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void Peak(float f, float q, float gainDb) {
        float A = powf(10.0f, gainDb / 40.0f);
        float w = 6.2831853f * f / SAMPLE_RATE, c = cosf(w), s = sinf(w);
        float al = s / (2 * q);
        Set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void Shelf(float f, float gainDb, bool high) {
        float A = powf(10.0f, gainDb / 40.0f);
        float w = 6.2831853f * f / SAMPLE_RATE, c = cosf(w), s = sinf(w);
        float al = s / 2 * sqrtf((A + 1 / A) * (1 / 0.9f - 1) + 2);
        float sa = 2 * sqrtf(A) * al;
        if (high)
            Set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c),
                A * ((A + 1) + (A - 1) * c - sa),
                (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c),
                (A + 1) - (A - 1) * c - sa);
        else
            Set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c),
                A * ((A + 1) - (A - 1) * c - sa),
                (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c),
                (A + 1) + (A - 1) * c - sa);
    }
};

// Everything the chain remembers between callbacks. Fixed size: the audio thread
// never allocates.
static const int AFX_DELAY = 4800;         // 100 ms of room, per channel
static const float AM_HISS = 0.004f;       // am noise floor, pre-band, pre-squeeze
struct AudioFxState {
    Biquad hp1, hp2, lp1, lp2, mid, bass, mud, air;
    Biquad ahp1, ahp2, alp1, alp2, apk;    // am radio band
    float env[2] = { 0, 0 };               // compressor followers, linear
    float dl[2][AFX_DELAY] = {};
    int   dw = 0;
    double lfo = 0;                        // am wobble phase, in samples
    unsigned rng = 0x2545F491u;            // am hiss, xorshift on the audio thread

    // White, uniform, +-1. Cheap enough to call once a frame.
    inline float Noise() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (float)(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    int   built = -1;                      // which preset the state was built for

    void Build(int preset) {
        if (built == preset) return;
        built = preset;
        // telephone: a narrow band with a presence bump where a handset lives
        hp1.HighPass(300.0f, 0.707f);  hp2.HighPass(300.0f, 0.707f);
        lp1.LowPass(3400.0f, 0.707f);  lp2.LowPass(3400.0f, 0.707f);
        mid.Peak(1800.0f, 1.2f, 6.0f);
        // cinematic: weight underneath, the boxy mids out, the top open
        bass.Shelf(110.0f, 2.5f, false);
        mud.Peak(400.0f, 1.2f, -3.0f);
        air.Shelf(8000.0f, 3.0f, true);
        // am radio: a wider band than the handset, honking in the middle
        ahp1.HighPass(200.0f, 0.707f); ahp2.HighPass(200.0f, 0.707f);
        alp1.LowPass(4500.0f, 0.707f); alp2.LowPass(4500.0f, 0.707f);
        apk.Peak(1500.0f, 1.0f, 5.0f);
        hp1.Reset(); hp2.Reset(); lp1.Reset(); lp2.Reset();
        mid.Reset(); bass.Reset(); mud.Reset(); air.Reset();
        ahp1.Reset(); ahp2.Reset(); alp1.Reset(); alp2.Reset(); apk.Reset();
        lfo = 0;
        rng = 0x2545F491u;
        env[0] = env[1] = 0;
        memset(dl, 0, sizeof(dl));
        dw = 0;
    }

    // One peak-following compressor both flavours share: cinematic breathes
    // slowly, the phone squeezes hard and fast.
    inline float Squeeze(int ch, float x, float thr, float ratio,
                         float atkMs, float relMs) {
        float a = expf(-1.0f / (atkMs * 0.001f * SAMPLE_RATE));
        float r = expf(-1.0f / (relMs * 0.001f * SAMPLE_RATE));
        float mag = fabsf(x);
        float& e = env[ch];
        e = mag > e ? a * e + (1 - a) * mag : r * e + (1 - r) * mag;
        if (e <= thr) return x;
        return x * powf(e / thr, 1.0f / ratio - 1.0f);
    }

    void Process(float* buf, ma_uint32 frames, int preset, float mix) {
        if (preset <= AFX_NONE || preset >= AFX_COUNT || mix <= 0.0001f) return;
        Build(preset);
        bool cine  = preset == AFX_CINE  || preset == AFX_PHONE_CINE ||
                     preset == AFX_AM_CINE;
        bool phone = preset == AFX_PHONE || preset == AFX_PHONE_CINE;
        bool am    = preset == AFX_AM    || preset == AFX_AM_CINE;
        for (ma_uint32 i = 0; i < frames; i++) {
            float dry[2] = { buf[i * 2], buf[i * 2 + 1] };
            float w[2] = { dry[0], dry[1] };
            if (phone || am) w[0] = w[1] = 0.5f * (dry[0] + dry[1]);   // one capsule
            float amGain = 1.0f;
            if (am) {                      // matches ffmpeg tremolo's cosine
                float ph = 6.2831853f * 0.7f * (float)(lfo / SAMPLE_RATE);
                amGain = 1.0f - 0.12f + 0.12f * 0.5f * (cosf(ph) + 1.0f);
                lfo += 1.0;
                if (lfo >= (double)SAMPLE_RATE) lfo -= (double)SAMPLE_RATE;
            }
            float hiss = am ? Noise() * AM_HISS : 0.0f;   // ahead of the band
            for (int ch = 0; ch < 2; ch++) {
                float x = w[ch];
                if (cine) {
                    x = bass.Run(ch, x);
                    x = mud.Run(ch, x);
                    x = air.Run(ch, x);
                    x = Squeeze(ch, x, 0.09f, 3.0f, 20.0f, 250.0f) * 1.6f;
                }
                if (phone) {
                    x = hp1.Run(ch, x); x = hp2.Run(ch, x);
                    x = lp1.Run(ch, x); x = lp2.Run(ch, x);
                    x = mid.Run(ch, x);
                    x = Squeeze(ch, x, 0.05f, 6.0f, 5.0f, 80.0f) * 2.0f;
                    x = tanhf(x * 1.8f) * 0.7f;     // the line itself, driven
                }
                if (am) {
                    x += hiss;             // the carrier's own floor
                    x = ahp1.Run(ch, x); x = ahp2.Run(ch, x);
                    x = alp1.Run(ch, x); x = alp2.Run(ch, x);
                    x = apk.Run(ch, x);
                    x = Squeeze(ch, x, 0.04f, 8.0f, 3.0f, 120.0f) * 2.2f;
                    x = tanhf(x * 2.2f) * 0.65f;    // the transmitter, pushed
                    x *= amGain;                    // the carrier, drifting
                }
                w[ch] = x;
            }
            if (cine) {                    // a short room, three taps, barely there
                const int taps[3] = { 1200, 1920, 2880 };        // 25 / 40 / 60 ms
                const float lvl[3] = { 0.20f, 0.13f, 0.08f };
                float pre[2] = { w[0], w[1] };
                for (int ch = 0; ch < 2; ch++) {
                    float wet = 0;
                    for (int t = 0; t < 3; t++) {
                        int idx = dw - taps[t] - (ch ? 90 : 0);  // right sits later
                        while (idx < 0) idx += AFX_DELAY;
                        wet += dl[ch][idx] * lvl[t];
                    }
                    w[ch] += wet;
                }
                dl[0][dw] = pre[0];
                dl[1][dw] = pre[1];
                dw = (dw + 1) % AFX_DELAY;
            }
            buf[i * 2]     = dry[0] * (1 - mix) + w[0] * mix;
            buf[i * 2 + 1] = dry[1] * (1 - mix) + w[1] * mix;
        }
    }
};

// The same chain as an ffmpeg filter string, no labels, trailing comma dropped.
// Empty when the track is dry.
static std::wstring AfxChain(int preset) {
    if (preset <= AFX_NONE || preset >= AFX_COUNT) return L"";
    bool cine  = preset == AFX_CINE  || preset == AFX_PHONE_CINE ||
                 preset == AFX_AM_CINE;
    bool phone = preset == AFX_PHONE || preset == AFX_PHONE_CINE;
    bool am    = preset == AFX_AM    || preset == AFX_AM_CINE;
    std::wstring f;
    if (cine)
        f += L"bass=g=2.5:f=110,equalizer=f=400:t=q:w=1.2:g=-3,treble=g=3:f=8000,"
             L"acompressor=threshold=0.09:ratio=3:attack=20:release=250:makeup=1.6,"
             L"aecho=1:0.85:25|40|60:0.20|0.13|0.08,";
    if (phone)
        f += L"aformat=channel_layouts=mono,"
             L"highpass=f=300:poles=2,lowpass=f=3400:poles=2,"
             L"equalizer=f=1800:t=q:w=1.2:g=6,"
             L"acompressor=threshold=0.05:ratio=6:attack=5:release=80:makeup=2,"
             L"volume=1.8,asoftclip=type=tanh,volume=0.7,"
             L"aformat=channel_layouts=stereo,";
    if (am)
        f += L"aformat=channel_layouts=mono,"
             // one expression, so no c=same: ffmpeg 8.1 crashes on a c=same
             // aeval followed by a channel-layout change
             L"aeval=val(0)+0.004*(random(1)*2-1),"
             L"highpass=f=200:poles=2,lowpass=f=4500:poles=2,"
             L"equalizer=f=1500:t=q:w=1:g=5,"
             L"acompressor=threshold=0.04:ratio=8:attack=3:release=120:makeup=2.2,"
             L"volume=2.2,asoftclip=type=tanh,volume=0.65,"
             L"tremolo=f=0.7:d=0.12,"
             L"aformat=channel_layouts=stereo,";
    if (!f.empty()) f.pop_back();
    return f;
}

// ---- textures
// What a noise bed plays: a sound made from nothing, not a chain run over audio.
// The preview generates it here; the export asks ffmpeg for the same recipe, on a
// mono line that is widened to stereo, so what you hear is what lands in the file.
enum { TEX_AM = 0, TEX_TAPE, TEX_ROOM, TEX_HUM, TEX_LINE, TEX_CRACKLE, TEX_CRT, TEX_COUNT };
static const char* kTexNames[TEX_COUNT] = { "am radio hiss", "tape hiss", "room tone",
                                            "mains hum", "phone line", "vinyl crackle",
                                            "crt tv static" };

struct TextureState {
    Biquad a, b, c;
    AudioFxState am;                       // the am radio hiss is that chain on silence
    unsigned rng = 0x9E3779B9u;
    double   n = 0;                        // samples into the current second
    int      built = -1;
    inline float N() {                     // white, uniform, +-1
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (float)(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    void Build(int k) {
        if (built == k) return;
        built = k;
        switch (k) {
        case TEX_TAPE:    a.HighPass(3000.0f, 0.707f); b.LowPass(12000.0f, 0.707f); break;
        case TEX_ROOM:    a.LowPass(150.0f, 0.707f); b.LowPass(150.0f, 0.707f);
                          c.LowPass(400.0f, 0.707f); break;
        case TEX_LINE:    a.HighPass(300.0f, 0.707f); b.LowPass(3400.0f, 0.707f); break;
        case TEX_CRACKLE: a.LowPass(6000.0f, 0.707f); break;
        case TEX_CRT:     a.HighPass(200.0f, 0.707f); b.LowPass(9000.0f, 0.707f); break;
        default: break;
        }
    }
    void Render(float* buf, ma_uint32 frames, int kind, float level) {
        if (kind < 0 || kind >= TEX_COUNT || level <= 0.0001f) return;
        Build(kind);
        if (kind == TEX_AM) { am.Process(buf, frames, AFX_AM, level); return; }
        const double TAU = 6.283185307179586;
        for (ma_uint32 i = 0; i < frames; i++) {
            const double t = n / SAMPLE_RATE;
            float x = 0.0f;
            switch (kind) {
            case TEX_TAPE:
                x = b.Run(0, a.Run(0, 0.02f * N()));
                break;
            case TEX_ROOM:
                x = c.Run(0, b.Run(0, a.Run(0, 0.03f * N())));
                break;
            case TEX_HUM:
                x = (float)(0.02 * sin(TAU * 50 * t) + 0.01 * sin(TAU * 150 * t) +
                            0.005 * sin(TAU * 250 * t));
                break;
            case TEX_LINE:
                x = b.Run(0, a.Run(0, 0.03f * N())) + (float)(0.006 * sin(TAU * 50 * t));
                break;
            case TEX_CRACKLE: {
                float u = 0.5f * (N() + 1.0f);
                float click = u < 0.0008f ? 0.4f * N() : 0.0f;
                x = a.Run(0, click + 0.006f * N());
                break;
            }
            case TEX_CRT:                  // snow buzzing at twice mains, flyback whine, mains
                x = b.Run(0, a.Run(0, 0.008f * N()));
                x = (float)(x * (0.85 + 0.15 * sin(TAU * 100 * t)) +
                            0.004 * sin(TAU * 15625 * t) + 0.006 * sin(TAU * 50 * t));
                break;
            }
            x *= level;
            buf[i * 2 + 0] += x;
            buf[i * 2 + 1] += x;
            n += 1.0;
            if (n >= SAMPLE_RATE) n -= SAMPLE_RATE;   // every tone here repeats each second
        }
    }
};

// The export's recipe for a texture, applied to a stretch of silence. No commas
// inside expressions: they would split the filter graph.
static std::wstring TexFilter(int kind) {
    switch (kind) {
    case TEX_AM:   return AfxChain(AFX_AM);
    case TEX_TAPE: return L"aformat=channel_layouts=mono,aeval=0.02*(random(0)*2-1),"
                          L"highpass=f=3000,lowpass=f=12000,aformat=channel_layouts=stereo";
    case TEX_ROOM: return L"aformat=channel_layouts=mono,aeval=0.03*(random(0)*2-1),"
                          L"lowpass=f=150,lowpass=f=150,lowpass=f=400,"
                          L"aformat=channel_layouts=stereo";
    case TEX_HUM:  return L"aformat=channel_layouts=mono,"
                          L"aeval=0.02*sin(2*PI*50*t)+0.01*sin(2*PI*150*t)+0.005*sin(2*PI*250*t),"
                          L"aformat=channel_layouts=stereo";
    case TEX_LINE: return L"aformat=channel_layouts=mono,aeval=0.03*(random(0)*2-1),"
                          L"highpass=f=300,lowpass=f=3400,aeval=val(0)+0.006*sin(2*PI*50*t),"
                          L"aformat=channel_layouts=stereo";
    case TEX_CRACKLE: return L"aformat=channel_layouts=mono,"
                             L"aeval=0.4*(random(1)*2-1)*floor(random(0)+0.0008)+0.006*(random(2)*2-1),"
                             L"lowpass=f=6000,aformat=channel_layouts=stereo";
    case TEX_CRT:  return L"aformat=channel_layouts=mono,aeval=0.008*(random(0)*2-1),"
                          L"highpass=f=200,lowpass=f=9000,"
                          L"aeval=val(0)*(0.85+0.15*sin(2*PI*100*t))"
                          L"+0.004*sin(2*PI*15625*t)+0.006*sin(2*PI*50*t),"
                          L"aformat=channel_layouts=stereo";
    default: return L"";
    }
}


// ---- tracks
// Video track 0 is the base cut: its clips are packed end to end and their order
// is the film. Every track above it is an overlay — clips sit at a free position
// (Clip::start) and are composited over whatever the base is showing.
struct VideoTrack {
    std::string name;
    bool visible = true;
    float height = 58.0f;                  // row height, dragged from the header
    std::vector<std::unique_ptr<Clip>> clips;
};
// Audio tracks are free-form too: any number of blocks, each with its own offset.
struct AudioTrack {
    std::string name;
    float volume = 1.0f;
    bool  mute = false;
    int   fx = AFX_NONE;                   // chain the whole track runs through
    float fxMix = 1.0f;                    // how much of it survives, 0 = dry
    AudioFxState fxs;                      // live filter state, never saved
    float height = 44.0f;                  // row height, dragged from the header
    std::vector<std::unique_ptr<Song>> blocks;
};

static Grade g_grade;                      // the whole film's colour, over the top

static float g_baseH = 58.0f;              // height of the base picture row
// Height of the whole timeline pane. 0 = size it to the tracks; dragging the bar
// above the toolbar pins it, double-clicking the bar hands it back to the tracks.
static float g_tlHeightUser = 0.0f;
static float g_panelWUser = 0.0f;   // side panel width, once dragged
static const float ROW_H_MIN = 30.0f, ROW_H_MAX = 260.0f;
// Muted shots get their own row under the picture, at their real length, so a
// mute reads as "parked off the cut" instead of shrinking to nothing.
static const float MUTE_ROW_H = 34.0f;

static std::vector<std::unique_ptr<Clip>> g_clips;          // base video track
static std::vector<std::unique_ptr<VideoTrack>> g_over;     // overlay video tracks
static std::vector<std::unique_ptr<AudioTrack>> g_atracks;

// Selection: track -1 = base video, 0..n = overlay track, -2 = an audio block.
static int g_sel = -1;                     // clip index inside the selected track
static int g_selTrack = -1;
static int g_selAT = -1;                   // audio track, when g_selTrack == -2

// Multi-selection, by clip uid so it survives reorders. g_sel / g_selTrack stay the
// primary pick (what the SHOT panel edits); this is what a drag or delete acts on.
static std::vector<int> g_selUids;
static int g_groupNext = 1;                // next sequence id to hand out

static bool SelHas(int uid) {
    for (int u : g_selUids) if (u == uid) return true;
    return false;
}
// Everything sharing a clip's group id, so picking one shot picks the sequence.
static void SelAddGroupOf(const Clip& c);

static void SelSet(int uid) { g_selUids.assign(1, uid); }
static void SelToggle(int uid) {
    for (size_t i = 0; i < g_selUids.size(); i++)
        if (g_selUids[i] == uid) { g_selUids.erase(g_selUids.begin() + i); return; }
    g_selUids.push_back(uid);
}

// A property set in the SHOT panel is set on the whole selection: the panel edits
// the primary pick, and this pushes that same change onto every other selected
// shot, on the base track or on any layer.
template <class F>
static void ForEachOtherSelected(const Clip& primary, F fn) {
    auto run = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& o : v) if (o.get() != &primary && SelHas(o->uid)) fn(*o);
    };
    run(g_clips);
    for (auto& t : g_over) run(t->clips);
}

template <class F>
static void ForEachOtherSelectedSong(const Song& primary, F fn) {
    auto run = [&](std::vector<std::unique_ptr<Song>>& v) {
        for (auto& o : v) if (o.get() != &primary && SelHas(o->uid)) fn(*o);
    };
    for (auto& t : g_atracks) run(t->blocks);
}

// How many shots/blocks a panel edit would land on, the primary included.
static int SelCount() {
    int n = 0;
    auto run = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& o : v) if (SelHas(o->uid)) n++;
    };
    run(g_clips);
    for (auto& t : g_over) run(t->clips);
    
    auto runS = [&](std::vector<std::unique_ptr<Song>>& v) {
        for (auto& o : v) if (SelHas(o->uid)) n++;
    };
    for (auto& t : g_atracks) runS(t->blocks);
    
    return n;
}

// ---- hypercut: two or more overlapping shots on different tracks, chopped so
// the stretch they share alternates between them on a fixed beat. Only overlay
// shots are cut; the base picture is never touched, it simply shows through the
// holes, so a base shot can take part without the film re-packing. The preview
// is a real edit made against a snapshot of the project text: changing the beat
// puts the snapshot back and chops again, so undo only ever sees the commit.
struct HcPart {
    int         track = -1;                // -1 base picture, else overlay index
    int         index = 0;                 // position in that track
    std::string label;
};
static std::vector<HcPart> g_hcParts;
static bool        g_hcLive = false;       // a preview is sitting on the timeline
static std::string g_hcSnap;               // the project text from before it
static float       g_hcSlice = 0.25f;      // seconds per slice, when beating in time
static bool        g_hcByFrames = false;   // beat in frames of the export rate instead
static int         g_hcFrames = 4;         // frames per slice, when beating in frames
static int         g_hcFirst = 0;          // which participant opens the run
static double      g_hcA = 0.0, g_hcB = 0.0;   // the shared stretch
static int         g_hcSlices = 0;
static std::string g_hcMsg;
static int         g_hcReq = 0;            // 1 arm, 2 re-chop, 3 commit, 4 drop

// A point on the aspect track. It is a point, not a span: the plate aspect it
// names holds from its offset until the next point. Nothing before the first
// point, which leaves the plate on the projector panel's own value.
struct AspectPoint {
    int    uid = g_uidNext++;
    double offset = 0.0;
    float  aspect = 1.777f;
};
static std::vector<std::unique_ptr<AspectPoint>> g_aspects;
static bool  g_aspectsVisible = true;
// Monitor-only cut of the base track: the picture row is parked so overlay tracks
// can be worked on without the film underneath bleeding through between their
// clips. Preview and preview sound only — the export still holds the whole film.
static bool  g_baseOff = false;
static const float ASPECT_ROW_H = 22.0f;

// Last frame's timeline layout, so a shell drop can tell which track it landed on.
// (SelAddGroupOf is defined once the track lists exist.)
struct TlGeom {
    struct Row { float y0, y1; int kind; int idx; };   // kind 0 base, 1 overlay, 2 audio
    bool  valid = false;
    float trackX = 0, pps = 60, scroll = 0;
    std::vector<Row> rows;
} g_geom;

// Shift-click means "everything between these two", and between is a matter of
// where they sit on the timeline. A track's vector is in the order clips were
// added to it, which is not that order once anything has been dragged about, so
// the run is worked out from the times.
template <class V, class Pos>
static void SelectRunBetween(V& v, int a, int b, Pos pos) {
    if (a < 0 || b < 0 || a >= (int)v.size() || b >= (int)v.size()) return;
    double lo = pos(*v[a]), hi = pos(*v[b]);
    if (lo > hi) { double t = lo; lo = hi; hi = t; }
    for (int i = 0; i < (int)v.size(); i++) {
        double at = pos(*v[i]);
        if (at >= lo - 1e-9 && at <= hi + 1e-9 && !SelHas(v[i]->uid))
            g_selUids.push_back(v[i]->uid);
    }
}

// A grouped shot never travels alone: touching one pulls in every sibling.
static void SelAddGroupOf(const Clip& c) {
    if (!c.group) return;
    auto scan = [&](const std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& o : v)
            if (o->group == c.group && !SelHas(o->uid)) g_selUids.push_back(o->uid);
    };
    scan(g_clips);
    for (auto& t : g_over) scan(t->clips);
    auto scanS = [&](const std::vector<std::unique_ptr<Song>>& v) {
        for (auto& o : v)
            if (o->group == c.group && !SelHas(o->uid)) g_selUids.push_back(o->uid);
    };
    for (auto& t : g_atracks) scanS(t->blocks);
}

static void SelAddGroupOf(const Song& c) {
    if (!c.group) return;
    auto scan = [&](const std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& o : v)
            if (o->group == c.group && !SelHas(o->uid)) g_selUids.push_back(o->uid);
    };
    scan(g_clips);
    for (auto& t : g_over) scan(t->clips);
    auto scanS = [&](const std::vector<std::unique_ptr<Song>>& v) {
        for (auto& o : v)
            if (o->group == c.group && !SelHas(o->uid)) g_selUids.push_back(o->uid);
    };
    for (auto& t : g_atracks) scanS(t->blocks);
}

static Clip* ClipByUid(int uid) {
    for (auto& c : g_clips) if (c->uid == uid) return c.get();
    for (auto& t : g_over)
        for (auto& c : t->clips) if (c->uid == uid) return c.get();
    return nullptr;
}

static Song* SongByUid(int uid) {
    for (auto& t : g_atracks)
        for (auto& s : t->blocks) if (s->uid == uid) return s.get();
    return nullptr;
}

// ---- one selection, one highlight
// g_sel / g_selTrack / g_selAT name the primary pick, the one the SHOT panel edits;
// g_selUids is what a drag, a delete or a panel edit lands on. Anything drawn with
// a bright border is a member of g_selUids and nothing else, so the two can never
// name different shots. SelSync runs once a frame and reconciles them: dead uids
// go, a primary that was moved by code alone (a fold, a drag onto another track,
// "to picture") pulls the selection onto itself, and a selection with no live
// primary hands the primary to its first member.
static int SelPrimaryUid() {
    if (g_selTrack == -1) {
        if (g_sel >= 0 && g_sel < (int)g_clips.size()) return g_clips[g_sel]->uid;
    } else if (g_selTrack >= 0) {
        if (g_selTrack < (int)g_over.size()) {
            auto& v = g_over[g_selTrack]->clips;
            if (g_sel >= 0 && g_sel < (int)v.size()) return v[g_sel]->uid;
        }
    } else if (g_selTrack == -2) {
        if (g_selAT >= 0 && g_selAT < (int)g_atracks.size()) {
            auto& b = g_atracks[g_selAT]->blocks;
            if (g_sel >= 0 && g_sel < (int)b.size()) return b[g_sel]->uid;
        }
    } else if (g_selTrack == -3) {
        if (g_sel >= 0 && g_sel < (int)g_aspects.size()) return g_aspects[g_sel]->uid;
    }
    return -1;
}

// Point the primary at whatever track and index a uid currently sits at.
static bool SelPrimaryTo(int uid) {
    for (int i = 0; i < (int)g_clips.size(); i++)
        if (g_clips[i]->uid == uid) { g_sel = i; g_selTrack = -1; return true; }
    for (int t = 0; t < (int)g_over.size(); t++) {
        auto& v = g_over[t]->clips;
        for (int i = 0; i < (int)v.size(); i++)
            if (v[i]->uid == uid) { g_sel = i; g_selTrack = t; return true; }
    }
    for (int t = 0; t < (int)g_atracks.size(); t++) {
        auto& b = g_atracks[t]->blocks;
        for (int i = 0; i < (int)b.size(); i++)
            if (b[i]->uid == uid) { g_sel = i; g_selTrack = -2; g_selAT = t; return true; }
    }
    for (int i = 0; i < (int)g_aspects.size(); i++)
        if (g_aspects[i]->uid == uid) { g_sel = i; g_selTrack = -3; return true; }
    return false;
}

static void SelSync() {
    for (size_t i = 0; i < g_selUids.size();) {
        int u = g_selUids[i];
        bool alive = ClipByUid(u) || SongByUid(u);
        if (!alive)
            for (auto& a : g_aspects) if (a->uid == u) { alive = true; break; }
        if (alive) i++;
        else g_selUids.erase(g_selUids.begin() + i);
    }
    int pu = SelPrimaryUid();
    if (pu >= 0) {
        if (!SelHas(pu)) {                       // primary moved on its own: it wins
            SelSet(pu);
            if (Clip* c = ClipByUid(pu))      SelAddGroupOf(*c);
            else if (Song* s = SongByUid(pu)) SelAddGroupOf(*s);
        }
    } else if (!g_selUids.empty()) {
        if (!SelPrimaryTo(g_selUids.front())) { g_sel = -1; g_selTrack = -1; g_selAT = -1; }
    } else {
        g_sel = -1; g_selTrack = -1; g_selAT = -1;
    }
}

// ImGui asserts (font_size > 0) rather than clamping, so a pane squeezed to zero
// height or a clip narrower than its own label would abort the process. Every
// computed size passes through here.
static float SafePx(float px) { return px < 1.0f ? 1.0f : (px > 4096.0f ? 4096.0f : px); }

static const char* LAYER_MODE_NAME(int i) {
    static const char* n[] = { "normal", "screen", "lighten", "overlay", "multiply",
                               "soft light", "difference", "addition", "darken" };
    return (i >= 0 && i < 9) ? n[i] : n[0];
}

// Tracks are never pre-made: one appears when media is dragged or dropped above
// the base cut (video) or below the last audio row, and disappears once empty.
static int NewOverlayTrack() {
    auto t = std::make_unique<VideoTrack>();
    char n[32];
    snprintf(n, sizeof(n), "Video %d", (int)g_over.size() + 2);   // base is "Video 1"
    t->name = n;
    g_over.push_back(std::move(t));
    return (int)g_over.size() - 1;
}
static int NewAudioTrack() {
    auto t = std::make_unique<AudioTrack>();
    char n[32];
    snprintf(n, sizeof(n), "Audio %d", (int)g_atracks.size() + 1);
    t->name = n;
    g_atracks.push_back(std::move(t));
    return (int)g_atracks.size() - 1;
}

// How long a shot lasts on the timeline. A muted shot still has a duration — that
// is what comes back when you unmute it — but it occupies none of the film.
static double TimeLen(const Clip& c) { return c.skip ? 0.0 : c.duration; }

static Clip* SelectedClip() {
    if (g_selTrack == -1)
        return (g_sel >= 0 && g_sel < (int)g_clips.size()) ? g_clips[g_sel].get() : nullptr;
    if (g_selTrack >= 0 && g_selTrack < (int)g_over.size()) {
        auto& v = g_over[g_selTrack]->clips;
        return (g_sel >= 0 && g_sel < (int)v.size()) ? v[g_sel].get() : nullptr;
    }
    return nullptr;
}

static std::atomic<double> g_playhead(0.0);
static std::atomic<bool>   g_playing(false);
static ma_device           g_audioDevice;
static bool                g_audioReady = false;
// Preview hush: silence every clip's own audio while cutting, without touching
// the clips themselves. Export and the project file never see it.
static std::atomic<bool>   g_hushClips(false);

// Ripple editing. On, trimming or deleting a base shot closes the film up and
// everything downstream follows. Off, the time is kept: what a shot gives up becomes
// a gap card, and nothing after the edit moves.
static bool g_rippleOn = false;             // the app opens with ripple off

// Where each shot sits on the film, once mutes and dissolves are taken into
// account. Muted shots collapse onto the cut they sit on; a dissolve pulls the
// next shot back over the tail of this one.
struct BaseSpan {
    double start = 0, end = 0;
    double fade = 0;                       // overlap with the next visible shot
};

// A dissolve cannot eat more than most of either shot, or the two would cross
// past each other and the order would stop meaning anything.
static double ClampFade(double want, double a, double b) {
    double lim = (a < b ? a : b) * 0.9;
    if (want > lim) want = lim;
    return want < 0 ? 0 : want;
}

static void BaseLayoutIn(const std::vector<std::unique_ptr<Clip>>& v,
                         std::vector<BaseSpan>& out) {
    out.assign(v.size(), BaseSpan());
    double at = 0;
    int prev = -1;                         // last visible shot, the one that fades
    for (int i = 0; i < (int)v.size(); i++) {
        Clip& c = *v[i];
        if (c.skip) { out[i].start = out[i].end = at; continue; }
        double d = c.duration;
        if (prev >= 0) {
            double f = ClampFade(v[prev]->xfade, v[prev]->duration, d);
            out[prev].fade = f;
            at -= f;                       // the overlap: this shot starts early
        }
        out[i].start = at;
        out[i].end = at + d;
        at += d;
        prev = i;
    }
}

// The cut being edited is just one clip list among several: a folded sequence has
// its own, and cutting one open needs its layout too.
static void BaseLayout(std::vector<BaseSpan>& out) { BaseLayoutIn(g_clips, out); }

// A ripple on the base track moves every later shot. Anything parked over that
// stretch - layer clips, sound blocks - belongs to those shots, so it travels the
// same distance and stays lined up with the picture it was cut against.
static void RippleOthers(double fromTime, double shift);

static double TotalDuration() {
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    double t = 0;
    for (size_t i = 0; i < lay.size(); i++) if (!g_clips[i]->skip && lay[i].end > t) t = lay[i].end;
    return t;
}
// How far the playhead may run. The base cut is only one layer: layer clips and
// sound blocks parked past its last shot are still there to be watched, so the
// preview reaches the last thing on any track, not the end of the base track.
static double TimelineEnd() {
    double t = TotalDuration();
    for (auto& tr : g_over)
        for (auto& c : tr->clips) {
            if (c->skip) continue;
            double e = c->start + c->duration;
            if (e > t) t = e;
        }
    for (auto& tr : g_atracks)
        for (auto& b : tr->blocks) {
            double e = b->offset + (b->trimEnd - b->trimStart);
            if (e > t) t = e;
        }
    return t;
}
static int ClipAt(double t, double* clipStart = nullptr) {
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    int last = -1;
    for (int i = 0; i < (int)g_clips.size(); i++) {
        if (g_clips[i]->skip) continue;
        last = i;
        if (t < lay[i].end) { if (clipStart) *clipStart = lay[i].start; return i; }
    }
    // Past the last shot the base track is simply over: it holds no frame there,
    // so anything parked further out on a layer plays over black. The last frame
    // itself still stands, so parking on the end mark shows a picture.
    if (last >= 0 && t <= lay[last].end + 1e-9) {
        if (clipStart) *clipStart = lay[last].start;
        return last;
    }
    return -1;
}

// ------------------------------------------------------------------- audio

// The mixer walks the track list on the audio thread, so anything that adds,
// moves or frees a block takes this spin lock first. Held for microseconds.
static std::atomic<bool> g_mixLock(false);
struct MixGuard {
    MixGuard()  { while (g_mixLock.exchange(true, std::memory_order_acquire)) Sleep(0); }
    ~MixGuard() { g_mixLock.store(false, std::memory_order_release); }
};

// ---- sequences
// The whole film is a tree of sequences. Sequence 0 is the reel you start on;
// every fold makes another one, and a Nest clip on a base track stands in for it.
// The editor only ever edits ONE sequence: g_clips / g_over / g_atracks are the
// working copy of whichever level you are inside. Stepping in or out commits the
// working copy back to its Sequence and loads the other one, so nothing has to
// know about nesting except the code that renders and the code that saves.
struct Sequence {
    int id = 0;
    std::string name;
    double playhead = 0.0;                 // where you left the head on this level
    std::vector<std::unique_ptr<Clip>> clips;
    std::vector<std::unique_ptr<VideoTrack>> over;
    std::vector<std::unique_ptr<AudioTrack>> atracks;
};

static std::vector<std::unique_ptr<Sequence>> g_seqs;
static std::vector<int> g_nav;              // ids from the root down to where you are
static int g_seqNext = 1;                   // next sequence id to hand out

static Sequence* FindSeq(int id) {
    for (auto& q : g_seqs) if (q->id == id) return q.get();
    return nullptr;
}
static int CurSeqId() { return g_nav.empty() ? 0 : g_nav.back(); }
#include "edit_workspace_state.h"
static Sequence* CurSeq() { return FindSeq(CurSeqId()); }
static int NestDepth() { return (int)g_nav.size() - 1; }

static Sequence* MakeSeq(const std::string& name) {
    auto q = std::make_unique<Sequence>();
    q->id = g_seqNext++;
    q->name = name;
    Sequence* r = q.get();
    g_seqs.push_back(std::move(q));
    return r;
}

// The root always exists, even in an empty project: it is the reel.
static void EnsureRootSeq() {
    if (FindSeq(0)) return;
    auto q = std::make_unique<Sequence>();
    q->id = 0;
    q->name = "FILM";
    g_seqs.insert(g_seqs.begin(), std::move(q));
    if (g_nav.empty()) g_nav.push_back(0);
}

struct VideoAudioBlock {
    std::shared_ptr<Song> audio;
    double start;
    double trimIn;
    double duration;
    float volume;
    bool reversed;
};
static std::vector<VideoAudioBlock> g_videoAudio;
// Live filter state for blocks carrying a chain of their own, by block uid. Audio
// thread only.
static std::map<int, AudioFxState> g_blockFx;
static std::map<int, TextureState> g_texState;   // noise beds, by block uid

static void AudioCallback(ma_device*, void* out, const void*, ma_uint32 frames) {
    MixGuard lock;
    float* o = (float*)out;
    memset(o, 0, sizeof(float) * frames * 2);
    if (!g_playing.load(std::memory_order_relaxed)) return;
    double ph = g_playhead.load(std::memory_order_relaxed);
    long long current_frames = llround(ph * SAMPLE_RATE);
    // Every track, every block. A track with a chain on it is summed into its own
    // scratch first so the chain sees the whole track, then folded into the mix.
    static std::vector<float> scratch, bscratch;
    if (scratch.size() < (size_t)frames * 2) scratch.resize((size_t)frames * 2);
    if (bscratch.size() < (size_t)frames * 2) bscratch.resize((size_t)frames * 2);
    for (int ti = 0; ti < (int)g_atracks.size(); ti++) {
        auto& tr = g_atracks[ti];
        if (tr->mute) continue;
        float gain = tr->volume * 0.9f;
        bool  fx = tr->fx > AFX_NONE && tr->fxMix > 0.0001f;
        // A chain region on any unmuted track can run over this one, so the track
        // needs a bus of its own for the region to work on.
        bool region = false;
        for (auto& t2 : g_atracks) {
            if (t2->mute) continue;
            for (auto& rb : t2->blocks) if (rb->gen == 2 && rb->target == ti) region = true;
        }
        const bool bus = fx || region;
        float* dst = bus ? scratch.data() : o;
        if (bus) memset(dst, 0, sizeof(float) * frames * 2);
        for (auto& sp : tr->blocks) {
            Song& s = *sp;
            if (!s.loaded || s.gen == 2) continue;          // a region makes no sound itself
            long long start_base = current_frames - llround(s.offset * SAMPLE_RATE);
            if (s.gen == 1) {                                // noise bed: a texture from nothing
                if (s.tex < 0 || s.tex >= TEX_COUNT || s.fxMix <= 0.0001f) continue;
                long long len = llround((s.trimEnd - s.trimStart) * SAMPLE_RATE);
                long long g0 = -start_base, g1 = len - start_base;
                if (g0 < 0) g0 = 0;
                if (g1 > (long long)frames) g1 = (long long)frames;
                if (g1 <= g0) continue;
                float* bd = bscratch.data();
                memset(bd, 0, sizeof(float) * frames * 2);
                g_texState[s.uid].Render(bd, frames, s.tex, s.fxMix);
                const bool bfade = s.fadeIn > 0.001f || s.fadeOut > 0.001f;
                for (long long i = g0; i < g1; i++) {
                    float gg = gain * s.volume;
                    if (bfade) gg *= SongFadeGain(s, ph + (double)i / SAMPLE_RATE);
                    dst[i * 2 + 0] += bd[i * 2 + 0] * gg;
                    dst[i * 2 + 1] += bd[i * 2 + 1] * gg;
                }
                continue;
            }
            long long lo = llround(s.trimStart * SAMPLE_RATE);
            long long hi = llround(s.trimEnd * SAMPLE_RATE);
            long long total = (long long)(s.pcm.size() / 2);
            if (hi > total) hi = total;
            if (lo < 0) lo = 0;
            // Which output samples land inside the block is a range, not a
            // per-sample question: solve it once and the inner loop is a
            // straight mix with no branch in it.
            long long i0 = -start_base, i1 = hi - lo - start_base;
            if (i0 < 0) i0 = 0;
            if (i1 > (long long)frames) i1 = (long long)frames;
            if (i1 <= i0) continue;
            // A block with its own chain runs through it alone first, then joins
            // the track (and the track's chain, if any).
            // A fading block also mixes alone, so its ramp lands after its own chain.
            const bool bfx = s.fx > AFX_NONE && s.fx < AFX_COUNT && s.fxMix > 0.0001f;
            const bool bfade = s.fadeIn > 0.001f || s.fadeOut > 0.001f;
            float* bd = dst;
            if (bfx || bfade) { bd = bscratch.data(); memset(bd, 0, sizeof(float) * frames * 2); }
            const float* pcm = s.pcm.data();
            // Without a chain the block's level is linear, so it rides in with the
            // track's; with one it waits until after the chain, like the export.
            const float bgain = bfx ? gain : gain * s.volume;
            if (s.reversed) {
                long long base = hi - 1 - start_base;
                for (long long i = i0; i < i1; i++) {
                    const float* q = pcm + (base - i) * 2;
                    bd[i * 2 + 0] += q[0] * bgain;
                    bd[i * 2 + 1] += q[1] * bgain;
                }
            } else {
                const float* q = pcm + (lo + start_base + i0) * 2;
                for (long long i = i0; i < i1; i++, q += 2) {
                    bd[i * 2 + 0] += q[0] * bgain;
                    bd[i * 2 + 1] += q[1] * bgain;
                }
            }
            if (bfx) g_blockFx[s.uid].Process(bd, frames, s.fx, s.fxMix);
            const float post = bfx ? s.volume : 1.0f;
            if (bfade || post != 1.0f) {
                for (ma_uint32 k = 0; k < frames; k++) {
                    float fg = post * (bfade ? SongFadeGain(s, ph + (double)k / SAMPLE_RATE) : 1.0f);
                    bd[k * 2 + 0] *= fg;
                    bd[k * 2 + 1] *= fg;
                }
            }
            if (bfx || bfade)
                for (ma_uint32 k = 0; k < frames * 2; k++) dst[k] += bd[k];
        }
        // Chain regions over this track: inside a region's span the bus is its chain's
        // output, outside it the bus passes untouched. Before the track's own chain.
        if (region) {
            for (auto& t2 : g_atracks) {
                if (t2->mute) continue;
                for (auto& rb : t2->blocks) {
                    Song& r = *rb;
                    if (r.gen != 2 || r.target != ti) continue;
                    if (r.fx <= AFX_NONE || r.fx >= AFX_COUNT || r.fxMix <= 0.0001f) continue;
                    long long sb = current_frames - llround(r.offset * SAMPLE_RATE);
                    long long len = llround((r.trimEnd - r.trimStart) * SAMPLE_RATE);
                    long long g0 = -sb, g1 = len - sb;
                    if (g0 < 0) g0 = 0;
                    if (g1 > (long long)frames) g1 = (long long)frames;
                    if (g1 <= g0) continue;
                    float* w = bscratch.data();
                    memcpy(w, dst, sizeof(float) * frames * 2);
                    g_blockFx[r.uid].Process(w, frames, r.fx, r.fxMix);
                    for (long long i = g0; i < g1; i++) {
                        dst[i * 2 + 0] = w[i * 2 + 0];
                        dst[i * 2 + 1] = w[i * 2 + 1];
                    }
                }
            }
        }
        if (fx) tr->fxs.Process(dst, frames, tr->fx, tr->fxMix);
        if (bus) for (ma_uint32 i = 0; i < frames * 2; i++) o[i] += dst[i];
    }
    if (!g_hushClips.load(std::memory_order_relaxed))
    for (auto& c : g_videoAudio) {
        Song& s = *c.audio;
        float gain = c.volume;
        long long start_base = current_frames - llround(c.start * SAMPLE_RATE);
        long long lo = llround(c.trimIn * SAMPLE_RATE);
        long long hi = llround((c.trimIn + c.duration) * SAMPLE_RATE);
        long long total = (long long)(s.pcm.size() / 2);
        if (hi > total) hi = total;
        if (lo < 0) lo = 0;
        long long i0 = -start_base, i1 = hi - lo - start_base;
        if (i0 < 0) i0 = 0;
        if (i1 > (long long)frames) i1 = (long long)frames;
        if (i1 <= i0) continue;
        const float* pcm = s.pcm.data();
        if (c.reversed) {
            long long base = hi - 1 - start_base;
            for (long long i = i0; i < i1; i++) {
                const float* q = pcm + (base - i) * 2;
                o[i * 2 + 0] += q[0] * gain;
                o[i * 2 + 1] += q[1] * gain;
            }
        } else {
            const float* q = pcm + (lo + start_base + i0) * 2;
            for (long long i = i0; i < i1; i++, q += 2) {
                o[i * 2 + 0] += q[0] * gain;
                o[i * 2 + 1] += q[1] * gain;
            }
        }
    }

    for (ma_uint32 i = 0; i < frames * 2; i++)      // keep the sum inside the rails
        o[i] = o[i] > 1.0f ? 1.0f : (o[i] < -1.0f ? -1.0f : o[i]);
    g_playhead.store((double)(current_frames + frames) / SAMPLE_RATE, std::memory_order_relaxed);
}

static void InitAudio() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = SAMPLE_RATE;
    cfg.dataCallback = AudioCallback;
    cfg.periodSizeInMilliseconds = 50;
    g_audioReady = ma_device_init(nullptr, &cfg, &g_audioDevice) == MA_SUCCESS &&
                   ma_device_start(&g_audioDevice) == MA_SUCCESS;
}

static std::future<bool> g_songPending;
static std::atomic<bool> g_songLoading(false);
static std::string       g_songStatus;
static int    g_songTargetTrack = 0;       // where the next load lands
static double g_songTargetTime = 0.0;

// Decoded audio is kept by path: undo reloads the project text many times over a
// session, and re-decoding a five-minute track each time would stall the UI.
static std::mutex g_songCacheMx;
static std::map<std::wstring, std::shared_ptr<Song>> g_songCache;

// Decode a file to interleaved stereo f32 plus a peak ladder for the waveform.
static std::unique_ptr<Song> DecodeSongFileUncached(const std::wstring& path) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, SAMPLE_RATE);
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &dc, &dec) != MA_SUCCESS) return nullptr;
    std::vector<float> pcm;
    ma_uint64 total_frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total_frames) == MA_SUCCESS && total_frames > 0) {
        pcm.reserve(total_frames * 2);
    }
    float buf[4096 * 2];
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        if (got == 0) break;
        pcm.insert(pcm.end(), buf, buf + got * 2);
    }
    ma_decoder_uninit(&dec);
    if (pcm.empty()) return nullptr;

    auto sp = std::make_unique<Song>();
    Song& s = *sp;
    pcm.shrink_to_fit();                   // block reads overshoot; an hour of stereo
    s.pcm = std::move(pcm);                // f32 wastes hundreds of MB in slack
    s.duration = (double)(s.pcm.size() / 2) / SAMPLE_RATE;
    size_t nFrames = s.pcm.size() / 2;
    size_t nPeaks = nFrames / s.framesPerPeak + 1;
    s.peaks.resize(nPeaks * 2);
    for (size_t p = 0; p < nPeaks; p++) {
        float lo = 0, hi = 0;
        size_t a2 = p * s.framesPerPeak, b2 = a2 + s.framesPerPeak;
        if (b2 > nFrames) b2 = nFrames;
        for (size_t f = a2; f < b2; f++) {
            float v = (s.pcm[f * 2] + s.pcm[f * 2 + 1]) * 0.5f;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        s.peaks[p * 2] = lo; s.peaks[p * 2 + 1] = hi;
    }
    s.path = path;
    s.label = Narrow(BaseName(path));
    s.trimStart = 0.0;
    s.trimEnd = s.duration;
    s.loaded = true;
    return sp;
}

static std::unique_ptr<Song> DecodeSongFile(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lk(g_songCacheMx);
        auto it = g_songCache.find(path);
        if (it != g_songCache.end()) {
            // the cached copy carries the first decode's uid; a second block of the
            // same file needs its own, or picking one picks every copy
            auto sp = std::make_unique<Song>(*it->second);
            sp->uid = g_uidNext++;
            return sp;
        }
    }
    auto sp = DecodeSongFileUncached(path);
    if (!sp) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_songCacheMx);
        g_songCache[path] = std::make_shared<Song>(*sp);
    }
    return sp;
}

static bool LoadSongBlocking(const std::wstring& path) {
    auto sp = DecodeSongFile(path);
    if (!sp) return false;
    sp->offset = g_songTargetTime < 0 ? 0.0 : g_songTargetTime;
    // -2 = the new-track strip, -1 = "wherever", anything else = that track.
    int t = g_songTargetTrack;
    if (t == -2 || g_atracks.empty()) t = NewAudioTrack();
    else if (t < 0 || t >= (int)g_atracks.size()) t = 0;
    {
        MixGuard lock;                     // keep the mixer out while the list grows
        g_atracks[t]->blocks.push_back(std::move(sp));
    }
    return true;
}

// The most recently added block, for callers that want to relabel it.
static Song* LastLoadedSong() {
    int t = g_songTargetTrack;
    if (t < 0 || t >= (int)g_atracks.size()) t = 0;
    auto& b = g_atracks[t]->blocks;
    return b.empty() ? nullptr : b.back().get();
}

// Download a YouTube URL's audio with yt-dlp (lossless flac intermediate so the
// final AAC encode is single-generation), then load it as the song.
static bool LoadSongFromYouTube(std::string url) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring out = std::wstring(tmp) + L"slidecut_yt.flac";
    DeleteFileW(out.c_str());
    std::wstring tpl = std::wstring(tmp) + L"slidecut_yt.%(ext)s";
    std::wstring cmd = L"yt-dlp -x --audio-format flac --audio-quality 0 --no-playlist"
                       L" --force-overwrites -o \"" + tpl + L"\" \"" + Widen(url) + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_songStatus = "yt-dlp not found on PATH";
        return false;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != 0 || GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_songStatus = "yt-dlp failed — check the URL";
        return false;
    }
    bool ok = LoadSongBlocking(out);
    if (ok) {
        // Nicer label than the temp filename.
        size_t slash = url.find_last_of('/');
        if (Song* s = LastLoadedSong())
            s->label = "YouTube · " + (slash == std::string::npos ? url : url.substr(slash + 1));
        g_songStatus.clear();
    } else {
        g_songStatus = "downloaded but could not decode audio";
    }
    return ok;
}

static void StartSongLoad(const std::wstring& path, int track = -1, double at = 0.0) {
    if (g_songLoading.load()) return;
    g_songTargetTrack = track;
    g_songTargetTime = at;
    g_songLoading.store(true);
    g_songStatus = "loading audio…";
    g_songPending = std::async(std::launch::async, [p = path] {
        bool ok = LoadSongBlocking(p);
        g_songStatus = ok ? "" : "could not decode audio file";
        g_songLoading.store(false);
        return ok;
    });
}

// ------------------------------------------------------------ image loading

static LoadedImage DecodeImage(std::wstring path) {
    LoadedImage r;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return r;
    int w, h, comp;
    unsigned char* px = stbi_load_from_file(f, &w, &h, &comp, 4);
    fclose(f);
    if (!px) return r;
    r.srcW = w; r.srcH = h;
    const int MAXDIM = 1440;               // display copy; export uses the original file
    int ow = w, oh = h;
    if (w > MAXDIM || h > MAXDIM) {
        float s = (float)MAXDIM / (w > h ? w : h);
        ow = (int)(w * s); oh = (int)(h * s);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        r.px.resize((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) {     // box-average downscale
            int sy0 = y * h / oh, sy1 = (y + 1) * h / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int x = 0; x < ow; x++) {
                int sx0 = x * w / ow, sx1 = (x + 1) * w / ow; if (sx1 <= sx0) sx1 = sx0 + 1;
                int acc[4] = {}, n = 0;
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) {
                        const unsigned char* p = px + ((size_t)sy * w + sx) * 4;
                        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; n++;
                    }
                unsigned char* d = r.px.data() + ((size_t)y * ow + x) * 4;
                d[0] = acc[0] / n; d[1] = acc[1] / n; d[2] = acc[2] / n; d[3] = acc[3] / n;
            }
        }
    } else {
        r.px.assign(px, px + (size_t)w * h * 4);
    }
    r.w = ow; r.h = oh;
    stbi_image_free(px);
    r.ok = true;
    return r;
}

static void AddImages(const std::vector<std::wstring>& paths) {
    for (auto& p : paths) {
        auto c = std::make_unique<Clip>();
        c->path = p;
        c->label = Narrow(BaseName(p));
        c->pending = std::async(std::launch::async, DecodeImage, p);
        g_clips.push_back(std::move(c));
    }
}

// --------------------------------------------------------------- video proxy

// Preview ladder quality. Higher settings mean a slower first extraction and more
// VRAM per cached frame, so the cache is bounded by bytes rather than frame count.
enum PreviewQuality { PV_FAST = 0, PV_GOOD = 1, PV_BEST = 2 };
static int g_preview = PV_GOOD;
static int  PreviewH()    { return g_preview == PV_FAST ? 360 : g_preview == PV_GOOD ? 540 : 720; }
static int  PreviewFps()  { return g_preview == PV_FAST ? 12  : g_preview == PV_GOOD ? 24  : 30; }
static int  PreviewJpegQ(){ return g_preview == PV_FAST ? 6 : 3; }   // ffmpeg -q:v, lower = better

static const size_t PROXY_CACHE_BYTES = 1024u * 1024 * 1024;  // GPU budget per source

// Project timebase. Auto-adopts the first video's rate so 60fps footage stays 60fps.
static int  g_fps = 30;
static bool g_fpsAuto = true;
static double MinClipDur() { return 1.0 / g_fps; }

static std::vector<std::shared_ptr<VideoSource>> g_videoSources;

// Runs a console tool hidden and waits. stdout/stderr go to outFile when given.
static bool RunHidden(const std::wstring& cmd, const std::wstring& outFile = L"") {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!outFile.empty())
        h = CreateFileW(outFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = { sizeof(si) };
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = h;
        si.hStdError = h;
    }
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, h != INVALID_HANDLE_VALUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); return false; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return code == 0;
}

static std::string ReadTextFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[4096];
    DWORD rd = 0;
    while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd) s.append(buf, rd);
    CloseHandle(f);
    return s;
}

// Stable per-file id so re-opening the same video reuses an existing proxy.
static std::wstring ProxyIdFor(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    unsigned long long h = 1469598103934665603ULL;
    std::wstring key = path;
    wchar_t stamp[64];
    swprintf(stamp, 64, L"|%lu|%lu|%lu", fa.nFileSizeLow, fa.ftLastWriteTime.dwLowDateTime,
             fa.ftLastWriteTime.dwHighDateTime);
    key += stamp;
    for (wchar_t ch : key) { h ^= (unsigned long long)ch; h *= 1099511628211ULL; }
    wchar_t out[32];
    swprintf(out, 32, L"%016llx", h);
    return out;
}

static int CountProxyFrames(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"*.jpg").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { n++; } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return n;
}

// Background: pull the video's own audio down to mono 8k and reduce it to peaks,
// cached next to the proxy frames so a reopened project draws instantly.
static void BuildVideoPeaks(std::shared_ptr<VideoSource> vs) {
    std::wstring wav = vs->proxyDir + L"audio.wav";
    if (GetFileAttributesW(wav.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t cmd[1024];
        swprintf(cmd, 1024,
                 L"ffmpeg -v error -y -i \"%ls\" -vn -ac 2 -ar 48000 \"%ls\"",
                 vs->path.c_str(), wav.c_str());
        if (!RunHidden(cmd)) return;
    }
    vs->audio = std::move(DecodeSongFileUncached(wav));
    if (vs->audio) {
        vs->apeakRate = 48000.0 / vs->audio->framesPerPeak;
        vs->apeaksReady.store(true);       // publishes ->audio to the drawing thread
    }
}

// Background: probe metadata, then transcode a small jpeg ladder for scrubbing.
static void BuildProxy(std::shared_ptr<VideoSource> vs) {
    vs->building.store(true);
    struct Done { std::shared_ptr<VideoSource> v; ~Done() { v->building.store(false); v->ready.store(true); } } done{ vs };
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"slidecut_proxy\\";
    CreateDirectoryW(root.c_str(), nullptr);
    wchar_t sub[128];
    swprintf(sub, 128, L"_%dp%d\\", PreviewH(), PreviewFps());
    vs->proxyDir = root + ProxyIdFor(vs->path) + sub;
    bool fresh = CreateDirectoryW(vs->proxyDir.c_str(), nullptr) != 0;

    std::wstring info = vs->proxyDir + L"info.txt";
    if (!RunHidden(L"ffprobe -v error -select_streams v:0 "
                   L"-show_entries stream=width,height,r_frame_rate "
                   L"-show_entries format=duration,start_time -of default=nw=1 \"" + vs->path + L"\"",
                   info)) {
        vs->probed.store(true);
        vs->ready.store(true);
        return;
    }
    {
        std::string s = ReadTextFile(info);
        auto value = [&](const char* key) -> std::string {
            size_t p = s.find(std::string(key) + "=");
            if (p == std::string::npos) return {};
            p += strlen(key) + 1;
            size_t e = s.find_first_of("\r\n", p);
            return s.substr(p, e == std::string::npos ? e : e - p);
        };
        vs->w = atoi(value("width").c_str());
        vs->h = atoi(value("height").c_str());
        vs->duration = atof(value("duration").c_str());
        vs->startTime = atof(value("start_time").c_str());   // "N/A" reads as 0
        std::string rate = value("r_frame_rate");        // "60000/1001" style
        size_t slash = rate.find('/');
        double num = atof(rate.c_str());
        double den = slash == std::string::npos ? 1.0 : atof(rate.c_str() + slash + 1);
        vs->fps = den > 0 ? num / den : 0;
        vs->aspect = vs->h > 0 ? (float)vs->w / (float)vs->h : 1.0f;
    }
    std::wstring ainfo = vs->proxyDir + L"audio.txt";
    RunHidden(L"ffprobe -v error -select_streams a:0 -show_entries stream=index "
              L"-of csv=p=0 \"" + vs->path + L"\"", ainfo);
    vs->hasAudio = !ReadTextFile(ainfo).empty();
    vs->probed.store(true);
    if (vs->hasAudio) std::thread(BuildVideoPeaks, vs).detach();

    int have = CountProxyFrames(vs->proxyDir);
    int want = (int)(vs->duration * PreviewFps());
    if (!fresh && have > 0 && have >= want - 2) {        // reuse a complete proxy
        vs->framesOnDisk.store(have);
        vs->ready.store(true);
        return;
    }
    wchar_t vf[160];
    swprintf(vf, 160, L" -vf fps=%d,scale=-2:%d:flags=lanczos -q:v %d ",
             PreviewFps(), PreviewH(), PreviewJpegQ());
    std::thread poll([vs] {                              // let scrubbing start early
        while (!vs->ready.load()) {
            vs->framesOnDisk.store(CountProxyFrames(vs->proxyDir));
            Sleep(400);
        }
    });
    RunHidden(L"ffmpeg -v error -y -i \"" + vs->path + L"\"" + vf +
              L"\"" + vs->proxyDir + L"%06d.jpg\"");
    vs->ready.store(true);
    poll.join();
    vs->framesOnDisk.store(CountProxyFrames(vs->proxyDir));
}

// A texture handed to ImGui this frame is not drawn until the frame is rendered,
// so releasing one mid-frame leaves a draw command pointing at freed memory. Every
// picture texture goes through here instead and is let go a few frames later, once
// nothing can still be holding it.
static std::vector<std::pair<ID3D11ShaderResourceView*, int>> g_retire;

static void RetireTexture(ID3D11ShaderResourceView* t) {
    if (t) g_retire.push_back({ t, 3 });
}

static void PumpRetiredTextures() {
    for (size_t i = 0; i < g_retire.size(); ) {
        if (--g_retire[i].second <= 0) {
            g_retire[i].first->Release();
            g_retire[i] = g_retire.back();
            g_retire.pop_back();
        } else i++;
    }
}

static void ReleaseProxyCache(VideoSource& vs);

// Preview quality changed: drop the uploaded frames and re-extract at the new size.
static void RebuildProxies() {
    for (auto& v : g_videoSources) {
        if (v->building.load()) continue;
        ReleaseProxyCache(*v);
        v->ready.store(false);
        v->framesOnDisk.store(0);
        std::thread(BuildProxy, v).detach();
    }
}

static bool AnyProxyBuilding() {
    for (auto& v : g_videoSources) if (v->building.load()) return true;
    return false;
}

static std::shared_ptr<VideoSource> GetVideoSource(const std::wstring& path) {
    for (auto& v : g_videoSources) if (v->path == path) return v;
    auto vs = std::make_shared<VideoSource>();
    vs->path = path;
    g_videoSources.push_back(vs);
    std::thread(BuildProxy, vs).detach();
    return vs;
}

// Read one proxy jpeg and hand back a texture. Runs on the drawing thread for the
// frame that is needed now, and on the prefetch thread for the ones about to be.
// D3D11 resource creation is free-threaded, so both are allowed to do this.
static ID3D11ShaderResourceView* LoadProxyTexture(VideoSource& vs, int idx,
                                                  int* outW, int* outH) {
    wchar_t name[32];
    swprintf(name, 32, L"%06d.jpg", idx);
    FILE* f = _wfopen((vs.proxyDir + name).c_str(), L"rb");
    if (!f) return nullptr;
    // Whole file in one read, then decode from memory: per-call stdio locking is a
    // real slice of the budget when this runs once a frame.
    static thread_local std::vector<unsigned char> jpg;
    fseek(f, 0, SEEK_END);
    long jlen = ftell(f);
    unsigned char* px = nullptr;
    int w = 0, h = 0, comp = 0;
    if (jlen > 0) {
        rewind(f);
        jpg.resize((size_t)jlen);
        size_t got = fread(jpg.data(), 1, (size_t)jlen, f);
        fclose(f);
        if (got == (size_t)jlen)
            px = stbi_load_from_memory(jpg.data(), (int)jlen, &w, &h, &comp, 4);
    } else fclose(f);
    if (!px) return nullptr;
    ID3D11ShaderResourceView* tex = CreateTextureRGBA(px, w, h);
    stbi_image_free(px);
    *outW = w; *outH = h;
    return tex;
}

// Put a decoded frame in the cache, making room around idx first. Drawing thread.
static void ProxyInsert(VideoSource& vs, int idx, ID3D11ShaderResourceView* tex,
                        int w, int h) {
    if (vs.cache.count(idx)) { RetireTexture(tex); return; }
    if (vs.aspect <= 0 || vs.w == 0) vs.aspect = (float)w / (float)h;
    vs.frameBytes = (size_t)w * h * 4;
    if (vs.cacheBytes + vs.frameBytes > PROXY_CACHE_BYTES) {   // drop frames far from here
        for (int span = 240; span >= 15 && vs.cacheBytes + vs.frameBytes > PROXY_CACHE_BYTES;
             span /= 2) {
            for (auto i = vs.cache.begin(); i != vs.cache.end(); ) {
                if (abs(i->first - idx) > span) {
                    RetireTexture(i->second);
                    vs.cacheBytes -= vs.frameBytes;
                    i = vs.cache.erase(i);
                } else ++i;
            }
        }
    }
    vs.cache[idx] = tex;
    vs.cacheBytes += vs.frameBytes;
}

// ---- prefetch: the frames playback is about to want, decoded off the drawing
// thread. Without it every played frame pays for a jpeg decode before it draws.
struct ProxyReq  { std::shared_ptr<VideoSource> vs; int idx; int gen; };
struct ProxyDone { std::shared_ptr<VideoSource> vs; int idx; int gen;
                   ID3D11ShaderResourceView* tex; int w, h; };
static std::mutex              g_pfMx;
static std::condition_variable g_pfCv;
static std::deque<ProxyReq>    g_pfQueue;     // guarded by g_pfMx
static std::vector<ProxyDone>  g_pfDone;      // guarded by g_pfMx
static std::atomic<bool>       g_pfStop{ false };
static std::thread             g_pfThread;
static const int PREFETCH_AHEAD = 12;         // about half a second of playback

static void ProxyPrefetchThread() {
    for (;;) {
        ProxyReq r;
        {
            std::unique_lock<std::mutex> lk(g_pfMx);
            g_pfCv.wait(lk, [] { return g_pfStop.load() || !g_pfQueue.empty(); });
            if (g_pfStop.load()) return;
            if (g_pfDone.size() > 64) { g_pfQueue.clear(); continue; }  // drawing thread is behind
            r = g_pfQueue.front();
            g_pfQueue.pop_front();
        }
        if (r.vs->cacheGen.load() != r.gen) continue;
        int w = 0, h = 0;
        ID3D11ShaderResourceView* tex = LoadProxyTexture(*r.vs, r.idx, &w, &h);
        if (!tex) continue;
        std::lock_guard<std::mutex> lk(g_pfMx);
        g_pfDone.push_back({ r.vs, r.idx, r.gen, tex, w, h });
    }
}

static void StartProxyPrefetch() {
    if (!g_pfThread.joinable()) g_pfThread = std::thread(ProxyPrefetchThread);
}

static void StopProxyPrefetch() {
    if (!g_pfThread.joinable()) return;
    g_pfStop.store(true);
    g_pfCv.notify_all();
    g_pfThread.join();
    std::lock_guard<std::mutex> lk(g_pfMx);
    for (auto& d : g_pfDone) if (d.tex) d.tex->Release();
    g_pfDone.clear();
    g_pfQueue.clear();
}

// Drawing thread: take whatever the prefetch thread finished since the last frame.
static void PumpProxyPrefetch() {
    std::vector<ProxyDone> done;
    {
        std::lock_guard<std::mutex> lk(g_pfMx);
        if (g_pfDone.empty()) return;
        done.swap(g_pfDone);
    }
    for (auto& d : done) {
        if (d.vs->cacheGen.load() != d.gen) { d.tex->Release(); continue; }
        ProxyInsert(*d.vs, d.idx, d.tex, d.w, d.h);
    }
}

// Ask for the frames after idx, in the direction the playhead is travelling.
static void RequestPrefetch(const std::shared_ptr<VideoSource>& vsp, int idx, int dir) {
    VideoSource& vs = *vsp;
    int have = vs.framesOnDisk.load();
    int gen = vs.cacheGen.load();
    std::lock_guard<std::mutex> lk(g_pfMx);
    if (g_pfQueue.size() > 128) return;
    for (int n = 1; n <= PREFETCH_AHEAD; n++) {
        int want = idx + dir * n;
        if (want < 1 || (have > 0 && want > have)) break;
        if (vs.cache.count(want)) continue;
        bool queued = false;
        for (auto& q : g_pfQueue) if (q.vs.get() == &vs && q.idx == want) { queued = true; break; }
        if (queued) continue;
        g_pfQueue.push_back({ vsp, want, gen });
    }
    g_pfCv.notify_one();
}

// Frame nearest to t seconds into the source; nullptr while it is not on disk yet.
static ID3D11ShaderResourceView* ProxyFrame(VideoSource& vs, double t) {
    if (t < 0) t = 0;
    int have = vs.framesOnDisk.load();
    // Variable-rate sources and wrong duration metadata make ffmpeg emit a frame
    // count that is not duration * fps, so once the ladder is complete the real
    // count is what maps time to a frame. Using the nominal rate instead is what
    // made the preview drift away from the footage.
    double rate = PreviewFps();
    if (vs.ready.load() && have > 1 && vs.duration > 0.05) rate = (have - 1) / vs.duration;
    int idx = (int)(t * rate + 0.5) + 1;                 // ffmpeg numbers from 1
    if (idx < 1) idx = 1;
    if (have > 0 && idx > have) idx = have;
    // Which way the playhead is travelling through this source, so a reversed shot
    // reads ahead backwards.
    int dir = idx >= vs.lastIdx ? 1 : -1;
    vs.lastIdx = idx;
    for (auto& v : g_videoSources)
        if (v.get() == &vs) { RequestPrefetch(v, idx, dir); break; }

    auto it = vs.cache.find(idx);
    if (it != vs.cache.end()) return it->second;

    int w = 0, h = 0;
    ID3D11ShaderResourceView* tex = LoadProxyTexture(vs, idx, &w, &h);
    if (!tex) return nullptr;
    ProxyInsert(vs, idx, tex, w, h);
    return tex;
}

static void ReleaseProxyCache(VideoSource& vs) {
    vs.cacheGen.fetch_add(1);              // frames in flight for the old cache are void
    for (auto& kv : vs.cache) RetireTexture(kv.second);
    vs.cache.clear();
    vs.cacheBytes = 0;
    vs.lastIdx = 0;
}

static ImFont* g_titleFont = nullptr;      // Special Elite, loaded at TITLE_FONT_PX
static const float TITLE_FONT_PX = 96.0f;  // rasterized once, scaled down when drawn

static std::string FirstLine(const std::string& s) {
    size_t n = s.find('\n');
    std::string l = n == std::string::npos ? s : s.substr(0, n);
    return l.empty() ? std::string("(text)") : l;
}

static void AddTextClip(const std::string& text, double dur, float scale, int at = -1) {
    auto c = std::make_unique<Clip>();
    c->kind = Clip::Text;
    c->text = text;
    c->duration = dur;
    c->textScale = scale;
    c->label = FirstLine(text);
    if (at < 0 || at > (int)g_clips.size()) g_clips.push_back(std::move(c));
    else g_clips.insert(g_clips.begin() + at, std::move(c));
}

static void AddVideos(const std::vector<std::wstring>& paths) {
    for (auto& p : paths) {
        auto c = std::make_unique<Clip>();
        c->kind = Clip::Video;
        c->path = p;
        c->label = Narrow(BaseName(p));
        c->vid = GetVideoSource(p);
        c->duration = 0;                   // set from the probe in PumpPendingLoads
        g_clips.push_back(std::move(c));
    }
}

// ------------------------------------------------- double-exposure layer

// Video by extension; anything else is treated as a still.
static bool LooksLikeVideo(const std::wstring& p) {
    std::wstring e = LowerExt(p);
    static const wchar_t* v[] = { L"mp4", L"mov", L"m4v", L"mkv", L"webm", L"avi", L"wmv",
                                  L"mpg", L"mpeg", L"m2ts", L"mts", L"ts", L"flv", nullptr };
    for (int i = 0; v[i]; i++) if (e == v[i]) return true;
    return false;
}

static void ClearDoubleExposure(Clip& c) {
    if (c.dxPending.valid()) c.dxPending.get();   // else it would land on the next layer
    if (c.dxTex && !c.dxIsVideo) RetireTexture(c.dxTex);
    c.dxTex = nullptr;
    c.dxVid.reset();
    c.dxOn = false;
    c.dxPath.clear();
    c.dxLabel.clear();
    c.dxIsVideo = false;
    c.dxTrimIn = 0;
}

static void SetDoubleExposure(Clip& c, const std::wstring& path) {
    ClearDoubleExposure(c);
    c.dxPath = path;
    c.dxLabel = Narrow(BaseName(path));
    c.dxIsVideo = LooksLikeVideo(path);
    c.dxOn = true;
    if (c.dxIsVideo) c.dxVid = GetVideoSource(path);
    else c.dxPending = std::async(std::launch::async, DecodeImage, path);
}

// ------------------------------------------------------- file intake / drops

static const wchar_t* IMAGE_EXTS[] = { L"jpg", L"jpeg", L"png", L"bmp", L"tif", L"tiff",
                                       L"webp", L"gif", L"tga", L"jfif", nullptr };
static const wchar_t* VIDEO_EXTS[] = { L"mp4", L"mov", L"m4v", L"mkv", L"webm", L"avi",
                                       L"wmv", L"mpg", L"mpeg", L"m2ts", L"mts", L"ts",
                                       L"flv", nullptr };
static const wchar_t* AUDIO_EXTS[] = { L"mp3", L"wav", L"flac", L"ogg", L"m4a", L"aac",
                                       L"opus", L"wma", L"aiff", L"aif", nullptr };

static bool ExtIn(const std::wstring& path, const wchar_t* const* list) {
    std::wstring e = LowerExt(path);
    if (e.empty()) return false;
    for (int i = 0; list[i]; i++) if (e == list[i]) return true;
    return false;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string g_intakeStatus;         // "12 images · 2 videos · 1 song" after a drop

// Where the next intake lands. A drop on a track row sets these; the toolbar
// buttons reset them to "append to the base track / music track".
static int    g_dropVTrack = -1;           // -1 base video, >=0 overlay track
static double g_dropTime = 0.0;
static int    g_dropATrack = -1;           // -2 new track, -1 wherever, else that track
static void ResetDropTarget() { g_dropVTrack = -1; g_dropTime = 0.0; g_dropATrack = -1; }

// A bin card let go over the timeline. Held for one frame and applied before the
// next layout, so the lists never change under a timeline half drawn.
struct BinDrop { int lib = -1; int rowKind = 0; int track = -1; double t = 0; };
static BinDrop g_binDrop;
static void DropLibraryItem(int lib, int rowKind, int track, double t);   // edit_workspace_ui.h
// A texture block at the playhead: gen 1 plays texture `kind` (TEX_*), gen 2 runs
// chain `kind` (AFX_*) over another track.
static void AddGenBlock(int track, int gen, int kind);
static void UndoCapture();                        // defined with the undo stack

// One folder level deep: dropping a shoot folder should just work.
static void ExpandFolders(std::vector<std::wstring>& paths) {
    std::vector<std::wstring> out;
    for (auto& p : paths) {
        if (!IsDir(p)) { out.push_back(p); continue; }
        std::wstring dir = p;
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out.push_back(dir + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    paths.swap(out);
}

// Drops and multi-select dialogs hand back selection order, which is effectively
// random. Slideshows want name order, and "IMG_9" before "IMG_10".
static void SortNatural(std::vector<std::wstring>& paths) {
    std::sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
}

// Single entry point for the toolbar buttons and for WM_DROPFILES: classify by
// extension, keep timeline order = name order, first audio file becomes the song.
static void AddFiles(std::vector<std::wstring> paths, bool sortByName = true) {
    ExpandFolders(paths);
    if (sortByName) SortNatural(paths);
    std::vector<std::wstring> imgs, vids;
    std::wstring song;
    int skipped = 0;
    for (auto& p : paths) {
        if (ExtIn(p, IMAGE_EXTS)) imgs.push_back(p);
        else if (ExtIn(p, VIDEO_EXTS)) vids.push_back(p);
        else if (ExtIn(p, AUDIO_EXTS)) { if (song.empty()) song = p; else skipped++; }
        else skipped++;
    }
    // Media keeps drop order relative to its own kind; images first mirrors how the
    // Add Images / Add Video buttons behave when both are used.
    if (g_dropVTrack == -2) g_dropVTrack = NewOverlayTrack();   // dropped on the new-track strip
    if (g_dropVTrack >= 0 && g_dropVTrack < (int)g_over.size()) {
        // An overlay track: lay the new clips end to end from the drop point.
        auto& dst = g_over[g_dropVTrack]->clips;
        double at = g_dropTime < 0 ? 0 : g_dropTime;
        auto place = [&](const std::wstring& p, bool isVideo) {
            auto c = std::make_unique<Clip>();
            c->path = p;
            c->label = Narrow(BaseName(p));
            c->start = at;
            if (isVideo) {
                c->kind = Clip::Video;
                c->vid = GetVideoSource(p);
                c->duration = 0;           // filled in from the probe
            } else {
                c->pending = std::async(std::launch::async, DecodeImage, p);
                c->duration = 3.0;
            }
            at += c->duration > 0 ? c->duration : 3.0;
            dst.push_back(std::move(c));
        };
        for (auto& p : imgs) place(p, false);
        for (auto& p : vids) place(p, true);
    } else {
        if (!imgs.empty()) AddImages(imgs);
        if (!vids.empty()) AddVideos(vids);
    }
    if (!song.empty()) StartSongLoad(song, g_dropATrack, g_dropTime < 0 ? 0 : g_dropTime);

    char buf[160];
    if (imgs.empty() && vids.empty() && song.empty()) {
        snprintf(buf, sizeof(buf), "Nothing added — %d unsupported file%s dropped",
                 skipped, skipped == 1 ? "" : "s");
    } else {
        int n = snprintf(buf, sizeof(buf), "Added");
        if (!imgs.empty()) n += snprintf(buf + n, sizeof(buf) - n, " %zu image%s",
                                         imgs.size(), imgs.size() == 1 ? "" : "s");
        if (!vids.empty()) n += snprintf(buf + n, sizeof(buf) - n, " %zu video%s",
                                         vids.size(), vids.size() == 1 ? "" : "s");
        if (!song.empty()) n += snprintf(buf + n, sizeof(buf) - n, " · song");
        if (skipped)       snprintf(buf + n, sizeof(buf) - n, " · %d skipped", skipped);
    }
    g_intakeStatus = buf;
}

// Cut clip i of `v` at `off` seconds into it; the tail becomes a new clip at i+1.
static void SplitClipIn(std::vector<std::unique_ptr<Clip>>& v, int i, double off) {
    Clip& c = *v[i];
    auto t = std::make_unique<Clip>();
    t->kind = c.kind;
    t->path = c.path;
    t->label = c.label;
    t->srcW = c.srcW; t->srcH = c.srcH;
    t->texAspect = c.texAspect;
    t->text = c.text;
    t->textScale = c.textScale;
    t->vid = c.vid;
    t->useAudio = c.useAudio;
    t->reversed = c.reversed;
    t->group = c.group;
    t->skip = c.skip;
    t->lanchor = c.lanchor;
    t->look = c.look;
    t->look43 = c.look43;
    t->grade = c.grade;
    t->tex = c.tex;
    t->ovlOn = c.ovlOn; t->ovlText = c.ovlText; t->ovlScale = c.ovlScale;
    t->ovlX = c.ovlX; t->ovlY = c.ovlY; t->ovlAlpha = c.ovlAlpha; t->ovlShadow = c.ovlShadow;
    memcpy(t->ovlCol, c.ovlCol, sizeof(c.ovlCol));
    t->dxOn = c.dxOn; t->dxPath = c.dxPath; t->dxLabel = c.dxLabel;
    t->dxIsVideo = c.dxIsVideo; t->dxBlend = c.dxBlend; t->dxAmount = c.dxAmount;
    t->dxVid = c.dxVid; t->dxAspect = c.dxAspect;
    t->dxTrimIn = c.dxTrimIn + (c.dxIsVideo ? off : 0.0);
    t->dxTex = c.dxTex;
    if (t->dxTex && !t->dxIsVideo) t->dxTex->AddRef();
    // still thumbnails are refcounted per clip; video posters belong to the proxy cache
    if (t->tex && t->kind != Clip::Video) t->tex->AddRef();
    t->trimIn = c.trimIn + (c.kind == Clip::Video ? off : 0.0);
    t->duration = c.duration - off;
    t->lblend = c.lblend;
    t->lopacity = c.lopacity;
    t->lfit = c.lfit;
    t->start = c.start + off;              // only meaningful on an overlay track
    c.duration = off;
    v.insert(v.begin() + i + 1, std::move(t));
}
static void SplitClip(int i, double off) { SplitClipIn(g_clips, i, off); }

// Cut a folded sequence in two at `off` seconds into it. The first half keeps the
// sequence it had; everything past the cut - shots, layers, sound - moves into a
// new sequence that gets its own nest clip right after. Fold and unfold still put
// it back the way it was.
static void RefreshNestDurations();

static bool SplitNest(int index, double off) {
    if (index < 0 || index >= (int)g_clips.size()) return false;
    Clip& n = *g_clips[index];
    if (n.kind != Clip::Nest) return false;
    Sequence* q = FindSeq(n.nest);
    if (!q) return false;
    const double MIN = MinClipDur();
    if (off <= MIN || off >= n.duration - MIN) return false;

    char nm[64];
    snprintf(nm, sizeof(nm), "seq %d", g_seqNext);
    Sequence* q2 = MakeSeq(nm);
    q = FindSeq(n.nest);                    // MakeSeq may have moved the vector

    // base cut: split the shot the blade lands in, then hand the tail over
    std::vector<BaseSpan> lay;
    BaseLayoutIn(q->clips, lay);
    int k = (int)q->clips.size();
    for (int i = 0; i < (int)q->clips.size(); i++) {
        if (q->clips[i]->skip) continue;
        if (off <= lay[i].start + 1e-4) { k = i; break; }
        if (off < lay[i].end - 1e-4) {
            // A nest inside a nest keeps its own shape: the cut falls on whichever
            // of its two edges is nearer rather than tearing it in half.
            if (q->clips[i]->kind == Clip::Nest) {
                double mid = (lay[i].start + lay[i].end) * 0.5;
                k = off < mid ? i : i + 1;
            } else {
                SplitClipIn(q->clips, i, off - lay[i].start);
                k = i + 1;
            }
            break;
        }
    }
    for (int i = k; i < (int)q->clips.size(); i++) q2->clips.push_back(std::move(q->clips[i]));
    q->clips.resize(k);

    // layers: a clip straddling the cut is split, the rest travels whole
    for (auto& t : q->over) {
        auto nt = std::make_unique<VideoTrack>();
        nt->name = t->name; nt->visible = t->visible; nt->height = t->height;
        for (int i = (int)t->clips.size() - 1; i >= 0; i--) {
            Clip& c = *t->clips[i];
            if (c.start + c.duration <= off + 1e-9) continue;
            int take = i;
            if (c.start < off - 1e-9) {                   // straddles the cut
                SplitClipIn(t->clips, i, off - c.start);
                take = i + 1;
            }
            auto mv = std::move(t->clips[take]);
            t->clips.erase(t->clips.begin() + take);
            mv->start -= off;
            if (mv->start < 0) mv->start = 0;
            nt->clips.push_back(std::move(mv));
        }
        q2->over.push_back(std::move(nt));
    }

    // sound: same rule, on trims instead of durations
    {
        MixGuard lock;
        for (auto& t : q->atracks) {
            auto nt = std::make_unique<AudioTrack>();
            nt->name = t->name; nt->volume = t->volume;
            nt->mute = t->mute; nt->height = t->height;
            nt->fx = t->fx; nt->fxMix = t->fxMix;
            for (int i = (int)t->blocks.size() - 1; i >= 0; i--) {
                Song& b = *t->blocks[i];
                double len = b.trimEnd - b.trimStart;
                if (b.offset + len <= off + 1e-9) continue;
                if (b.offset < off - 1e-9) {              // straddles the cut
                    auto tail = std::make_unique<Song>(b);
                    tail->uid = g_uidNext++;
                    tail->trimStart = b.trimStart + (off - b.offset);
                    tail->offset = 0;
                    tail->fadeIn = 0;                     // the cut is not a fade
                    b.trimEnd = tail->trimStart;
                    b.fadeOut = 0;
                    nt->blocks.push_back(std::move(tail));
                    continue;
                }
                auto mv = std::move(t->blocks[i]);
                t->blocks.erase(t->blocks.begin() + i);
                mv->offset -= off;
                nt->blocks.push_back(std::move(mv));
            }
            q2->atracks.push_back(std::move(nt));
        }
    }

    auto nc = std::make_unique<Clip>();
    nc->kind = Clip::Nest;
    nc->nest = q2->id;
    nc->label = q2->name;
    nc->duration = n.duration - off;
    nc->reversed = n.reversed;
    nc->skip = n.skip;
    nc->lanchor = n.lanchor;
    nc->look = n.look;
    nc->look43 = n.look43;
    nc->grade = n.grade;
    nc->lblend = n.lblend;
    nc->lopacity = n.lopacity;
    nc->lfit = n.lfit;
    nc->start = n.start + off;
    g_clips[index]->duration = off;
    g_clips.insert(g_clips.begin() + index + 1, std::move(nc));
    RefreshNestDurations();
    g_intakeStatus = "sequence split";
    return true;
}

// Timeline index where a clip inserted at time t would go, splitting whatever
// clip straddles t. Used for injecting cards mid-shot.
static int SplitPoint(double t) {
    const double EPS = 1e-4;
    double s = 0;
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    for (int i = 0; i < (int)g_clips.size(); i++) {
        if (g_clips[i]->skip) continue;                            // no time, no cut
        if (t <= lay[i].start + EPS) return i;
        if (t < lay[i].end - EPS) {
            // A sequence is a clip like any other: cutting it cuts what it holds.
            if (g_clips[i]->kind == Clip::Nest) {
                if (!SplitNest(i, t - lay[i].start)) return i;
            } else {
                SplitClip(i, t - lay[i].start);
            }
            return i + 1;
        }
    }
    return (int)g_clips.size();
}

// ---- ripple off: gaps
// A gap is an empty card - a plain black frame - named so it can be found again.
static bool IsGap(const Clip& c) {
    return c.kind == Clip::Text && c.text.empty() && c.label == "gap";
}

// Put `dur` seconds of gap at base index `at`, folded into a gap already beside it
// so repeated trims do not leave a row of slivers.
static void InsertGap(int at, double dur) {
    // Even a single deleted frame leaves its gap: ripple off means nothing moves.
    if (dur < 1e-6) return;
    if (at > 0 && at - 1 < (int)g_clips.size() && IsGap(*g_clips[at - 1])) {
        g_clips[at - 1]->duration += dur;
        return;
    }
    if (at >= 0 && at < (int)g_clips.size() && IsGap(*g_clips[at])) {
        g_clips[at]->duration += dur;
        return;
    }
    auto g = std::make_unique<Clip>();
    g->kind = Clip::Text;
    g->label = "gap";
    g->duration = dur;
    if (at < 0) at = 0;
    if (at > (int)g_clips.size()) at = (int)g_clips.size();
    g_clips.insert(g_clips.begin() + at, std::move(g));
}

static void EraseBaseClip(int i) {
    Clip& c = *g_clips[i];
    if (c.kind != Clip::Video) RetireTexture(c.tex);
    ClearDoubleExposure(c);
    g_clips.erase(g_clips.begin() + i);
}

// Overwrite: a shot growing into its neighbours eats them instead of pushing them.
// Walks from base index `from` - forward trims heads, backward trims tails - taking
// `want` seconds: shots wholly covered go, the one the edge lands in is trimmed and
// keeps the right piece of its source. Muted shots have no width and are stepped
// over; a sequence owns its length, so the walk stops there. Returns what is left
// over, which can only push.
static double OverwriteNeighbours(int from, double want, bool forward) {
    int j = from;
    while (want > 1e-9 && j >= 0 && j < (int)g_clips.size()) {
        Clip& n = *g_clips[j];
        if (n.skip) { j += forward ? 1 : -1; continue; }
        if (n.kind == Clip::Nest) break;
        if (want >= n.duration - MinClipDur() * 0.5) {
            want = fmax(0.0, want - n.duration);
            EraseBaseClip(j);
            if (!forward) j--;
            continue;
        }
        // Losing the head of a forward shot, or the tail of a reversed one, is losing
        // the start of its source window.
        if (n.kind == Clip::Video && forward != n.reversed) n.trimIn += want;
        n.duration -= want;
        want = 0;
    }
    return want;
}

// Neighbouring gaps become one, and a gap at the very end is no film at all.
static void MergeGaps() {
    for (int i = (int)g_clips.size() - 1; i > 0; i--)
        if (IsGap(*g_clips[i]) && IsGap(*g_clips[i - 1])) {
            g_clips[i - 1]->duration += g_clips[i]->duration;
            g_clips.erase(g_clips.begin() + i);
        }
    while (!g_clips.empty() && IsGap(*g_clips.back())) g_clips.pop_back();
}

// Base index of the first shot at or after time t, splitting the shot t falls inside
// so the cut lands exactly there. A sequence is not torn: t goes to its nearer edge.
static int CutBaseAt(double t) {
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    for (int i = 0; i < (int)g_clips.size(); i++) {
        if (g_clips[i]->skip) continue;
        if (t <= lay[i].start + 1e-4) return i;
        if (t < lay[i].end - 1e-4) {
            if (g_clips[i]->kind == Clip::Nest)
                return t - lay[i].start < lay[i].end - t ? i : i + 1;
            SplitClip(i, t - lay[i].start);
            return i + 1;
        }
    }
    return (int)g_clips.size();
}

// Ripple off, a shot dragged along the base track: lift it out, leaving its time
// behind as a gap, and lay it down at `at` over whatever is there. Nothing else on
// the film moves. Returns the shot's new index.
static int LayDownOverwrite(std::unique_ptr<Clip> mv, double at);
static int PlaceOverwrite(int idx, double at) {
    if (idx < 0 || idx >= (int)g_clips.size()) return -1;
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    double len = lay[idx].end - lay[idx].start;
    auto mv = std::move(g_clips[idx]);
    g_clips.erase(g_clips.begin() + idx);
    InsertGap(idx, len);
    return LayDownOverwrite(std::move(mv), at);
}

// Lay a shot - lifted off the base track or brought down from a layer - onto the base
// track at `at` seconds, covering whatever is there. Nothing else moves.
static int LayDownOverwrite(std::unique_ptr<Clip> mv, double at) {
    double len = mv->duration;
    mv->xfade = 0;                         // what it dissolved into is not beside it now
    const int uid = mv->uid;
    if (at < 0) at = 0;
    double total = TotalDuration();
    if (at >= total - 1e-6) {              // past the end: a gap up to it, then the shot
        InsertGap((int)g_clips.size(), at - total);
        g_clips.push_back(std::move(mv));
    } else {
        int a = CutBaseAt(at);
        int b = CutBaseAt(at + len);       // splits at or after a, so a stays put
        for (int i = b - 1; i >= a; i--)
            if (!g_clips[i]->skip) EraseBaseClip(i);
        int ins = a;
        if (ins > (int)g_clips.size()) ins = (int)g_clips.size();
        g_clips.insert(g_clips.begin() + ins, std::move(mv));
    }
    MergeGaps();
    for (int i = 0; i < (int)g_clips.size(); i++)
        if (g_clips[i]->uid == uid) return i;
    return -1;
}

static void RippleOthers(double fromTime, double shift) {
    if (fabs(shift) < 1e-9) return;
    for (auto& m : g_editMarkers)
        if (m.seq == CurSeqId() && m.time >= fromTime - 1e-9)
            m.time = fmax(0.0, m.time + shift);
    for (auto& t : g_over)
        for (auto& c : t->clips)
            if (c->start >= fromTime - 1e-9) {
                c->start += shift;
                if (c->start < 0) c->start = 0;
            }
    MixGuard lock;
    for (auto& t : g_atracks)
        for (auto& b : t->blocks)
            if (b->offset >= fromTime - 1e-9) b->offset += shift;
}

// ------------------------------------------------------- scene-change split

// ffmpeg's `select=gt(scene,T)` scores every frame against the previous one;
// 0.0 = identical, 1.0 = nothing in common. Real hard cuts land around 0.4+,
// fast camera moves around 0.15, so 0.30 catches cuts without shredding pans.
static float  g_sceneThresh = 0.30f;
static double g_sceneMinLen = 0.40;        // drop cuts closer together than this
static int    g_sceneMax    = 200;         // safety cap on new clips per run

struct SceneJob {
    std::future<std::vector<double>> fut;
    int         clipIndex = -1;
    int         track = -1;                // -1 base video, >= 0 overlay track
    bool        active = false;
    std::string status;
} g_sceneJob;

// Times (relative to the clip's in-point) where ffmpeg saw a cut.
static std::vector<double> DetectScenes(std::wstring path, double ss, double dur,
                                        float thresh) {
    std::vector<double> cuts;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring log = std::wstring(tmp) + L"slidecut_scenes.txt";
    wchar_t opt[128];
    swprintf(opt, 128, L" -ss %.4f -t %.4f -i ", ss, dur);
    wchar_t vf[160];
    // Single quotes protect the comma from the filtergraph separator — no backslash.
    swprintf(vf, 160, L" -vf \"select='gt(scene,%.4f)',showinfo\" -an -sn -f null -",
             (double)thresh);
    std::wstring cmd = L"ffmpeg -hide_banner -nostdin -v info" + std::wstring(opt) +
                       L"\"" + path + L"\"" + vf;
    RunHidden(cmd, log);
    std::string s = ReadTextFile(log);
    for (size_t p = s.find("pts_time:"); p != std::string::npos;
         p = s.find("pts_time:", p + 1)) {
        double t = atof(s.c_str() + p + 9);
        if (t > 0) cuts.push_back(t);
    }
    std::sort(cuts.begin(), cuts.end());
    return cuts;
}

// Which clip vector a track index refers to; nullptr when the track is gone.
// ---- stepping in and out of a sequence
// The editor's lists are a working copy: commit puts them back where they came
// from, load pulls another level in. Both take the mix lock, because the audio
// thread walks g_atracks.
static void CommitLevel() {
    EnsureRootSeq();
    Sequence* q = CurSeq();
    if (!q) return;
    q->playhead = g_playhead.load();
    MixGuard lock;
    q->clips = std::move(g_clips);
    q->over = std::move(g_over);
    q->atracks = std::move(g_atracks);
    g_clips.clear(); g_over.clear(); g_atracks.clear();
}

static void LoadLevel(int id, bool resetSel = true) {
    Sequence* q = FindSeq(id);
    if (!q) return;
    {
        MixGuard lock;
        g_clips = std::move(q->clips);
        g_over = std::move(q->over);
        g_atracks = std::move(q->atracks);
        q->clips.clear(); q->over.clear(); q->atracks.clear();
    }
    // The serialiser round-trips the working copy every frame; moving the head
    // there too would fight whatever is advancing it, so both are opt-in.
    if (resetSel) g_playhead.store(q->playhead);
    if (resetSel) {
        g_sel = -1; g_selTrack = -1; g_selAT = -1;
        g_selUids.clear();
    }
}

// Where a sequence's audio tracks live right now: the working copy if you are
// standing in it, the sequence itself otherwise.
static std::vector<std::unique_ptr<AudioTrack>>* ATracksOf(int seqId) {
    if (seqId == CurSeqId()) return &g_atracks;
    Sequence* q = FindSeq(seqId);
    return q ? &q->atracks : nullptr;
}

// A sequence is as long as its base cut. Nest clips carry a cached length so this
// stays a plain sum; RefreshNestDurations is what keeps the cache honest.
// A sequence lasts as long as the last thing in it on any track: folding two
// layer clips with no base cut still has to hold their span open, or the nest
// standing for them would collapse to nothing.
static double SeqDurationOf(const Sequence& q) {
    double t = 0;
    for (auto& c : q.clips) t += TimeLen(*c);
    for (auto& tr : q.over)
        for (auto& c : tr->clips) {
            if (c->skip) continue;
            double e = c->start + c->duration;
            if (e > t) t = e;
        }
    for (auto& tr : q.atracks)
        for (auto& b : tr->blocks) {
            double e = b->offset + (b->trimEnd - b->trimStart);
            if (e > t) t = e;
        }
    return t;
}
static double SeqDuration(int id) {
    if (id == CurSeqId()) return TimelineEnd();
    Sequence* q = FindSeq(id);
    return q ? SeqDurationOf(*q) : 0.0;
}

// Walk every level and stretch each Nest clip to the sequence it stands for.
// Repeated until nothing moves, so a change deep down reaches the root.
static void RefreshNestDurations() {
    for (size_t pass = 0; pass <= g_seqs.size(); pass++) {
        bool changed = false;
        auto fix = [&](std::vector<std::unique_ptr<Clip>>& v) {
            for (auto& c : v) {
                if (c->kind != Clip::Nest) continue;
                double d = SeqDuration(c->nest);
                if (d < 0.04) d = 0.04;                  // an empty sequence still shows
                if (fabs(d - c->duration) > 1e-6) { c->duration = d; changed = true; }
            }
        };
        fix(g_clips);
        for (auto& t : g_over) fix(t->clips);
        for (auto& q : g_seqs) {
            fix(q->clips);
            for (auto& t : q->over) fix(t->clips);
        }
        if (!changed) break;
    }
}

static void EnterSeq(int id) {
    if (id == CurSeqId()) return;
    for (int n : g_nav) if (n == id) return;             // never nest inside itself
    if (!FindSeq(id)) return;
    CommitLevel();
    RefreshNestDurations();
    g_nav.push_back(id);
    LoadLevel(id);
    g_intakeStatus = "inside " + std::string(FindSeq(id)->name);
}

// Pop back up to a depth: 0 is the root reel, 1 the first sequence you opened.
static void NavToDepth(int depth) {
    if (depth < 0) depth = 0;
    if (depth >= (int)g_nav.size()) return;
    CommitLevel();
    g_nav.resize(depth + 1);
    RefreshNestDurations();
    LoadLevel(g_nav.back());
    Sequence* q = CurSeq();
    g_intakeStatus = q ? ("back in " + q->name) : std::string();
}
static void ExitSeq() { if (NestDepth() > 0 || !g_nav.empty()) NavToDepth((int)g_nav.size() - 2); }

// ---- folding a run of shots into a sequence
// Only the base cut folds: the shots have to be next to each other, because the
// Nest clip that replaces them occupies one continuous slot in the cut.
static void FoldSelection() {
    EnsureRootSeq();
    int loBase = -1, hiBase = -1, nBase = 0;
    for (int i = 0; i < (int)g_clips.size(); i++)
        if (SelHas(g_clips[i]->uid)) { if (loBase < 0) loBase = i; hiBase = i; nBase++; }

    std::vector<std::pair<int, int>> selOver;
    for (int t = 0; t < (int)g_over.size(); t++) {
        for (int i = 0; i < (int)g_over[t]->clips.size(); i++) {
            if (SelHas(g_over[t]->clips[i]->uid)) selOver.push_back({t, i});
        }
    }

    std::vector<std::pair<int, int>> selAud;
    for (int t = 0; t < (int)g_atracks.size(); t++) {
        for (int i = 0; i < (int)g_atracks[t]->blocks.size(); i++) {
            if (SelHas(g_atracks[t]->blocks[i]->uid)) selAud.push_back({t, i});
        }
    }

    if (nBase == 0 && selOver.empty() && selAud.empty()) {
        if (g_selTrack == -1 && g_sel >= 0 && g_sel < (int)g_clips.size()) {
            loBase = hiBase = g_sel; nBase = 1;
        } else if (g_selTrack >= 0 && g_selTrack < (int)g_over.size() && g_sel >= 0 && g_sel < (int)g_over[g_selTrack]->clips.size()) {
            selOver.push_back({g_selTrack, g_sel});
        } else if (g_selTrack == -2 && g_selAT >= 0 && g_selAT < (int)g_atracks.size() && g_sel >= 0 && g_sel < (int)g_atracks[g_selAT]->blocks.size()) {
            selAud.push_back({g_selAT, g_sel});
        } else {
            g_intakeStatus = "pick shots to fold"; return;
        }
    }

    if (nBase > 0 && hiBase - loBase + 1 != nBase) {
        g_intakeStatus = "fold needs base shots that sit next to each other"; return;
    }

    char nm[64];
    snprintf(nm, sizeof(nm), "seq %d", g_seqNext);
    Sequence* q = MakeSeq(nm);

    double startTime = 0.0;
    double endTime = 0.0;
    bool hasBounds = false;

    if (nBase > 0) {
        double acc = 0;
        for (int i = 0; i < loBase; i++) acc += g_clips[i]->duration;
        startTime = acc;
        double dur = 0;
        for (int i = loBase; i <= hiBase; i++) dur += g_clips[i]->duration;
        endTime = startTime + dur;
        hasBounds = true;
    } else {
        for (auto& p : selOver) {
            Clip& c = *g_over[p.first]->clips[p.second];
            if (!hasBounds || c.start < startTime) { startTime = c.start; hasBounds = true; }
            if (!hasBounds || c.start + c.duration > endTime) { endTime = c.start + c.duration; hasBounds = true; }
        }
        for (auto& p : selAud) {
            Song& s = *g_atracks[p.first]->blocks[p.second];
            double blockLen = s.trimEnd - s.trimStart;
            if (!hasBounds || s.offset < startTime) { startTime = s.offset; hasBounds = true; }
            if (!hasBounds || s.offset + blockLen > endTime) { endTime = s.offset + blockLen; hasBounds = true; }
        }
    }

    std::sort(selOver.begin(), selOver.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });

    for (auto& p : selOver) {
        int t = p.first;
        int i = p.second;
        auto c = std::move(g_over[t]->clips[i]);
        g_over[t]->clips.erase(g_over[t]->clips.begin() + i);
        c->group = 0;
        c->start -= startTime;
        while (q->over.size() <= t) {
            auto tr = std::make_unique<VideoTrack>();
            tr->name = "layer " + std::to_string(q->over.size() + 1);
            q->over.push_back(std::move(tr));
        }
        q->over[t]->clips.push_back(std::move(c));
    }

    std::sort(selAud.begin(), selAud.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });

    for (auto& p : selAud) {
        int t = p.first;
        int i = p.second;
        auto s = std::move(g_atracks[t]->blocks[i]);
        g_atracks[t]->blocks.erase(g_atracks[t]->blocks.begin() + i);
        s->group = 0;
        s->offset -= startTime;
        while (q->atracks.size() <= t) {
            auto tr = std::make_unique<AudioTrack>();
            tr->name = "sound " + std::to_string(q->atracks.size() + 1);
            q->atracks.push_back(std::move(tr));
        }
        q->atracks[t]->blocks.push_back(std::move(s));
    }

    auto nc = std::make_unique<Clip>();
    nc->kind = Clip::Nest;
    nc->nest = q->id;
    nc->label = q->name;

    if (nBase > 0) {
        for (int i = loBase; i <= hiBase; i++) {
            g_clips[i]->group = 0;
            q->clips.push_back(std::move(g_clips[i]));
        }
        g_clips.erase(g_clips.begin() + loBase, g_clips.begin() + hiBase + 1);
        double baseDur = 0;
        for (auto& c : q->clips) baseDur += c->duration;
        nc->duration = baseDur;
        g_clips.insert(g_clips.begin() + loBase, std::move(nc));
        g_sel = loBase; g_selTrack = -1;
    } else {
        nc->duration = endTime - startTime;
        nc->start = startTime;
        int targetTrack = selOver.empty() ? 0 : selOver.back().first;
        g_over[targetTrack]->clips.push_back(std::move(nc));
        g_sel = (int)g_over[targetTrack]->clips.size() - 1;
        g_selTrack = targetTrack;
    }

    g_selUids.clear();
    int n = nBase + (int)selOver.size() + (int)selAud.size();
    g_intakeStatus = std::string(nm) + " — " + std::to_string(n) + " items folded";
}

// A brand new empty sequence, dropped into the cut at the playhead and opened.
static void NewSeqAtPlayhead() {
    EnsureRootSeq();
    char nm[64];
    snprintf(nm, sizeof(nm), "seq %d", g_seqNext);
    Sequence* q = MakeSeq(nm);
    auto nc = std::make_unique<Clip>();
    nc->kind = Clip::Nest;
    nc->nest = q->id;
    nc->label = q->name;
    nc->duration = 0.04;
    double ph = g_playhead.load(), acc = 0;
    int at = (int)g_clips.size();
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    for (int i = 0; i < (int)g_clips.size(); i++) {       // nearest cut, never mid-shot
        if (ph < (lay[i].start + lay[i].end) * 0.5) { at = i; break; }
    }
    (void)acc;
    g_clips.insert(g_clips.begin() + at, std::move(nc));
    int id = q->id;
    EnterSeq(id);
}

// Tear a sequence back open in place: its shots return to the cut, its layers and
// its music come along shifted to where the nest sat.
static void UnfoldNest(int index) {
    if (index < 0 || index >= (int)g_clips.size()) return;
    if (g_clips[index]->kind != Clip::Nest) return;
    Sequence* q = FindSeq(g_clips[index]->nest);
    if (!q) return;
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    double at = lay[index].start;
    double dur = g_clips[index]->duration;
    bool rev = g_clips[index]->reversed;

    g_clips.erase(g_clips.begin() + index);
    int k = index;
    if (rev) std::reverse(q->clips.begin(), q->clips.end());
    for (auto& c : q->clips) {
        if (rev) c->reversed = !c->reversed;
        g_clips.insert(g_clips.begin() + k++, std::move(c));
    }
    q->clips.clear();
    for (auto& t : q->over) {
        int dst = NewOverlayTrack();
        g_over[dst]->visible = t->visible;     // a hidden layer stays hidden
        for (auto& c : t->clips) {
            if (rev) {
                c->start = dur - (c->start + c->duration);
                c->reversed = !c->reversed;
            }
            c->start += at; 
            g_over[dst]->clips.push_back(std::move(c)); 
        }
    }
    q->over.clear();
    {
        MixGuard lock;
        for (auto& t : q->atracks) {
            g_atracks.push_back(std::move(t));
            for (auto& b : g_atracks.back()->blocks) {
                if (rev) {
                    b->offset = dur - (b->offset + b->duration);
                    b->reversed = !b->reversed;
                }
                b->offset += at;
            }
        }
        q->atracks.clear();
    }
    g_intakeStatus = q->name + " unfolded";
    g_sel = index; g_selTrack = -1;
    g_selUids.clear();
}

// The same tear-open on an overlay track: the sequence's own cut is laid out
// along the track from where the nest sat, and its layers and sound come up
// beside it. Folding the run again puts it back.
static void UnfoldNestOnTrack(int track, int index) {
    if (track < 0 || track >= (int)g_over.size()) return;
    auto& v = g_over[track]->clips;
    if (index < 0 || index >= (int)v.size() || v[index]->kind != Clip::Nest) return;
    Sequence* q = FindSeq(v[index]->nest);
    if (!q) return;
    double at = v[index]->start;
    double dur = v[index]->duration;
    int blend = v[index]->lblend;
    float opacity = v[index]->lopacity;
    int lfit = v[index]->lfit;
    bool rev = v[index]->reversed;

    v.erase(v.begin() + index);
    if (rev) std::reverse(q->clips.begin(), q->clips.end());
    double run = at;
    for (auto& c : q->clips) {
        if (rev) c->reversed = !c->reversed;
        c->start = run;
        c->lblend = blend;
        c->lopacity = opacity;
        c->lfit = lfit;
        run += c->duration;
        v.push_back(std::move(c));
    }
    q->clips.clear();
    for (auto& t : q->over) {
        int dst = NewOverlayTrack();
        g_over[dst]->visible = t->visible;     // a hidden layer stays hidden
        for (auto& c : t->clips) {
            if (rev) {
                c->start = dur - (c->start + c->duration);
                c->reversed = !c->reversed;
            }
            c->start += at;
            g_over[dst]->clips.push_back(std::move(c));
        }
    }
    q->over.clear();
    {
        MixGuard lock;
        for (auto& t : q->atracks) {
            g_atracks.push_back(std::move(t));
            for (auto& b : g_atracks.back()->blocks) {
                if (rev) {
                    b->offset = dur - (b->offset + b->duration);
                    b->reversed = !b->reversed;
                }
                b->offset += at;
            }
        }
        q->atracks.clear();
    }
    g_intakeStatus = q->name + " unfolded";
    g_sel = -1; g_selTrack = -1;
    g_selUids.clear();
}

// ---- what a nest shows at a given moment
// Base picture first, then the nested overlay tracks bottom-up, each with the
// blend and opacity it carries inside. Recurses, so a nest inside a nest resolves.
// Mute is a cut decision, not a delete: the shot keeps its trim, its layers and
// its place in the order, and the film simply runs past it. Toggling is all-or-
// nothing over the selection, so a half-muted sequence resolves to muted.
static void MuteSelection() {
    std::vector<Clip*> hit;
    auto pick = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& c : v) if (SelHas(c->uid)) hit.push_back(c.get());
    };
    pick(g_clips);
    for (auto& t : g_over) pick(t->clips);
    if (hit.empty()) { Clip* c = SelectedClip(); if (c) hit.push_back(c); }
    if (hit.empty()) { g_intakeStatus = "pick a shot to mute"; return; }

    bool anyLive = false;
    for (Clip* c : hit) if (!c->skip) anyLive = true;
    for (Clip* c : hit) c->skip = anyLive;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d shot%s", anyLive ? "muted" : "unmuted",
             (int)hit.size(), hit.size() == 1 ? "" : "s");
    g_intakeStatus = buf;
}

static int  g_navEnter = -1;                // sequence id to step into
static int  g_navDepth = -1;                // depth to pop back out to
static bool g_navFold = false;              // fold the selection into a sequence
static bool g_navNew = false;               // start a fresh empty sequence
static bool g_muteToggle = false;           // mute or unmute the selection
static std::wstring g_vaultRestore;         // a shelf copy the panel asked for
static bool RestoreFromVault(const std::wstring& path);
static int  g_navUnfold = -1;               // index of a nest to tear open
static int  g_navUnfoldTrack = -1;          // which track it sits on, -1 = base

static void ApplyNavRequests() {
    if (g_navFold)        { g_navFold = false;   FoldSelection(); RefreshNestDurations(); }
    if (g_navNew)         { g_navNew = false;    NewSeqAtPlayhead(); }
    if (g_muteToggle)     { g_muteToggle = false; MuteSelection(); }
    if (!g_vaultRestore.empty()) {
        std::wstring path = g_vaultRestore;
        g_vaultRestore.clear();
        RestoreFromVault(path);
    }
    if (g_navUnfold >= 0) {
        int i = g_navUnfold, t = g_navUnfoldTrack;
        g_navUnfold = -1; g_navUnfoldTrack = -1;
        if (t < 0) UnfoldNest(i); else UnfoldNestOnTrack(t, i);
        RefreshNestDurations();
    }
    if (g_navEnter >= 0)  { int id = g_navEnter; g_navEnter = -1; EnterSeq(id); }
    if (g_navDepth >= 0)  { int d = g_navDepth;  g_navDepth = -1; NavToDepth(d); }
}

struct NestHit { Clip* clip; double local; int mode; float opacity; };
static void NestResolve(Clip& c, double local, std::vector<NestHit>& out, int depth = 0) {
    if (depth > 8) return;                                // paranoia, not a real case
    if (c.kind != Clip::Nest) return;
    if (c.reversed) local = c.duration - local;
    const std::vector<std::unique_ptr<Clip>>* base = nullptr;
    const std::vector<std::unique_ptr<VideoTrack>>* over = nullptr;
    Sequence* q = FindSeq(c.nest);
    if (c.nest == CurSeqId()) { base = &g_clips; over = &g_over; }
    else if (q) { base = &q->clips; over = &q->over; }
    if (!base) return;
    double acc = 0;
    for (auto& b : *base) {
        if (b->skip) continue;                       // muted inside a sequence too
        if (local < acc + b->duration) {
            if (b->kind == Clip::Nest) NestResolve(*b, local - acc, out, depth + 1);
            else out.push_back({ b.get(), local - acc, 0, 1.0f });
            break;
        }
        acc += b->duration;
    }
    if (!over) return;
    for (auto& t : *over) {
        if (!t->visible) continue;
        for (auto& o : t->clips) {
            if (o->skip) continue;
            if (local < o->start || local >= o->start + o->duration) continue;
            if (o->kind == Clip::Nest) NestResolve(*o, local - o->start, out, depth + 1);
            else out.push_back({ o.get(), local - o->start, o->lblend, o->lopacity });
        }
    }
}

// The same walk, one level deep: an inner nest comes back as a Nest clip rather
// than being opened out. A sequence composites onto its own canvas and that canvas
// is blended into its parent exactly once, so whoever renders it has to be able to
// stop at the sequence boundary.
static void NestChildren(Clip& c, double local, std::vector<NestHit>& out) {
    if (c.kind != Clip::Nest) return;
    if (c.reversed) local = c.duration - local;
    const std::vector<std::unique_ptr<Clip>>* base = nullptr;
    const std::vector<std::unique_ptr<VideoTrack>>* over = nullptr;
    Sequence* q = FindSeq(c.nest);
    if (c.nest == CurSeqId()) { base = &g_clips; over = &g_over; }
    else if (q) { base = &q->clips; over = &q->over; }
    if (!base) return;
    double acc = 0;
    for (auto& b : *base) {
        if (b->skip) continue;
        if (local < acc + b->duration) {
            out.push_back({ b.get(), local - acc, 0, 1.0f });   // the cut, always normal
            break;
        }
        acc += b->duration;
    }
    if (!over) return;
    for (auto& t : *over) {
        if (!t->visible) continue;
        for (auto& o : t->clips) {
            if (o->skip) continue;
            if (local < o->start || local >= o->start + o->duration) continue;
            out.push_back({ o.get(), local - o->start, o->lblend, o->lopacity });
        }
    }
}

static std::vector<std::unique_ptr<Clip>>* TrackClips(int track) {
    if (track == -1) return &g_clips;
    if (track >= 0 && track < (int)g_over.size()) return &g_over[track]->clips;
    return nullptr;
}

static void StartSceneSplit(int track, int clipIndex) {
    auto* v = TrackClips(track);
    if (g_sceneJob.active || !v || clipIndex < 0 || clipIndex >= (int)v->size()) return;
    Clip& c = *(*v)[clipIndex];
    if (c.kind != Clip::Video || c.path.empty()) {
        g_sceneJob.status = "Scene detection needs a video clip.";
        return;
    }
    g_sceneJob.clipIndex = clipIndex;
    g_sceneJob.track = track;
    g_sceneJob.active = true;
    g_sceneJob.status = "scanning for scene changes…";
    g_sceneJob.fut = std::async(std::launch::async, DetectScenes,
                                c.path, c.trimIn, c.duration, g_sceneThresh);
}

// Main thread: turn detected cut times into real splits of the source clip.
static void PumpSceneSplit() {
    if (!g_sceneJob.active || !g_sceneJob.fut.valid()) return;
    if (g_sceneJob.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::vector<double> cuts = g_sceneJob.fut.get();
    g_sceneJob.active = false;
    int i = g_sceneJob.clipIndex;
    auto* v = TrackClips(g_sceneJob.track);
    if (!v || i < 0 || i >= (int)v->size()) { g_sceneJob.status = "clip disappeared"; return; }

    double len = (*v)[i]->duration;
    double lastCut = 0;
    int made = 0;
    for (double t : cuts) {
        if (made >= g_sceneMax) break;
        if (t - lastCut < g_sceneMinLen) continue;
        if (len - t < g_sceneMinLen) break;
        // the clip that currently holds the tail is at index i+made
        SplitClipIn(*v, i + made, t - lastCut);
        lastCut = t;
        made++;
    }
    char buf[128];
    if (made == 0) snprintf(buf, sizeof(buf),
                            "No scene changes above %.2f — try a lower threshold.",
                            g_sceneThresh);
    else snprintf(buf, sizeof(buf), "Split into %d clips at threshold %.2f.",
                  made + 1, g_sceneThresh);
    g_sceneJob.status = buf;
    if (g_selTrack == g_sceneJob.track && g_sel > i) g_sel += made;
}

static void PumpPendingLoads() {          // main thread: turn decoded pixels into textures
    std::vector<Clip*> all;
    for (auto& c : g_clips) all.push_back(c.get());
    for (auto& t : g_over) for (auto& c : t->clips) all.push_back(c.get());
    // Sequences are no longer opened out before the render, so their cards live in
    // g_seqs rather than on the working copy.
    for (auto& q : g_seqs) {
        for (auto& c : q->clips) all.push_back(c.get());
        for (auto& t : q->over) for (auto& c : t->clips) all.push_back(c.get());
    }
    for (Clip* c : all) {
        if (c->kind == Clip::Video && c->vid && c->vid->probed.load() && c->srcW == 0) {
            c->srcW = c->vid->w;
            c->srcH = c->vid->h;
            c->texAspect = c->vid->aspect;
            if (g_fpsAuto && c->vid->fps > 0) {         // first footage sets the timebase
                const int std_[] = { 24, 25, 30, 50, 60 };
                int best = 30;
                double bestErr = 1e9;
                for (int s : std_) {
                    double e = fabs(c->vid->fps - s);
                    if (e < bestErr) { bestErr = e; best = s; }
                }
                g_fps = best;
                g_fpsAuto = false;
            }
            if (c->duration <= 0)          // first time: use the whole file
                c->duration = c->vid->duration > 0.05 ? c->vid->duration : 3.0;
        }
        if (c->dxPending.valid() &&
            c->dxPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            LoadedImage dl = c->dxPending.get();
            if (dl.ok) {
                c->dxTex = CreateTextureRGBA(dl.px.data(), dl.w, dl.h);
                c->dxAspect = (float)dl.w / (float)dl.h;
            }
        }
        if (!c->pending.valid()) continue;
        if (c->pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        LoadedImage li = c->pending.get();
        if (li.ok) {
            c->tex = CreateTextureRGBA(li.px.data(), li.w, li.h);
            c->texAspect = (float)li.w / (float)li.h;
            c->srcW = li.srcW; c->srcH = li.srcH;
        }
    }
}

// ------------------------------------------------------------- file dialogs

static std::vector<std::wstring> PickFiles(bool multi, const wchar_t* filterName,
                                           const wchar_t* filterSpec) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (multi ? FOS_ALLOWMULTISELECT : 0));
    COMDLG_FILTERSPEC spec = { filterName, filterSpec };
    dlg->SetFileTypes(1, &spec);
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it))) {
                    PWSTR ps = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &ps))) {
                        out.push_back(ps);
                        CoTaskMemFree(ps);
                    }
                    it->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

// Last folder an export was written to; seeds the next Save dialog.
static std::wstring g_lastExportDir;

static std::wstring PickSaveVideo(const std::wstring& defaultName, const wchar_t* ext,
                                  const std::wstring& suggestDir,
                                  const wchar_t* kindLabel = L" video") {
    std::wstring out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) return out;
    std::wstring filterName = std::wstring(ext) + kindLabel;
    std::wstring filterSpec = L"*." + std::wstring(ext);
    for (auto& ch : filterName) ch = (wchar_t)towupper(ch);
    COMDLG_FILTERSPEC spec = { filterName.c_str(), filterSpec.c_str() };
    dlg->SetFileTypes(1, &spec);
    dlg->SetDefaultExtension(ext);
    dlg->SetFileName(defaultName.c_str());
    const std::wstring& dir = !g_lastExportDir.empty() ? g_lastExportDir : suggestDir;
    if (!dir.empty()) {                    // start next to the footage, not in Documents
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR ps = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &ps))) {
                out = ps;
                CoTaskMemFree(ps);
            }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

// --------------------------------------------------- projector shader (python)

// projector_render.py is a separate renderer: it runs the plate/gate/grain shader
// over a finished file. The app drives it for a single-frame preview and, when
// enabled, as a second pass over the exported video.
static void ResolveCanvas(int* outW, int* outH);

static bool  g_projOn = false;             // apply at render
static float g_projMargin = 0.94f;
static float g_projIntensity = 1.0f;
static float g_projGrain = 0.16f;
static float g_projGrainFps = 24.0f;
static int   g_projFit = 0;                // 0 cover, 1 contain, 2 stretch
static bool  g_projGate = true;
static float g_projGateInset = 0.0f;
static float g_projPlateAr = 0.0f;         // 0 = follow the source

// ---- the aspect track drives that same plate, over time
static void SortAspects() {
    std::stable_sort(g_aspects.begin(), g_aspects.end(),
                     [](const std::unique_ptr<AspectPoint>& a,
                        const std::unique_ptr<AspectPoint>& b) { return a->offset < b->offset; });
}

// The point in force at t: the last one at or before it, -1 when none is.
static int AspectAt(double t) {
    int best = -1;
    for (int i = 0; i < (int)g_aspects.size(); i++)
        if (g_aspects[i]->offset <= t + 1e-9) best = i;
    return best;
}

// Where a point's hold ends on the timeline: the next point, or the end.
static double AspectSpanEnd(int i) {
    if (i + 1 < (int)g_aspects.size()) return g_aspects[i + 1]->offset;
    double tot = TotalDuration();
    double end = g_aspects[i]->offset + 2.0;
    return tot > end ? tot : end;
}

// What the plate should be right now: the point in force, else the panel's value.
static float PlateAspect() {
    int i = AspectAt(g_playhead.load());
    return i >= 0 ? g_aspects[i]->aspect : g_projPlateAr;
}

// A shot that plays through the projector is seen through the plate, not the canvas.
// With the plate covering the canvas at an aspect of its own, only a centred part of
// the canvas (rx by ry of it) ever reaches the gate, so a filling shot crops to that
// part - and its keep anchor picks what the gate shows, instead of being cropped a
// second time, always from the centre, by the plate.
// pAr < 0: the plate showing at the playhead (the preview). The export passes the
// plate in force when the shot is on screen.
static bool PlateCropRegion(float canvasAr, float* rx, float* ry, float pAr = -1.0f) {
    *rx = *ry = 1.0f;
    if (pAr < 0) pAr = PlateAspect();
    if (g_projFit != 0 || pAr <= 0.01f || canvasAr <= 0.01f) return false;
    if (canvasAr > pAr) *rx = pAr / canvasAr;
    else                *ry = canvasAr / pAr;
    return true;
}

// Editing the aspect from the projector panel writes to the point in force; with
// no point in force it stays the plain global it always was.
static void SetPlateAspect(float ar) {
    int i = AspectAt(g_playhead.load());
    if (i >= 0) g_aspects[i]->aspect = ar;
    else        g_projPlateAr = ar;
}

// Drop a point at the playhead carrying whatever the plate is showing now.
static void AddAspectPoint(double at) {
    if (at < 0) at = 0;
    float carry = PlateAspect();
    auto b = std::make_unique<AspectPoint>();
    b->offset = at;
    b->aspect = carry > 0.01f ? carry : 1.777f;
    g_aspects.push_back(std::move(b));
    SortAspects();
}
static float g_projWall[3] = { 0, 0, 0 };
static float g_projTimeOffset = 0.0f;
// Per-effect amounts. 1.0 is the look as the shader was written; 0 removes that
// effect alone, above 1 pushes it past the original.
static float g_fxWeave    = 1.0f;          // gate weave + transport jitter
static float g_fxRipple   = 1.0f;          // curtain ripple
static float g_fxAber     = 1.0f;          // chromatic aberration
static float g_fxHalation = 1.0f;          // ring blur, focus breathing, glow
static float g_fxFlicker  = 1.0f;          // lamp flicker
static float g_fxDust     = 1.0f;          // gate dust specks
static float g_fxHair     = 1.0f;          // hair in the gate
static float g_fxScratch  = 1.0f;          // emulsion scratch
static float g_fxVignette = 1.0f;
static int   g_projCrf = 16;
static char  g_pythonExe[128] = "python";
static const char* PROJ_FIT_ITEMS = "cover\0" "contain\0" "stretch\0";

// projector_render.py sits next to the exe, or up to two levels above it (build dirs).
static std::wstring ProjectorScriptPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const wchar_t* rel[] = { L"", L"..\\", L"..\\..\\", L"..\\..\\..\\" };
    for (const wchar_t* r : rel) {
        std::wstring p = dir + r + L"projector_render.py";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

// The clip shader helper sits wherever projector_render.py does.
static std::wstring ClipShaderScriptPath() {
    std::wstring r = ProjectorScriptPath();
    if (r.empty()) return r;
    size_t cut = r.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return L"";
    return r.substr(0, cut + 1) + L"clip_shader_export.py";
}

static std::wstring ProjectorArgs(int W, int H, bool still) {
    wchar_t buf[900];
    swprintf(buf, 900,
             L" --res %dx%d --fit %ls --margin %.4f --intensity %.4f --grain %.4f"
             L" --grain-fps %.3f --gate-inset %.2f --time-offset %.3f --crf %d"
             L" --wall \"#%02x%02x%02x\" --no-progress"
             L" --weave %.4f --ripple %.4f --aberration %.4f --halation %.4f"
             L" --flicker %.4f --dust %.4f --hair %.4f --scratch %.4f"
             L" --vignette %.4f",
             W, H,
             g_projFit == 1 ? L"contain" : g_projFit == 2 ? L"stretch" : L"cover",
             // grain refreshes once per film frame: any other rate beats against the
             // frames and reads as a regular pattern drifting over the picture
             g_projMargin, g_projIntensity, g_projGrain, (float)g_fps,
             g_projGateInset, g_projTimeOffset, g_projCrf,
             (int)(g_projWall[0] * 255) & 255, (int)(g_projWall[1] * 255) & 255,
             (int)(g_projWall[2] * 255) & 255,
             g_fxWeave, g_fxRipple, g_fxAber, g_fxHalation,
             g_fxFlicker, g_fxDust, g_fxHair, g_fxScratch, g_fxVignette);
    std::wstring s = buf;
    if (!g_projGate) s += L" --no-gate";
    // The panel's own aspect, not whatever the playhead happens to sit on: over time the
    // aspect track reaches the render as aspect.schedule.
    if (g_projPlateAr > 0.01f) {
        wchar_t ar[48];
        swprintf(ar, 48, L" --plate-ar %.5f", g_projPlateAr);
        s += ar;
    }
    if (still) s += L" --audio none";
    return s;
}

// Live preview toggle: the look runs on the GPU (see RenderProjectorGPU), the
// export still hands the finished file to projector_render.py.
static bool g_projLive = false;

static bool WriteWholeFile(const std::wstring& path, const std::string& data);

// Which screen the cut is playing on at time t. A sequence claims the screen for
// everything inside it; otherwise the shot resolved out of it speaks for itself.
static void LookAtTime(double t, int* look, int* pillar) {
    *look = LOOK_PROJECTOR;
    *pillar = 0;
    double start = 0;
    int i = g_baseOff ? -1 : ClipAt(t, &start);
    // Nothing on screen - a gap, a hidden base track, a blank card - with no layer over
    // it: no film either. A projector with nothing in the gate shows no plate, no gate
    // and no grain, so the frame stays the plain black it already is.
    auto blank = [](int k) {
        return k < 0 || k >= (int)g_clips.size() ||
               (g_clips[k]->kind == Clip::Text && g_clips[k]->text.empty());
    };
    bool baseShows = !blank(i);
    if (!baseShows && !g_baseOff) {
        // A crossfade out of a blank card: ClipAt names the card, but the next shot is
        // already fading in over it. Those frames are that shot's, film look and all.
        std::vector<BaseSpan> lay;
        BaseLayout(lay);
        for (int k = 0; k < (int)g_clips.size(); k++) {
            if (g_clips[k]->skip || blank(k) || t < lay[k].start || t >= lay[k].end) continue;
            i = k;
            start = lay[k].start;
            baseShows = true;
            break;
        }
    }
    if (!baseShows) {
        bool layerShows = false;
        for (auto& tr : g_over) {
            if (!tr->visible) continue;
            for (auto& lc : tr->clips)
                if (!lc->skip && t >= lc->start && t < lc->start + lc->duration) layerShows = true;
        }
        if (!layerShows) *look = LOOK_NONE;
        return;
    }
    if (g_baseOff) return;
    Clip& c = *g_clips[i];
    if (c.look != LOOK_PROJECTOR) {
        *look = c.look;
        *pillar = c.look43 ? 1 : 0;
        return;
    }
    if (c.kind != Clip::Nest) return;
    std::vector<NestHit> flat;
    NestResolve(c, t - start, flat);
    for (auto& h : flat) {
        if (h.clip->look != LOOK_PROJECTOR) {
            *look = h.clip->look;
            *pillar = h.clip->look43 ? 1 : 0;
            return;
        }
    }
}

// The same answer for every frame of the cut, run-length encoded as
// "firstFrame frameCount look pillar", so the offline renderer can switch screens
// frame by frame in one pass rather than the file being cut into pieces and joined
// back up. Returns false when the whole cut is the projector and needs no schedule.
static bool LookSchedule(std::string& out) {
    out.clear();
    const int n = (int)(TotalDuration() * g_fps + 0.5);
    bool any = false;
    int runLook = LOOK_PROJECTOR, runPillar = 0, runStart = 0;
    for (int f = 0; f <= n; f++) {
        int look = LOOK_PROJECTOR, pillar = 0;
        if (f < n) LookAtTime((f + 0.5) / g_fps, &look, &pillar);
        if (f == 0) { runLook = look; runPillar = pillar; continue; }
        if (f < n && look == runLook && pillar == runPillar) continue;
        out += std::to_string(runStart) + " " + std::to_string(f - runStart) + " "
             + std::to_string(runLook) + " " + std::to_string(runPillar) + "\n";
        if (runLook != LOOK_PROJECTOR) any = true;
        runLook = look; runPillar = pillar; runStart = f;
    }
    return any;
}


// ------------------------------------------------------------------- export

struct ExportJob {
    HANDLE       process = nullptr;
    std::wstring progressFile;
    std::wstring logFile;
    std::wstring outPath;
    std::string  cmd;                     // utf8 copy of the ffmpeg command, for "Copy"
    double       totalDur = 0;
    float        progress = 0;            // 0..1, or <0 while the stage has no progress
    bool         active = false;
    bool         failed = false;
    std::string  message;
    int          stage = 1;               // 1 = ffmpeg encode, 2 = projector shader pass
    bool         wantProjector = false;   // run stage 2 when the encode succeeds
    bool         lookSchedule = false;    // some of the cut plays on its own screen
    bool         aspectSchedule = false;  // the aspect track shapes the plate over time
    std::wstring stageTmp;                // stage-2 output, moved over outPath at the end
    bool         toClipboard = false;     // hand the finished file to the OS clipboard
    std::wstring stageExt;                // container the encode was actually built for
    bool         clipShaders = false;     // stage 1 runs per-shot screen passes before ffmpeg
    bool         encoding = false;        // ffmpeg itself has started reporting progress
    std::string  shaderStep;              // last "Clip shader n/N: name" the helper printed
    std::string  encNote;                 // why the encoder differs from the one picked, if it does
    std::wstring stage1;                  // lossless stage-1 intermediate the projector pass reads
    // Stall watch: the final encode has been seen to sit for hours without a frame.
    ULONGLONG    finalAt = 0;             // when the final ffmpeg started (0 = not yet)
    ULONGLONG    progAt = 0;              // when out_time last moved
    double       lastUs = -1;
    ULONGLONG    watchAt = 0;             // last sample of the ffmpeg process
    ULONGLONG    cpuPrev = 0;             // its CPU time then, 100 ns
    DWORD        pidPrev = 0;
    std::string  stallNote;               // shown next to the stage while it lasts
} g_export;

// Temp films rendered for the Windows clipboard. They are ours to clean up, so the
// paths are kept and the files deleted on the way out.
static std::vector<std::wstring> g_clipTemps;

static void SweepClipboardTemps() {
    for (auto& p : g_clipTemps) DeleteFileW(p.c_str());
    g_clipTemps.clear();
}

// A second Ctrl+Shift+C while the copy is still encoding calls it off.
static void CancelClipboardRender() {
    if (!g_export.active || !g_export.toClipboard) return;
    if (g_export.process) {
        TerminateProcess(g_export.process, 1);
        WaitForSingleObject(g_export.process, 2000);
        CloseHandle(g_export.process);
        g_export.process = nullptr;
    }
    if (!g_export.stageTmp.empty()) DeleteFileW(g_export.stageTmp.c_str());
    if (!g_export.stage1.empty()) DeleteFileW(g_export.stage1.c_str());
    if (!g_clipTemps.empty()) {                // the half-written film is no use to anyone
        DeleteFileW(g_clipTemps.back().c_str());
        g_clipTemps.pop_back();
    }
    g_export.active = false;
    g_export.toClipboard = false;
    g_export.stage = 1;
    g_export.progress = 0;
    g_export.failed = false;
    g_export.message = "Copy cancelled";
}

// ---- Windows clipboard: a file, the way Explorer puts one there (CF_HDROP), so
// any app that takes a dropped file takes a paste from us too.
static bool SetClipboardFile(const std::wstring& path) {
    size_t chars = path.size() + 2;                      // list terminator is a second NUL
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    auto* df = (DROPFILES*)GlobalLock(h);
    ZeroMemory(df, bytes);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    if (!OpenClipboard(nullptr)) { GlobalFree(h); return false; }
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, h)) { CloseClipboard(); GlobalFree(h); return false; }
    CloseClipboard();                                    // the clipboard owns h now
    return true;
}

// Output presets. Social targets are fixed canvases; "Original" keeps the largest
// source resolution, "Custom" takes whatever is typed in the settings dialog.
enum ExportPreset {
    PRESET_TIKTOK = 0, PRESET_REELS, PRESET_IG_FEED, PRESET_IG_SQUARE,
    PRESET_YT_1080, PRESET_YT_4K, PRESET_ORIGINAL, PRESET_CUSTOM
};
static const char* PRESET_ITEMS =
    "TikTok  9:16  1080x1920\0"
    "Instagram Reels  9:16  1080x1920\0"
    "Instagram Feed  4:5  1080x1350\0"
    "Instagram Square  1:1  1080x1080\0"
    "YouTube  16:9  1920x1080\0"
    "YouTube 4K  16:9  3840x2160\0"
    "Original size\0"
    "Custom…\0";

// How a source that does not match the canvas aspect is made to fit it.
enum FitMode { FIT_BLUR = 0, FIT_BARS, FIT_CROP };   // the pre-per-clip global
static int LfitFromOldFit(int f) {
    return f == FIT_CROP ? LFIT_FILL : f == FIT_BARS ? LFIT_BLACK : LFIT_BLUR;
}
// Video encoder. NVENC entries need an NVIDIA GPU; when ffmpeg cannot open one the
// export falls back to the matching software encoder (see ResolveEncoder).
enum VCodec { VC_X264 = 0, VC_X265, VC_NVENC_H264, VC_NVENC_HEVC };
enum RateMode { RM_CRF = 0, RM_BITRATE };
enum Container { CT_MP4 = 0, CT_MOV, CT_MKV };

static int   g_preset = PRESET_YT_1080;
static int   g_customW = 1080, g_customH = 1920;
// The export panel's row: not a setting of its own any more, but the fill it stamps
// onto every clip, and what a project saved before clips carried their own is read as.
static int   g_fit = LFIT_FILL;
static int   g_vcodec = VC_X265;
static int   g_speed = 2;                 // index into SPEED_NAMES
static int   g_rateMode = RM_BITRATE;
static int   g_crf = 15;                  // lower = better; 15 is visually lossless-ish
static float g_targetMbps = 10.0f;        // used when g_rateMode == RM_BITRATE
static int   g_container = CT_MP4;
static int   g_abrIdx = 3;                // index into AUDIO_KBPS
static bool  g_loudnorm = false;          // one-pass EBU R128 normalise of the final mix
static bool  g_faststart = true;          // move the moov atom up front (mp4/mov)
static bool  g_keepGrain = false;         // x264/x265 -tune grain: spend bits on noise, don't smooth it
static float g_fadeIn = 0.0f;             // seconds of fade from black / silence
static float g_fadeOut = 0.0f;            // audio levels live on the tracks themselves

static const int AUDIO_KBPS[] = { 128, 192, 256, 320 };
static const char* AUDIO_KBPS_ITEMS = "128 kbps\0" "192 kbps\0" "256 kbps\0" "320 kbps\0";
// x264/x265 names; NVENC uses p1..p7 and is mapped from the same index.
static const wchar_t* SPEED_NAMES[] = { L"veryslow", L"slower", L"slow", L"medium", L"fast" };
static const wchar_t* NVENC_SPEEDS[] = { L"p7", L"p6", L"p5", L"p4", L"p3" };
static const char* SPEED_ITEMS =
    "veryslow (smallest)\0" "slower\0" "slow\0" "medium\0" "fast\0";
static const char* CODEC_ITEMS =
    "H.264  (libx264)\0" "H.265  (libx265)\0" "H.264  NVENC (GPU)\0" "H.265  NVENC (GPU)\0";
static const char* FIT_ITEMS = "Blur fill\0" "Black bars\0" "Crop to fill\0";
static const char* CONTAINER_ITEMS = "MP4\0" "MOV\0" "MKV\0";

static bool IsNvenc(int c) { return c == VC_NVENC_H264 || c == VC_NVENC_HEVC; }

// Whether this machine's ffmpeg can actually open the NVENC encoder: a tiny null
// encode, run once per encoder and remembered. A build without NVENC, no NVIDIA GPU,
// an old driver, or a card without HEVC all fail it the same way.
static bool NvencWorks(int c) {
    static int known[2] = { -1, -1 };      // h264, hevc: -1 untested, 0 no, 1 yes
    int& k = known[c == VC_NVENC_HEVC ? 1 : 0];
    if (k < 0) {
        std::wstring cmd = std::wstring(L"ffmpeg -hide_banner -nostdin -loglevel error"
                           L" -f lavfi -i color=c=black:s=256x256:r=30:d=0.2 -c:v ") +
                           (c == VC_NVENC_HEVC ? L"hevc_nvenc" : L"h264_nvenc") + L" -f null -";
        k = RunHidden(cmd) ? 1 : 0;
    }
    return k == 1;
}

// The encoder an export really uses: the one picked, or its software twin when the
// GPU one will not open. `note` says so in words when that happens.
static int ResolveEncoder(int c, std::string* note) {
    note->clear();
    if (!IsNvenc(c) || NvencWorks(c)) return c;
    int sw = c == VC_NVENC_HEVC ? VC_X265 : VC_X264;
    *note = sw == VC_X265 ? "no NVENC HEVC on this machine, used libx265"
                          : "no NVENC on this machine, used libx264";
    return sw;
}
static const wchar_t* ContainerExt(int c) {
    return c == CT_MOV ? L"mov" : c == CT_MKV ? L"mkv" : L"mp4";
}
static const wchar_t* PresetTag(int p) {
    switch (p) {
    case PRESET_TIKTOK:    return L"tiktok";
    case PRESET_REELS:     return L"reels";
    case PRESET_IG_FEED:   return L"igfeed";
    case PRESET_IG_SQUARE: return L"igsquare";
    case PRESET_YT_1080:   return L"yt1080";
    case PRESET_YT_4K:     return L"yt4k";
    case PRESET_CUSTOM:    return L"custom";
    default:               return L"original";
    }
}

static void PresetSize(int preset, int* w, int* h) {
    switch (preset) {
    case PRESET_TIKTOK:
    case PRESET_REELS:     *w = 1080; *h = 1920; break;
    case PRESET_IG_FEED:   *w = 1080; *h = 1350; break;
    case PRESET_IG_SQUARE: *w = 1080; *h = 1080; break;
    case PRESET_YT_1080:   *w = 1920; *h = 1080; break;
    case PRESET_YT_4K:     *w = 3840; *h = 2160; break;
    case PRESET_CUSTOM:    *w = g_customW; *h = g_customH; break;
    default:               *w = 0;    *h = 0;    break;   // follow the sources
    }
}

// Canvas actually handed to the encoder, sources included when the preset follows them.
static void ResolveCanvas(int* outW, int* outH) {
    int W = 0, H = 0;
    PresetSize(g_preset, &W, &H);
    if (W == 0) {
        for (auto& c : g_clips) { if (c->srcW > W) W = c->srcW; if (c->srcH > H) H = c->srcH; }
        if (W <= 0 || H <= 0) { W = 1080; H = 1920; }     // text-only project
    }
    W += W & 1; H += H & 1;                               // yuv420p needs even dimensions
    *outW = W; *outH = H;
}

static float PresetAspect() {
    int W, H;
    ResolveCanvas(&W, &H);
    return H > 0 ? (float)W / (float)H : 1.0f;
}

// Name template for the Save dialog. "slideshow.mp4" told you nothing and collided
// with the last export; this reads like <source>_<target>_<size>_<when>.<ext>, e.g.
//   lisbon-rooftop_reels_1080x1920_20260810-0117.mp4
static std::wstring g_namePrefix;          // optional user override, set in settings

static std::wstring DefaultOutputName() {
    std::wstring stem = SanitizeStem(g_namePrefix);
    if (stem.empty()) {
        for (auto& c : g_clips) {          // first real media file names the export
            if (c->kind == Clip::Text || c->path.empty()) continue;
            stem = SanitizeStem(StemOf(c->path));
            break;
        }
    }
    if (stem.empty() && !g_clips.empty() && g_clips[0]->kind == Clip::Text)
        stem = SanitizeStem(Widen(FirstLine(g_clips[0]->text)));
    if (stem.empty()) stem = L"slidecut";

    int W, H;
    ResolveCanvas(&W, &H);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[320];
    swprintf(buf, 320, L"%ls_%ls_%dx%d_%04d%02d%02d-%02d%02d.%ls",
             stem.c_str(), PresetTag(g_preset), W, H,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             ContainerExt(g_container));
    return buf;
}

// Where the Save dialog should open when nothing has been exported yet.
static std::wstring SuggestExportDir() {
    for (auto& c : g_clips)
        if (c->kind != Clip::Text && !c->path.empty()) return DirName(c->path);
    return {};
}

// ffmpeg runs with its cwd set here, so filter args (font, text files) stay bare
// filenames — no drive-letter colons to escape inside filter_complex.
static std::wstring g_workDir;

static bool PrepareWorkDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_workDir = std::wstring(tmp) + L"slidecut_work\\";
    CreateDirectoryW(g_workDir.c_str(), nullptr);

    // every clip on every video track, and inside every sequence: a sequence the
    // export keeps whole still burns the words of the clips it holds
    std::vector<Clip*> all;
    for (auto& c : g_clips) all.push_back(c.get());
    for (auto& t : g_over) for (auto& c : t->clips) all.push_back(c.get());
    for (auto& q : g_seqs) {
        for (auto& c : q->clips) all.push_back(c.get());
        for (auto& t : q->over) for (auto& c : t->clips) all.push_back(c.get());
    }

    bool needFont = false;
    for (Clip* c : all)
        if (c->kind == Clip::Text || (c->ovlOn && !c->ovlText.empty())) needFont = true;
    if (needFont) {
        std::wstring src = AssetPath(TITLE_FONT_FILE);
        if (src.empty()) return false;
        CopyFileW(src.c_str(), (g_workDir + L"font.ttf").c_str(), FALSE);
    }
    // t<uid>.txt = the card's own words, o<uid>.txt = an overlay burned over a picture.
    auto writeText = [&](const wchar_t* kind, int i, const std::string& text) {
        wchar_t name[64];
        swprintf(name, 64, L"%ls%d.txt", kind, i);
        HANDLE f = CreateFileW((g_workDir + name).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wr = 0;                      // raw UTF-8, no BOM: drawtext reads it as-is
        WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr);
        CloseHandle(f);
        return true;
    };
    for (Clip* c : all) {
        if (c->kind == Clip::Text && !writeText(L"t", c->uid, c->text)) return false;
        if (c->ovlOn && !c->ovlText.empty() && !writeText(L"o", c->uid, c->ovlText)) return false;
    }
    return true;
}

// Rendering does not understand nesting: every sequence is opened out into plain
// clips, layers and music first. The project is reloaded from a snapshot right
// after, so the flattening is never something the editor sees.
// How long a sequence renders on its own clock: all of it, whatever a range cut off.
static double NestLen(const Clip& n) { return n.duration + n.nestHead + n.nestTail; }

static bool FlattenNestsHere() {
    bool did = false;
    // Opened out, a sequence is gone and so is the text overlay it carried. Its words
    // come back as a text clip of the same length and place, on a track of their own
    // above everything unfolding brings up: the preview draws them over the lot.
    std::vector<std::unique_ptr<Clip>> words;
    for (int guard = 0; guard < 256; guard++) {
        // A sequence on a screen of its own is not opened: the screen shows the whole
        // sequence at once, and opened out there would be nothing left to put it on.
        int idx = -1;
        for (int i = 0; i < (int)g_clips.size(); i++)
            if (g_clips[i]->kind == Clip::Nest &&
                (g_clips[i]->skip || g_clips[i]->look == LOOK_PROJECTOR)) { idx = i; break; }
        if (idx < 0) break;
        if (g_clips[idx]->skip) {          // muted: it never reaches the film
            g_clips.erase(g_clips.begin() + idx);
            did = true;
            continue;
        }
        const Clip& n = *g_clips[idx];
        if (n.ovlOn && !n.ovlText.empty()) {
            std::vector<BaseSpan> lay;
            BaseLayout(lay);
            auto w = std::make_unique<Clip>();
            w->kind = Clip::Text;
            w->label = n.label;
            w->start = lay[idx].start;
            w->duration = n.duration;
            w->ovlOn = true;
            w->ovlText = n.ovlText;
            w->ovlScale = n.ovlScale;
            w->ovlX = n.ovlX;
            w->ovlY = n.ovlY;
            for (int k = 0; k < 3; k++) w->ovlCol[k] = n.ovlCol[k];
            w->ovlAlpha = n.ovlAlpha;
            w->ovlShadow = n.ovlShadow;
            words.push_back(std::move(w));
        }
        // A sequence can run longer than its own cut - one made of layers alone has no
        // cut at all. Opened out, only the cut takes up room on the base track, so
        // everything after it would slide earlier while the sound stays put. A blank
        // card holds the rest of its length: black, what the preview shows there too.
        double gap = n.duration;
        size_t inner = 0;
        if (Sequence* q = FindSeq(n.nest)) {
            std::vector<BaseSpan> cut;
            BaseLayoutIn(q->clips, cut);
            for (size_t k = 0; k < cut.size(); k++)
                if (!q->clips[k]->skip) gap = std::min(gap, n.duration - cut[k].end);
            inner = q->clips.size();
        }
        UnfoldNest(idx);
        if (gap > 1e-4) {
            auto blank = std::make_unique<Clip>();
            blank->kind = Clip::Text;              // an empty card is a plain black frame
            blank->label = "blank";
            blank->duration = gap;
            g_clips.insert(g_clips.begin() + idx + inner, std::move(blank));
        }
        did = true;
    }
    if (!words.empty()) {
        int t = NewOverlayTrack();
        for (auto& w : words) g_over[t]->clips.push_back(std::move(w));
    }
    // A nest on an overlay track is NOT opened out. A sequence is one picture: it
    // composites onto a canvas of its own and is blended into the cut once, in its
    // own mode. Opening it out means stamping that mode onto every clip inside it,
    // which is why a folded sequence used to look different from the same sequence
    // opened up. StartExport builds the canvas; all that has to happen here is
    // lifting the sound out, because the audio graph is flat.
    {
        std::vector<int> lifted;
        std::function<void(Clip&, double, bool)> liftAudio =
            [&](Clip& n, double at, bool rev) {
            Sequence* q = FindSeq(n.nest);
            if (!q) return;
            for (int u : lifted) if (u == n.uid) return;   // a sequence used twice
            lifted.push_back(n.uid);
            {
                MixGuard lock;
                for (auto& tr : q->atracks) {
                    g_atracks.push_back(std::move(tr));
                    for (auto& b : g_atracks.back()->blocks) {
                        if (rev) {
                            b->offset = n.duration - (b->offset + b->duration);
                            b->reversed = !b->reversed;
                        }
                        b->offset += at;
                    }
                }
                q->atracks.clear();
            }
            double acc = 0;
            for (auto& b : q->clips) {         // sequences inside this one, in turn
                double s = rev ? n.duration - (acc + b->duration) : acc;
                acc += b->duration;
                if (b->kind == Clip::Nest && !b->skip)
                    liftAudio(*b, at + s, rev != b->reversed);
            }
            for (auto& t : q->over)
                for (auto& c : t->clips) {
                    if (c->kind != Clip::Nest || c->skip) continue;
                    double s = rev ? n.duration - (c->start + c->duration) : c->start;
                    liftAudio(*c, at + s, rev != c->reversed);
                }
            did = true;
        };
        for (auto& t : g_over)
            for (auto& c : t->clips)
                if (c->kind == Clip::Nest && !c->skip)
                    liftAudio(*c, c->start, c->reversed);
        // The same for a sequence left whole on the base cut because it plays on a
        // screen. The sound of its own shots rides in its slot of the cut instead.
        std::vector<BaseSpan> lay;
        BaseLayout(lay);
        for (size_t i = 0; i < g_clips.size(); i++)
            if (g_clips[i]->kind == Clip::Nest && !g_clips[i]->skip)
                liftAudio(*g_clips[i], lay[i].start, g_clips[i]->reversed);
    }
    return did;
}

// Returns "" when the grade is neutral, so a clip with no colour work on it adds
// nothing to the graph at all.
// The same steps as ApplyGrade in the preview shader, in RGB and in its order:
// brightness, contrast about mid-grey, warmth on red and blue, saturation about luma.
// eq works on luma in YUV instead, and its brightness lands visibly off the preview.
static std::wstring GradeFilter(const Grade& g) {
    if (!g.On()) return L"";
    wchar_t buf[512];
    const double c = 1.0 + (g.contrast - 1.0);
    const double off = 255.0 * g.bright - 127.5;
    std::wstring e;
    swprintf(buf, 512, L"(val%+.4f)*%.4f+127.5", off, c);
    e = buf;
    std::wstring out = L"lutrgb=r=" + e + L":g=" + e + L":b=" + e;
    if (fabsf(g.temp) > 1e-4f) {
        swprintf(buf, 512, L",colorchannelmixer=rr=%.4f:bb=%.4f",
                 1.0f + 0.25f * g.temp, 1.0f - 0.25f * g.temp);
        out += buf;
    }
    const double s = g.mono ? 0.0 : g.sat;
    if (fabs(s - 1.0) > 1e-4) {
        const double k = 1.0 - s;
        swprintf(buf, 512,
                 L",colorchannelmixer=rr=%.4f:rg=%.4f:rb=%.4f:gr=%.4f:gg=%.4f:gb=%.4f"
                 L":br=%.4f:bg=%.4f:bb=%.4f",
                 0.299 * k + s, 0.587 * k, 0.114 * k,
                 0.299 * k, 0.587 * k + s, 0.114 * k,
                 0.299 * k, 0.587 * k, 0.114 * k + s);
        out += buf;
    }
    return out;
}

static void StartExport(const std::wstring& outPath) {
    if (g_clips.empty()) return;
    {
        bool anyLive = false;
        for (auto& c : g_clips) if (!c->skip) anyLive = true;
        if (!anyLive) {
            g_export.failed = true;
            g_export.message = "Every shot is muted — nothing to render.";
            return;
        }
    }

    int W = 0, H = 0;
    ResolveCanvas(&W, &H);
    const int FPS = g_fps;

    std::vector<Clip*> layers;             // overlay-track clips, bottom track first
    for (auto& t : g_over) {
        if (!t->visible) continue;
        for (auto& c : t->clips)
            if (c->duration > 0.001 && !c->skip) layers.push_back(c.get());
    }

    // What one sequence holds, laid out on the parent's clock: its cut first (which
    // always composites normally, the way it does when you are standing inside it),
    // then its own layers with the modes they were given in there. An inner nest
    // comes back as a Nest clip - it gets a canvas of its own in turn.
    struct LayerItem { Clip* clip; double start, dur; int mode; float opacity; bool rev; };
    auto nestItems = [](Clip& n, double at, bool rev, std::vector<LayerItem>& out) {
        Sequence* q = FindSeq(n.nest);
        if (!q) return;
        const double len = NestLen(n);
        double acc = 0;
        for (auto& b : q->clips) {
            double s = rev ? len - (acc + b->duration) : acc;
            acc += b->duration;
            if (b->skip || b->duration <= 0.001) continue;
            out.push_back({ b.get(), at + s, b->duration, 0, 1.0f, rev != b->reversed });
        }
        for (auto& t : q->over) {
            if (!t->visible) continue;
            for (auto& c : t->clips) {
                if (c->skip || c->duration <= 0.001) continue;
                double s = rev ? len - (c->start + c->duration) : c->start;
                out.push_back({ c.get(), at + s, c->duration, c->lblend, c->lopacity,
                                rev != c->reversed });
            }
        }
    };

    // Everything inside the overlay nests still needs an ffmpeg input of its own, and
    // every nest needs a transparent canvas to composite onto.
    std::vector<Clip*> nested;             // clips living inside an overlay nest
    std::vector<Clip*> nests;              // the nests themselves
    {
        std::vector<int> seen;
        std::function<void(Clip&, bool)> walkNest = [&](Clip& n, bool rev) {
            for (int u : seen) if (u == n.uid) return;
            seen.push_back(n.uid);
            nests.push_back(&n);
            std::vector<LayerItem> items;
            nestItems(n, 0.0, rev, items);
            for (auto& it : items) {
                if (it.clip->kind == Clip::Nest) walkNest(*it.clip, it.rev);
                else {
                    bool dup = false;
                    for (Clip* q : nested) if (q == it.clip) dup = true;
                    if (!dup) nested.push_back(it.clip);
                }
            }
        };
        for (Clip* c : layers)
            if (c->kind == Clip::Nest) walkNest(*c, c->reversed);
        for (auto& c : g_clips)            // a base sequence left whole for its screen
            if (c->kind == Clip::Nest && !c->skip) walkNest(*c, c->reversed);
    }

    bool anyPending = false, anyProbing = false;
    for (auto& c : g_clips) {
        if (c->kind == Clip::Image && c->srcW == 0) anyPending = true;
        if (c->kind == Clip::Video && (!c->vid || !c->vid->probed.load())) anyProbing = true;
    }
    for (Clip* c : layers) {
        if (c->kind == Clip::Image && c->srcW == 0) anyPending = true;
        if (c->kind == Clip::Video && (!c->vid || !c->vid->probed.load())) anyProbing = true;
    }
    for (Clip* c : nested) {
        if (c->kind == Clip::Image && c->srcW == 0) anyPending = true;
        if (c->kind == Clip::Video && (!c->vid || !c->vid->probed.load())) anyProbing = true;
    }
    if (anyPending) { g_export.failed = true; g_export.message = "Images still loading."; return; }
    if (anyProbing) { g_export.failed = true; g_export.message = "Still reading video files."; return; }

    if (!PrepareWorkDir()) {
        g_export.failed = true;
        g_export.message = "Missing assets/SpecialElite-Regular.ttf next to the exe.";
        return;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_export.progressFile = std::wstring(tmp) + L"slidecut_progress.txt";
    DeleteFileW(g_export.progressFile.c_str());

    // -nostdin: the child has no console, so ffmpeg must not try to read the keyboard.
    // Warnings too: when an encode stalls, what ffmpeg last complained about is the lead.
    std::wstring cmd = L"ffmpeg -y -hide_banner -nostdin -loglevel level+warning -nostats -progress \"" +
                       g_export.progressFile + L"\"";
    // The final encode does not take the files as -i inputs. ffmpeg 8 given a long cut's
    // ~180 inputs into one filter graph spins one core forever without a single frame
    // (bigger queues, fewer threads: no difference). Each input is also described as a
    // source filter - movie/amovie/color/anullsrc - that the graph reads on demand, and
    // the final command is rebuilt from those. The -i form below still feeds the
    // per-shot screen passes, which only ever pull one shot. A looped layer video stays
    // an -i input: movie's own loop never ends under a trim.
    const size_t inputsAt = cmd.size();
    struct GraphIn { std::wstring kept, v, a; };   // kept = the -i form, when it stays one
    std::vector<GraphIn> gIn;
    auto quoteP = [](const std::wstring& p) {      // a path as a quoted filter option value
        std::wstring q = L"'";
        for (wchar_t ch : p) {
            if (ch == L'\\') q += L'/';
            else if (ch == L':') q += L"\\:";
            else if (ch == L'\'') q += L"'\\\\\\''";
            else q += ch;
        }
        return q + L"'";
    };
    // movie hands out the file's own timestamps, and -ss counts from the container's
    // start time: the trim starts there, or a file that does not begin at 0 is cut
    // early by exactly that much.
    // A cut that runs to the end of its file: -i tells the graph the stream ends one
    // frame after the last one, movie ends it on the last frame itself, and fps then
    // drops that frame. One cloned frame puts the end back where -i has it; fps never
    // outputs the clone. A cut that stops mid-file already ends on the next real frame,
    // and a clone there would add one.
    auto fileIn = [&](const std::wstring& path, double ss, double dur, double st, bool toEnd) {
        wchar_t b[256];
        GraphIn g;
        swprintf(b, 256, L":seek_point=%.4f,trim=start=%.6f:duration=%.4f,setpts=PTS-STARTPTS%ls",
                 ss, ss + st, dur, toEnd ? L",tpad=stop_mode=clone:stop=1" : L"");
        g.v = L"movie=" + quoteP(path) + b;
        swprintf(b, 160, L":seek_point=%.4f,atrim=start=%.6f:duration=%.4f,asetpts=PTS-STARTPTS",
                 ss, ss + st, dur);
        g.a = L"amovie=" + quoteP(path) + b;
        gIn.push_back(g);
    };
    auto stillIn = [&](const std::wstring& path, double dur) {
        wchar_t b[128];
        // an image opens at 1/25: without settb the held frames land on a 25 fps clock
        swprintf(b, 128, L",loop=loop=-1:size=1,settb=1/%d,setpts=N,trim=duration=%.4f", FPS, dur);
        GraphIn g;
        g.v = L"movie=" + quoteP(path) + b;
        gIn.push_back(g);
    };
    // Inputs are no longer 1:1 with clips — double exposures, overlay tracks and
    // every audio block add their own — so record the index each one landed on.
    std::map<int, int> vIn, dIn;           // clip uid -> ffmpeg input index
    int nIn = 0;
    auto addClipInputs = [&](Clip& c) {
        wchar_t seg[128];
        if (c.kind == Clip::Text) {
            swprintf(seg, 128, L" -f lavfi -t %.4f -i color=c=black:s=%dx%d:r=%d",
                     c.duration, W, H, FPS);
            cmd += seg;
            // frame-exact: -t on a lavfi input rounds to the nearest frame, color's own
            // d= rounds up, and one extra frame on a card shifts every later shot
            swprintf(seg, 128, L"color=c=black:s=%dx%d:r=%d,trim=end_frame=%lld", W, H, FPS,
                     llround(c.duration * FPS));
            gIn.push_back({ L"", seg, L"" });
        } else if (c.kind == Clip::Video) {
            swprintf(seg, 128, L" -ss %.4f -t %.4f -i ", c.trimIn, c.duration);
            cmd += seg;
            cmd += L"\"" + c.path + L"\"";
            {   // does the cut run to the file's end (within half a source frame)?
                double srcFps = c.vid && c.vid->fps > 0 ? c.vid->fps : FPS;
                bool toEnd = c.vid && c.vid->duration > 0 &&
                             c.trimIn + c.duration >= c.vid->duration - 0.5 / srcFps;
                fileIn(c.path, c.trimIn, c.duration, c.vid ? c.vid->startTime : 0.0, toEnd);
            }
        } else {
            swprintf(seg, 128, L" -loop 1 -framerate %d -t %.4f -i ", FPS, c.duration);
            cmd += seg;
            cmd += L"\"" + c.path + L"\"";
            stillIn(c.path, c.duration);
        }
        vIn[c.uid] = nIn++;
        if (c.dxOn && !c.dxPath.empty()) {
            // The layer must cover the whole clip: loop a short video, hold a still.
            if (c.dxIsVideo)
                swprintf(seg, 128, L" -stream_loop -1 -ss %.4f -t %.4f -i ",
                         c.dxTrimIn, c.duration);
            else
                swprintf(seg, 128, L" -loop 1 -framerate %d -t %.4f -i ", FPS, c.duration);
            cmd += seg;
            cmd += L"\"" + c.dxPath + L"\"";
            if (c.dxIsVideo) gIn.push_back({ std::wstring(seg) + L"\"" + c.dxPath + L"\"", L"", L"" });
            else stillIn(c.dxPath, c.duration);
            dIn[c.uid] = nIn++;
        }
    };
    for (auto& c : g_clips) if (!c->skip && c->kind != Clip::Nest) addClipInputs(*c);
    for (Clip* c : layers)  if (c->kind != Clip::Nest) addClipInputs(*c);
    for (Clip* c : nested)  addClipInputs(*c);
    // One transparent canvas per nest, as long as the sequence, so it can be composited
    // on its own clock and dropped into its parent in one piece.
    std::map<int, int> nestIn;
    {
        for (Clip* n : nests) {
            wchar_t seg[160];
            swprintf(seg, 160, L" -f lavfi -t %.4f -i color=c=black:s=%dx%d:r=%d",
                     NestLen(*n), W, H, FPS);
            cmd += seg;
            swprintf(seg, 160, L"color=c=black:s=%dx%d:r=%d,trim=end_frame=%lld", W, H, FPS,
                     llround(NestLen(*n) * FPS));   // frame-exact, as for a card
            gIn.push_back({ L"", seg, L"" });
            nestIn[n->uid] = nIn++;
        }
    }

    // audio blocks, in track order
    struct AudioIn { Song* s; int in; float vol; int fx; float fxMix; int track; };
    std::vector<AudioIn> aIns;
    for (int ti = 0; ti < (int)g_atracks.size(); ti++) {
        auto& tr = g_atracks[ti];
        if (tr->mute) continue;
        for (auto& b : tr->blocks) {
            if (!b->loaded || b->gen == 2) continue;        // regions act on other blocks
            if (b->gen == 1) {                               // noise bed: silence, then its chain
                double len = b->trimEnd - b->trimStart;
                if (b->tex < 0 || b->tex >= TEX_COUNT || b->fxMix <= 0.0001f || len < 0.01)
                    continue;
                wchar_t gi[128];
                swprintf(gi, 128, L" -f lavfi -t %.4f -i anullsrc=r=48000:cl=stereo", len);
                cmd += gi;
                swprintf(gi, 128, L"anullsrc=r=48000:cl=stereo,atrim=duration=%.4f", len);
                gIn.push_back({ L"", L"", gi });
            } else {
                if (b->path.empty()) continue;
                cmd += L" -i \"" + b->path + L"\"";
                // -i starts a file at 0; amovie keeps its start time (0.025 s on an
                // mp3), which would put the whole block late. A sound-only file's
                // first timestamp is that start time, so this is the same shift.
                gIn.push_back({ L"", L"", L"amovie=" + quoteP(b->path) + L",asetpts=PTS-STARTPTS" });
            }
            aIns.push_back({ b.get(), nIn++, tr->volume, tr->fx, tr->fxMix, ti });
        }
    }

    // Any clip contributing its own audio forces the concat to carry an audio pad,
    // so every other clip needs a matching block of silence.
    // A sequence kept whole on the base cut counts by the shots of its own cut.
    std::function<bool(const Clip&)> ownSound = [&](const Clip& c) -> bool {
        if (c.kind == Clip::Video) return c.useAudio && c.vid && c.vid->hasAudio;
        if (c.kind != Clip::Nest) return false;
        if (Sequence* q = FindSeq(c.nest))
            for (auto& b : q->clips) if (!b->skip && ownSound(*b)) return true;
        return false;
    };
    bool clipAudio = false;
    for (auto& c : g_clips)
        if (!c->skip && ownSound(*c)) clipAudio = true;
    bool music = !aIns.empty();
    bool audio = music || clipAudio;

    std::wstring fc;
    int scratch = 0;                       // unique suffix for intermediate labels
    // Film time the picture being built starts at, for the plate it fits into: the
    // aspect track can give each shot a different plate, and the preview fits every
    // shot inside the plate showing while it plays.
    double plateAt = 0;
    auto PlateAtNow = [&]() -> float {
        int i = AspectAt(plateAt + 0.5 / FPS);     // the point in force on its first frame
        return i >= 0 ? g_aspects[i]->aspect : g_projPlateAr;
    };
    // Scale one input onto the canvas the way its own lfit asks, and name the result.
    // Everything lands on an rgba bed: where the clip does not reach and asks for no
    // bed of its own, the alpha is the mask, so a layer shows the cut underneath and a
    // base cut shows the wall - the same as the preview's transparent canvas.
    auto FitOne = [&](int inIdx, const std::wstring& out, int lfit,
                      int anchor = LANCHOR_CENTER, bool throughPlate = false) {
        wchar_t seg[1024];
        int u = scratch++;
        // Through the projector only the plate's part of the canvas is seen, so every
        // fit - fill, inside, blur bed, black bed - treats that part as its frame,
        // and the rest of the canvas stays clear.
        float rx = 1.0f, ry = 1.0f;
        int cw = W, ch = H;
        if (throughPlate && g_projOn && PlateCropRegion((float)W / (float)H, &rx, &ry, PlateAtNow())) {
            cw = (int)lround(W * rx); cw += cw & 1;
            ch = (int)lround(H * ry); ch += ch & 1;
            if (cw > W) cw = W;
            if (ch > H) ch = H;
        }
        if (lfit == LFIT_BLUR) {           // opaque blurred bed, the layer whole on top
            swprintf(seg, 1024,
                     L"[%d:v]fps=%d,setpts=PTS-STARTPTS,split=2[lb%d][lf%d];"
                     L"[lb%d]scale=%d:%d:force_original_aspect_ratio=increase,"
                     L"crop=%d:%d,gblur=sigma=%d[lbb%d];"
                     L"[lf%d]scale=%d:%d:force_original_aspect_ratio=decrease:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int[lff%d];"
                     L"[lbb%d][lff%d]overlay=(W-w)/2:(H-h)/2,format=rgba,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000,setsar=1",
                     inIdx, FPS, u, u,
                     u, cw, ch, cw, ch, ch / 48 > 0 ? ch / 48 : 1, u,
                     u, cw, ch, u,
                     u, u, W, H);
        } else if (lfit == LFIT_BLACK) {   // opaque black bed
            swprintf(seg, 1024,
                     L"[%d:v]fps=%d,setpts=PTS-STARTPTS,"
                     L"scale=%d:%d:force_original_aspect_ratio=decrease:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,format=rgba,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000,setsar=1",
                     inIdx, FPS, cw, ch, cw, ch, W, H);
        } else if (lfit == LFIT_FILL) {    // crop until it covers the frame
            float ax, ay;
            AnchorFrac(anchor, &ax, &ay);
            // The overflow is (iw-ow) wide; the anchor says how much of it comes
            // off the left, so the preview and the render keep the same part.
            swprintf(seg, 1024,
                     L"[%d:v]fps=%d,setpts=PTS-STARTPTS,"
                     L"scale=%d:%d:force_original_aspect_ratio=increase:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int,"
                     L"crop=%d:%d:(iw-ow)*%.3f:(ih-oh)*%.3f,format=rgba,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000,setsar=1",
                     inIdx, FPS, cw, ch, cw, ch, ax, ay, W, H);
        } else {
            swprintf(seg, 1024,
                     L"[%d:v]fps=%d,setpts=PTS-STARTPTS,"
                     L"scale=%d:%d:force_original_aspect_ratio=decrease:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int,format=rgba,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000,setsar=1",
                     inIdx, FPS, cw, ch, cw, ch, W, H);
        }
        fc += seg;
        fc += L"[" + out + L"];";
    };

    // Mix [top] into [bottom] in `mode` at `opacity`, but only where [top] is opaque.
    // The blend itself runs full frame, then the top's own alpha is put back on the
    // result and an overlay drops that onto the untouched bottom - which is exactly
    // the preview's lerp(B, blend(T, B), opacity * T.a). `enable`, when given, gates
    // that last overlay to the stretch of timeline the layer occupies.
    auto BlendMasked = [&](const std::wstring& top, const std::wstring& bottom,
                           const wchar_t* mode, float opacity, int uid,
                           const wchar_t* tag, const std::wstring& enable,
                           bool keepAlpha, const std::wstring& out) {
        wchar_t seg[768];
        swprintf(seg, 768, L"[%ls]split=2[%lsba%d][%lsbb%d];",
                 bottom.c_str(), tag, uid, tag, uid);
        fc += seg;
        // format=rgba before the mask is pulled: any filter upstream that only speaks YUV
        // drops the alpha plane, and then this would fail the whole export. Pinned here,
        // the worst such a clip can do is come out opaque.
        swprintf(seg, 768, L"[%ls]format=rgba,split=2[%lsta%d][%lstb%d];[%lsta%d]alphaextract[%lsm%d];",
                 top.c_str(), tag, uid, tag, uid, tag, uid, tag, uid);
        fc += seg;
        // Both sides pinned to planar RGB before the blend. blend does its maths on
        // whatever format the two inputs agree on, and with the top in rgba and the
        // cut in something else that can be YUV: difference, addition and the other
        // modes then run on the chroma planes and turn the picture green or purple.
        swprintf(seg, 768,
                 L"[%lstb%d]format=gbrp[%lstg%d];[%lsba%d]format=gbrp[%lsbg%d];"
                 L"[%lstg%d][%lsbg%d]blend=all_mode=%ls:all_opacity=%.4f:"
                 L"repeatlast=1:shortest=0,format=gbrp[%lsx%d];",
                 tag, uid, tag, uid, tag, uid, tag, uid,
                 tag, uid, tag, uid, mode, opacity, tag, uid);
        fc += seg;
        swprintf(seg, 768, L"[%lsx%d][%lsm%d]alphamerge[%lsy%d];",
                 tag, uid, tag, uid, tag, uid);
        fc += seg;
        // Both sides of the overlay pinned to full range. overlay converts them to YUV,
        // and which range that conversion picks is left to format negotiation: with
        // -i inputs something upstream narrowed it to full, with in-graph sources
        // nothing does and it lands on unspecified - a visibly different picture.
        swprintf(seg, 768,
                 L"[%lsbb%d]format=color_ranges=pc[%lspb%d];[%lsy%d]format=color_ranges=pc[%lspy%d];"
                 L"[%lspb%d][%lspy%d]overlay=eof_action=pass",
                 tag, uid, tag, uid, tag, uid, tag, uid, tag, uid, tag, uid);
        fc += seg;
        fc += enable;
        // A sequence canvas is itself a layer further up, and its alpha is the mask
        // that keeps it off the frame where it holds nothing. overlay would happily
        // negotiate that alpha away, so pin the format.
        if (keepAlpha) fc += L",format=rgba";
        fc += L",setsar=1[" + out + L"];";
    };

    // One clip's picture: fit to canvas, blend its double exposure, burn its text.
    // Leaves the result in [v<uid>].
    // The inputs as they stand now: a shot that plays on its own screen is rendered
    // on its own first, by re-running ffmpeg with these same inputs and just that
    // shot's slice of the graph.
    const std::wstring lookInputs = cmd;
    std::string lookJobs;

    // A clip's words as a drawtext - the card's own, or the overlay burned over its
    // picture - or "" when it has none.
    auto TextFilter = [&](const Clip& c) -> std::wstring {
        bool card = c.kind == Clip::Text && !c.text.empty();
        bool ovl  = c.ovlOn && !c.ovlText.empty();
        if (!card && !ovl) return L"";
        float scale = card ? c.textScale : c.ovlScale;
        int fs = (int)(scale * H);
        if (fs < 8) fs = 8;
        unsigned rgb = ((unsigned)(c.ovlCol[0] * 255) << 16) |
                       ((unsigned)(c.ovlCol[1] * 255) << 8) |
                        (unsigned)(c.ovlCol[2] * 255);
        wchar_t seg[768];
        swprintf(seg, 768,
                 L"drawtext=fontfile=font.ttf:textfile=%ls%d.txt:"
                 // text_align=C: every line centred in the block, the way the preview
                 // lays them out - drawtext's default starts each one at the left.
                 L"fontcolor=0x%06x@%.3f:fontsize=%d:line_spacing=%d:text_align=C:"
                 L"x=(w*%.4f-text_w/2):y=(h*%.4f-text_h/2)%ls",
                 card ? L"t" : L"o", c.uid,
                 rgb, c.ovlAlpha, fs, fs / 4,
                 c.ovlX, c.ovlY,
                 c.ovlShadow ? L":shadowcolor=black@0.55:shadowx=2:shadowy=2" : L"");
        return seg;
    };

    // A picture on a screen of its own - one shot, or a whole sequence - is rendered by
    // itself, before any track blending, so the render matches the preview: whatever
    // is layered over it stays off the glass. Its slice of the graph, everything from
    // chainStart on and ending in [v<uid>], is lifted out here and replaced by the file
    // that slice will have produced. Returns the label that file arrives in.
    auto LiftLook = [&](int uid, int look, bool pillar, double dur, size_t chainStart) {
        const std::string name = "look_" + std::to_string(uid);
        const std::wstring graph = fc.substr(chainStart);
        if (!WriteWholeFile(g_workDir + Widen(name + ".graph"), Narrow(graph)))
            lookJobs += "ERROR\n";
        lookJobs += name + " " + std::to_string(look) + " "
                  + std::to_string(dur) + (pillar ? " 1" : " 0") + "\n";
        fc.resize(chainStart);
        // No fps filter here: the renderer already wrote exactly this picture's frames
        // at the project rate, and an fps pass over them drops the last one.
        wchar_t back[256];
        // format=rgba: the layer blend pulls alpha out of this, and must never meet a
        // file that has none ("Requested planes not available").
        swprintf(back, 256, L"movie=%ls.mkv,format=rgba,settb=1/%d,setpts=N,setsar=1[lk%d];",
                 Widen(name).c_str(), FPS, uid);
        fc += back;
        swprintf(back, 256, L"lk%d", uid);
        return std::wstring(back);
    };

    // `words` false: a sequence above owns the screen, and burns this clip's words
    // itself once the screen has run.
    auto ClipChain = [&](Clip& c, bool layer, bool reversed, int look = LOOK_PROJECTOR,
                         bool words = true) {
        const size_t chainStart = fc.size();
        wchar_t seg[768];
        wchar_t cur[32];
        swprintf(cur, 32, L"p%d", c.uid);
        std::wstring stage = cur;          // label holding the picture so far

        if (c.kind == Clip::Text) {        // the lavfi colour source is already canvas-size
            // format=rgba: the colour source carries no alpha plane, and a card on a
            // layer track goes through the masked blend, which extracts one. On the base
            // track it is the opaque black card the preview draws. On a layer the preview
            // draws only its words, so the shot or sequence underneath shows round them:
            // the bed goes clear, in the text's own colour so the soft edges of the
            // letters do not fringe dark once they are blended.
            if (layer)
                swprintf(seg, 768, L"[%d:v]fps=%d,format=rgba,lutrgb=r=%d:g=%d:b=%d:a=0,setsar=1[%ls];",
                         vIn[c.uid], FPS, (int)(c.ovlCol[0] * 255), (int)(c.ovlCol[1] * 255),
                         (int)(c.ovlCol[2] * 255), stage.c_str());
            else
                swprintf(seg, 768, L"[%d:v]fps=%d,format=rgba,setsar=1[%ls];", vIn[c.uid], FPS, stage.c_str());
            fc += seg;
        } else {
            FitOne(vIn[c.uid], stage, c.lfit, c.lanchor, words && look == LOOK_PROJECTOR);
        }

        if (reversed && c.kind != Clip::Image) {
            // reverse buffers the whole segment, which is fine at shot length. It
            // goes on before the layer and the text so those stay the right way up.
            wchar_t rv[64];
            swprintf(rv, 64, L"[%ls]reverse,setpts=PTS-STARTPTS[r%d];", stage.c_str(), c.uid);
            fc += rv;
            wchar_t rl[32];
            swprintf(rl, 32, L"r%d", c.uid);
            stage = rl;
        }

        std::wstring gf = GradeFilter(c.grade);
        if (!gf.empty()) {                 // this shot's own colour
            // eq and friends only take YUV: ffmpeg converts on the way in and the alpha
            // plane is simply gone, which a layer's masked blend then fails on. So the
            // alpha is lifted off first and put back on the graded picture.
            wchar_t gseg[512];
            swprintf(gseg, 512,
                     L"[%ls]split=2[ga%d][gb%d];[ga%d]alphaextract[gm%d];"
                     L"[gb%d]%ls,format=rgba[gc%d];[gc%d][gm%d]alphamerge[g%d];",
                     stage.c_str(), c.uid, c.uid, c.uid, c.uid,
                     c.uid, gf.c_str(), c.uid, c.uid, c.uid, c.uid);
            fc += gseg;
            wchar_t gl[32];
            swprintf(gl, 32, L"g%d", c.uid);
            stage = gl;
        }

        auto dit = dIn.find(c.uid);
        if (dit != dIn.end()) {            // double exposure
            wchar_t dl[32];
            swprintf(dl, 32, L"d%d", c.uid);
            FitOne(dit->second, dl, c.lfit, c.lanchor, words && look == LOOK_PROJECTOR);
            wchar_t out[32];
            swprintf(out, 32, L"x%d", c.uid);
            int bm = c.dxBlend;
            if (bm < 0 || bm >= (int)(sizeof(BLEND_MODES_W) / sizeof(*BLEND_MODES_W))) bm = 0;
            // ffmpeg's blend takes the TOP layer first: all_opacity mixes the result
            // back toward the second input, so the picture has to be second.
            BlendMasked(dl, stage, BLEND_MODES_W[bm], c.dxAmount, c.uid, L"dx", L"",
                        layer, out);
            stage = out;
        }

        // Text: the card's own words, or an overlay burned over the picture. The
        // preview draws them over the finished frame, so a shot on a screen of its own
        // gets them after the screen - off the glass, not into it.
        // Inside a sequence that owns the screen, the words wait for that screen instead.
        auto finish = [&](const std::wstring& in) {
            std::wstring tf = words ? TextFilter(c) : L"";
            if (tf.empty()) swprintf(seg, 768, L"null[v%d];", c.uid);
            else            swprintf(seg, 768, L",setsar=1[v%d];", c.uid);
            fc += L"[" + in + L"]" + tf + seg;
        };
        if (look == LOOK_PROJECTOR) { finish(stage); return; }
        swprintf(seg, 768, L"[%ls]null[v%d];", stage.c_str(), c.uid);
        fc += seg;
        finish(LiftLook(c.uid, look, c.look43, c.duration, chainStart));
    };

    // Put one element onto `canvas` in `mode` at `opacity`, live only between s and e.
    // tpad gives the element a lead-in of s seconds so it lines up with the canvas's
    // clock, and `enable` keeps the blend switched off everywhere outside the element.
    // The layer is the TOP input of blend, so the cut underneath is second and
    // all_opacity fades toward it. `enable` cannot gate blend here — a disabled
    // filter passes its FIRST input through, which would show the bare layer — so the
    // window is applied by an overlay of the blended result instead.
    auto CompositeOn = [&](const std::wstring& canvas, int uid, const std::wstring& pic,
                           double s, double e, int mode, float opacity,
                           bool keepAlpha) -> std::wstring {
        // The lead-in is transparent, not black: a black pad is opaque, and an
        // opaque pad would blend over the cut on every frame before the layer.
        wchar_t seg[768];
        swprintf(seg, 768,
                 L"[%ls]tpad=start_duration=%.4f:start_mode=add:color=0x00000000[L%d];",
                 pic.c_str(), s, uid);
        fc += seg;
        wchar_t top[32];
        swprintf(top, 32, L"L%d", uid);
        int m = mode;
        if (m < 0 || m >= (int)(sizeof(LAYER_MODES_W) / sizeof(*LAYER_MODES_W))) m = 0;
        wchar_t lab[32], en[96];
        swprintf(lab, 32, L"o%d", uid);
        swprintf(en, 96, L":enable='between(t,%.4f,%.4f)'", s, e);
        BlendMasked(top, canvas, LAYER_MODES_W[m], opacity, uid, L"ly", en, keepAlpha, lab);
        return lab;
    };

    // Words waiting on a screen: whose they are, and when they show on the clock of the
    // picture they will be burned over.
    struct Words { const Clip* clip; double s, e; };
    auto BurnWords = [&](const std::wstring& pic, const std::vector<Words>& ws, int uid) {
        std::wstring chain;
        for (auto& w : ws) {
            std::wstring tf = TextFilter(*w.clip);
            if (tf.empty() || w.e <= w.s) continue;
            wchar_t en[96];
            swprintf(en, 96, L":enable='between(t,%.4f,%.4f)'", w.s, w.e);
            if (!chain.empty()) chain += L",";
            chain += tf + en;
        }
        if (chain.empty()) return pic;
        wchar_t seg[64];
        swprintf(seg, 64, L",format=rgba,setsar=1[nt%d];", uid);
        fc += L"[" + pic + L"]" + chain + seg;
        swprintf(seg, 64, L"nt%d", uid);
        return std::wstring(seg);
    };

    // A sequence as one picture, on its own clock from 0: its cut and its layers
    // composited onto a canvas of its own, each in the mode it was given in there, and
    // the finished canvas blended into its parent once. On a screen of its own it goes
    // through the look job whole, the way the preview runs the look over the canvas.
    // `claimed` means a screen further up owns this sequence: nothing inside runs a look
    // of its own, and every word inside is handed up - on this sequence's clock - to go
    // on after that screen, off the glass. A range render's cut comes off at the end.
    std::function<std::wstring(Clip&, bool, std::vector<Words>*)> NestPicture;
    NestPicture = [&](Clip& n, bool rev, std::vector<Words>* claimed) -> std::wstring {
        const double len = NestLen(n);
        const double seqAt = plateAt;      // this sequence's start on the film's clock
        const bool screen = !claimed && n.look != LOOK_PROJECTOR;
        const size_t chainStart = fc.size();
        std::vector<Words> own;
        std::vector<Words>* held = claimed ? claimed : screen ? &own : nullptr;
        wchar_t seg[256];
        swprintf(seg, 256, L"[%d:v]format=rgba,colorchannelmixer=aa=0,setsar=1[nc%d];",
                 nestIn[n.uid], n.uid);
        fc += seg;
        swprintf(seg, 256, L"nc%d", n.uid);
        std::wstring pic = seg;
        std::vector<LayerItem> items;
        nestItems(n, 0.0, rev, items);
        for (auto& it : items) {
            double s = it.start < 0 ? 0 : it.start;
            double e = it.start + it.dur;
            if (e <= 0.001 || s >= len) continue;
            if (e > len) e = len;
            std::wstring layer;
            if (it.clip->kind == Clip::Nest) {
                size_t first = held ? held->size() : 0;
                layer = NestPicture(*it.clip, it.rev, held);
                if (held)                  // from the inner sequence's clock onto this one
                    for (size_t k = first; k < held->size(); k++) {
                        (*held)[k].s = std::max(s, (*held)[k].s + it.start);
                        (*held)[k].e = std::min(e, (*held)[k].e + it.start);
                    }
            } else {
                plateAt = seqAt + s;
                ClipChain(*it.clip, true, it.rev, held ? LOOK_PROJECTOR : it.clip->look, !held);
                plateAt = seqAt;
                if (held) held->push_back({ it.clip, s, e });
                swprintf(seg, 256, L"v%d", it.clip->uid);
                layer = seg;
            }
            pic = CompositeOn(pic, it.clip->uid, layer, s, e, it.mode, it.opacity, true);
        }
        if (claimed) {
            claimed->push_back({ &n, 0.0, len });      // the sequence's own words, over the lot
            return pic;
        }
        if (screen) {
            swprintf(seg, 256, L"[%ls]null[v%d];", pic.c_str(), n.uid);
            fc += seg;
            pic = LiftLook(n.uid, n.look, n.look43, len, chainStart);
        }
        own.push_back({ &n, 0.0, len });
        pic = BurnWords(pic, own, n.uid);
        if (n.nestHead > 1e-6 || n.nestTail > 1e-6) {
            swprintf(seg, 256, L"[%ls]trim=start=%.4f:duration=%.4f,setpts=PTS-STARTPTS[tr%d];",
                     pic.c_str(), n.nestHead, n.duration, n.uid);
            fc += seg;
            swprintf(seg, 256, L"tr%d", n.uid);
            pic = seg;
        }
        return pic;
    };

    std::vector<BaseSpan> baseLay;
    BaseLayout(baseLay);
    for (size_t bi = 0; bi < g_clips.size(); bi++) {
        auto& c = g_clips[bi];
        if (c->skip) continue;
        plateAt = baseLay[bi].start;
        if (c->kind != Clip::Nest) { ClipChain(*c, false, c->reversed, c->look); continue; }
        // A sequence left whole for its screen, into its slot of the cut.
        std::wstring pic = NestPicture(*c, c->reversed, nullptr);
        wchar_t seg[160];
        swprintf(seg, 160, L"[%ls]setsar=1[v%d];", pic.c_str(), c->uid);
        fc += seg;
    }
    for (Clip* c : layers)
        if (c->kind != Clip::Nest) {
            plateAt = c->start < 0 ? 0 : c->start;
            ClipChain(*c, true, c->reversed, c->look);
        }

    if (clipAudio) {                       // one audio block per clip, exact length
        // A sequence's block is the blocks of its own cut end to end - the sound that
        // cut carries opened out - cut down the way a range render cut the sequence.
        std::function<void(Clip&, bool)> AudioBlock = [&](Clip& c, bool rev) {
            wchar_t seg2[512];
            std::vector<Clip*> cut;
            if (c.kind == Clip::Nest)
                if (Sequence* q = FindSeq(c.nest))
                    for (auto& b : q->clips)
                        if (!b->skip && b->duration > 0.001) cut.push_back(b.get());
            if (!cut.empty()) {
                if (rev) std::reverse(cut.begin(), cut.end());
                std::wstring cat;          // every block first: the concat's labels must be adjacent
                for (Clip* b : cut) {
                    AudioBlock(*b, rev != b->reversed);
                    swprintf(seg2, 512, L"[a%d]", b->uid);
                    cat += seg2;
                }
                fc += cat;
                swprintf(seg2, 512,
                         L"concat=n=%zu:v=0:a=1,apad,atrim=start=%.4f:duration=%.4f,"
                         L"asetpts=PTS-STARTPTS[a%d];",
                         cut.size(), c.nestHead, c.duration, c.uid);
                fc += seg2;
                return;
            }
            bool own = c.kind == Clip::Video && c.useAudio && c.vid && c.vid->hasAudio;
            if (own) {
                swprintf(seg2, 512,
                         L"[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,"
                         L"%lsasetpts=PTS-STARTPTS,volume=%.4f,apad,atrim=end=%.4f[a%d];",
                         vIn[c.uid], rev ? L"areverse," : L"", c.volume, c.duration, c.uid);
            } else {
                swprintf(seg2, 512,
                         L"anullsrc=r=48000:cl=stereo,atrim=end=%.4f,asetpts=PTS-STARTPTS[a%d];",
                         c.duration, c.uid);
            }
            fc += seg2;
        };
        for (auto& c : g_clips) if (!c->skip) AudioBlock(*c, c->reversed);
    }
    // Picture and sound are joined by two separate concats, never one v+a concat. A
    // combined concat couples the streams segment by segment: over a long cut the
    // sound falls a fraction of a second behind the picture inside it, and at the end
    // it spins waiting to pair them - the export sat at 98% forever. Every block is
    // already cut to its shot's exact length, so the two stay in step on their own.
    size_t nCat = 0;
    std::wstring vCat, aCat;
    for (size_t bi = 0; bi < g_clips.size(); bi++) {   // concat only walks the base track
        auto& cp = g_clips[bi];
        if (cp->skip) continue;            // muted shots are not in the film at all
        nCat++;
        // Each shot gets exactly the frames its span covers on the film's frame grid.
        // A length that is not a whole number of frames, rounded per shot, adds up
        // over the cut: later cuts landed frames late against the layers above and
        // the look schedule (a shot losing its screen before its cut).
        long long f0 = llround(baseLay[bi].start * g_fps), f1 = llround((baseLay[bi].end - baseLay[bi].fade) * g_fps);
        long long nf = f1 - f0 > 1 ? f1 - f0 : 1;
        wchar_t seg[200];
        swprintf(seg, 200, L"[v%d]setpts=N/(%.6f*TB),tpad=stop_mode=clone:stop=%lld,"
                           L"trim=end_frame=%lld[vq%d];",
                 cp->uid, (double)g_fps, nf, nf, cp->uid);
        fc += seg;
        swprintf(seg, 200, L"[vq%d]", cp->uid);
        vCat += seg;
        swprintf(seg, 32, L"[a%d]", cp->uid);
        aCat += seg;
    }
    {   // NOTE: never %s a wide literal through swprintf — MinGW reads %s as char*
        // and silently truncates it (that is what turned "[ac]" into "["). Concatenate.
        wchar_t seg[128];
        swprintf(seg, 128, L"concat=n=%zu:v=1:a=0[vc];", nCat);
        fc += vCat;
        fc += seg;
        if (clipAudio) {
            swprintf(seg, 128, L"concat=n=%zu:v=0:a=1[ac];", nCat);
            fc += aCat;
            fc += seg;
        }
    }
    double total = TotalDuration();

    // ---- overlay tracks: each layer, or each sequence as one picture, shifted to its
    // start and composited there.
    std::wstring vstage = L"vc";
    for (Clip* c : layers) {
        double s = c->start < 0 ? 0 : c->start;
        double e = s + c->duration;
        if (e <= 0.001 || s >= total) continue;          // outside the film entirely
        if (e > total) e = total;
        std::wstring pic;
        if (c->kind == Clip::Nest) {
            plateAt = s;                   // its shots fit the plate from its own start
            pic = NestPicture(*c, c->reversed, nullptr);
        } else {
            wchar_t v[32];
            swprintf(v, 32, L"v%d", c->uid);
            pic = v;
        }
        vstage = CompositeOn(vstage, c->uid, pic, s, e, c->lblend, c->lopacity, false);
    }

    {   // tag bt709 in the graph: output-side -color_primaries/-color_trc alone do not stick
        std::wstring fg = GradeFilter(g_grade);
        if (!fg.empty()) {                 // the film's own colour, over the lot
            wchar_t gseg[384];
            swprintf(gseg, 384, L"[%ls]%ls[fg];", vstage.c_str(), fg.c_str());
            fc += gseg;
            vstage = L"fg";
        }
        wchar_t vt[256];
        // explicit matrix: format= alone converts RGB with BT.601, which setparams then
        // merely relabels BT.709 - reds came out hotter than the preview
        swprintf(vt, 256, L"[%ls]fps=%d,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
                          L"setparams=color_primaries=bt709:color_trc=bt709:"
                          L"colorspace=bt709:range=tv", vstage.c_str(), FPS);
        fc += vt;
        if (g_fadeIn > 0.001f) {
            wchar_t fi[96];
            swprintf(fi, 96, L",fade=t=in:st=0:d=%.3f", g_fadeIn);
            fc += fi;
        }
        if (g_fadeOut > 0.001f) {
            double st = total - g_fadeOut;
            if (st < 0) st = 0;
            wchar_t fo[96];
            swprintf(fo, 96, L",fade=t=out:st=%.3f:d=%.3f", st, g_fadeOut);
            fc += fo;
        }
        // Hard stop at the last shot of the base track, in the graph itself: looped
        // layers and padded audio are endless, and the output -t alone has let a
        // whole-film render run on forever.
        wchar_t vtrim[64];
        swprintf(vtrim, 64, L",trim=duration=%.4f", total);
        fc += vtrim;
        fc += L"[v]";
    }

    if (music) {
        // One trimmed, delayed, level-set stream per audio block, then a single mix.
        for (auto& ai : aIns) {
            Song& s = *ai.s;
            // A noise bed's source is its own length of silence, starting at 0.
            double srcA = s.gen == 1 ? 0.0 : s.trimStart;
            double effStart = srcA + (s.offset < 0 ? -s.offset : 0.0);
            double effEnd = s.gen == 1 ? s.trimEnd - s.trimStart : s.trimEnd;
            int delayMs = s.offset > 0 ? (int)llround(s.offset * 1000.0) : 0;
            int volPct = (int)lround(ai.vol * 100.0f);
            wchar_t af[420];
            if (effStart >= effEnd) {          // trimmed entirely off-screen: silence
                swprintf(af, 420, L";[%d:a]atrim=end=0,asetpts=PTS-STARTPTS,apad[r%d]",
                         ai.in, ai.in);
            } else {
                swprintf(af, 420,
                         L";[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,"
                         L"atrim=start=%.4f:end=%.4f,asetpts=PTS-STARTPTS,"
                         L"%ls"
                         L"adelay=%d|%d:all=1,volume=%d/100,apad[r%d]",
                         ai.in, effStart, effEnd,
                         s.reversed ? L"areverse," : L"",
                         delayMs, delayMs, volPct, ai.in);
            }
            fc += af;
            // The block's own chain first, [r] -> [p], the same order as the mixer.
            {
                // a noise bed's "chain" is its texture recipe, on its silent source
                std::wstring bchain = s.fxMix <= 0.0001f ? L""
                                    : s.gen == 1 ? TexFilter(s.tex) : AfxChain(s.fx);
                wchar_t bl[256];
                if (bchain.empty()) {
                    swprintf(bl, 256, L";[r%d]anull[p%d]", ai.in, ai.in);
                    fc += bl;
                } else if (s.fxMix >= 0.999f) {
                    swprintf(bl, 256, L";[r%d]", ai.in);
                    fc += bl; fc += bchain;
                    swprintf(bl, 256, L"[p%d]", ai.in);
                    fc += bl;
                } else {
                    swprintf(bl, 256, L";[r%d]asplit=2[bd%d][bw%d];[bw%d]",
                             ai.in, ai.in, ai.in, ai.in);
                    fc += bl; fc += bchain;
                    swprintf(bl, 256,
                             L"[bx%d];[bd%d][bx%d]amix=inputs=2:weights=%.3f %.3f:"
                             L"normalize=0[p%d]",
                             ai.in, ai.in, ai.in, 1.0f - s.fxMix, s.fxMix, ai.in);
                    fc += bl;
                }
                {
                    // Every block's stream is padded with silence out to the whole film
                    // (the mix needs every stream that long). A texture, or a chain with
                    // hiss in it (am radio), would make noise on that padding from the
                    // first frame to the last. Keep it to its own block.
                    double a0 = s.offset > 0 ? s.offset : 0.0;
                    double a1 = s.offset + (s.trimEnd - s.trimStart);
                    wchar_t gate[160];
                    swprintf(gate, 160, L";[p%d]volume=0:enable='not(between(t,%.4f,%.4f))'[pg%d]",
                             ai.in, a0, a1, ai.in);
                    fc += gate;
                }
            }
            // Chain regions over this block's track: inside a region's span the stream
            // is its chain's output, outside it the stream passes dry - the same switch
            // the mixer makes. The stream's t is timeline time (it was delayed into place).
            std::wstring cur = L"pg" + std::to_wstring(ai.in);
            // The block's level and fades, after its own chain. The stream's t is
            // timeline time.
            {
                double len = s.trimEnd - s.trimStart, e = s.offset + len;
                std::wstring fd;
                wchar_t fb[128];
                if (fabsf(s.volume - 1.0f) > 0.0005f) {
                    swprintf(fb, 128, L"volume=%.4f", s.volume);
                    fd += fb;
                }
                if (s.fadeIn > 0.001f) {
                    double a = s.offset > 0 ? s.offset : 0.0, b = s.offset + s.fadeIn;
                    if (b > a) {
                        swprintf(fb, 128, L"%lsafade=t=in:st=%.4f:d=%.4f",
                                 fd.empty() ? L"" : L",", a, b - a);
                        fd += fb;
                    }
                }
                if (s.fadeOut > 0.001f) {
                    double a = std::max(e - s.fadeOut, 0.0);
                    if (e > a) {
                        swprintf(fb, 128, L"%lsafade=t=out:st=%.4f:d=%.4f",
                                 fd.empty() ? L"" : L",", a, e - a);
                        fd += fb;
                    }
                }
                if (!fd.empty()) {
                    std::wstring nxt = L"f" + std::to_wstring(ai.in);
                    fc += L";[" + cur + L"]" + fd + L"[" + nxt + L"]";
                    cur = nxt;
                }
            }
            {
                int rk = 0;
                double bStart = s.offset, bEnd = s.offset + (s.trimEnd - s.trimStart);
                for (auto& t2 : g_atracks) {
                    if (t2->mute) continue;
                    for (auto& rb : t2->blocks) {
                        const Song& r = *rb;
                        if (r.gen != 2 || r.target != ai.track) continue;
                        std::wstring rc = r.fxMix > 0.0001f ? AfxChain(r.fx) : L"";
                        if (rc.empty()) continue;
                        double ra = r.offset, rbnd = r.offset + (r.trimEnd - r.trimStart);
                        if (rbnd <= bStart || ra >= bEnd) continue;
                        float m = r.fxMix > 1.0f ? 1.0f : r.fxMix;
                        std::wstring nxt = L"q" + std::to_wstring(ai.in) + L"_" + std::to_wstring(rk);
                        wchar_t rg[640];
                        swprintf(rg, 640, L";[%ls]asplit=2[rd%d_%d][rw%d_%d];[rw%d_%d]",
                                 cur.c_str(), ai.in, rk, ai.in, rk, ai.in, rk);
                        fc += rg; fc += rc;
                        swprintf(rg, 640,
                                 L",volume='if(between(t,%.4f,%.4f),%.4f,0)':eval=frame[rx%d_%d];"
                                 L"[rd%d_%d]volume='if(between(t,%.4f,%.4f),%.4f,1)':eval=frame[ry%d_%d];"
                                 L"[ry%d_%d][rx%d_%d]amix=inputs=2:normalize=0[%ls]",
                                 ra, rbnd, m, ai.in, rk,
                                 ai.in, rk, ra, rbnd, 1.0f - m, ai.in, rk,
                                 ai.in, rk, ai.in, rk, nxt.c_str());
                        fc += rg;
                        cur = nxt;
                        rk++;
                    }
                }
            }
            // The track's chain, on the block's own stream. Below full amount it
            // is a wet/dry mix, the same blend the preview mixer does.
            std::wstring chain = ai.fxMix > 0.0001f ? AfxChain(ai.fx) : L"";
            wchar_t fxl[256];
            if (chain.empty()) {
                swprintf(fxl, 256, L";[%ls]anull[m%d]", cur.c_str(), ai.in);
                fc += fxl;
            } else if (ai.fxMix >= 0.999f) {
                swprintf(fxl, 256, L";[%ls]", cur.c_str());
                fc += fxl; fc += chain;
                swprintf(fxl, 256, L"[m%d]", ai.in);
                fc += fxl;
            } else {
                swprintf(fxl, 256, L";[%ls]asplit=2[d%d][w%d];[w%d]",
                         cur.c_str(), ai.in, ai.in, ai.in);
                fc += fxl; fc += chain;
                swprintf(fxl, 256,
                         L"[x%d];[d%d][x%d]amix=inputs=2:weights=%.3f %.3f:"
                         L"normalize=0[m%d]",
                         ai.in, ai.in, ai.in, 1.0f - ai.fxMix, ai.fxMix, ai.in);
                fc += fxl;
            }
            {   // the track's chain can hiss too: same gate, with room for an echo tail
                double a0 = s.offset > 0 ? s.offset : 0.0;
                double a1 = s.offset + (s.trimEnd - s.trimStart) + 0.25;
                swprintf(fxl, 256, L";[m%d]volume=0:enable='not(between(t,%.4f,%.4f))'[mg%d]",
                         ai.in, a0, a1, ai.in);
                fc += fxl;
            }
        }
        fc += L";";
        if (clipAudio) fc += L"[ac]";
        for (auto& ai : aIns) {
            wchar_t lab[32];
            swprintf(lab, 32, L"[mg%d]", ai.in);
            fc += lab;
        }
        int nMix = (int)aIns.size() + (clipAudio ? 1 : 0);
        if (nMix > 1) {
            wchar_t mx[96];
            swprintf(mx, 96, L"amix=inputs=%d:duration=first:normalize=0[apre]", nMix);
            fc += mx;
        } else {
            fc += L"anull[apre]";
        }
    } else if (clipAudio) {
        fc += L";[ac]apad[apre]";
    }
    if (audio) {                           // post chain: loudness, then fades
        fc += L";[apre]";
        bool any = false;
        if (g_loudnorm) { fc += L"loudnorm=I=-14:TP=-1.5:LRA=11"; any = true; }
        if (g_fadeIn > 0.001f) {
            wchar_t fi[96];
            swprintf(fi, 96, L"%lsafade=t=in:st=0:d=%.3f", any ? L"," : L"", g_fadeIn);
            fc += fi; any = true;
        }
        if (g_fadeOut > 0.001f) {
            double st = total - g_fadeOut;
            if (st < 0) st = 0;
            wchar_t fo[96];
            swprintf(fo, 96, L"%lsafade=t=out:st=%.3f:d=%.3f", any ? L"," : L"", st, g_fadeOut);
            fc += fo; any = true;
        }
        wchar_t atr[64];                   // same hard stop as the picture
        swprintf(atr, 64, L"%lsatrim=duration=%.4f", any ? L"," : L"", total);
        fc += atr;
        fc += L"[a]";
    }

    wchar_t tbuf[64];
    swprintf(tbuf, 64, L"%.4f", total);
    {   // The final command: -i only for inputs that stay one, everything else a source
        // filter at the head of the graph, and the graph in a file (cwd is the work dir)
        // - with every source spelled out it is far past the 32k command line.
        std::vector<int> remap(gIn.size(), -1);
        std::wstring keptIn;
        int nKept = 0;
        for (size_t i = 0; i < gIn.size(); i++)
            if (!gIn[i].kept.empty()) { keptIn += gIn[i].kept; remap[i] = nKept++; }
        std::wstring heads, body;
        body.reserve(fc.size());
        for (size_t p = 0; p < fc.size(); ) {
            // an input pad reference: [<index>:v] or [<index>:a]
            size_t q = p + 1;
            while (fc[p] == L'[' && q < fc.size() && iswdigit(fc[q])) q++;
            if (fc[p] == L'[' && q > p + 1 && q + 2 < fc.size() && fc[q] == L':' &&
                (fc[q + 1] == L'v' || fc[q + 1] == L'a') && fc[q + 2] == L']') {
                int idx = _wtoi(fc.c_str() + p + 1);
                wchar_t kind = fc[q + 1];
                if (idx >= 0 && idx < (int)gIn.size()) {
                    // concatenated, not %lc: MinGW's swprintf misreads wide conversions
                    std::wstring lab;
                    if (remap[idx] >= 0) {
                        lab = L"[" + std::to_wstring(remap[idx]) + L":" + kind + L"]";
                    } else {
                        lab = L"[in" + std::to_wstring(idx) + kind + L"]";
                        const std::wstring& src = kind == L'v' ? gIn[idx].v : gIn[idx].a;
                        heads += src + lab + L";";
                    }
                    body += lab;
                    p = q + 3;
                    continue;
                }
            }
            body += fc[p++];
        }
        // final.inputs is the same run with every input an -i, the form the final encode
        // used before it hung on long cuts: kept beside the graph so any export can be
        // replayed that way and the two compared frame for frame.
        WriteWholeFile(g_workDir + L"final.inputs", Narrow(lookInputs));
        if (!WriteWholeFile(g_workDir + L"final.graph", Narrow(heads + body))) {
            g_export.failed = true;
            g_export.message = "Could not write the export's filter graph.";
            return;
        }
        cmd.resize(inputsAt);
        cmd += keptIn;
    }
    cmd += L" -filter_complex_script final.graph -map \"[v]\"";
    // Platforms re-encode everything on upload. The master they transcode from should
    // be native resolution, native frame rate, high bitrate and fixed-GOP: that is what
    // keeps their encoder from spending its budget on our compression artifacts.
    if (audio) {
        wchar_t ab[64];
        swprintf(ab, 64, L" -map \"[a]\" -c:a aac -b:a %dk -ar 48000 -ac 2",
                 AUDIO_KBPS[g_abrIdx]);
        cmd += ab;
    }

    // The video half of the encode is kept on its own: the projector pass re-encodes
    // the film and must come out with exactly the same encoder and rate settings.
    const int vc = ResolveEncoder(g_vcodec, &g_export.encNote);
    const wchar_t* enc_name =
        vc == VC_X264 ? L"libx264" : vc == VC_X265 ? L"libx265"
      : vc == VC_NVENC_H264 ? L"h264_nvenc" : L"hevc_nvenc";
    const wchar_t* prof = (vc == VC_X264 || vc == VC_NVENC_H264) ? L"high" : L"main";
    std::wstring venc = L" -c:v " + std::wstring(enc_name) + L" -pix_fmt yuv420p -profile:v " + prof +
           L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";

    // CRF mode still gets a ceiling so a busy shot cannot blow past what an
    // uploader will accept; bitrate mode is the ceiling.
    double bitsPerPixel = 0.02 * (30 - g_crf);            // crf 15 -> 0.30, crf 23 -> 0.14
    if (bitsPerPixel < 0.05) bitsPerPixel = 0.05;
    long long maxrate = (long long)((double)W * H * FPS * bitsPerPixel);
    if (maxrate > 120000000LL) maxrate = 120000000LL;
    if (maxrate < 4000000LL)   maxrate = 4000000LL;
    if (g_rateMode == RM_BITRATE) maxrate = (long long)(g_targetMbps * 1e6);

    const int gop = FPS * 2;
    wchar_t enc[384];
    if (IsNvenc(vc)) {
        if (g_rateMode == RM_CRF)
            swprintf(enc, 384, L" -preset %ls -tune hq -rc vbr -cq %d -b:v 0"
                               L" -maxrate %lldk -bufsize %lldk -bf 3",
                     NVENC_SPEEDS[g_speed], g_crf, maxrate / 1000, maxrate / 500);
        else
            swprintf(enc, 384, L" -preset %ls -tune hq -rc vbr -b:v %lldk"
                               L" -maxrate %lldk -bufsize %lldk -bf 3",
                     NVENC_SPEEDS[g_speed], maxrate / 1000, maxrate / 1000, maxrate / 500);
    } else if (g_rateMode == RM_CRF && g_keepGrain) {
        // Grain is the one thing a ceiling destroys: at CRF 15 a grainy 1080p shot
        // wants many times the cap above, and held to it the encoder averages the
        // grain away whatever the CRF says. Keeping grain means trusting the CRF alone.
        swprintf(enc, 384, L" -preset %ls -crf %d", SPEED_NAMES[g_speed], g_crf);
    } else if (g_rateMode == RM_CRF) {
        swprintf(enc, 384, L" -preset %ls -crf %d -maxrate %lldk -bufsize %lldk",
                 SPEED_NAMES[g_speed], g_crf, maxrate / 1000, maxrate / 500);
    } else {
        swprintf(enc, 384, L" -preset %ls -b:v %lldk -maxrate %lldk -bufsize %lldk",
                 SPEED_NAMES[g_speed], maxrate / 1000, maxrate / 1000, maxrate / 500);
    }
    venc += enc;
    if (g_keepGrain && (vc == VC_X264 || vc == VC_X265))
        venc += L" -tune grain";           // NVENC has no such mode; it keeps its -tune hq
    if (vc == VC_X264) {                   // x264-only knobs; the others reject or ignore them
        wchar_t x[128];
        swprintf(x, 128, L" -level %ls -sc_threshold 0 -bf 3 -refs 4", FPS >= 50 ? L"5.1" : L"4.2");
        venc += x;
    }
    if ((vc == VC_X265 || vc == VC_NVENC_HEVC) && g_container != CT_MKV)
        venc += L" -tag:v hvc1";           // QuickTime/Apple players need the hvc1 brand
    wchar_t rate[96];
    swprintf(rate, 96, L" -r %d -g %d -keyint_min %d", FPS, gop, gop);
    venc += rate;
    // With the projector on, this encode is not the delivery: the projector pass decodes
    // it and encodes the real file. A lossy file here would make that a second
    // generation - grain and fine texture lost once, then again - so stage 1 writes a
    // lossless intermediate and the chosen encoder runs exactly once, at the end.
    std::wstring stage1Out = outPath;
    if (g_projOn) {
        stage1Out = outPath + L".stage1.mkv";
        DeleteFileW(stage1Out.c_str());
        cmd += L" -c:v libx264 -qp 0 -preset ultrafast -pix_fmt yuv444p"
               L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
        cmd += rate;
    } else {
        cmd += venc;
    }

    cmd += L" -t " + std::wstring(tbuf);
    if (!g_projOn && g_faststart && g_container != CT_MKV) cmd += L" -movflags +faststart";
    cmd += L" \"" + stage1Out + L"\"";
    if (g_faststart && g_container != CT_MKV) venc += L" -movflags +faststart";   // for stage 2

    // Shots that play on a screen of their own are rendered one at a time first, so
    // a helper drives the whole run: the per-shot passes, then this command. Without
    // any, ffmpeg is launched directly exactly as before.
    WriteWholeFile(g_workDir + L"final.cmd", Narrow(cmd));   // the encode itself, beside final.inputs
    std::wstring launch = cmd;
    if (!lookJobs.empty()) {
        std::wstring helper = ClipShaderScriptPath();
        if (helper.empty()) {
            g_export.failed = true;
            g_export.message = "clip_shader_export.py was not found next to the exe.";
            return;
        }
        if (!WriteWholeFile(g_workDir + L"looks.jobs", lookJobs) ||
            !WriteWholeFile(g_workDir + L"looks.inputs", Narrow(lookInputs)) ||
            !WriteWholeFile(g_workDir + L"looks.final", Narrow(cmd)) ||
            !WriteWholeFile(g_workDir + L"looks.args", Narrow(ProjectorArgs(W, H, false)))) {
            g_export.failed = true;
            g_export.message = "Could not stage the clip shader jobs.";
            return;
        }
        launch = Widen(g_pythonExe) + L" \"" + helper + L"\"";
    }

    g_export.cmd = Narrow(launch);
    g_export.outPath = outPath;
    g_export.stageExt = ContainerExt(g_container);
    g_export.stage1 = g_projOn ? stage1Out : L"";
    if (g_projOn && !WriteWholeFile(g_workDir + L"projector.venc", Narrow(venc))) {
        g_export.failed = true;
        g_export.message = "Could not stage the projector pass encoder settings.";
        return;
    }
    g_export.logFile = std::wstring(tmp) + L"slidecut_ffmpeg.log";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW(g_export.logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {     // header first, so a failed run is reproducible
        std::string hdr = "# " + g_export.cmd + "\r\n";
        DWORD wr = 0;
        WriteFile(log, hdr.data(), (DWORD)hdr.size(), &wr, nullptr);
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = log;
    si.hStdOutput = log;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mut(launch.begin(), launch.end());
    mut.push_back(0);
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, g_workDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        g_export.failed = true;
        g_export.message = lookJobs.empty() ? "Could not launch ffmpeg (is it on PATH?)"
                                            : "Could not launch python for the clip shaders.";
        return;
    }
    CloseHandle(pi.hThread);
    g_export.process = pi.hProcess;
    g_export.totalDur = total;
    g_export.progress = 0;
    g_export.active = true;
    g_export.failed = false;
    g_export.stage = 1;
    g_export.clipShaders = !lookJobs.empty();
    g_export.encoding = false;
    g_export.shaderStep.clear();
    g_export.finalAt = lookJobs.empty() ? GetTickCount64() : 0;   // the helper announces its own
    g_export.progAt = g_export.watchAt = g_export.cpuPrev = 0;
    g_export.pidPrev = 0;
    g_export.lastUs = -1;
    g_export.stallNote.clear();
    DeleteFileW((g_export.logFile + L".watch.txt").c_str());
    // The screens are already baked in by the per-shot passes above, so stage 2 is
    // only ever the film look. The schedule tells it which frames to leave alone:
    // a shot that played on a tube must not then be projected onto a wall.
    std::string schedule;
    const bool anyLook = LookSchedule(schedule);
    g_export.wantProjector = g_projOn;
    g_export.lookSchedule = anyLook && g_projOn;
    if (g_export.lookSchedule) WriteWholeFile(g_workDir + L"looks.schedule", schedule);
    {   // The plate aspect over the film, frame by frame, as runs: the aspect track where
        // it has a point in force, the panel's value before the first one.
        std::string runs;
        const int n = (int)(TotalDuration() * FPS + 0.5);
        int runStart = 0;
        float runAr = 0;
        for (int f = 0; f <= n; f++) {
            float ar = 0;
            if (f < n) {
                int i = AspectAt((f + 0.5) / FPS);
                ar = i >= 0 ? g_aspects[i]->aspect : g_projPlateAr;
            }
            if (f == 0) { runAr = ar; continue; }
            if (f < n && fabsf(ar - runAr) < 1e-5f) continue;
            char line[64];
            snprintf(line, sizeof(line), "%d %d %.5f\n", runStart, f - runStart, runAr);
            runs += line;
            runAr = ar; runStart = f;
        }
        g_export.aspectSchedule = !g_aspects.empty() &&
                                  WriteWholeFile(g_workDir + L"aspect.schedule", runs);
    }
    g_export.message = "Encoding…";
}

// Stage 2: hand the finished file to projector_render.py, then swap the result in.
static bool StartProjectorPass() {
    std::wstring script = ProjectorScriptPath();
    if (script.empty()) {
        g_export.message = "projector_render.py was not found - nothing exported.";
        g_export.failed = true;
        return false;
    }
    int W, H;
    ResolveCanvas(&W, &H);
    // The container that was live when the encode was built, not whatever the panel
    // says now: a clipboard copy renders mp4 whatever the delivery setting is.
    g_export.stageTmp = g_export.outPath + L".proj." + g_export.stageExt;
    DeleteFileW(g_export.stageTmp.c_str());
    // Stage 1 wrote a lossless intermediate; its audio is already the final encode, so
    // it is copied across rather than squeezed a second time.
    const std::wstring& src = g_export.stage1.empty() ? g_export.outPath : g_export.stage1;
    std::wstring cmd = Widen(g_pythonExe) + L" \"" + script + L"\" \"" + src +
                       L"\" -o \"" + g_export.stageTmp + L"\"" + ProjectorArgs(W, H, false) +
                       L" --audio copy";
    if (g_export.lookSchedule)
        cmd += L" --look-schedule \"" + g_workDir + L"looks.schedule\"";
    cmd += L" --venc-file \"" + g_workDir + L"projector.venc\"";   // same encoder as stage 1
    if (g_export.aspectSchedule)
        cmd += L" --aspect-schedule \"" + g_workDir + L"aspect.schedule\"";
    g_export.cmd = Narrow(cmd);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW(g_export.logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {     // same header as stage 1, so "open log" names the run
        std::string hdr = "# " + g_export.cmd + "\r\n";
        DWORD wr = 0;
        WriteFile(log, hdr.data(), (DWORD)hdr.size(), &wr, nullptr);
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = log;
    si.hStdOutput = log;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        g_export.message = "python could not be launched for the projector pass - nothing exported.";
        g_export.failed = true;
        return false;
    }
    CloseHandle(pi.hThread);
    g_export.process = pi.hProcess;
    g_export.stage = 2;
    g_export.active = true;
    g_export.progress = -1.0f;             // no progress feed from the script
    g_export.message = "Projector shader pass…";
    return true;
}

// The ffmpeg.exe doing the export's encode: the export process itself, or the one the
// clip-shader helper spawned under it.
static DWORD ExportFfmpegPid(DWORD& threads) {
    DWORD root = GetProcessId(g_export.process), found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, DWORD> parent;
    std::vector<std::pair<DWORD, DWORD>> ff;                 // pid, threads
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        parent[pe.th32ProcessID] = pe.th32ParentProcessID;
        if (!_wcsicmp(pe.szExeFile, L"ffmpeg.exe")) ff.push_back({ pe.th32ProcessID, pe.cntThreads });
    }
    CloseHandle(snap);
    for (auto& e : ff) {
        DWORD p = e.first;
        for (int d = 0; d < 4 && p; d++) {
            if (p == root) { found = e.first; threads = e.second; break; }
            auto it = parent.find(p);
            p = it == parent.end() ? 0 : it->second;
        }
    }
    return found;
}

// A final encode that has not moved for a minute gets sampled every 30 s: an idle
// ffmpeg (next to no CPU) is deadlocked, a busy one is only slow. Each sample goes to
// <log>.watch.txt and the latest shows next to the stage in the export panel.
static void WatchExportStall(ULONGLONG nowMs) {
    if (g_export.stage != 1 || !g_export.finalAt) return;
    ULONGLONG since = g_export.progAt > g_export.finalAt ? g_export.progAt : g_export.finalAt;
    ULONGLONG quiet = nowMs - since;
    if (quiet < 60000) {
        if (!g_export.stallNote.empty()) {
            if (FILE* w = _wfopen((g_export.logFile + L".watch.txt").c_str(), L"a")) {
                fprintf(w, "progress resumed\n");
                fclose(w);
            }
            g_export.stallNote.clear();
        }
        return;
    }
    if (g_export.watchAt && nowMs - g_export.watchAt < 30000) return;
    DWORD threads = 0;
    DWORD pid = ExportFfmpegPid(threads);
    ULONGLONG cpu = 0, mb = 0;
    if (HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u))
            cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                  (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
        PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
        if (K32GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) mb = pmc.WorkingSetSize >> 20;
        CloseHandle(h);
    }
    char note[256];
    if (!pid) {
        snprintf(note, sizeof(note), "no encode progress for %llum%02llus, ffmpeg not found",
                 quiet / 60000, quiet / 1000 % 60);
    } else {
        bool fresh = g_export.pidPrev == pid && g_export.watchAt;
        double busy = fresh ? (cpu - g_export.cpuPrev) / 1e7 / ((nowMs - g_export.watchAt) / 1000.0) : -1;
        snprintf(note, sizeof(note),
                 "no encode progress for %llum%02llus - ffmpeg %lu: %lu threads, %llu MB, %s",
                 quiet / 60000, quiet / 1000 % 60, (unsigned long)pid, (unsigned long)threads, mb,
                 busy < 0 ? "sampling CPU" :
                 busy < 0.05 ? "idle, likely deadlocked - cancel and see the log" : "busy, still working");
    }
    g_export.stallNote = note;
    g_export.watchAt = nowMs;
    g_export.cpuPrev = cpu;
    g_export.pidPrev = pid;
    if (FILE* w = _wfopen((g_export.logFile + L".watch.txt").c_str(), L"a")) {
        fprintf(w, "%s\n", note);
        fclose(w);
    }
}

static void PumpExport() {
    if (!g_export.active) return;
    // Progress: last out_time_us= line in the progress file. Reopening and reading
    // it every frame fights ffmpeg for the same file for a bar that only needs to
    // move a few times a second, so the read is on its own clock.
    // Stage 2 has no feed of its own, and stage 1's finished file is still lying there:
    // reading it would pin the bar at full instead of the indeterminate sweep.
    static ULONGLONG lastProgRead = 0;
    ULONGLONG nowMs = GetTickCount64();
    if (g_export.stage == 1 && g_export.clipShaders && !g_export.encoding &&
        nowMs - lastProgRead >= 100) {
        // The helper prints one line per shot it renders; the newest one is the step.
        HANDLE lf = CreateFileW(g_export.logFile.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (lf != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(lf, nullptr);
            DWORD want = size > 4096 ? 4096 : size;
            if (want > 0) {
                SetFilePointer(lf, -(LONG)want, nullptr, FILE_END);
                std::string buf(want, 0);
                DWORD rd = 0;
                ReadFile(lf, buf.data(), want, &rd, nullptr);
                if (!g_export.finalAt && buf.rfind("Final encode") != std::string::npos)
                    g_export.finalAt = nowMs;
                size_t p = buf.rfind("Clip shader ");
                if (p != std::string::npos) {
                    size_t e = buf.find_first_of("\r\n", p);
                    g_export.shaderStep = buf.substr(p, e == std::string::npos ? e : e - p);
                }
            }
            CloseHandle(lf);
        }
    }
    HANDLE f = (g_export.stage != 1 || nowMs - lastProgRead < 100) ? INVALID_HANDLE_VALUE : CreateFileW(g_export.progressFile.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(f, nullptr);
        DWORD want = size > 4096 ? 4096 : size;
        if (want > 0) {
            SetFilePointer(f, -(LONG)want, nullptr, FILE_END);
            std::string buf(want, 0);
            DWORD rd = 0;
            ReadFile(f, buf.data(), want, &rd, nullptr);
            size_t p = buf.rfind("out_time_us=");
            if (p != std::string::npos) {
                double us = atof(buf.c_str() + p + 12);
                if (us > g_export.lastUs) { g_export.lastUs = us; g_export.progAt = nowMs; }
                g_export.encoding = true;
                if (g_export.totalDur > 0)
                    g_export.progress = (float)fmin(us / 1e6 / g_export.totalDur, 1.0);
            }
        }
        CloseHandle(f);
        lastProgRead = nowMs;
    }
    WatchExportStall(nowMs);
    if (WaitForSingleObject(g_export.process, 0) == WAIT_OBJECT_0) {
        if (!g_export.stallNote.empty()) {
            if (FILE* w = _wfopen((g_export.logFile + L".watch.txt").c_str(), L"a")) {
                fprintf(w, "export process ended while stalled\n");
                fclose(w);
            }
            g_export.stallNote.clear();
        }
        DWORD code = 1;
        GetExitCodeProcess(g_export.process, &code);
        CloseHandle(g_export.process);
        g_export.process = nullptr;
        g_export.active = false;
        g_export.failed = code != 0;
        if (g_export.stage == 2) {         // projector pass finished
            g_export.stage = 1;
            g_export.progress = 1.0f;
            if (!g_export.stage1.empty()) DeleteFileW(g_export.stage1.c_str());   // lossless: huge
            if (code == 0 && MoveFileExW(g_export.stageTmp.c_str(), g_export.outPath.c_str(),
                                         MOVEFILE_REPLACE_EXISTING)) {
                g_export.failed = false;
                if (g_export.toClipboard) {
                    g_export.toClipboard = false;
                    bool ok = SetClipboardFile(g_export.outPath);
                    g_export.failed = !ok;
                    g_export.message = ok ? "Copied to the clipboard — paste it anywhere"
                                          : "Rendered, but the clipboard would not take it.";
                    return;
                }
                g_lastExportDir = DirName(g_export.outPath);
                g_export.message = "Done (projector) — " + Narrow(BaseName(g_export.outPath));
                if (!g_export.encNote.empty()) g_export.message += "  (" + g_export.encNote + ")";
            } else {
                DeleteFileW(g_export.stageTmp.c_str());
                g_export.toClipboard = false;
                g_export.failed = true;
                g_export.message = "Projector pass failed — see the log.";
            }
            return;
        }
        if (code == 0 && g_export.wantProjector) {
            if (StartProjectorPass()) return;
            // Stage 1 was only an intermediate, so there is nothing to hand over.
            if (!g_export.stage1.empty()) DeleteFileW(g_export.stage1.c_str());
            g_export.progress = 1.0f;
            return;                        // message already explains what went wrong
        }
        if (code == 0) {
            if (g_export.toClipboard) {
                g_export.toClipboard = false;
                bool ok = SetClipboardFile(g_export.outPath);
                g_export.failed = !ok;
                g_export.message = ok ? "Copied to the clipboard — paste it anywhere"
                                      : "Rendered, but the clipboard would not take it.";
                g_export.progress = 1.0f;
                return;
            }
            g_lastExportDir = DirName(g_export.outPath);
            g_export.message = "Done — " + Narrow(BaseName(g_export.outPath));
            if (!g_export.encNote.empty()) g_export.message += "  (" + g_export.encNote + ")";
            g_export.progress = 1.0f;
        } else {
            g_export.toClipboard = false;
            if (!g_export.stage1.empty()) DeleteFileW(g_export.stage1.c_str());
            // ffmpeg's *first* error line is the useful one — the last is usually the
            // generic "Invalid argument" epilogue, which says nothing on its own.
            g_export.message = "ffmpeg failed — see the log for details";
            std::string log = ReadTextFile(g_export.logFile);
            std::vector<std::string> lines;
            for (size_t b = 0; b < log.size(); ) {
                size_t e = log.find('\n', b);
                std::string ln = log.substr(b, e == std::string::npos ? e : e - b);
                while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                if (!ln.empty() && ln.compare(0, 2, "# ") != 0) lines.push_back(ln);
                if (e == std::string::npos) break;
                b = e + 1;
            }
            if (!lines.empty()) {
                std::string best = lines.front();
                for (auto& ln : lines) {                   // prefer a line that names a cause
                    if (ln.find("No such filter") != std::string::npos ||
                        ln.find("Unknown encoder") != std::string::npos ||
                        ln.find("Error parsing") != std::string::npos ||
                        ln.find("Unable to") != std::string::npos ||
                        ln.find("No such file") != std::string::npos) { best = ln; break; }
                }
                if (best.size() > 220) best = best.substr(0, 217) + "...";
                g_export.message = "ffmpeg: " + best;
                if (lines.size() > 1) g_export.message += "  (+" +
                    std::to_string(lines.size() - 1) + " more lines in the log)";
            }
        }
    }
}

// ----------------------------------------------------------------- timeline

struct TimelineState {
    float  pps = 60.0f;                   // pixels per second (zoom)
    float  scrollSec = 0.0f;              // left edge of view, seconds
    // drag state
    enum DragKind { None, LeftEdge, RightEdge, Move, Audio, AudioLeft, AudioRight,
                    LayerMove, LayerLeft, LayerRight, Scrub, RowResize, Fade,
                    AspectMove, Marquee, RangeIn, RangeOut } drag = None;
    int    rzKind = 0;                    // 0 base, 1 overlay, 2 audio
    float  rzStartH = 0;
    float  rzStartY = 0;
    int    dragIndex = -1;
    int    dragTrack = -1;                // overlay video track / audio track being dragged
    double dragStartVal = 0;              // duration or offset at drag start
    double dragStartVal2 = 0;             // trimStart/trimEnd at drag start
    double dragStartVal3 = 0;             // layer duration at drag start
    float  dragStartMouseX = 0;
    int    clickCollapseUid = -1;         // clicked inside a multi-selection: if the
                                          // press turns out not to be a drag, the
                                          // release drops the selection to this one
    int    clickCycleUid = -1;            // clicked a picked sound block with others
                                          // stacked under it: a still release picks
                                          // the next one down instead
    int    editIndex = -1;                // duration-edit popup target
    // (Fade is a drag kind: see TimelineState::Fade)
    int    editTrack = -1;                // -1 = base track, else overlay track
    double editValue = 0;
    bool   editOpenText = false;          // request to open the text-card editor
    double snapAt = -1e18;                // where the last snap landed, for the guide
    const char* snapWhat = nullptr;       // what it snapped to
    // Roll ("ripple off"): Ctrl while dragging a base-track edge moves the cut
    // rather than the clip's length, so the neighbour absorbs the change and every
    // shot after the seam keeps its place on the timeline.
    int    rollIndex = -1;                // neighbour clip absorbing the roll
    double rollDur = 0;                   // its duration at drag start
    double rollIn = 0;                    // its trimIn at drag start
    // While an edge is being dragged the playhead parks on that edge, so the
    // viewer shows the frame the trim is landing on. -1 = in-point, +1 = out-point.
    int    previewEdge = 0;
    // Ripple held off during the drag: the base track is packed, so trimming a shot
    // would slide every shot after it under the cursor. While the mouse is down the
    // timeline instead draws clips from holdFrom on shifted by holdShift, which is
    // exactly the amount that keeps them where they were. Letting go clears the
    // hold and the film closes up in one step -- the ripple.
    int    holdFrom = -1;
    double holdShift = 0;
    // Where the ripple starts on the timeline, measured before the drag changed
    // anything: layer clips and sound at or after this point ride the ripple out.
    double rippleAt = -1e18;
    // Ripple off: where a base shot being moved will be laid down, in timeline seconds.
    double overwriteAt = -1;
    int    overwriteUid = -1;             // the clip that drop belongs to
    // Ctrl-drag over the tracks draws a box; everything it touches is selected.
    ImVec2 boxFrom = ImVec2(0, 0);
    std::vector<int> boxKeep;             // what was already selected when it started
} g_tl;

// The stretch EXPORT RANGE renders, in timeline seconds; -1 = not marked. Set with
// i / o at the playhead or by dragging its edges on the ruler.
static double g_rangeIn = -1, g_rangeOut = -1;
static bool HasRange() { return g_rangeIn >= 0 && g_rangeOut > g_rangeIn + 1e-3; }

// text-card editor state, shared by "Add Text" and double-click-to-edit
static char  g_textBuf[1024] = "";
static float g_textScale = 0.13f;
static double g_textDur = 2.0;
static int   g_textTarget = -1;           // uid of the card being edited, -1 = creating a new one
static int   g_textTargetTrack = -1;      // which track the edited card lives on
static bool  g_textInsert = true;         // drop the new card at the playhead
static bool  g_textOpenNew = false;       // a child window asked for the editor


static double SnapDuration(double d) {
    ImGuiIO& io = ImGui::GetIO();
    double grid = io.KeyAlt ? 0.0 : (io.KeyShift ? 0.01 : 0.05);
    if (grid > 0) d = round(d / grid) * grid;
    return d < MinClipDur() ? MinClipDur() : d;
}

// Longest a clip can be: video runs out of source, stills are unbounded.
static double MaxDuration(const Clip& c) {
    if (c.kind != Clip::Video || !c.vid || c.vid->duration <= 0) return 1e9;
    double m = c.vid->duration - c.trimIn;
    return m < MinClipDur() ? MinClipDur() : m;
}

// The shot before / after this one on the base track, skipping muted shots: those
// are off the film, so a roll reaches past them to the next real neighbour.
static int PrevVisibleClip(int i) {
    for (int j = i - 1; j >= 0; j--) if (!g_clips[j]->skip) return j;
    return -1;
}
static int NextVisibleClip(int i) {
    for (int j = i + 1; j < (int)g_clips.size(); j++) if (!g_clips[j]->skip) return j;
    return -1;
}

// Trim every selected shot to one length. A video whose in-point sits too late to
// give that many seconds has its in-point pulled back rather than being left
// short; only a source shorter than the target comes up short. Shift+wheel over
// the run afterwards slides the content inside the new windows.
static int TrimSelectionTo(double len) {
    if (len < MinClipDur()) len = MinClipDur();
    int n = 0;
    auto run = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& up : v) {
            Clip& c = *up;
            if (!SelHas(c.uid)) continue;
            if (c.kind == Clip::Nest) continue;              // a folded run owns its length
            if (c.kind == Clip::Video && c.vid && c.vid->duration > 0) {
                double src = c.vid->duration;
                if (c.trimIn + len > src) {                  // pull the in-point back
                    double in = src - len;
                    c.trimIn = in < 0 ? 0 : in;
                }
                double mx = MaxDuration(c);
                c.duration = len > mx ? mx : len;
            } else {
                c.duration = len;                            // stills and cards: no ceiling
            }
            n++;
        }
    };
    run(g_clips);
    for (auto& t : g_over) run(t->clips);
    return n;
}

// ---- quantize to the beat
//
// Cuts land on the music by moving them, never by re-timing the picture: a shot
// edge inside the window of an onset is pulled onto it, everything further away
// is left alone. The onsets come from the sound tracks themselves - an energy
// flux over the decoded samples, picked against a local average - so no tempo is
// assumed and a rubato take quantizes as well as a metronomic one.

static float g_quantWin = 0.10f;           // seconds an edge may travel to reach a beat
// Cutting exactly on the transient often reads as late: the eye needs a frame or
// two to catch a cut that the ear catches instantly. This offset moves the target
// off the beat - negative lands the cut ahead of it, positive behind - and the
// window is measured from that shifted target, not from the beat itself.
static float g_quantOff = 0.0f;            // seconds, - = ahead of the beat

// Onsets of one decoded song, in source seconds. Cached per block: the decode is
// the expensive part and the samples never change once loaded.
static const std::vector<double>& SongOnsets(const Song& s) {
    struct Entry { size_t n = 0; std::vector<double> on; };
    static std::map<int, Entry> cache;
    Entry& e = cache[s.uid];
    if (e.n == s.pcm.size() && e.n) return e.on;
    e.n = s.pcm.size();
    e.on.clear();
    const size_t frames = s.pcm.size() / 2;
    const int HOP = 512;                                  // ~10.7 ms at 48k
    if (frames < (size_t)HOP * 4) return e.on;
    // Loudness envelope, one value per hop.
    std::vector<float> env(frames / HOP);
    for (size_t h = 0; h < env.size(); h++) {
        const float* p = s.pcm.data() + h * HOP * 2;
        double sum = 0;
        for (int i = 0; i < HOP; i++) {
            float m = 0.5f * (p[i * 2] + p[i * 2 + 1]);
            sum += (double)m * m;
        }
        env[h] = (float)log10(1e-9 + sqrt(sum / HOP));    // dB-ish: beats read the same loud or quiet
    }
    // Rising edges only, measured against the local average of the flux so a
    // dense passage does not fire on every hop.
    std::vector<float> flux(env.size(), 0.0f);
    for (size_t h = 1; h < env.size(); h++) {
        float d = env[h] - env[h - 1];
        flux[h] = d > 0 ? d : 0.0f;
    }
    const int W = 12;                                     // ~130 ms either side
    const double minGap = 0.08;                           // no two beats closer than this
    const double hopSec = (double)HOP / SAMPLE_RATE;
    double last = -1e9;
    for (size_t h = 1; h + 1 < flux.size(); h++) {
        if (flux[h] < flux[h - 1] || flux[h] < flux[h + 1]) continue;   // local peak only
        size_t a = h > (size_t)W ? h - W : 0, b = h + W < flux.size() ? h + W : flux.size() - 1;
        double mean = 0;
        for (size_t i = a; i <= b; i++) mean += flux[i];
        mean /= (double)(b - a + 1);
        if (flux[h] < mean * 1.6 + 0.012) continue;
        double t = h * hopSec;
        if (t - last < minGap) continue;
        last = t;
        e.on.push_back(t);
    }
    return e.on;
}

// Every onset under the film, in timeline seconds. Muted tracks are not part of
// what you are cutting to, so they are skipped.
static void BeatGrid(std::vector<double>& out) {
    out.clear();
    for (auto& tr : g_atracks) {
        if (tr->mute) continue;
        for (auto& bp : tr->blocks) {
            Song& s = *bp;
            if (!s.loaded || s.pcm.empty()) continue;
            const std::vector<double>& on = SongOnsets(s);
            for (double t : on) {
                if (t < s.trimStart || t > s.trimEnd) continue;
                double tl = s.reversed ? s.offset + (s.trimEnd - t)
                                       : s.offset + (t - s.trimStart);
                if (tl >= 0) out.push_back(tl);
            }
        }
    }
    std::sort(out.begin(), out.end());
}

// The nearest beat to t, if one sits inside the window. Returns the move, 0 for
// "leave it where it is".
static double BeatPull(const std::vector<double>& grid, double t, double win) {
    size_t i = (size_t)(std::lower_bound(grid.begin(), grid.end(), t) - grid.begin());
    double best = 0.0, bestAbs = win;
    for (int k = -1; k <= 0; k++) {
        size_t j = i + k;
        if (i == 0 && k < 0) continue;
        if (j >= grid.size()) continue;
        double d = grid[j] - t;
        if (fabs(d) <= bestAbs) { bestAbs = fabs(d); best = d; }
    }
    return best;
}

// Pull the edges of the selected shots onto the beat. The base track packs, so a
// shot's start is moved by lengthening or shortening the shot before it and the
// rest of the film rides along; an overlay shot carries its own start and simply
// slides. Nothing moves further than the window.
static int QuantizeSelectionToBeats(double win, double off, std::string& err) {
    std::vector<double> grid;
    BeatGrid(grid);
    if (grid.empty()) { err = "no beats found - load a sound track first"; return 0; }
    const double MIN = MinClipDur();
    int moved = 0;

    // ---- base track, in order, carrying the ripple forward
    {
        std::vector<BaseSpan> lay;
        BaseLayout(lay);
        double shift = 0;                                 // what the earlier edits already moved
        int prev = -1;                                    // last visible shot: it absorbs a start move
        for (int i = 0; i < (int)g_clips.size(); i++) {
            Clip& c = *g_clips[i];
            if (c.skip) continue;
            double start = lay[i].start + shift;
            if (SelHas(c.uid)) {
                if (prev >= 0) {                          // snap the cut into this shot
                    double d = BeatPull(grid, start - off, win);
                    if (d != 0.0) {
                        Clip& p = *g_clips[prev];
                        double nd = p.duration + d;
                        double mx = MaxDuration(p);
                        if (nd < MIN) nd = MIN;
                        if (nd > mx) nd = mx;
                        d = nd - p.duration;
                        if (d != 0.0) { p.duration = nd; shift += d; start += d; moved++; }
                    }
                }
                double d2 = BeatPull(grid, start + c.duration - off, win);   // snap the cut out of it
                if (d2 != 0.0) {
                    double nd = c.duration + d2;
                    double mx = MaxDuration(c);
                    if (nd < MIN) nd = MIN;
                    if (nd > mx) nd = mx;
                    d2 = nd - c.duration;
                    if (d2 != 0.0) { c.duration = nd; shift += d2; moved++; }
                }
            }
            prev = i;
        }
    }

    // ---- overlay shots: free-standing, so both edges move on their own
    for (auto& tr : g_over) {
        for (auto& up : tr->clips) {
            Clip& c = *up;
            if (!SelHas(c.uid) || c.skip) continue;
            double d = BeatPull(grid, c.start - off, win);
            if (d != 0.0) { c.start += d; if (c.start < 0) c.start = 0; moved++; }
            double d2 = BeatPull(grid, c.start + c.duration - off, win);
            if (d2 != 0.0) {
                double nd = c.duration + d2, mx = MaxDuration(c);
                if (nd < MIN) nd = MIN;
                if (nd > mx) nd = mx;
                if (nd != c.duration) { c.duration = nd; moved++; }
            }
        }
    }
    if (!moved) err = "no shot edge was within reach of a beat";
    return moved;
}

// ---- hypercut

static const int HC_MAX_SLICES = 2000;     // a beat of a frame over a long stretch

// Where a shot sits on the timeline. The base track packs, so its span comes out
// of the layout; an overlay shot carries its own start.
static bool HcSpan(int track, int index, double& s, double& e) {
    if (track < 0) {
        if (index < 0 || index >= (int)g_clips.size()) return false;
        std::vector<BaseSpan> lay;
        BaseLayout(lay);
        s = lay[index].start; e = lay[index].end;
        return e > s;
    }
    if (track < 0 || track >= (int)g_over.size()) return false;
    auto& v = g_over[track]->clips;
    if (index < 0 || index >= (int)v.size()) return false;
    s = v[index]->start; e = s + v[index]->duration;
    return e > s;
}

// Read the selection as an ordered list of participants plus the stretch all of
// them cover. One shot per track: two picks on the same track have no
// alternation to describe.
static bool HcGather(std::string& err) {
    std::vector<HcPart> parts;
    double a = -1e18, b = 1e18;
    auto take = [&](int track, Clip& c, int index) -> bool {
        if (!SelHas(c.uid)) return true;
        for (auto& p : parts)
            if (p.track == track) { err = "two shots on one track - pick one per track"; return false; }
        if (c.skip) { err = "a muted shot has no length to alternate"; return false; }
        if (track >= 0 && c.kind == Clip::Nest) { err = "unfold the sequence before hypercutting it"; return false; }
        double s, e;
        if (!HcSpan(track, index, s, e)) { err = "that shot has no length"; return false; }
        if (s > a) a = s;
        if (e < b) b = e;
        HcPart p;
        p.track = track;
        p.index = index;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s  (%s)", c.label.empty() ? "shot" : c.label.c_str(),
                 track < 0 ? "picture" : g_over[track]->name.c_str());
        p.label = buf;
        parts.push_back(p);
        return true;
    };
    for (int i = 0; i < (int)g_clips.size(); i++)
        if (!take(-1, *g_clips[i], i)) return false;
    for (int t = 0; t < (int)g_over.size(); t++)
        for (int i = 0; i < (int)g_over[t]->clips.size(); i++)
            if (!take(t, *g_over[t]->clips[i], i)) return false;

    if ((int)parts.size() < 2) { err = "pick two shots on two different tracks"; return false; }
    if (b - a < 2 * MinClipDur()) { err = "those shots do not overlap"; return false; }
    std::sort(parts.begin(), parts.end(),
              [](const HcPart& x, const HcPart& y) { return x.track < y.track; });
    g_hcParts = parts;
    g_hcA = a; g_hcB = b;
    if (g_hcFirst < 0 || g_hcFirst >= (int)parts.size()) g_hcFirst = 0;
    err.clear();
    return true;
}

static void HcDrop(std::vector<std::unique_ptr<Clip>>& v, int i) {
    if (v[i]->kind != Clip::Video) RetireTexture(v[i]->tex);
    v.erase(v.begin() + i);
}

// Cut `rem` out of one overlay shot. The ranges are timeline seconds and sorted;
// working backwards through them keeps the shot's own index and start fixed.
static void HcRemoveRanges(std::vector<std::unique_ptr<Clip>>& v, int i,
                           const std::vector<std::pair<double, double>>& rem) {
    const double MIN = MinClipDur();
    for (int k = (int)rem.size() - 1; k >= 0; k--) {
        if (i < 0 || i >= (int)v.size()) return;
        double s = v[i]->start, e = s + v[i]->duration;
        double r0 = rem[k].first  < s ? s : rem[k].first;
        double r1 = rem[k].second > e ? e : rem[k].second;
        if (r1 - r0 < 1e-9) continue;
        bool headGone = r0 - s < MIN;
        bool tailGone = e - r1 < MIN;
        if (headGone && tailGone) { HcDrop(v, i); return; }   // nothing of it survives
        if (tailGone) { v[i]->duration = r0 - s; continue; }  // shorten from the back
        if (headGone) {                                       // shorten from the front
            SplitClipIn(v, i, r1 - s);
            HcDrop(v, i);
            continue;
        }
        SplitClipIn(v, i, r1 - s);            // i = [s,r1)  i+1 = [r1,e)
        SplitClipIn(v, i, r0 - s);            // i = [s,r0)  i+1 = [r0,r1)
        HcDrop(v, i + 1);
    }
}

// The beat, in seconds. Counting in frames is the same beat measured against the
// export rate, so changing the project fps re-times a frame-counted hypercut.
static double HcSlice() {
    double s = g_hcByFrames ? (g_hcFrames < 1 ? 1 : g_hcFrames) / (double)(g_fps < 1 ? 1 : g_fps)
                            : (double)g_hcSlice;
    return s < MinClipDur() ? MinClipDur() : s;
}

// Chop the shared stretch into slices and delete, on every overlay participant,
// the slices that are not its turn. The cuts land on whole frames of the export
// rate: the beat is rounded to a frame count and the slots are counted off the
// first whole frame inside the stretch, so no slice ends mid-frame. The ragged
// sub-frame ends of the stretch itself go to the slices that reach them, which
// keeps the alternation covering every last bit of the overlap.
static void HcChop() {
    int n = (int)g_hcParts.size();
    if (n < 2) return;
    const double fps = g_fps < 1 ? 1.0 : (double)g_fps;
    long long fA = (long long)ceil (g_hcA * fps - 1e-6);   // first whole frame inside
    long long fB = (long long)floor(g_hcB * fps + 1e-6);   // last one still inside
    long long sf = (long long)llround(HcSlice() * fps);    // beat, in frames
    if (sf < 1) sf = 1;
    if (fB - fA < 2) { g_hcSlices = 0; return; }           // no room for two slices
    long long slotsLL = (fB - fA + sf - 1) / sf;
    int slots = slotsLL > HC_MAX_SLICES ? HC_MAX_SLICES : (int)slotsLL;
    if (slots < 2) slots = 2;
    g_hcSlices = slots;

    for (int p = 0; p < n; p++) {
        if (g_hcParts[p].track < 0) continue;              // the picture is never cut
        if (g_hcParts[p].track >= (int)g_over.size()) continue;
        std::vector<std::pair<double, double>> rem;
        for (int k = 0; k < slots; k++) {
            if ((g_hcFirst + k) % n == p) continue;        // its own turn, leave it
            double t0 = k == 0 ? g_hcA : (fA + k * sf) / fps;
            double t1 = k == slots - 1 ? g_hcB : (fA + (k + 1) * sf) / fps;
            if (t1 > g_hcB) t1 = g_hcB;
            if (t1 - t0 < 1e-9) continue;
            if (!rem.empty() && t0 - rem.back().second < 1e-9) rem.back().second = t1;
            else rem.push_back(std::make_pair(t0, t1));
        }
        HcRemoveRanges(g_over[g_hcParts[p].track]->clips, g_hcParts[p].index, rem);
    }

    // whatever came out of the chop is what you now have hold of
    g_selUids.clear();
    for (auto& p : g_hcParts) {
        if (p.track < 0) {
            if (p.index >= 0 && p.index < (int)g_clips.size()) g_selUids.push_back(g_clips[p.index]->uid);
            continue;
        }
        if (p.track >= (int)g_over.size()) continue;
        for (auto& c : g_over[p.track]->clips) {
            double s = c->start, e = s + c->duration;
            if (e > g_hcA + 1e-9 && s < g_hcB - 1e-9) g_selUids.push_back(c->uid);
        }
    }
    SelSync();
}

// ---- property layout
//
// Every row in the side panel is the same shape: a label in a fixed left column,
// then one control that runs to the right edge. Rows line up, controls line up,
// and nothing has to be sized by hand.

static float LabelW() { return ImGui::GetFontSize() * 7.0f; }

// Width of one cell when n of them share a row.
static float ColW(int n) {
    float gap = ImGui::GetStyle().ItemSpacing.x;
    return (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
}

// Label, then the control column. The next widget fills the rest of the row.
static void Prop(const char* label) {
    float x = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + LabelW());
    ImGui::SetNextItemWidth(-1);
}

// The hypercut block in the shot panel. It only ever sets a request: the chop
// reloads the project, which would pull the ground out from under the panel, so
// the work happens at the end of the frame.
static void HyperCutPanel() {
    Prop("hypercut");
    ImGui::TextDisabled(g_hcLive ? "previewing - commit or drop it"
                                 : "alternate two overlapping shots on two tracks");
    Prop("beat in");
    if (ImGui::RadioButton("seconds", !g_hcByFrames)) {
        if (g_hcByFrames) { g_hcByFrames = false; if (g_hcLive) g_hcReq = 2; }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("frames", g_hcByFrames)) {
        if (!g_hcByFrames) { g_hcByFrames = true; if (g_hcLive) g_hcReq = 2; }
    }
    Prop("beat");
    float half = ColW(2);
    ImGui::SetNextItemWidth(half);
    bool beatDone = false;
    if (g_hcByFrames) {
        char fmt[48];
        snprintf(fmt, sizeof(fmt), "%%d fr  (%.3f s)", HcSlice());
        ImGui::DragInt("##hcbeatf", &g_hcFrames, 0.1f, 1, 240, fmt, ImGuiSliderFlags_AlwaysClamp);
        beatDone = ImGui::IsItemDeactivatedAfterEdit();
    } else {
        ImGui::DragFloat("##hcbeat", &g_hcSlice, 0.005f, (float)MinClipDur(), 5.0f, "%.3f s",
                         ImGuiSliderFlags_AlwaysClamp);
        beatDone = ImGui::IsItemDeactivatedAfterEdit();
    }
    ImGui::SameLine();
    if (!g_hcLive) {
        if (ImGui::Button("preview hypercut", ImVec2(-1, 0))) g_hcReq = 1;
    } else if (ImGui::Button("commit", ImVec2(-1, 0))) {
        g_hcReq = 3;
    }
    if (g_hcLive) {
        if (beatDone) g_hcReq = 2;
        Prop("starts with");
        std::string items;
        for (auto& p : g_hcParts) { items += p.label; items.push_back('\0'); }
        items.push_back('\0');
        if (ImGui::Combo("##hcfirst", &g_hcFirst, items.c_str())) g_hcReq = 2;
        Prop("");
        if (ImGui::Button("flip the order", ImVec2(half, 0))) {
            g_hcFirst = (g_hcFirst + 1) % (int)g_hcParts.size();
            g_hcReq = 2;
        }
        ImGui::SameLine();
        if (ImGui::Button("drop it", ImVec2(-1, 0))) g_hcReq = 4;
    }
    if (!g_hcMsg.empty()) ImGui::TextDisabled("%s", g_hcMsg.c_str());
}


// A row of buttons in place of a combo: every choice on screen, one click away.
// The cells are all one width and the row fills the column, so the grid reads as
// a block rather than a ragged line of labels.
static bool SegW(const char* id, int* v, const char* items, float width) {
    ImGuiStyle& st = ImGui::GetStyle();
    int n = 0;
    float widest = 0;
    for (const char* p = items; *p; p += strlen(p) + 1) {
        n++;
        float w = ImGui::CalcTextSize(p).x;
        if (w > widest) widest = w;
    }
    if (!n) return false;

    const float gap = 3.0f;
    float avail = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    float need = widest + st.FramePadding.x * 2 + 2;
    int perRow = n;
    while (perRow > 1 && (avail - gap * (perRow - 1)) / perRow < need) perRow--;
    int rows = (n + perRow - 1) / perRow;      // spread evenly, no orphan last row
    perRow = (n + rows - 1) / rows;
    float w = (avail - gap * (perRow - 1)) / perRow;
    float h = ImGui::GetFrameHeight() * 0.84f;

    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    int i = 0;
    for (const char* p = items; *p; p += strlen(p) + 1, i++) {
        if (i % perRow) ImGui::SameLine(0, gap);
        bool on = (*v == i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? st.Colors[ImGuiCol_ButtonActive]
                                                  : ImVec4(1, 1, 1, 0.045f));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1)
                                                : st.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(p, ImVec2(w, h))) { *v = i; changed = true; }
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

static bool Seg(const char* id, int* v, const char* items) {
    return SegW(id, v, items, 0);
}

// Seg on a property row.
static bool SegRow(const char* label, int* v, const char* items) {
    Prop(label);
    return Seg(label, v, items);
}

// A section header that reads like a strip of leader: typewriter caption, then
// sprocket holes running out to the edge of the panel. Clicking it folds the
// section away; the return value says whether to draw the body.
static bool Reel(const char* label, bool* open) {
    ImGui::Dummy(ImVec2(1, 6));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
    float px = SafePx(ImGui::GetFontSize() * 1.15f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, label);
    float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    ImGui::InvisibleButton("##hdr", ImVec2(w, ts.y));
    bool hot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && open) *open = !*open;
    ImGui::PopID();

    bool shown = !open || *open;
    ImU32 ink = hot ? IM_COL32(255, 255, 255, 255)
                    : (shown ? IM_COL32(226, 222, 212, 255) : IM_COL32(150, 147, 140, 255));
    dl->AddText(tf, px, p, ink, label);
    float y  = p.y + ts.y * 0.55f;
    float x  = p.x + ts.x + 10;
    float xr = p.x + w;
    dl->AddLine(ImVec2(x, y), ImVec2(xr, y), IM_COL32(255, 255, 255, 24));
    // Sprockets while open, a single dashed run while folded.
    for (float sx = x + 6; sx + 8 < xr; sx += 14)
        dl->AddRectFilled(ImVec2(sx, y - (shown ? 3.0f : 1.0f)),
                          ImVec2(sx + 8, y + (shown ? 3.0f : 1.0f)),
                          IM_COL32(255, 255, 255, shown ? 22 : 14), 1.5f);
    ImGui::Dummy(ImVec2(1, 2));
    return shown;
}

// Editor for a Special Elite title card. g_textTarget < 0 creates a new clip,
// otherwise it rewrites an existing one.
static void DrawTextCardPopup() {
    if (!ImGui::BeginPopupModal("Text card", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("Text (Enter = new line)");
    ImGui::InputTextMultiline("##txt", g_textBuf, sizeof(g_textBuf), ImVec2(420, 110));
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("size", &g_textScale, 0.03f, 0.30f, "%.3f x frame height");
    ImGui::SetNextItemWidth(200);
    ImGui::InputDouble("seconds", &g_textDur, MinClipDur(), 0.5, "%.3f");
    ImGui::SameLine();
    if (ImGui::SmallButton("1 frame")) g_textDur = MinClipDur();
    ImGui::SameLine();
    if (ImGui::SmallButton("3")) g_textDur = 3.0 * MinClipDur();
    if (g_textTarget < 0)
        ImGui::Checkbox("insert at playhead (splits the clip under it)", &g_textInsert);
    if (!g_titleFont)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "assets/%ls not found — preview uses the UI font",
                           TITLE_FONT_FILE);

    bool ok = ImGui::Button(g_textTarget < 0 ? "Add" : "Save", ImVec2(90, 0));
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(90, 0));
    if (ok) {
        double d = g_textDur < MinClipDur() ? MinClipDur() : g_textDur;
        if (g_textTarget >= 0) {
            // Editing a card: rewrite it wherever it now lives. If it's gone, the
            // edit is dropped - never turned into a new card on the base track.
            if (Clip* c = ClipByUid(g_textTarget)) {
                c->text = g_textBuf;
                c->textScale = g_textScale;
                c->duration = d;
                c->label = FirstLine(c->text);
            }
        } else {
            int at = g_textInsert ? SplitPoint(g_playhead.load()) : -1;
            AddTextClip(g_textBuf, d, g_textScale, at);
        }
        ImGui::CloseCurrentPopup();
    }
    if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void DrawTimeline() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_binDrop.lib >= 0) {
        BinDrop d = g_binDrop;
        g_binDrop = BinDrop();
        DropLibraryItem(d.lib, d.rowKind, d.track, d.t);
    }

    const float RULER_H = 22.0f, CLIP_H = 58.0f, AUDIO_H = 44.0f, ROW_GAP = 3.0f;
    const float HDR_W = 104.0f;             // track-name column
    const float EDGE = 7.0f;

    const float GHOST_H = 18.0f;            // "drop here for a new track" strips
    int nOver = (int)g_over.size(), nAud = (int)g_atracks.size();
    bool anyMuted = false;
    for (auto& c : g_clips) if (c->skip) { anyMuted = true; break; }
    float rowsH = g_baseH + ROW_GAP + (anyMuted ? MUTE_ROW_H + ROW_GAP : 0.0f);
    if (g_aspectsVisible) rowsH += ASPECT_ROW_H + ROW_GAP;
    for (auto& t : g_over)   rowsH += t->height + ROW_GAP;
    for (auto& t : g_atracks) rowsH += t->height + ROW_GAP;
    float needH = RULER_H + 4 + GHOST_H + ROW_GAP + rowsH + 6 + GHOST_H + 6;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float viewH = avail.y > 40 ? avail.y : 40;
    ImGui::InvisibleButton("timeline", ImVec2(avail.x, needH > viewH ? needH : viewH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    float trackX = origin.x + HDR_W;        // where the time area starts
    float trackW = avail.x - HDR_W;

    auto SecToX = [&](double t) { return trackX + (float)((t - g_tl.scrollSec) * g_tl.pps); };
    auto XToSec = [&](float x) { return (double)(x - trackX) / g_tl.pps + g_tl.scrollSec; };
    bool inTracks = hovered && io.MousePos.x > trackX;

    // zoom (ctrl+wheel around cursor), slip (shift+wheel over a clip), pan (wheel)
    float slipWheel = 0.0f;                 // held until the hit test knows the clip
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyShift && !io.KeyCtrl && inTracks) {
            slipWheel = io.MouseWheel;
        } else if (io.KeyCtrl) {
            double anchor = XToSec(io.MousePos.x);
            g_tl.pps *= powf(1.25f, io.MouseWheel);
            g_tl.pps = g_tl.pps < 4 ? 4 : (g_tl.pps > 2000 ? 2000 : g_tl.pps);
            g_tl.scrollSec = (float)(anchor - (io.MousePos.x - trackX) / g_tl.pps);
        } else {
            g_tl.scrollSec -= io.MouseWheel * 60.0f / g_tl.pps;
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        g_tl.scrollSec -= io.MouseDelta.x / g_tl.pps;
    if (g_tl.scrollSec < -2) g_tl.scrollSec = -2;

    double total = TotalDuration();

    // Snap a position to anything an editor would want it aligned to: the start,
    // the playhead, every base cut, and the edges of every layer clip and audio
    // block except the one being dragged. The window is in pixels, so it tightens
    // as the timeline is zoomed in. Alt holds it off.
    auto snapPos = [&](double pos, int skipUid = -1) {
        if (io.KeyAlt) return pos;
        double snapPx = 9.0 / g_tl.pps;
        double best = 1e18, bestTo = pos;
        const char* what = nullptr;
        auto consider = [&](double target, const char* name) {
            double d = fabs(pos - target);
            if (d < snapPx && d < best) { best = d; bestTo = target; what = name; }
        };
        consider(0.0, "start");
        consider(g_playhead.load(), "playhead");
        double s = 0;
        {
            std::vector<BaseSpan> lay;
            BaseLayout(lay);
            for (size_t i = 0; i < lay.size(); i++) {
                if (g_clips[i]->skip) continue;
                consider(lay[i].end, "cut");
                if (lay[i].fade > 0) consider(lay[i].end - lay[i].fade, "fade");
            }
        }
        (void)s;
        for (auto& t : g_over)
            for (auto& c : t->clips) {
                if (c->uid == skipUid) continue;
                consider(c->start, "layer");
                consider(c->start + c->duration, "layer");
            }
        for (auto& t : g_atracks)
            for (auto& b : t->blocks) {
                consider(b->offset, "sound");
                consider(b->offset + (b->trimEnd - b->trimStart), "sound");
            }
        if (what) { g_tl.snapAt = bestTo; g_tl.snapWhat = what; }
        return bestTo;
    };
    g_tl.snapAt = -1e18;
    g_tl.snapWhat = nullptr;

    // ---- rows: overlay tracks (top), base video, audio tracks
    struct Row { float y0, y1; int kind; int idx; };   // kind 0 base, 1 overlay, 2 audio
    std::vector<Row> rows;
    float y = origin.y + RULER_H + 4;
    if (g_aspectsVisible) {                            // aspect points, kind 6
        rows.push_back({ y, y + ASPECT_ROW_H, 6, -1 });
        y += ASPECT_ROW_H + ROW_GAP;
    }
    float ghostVy0 = y, ghostVy1 = y + GHOST_H;        // new video track
    y += GHOST_H + ROW_GAP;
    for (int t = nOver - 1; t >= 0; t--) {             // last track drawn topmost
        float th = g_over[t]->height;
        rows.push_back({ y, y + th, 1, t });
        y += th + ROW_GAP;
    }
    rows.push_back({ y, y + g_baseH, 0, -1 });
    y += g_baseH + ROW_GAP;
    if (anyMuted) {                                    // the mute siding, kind 5
        rows.push_back({ y, y + MUTE_ROW_H, 5, -1 });
        y += MUTE_ROW_H + ROW_GAP;
    }
    y += 6;
    for (int t = 0; t < nAud; t++) {
        float th = g_atracks[t]->height;
        rows.push_back({ y, y + th, 2, t });
        y += th + ROW_GAP;
    }
    float ghostAy0 = y, ghostAy1 = y + GHOST_H;        // new audio track
    y += GHOST_H;
    auto inGhostV = [&](ImVec2 p) { return p.y >= ghostVy0 && p.y <= ghostVy1 && p.x > trackX; };
    auto inGhostA = [&](ImVec2 p) { return p.y >= ghostAy0 && p.y <= ghostAy1 && p.x > trackX; };
    g_geom.valid = true;                    // cached for drops (see WM_DROPFILES)
    g_geom.trackX = trackX;
    g_geom.pps = g_tl.pps;
    g_geom.scroll = g_tl.scrollSec;
    g_geom.rows.clear();
    for (auto& r : rows) g_geom.rows.push_back({ r.y0, r.y1, r.kind, r.idx });
    g_geom.rows.push_back({ ghostVy0, ghostVy1, 3, -1 });   // drop = spawn a video track
    g_geom.rows.push_back({ ghostAy0, ghostAy1, 4, -1 });   // drop = spawn an audio track

    // ---- a card dragged in from the bin: the row under the pointer picks the track
    // (the strips spawn a new one), the pointer's x the time. A guide shows the spot.
    {
        ImRect box(origin, ImVec2(origin.x + avail.x, origin.y + (needH > viewH ? needH : viewH)));
        if (ImGui::BeginDragDropTargetCustom(box, ImGui::GetID("##binDrop"))) {
            const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SLIDECUT_MEDIA_ORDER",
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (pl && io.MousePos.x > trackX) {
                double t = XToSec(io.MousePos.x);
                if (t < 0) t = 0;
                if (!io.KeyAlt) t = round(t * g_fps) / g_fps;           // land on a frame
                for (auto& r : g_geom.rows) {
                    if (io.MousePos.y < r.y0 || io.MousePos.y > r.y1) continue;
                    if (r.kind > 4) break;                               // aspect / mute rows
                    float gx = SecToX(t);
                    dl->AddRectFilled(ImVec2(trackX, r.y0), ImVec2(origin.x + avail.x, r.y1),
                                      IM_COL32(255, 255, 255, 18));
                    dl->AddLine(ImVec2(gx, r.y0), ImVec2(gx, r.y1), IM_COL32(255, 255, 255, 230), 2.0f);
                    ImGui::SetTooltip("%s  %d:%05.2f",
                                      r.kind == 0 ? "insert into the cut" :
                                      r.kind == 1 ? "place on this layer" :
                                      r.kind == 2 ? "place on this audio track" :
                                      r.kind == 3 ? "new video track" : "new audio track",
                                      (int)t / 60, fmod(t, 60.0));
                    if (pl->IsDelivery()) {
                        g_binDrop.lib = *(const int*)pl->Data;
                        g_binDrop.rowKind = r.kind;
                        g_binDrop.track = r.idx;
                        g_binDrop.t = t;
                    }
                    break;
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // ---- ruler
    float rulerY = origin.y;
    dl->AddRectFilled(ImVec2(origin.x, rulerY), ImVec2(origin.x + avail.x, rulerY + RULER_H),
                      IM_COL32(20, 20, 20, 255));
    double step = 1.0;
    while (step * g_tl.pps < 70) step *= 2;
    while (step * g_tl.pps > 220) step /= 2;
    double t0 = floor(g_tl.scrollSec / step) * step;
    for (double t = t0; SecToX(t) < origin.x + avail.x; t += step) {
        float x = SecToX(t);
        if (x < trackX) continue;
        dl->AddLine(ImVec2(x, rulerY + RULER_H - 7), ImVec2(x, rulerY + RULER_H),
                    IM_COL32(105, 105, 105, 255));
        char lbl[32];
        if (step >= 1.0) snprintf(lbl, 32, "%d:%02d", (int)t / 60, (int)t % 60);
        else snprintf(lbl, 32, "%.2fs", t);
        dl->AddText(ImVec2(x + 4, rulerY + 3), IM_COL32(135, 135, 135, 255), lbl);
    }

    // ---- row backgrounds + header column
    for (auto& r : rows) {
        ImU32 bg = r.kind == 0 ? IM_COL32(21, 21, 21, 255)
                 : r.kind == 1 ? IM_COL32(16, 16, 16, 255)
                 : r.kind == 5 ? IM_COL32(18, 15, 15, 255)
                               : IM_COL32(13, 13, 13, 255);
        dl->AddRectFilled(ImVec2(trackX, r.y0), ImVec2(origin.x + avail.x, r.y1), bg, 3.0f);
        dl->AddRectFilled(ImVec2(origin.x, r.y0), ImVec2(trackX - 4, r.y1),
                          IM_COL32(28, 28, 28, 255), 4.0f);
        // sprocket holes down the outside of the header, like the edge of a strip
        for (float sy = r.y0 + 7; sy + 9 < r.y1; sy += 15)
            dl->AddRectFilled(ImVec2(trackX - 13, sy), ImVec2(trackX - 7, sy + 9),
                              IM_COL32(255, 255, 255, 26), 1.5f);
        // Only the picture and sound rows stand for a track; the siding and the
        // aspect row carry no index, so they never reach for one.
        const char* name = r.kind == 0 ? "PICTURE"
                         : r.kind == 5 ? "MUTED"
                         : r.kind == 6 ? "ASPECT"
                         : r.kind == 1 ? g_over[r.idx]->name.c_str()
                         : r.kind == 2 ? g_atracks[r.idx]->name.c_str()
                                       : "";
        bool on = r.kind == 1 ? g_over[r.idx]->visible
                : r.kind == 2 ? !g_atracks[r.idx]->mute
                : r.kind == 0 ? !g_baseOff
                : r.kind != 5;
        dl->AddText(ImVec2(origin.x + 8, r.y0 + 6),
                    on ? IM_COL32(220, 220, 220, 255) : IM_COL32(110, 110, 110, 255), name);
        if (r.kind == 0 || r.kind == 1 || r.kind == 2) {   // a mute / hide toggle under the name
            ImVec2 b0(origin.x + 8, r.y1 - 22), b1(origin.x + 34, r.y1 - 6);
            bool overBtn = io.MousePos.x >= b0.x && io.MousePos.x <= b1.x &&
                           io.MousePos.y >= b0.y && io.MousePos.y <= b1.y && hovered;
            dl->AddRectFilled(b0, b1, on ? IM_COL32(72, 72, 72, 255) : IM_COL32(34, 34, 34, 255), 4.0f);
            dl->AddText(ImVec2(b0.x + 7, b0.y + 1), IM_COL32(235, 235, 235, 255),
                        r.kind == 1 ? (on ? "vis" : "off") : (on ? "on" : "mute"));
            if (overBtn) ImGui::SetTooltip(r.kind == 0
                ? "park the picture track — preview only, the export keeps it"
                : (r.kind == 1 ? "hide this layer" : "mute this sound track"));
            if (overBtn && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if      (r.kind == 0) g_baseOff = !g_baseOff;
                else if (r.kind == 1) g_over[r.idx]->visible = !g_over[r.idx]->visible;
                else                  g_atracks[r.idx]->mute = !g_atracks[r.idx]->mute;
            }
        }
    }

    // ---- new-track strips
    {
        bool dragging = g_tl.drag == TimelineState::Move || g_tl.drag == TimelineState::LayerMove ||
                        g_tl.drag == TimelineState::Audio;
        auto strip = [&](float y0, float y1, bool hot, const char* label) {
            ImU32 col = hot ? IM_COL32(235, 235, 235, 220)
                            : (dragging ? IM_COL32(150, 150, 150, 160) : IM_COL32(70, 70, 70, 120));
            dl->AddRect(ImVec2(trackX, y0), ImVec2(origin.x + avail.x - 2, y1), col, 4.0f,
                        0, hot ? 2.0f : 1.0f);
            dl->AddText(ImVec2(trackX + 10, y0 + 1),
                        hot ? IM_COL32(240, 240, 240, 255) : IM_COL32(115, 115, 115, 200), label);
        };
        strip(ghostVy0, ghostVy1, hovered && inGhostV(io.MousePos), "+ picture");
        strip(ghostAy0, ghostAy1, hovered && inGhostA(io.MousePos), "+ sound");
    }

    // A clip that keeps its own sound gets its waveform along the bottom edge, so
    // cuts can be placed on a beat without moving the audio onto a track first.
    auto drawClipWave = [&](const Clip& c, float x0, float x1, float y0, float y1,
                            double localStart) {
        if (c.kind != Clip::Video || !c.useAudio || !c.vid) return;
        if (!c.vid->apeaksReady.load() || !c.vid->audio) return;
        const std::vector<float>& pk = c.vid->audio->peaks;
        size_t n = pk.size() / 2;
        if (!n || x1 - x0 < 6) return;
        float h = (y1 - y0) * 0.30f;
        float base = y1 - 1;
        const float* pkp = pk.data();
        const double rate = c.vid->apeakRate;
        for (float x = x0; x < x1; x += 1.0f) {
            double tIn = localStart + (XToSec(x) - XToSec(x0));
            size_t i = (size_t)(tIn * rate);
            if (i >= n) break;
            float amp = pkp[i * 2 + 1] - pkp[i * 2];     // peak to peak, 0..2
            if (amp <= 0.0f) continue;                   // silence: nothing to draw
            if (amp > 2.0f) amp = 2.0f;
            dl->AddLine(ImVec2(x, base), ImVec2(x, base - amp * 0.5f * h),
                        IM_COL32(255, 255, 255, 90));
        }
    };

    // The bottom few pixels of a header cell are a grab handle for the row height.
    int hotResizeKind = -1, hotResizeIdx = -1;
    for (auto& r : rows) {
        if (!hovered) break;
        if (io.MousePos.x > trackX - 4) break;
        // The grab is the TOP edge: rows grow downward and the timeline pane is
        // anchored to the window bottom, so pulling the top up is what visually
        // makes the row taller.
        if (r.kind == 5 || r.kind == 6) continue;   // siding and aspect row: fixed height
        if (fabsf(io.MousePos.y - r.y0) <= 5.0f) { hotResizeKind = r.kind; hotResizeIdx = r.idx; }
    }
    if (hotResizeKind >= 0 || g_tl.drag == TimelineState::RowResize)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // ---- hit state gathered while drawing
    int hotMute = -1;                      // a muted shot's tab, click to bring it back
    int hotFade = -1;                      // the dissolve between this shot and the next
    int hotEdgeClip = -1, hotEdgeSide = 0, hotBody = -1;          // base track
    int hotLayerTrack = -1, hotLayer = -1, hotLayerSide = 0;      // overlay tracks
    int hotAudTrack = -1, hotAudBlock = -1, hotAudSide = 0;
    std::vector<int> audHits;              // every block under the mouse, bottom to top
    int hotAspect = -1;

    // ---- base video track
    float clipY = 0, clipH = g_baseH;
    for (auto& r : rows) if (r.kind == 0) { clipY = r.y0; clipH = r.y1 - r.y0; }
    {
        double start = 0;
        struct MuteTab { float x; int idx; };   // x = the cut the shot was pulled off
        std::vector<MuteTab> tabs;
        std::vector<BaseSpan> lay;
        BaseLayout(lay);
        for (int i = 0; i < (int)g_clips.size(); i++) {
            Clip& c = *g_clips[i];
            double hold = (g_tl.holdFrom >= 0 && i >= g_tl.holdFrom) ? g_tl.holdShift : 0.0;
            float x0 = SecToX(lay[i].start + hold), x1 = SecToX(lay[i].end + hold);
            float rawX0 = x0;                  // unclamped: the real in-point edge
            start = lay[i].end;
            if (c.skip) {
                tabs.push_back({ x0, i });
                continue;
            }
            if (x1 < trackX || x0 > origin.x + avail.x) continue;
            if (x0 < trackX) x0 = trackX;

            if (IsGap(c)) {
                // Empty film: a faint outline and nothing else. It is not a clip to pick
                // up or edit - hovering it only lets Delete close it.
                dl->AddRect(ImVec2(x0 + 1, clipY + 2), ImVec2(x1 - 1, clipY + clipH - 2),
                            IM_COL32(255, 255, 255, 22), 5.0f);
                if (inTracks && io.MousePos.y >= clipY && io.MousePos.y <= clipY + clipH &&
                    hotEdgeClip == -1 && io.MousePos.x > x0 && io.MousePos.x < x1)
                    hotBody = i;
                continue;
            }

            if (c.kind == Clip::Video && c.vid) c.tex = ProxyFrame(*c.vid, c.trimIn);
            bool isDragged = g_tl.drag == TimelineState::Move && g_tl.dragIndex == i;
            bool selected = SelHas(c.uid);
            ImU32 fill = c.kind == Clip::Text
                       ? (isDragged ? IM_COL32(34, 34, 34, 255) : IM_COL32(12, 12, 12, 255))
                       : (isDragged ? IM_COL32(78, 78, 78, 255) : IM_COL32(48, 48, 48, 255));
            dl->AddRectFilled(ImVec2(x0 + 1, clipY + 2), ImVec2(x1 - 1, clipY + clipH - 2),
                              fill, 5.0f);
            if (c.tex) {
                float bw = x1 - x0 - 2, bh = clipH - 4;
                if (bw > 8) {
                    float boxA = bw / bh;
                    ImVec2 uv0(0, 0), uv1(1, 1);
                    if (c.texAspect > boxA) { float f = boxA / c.texAspect;
                                              uv0.x = 0.5f - f * 0.5f; uv1.x = 0.5f + f * 0.5f; }
                    else                    { float f = c.texAspect / boxA;
                                              uv0.y = 0.5f - f * 0.5f; uv1.y = 0.5f + f * 0.5f; }
                    dl->AddImageRounded((ImTextureID)c.tex, ImVec2(x0 + 1, clipY + 2),
                                        ImVec2(x1 - 1, clipY + clipH - 2), uv0, uv1,
                                        IM_COL32(255, 255, 255, isDragged ? 160 : 220), 5.0f);
                }
            } else if (c.kind == Clip::Nest) {
                // A folded sequence reads as a spool: the first frame inside it, a
                // double edge, and its name where a clip would show its file.
                std::vector<NestHit> hits;
                NestResolve(c, 0.0, hits);
                ID3D11ShaderResourceView* ft = nullptr;
                float fa = 1.0f;
                if (!hits.empty()) {
                    Clip& in0 = *hits[0].clip;
                    if (in0.kind == Clip::Video && in0.vid) {
                        ft = ProxyFrame(*in0.vid, in0.trimIn);
                        fa = in0.vid->aspect > 0 ? in0.vid->aspect : 1.0f;
                    } else if (in0.kind == Clip::Image) { ft = in0.tex; fa = in0.texAspect; }
                }
                float bw = x1 - x0 - 2, bh = clipH - 4;
                if (ft && bw > 8) {
                    ImVec2 uv0(0, 0), uv1(1, 1);
                    float boxA = bw / bh;
                    if (fa > boxA) { float f = boxA / fa;
                                     uv0.x = 0.5f - f * 0.5f; uv1.x = 0.5f + f * 0.5f; }
                    else           { float f = fa / boxA;
                                     uv0.y = 0.5f - f * 0.5f; uv1.y = 0.5f + f * 0.5f; }
                    dl->AddImageRounded((ImTextureID)ft, ImVec2(x0 + 1, clipY + 2),
                                        ImVec2(x1 - 1, clipY + clipH - 2), uv0, uv1,
                                        IM_COL32(255, 255, 255, 120), 5.0f);
                }
                dl->AddRect(ImVec2(x0 + 4, clipY + 5), ImVec2(x1 - 4, clipY + clipH - 5),
                            IM_COL32(255, 255, 255, 70), 4.0f);
                Sequence* q = FindSeq(c.nest);
                char nl[80];
                snprintf(nl, sizeof(nl), "%s  (%d)", q ? q->name.c_str() : "sequence",
                         q ? (int)q->clips.size() : 0);
                ImVec2 ns = ImGui::CalcTextSize(nl);
                if (x1 - x0 > ns.x + 16) {
                    dl->AddRectFilled(ImVec2(x0 + 8, clipY + 6),
                                      ImVec2(x0 + 16 + ns.x, clipY + 10 + ns.y),
                                      IM_COL32(0, 0, 0, 190), 3.0f);
                    dl->AddText(ImVec2(x0 + 12, clipY + 8), IM_COL32(235, 235, 235, 255), nl);
                }
            } else if (c.kind == Clip::Text) {
                std::string one = FirstLine(c.text);
                ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
                float px = 20.0f;
                ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, one.c_str());
                float bw = x1 - x0 - 10;
                if (bw > 0 && ts.x > bw && ts.x > 0) {
                    px = SafePx(px * bw / ts.x);
                    ts = tf->CalcTextSizeA(px, FLT_MAX, 0, one.c_str());
                }
                if (bw > 10)
                    dl->AddText(tf, px, ImVec2(x0 + (x1 - x0 - ts.x) * 0.5f,
                                               clipY + (clipH - ts.y) * 0.5f - 4),
                                IM_COL32(240, 240, 240, 255), one.c_str());
            }
            dl->AddRect(ImVec2(x0 + 1, clipY + 2), ImVec2(x1 - 1, clipY + clipH - 2),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 40),
                        5.0f, 0, selected ? 2.5f : 1.0f);
            {   // badges: what is stacked on this clip
                float bx = x1 - 8;
                if (c.reversed) {
                    dl->AddText(ImVec2(x0 + 6, clipY + 4), IM_COL32(235, 235, 235, 210), "<<");
                }
                if (c.grade.On())
                    dl->AddText(ImVec2(x0 + (c.reversed ? 28.0f : 6.0f), clipY + 4),
                                IM_COL32(235, 235, 235, 210), c.grade.mono ? "B&W" : "col");
                if (c.dxOn) { dl->AddCircleFilled(ImVec2(bx, clipY + 10), 4.0f,
                                                  IM_COL32(235, 235, 235, 230)); bx -= 11; }
                if ((c.ovlOn && !c.ovlText.empty()) || c.kind == Clip::Text)
                    dl->AddCircleFilled(ImVec2(bx, clipY + 10), 4.0f, IM_COL32(150, 150, 150, 230));
            }
            drawClipWave(c, x0 + 1, x1 - 1, clipY + 2, clipY + clipH - 2, c.trimIn);
            char dur[32];
            snprintf(dur, 32, "%.2fs", c.duration);
            ImVec2 ds = ImGui::CalcTextSize(dur);
            if (x1 - x0 > ds.x + 12) {
                dl->AddRectFilled(ImVec2(x0 + 5, clipY + clipH - ds.y - 9),
                                  ImVec2(x0 + 11 + ds.x, clipY + clipH - 5),
                                  IM_COL32(0, 0, 0, 170), 3.0f);
                dl->AddText(ImVec2(x0 + 8, clipY + clipH - ds.y - 7),
                            IM_COL32(235, 235, 235, 255), dur);
            }

            // hit zones — every clip owns the band on its own side of a cut, so the
            // left half of a seam trims the out-point of the shot before it and the
            // right half trims the in-point of the shot after it. Claiming the left
            // edge overrides a previous clip's right-edge claim on the same pixel.
            if (inTracks && io.MousePos.y >= clipY && io.MousePos.y <= clipY + clipH) {
                if (c.kind == Clip::Nest) {
                    if (io.MousePos.x > x0 && io.MousePos.x < x1) hotBody = i;
                } else if (rawX0 >= trackX && io.MousePos.x >= rawX0 &&
                           io.MousePos.x - rawX0 <= EDGE && io.MousePos.x < x1) {
                    hotEdgeClip = i; hotEdgeSide = -1;
                    hotBody = -1;
                } else if (hotEdgeClip == -1 && io.MousePos.x <= x1 &&
                           x1 - io.MousePos.x <= EDGE && io.MousePos.x > x0) {
                    // own side only: past x1 is the next shot's in-point band
                    hotEdgeClip = i; hotEdgeSide = +1;
                } else if (hotEdgeClip == -1 && io.MousePos.x > x0 && io.MousePos.x < x1) {
                    hotBody = i;
                }
            }
        }
        // A dissolve is the overlap between two shots. It is drawn over the seam as
        // a wedge, with a grip in the middle to drag its length.
        for (int i = 0; i < (int)g_clips.size(); i++) {
            if (g_clips[i]->skip || lay[i].fade <= 0.0001) continue;
            double hold = (g_tl.holdFrom >= 0 && i >= g_tl.holdFrom) ? g_tl.holdShift : 0.0;
            float fx0 = SecToX(lay[i].end - lay[i].fade + hold), fx1 = SecToX(lay[i].end + hold);
            if (fx1 < trackX || fx0 > origin.x + avail.x) continue;
            float ytop = clipY + 2, ybot = clipY + clipH - 2;
            dl->AddRectFilled(ImVec2(fx0, ytop), ImVec2(fx1, ybot), IM_COL32(0, 0, 0, 90));
            dl->AddLine(ImVec2(fx0, ybot), ImVec2(fx1, ytop), IM_COL32(255, 255, 255, 150), 1.5f);
            dl->AddLine(ImVec2(fx0, ytop), ImVec2(fx1, ybot), IM_COL32(255, 255, 255, 60), 1.0f);
            dl->AddLine(ImVec2(fx0, ytop), ImVec2(fx0, ybot), IM_COL32(255, 255, 255, 90));
            dl->AddLine(ImVec2(fx1, ytop), ImVec2(fx1, ybot), IM_COL32(255, 255, 255, 90));
            char fl[32];
            snprintf(fl, sizeof(fl), "%.2fs", lay[i].fade);
            ImVec2 fs = ImGui::CalcTextSize(fl);
            if (fx1 - fx0 > fs.x + 10)
                dl->AddText(ImVec2((fx0 + fx1 - fs.x) * 0.5f, ytop + 2),
                            IM_COL32(255, 255, 255, 200), fl);
            if (inTracks && io.MousePos.y >= ytop && io.MousePos.y <= ybot &&
                io.MousePos.x >= fx0 - 3 && io.MousePos.x <= fx1 + 3) {
                hotFade = i;
                hotBody = -1;
                hotEdgeClip = -1;
            }
        }

        // A muted shot is out of the film, so it is parked on the mute siding under
        // the picture row: still at its own length, still sitting near the cut it
        // came off, still selectable and gradeable. Double-click puts it back.
        float muteY = 0, muteH = MUTE_ROW_H;
        for (auto& r : rows) if (r.kind == 5) { muteY = r.y0; muteH = r.y1 - r.y0; }
        if (muteY > 0) {
            float prevEnd = trackX;                    // parked shots never overlap
            for (auto& t : tabs) {
                Clip& c = *g_clips[t.idx];
                float w = (float)(c.duration * g_tl.pps);
                if (w < 26.0f) w = 26.0f;
                float x0 = t.x < prevEnd ? prevEnd : t.x;
                float x1 = x0 + w;
                prevEnd = x1 + 2.0f;
                if (x1 < trackX || x0 > origin.x + avail.x) continue;
                bool selected = SelHas(c.uid);
                ImVec2 a(x0 + 1, muteY + 2), b(x1 - 1, muteY + muteH - 2);
                dl->AddRectFilled(a, b, IM_COL32(30, 26, 26, 255), 4.0f);
                for (float hx = 4; hx < (b.x - a.x) + (b.y - a.y); hx += 7.0f) {  // hatch
                    float span = b.y - a.y;
                    float x1h = a.x + (hx > (b.x - a.x) ? (b.x - a.x) : hx);
                    float y1h = a.y + (hx > (b.x - a.x) ? hx - (b.x - a.x) : 0);
                    float x2h = a.x + (hx > span ? hx - span : 0);
                    float y2h = a.y + (hx > span ? span : hx);
                    dl->AddLine(ImVec2(x1h, y1h), ImVec2(x2h, y2h), IM_COL32(255, 255, 255, 22));
                }
                dl->AddRect(a, b, selected ? IM_COL32(255, 255, 255, 235)
                                           : IM_COL32(255, 255, 255, 70),
                            4.0f, 0, selected ? 2.0f : 1.0f);
                char ml[96];
                snprintf(ml, sizeof(ml), "%s  %.2fs", c.label.c_str(), c.duration);
                ImVec2 ms = ImGui::CalcTextSize(ml);
                if (b.x - a.x > ms.x + 10)
                    dl->AddText(ImVec2(a.x + 6, a.y + (b.y - a.y - ms.y) * 0.5f),
                                IM_COL32(210, 205, 205, 220), ml);
                // a thin leader back to the cut this shot was pulled off
                if (t.x >= trackX && t.x < a.x)
                    dl->AddLine(ImVec2(t.x, muteY + 1), ImVec2(a.x, muteY + 1),
                                IM_COL32(255, 255, 255, 45));
                if (inTracks && io.MousePos.x >= a.x && io.MousePos.x <= b.x &&
                    io.MousePos.y >= a.y && io.MousePos.y <= b.y) {
                    hotMute = t.idx;
                    ImGui::SetTooltip("%s — muted, double-click to bring it back",
                                      c.label.c_str());
                }
            }
        }

        if (g_clips.empty())
            dl->AddText(ImVec2(trackX + 10, clipY + clipH * 0.5f - 8),
                        IM_COL32(95, 95, 95, 255), "Drop footage here");
    }

    // ---- overlay video tracks
    for (auto& r : rows) {
        if (r.kind != 1) continue;
        VideoTrack& tr = *g_over[r.idx];
        for (int i = 0; i < (int)tr.clips.size(); i++) {
            Clip& c = *tr.clips[i];
            float x0 = SecToX(c.start), x1 = SecToX(c.start + c.duration);
            if (x1 < trackX || x0 > origin.x + avail.x) continue;
            float vx0 = x0 < trackX ? trackX : x0;
            if (c.kind == Clip::Video && c.vid) c.tex = ProxyFrame(*c.vid, c.trimIn);
            bool selected = SelHas(c.uid);
            ImU32 fill = tr.visible ? IM_COL32(58, 58, 58, 255) : IM_COL32(32, 32, 32, 255);
            dl->AddRectFilled(ImVec2(vx0 + 1, r.y0 + 2), ImVec2(x1 - 1, r.y1 - 2), fill, 5.0f);
            if (c.tex && x1 - vx0 > 10) {
                float bw = x1 - vx0 - 2, bh = r.y1 - r.y0 - 4;
                float boxA = bw / bh;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (c.texAspect > boxA) { float f = boxA / c.texAspect;
                                          uv0.x = 0.5f - f * 0.5f; uv1.x = 0.5f + f * 0.5f; }
                else                    { float f = c.texAspect / boxA;
                                          uv0.y = 0.5f - f * 0.5f; uv1.y = 0.5f + f * 0.5f; }
                dl->AddImageRounded((ImTextureID)c.tex, ImVec2(vx0 + 1, r.y0 + 2),
                                    ImVec2(x1 - 1, r.y1 - 2), uv0, uv1,
                                    IM_COL32(255, 255, 255, tr.visible ? 210 : 110), 5.0f);
            }
            dl->AddRect(ImVec2(vx0 + 1, r.y0 + 2), ImVec2(x1 - 1, r.y1 - 2),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 45),
                        5.0f, 0, selected ? 2.5f : 1.0f);
            drawClipWave(c, vx0 + 1, x1 - 1, r.y0 + 2, r.y1 - 2, c.trimIn);
            char lab[96];
            snprintf(lab, 96, "%s %.0f%%", LAYER_MODE_NAME(c.lblend), c.lopacity * 100.0f);
            if (x1 - vx0 > 90)
                dl->AddText(ImVec2(vx0 + 6, r.y1 - 20), IM_COL32(215, 215, 215, 200), lab);

            // Each edge grabs only on its own clip's side, so a seam between two
            // clips splits cleanly. Where clips touch or overlap, the selected clip
            // beats an unselected neighbour, and an edge beats a plain body hit.
            if (inTracks && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) {
                float mx = io.MousePos.x;
                int side = 2;                              // 2 = not under the pointer
                if (mx >= x0 && mx - x0 <= EDGE && mx < x1)      side = -1;
                else if (mx <= x1 && x1 - mx <= EDGE && mx > x0) side = +1;
                else if (mx > x0 && mx < x1)                     side = 0;
                if (side != 2) {
                    bool mine = SelHas(c.uid);
                    bool hotSel = hotLayer >= 0 && hotLayerTrack == r.idx &&
                                  SelHas(tr.clips[hotLayer]->uid);
                    if (hotLayer == -1 || (mine && !hotSel) ||
                        (mine == hotSel && side != 0 && hotLayerSide == 0)) {
                        hotLayerTrack = r.idx; hotLayer = i; hotLayerSide = side;
                    }
                }
            }
        }
        if (tr.clips.empty())
            dl->AddText(ImVec2(trackX + 10, r.y0 + (r.y1 - r.y0) * 0.5f - 8),
                        IM_COL32(85, 85, 85, 255), "");
    }

    // ---- audio tracks
    for (auto& r : rows) {
        if (r.kind != 2) continue;
        AudioTrack& tr = *g_atracks[r.idx];
        float rh = r.y1 - r.y0;
        for (int b = 0; b < (int)tr.blocks.size(); b++) {
            Song& s = *tr.blocks[b];
            if (!s.loaded) continue;
            double blockLen = s.trimEnd - s.trimStart;
            float ax0 = SecToX(s.offset), ax1 = SecToX(s.offset + blockLen);
            if (ax1 < trackX || ax0 > origin.x + avail.x) continue;
            bool selected = SelHas(s.uid);
            dl->AddRectFilled(ImVec2(ax0 < trackX ? trackX : ax0, r.y0 + 2),
                              ImVec2(ax1, r.y1 - 2),
                              tr.mute ? IM_COL32(26, 26, 26, 255)
                              : s.gen ? IM_COL32(38, 46, 56, 255)   // texture: no file under it
                                      : IM_COL32(42, 42, 42, 255), 5.0f);
            float cy = r.y0 + rh * 0.5f;
            float amp = (rh - 12) * 0.5f;
            float px0 = ax0 > trackX ? ax0 : trackX;
            float px1 = ax1 < origin.x + avail.x ? ax1 : origin.x + avail.x;
            size_t nPeaks = s.peaks.size() / 2;
            const float* pk = s.peaks.data();
            const double peakRate = (double)SAMPLE_RATE / s.framesPerPeak;
            const ImU32 wcol = tr.mute ? IM_COL32(105, 105, 105, 160)
                                       : IM_COL32(205, 205, 205, 210);
            const bool faded = s.fadeIn > 0.001f || s.fadeOut > 0.001f;
            for (float x = px0; x < px1; x += 1.0f) {
                double tl = XToSec(x);
                double tIn = tl - s.offset + s.trimStart;
                size_t p = (size_t)(tIn * peakRate);
                if (p >= nPeaks) break;
                float lo = pk[p * 2], hi = pk[p * 2 + 1];
                if (hi <= lo) continue;                  // silence: nothing to draw
                float g = s.volume * (faded ? SongFadeGain(s, tl) : 1.0f);
                lo = std::max(lo * g, -1.0f); hi = std::min(hi * g, 1.0f);
                dl->AddLine(ImVec2(x, cy - hi * amp), ImVec2(x, cy - lo * amp), wcol);
            }
            // Fades: a shaded wedge where the level is down, and its ramp drawn over it.
            if (faded && s.gen != 2) {
                const float by0 = r.y0 + 2, by1 = r.y1 - 2;
                const ImU32 shade = IM_COL32(0, 0, 0, 90);
                const ImU32 ramp = tr.mute ? IM_COL32(150, 150, 150, 160)
                                           : IM_COL32(255, 200, 90, 230);
                auto wedge = [&](double ta, double tb, bool in) {
                    float xa = SecToX(ta), xb = SecToX(tb);
                    if (xb - xa < 1.0f) return;
                    ImVec2 pa(xa, in ? by1 : by0), pb(xb, in ? by0 : by1);
                    ImVec2 top(in ? xa : xb, by0);       // the quiet corner above the ramp
                    dl->AddTriangleFilled(pa, top, pb, shade);
                    dl->AddLine(pa, pb, ramp, 1.5f);
                };
                double e = s.offset + blockLen;
                if (s.fadeIn > 0.001f) {
                    double a = s.offset > 0 ? s.offset : 0.0, b2 = s.offset + s.fadeIn;
                    if (b2 > a) wedge(a, b2, true);
                }
                if (s.fadeOut > 0.001f) {
                    double a = std::max(e - s.fadeOut, 0.0);
                    if (e > a) wedge(a, e, false);
                }
            }
            dl->AddRect(ImVec2(ax0 < trackX ? trackX : ax0, r.y0 + 2), ImVec2(ax1, r.y1 - 2),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 40),
                        5.0f, 0, selected ? 2.5f : 1.0f);
            const char* blabel = s.label.c_str();
            char glab[160];
            if (s.gen) {                          // what it is, what it runs, and over what
                const int f = std::clamp(s.fx, 0, AFX_COUNT - 1);
                if (s.gen == 1)
                    snprintf(glab, sizeof(glab), "noise bed - %s",
                             kTexNames[std::clamp(s.tex, 0, TEX_COUNT - 1)]);
                else
                    snprintf(glab, sizeof(glab), "chain region - %s > %s", kAfxNames[f],
                             s.target >= 0 && s.target < (int)g_atracks.size()
                                 ? g_atracks[s.target]->name.c_str() : "no track");
                blabel = glab;
            }
            dl->AddText(ImVec2((ax0 > trackX ? ax0 : trackX) + 6, r.y0 + 4),
                        IM_COL32(225, 225, 225, 220), blabel);

            // later blocks draw on top, so the last one under the mouse wins the hit
            if (inTracks && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1 &&
                io.MousePos.x >= ax0 - EDGE && io.MousePos.x <= ax1 + EDGE) {
                if (hotAudTrack != r.idx) audHits.clear();
                audHits.push_back(b);
            }
            if (inTracks && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) {
                if (fabsf(io.MousePos.x - ax0) <= EDGE) { hotAudTrack = r.idx; hotAudBlock = b; hotAudSide = -1; }
                else if (fabsf(io.MousePos.x - ax1) <= EDGE) { hotAudTrack = r.idx; hotAudBlock = b; hotAudSide = +1; }
                else if (io.MousePos.x > ax0 && io.MousePos.x < ax1) {
                    hotAudTrack = r.idx; hotAudBlock = b; hotAudSide = 0;
                }
            }
        }
        if (tr.blocks.empty())
            dl->AddText(ImVec2(trackX + 10, r.y0 + rh * 0.5f - 8), IM_COL32(85, 85, 85, 255),
                        "Drop audio here");
    }

    // ---- aspect track: each point holds until the next one
    for (auto& r : rows) {
        if (r.kind != 6) continue;
        for (int b = 0; b < (int)g_aspects.size(); b++) {
            AspectPoint& a = *g_aspects[b];
            float ax0 = SecToX(a.offset), ax1 = SecToX(AspectSpanEnd(b));
            if (ax1 < trackX || ax0 > origin.x + avail.x) continue;
            bool selected = SelHas(a.uid);
            float dx0 = ax0 < trackX ? trackX : ax0;
            dl->AddRectFilled(ImVec2(dx0, r.y0 + 2), ImVec2(ax1 - 1, r.y1 - 2),
                              IM_COL32(50, 60, 70, 255), 4.0f);
            dl->AddRect(ImVec2(dx0, r.y0 + 2), ImVec2(ax1 - 1, r.y1 - 2),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 40),
                        4.0f, 0, selected ? 2.0f : 1.0f);
            if (ax0 >= trackX)             // the point itself reads as a tick
                dl->AddRectFilled(ImVec2(ax0, r.y0 + 2), ImVec2(ax0 + 2, r.y1 - 2),
                                  IM_COL32(235, 205, 130, 255));
            char lab[48];
            if (a.aspect < 0.01f) snprintf(lab, sizeof(lab), "source");
            else                  snprintf(lab, sizeof(lab), "%.2f", a.aspect);
            if (ax1 - dx0 > 34)
                dl->AddText(ImVec2(dx0 + 6, r.y0 + 3), IM_COL32(225, 225, 225, 220), lab);
            // a point has no out edge to drag: the whole hold is the move handle
            if (inTracks && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1 &&
                hotAspect == -1 && io.MousePos.x > dx0 - EDGE && io.MousePos.x < ax1)
                hotAspect = b;
        }
        if (g_aspects.empty())
            dl->AddText(ImVec2(trackX + 10, r.y0 + 3), IM_COL32(85, 85, 85, 255),
                        "Double-click for an aspect point");
        bool overRow = inTracks && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1;
        if (overRow && hotAspect == -1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            AddAspectPoint(XToSec(io.MousePos.x));
            g_intakeStatus = "aspect point added";
        }
    }

    // Grouped shots get a bar across the top of the run, so a sequence reads as
    // one object even though it is still a row of separate shots.
    {
        std::vector<BaseSpan> lay0;
        BaseLayout(lay0);
        double acc = 0;
        int i = 0;
        while (i < (int)g_clips.size()) {
            int g = g_clips[i]->group;
            if (!g) { acc = lay0[i].end; i++; continue; }
            double runStart = lay0[i].start;
            int j = i;
            while (j < (int)g_clips.size() && g_clips[j]->group == g) {
                acc = lay0[j].end;
                j++;
            }
            float bx0 = SecToX(runStart), bx1 = SecToX(acc);
            if (bx1 > trackX && bx0 < origin.x + avail.x) {
                if (bx0 < trackX) bx0 = trackX;
                dl->AddRectFilled(ImVec2(bx0 + 1, clipY - 3), ImVec2(bx1 - 1, clipY),
                                  IM_COL32(255, 255, 255, 130), 1.0f);
                char gl[32];
                snprintf(gl, sizeof(gl), "seq %d", g);
                if (bx1 - bx0 > 60)
                    dl->AddText(ImVec2(bx0 + 5, clipY + clipH - 17),
                                IM_COL32(255, 255, 255, 130), gl);
            }
            i = j;
        }
    }

    // Hovering anywhere over the tracks shows the frame that lives at that spot:
    // the topmost visible layer covering it, otherwise the cut underneath.
    if (hovered && g_tl.drag == TimelineState::None && io.MousePos.x > trackX) {
        double ht = XToSec(io.MousePos.x);
        Clip* pick = nullptr;
        double local = 0;
        for (int t = (int)g_over.size() - 1; t >= 0 && !pick; t--) {
            if (!g_over[t]->visible) continue;
            for (auto& c : g_over[t]->clips)
                if (ht >= c->start && ht < c->start + c->duration) {
                    pick = c.get();
                    local = ht - c->start;
                    break;
                }
        }
        if (!pick && ht >= 0) {
            double acc = 0;
            for (auto& c : g_clips) {
                if (c->skip) continue;
                if (ht < acc + c->duration) { pick = c.get(); local = ht - acc; break; }
                acc += c->duration;
            }
        }
        if (pick) {
            ID3D11ShaderResourceView* frame = nullptr;
            float ar = 1.0f;
            if (pick->kind == Clip::Nest) {         // peek inside the sequence
                std::vector<NestHit> hits;
                NestResolve(*pick, local, hits);
                if (!hits.empty()) { local = hits[0].local; pick = hits[0].clip; }
            }
            if (pick->kind == Clip::Video && pick->vid) {
                double at = pick->reversed ? pick->duration - local : local;
                frame = ProxyFrame(*pick->vid, pick->trimIn + at);
                ar = pick->vid->aspect > 0 ? pick->vid->aspect : 1.0f;
            } else if (pick->kind == Clip::Image && pick->tex) {
                frame = pick->tex;
                ar = pick->texAspect;
            }
            ImGui::BeginTooltip();
            float w = 220.0f, h = w / (ar > 0.05f ? ar : 1.0f);
            if (h > 220.0f) { h = 220.0f; w = h * ar; }
            if (frame) ImGui::Image((ImTextureID)frame, ImVec2(w, h));
            else {
                ImGui::Dummy(ImVec2(w, 20));
                ImGui::TextDisabled("%s", pick->kind == Clip::Text ? "title card"
                                                                   : "no frame yet");
            }
            ImGui::Text("%02d:%02d+%02d", (int)ht / 60, (int)fmod(ht, 60.0),
                        (int)(fmod(ht, 1.0) * g_fps));
            ImGui::TextDisabled("%s  ·  %.2f s in", pick->label.c_str(), local);
            ImGui::EndTooltip();
        }
    }

    if (hotEdgeClip >= 0 || hotLayerSide != 0 || hotAudSide != 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hotEdgeClip >= 0 && g_tl.drag == TimelineState::None) {
        int nb = hotEdgeSide < 0 ? PrevVisibleClip(hotEdgeClip) : NextVisibleClip(hotEdgeClip);
        if (nb >= 0 && g_clips[nb]->kind != Clip::Nest)
            ImGui::SetTooltip("drag trims, ctrl+drag rolls the cut");
    }

    // Shift+wheel slips the picture inside a shot: the shot keeps its place on the
    // timeline and its length, the source slides under it. Over a selected shot it
    // slips the whole selection together — trim a run to one length, then dial in
    // what each of them shows. Alt makes it fine.
    if (slipWheel != 0.0f) {
        Clip* target = nullptr;
        if (hotBody >= 0)            target = g_clips[hotBody].get();
        else if (hotMute >= 0)       target = g_clips[hotMute].get();
        else if (hotLayer >= 0)      target = g_over[hotLayerTrack]->clips[hotLayer].get();
        else if (hotEdgeClip >= 0)   target = g_clips[hotEdgeClip].get();
        if (target) {
            double d = slipWheel * (io.KeyAlt ? 4.0 : 20.0) / g_tl.pps;
            int n = 0;
            auto slip = [&](Clip& c) {
                if (c.kind != Clip::Video || !c.vid || c.vid->duration <= 0) return;
                double lim = c.vid->duration - c.duration;
                if (lim < 0) lim = 0;
                double t = c.trimIn + d;
                c.trimIn = t < 0 ? 0 : (t > lim ? lim : t);
                n++;
            };
            slip(*target);
            if (SelHas(target->uid)) ForEachOtherSelected(*target, slip);
            if (n > 1) ImGui::SetTooltip("slip %+.2f s  ·  %d shots", d, n);
            else       ImGui::SetTooltip("slip %+.2f s  ·  in %.3f s", d, target->trimIn);
        } else {
            g_tl.scrollSec -= slipWheel * 60.0f / g_tl.pps;   // empty space: just pan
        }
    }

    // ---- interactions
    bool overRuler = hovered && io.MousePos.y >= rulerY && io.MousePos.y <= rulerY + RULER_H;
    int rangeEdge = 0;                     // -1 in, +1 out: the ruler grabs a range edge before it scrubs
    if (overRuler && HasRange()) {
        if (fabs(io.MousePos.x - SecToX(g_rangeOut)) <= 5) rangeEdge = 1;
        else if (fabs(io.MousePos.x - SecToX(g_rangeIn)) <= 5) rangeEdge = -1;
        if (rangeEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActivated()) {
        g_tl.dragStartMouseX = io.MousePos.x;
        g_tl.clickCollapseUid = -1;
        g_tl.clickCycleUid = -1;
        if (hotResizeKind >= 0) {
            g_tl.drag = TimelineState::RowResize;
            g_tl.rzKind = hotResizeKind;
            g_tl.dragTrack = hotResizeIdx;
            g_tl.rzStartY = io.MousePos.y;
            g_tl.rzStartH = hotResizeKind == 0 ? g_baseH
                          : hotResizeKind == 1 ? g_over[hotResizeIdx]->height
                                               : g_atracks[hotResizeIdx]->height;
        } else if (hotEdgeClip >= 0) {
            g_tl.drag = hotEdgeSide < 0 ? TimelineState::LeftEdge : TimelineState::RightEdge;
            {   // the shot's out-point as it stands now: the seam the ripple opens at
                std::vector<BaseSpan> lay0;
                BaseLayout(lay0);
                g_tl.rippleAt = lay0[hotEdgeClip].end;
            }
            g_tl.dragIndex = hotEdgeClip;
            g_tl.dragStartVal = g_clips[hotEdgeClip]->duration;
            g_tl.dragStartVal2 = g_clips[hotEdgeClip]->trimIn;
            g_tl.rollIndex = hotEdgeSide < 0 ? PrevVisibleClip(hotEdgeClip)
                                             : NextVisibleClip(hotEdgeClip);
            if (g_tl.rollIndex >= 0 && g_clips[g_tl.rollIndex]->kind == Clip::Nest)
                g_tl.rollIndex = -1;              // a folded run owns its own length
            if (g_tl.rollIndex >= 0) {
                g_tl.rollDur = g_clips[g_tl.rollIndex]->duration;
                g_tl.rollIn = g_clips[g_tl.rollIndex]->trimIn;
            }
        } else if (hotLayer >= 0) {
            Clip& c = *g_over[hotLayerTrack]->clips[hotLayer];
            if (io.KeyCtrl) SelToggle(c.uid);
            else if (io.KeyShift && g_selTrack == hotLayerTrack) {
                SelectRunBetween(g_over[hotLayerTrack]->clips, hotLayer, g_sel,
                                 [](const Clip& o) { return o.start; });
            } else if (!SelHas(c.uid)) SelSet(c.uid);
            // clicked one shot inside a multi-selection: the press still drags the
            // whole group, but a click that never moves drops down to this one.
            else if (g_selUids.size() > 1) g_tl.clickCollapseUid = c.uid;
            SelAddGroupOf(c);
            g_sel = hotLayer; g_selTrack = hotLayerTrack;
            g_tl.dragTrack = hotLayerTrack;
            g_tl.dragIndex = hotLayer;
            g_tl.dragStartVal = c.start;
            g_tl.dragStartVal2 = hotLayerSide > 0 ? c.duration : c.trimIn;
            g_tl.dragStartVal3 = c.duration;
            g_tl.drag = hotLayerSide < 0 ? TimelineState::LayerLeft
                      : hotLayerSide > 0 ? TimelineState::LayerRight
                                         : TimelineState::LayerMove;
            if (hotLayerSide == 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g_tl.drag = TimelineState::None;
                g_tl.editIndex = hotLayer;
                g_tl.editTrack = hotLayerTrack;
                g_tl.editValue = c.duration;
                // A card is a card wherever it sits: double-click edits its words,
                // the same as one on the base track.
                if (c.kind == Clip::Nest) g_navEnter = c.nest;
                else if (c.kind == Clip::Text) g_tl.editOpenText = true;
                else ImGui::OpenPopup("edit_duration");
            }
        } else if (hotAudBlock >= 0) {
            // Stacked blocks: while the picked one is under the mouse the press stays
            // on it (so a drag moves what you see picked), and a click that never
            // moves steps down to the next block in the stack, wrapping at the bottom.
            if (!io.KeyCtrl && !io.KeyShift && audHits.size() > 1 &&
                g_selTrack == -2 && g_selAT == hotAudTrack) {
                for (size_t k = 0; k < audHits.size(); k++) {
                    if (audHits[k] != g_sel) continue;
                    auto& bl = g_atracks[hotAudTrack]->blocks;
                    hotAudBlock = g_sel;
                    Song& cur = *bl[g_sel];
                    double len = cur.trimEnd - cur.trimStart;
                    float cx0 = SecToX(cur.offset), cx1 = SecToX(cur.offset + len);
                    hotAudSide = fabsf(io.MousePos.x - cx0) <= EDGE ? -1
                               : fabsf(io.MousePos.x - cx1) <= EDGE ? +1 : 0;
                    size_t next = k == 0 ? audHits.size() - 1 : k - 1;
                    g_tl.clickCycleUid = bl[audHits[next]]->uid;
                    break;
                }
            }
            Song& s = *g_atracks[hotAudTrack]->blocks[hotAudBlock];
            if (io.KeyCtrl) SelToggle(s.uid);
            else if (io.KeyShift && g_selTrack == -2 && g_sel >= 0 && g_selAT == hotAudTrack) {
                SelectRunBetween(g_atracks[hotAudTrack]->blocks, hotAudBlock, g_sel,
                                 [](const Song& o) { return o.offset; });
            } else if (!SelHas(s.uid)) SelSet(s.uid);
            else if (g_selUids.size() > 1) g_tl.clickCollapseUid = s.uid;
            SelAddGroupOf(s);
            g_sel = hotAudBlock; g_selTrack = -2; g_selAT = hotAudTrack;
            g_tl.dragTrack = hotAudTrack;
            g_tl.dragIndex = hotAudBlock;
            g_tl.dragStartVal = s.offset;
            g_tl.dragStartVal2 = hotAudSide > 0 ? s.trimEnd : s.trimStart;
            g_tl.drag = hotAudSide < 0 ? TimelineState::AudioLeft
                      : hotAudSide > 0 ? TimelineState::AudioRight
                                       : TimelineState::Audio;
        } else if (hotAspect >= 0) {
            AspectPoint& a = *g_aspects[hotAspect];
            SelSet(a.uid);
            g_sel = hotAspect; g_selTrack = -3;
            g_tl.dragIndex = hotAspect;
            g_tl.dragStartVal = a.offset;
            g_tl.drag = TimelineState::AspectMove;
        } else if (hotFade >= 0) {
            g_tl.drag = TimelineState::Fade;
            g_tl.dragIndex = hotFade;
            g_tl.dragStartMouseX = io.MousePos.x;
            g_tl.dragStartVal = g_clips[hotFade]->xfade;
            g_sel = hotFade; g_selTrack = -1;
        } else if (hotMute >= 0) {
            Clip& mc = *g_clips[hotMute];
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                mc.skip = false;
                SelSet(mc.uid);
                g_intakeStatus = mc.label + " back in the cut";
            } else if (io.KeyCtrl) SelToggle(mc.uid);
            else if (io.KeyShift && g_selTrack == -1 && g_sel >= 0) {
                int lo = hotMute < g_sel ? hotMute : g_sel;
                int hi = hotMute < g_sel ? g_sel : hotMute;
                for (int i = lo; i <= hi && i < (int)g_clips.size(); i++)
                    if (!SelHas(g_clips[i]->uid)) g_selUids.push_back(g_clips[i]->uid);
            } else if (!SelHas(mc.uid)) SelSet(mc.uid);
            else if (g_selUids.size() > 1) g_tl.clickCollapseUid = mc.uid;
            SelAddGroupOf(mc);
            g_sel = hotMute; g_selTrack = -1;
        } else if (hotBody >= 0 && IsGap(*g_clips[hotBody])) {
            g_tl.drag = TimelineState::Scrub;     // a gap is empty film: clicking it scrubs
        } else if (hotBody >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            g_tl.editIndex = hotBody;
            g_tl.editTrack = -1;
            g_tl.editValue = g_clips[hotBody]->duration;
            if (g_clips[hotBody]->kind == Clip::Nest) g_navEnter = g_clips[hotBody]->nest;
            else if (g_clips[hotBody]->kind == Clip::Text) g_tl.editOpenText = true;
            else ImGui::OpenPopup("edit_duration");
        } else if (hotBody >= 0) {
            Clip& hc = *g_clips[hotBody];
            if (io.KeyCtrl) SelToggle(hc.uid);
            else if (io.KeyShift && g_selTrack == -1 && g_sel >= 0) {
                int lo = hotBody < g_sel ? hotBody : g_sel;
                int hi = hotBody < g_sel ? g_sel : hotBody;
                for (int i = lo; i <= hi && i < (int)g_clips.size(); i++)
                    if (!SelHas(g_clips[i]->uid)) g_selUids.push_back(g_clips[i]->uid);
            } else if (!SelHas(hc.uid)) SelSet(hc.uid);
            else if (g_selUids.size() > 1) g_tl.clickCollapseUid = hc.uid;
            SelAddGroupOf(hc);
            g_sel = hotBody; g_selTrack = -1;
            g_tl.drag = TimelineState::Move;
            g_tl.dragIndex = hotBody;
        } else if (inTracks && io.KeyCtrl) {
            g_tl.drag = TimelineState::Marquee;   // ctrl-drag: box out a selection
            g_tl.boxFrom = io.MousePos;
            if (!io.KeyShift) g_selUids.clear();
            g_tl.boxKeep = g_selUids;             // shift keeps what was already picked
        } else if (rangeEdge != 0) {
            g_tl.drag = rangeEdge < 0 ? TimelineState::RangeIn : TimelineState::RangeOut;
        } else if (overRuler || inTracks) {
            g_tl.drag = TimelineState::Scrub;
        }
    }

    if (active && g_tl.drag != TimelineState::None) {
        double dSec = (double)(io.MousePos.x - g_tl.dragStartMouseX) / g_tl.pps;
        switch (g_tl.drag) {
        case TimelineState::RangeIn:
        case TimelineState::RangeOut: {
            double t = XToSec(io.MousePos.x);
            if (!io.KeyAlt) t = round(t * g_fps) / g_fps;           // land on a frame
            double lim = TimelineEnd();
            t = t < 0 ? 0 : (t > lim ? lim : t);
            bool in = g_tl.drag == TimelineState::RangeIn;
            if (in) g_rangeIn = fmax(0.0, fmin(t, g_rangeOut - MinClipDur()));
            else    g_rangeOut = fmax(t, g_rangeIn + MinClipDur());
            double at = in ? g_rangeIn : g_rangeOut;
            ImGui::SetTooltip("%s %d:%05.2f  ·  range %.2f s", in ? "in" : "out",
                              (int)at / 60, fmod(at, 60.0), g_rangeOut - g_rangeIn);
            break;
        }
        case TimelineState::RowResize: {
            float h = g_tl.rzStartH - (io.MousePos.y - g_tl.rzStartY);   // up = taller
            if (h < ROW_H_MIN) h = ROW_H_MIN;
            if (h > ROW_H_MAX) h = ROW_H_MAX;
            if (g_tl.rzKind == 0) g_baseH = h;
            else if (g_tl.rzKind == 1 && g_tl.dragTrack < (int)g_over.size())
                g_over[g_tl.dragTrack]->height = h;
            else if (g_tl.rzKind == 2 && g_tl.dragTrack < (int)g_atracks.size())
                g_atracks[g_tl.dragTrack]->height = h;
            ImGui::SetTooltip("%.0f px", h);
            break;
        }
        case TimelineState::RightEdge: {
            Clip& c = *g_clips[g_tl.dragIndex];
            if (io.KeyCtrl && g_tl.rollIndex >= 0) {
                // Move the cut: this shot grows by delta, the next one gives up the
                // same amount off its head. The seam is the only thing that moves.
                Clip& n = *g_clips[g_tl.rollIndex];
                double delta = SnapDuration(g_tl.dragStartVal + dSec) - g_tl.dragStartVal;
                double lo = MinClipDur() - g_tl.dragStartVal;          // this shot's floor
                double hi = g_tl.rollDur - MinClipDur();               // neighbour's floor
                if (c.kind == Clip::Video && c.vid && c.vid->duration > 0) {
                    double room = c.vid->duration - c.trimIn - g_tl.dragStartVal;
                    if (room < hi) hi = room;                          // source runs out
                }
                if (n.kind == Clip::Video) { double v = -g_tl.rollIn; if (v > lo) lo = v; }  // no source before 0
                if (hi < lo) hi = lo;
                delta = delta < lo ? lo : (delta > hi ? hi : delta);
                c.duration = g_tl.dragStartVal + delta;
                n.duration = g_tl.rollDur - delta;
                if (n.kind == Clip::Video) n.trimIn = g_tl.rollIn + delta;
                ImGui::SetTooltip("roll %+.3f s  ·  %.3f s | %.3f s",
                                  delta, c.duration, n.duration);
                g_tl.holdFrom = -1;                  // a roll moves nothing downstream
            } else {
                double d = SnapDuration(g_tl.dragStartVal + dSec);
                double mx = MaxDuration(c);
                c.duration = d > mx ? mx : d;
                // Everything after this shot stays put: only the edge moves.
                g_tl.holdFrom = g_tl.dragIndex + 1;
                g_tl.holdShift = g_tl.dragStartVal - c.duration;
                ImGui::SetTooltip("%.3f s", c.duration);
            }
            g_tl.previewEdge = +1;
            break;
        }
        case TimelineState::LeftEdge: {
            Clip& c = *g_clips[g_tl.dragIndex];
            // How far the head moved, snapped through the clip's own length so the
            // grid lands on the same values the right edge would give.
            double delta = g_tl.dragStartVal - SnapDuration(g_tl.dragStartVal - dSec);
            if (io.KeyCtrl && g_tl.rollIndex >= 0) {
                // Move the cut: this shot gives up delta off its head and the shot
                // before it grows by the same amount, so nothing downstream shifts.
                Clip& p = *g_clips[g_tl.rollIndex];
                double lo = MinClipDur() - g_tl.rollDur;               // previous shot's floor
                double hi = g_tl.dragStartVal - MinClipDur();          // this shot's floor
                if (c.kind == Clip::Video) { double v = -g_tl.dragStartVal2; if (v > lo) lo = v; }
                if (p.kind == Clip::Video && p.vid && p.vid->duration > 0) {
                    double room = p.vid->duration - g_tl.rollIn - g_tl.rollDur;
                    if (room < hi) hi = room;                          // source runs out
                }
                if (hi < lo) hi = lo;
                delta = delta < lo ? lo : (delta > hi ? hi : delta);
                if (c.kind == Clip::Video) c.trimIn = g_tl.dragStartVal2 + delta;
                c.duration = g_tl.dragStartVal - delta;
                p.duration = g_tl.rollDur + delta;
                ImGui::SetTooltip("roll %+.3f s  ·  %.3f s | %.3f s",
                                  delta, p.duration, c.duration);
                g_tl.holdFrom = -1;                  // a roll moves nothing downstream
            } else if (c.kind == Clip::Video) {   // move the in-point, keep the out-point
                double outPoint = g_tl.dragStartVal2 + g_tl.dragStartVal;
                double in = g_tl.dragStartVal2 + delta;
                if (in < 0) in = 0;
                if (in > outPoint - MinClipDur()) in = outPoint - MinClipDur();
                c.trimIn = in;
                c.duration = outPoint - in;
                // The shot itself slides right by what it lost, so its out-point and
                // every shot after it stay where they are and only the head moves.
                g_tl.holdFrom = g_tl.dragIndex;
                g_tl.holdShift = g_tl.dragStartVal - c.duration;
                ImGui::SetTooltip("in %.3f s  ·  %.3f s", c.trimIn, c.duration);
            } else {
                c.duration = SnapDuration(g_tl.dragStartVal - dSec);
                g_tl.holdFrom = g_tl.dragIndex;
                g_tl.holdShift = g_tl.dragStartVal - c.duration;
                ImGui::SetTooltip("%.3f s", c.duration);
            }
            g_tl.previewEdge = -1;
            break;
        }
        case TimelineState::Fade: {
            // Dragging left lengthens the dissolve: the seam is its right edge.
            int i = g_tl.dragIndex;
            if (i < 0 || i >= (int)g_clips.size()) break;
            double d = (g_tl.dragStartMouseX - io.MousePos.x) / g_tl.pps;
            double want = g_tl.dragStartVal + d;
            int nxt = -1;
            for (int j = i + 1; j < (int)g_clips.size(); j++)
                if (!g_clips[j]->skip) { nxt = j; break; }
            double lim = nxt >= 0 ? g_clips[nxt]->duration : g_clips[i]->duration;
            g_clips[i]->xfade = ClampFade(want, g_clips[i]->duration, lim);
            break;
        }
        case TimelineState::Move: {
            // Lifted off the base row: onto an existing layer, or onto the strip,
            // which spawns a track. The clip keeps the time it was dragged to.
            {
                int dest = -2;
                for (auto& r : rows)
                    if (r.kind == 1 && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) dest = r.idx;
                if (dest == -2 && inGhostV(io.MousePos)) dest = NewOverlayTrack();
                if (dest >= 0 && g_tl.dragIndex >= 0 && g_tl.dragIndex < (int)g_clips.size()) {
                    double s = 0;
                    { std::vector<BaseSpan> lay; BaseLayout(lay); s = lay[g_tl.dragIndex].start; }
                    auto mv = std::move(g_clips[g_tl.dragIndex]);
                    mv->start = s;
                    g_clips.erase(g_clips.begin() + g_tl.dragIndex);
                    // Ripple off: the shot leaves its time behind as empty film instead
                    // of the cut closing up over where it was.
                    if (!g_rippleOn) { InsertGap(g_tl.dragIndex, mv->duration); MergeGaps(); }
                    g_over[dest]->clips.push_back(std::move(mv));
                    g_tl.dragTrack = dest;
                    g_tl.dragIndex = (int)g_over[dest]->clips.size() - 1;
                    g_tl.dragStartVal = s;
                    g_tl.dragStartMouseX = io.MousePos.x;
                    g_sel = g_tl.dragIndex; g_selTrack = dest;
                    g_tl.drag = TimelineState::LayerMove;
                    break;
                }
            }
            // A click that wanders a pixel is still a click: nothing reorders until
            // the pointer has travelled DRAG_SLOP, and then only when it has cleared
            // the neighbour entirely. Comparing against the neighbour's midpoint made
            // a shot flip back and forth around a single pixel.
            const float DRAG_SLOP = 6.0f;
            if (fabsf(io.MousePos.x - g_tl.dragStartMouseX) < DRAG_SLOP) break;
            if (!g_rippleOn) {
                // Ripple off: the shot is not reordered, it is picked up. A ghost follows
                // the pointer in time and the drop overwrites whatever it lands on.
                int idx = g_tl.dragIndex;
                if (idx < 0 || idx >= (int)g_clips.size()) break;
                std::vector<BaseSpan> lay;
                BaseLayout(lay);
                double len = lay[idx].end - lay[idx].start;
                double at = snapPos(lay[idx].start + dSec, g_clips[idx]->uid);
                if (at < 0) at = 0;
                g_tl.overwriteAt = at;
                float gx0 = SecToX(at), gx1 = SecToX(at + len);
                dl->AddRectFilled(ImVec2(gx0 + 1, clipY + 2), ImVec2(gx1 - 1, clipY + clipH - 2),
                                  IM_COL32(255, 255, 255, 45), 5.0f);
                dl->AddRect(ImVec2(gx0 + 1, clipY + 2), ImVec2(gx1 - 1, clipY + clipH - 2),
                            IM_COL32(255, 255, 255, 220), 5.0f, 0, 2.0f);
                ImGui::SetTooltip("overwrite at %d:%05.2f  ·  %.2f s", (int)at / 60,
                                  fmod(at, 60.0), len);
                break;
            }
            {
                int idx = g_tl.dragIndex;
                if (idx < 0 || idx >= (int)g_clips.size()) break;
                double t = XToSec(io.MousePos.x);
                double s = 0;
                std::vector<BaseSpan> lay;
                BaseLayout(lay);
                double myStart = lay[idx].start, myEnd = lay[idx].end;
                int target = idx;
                if (t < myStart && idx > 0) {          // clear of the shot on the left
                    if (t < myStart - (lay[idx - 1].end - lay[idx - 1].start) * 0.55)
                        target = idx - 1;
                } else if (t > myEnd && idx + 1 < (int)g_clips.size()) {
                    if (t > myEnd + (lay[idx + 1].end - lay[idx + 1].start) * 0.55)
                        target = idx + 1;
                }
                if (target != idx) {
                    auto mv = std::move(g_clips[idx]);
                    g_clips.erase(g_clips.begin() + idx);
                    g_clips.insert(g_clips.begin() + target, std::move(mv));
                    if (g_selTrack == -1 && g_sel == idx) g_sel = target;
                    g_tl.dragIndex = target;
                    g_tl.dragStartMouseX = io.MousePos.x;   // re-arm the slop
                }
            }
            break;
        }
        case TimelineState::LayerMove: {
            if (g_tl.dragTrack < 0 || g_tl.dragTrack >= (int)g_over.size()) break;
            auto& src = g_over[g_tl.dragTrack]->clips;
            if (g_tl.dragIndex < 0 || g_tl.dragIndex >= (int)src.size()) break;
            Clip& c = *src[g_tl.dragIndex];
            if (fabsf(io.MousePos.x - g_tl.dragStartMouseX) < 3.0f) break;
            double before = c.start;
            double pos = snapPos(g_tl.dragStartVal + dSec, c.uid);
            c.start = pos < 0 ? 0 : pos;
            // everything else in the selection rides along
            double moved = c.start - before;
            if (moved != 0.0 && g_selUids.size() > 1) {
                for (auto& t : g_over)
                    for (auto& o : t->clips)
                        if (o->uid != c.uid && SelHas(o->uid)) {
                            o->start += moved;
                            if (o->start < 0) o->start = 0;
                        }
                for (auto& t : g_atracks)
                    for (auto& o : t->blocks)
                        if (SelHas(o->uid)) {
                            o->offset += moved;
                        }
            }
            // dragged onto the base row: fold it back into the cut at that point
            bool ontoBase = false;
            for (auto& r : rows)
                if (r.kind == 0 && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) ontoBase = true;
            g_tl.overwriteAt = -1;                // re-armed each frame it is over the base row
            if (ontoBase && !g_rippleOn) {
                // Ripple off: not folded in between shots (which pushes the film along) but
                // shown where it will cover the base track; the drop lays it down.
                g_tl.overwriteAt = c.start;
                g_tl.overwriteUid = c.uid;
                float gx0 = SecToX(c.start), gx1 = SecToX(c.start + c.duration);
                dl->AddRectFilled(ImVec2(gx0 + 1, clipY + 2), ImVec2(gx1 - 1, clipY + clipH - 2),
                                  IM_COL32(255, 255, 255, 45), 5.0f);
                dl->AddRect(ImVec2(gx0 + 1, clipY + 2), ImVec2(gx1 - 1, clipY + clipH - 2),
                            IM_COL32(255, 255, 255, 220), 5.0f, 0, 2.0f);
                ImGui::SetTooltip("overwrite at %d:%05.2f  ·  %.2f s", (int)c.start / 60,
                                  fmod(c.start, 60.0), c.duration);
                break;
            }
            if (ontoBase) {
                double t = XToSec(io.MousePos.x);
                double s = 0;
                int at = (int)g_clips.size();
                std::vector<BaseSpan> layD;
                BaseLayout(layD);
                for (int i = 0; i < (int)g_clips.size(); i++) {
                    double mid = (layD[i].start + layD[i].end) * 0.5;
                    if (t < mid) { at = i; break; }
                }
                auto mv = std::move(src[g_tl.dragIndex]);
                src.erase(src.begin() + g_tl.dragIndex);
                g_clips.insert(g_clips.begin() + at, std::move(mv));
                g_sel = at; g_selTrack = -1;
                g_tl.drag = TimelineState::Move;
                g_tl.dragIndex = at;
                g_tl.dragTrack = -1;
                break;
            }
            // onto another overlay row, or the strip, which spawns a track
            int dest = -2;
            for (auto& r : rows)
                if (r.kind == 1 && r.idx != g_tl.dragTrack &&
                    io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) dest = r.idx;
            if (dest == -2 && inGhostV(io.MousePos)) dest = NewOverlayTrack();
            if (dest >= 0) {
                auto mv = std::move(src[g_tl.dragIndex]);
                src.erase(src.begin() + g_tl.dragIndex);
                g_over[dest]->clips.push_back(std::move(mv));
                g_tl.dragTrack = dest;
                g_tl.dragIndex = (int)g_over[dest]->clips.size() - 1;
                g_sel = g_tl.dragIndex; g_selTrack = dest;
            }
            ImGui::SetTooltip("start %.3f s", pos < 0 ? 0 : pos);
            break;
        }
        case TimelineState::LayerRight: {
            auto& src = g_over[g_tl.dragTrack]->clips;
            if (g_tl.dragIndex >= (int)src.size()) break;
            Clip& c = *src[g_tl.dragIndex];
            double d = SnapDuration(g_tl.dragStartVal2 + dSec);
            double mx = MaxDuration(c);
            c.duration = d > mx ? mx : d;
            ImGui::SetTooltip("%.3f s", c.duration);
            break;
        }
        case TimelineState::LayerLeft: {
            auto& src = g_over[g_tl.dragTrack]->clips;
            if (g_tl.dragIndex >= (int)src.size()) break;
            Clip& c = *src[g_tl.dragIndex];
            double shift = dSec;
            double newStart = g_tl.dragStartVal + shift;
            if (newStart < 0) { shift -= newStart; newStart = 0; }
            double newDur = g_tl.dragStartVal3 - shift;
            if (newDur < MinClipDur()) newDur = MinClipDur();
            if (c.kind == Clip::Video) {     // eat into the source in-point too
                double in = g_tl.dragStartVal2 + shift;
                if (in < 0) { newStart -= in; newDur += in; in = 0; }
                c.trimIn = in;
            }
            c.start = newStart;
            c.duration = newDur;
            ImGui::SetTooltip("start %.3f s · %.3f s", c.start, c.duration);
            break;
        }
        case TimelineState::Audio:
        case TimelineState::AudioLeft:
        case TimelineState::AudioRight: {
            if (g_tl.dragTrack < 0 || g_tl.dragTrack >= (int)g_atracks.size()) break;
            auto& blocks = g_atracks[g_tl.dragTrack]->blocks;
            if (g_tl.dragIndex < 0 || g_tl.dragIndex >= (int)blocks.size()) break;
            Song& s = *blocks[g_tl.dragIndex];
            if (g_tl.drag == TimelineState::Audio) {
                double before = s.offset;
                s.offset = snapPos(g_tl.dragStartVal + dSec);
                double moved = s.offset - before;
                if (moved != 0.0 && g_selUids.size() > 1) {
                    for (auto& t : g_over)
                        for (auto& o : t->clips)
                            if (SelHas(o->uid)) {
                                o->start += moved;
                                if (o->start < 0) o->start = 0;
                            }
                    for (auto& t : g_atracks)
                        for (auto& o : t->blocks)
                            if (o->uid != s.uid && SelHas(o->uid)) {
                                o->offset += moved;
                            }
                }
                ImGui::SetTooltip("offset %+.3f s", s.offset);
                // onto another audio row, or the strip, which spawns a track
                int dest = -2;
                for (auto& r : rows)
                    if (r.kind == 2 && r.idx != g_tl.dragTrack &&
                        io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) dest = r.idx;
                if (dest == -2 && inGhostA(io.MousePos)) dest = NewAudioTrack();
                if (dest >= 0) {
                    MixGuard lock;
                    auto mv = std::move(blocks[g_tl.dragIndex]);
                    blocks.erase(blocks.begin() + g_tl.dragIndex);
                    g_atracks[dest]->blocks.push_back(std::move(mv));
                    g_tl.dragTrack = dest;
                    g_tl.dragIndex = (int)g_atracks[dest]->blocks.size() - 1;
                    g_sel = g_tl.dragIndex; g_selTrack = -2; g_selAT = dest;
                }
            } else if (g_tl.drag == TimelineState::AudioLeft) {
                // move the block's left edge: trims the head, keeps content in place
                double edge = snapPos(g_tl.dragStartVal + dSec);
                double newTrim = g_tl.dragStartVal2 + (edge - g_tl.dragStartVal);
                if (newTrim < 0) newTrim = 0;
                if (newTrim > s.trimEnd - 0.1) newTrim = s.trimEnd - 0.1;
                s.offset = g_tl.dragStartVal + (newTrim - g_tl.dragStartVal2);
                s.trimStart = newTrim;
                ImGui::SetTooltip("trim in %.3f s", newTrim);
            } else {
                double edge = snapPos(s.offset + (g_tl.dragStartVal2 - s.trimStart) + dSec);
                double newEnd = edge - s.offset + s.trimStart;
                if (newEnd > s.duration) newEnd = s.duration;
                if (newEnd < s.trimStart + 0.1) newEnd = s.trimStart + 0.1;
                s.trimEnd = newEnd;
                ImGui::SetTooltip("trim out %.3f s", newEnd);
            }
            break;
        }
        case TimelineState::AspectMove: {
            if (g_tl.dragIndex < 0 || g_tl.dragIndex >= (int)g_aspects.size()) break;
            AspectPoint& a = *g_aspects[g_tl.dragIndex];
            double at = snapPos(g_tl.dragStartVal + dSec, a.uid);
            a.offset = at < 0 ? 0 : at;
            ImGui::SetTooltip("aspect %.3f at %.3f s", a.aspect, a.offset);
            break;
        }
        case TimelineState::Marquee: {
            float minX = g_tl.boxFrom.x < io.MousePos.x ? g_tl.boxFrom.x : io.MousePos.x;
            float maxX = g_tl.boxFrom.x < io.MousePos.x ? io.MousePos.x : g_tl.boxFrom.x;
            float minY = g_tl.boxFrom.y < io.MousePos.y ? g_tl.boxFrom.y : io.MousePos.y;
            float maxY = g_tl.boxFrom.y < io.MousePos.y ? io.MousePos.y : g_tl.boxFrom.y;
            g_selUids = g_tl.boxKeep;
            auto touches = [&](float x0, float x1) { return maxX >= x0 && minX <= x1; };
            auto take = [&](int uid) { if (!SelHas(uid)) g_selUids.push_back(uid); };
            std::vector<BaseSpan> layM;
            BaseLayout(layM);
            for (auto& r : rows) {
                if (maxY < r.y0 || minY > r.y1) continue;
                if (r.kind == 0) {
                    for (int i = 0; i < (int)g_clips.size(); i++)
                        if (touches(SecToX(layM[i].start), SecToX(layM[i].end)))
                            take(g_clips[i]->uid);
                } else if (r.kind == 5) {          // the mute siding
                    for (int i = 0; i < (int)g_clips.size(); i++)
                        if (g_clips[i]->skip) take(g_clips[i]->uid);
                } else if (r.kind == 1 && r.idx >= 0 && r.idx < (int)g_over.size()) {
                    for (auto& c : g_over[r.idx]->clips)
                        if (touches(SecToX(c->start), SecToX(c->start + c->duration)))
                            take(c->uid);
                } else if (r.kind == 2 && r.idx >= 0 && r.idx < (int)g_atracks.size()) {
                    for (auto& b : g_atracks[r.idx]->blocks)
                        if (touches(SecToX(b->offset),
                                    SecToX(b->offset + (b->trimEnd - b->trimStart))))
                            take(b->uid);
                } else if (r.kind == 6) {
                    for (int i = 0; i < (int)g_aspects.size(); i++)
                        if (touches(SecToX(g_aspects[i]->offset), SecToX(AspectSpanEnd(i))))
                            take(g_aspects[i]->uid);
                }
            }
            dl->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY),
                              IM_COL32(120, 170, 255, 45));
            dl->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY), IM_COL32(150, 200, 255, 200));
            break;
        }
        case TimelineState::Scrub: {
            double t = XToSec(io.MousePos.x);
            if (t < 0) t = 0;
            double lim = TimelineEnd();
            if (t > lim) t = lim;
            g_playhead.store(t);
            break;
        }
        default: break;
        }
        // Live preview: the playhead follows the edge under the cursor, so the
        // viewer shows the frame the trim is landing on. The in-point shows the
        // first frame that survives, the out-point the last one.
        if (g_tl.previewEdge != 0 && !g_playing.load() &&
            g_tl.dragIndex >= 0 && g_tl.dragIndex < (int)g_clips.size()) {
            std::vector<BaseSpan> now;
            BaseLayout(now);
            const BaseSpan& sp = now[g_tl.dragIndex];
            double t = g_tl.previewEdge < 0 ? sp.start : sp.end - 1.0 / g_fps;
            if (t < 0) t = 0;
            g_playhead.store(t);
        }
        g_tl.previewEdge = 0;
    }
    if (ImGui::IsItemDeactivated()) {
        // A press inside a multi-selection that never turned into a drag was a
        // plain pick: keep only that shot (and its group).
        if (g_tl.clickCycleUid >= 0 &&
            fabsf(io.MousePos.x - g_tl.dragStartMouseX) < 4.0f) {
            // a still click on a stacked block: hand the pick to the one beneath
            if (Song* ns = SongByUid(g_tl.clickCycleUid)) {
                SelSet(ns->uid);
                SelAddGroupOf(*ns);
                SelPrimaryTo(ns->uid);
            }
        } else if (g_tl.clickCollapseUid >= 0 &&
            fabsf(io.MousePos.x - g_tl.dragStartMouseX) < 4.0f) {
            Clip* pc = ClipByUid(g_tl.clickCollapseUid);
            SelSet(g_tl.clickCollapseUid);
            if (pc) SelAddGroupOf(*pc);
        } else if (g_tl.drag == TimelineState::Audio && fabsf(io.MousePos.x - g_tl.dragStartMouseX) >= 4.0f) {
            double t = XToSec(io.MousePos.x);
            Clip* dropNest = nullptr;
            double nestStart = 0;
            
            for (auto& r : rows) {
                if (r.kind == 0 && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) {
                    std::vector<BaseSpan> layD;
                    BaseLayout(layD);
                    for (int i = 0; i < (int)g_clips.size(); i++) {
                        if (t >= layD[i].start && t < layD[i].end) {
                            if (g_clips[i]->kind == Clip::Nest) {
                                dropNest = g_clips[i].get();
                                nestStart = layD[i].start;
                            }
                            break;
                        }
                    }
                } else if (r.kind == 1 && io.MousePos.y >= r.y0 && io.MousePos.y <= r.y1) {
                    int dest = r.idx;
                    for (auto& c : g_over[dest]->clips) {
                        if (t >= c->start && t < c->start + c->duration) {
                            if (c->kind == Clip::Nest) {
                                dropNest = c.get();
                                nestStart = c->start;
                            }
                            break;
                        }
                    }
                }
            }
            
            if (dropNest) {
                Sequence* q = FindSeq(dropNest->nest);
                if (q) {
                    MixGuard lock;
                    int movedCount = 0;
                    for (int tr = 0; tr < (int)g_atracks.size(); tr++) {
                        for (int i = (int)g_atracks[tr]->blocks.size() - 1; i >= 0; i--) {
                            if (SelHas(g_atracks[tr]->blocks[i]->uid)) {
                                auto s = std::move(g_atracks[tr]->blocks[i]);
                                g_atracks[tr]->blocks.erase(g_atracks[tr]->blocks.begin() + i);
                                s->offset -= nestStart;
                                while (q->atracks.size() <= tr) {
                                    auto trNew = std::make_unique<AudioTrack>();
                                    trNew->name = "sound " + std::to_string(q->atracks.size() + 1);
                                    q->atracks.push_back(std::move(trNew));
                                }
                                q->atracks[tr]->blocks.push_back(std::move(s));
                                movedCount++;
                            }
                        }
                    }
                    if (movedCount > 0) {
                        g_selUids.clear();
                        g_sel = -1;
                        g_selTrack = -1;
                        g_selAT = -1;
                        
                        char buf[64];
                        snprintf(buf, sizeof(buf), "moved %d item%s into sequence", movedCount, movedCount == 1 ? "" : "s");
                        g_intakeStatus = buf;
                    }
                }
            }
        }
        // The ripple lands here: base shots downstream close up on their own, so the
        // layers and the sound over them move by the same amount.
        if ((g_tl.drag == TimelineState::LeftEdge || g_tl.drag == TimelineState::RightEdge) &&
            g_tl.holdFrom >= 0 && g_tl.rippleAt > -1e17) {
            if (g_rippleOn) {
                RippleOthers(g_tl.rippleAt, -g_tl.holdShift);
            } else {
                // Ripple off: the time the shot gave up stays on the film as a gap, on
                // the side that was trimmed. Growing a shot overwrites its neighbours -
                // gaps and shots alike - and only what cannot be taken (a sequence, the
                // start of the film) still pushes.
                bool head = g_tl.drag == TimelineState::LeftEdge;
                int i = g_tl.dragIndex;
                double shift = g_tl.holdShift;             // > 0: the shot got shorter
                if (shift > 0) {
                    InsertGap(head ? i : i + 1, shift);
                } else if (shift < 0) {
                    double left = OverwriteNeighbours(head ? i - 1 : i + 1, -shift, !head);
                    if (left > 1e-9) RippleOthers(g_tl.rippleAt, left);
                }
                MergeGaps();
            }
        }
        // Ripple off, a base shot let go after a move: lay it down where the ghost was.
        if (g_tl.drag == TimelineState::Move && !g_rippleOn && g_tl.overwriteAt >= 0 &&
            fabsf(io.MousePos.x - g_tl.dragStartMouseX) >= 4.0f) {
            int ni = PlaceOverwrite(g_tl.dragIndex, g_tl.overwriteAt);
            if (ni >= 0) { g_sel = ni; g_selTrack = -1; }
        }
        // Ripple off, a layer clip let go over the base track: it leaves its layer and
        // covers the base track at the time it sits at. Checked by uid, in case the
        // drop was already taken by something else (a sequence under the pointer).
        if (g_tl.drag == TimelineState::LayerMove && !g_rippleOn && g_tl.overwriteAt >= 0 &&
            g_tl.dragTrack >= 0 && g_tl.dragTrack < (int)g_over.size()) {
            auto& src = g_over[g_tl.dragTrack]->clips;
            if (g_tl.dragIndex >= 0 && g_tl.dragIndex < (int)src.size() &&
                src[g_tl.dragIndex]->uid == g_tl.overwriteUid) {
                auto mv = std::move(src[g_tl.dragIndex]);
                src.erase(src.begin() + g_tl.dragIndex);
                int ni = LayDownOverwrite(std::move(mv), g_tl.overwriteAt);
                if (ni >= 0) { g_sel = ni; g_selTrack = -1; }
            }
        }
        g_tl.overwriteAt = -1;
        g_tl.overwriteUid = -1;
        g_tl.rippleAt = -1e18;
        g_tl.clickCollapseUid = -1;
        g_tl.clickCycleUid = -1;
        g_tl.drag = TimelineState::None;
        g_tl.dragIndex = -1;
        g_tl.dragTrack = -1;
        g_tl.rollIndex = -1;
        g_tl.previewEdge = 0;
        g_tl.holdFrom = -1;
        g_tl.holdShift = 0;
    }

    // mute: the film runs straight past these shots without losing them
    if (g_tl.drag == TimelineState::None && !io.WantTextInput &&
        !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M, false))
        g_muteToggle = true;

    // hush: kill clip sound in the preview only, so the cut can be watched silent
    if (g_tl.drag == TimelineState::None && !io.WantTextInput &&
        io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M, false))
        g_hushClips.store(!g_hushClips.load());

    // export range: i / o mark it at the playhead, alt+x forgets it. Marking one end
    // alone fills in the other from the film's own start or end.
    if (g_tl.drag == TimelineState::None && !io.WantTextInput && !io.KeyCtrl) {
        double ph = round(g_playhead.load() * g_fps) / g_fps;
        if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
            g_rangeIn = ph;
            if (g_rangeOut <= ph + MinClipDur()) g_rangeOut = TimelineEnd();
        }
        if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            g_rangeOut = ph;
            if (g_rangeIn < 0 || g_rangeIn >= ph - MinClipDur()) g_rangeIn = 0;
        }
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X, false)) g_rangeIn = g_rangeOut = -1;
    }

    // trim the whole selection to the length of the shot the panel is on
    if (g_tl.drag == TimelineState::None && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        Clip* pc = SelectedClip();
        if (pc && pc->kind != Clip::Nest) {
            int n = TrimSelectionTo(pc->duration);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d shot%s trimmed to %.2f s", n,
                     n == 1 ? "" : "s", pc->duration);
            g_intakeStatus = buf;
        }
    }

    // Split at the playhead. A selection says what to cut: a layer clip or a sound
    // block is cut on its own row, and only with nothing picked does the blade fall
    // on the base cut.
    if (g_tl.drag == TimelineState::None && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        double ph = g_playhead.load();
        const double MIN = MinClipDur();
        bool did = false;
        if (g_selTrack >= 0 && g_selTrack < (int)g_over.size() &&
            g_sel >= 0 && g_sel < (int)g_over[g_selTrack]->clips.size()) {
            auto& v = g_over[g_selTrack]->clips;
            Clip& c = *v[g_sel];
            double off = ph - c.start;
            if (off > MIN && off < c.duration - MIN) {
                SplitClipIn(v, g_sel, off);
                SelSet(v[g_sel]->uid);
                did = true;
                g_intakeStatus = "layer clip split";
            }
        } else if (g_selTrack == -2 && g_selAT >= 0 && g_selAT < (int)g_atracks.size() &&
                   g_sel >= 0 && g_sel < (int)g_atracks[g_selAT]->blocks.size()) {
            auto& v = g_atracks[g_selAT]->blocks;
            Song& b = *v[g_sel];
            double off = ph - b.offset;                  // seconds into the block
            double len = b.trimEnd - b.trimStart;
            if (off > MIN && off < len - MIN) {
                auto t = std::make_unique<Song>(b);      // same audio, second half
                t->uid = g_uidNext++;
                t->offset = b.offset + off;
                t->trimStart = b.trimStart + off;
                t->fadeIn = 0;                           // fades stay at the outer ends
                b.trimEnd = b.trimStart + off;
                b.fadeOut = 0;
                MixGuard lock;
                v.insert(v.begin() + g_sel + 1, std::move(t));
                did = true;
                g_intakeStatus = "sound block split";
            }
        } else if (g_selTrack == -1 && g_sel >= 0 && g_sel < (int)g_clips.size()) {
            std::vector<BaseSpan> layS;
            BaseLayout(layS);
            Clip& c = *g_clips[g_sel];
            double off = ph - layS[g_sel].start;
            if (!c.skip && off > MIN && off < c.duration - MIN) {
                if (c.kind == Clip::Nest) did = SplitNest(g_sel, off);
                else { SplitClip(g_sel, off); did = true; g_intakeStatus = "shot split"; }
            }
        }
        if (!did && !g_clips.empty()) SplitPoint(ph);
    }

    // delete whatever the cursor is over
    if (g_tl.drag == TimelineState::None && g_selTrack == -3 &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        if (g_sel >= 0 && g_sel < (int)g_aspects.size()) {
            g_aspects.erase(g_aspects.begin() + g_sel);
            g_intakeStatus = "aspect point removed";
        }
        g_sel = -1; g_selTrack = -1;
        g_selUids.clear();
    } else if (g_tl.drag == TimelineState::None &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        // What's highlighted is what goes. Only with nothing picked does the clip
        // under the cursor go instead - a hovered neighbour (or the block stacked
        // over a picked one) must never be deleted in place of the selection.
        if (!g_selUids.empty()) {
            {   // removing a base shot closes the film up, so whatever sat over it
                // moves back by the same length. Back to front, so each seam is
                // still measured in the world the one before it left behind.
                std::vector<BaseSpan> lay0;
                BaseLayout(lay0);
                for (int i = (int)g_clips.size() - 1; i >= 0; i--) {
                    if (!SelHas(g_clips[i]->uid)) continue;
                    // Ripple off leaves the shot's time behind as a gap (the gap is a
                    // new clip, unselected, so the sweep below keeps it). Deleting a gap
                    // itself is how you close one, so that still ripples.
                    if (g_rippleOn || IsGap(*g_clips[i]))
                        RippleOthers(lay0[i].end, -TimeLen(*g_clips[i]));
                    else
                        InsertGap(i + 1, TimeLen(*g_clips[i]));
                }
            }
            auto sweep = [&](std::vector<std::unique_ptr<Clip>>& v) {
                for (int i = (int)v.size() - 1; i >= 0; i--) {
                    if (!SelHas(v[i]->uid)) continue;
                    if (v[i]->kind != Clip::Video) RetireTexture(v[i]->tex);
                    ClearDoubleExposure(*v[i]);
                    v.erase(v.begin() + i);
                }
            };
            sweep(g_clips);
            for (auto& t : g_over) sweep(t->clips);
            
            auto sweepS = [&](std::vector<std::unique_ptr<Song>>& v) {
                for (int i = (int)v.size() - 1; i >= 0; i--) {
                    if (SelHas(v[i]->uid)) v.erase(v.begin() + i);
                }
            };
            MixGuard lock;
            for (auto& t : g_atracks) sweepS(t->blocks);

            g_selUids.clear();
            g_sel = -1;
        } else if (hotBody >= 0) {
            if (g_rippleOn || IsGap(*g_clips[hotBody])) {
                std::vector<BaseSpan> lay0;
                BaseLayout(lay0);
                RippleOthers(lay0[hotBody].end, -TimeLen(*g_clips[hotBody]));
            } else {
                InsertGap(hotBody + 1, TimeLen(*g_clips[hotBody]));   // lands after, index stays
            }
            if (g_clips[hotBody]->tex && g_clips[hotBody]->kind != Clip::Video)
                RetireTexture(g_clips[hotBody]->tex);
            ClearDoubleExposure(*g_clips[hotBody]);
            g_clips.erase(g_clips.begin() + hotBody);
            if (g_selTrack == -1) {
                if (g_sel == hotBody) g_sel = -1;
                else if (g_sel > hotBody) g_sel--;
            }
        } else if (hotLayer >= 0) {
            auto& v = g_over[hotLayerTrack]->clips;
            if (v[hotLayer]->kind != Clip::Video) RetireTexture(v[hotLayer]->tex);
            ClearDoubleExposure(*v[hotLayer]);
            v.erase(v.begin() + hotLayer);
            if (g_selTrack == hotLayerTrack && g_sel >= hotLayer) g_sel = -1;
        } else if (hotAudBlock >= 0) {
            MixGuard lock;                 // the mixer must not be inside this list
            auto& b = g_atracks[hotAudTrack]->blocks;
            b.erase(b.begin() + hotAudBlock);
            if (g_selTrack == -2 && g_selAT == hotAudTrack) g_sel = -1;
        }
    }

    // exact-duration popup (base clips and layer clips)
    if (ImGui::BeginPopup("edit_duration")) {
        Clip* ec = nullptr;
        if (g_tl.editTrack == -1) {
            if (g_tl.editIndex >= 0 && g_tl.editIndex < (int)g_clips.size())
                ec = g_clips[g_tl.editIndex].get();
        } else if (g_tl.editTrack >= 0 && g_tl.editTrack < (int)g_over.size()) {
            auto& v = g_over[g_tl.editTrack]->clips;
            if (g_tl.editIndex >= 0 && g_tl.editIndex < (int)v.size())
                ec = v[g_tl.editIndex].get();
        }
        if (ec && ec->kind == Clip::Video) {
            ImGui::Text("in %.3f s  ·  source %.2f s", ec->trimIn,
                        ec->vid ? ec->vid->duration : 0.0);
            ImGui::BeginDisabled(!ec->vid || !ec->vid->hasAudio);
            ImGui::Checkbox("keep this clip's audio", &ec->useAudio);
            ImGui::EndDisabled();
            if (ec->vid && !ec->vid->hasAudio) ImGui::TextDisabled("(no audio track)");
            ImGui::Separator();
        }
        ImGui::TextUnformatted("Duration (seconds)");
        ImGui::SetNextItemWidth(140);
        if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0)) ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputDouble("##dur", &g_tl.editValue, 0, 0, "%.3f",
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        if (commit && ec) {
            double v = g_tl.editValue < MinClipDur() ? MinClipDur() : g_tl.editValue;
            double mx = MaxDuration(*ec);
            ec->duration = v > mx ? mx : v;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The text-card editor is opened by the root window (see DrawApp) so that the
    // toolbar button and this double-click share one popup id.

    // ---- export range: dim what it leaves out, band the ruler over what it keeps
    if (HasRange()) {
        float bottom = rows.empty() ? origin.y + needH : rows.back().y1;
        float right = origin.x + avail.x;
        float xi = SecToX(g_rangeIn), xo = SecToX(g_rangeOut);
        float ci = xi < trackX ? trackX : (xi > right ? right : xi);
        float co = xo < trackX ? trackX : (xo > right ? right : xo);
        const ImU32 dim = IM_COL32(0, 0, 0, 90);
        if (ci > trackX) dl->AddRectFilled(ImVec2(trackX, rulerY + RULER_H), ImVec2(ci, bottom), dim);
        if (co < right)  dl->AddRectFilled(ImVec2(co, rulerY + RULER_H), ImVec2(right, bottom), dim);
        if (co > ci)
            dl->AddRectFilled(ImVec2(ci, rulerY), ImVec2(co, rulerY + RULER_H),
                              IM_COL32(230, 180, 90, 60));
        const ImU32 edge = IM_COL32(230, 180, 90, 220);
        if (xi >= trackX && xi <= right) dl->AddLine(ImVec2(xi, rulerY), ImVec2(xi, bottom), edge);
        if (xo >= trackX && xo <= right) dl->AddLine(ImVec2(xo, rulerY), ImVec2(xo, bottom), edge);
    }

    // ---- playhead
    double ph = g_playhead.load();
    double phHold = 0.0;
    if (g_tl.holdFrom >= 0 && g_tl.holdFrom < (int)g_clips.size()) {
        std::vector<BaseSpan> phLay;
        BaseLayout(phLay);
        if (ph >= phLay[g_tl.holdFrom].start) phHold = g_tl.holdShift;
    }
    float phx = SecToX(ph + phHold);
    if (phx >= trackX && phx <= origin.x + avail.x) {
        float bottom = rows.empty() ? origin.y + needH : rows.back().y1;
        if (g_tl.snapAt > -1e17) {          // where the drag just locked on
            float sx = SecToX(g_tl.snapAt);
            if (sx >= trackX && sx <= origin.x + avail.x) {
                for (float yy = rulerY + RULER_H; yy < bottom; yy += 7)
                    dl->AddLine(ImVec2(sx, yy), ImVec2(sx, yy + 4),
                                IM_COL32(255, 255, 255, 120));
                if (g_tl.snapWhat)
                    dl->AddText(ImVec2(sx + 4, rulerY + RULER_H + 2),
                                IM_COL32(255, 255, 255, 150), g_tl.snapWhat);
            }
        }
        dl->AddLine(ImVec2(phx, rulerY), ImVec2(phx, bottom), IM_COL32(255, 255, 255, 235), 1.5f);
        dl->AddTriangleFilled(ImVec2(phx - 6, rulerY), ImVec2(phx + 6, rulerY),
                              ImVec2(phx, rulerY + 8), IM_COL32(255, 255, 255, 235));
    }
    // keep playhead in view while playing
    if (g_playing.load()) {
        float viewW = trackW / g_tl.pps;
        if (ph > g_tl.scrollSec + viewW * 0.95 || ph < g_tl.scrollSec)
            g_tl.scrollSec = (float)ph - viewW * 0.1f;
    }
}


// ------------------------------------------------------- export settings UI

// Everything that shapes the encode lives here so the toolbar stays one row.
// Everything the encoder is told, laid out flat in the side panel — no dialog to
// open, no choice hidden behind a drop-down.
static void DrawOutputPanel() {
    SegRow("canvas", &g_preset,
           "TikTok\0" "Reels\0" "IG 4:5\0" "IG 1:1\0" "YouTube\0" "YT 4K\0"
           "Original\0" "Custom\0");
    if (g_preset == PRESET_CUSTOM) {
        Prop("pixels");
        float half = ColW(2);
        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##cw", &g_customW, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##ch", &g_customH, 0, 0);
        g_customW = g_customW < 16 ? 16 : (g_customW > 7680 ? 7680 : g_customW);
        g_customH = g_customH < 16 ? 16 : (g_customH > 7680 ? 7680 : g_customH);
    }
    ImGui::BeginDisabled(g_preset == PRESET_ORIGINAL);
    // Each clip carries its own off-canvas fill; this row stamps one onto the lot,
    // which is what the old global setting effectively did.
    if (SegRow("off-canvas", &g_fit, LFIT_ITEMS)) {
        auto stamp = [](std::vector<std::unique_ptr<Clip>>& v, int f) {
            for (auto& c : v) c->lfit = f;
        };
        stamp(g_clips, g_fit);
        for (auto& t : g_over) stamp(t->clips, g_fit);
        for (auto& q : g_seqs) {
            stamp(q->clips, g_fit);
            for (auto& t : q->over) stamp(t->clips, g_fit);
        }
    }
    ImGui::EndDisabled();

    static const int FPS_CHOICES[] = { 24, 25, 30, 50, 60 };
    int fpsIdx = 2;
    for (int i = 0; i < 5; i++) if (FPS_CHOICES[i] == g_fps) fpsIdx = i;
    if (SegRow("fps", &fpsIdx, "24\0" "25\0" "30\0" "50\0" "60\0")) {
        g_fps = FPS_CHOICES[fpsIdx];
        g_fpsAuto = false;                 // an explicit choice wins over incoming footage
    }

    SegRow("codec", &g_vcodec, "H.264\0" "H.265\0" "H.264 GPU\0" "H.265 GPU\0");
    SegRow("speed", &g_speed, "veryslow\0" "slower\0" "slow\0" "medium\0" "fast\0");
    SegRow("rate", &g_rateMode, "Quality\0" "Bitrate\0");
    if (g_rateMode == RM_CRF) {
        Prop("crf");
        ImGui::SliderInt("##crf", &g_crf, 10, 30, "%d");
    } else {
        static const float MBPS_PICKS[] = { 5.0f, 10.0f, 20.0f, 50.0f };
        int pick = -1;
        for (int i = 0; i < 4; i++) if (fabsf(g_targetMbps - MBPS_PICKS[i]) < 0.05f) pick = i;
        if (SegRow("bitrate", &pick, "5\0" "10\0" "20\0" "50\0") && pick >= 0)
            g_targetMbps = MBPS_PICKS[pick];
        Prop("Mbps");
        ImGui::SliderFloat("##mbps", &g_targetMbps, 2.0f, 120.0f, "%.1f");
    }
    SegRow("container", &g_container, "MP4\0" "MOV\0" "MKV\0");
    SegRow("aac", &g_abrIdx, "128k\0" "192k\0" "256k\0" "320k\0");

    Prop("flags");
    ImGui::BeginDisabled(g_container == CT_MKV);
    ImGui::Checkbox("fast start", &g_faststart);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("-14 LUFS", &g_loudnorm);

    Prop("grain");                         // its own row: a third flag overflows a narrow panel
    ImGui::BeginDisabled(IsNvenc(g_vcodec));
    ImGui::Checkbox("keep##grain", &g_keepGrain);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(IsNvenc(g_vcodec)
            ? "H.264 / H.265 only - NVENC has no grain mode"
            : "tell x264 / x265 to keep noise and grain instead of smoothing it,\n"
              "and in Quality mode drop the bitrate ceiling so it can afford to\n"
              "(much bigger files - fine for Vimeo, check before other uploads)");

    Prop("fade in");
    ImGui::SliderFloat("##fin", &g_fadeIn, 0.0f, 3.0f, "%.2f s");
    Prop("fade out");
    ImGui::SliderFloat("##fout", &g_fadeOut, 0.0f, 3.0f, "%.2f s");

    static char prefix[128] = "";
    static bool prefixSynced = false;
    if (!prefixSynced) { snprintf(prefix, sizeof(prefix), "%s", Narrow(g_namePrefix).c_str());
                         prefixSynced = true; }
    Prop("name");
    if (ImGui::InputTextWithHint("##prefix", "first clip's name", prefix, sizeof(prefix)))
        g_namePrefix = Widen(prefix);
}

// ------------------------------------------------------ projector shader (GPU)
//
// The same look projector_render.py renders offline, ported to HLSL and run live
// on the preview. The python script's two passes only ever sample each other at
// the same pixel, so they collapse into one pixel shader here.

static ID3D11VertexShader*  g_fsVS = nullptr;      // fullscreen triangle
static ID3D11VertexShader*  g_quadVS = nullptr;    // composite quad
static ID3D11PixelShader*   g_projPS = nullptr;    // plate + gate + grain
static ID3D11PixelShader*   g_quadPS = nullptr;    // plain textured quad, for compositing
static ID3D11PixelShader*   g_blendPS = nullptr;   // one layer over the canvas
static ID3D11Buffer*        g_cb = nullptr;
static ID3D11SamplerState*  g_sampLinear = nullptr;
static ID3D11BlendState*    g_blendAlpha = nullptr;
static ID3D11RasterizerState* g_rsNone = nullptr;
static ID3D11DepthStencilState* g_dsOff = nullptr;

// One constant buffer serves both shaders; unused fields are ignored.
struct ProjCB {
    float outSize[2];      float plateOrg[2];
    float plateSize[2];    float srcScale[2];
    float srcOffset[2];    float time;        float intensity;
    float grain;           float grainFps;    float ap[2];
    float wall[3];         float gateOn;
    float quadRect[4];     // x0,y0,x1,y1 in NDC (quad pass)
    float quadUV[4];       // u0,v0,u1,v1
    float tint[4];
    float blendMode;       // index into LAYER_MODES_W
    float blendOpacity;
    float pad0[2];
    float fxWeave;         float fxRipple;   float fxAber;    float fxHalation;
    float fxFlicker;       float fxDust;     float fxHair;    float fxScratch;
    float fxVignette;      float pad1[3];
    float gBright;         float gContrastM1; float gSatM1;  float gTemp;
    float lookMode;        float lookPillar;  float lookPad[2];
};
static_assert(sizeof(ProjCB) % 16 == 0, "cbuffer must be 16-byte aligned");

static const char* PROJ_HLSL = R"HLSL(
cbuffer CB : register(b0) {
    float2 outSize;   float2 plateOrg;
    float2 plateSize; float2 srcScale;
    float2 srcOffset; float  time;      float intensity;
    float  grain;     float  grainFps;  float2 ap;
    float3 wall;      float  gateOn;
    float4 quadRect;
    float4 quadUV;
    float4 tint;
    float  blendMode;
    float  blendOpacity;
    float2 pad0;
    float  fxWeave;   float fxRipple; float fxAber;  float fxHalation;
    float  fxFlicker; float fxDust;   float fxHair;  float fxScratch;
    float  fxVignette; float3 pad1;
    float  gBright;    float gContrastM1;  float gSatM1;  float gTemp;
    float  lookMode;   float lookPillar;   float2 lookPad;
};
Texture2D    tex0 : register(t0);      // source / top layer
Texture2D    tex1 : register(t1);      // bottom layer, blend pass only
SamplerState samp : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Fullscreen triangle: uv has y down, matching the python port's a_tex_coord.
VSOut VSFull(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(p, 0, 1);
    o.uv = float2((p.x + 1) * 0.5, 1 - (p.y + 1) * 0.5);
    return o;
}

// Quad from constants, for compositing clips into the offscreen canvas.
VSOut VSQuad(uint id : SV_VertexID) {
    VSOut o;
    float2 c = float2((id == 1 || id == 3) ? quadRect.z : quadRect.x,
                      (id >= 2)            ? quadRect.w : quadRect.y);
    float2 uv = float2((id == 1 || id == 3) ? quadUV.z : quadUV.x,
                       (id >= 2)            ? quadUV.w : quadUV.y);
    o.pos = float4(c, 0, 1);
    o.uv = uv;
    return o;
}

// Offsets, so an all-zero constant buffer is the identity grade.
float3 ApplyGrade(float3 c) {
    c += gBright;
    c = (c - 0.5) * (1.0 + gContrastM1) + 0.5;
    c.r *= 1.0 + 0.25 * gTemp;
    c.b *= 1.0 - 0.25 * gTemp;
    float l = dot(saturate(c), float3(0.299, 0.587, 0.114));
    c = lerp(l.xxx, c, 1.0 + gSatM1);
    return saturate(c);
}

float4 PSQuad(VSOut i) : SV_Target {
    float4 s = tex0.Sample(samp, i.uv) * tint;
    s.rgb = ApplyGrade(s.rgb);
    return s;
}

// ffmpeg's blend modes, verified against vf_blend output. A is the TOP layer,
// B the bottom one, and all_opacity mixes the result back toward B.
float3 BlendModes(int m, float3 A, float3 B) {
    if (m == 1) return 1.0 - (1.0 - A) * (1.0 - B);                 // screen
    if (m == 2) return max(A, B);                                   // lighten
    if (m == 3) return lerp(1.0 - 2.0 * (1.0 - A) * (1.0 - B),      // overlay
                            2.0 * A * B, step(A, 0.5));
    if (m == 4) return A * B;                                       // multiply
    if (m == 5) return (1.0 - 2.0 * B) * A * A + 2.0 * A * B;       // soft light
    if (m == 6) return abs(A - B);                                  // difference
    if (m == 7) return saturate(A + B);                             // addition
    if (m == 8) return min(A, B);                                   // darken
    return A;                                                       // normal
}

float4 PSBlend(VSOut i) : SV_Target {
    float4 T = tex0.Sample(samp, i.uv);
    float3 B = tex1.Sample(samp, i.uv).rgb;
    float3 r = BlendModes((int)(blendMode + 0.5), saturate(T.rgb), saturate(B));
    return float4(lerp(B, r, saturate(blendOpacity) * T.a), 1.0);
}

float hash1(float n) { return frac(sin(n) * 43758.5453); }
float hash2(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}
// PCG3D integer hash, bit-identical to projector_render.py. The float hash this
// replaced ran out of precision on pixel coordinates and left a faint grid, which
// shows on dark footage.
uint3 pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float hash13(float3 p) {
    return float(pcg3d(uint3(int3(p))).x) / 4294967295.0;
}
float4 sampleSrc(float2 uv) {
    return tex0.Sample(samp, clamp(uv, 0.0, 1.0) * srcScale + srcOffset);
}

)HLSL"
#include "../surveillance_shader.h"
R"HLSL(

float4 PSProjector(VSOut input) : SV_Target {
    // A tube is not a projector: no plate, no gate, no wall behind it.
    if (lookMode >= 1.0) return Surveillance(input.uv);
    float2 tc = input.uv;                       // y down
    float2 px = tc * outSize;
    float2 uv0 = (px - plateOrg) / plateSize;

    float frame = floor(time * 18.0);           // film runs ~18 fps
    float dip = step(0.985, hash1(frame * 7.1 + 2.0));
    float flicker = 0.992 + 0.008 * hash1(frame * 12.9898)
                  + 0.002 * sin(time * 0.7) * sin(time * 1.7);
    flicker -= 0.005 * dip;
    flicker = lerp(1.0, flicker, fxFlicker);

    float3 col = wall;
    float  a = 0.0;
    float3 rgb = 0;

    if (uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0) {
        float2 frameSize = plateSize;
        float aspect = frameSize.x / max(frameSize.y, 1.0);

        // --- gate weave -------------------------------------------------
        float2 uv = uv0;
        uv += float2(sin(time * 3.0) * 0.00004,
                     (sin(time * 5.0) * 0.5 + sin(time * 2.1)) * 0.00008) * fxWeave;
        float2 jitter = float2(hash1(frame * 4.17 + 1.3), hash1(frame * 6.91 + 8.7)) - 0.5;
        uv += jitter * float2(0.00065, 0.00050) * fxWeave;
        uv.y += dip * (hash1(frame * 3.9) - 0.5) * 0.00025 * fxWeave;

        // --- curtain ripple ---------------------------------------------
        float2 cuv = uv;
        float rippleScaleX = 1.0 / max(aspect, 1.0);
        cuv.x += (sin(uv.x * 3.0 + time * 0.65) * 0.00035
                + sin(uv.x * 1.6 - time * 0.38) * 0.00045) * rippleScaleX * fxRipple;
        cuv.y += sin(uv.x * 2.2 + time * 0.50) * 0.00015 * fxRipple;

        float4 t = sampleSrc(cuv);
        float3 clean = t.rgb;
        a = t.a;

        // --- lens chromatic aberration ----------------------------------
        float2 rc  = (cuv - 0.5) * float2(aspect, 1.0);
        float2 cav = (cuv - 0.5) * dot(rc, rc) * 0.0035 * fxAber;
        // Lateral aberration is a spread of wavelengths, not one shifted copy, so each
        // fringe is smeared along its own offset. A single-tap shift is a hard coloured
        // edge on a sharp source (the full-res export) and only looked soft on a soft
        // one (the preview proxy). Same mean offset, so the amount is unchanged.
        t.r = (sampleSrc(cuv - cav * 0.5).r + sampleSrc(cuv - cav).r + sampleSrc(cuv - cav * 1.5).r) / 3.0;
        t.b = (sampleSrc(cuv + cav * 0.5).b + sampleSrc(cuv + cav).b + sampleSrc(cuv + cav * 1.5).b) / 3.0;

        // --- soft ring blur: halation + focus breathing ------------------
        float3 blur = t.rgb;
        [unroll] for (int i = 0; i < 6; i++) {
            float ang = float(i) * 1.0471976;
            float2 o = float2(cos(ang), sin(ang)) * 4.5 / frameSize;
            blur += sampleSrc(cuv + o).rgb;
        }
        blur /= 7.0;

        float breath = 0.5 + 0.5 * sin(time * 0.31 + sin(time * 0.127) * 2.0);
        t.rgb = lerp(t.rgb, blur, (0.05 + 0.10 * breath + 0.05 * dip) * fxHalation);

        float lum = dot(blur, float3(0.299, 0.587, 0.114));
        t.rgb += blur * float3(1.06, 1.0, 0.90) * smoothstep(0.55, 0.95, lum)
               * 0.18 * fxHalation;

        // --- luminance lift + lamp flicker -------------------------------
        t.rgb = t.rgb * 1.12 + 0.035;
        t.rgb *= flicker;

        // --- gate dust ---------------------------------------------------
        float2 duv  = float2(cuv.x * aspect, cuv.y);
        float2 cell = floor(duv * 14.0);
        float seed = hash2(cell + float2(frame * 0.613, frame * 0.269));
        if (seed > 0.955 && fxDust > 0.0) {
            float2 sp = float2(hash2(cell + float2(frame * 0.83, 1.7)),
                               hash2(cell + float2(2.9, frame * 0.51)));
            float rr = 0.04 + 0.10 * hash2(cell + float2(frame * 0.37, 5.3));
            float speck = 1.0 - smoothstep(rr * 0.1, rr, length(frac(duv * 14.0) - sp));
            t.rgb *= 1.0 - speck * 0.05 * fxDust;
        }

        // --- a hair in the gate ------------------------------------------
        float hseed = hash1(frame * 4.451);
        if (hseed > 0.90 && fxHair > 0.0) {
            float side  = step(0.5, hash1(frame * 7.9));
            float reach = 0.12 + 0.30 * hash1(frame * 2.63);
            float depth = lerp(cuv.y, 1.0 - cuv.y, side);
            float wob = sin(cuv.y * 17.0 + frame * 1.3) * 0.006
                      + sin(cuv.y * 41.0 + frame) * 0.002;
            float hd = abs(cuv.x - (0.08 + 0.84 * hash1(frame * 9.77)) - wob);
            float hair = (1.0 - smoothstep(0.0006, 0.0022, hd))
                       * (1.0 - smoothstep(reach * 0.6, reach, depth));
            t.rgb *= 1.0 - hair * 0.55 * fxHair;
        }

        // --- emulsion scratch --------------------------------------------
        float bwin = floor(time * 0.37);
        if (hash1(bwin * 17.3) > 0.72 && fxScratch > 0.0) {
            float sx = 0.1 + 0.8 * hash1(bwin * 5.1) + (hash1(frame * 3.17) - 0.5) * 0.006;
            float scr = 1.0 - smoothstep(0.0004, 0.0014, abs(uv.x - sx));
            t.rgb = lerp(t.rgb, t.rgb * 0.7 + 0.12,
                         scr * 0.5 * (0.4 + 0.6 * hash1(frame * 8.13)) * fxScratch);
        }

        // --- light vignette ------------------------------------------------
        float2 vc = abs(cuv - 0.5) * 2.0;
        float vigDist = pow(vc.x, 2.5) + pow(vc.y, 2.5);
        float vig = saturate(1.0 - vigDist * 0.7);
        vig = vig * vig;
        t.rgb *= lerp(lerp(1.0, 0.50, fxVignette), 1.0, vig);

        t.rgb = lerp(clean, t.rgb, intensity);
        a *= lerp(1.0, 0.9, intensity);
        rgb = t.rgb * a;                        // premultiplied, like the python pass 1
    }

    // ---- the ragged gate, over the wall
    float inside = 1.0;
    if (gateOn > 0.5) {
        float2 p = (tc - 0.5) * outSize;
        float2 halfSz = ap;
        float baseR = 12.0;
        float soft  = 2.0;
        float2 sgn = step(0.0, p);
        float cid = sgn.x * 2.0 + sgn.y;
        float rad = baseR * (0.9 + 0.2 * frac(sin(cid * 91.7 + 3.1) * 43758.5453));
        float2 q = abs(p) - (halfSz - 2.5 - soft - rad);
        float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;

        float2 np = p / max(halfSz, float2(1.0, 1.0));
        float ragged = sin(np.x * 5.0 + 1.7) + sin(np.y * 6.0 + 0.5)
                     + 0.4 * sin(np.x * 11.0 - 0.3) + 0.4 * sin(np.y * 9.0 + 2.1);
        dist += ragged * 0.6 + (hash1(frame * 5.71) - 0.5) * 0.55;
        inside = 1.0 - smoothstep(-soft, soft, dist);
    }

    float av = a * inside;
    col = rgb * inside + wall * (1.0 - av);

    // ---- full-frame grain
    if (grain > 0.0) {
        float2 gpx = floor(tc * outSize);
        // rounded: time is frame / fps, and floor of that times fps lands one short
        // on some frames, which then repeat the previous frame's grain
        float stepT = floor(time * grainFps + 0.5);
        float g = hash13(float3(gpx, stepT)) - 0.5;
        float ga = abs(g) * grain;
        float3 gc = (g > 0.0) ? float3(1, 1, 1) : float3(0, 0, 0);
        col = col * (1.0 - ga) + gc * ga;
    }
    return float4(saturate(col), 1.0);
}
)HLSL";

// 0/1 ping-pong for the composite, 2 stages one layer, 3 holds the film pass.
// A nested sequence needs a canvas of its own, so every depth below the root gets
// its own pair and scratch from 4 up: TargetBase(d) + 0/1 are its ping-pong, +2 its
// element stage.
static const int PROJ_MAX_DEPTH = 3;                  // root plus three nest levels
static const int PROJ_TARGETS = 4 + 3 * PROJ_MAX_DEPTH;
static ID3D11Texture2D*          g_projTex[PROJ_TARGETS] = {};
static ID3D11RenderTargetView*   g_projRTV[PROJ_TARGETS] = {};
static ID3D11ShaderResourceView* g_projSRV[PROJ_TARGETS] = {};
static int  g_projRtN = 0;                            // how many are actually allocated
static int  TargetBase(int depth) { return depth == 0 ? 0 : 4 + 3 * (depth - 1); }
static int  g_projRtW = 0, g_projRtH = 0;
static bool g_gpuReady = false;
static bool g_gpuFailed = false;
static std::string g_gpuError;

static bool CompileOne(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(PROJ_HLSL, strlen(PROJ_HLSL), "projector.hlsl", nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        g_gpuError = err ? std::string((const char*)err->GetBufferPointer()) : "compile failed";
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

static bool InitProjectorGPU() {
    if (g_gpuReady) return true;
    if (g_gpuFailed) return false;
    ID3DBlob *vs = nullptr, *vsq = nullptr, *ps = nullptr, *psq = nullptr, *psb = nullptr;
    if (!CompileOne("VSFull", "vs_4_0", &vs) || !CompileOne("VSQuad", "vs_4_0", &vsq) ||
        !CompileOne("PSProjector", "ps_4_0", &ps) || !CompileOne("PSQuad", "ps_4_0", &psq) ||
        !CompileOne("PSBlend", "ps_4_0", &psb)) {
        g_gpuFailed = true;
        return false;
    }
    g_d3dDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_fsVS);
    static ID3D11VertexShader* quadVS = nullptr;
    g_d3dDevice->CreateVertexShader(vsq->GetBufferPointer(), vsq->GetBufferSize(), nullptr, &quadVS);
    g_d3dDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_projPS);
    g_d3dDevice->CreatePixelShader(psq->GetBufferPointer(), psq->GetBufferSize(), nullptr, &g_quadPS);
    g_d3dDevice->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_blendPS);
    vs->Release(); vsq->Release(); ps->Release(); psq->Release(); psb->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ProjCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_d3dDevice->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_d3dDevice->CreateSamplerState(&sd, &g_sampLinear);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_d3dDevice->CreateBlendState(&bl, &g_blendAlpha);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_d3dDevice->CreateRasterizerState(&rd, &g_rsNone);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    g_d3dDevice->CreateDepthStencilState(&dsd, &g_dsOff);

    g_quadVS = quadVS;
    g_gpuReady = g_fsVS && g_projPS && g_quadPS && g_blendPS && g_cb && g_sampLinear;
    if (!g_gpuReady) { g_gpuFailed = true; g_gpuError = "could not create shader objects"; }
    return g_gpuReady;
}

static bool EnsureProjectorTargets(int w, int h, int count) {
    if (count < 4) count = 4;
    if (count > PROJ_TARGETS) count = PROJ_TARGETS;
    if (w == g_projRtW && h == g_projRtH && count <= g_projRtN) return true;
    if (w == g_projRtW && h == g_projRtH) {           // same canvas, just deeper
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = g_projRtN; i < count; i++) {
            if (FAILED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_projTex[i]))) return false;
            g_d3dDevice->CreateRenderTargetView(g_projTex[i], nullptr, &g_projRTV[i]);
            g_d3dDevice->CreateShaderResourceView(g_projTex[i], nullptr, &g_projSRV[i]);
        }
        g_projRtN = count;
        return true;
    }
    for (int i = 0; i < PROJ_TARGETS; i++) {
        if (g_projSRV[i]) { g_projSRV[i]->Release(); g_projSRV[i] = nullptr; }
        if (g_projRTV[i]) { g_projRTV[i]->Release(); g_projRTV[i] = nullptr; }
        if (g_projTex[i]) { g_projTex[i]->Release(); g_projTex[i] = nullptr; }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < count; i++) {
        if (FAILED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_projTex[i]))) return false;
        g_d3dDevice->CreateRenderTargetView(g_projTex[i], nullptr, &g_projRTV[i]);
        g_d3dDevice->CreateShaderResourceView(g_projTex[i], nullptr, &g_projSRV[i]);
    }
    g_projRtW = w; g_projRtH = h; g_projRtN = count;
    return true;
}

static void SetCB(const ProjCB& cb) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_d3dContext->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, &cb, sizeof(cb));
        g_d3dContext->Unmap(g_cb, 0);
    }
}

// Draw one texture into the offscreen canvas, cover- or contain-fitted.
static void CompositeQuad(ID3D11ShaderResourceView* srv, float aspect, bool cover,
                          float alpha, int w, int h, const Grade* g = nullptr,
                          int anchor = LANCHOR_CENTER, float rx = 1.0f, float ry = 1.0f) {
    if (!srv) return;
    ProjCB cb = {};
    if (g) {
        cb.gBright = g->bright;
        cb.gContrastM1 = g->contrast - 1.0f;
        cb.gSatM1 = (g->mono ? 0.0f : g->sat) - 1.0f;
        cb.gTemp = g->temp;
    }
    // rx/ry: the box is only that centred part of the target (see PlateCropRegion)
    float boxA = ((float)w * rx) / ((float)h * ry);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float x0 = -rx, y0 = -ry, x1 = rx, y1 = ry;     // NDC, y up
    if (cover) {
        float ax, ay;
        AnchorFrac(anchor, &ax, &ay);
        if (aspect > boxA) { float f = boxA / aspect; u0 = (1.0f - f) * ax; u1 = u0 + f; }
        else               { float f = aspect / boxA; v0 = (1.0f - f) * ay; v1 = v0 + f; }
    } else {
        float fw = 2.0f * rx, fh = 2.0f * ry;
        if (aspect > boxA) fh *= boxA / aspect;
        else               fw *= aspect / boxA;
        x0 = -fw * 0.5f; x1 = fw * 0.5f;
        y0 = -fh * 0.5f; y1 = fh * 0.5f;
    }
    cb.quadRect[0] = x0; cb.quadRect[1] = y1;   // top-left
    cb.quadRect[2] = x1; cb.quadRect[3] = y0;   // bottom-right
    cb.quadUV[0] = u0; cb.quadUV[1] = v0;
    cb.quadUV[2] = u1; cb.quadUV[3] = v1;
    cb.tint[0] = cb.tint[1] = cb.tint[2] = 1.0f;
    cb.tint[3] = alpha;
    SetCB(cb);
    g_d3dContext->VSSetShader(g_quadVS, nullptr, 0);
    g_d3dContext->PSSetShader(g_quadPS, nullptr, 0);
    g_d3dContext->PSSetShaderResources(0, 1, &srv);
    g_d3dContext->Draw(4, 0);
}

// Composite the frame at the playhead into an offscreen canvas, then run the
// projector shader over it. Returns the finished texture, or null if unavailable.
static ID3D11ShaderResourceView* RenderProjectorGPU(int outW, int outH, double t,
                                                   bool applyFilm) {
    if (!InitProjectorGPU()) return nullptr;
    if (outW < 16) outW = 16;
    if (outH < 16) outH = 16;
    // Only pay for the canvases this frame needs: most films never nest at all.
    std::function<int(Clip&, double)> nestDepth = [&](Clip& c, double local) -> int {
        if (c.kind != Clip::Nest) return 0;
        std::vector<NestHit> kids;
        NestChildren(c, local, kids);
        int deepest = 0;
        for (auto& k : kids) {
            int d = nestDepth(*k.clip, k.local);
            if (d > deepest) deepest = d;
        }
        return 1 + deepest;
    };
    int wantDepth = 0;
    {
        double at = g_playhead.load();
        double cs = 0;
        int i = ClipAt(at, &cs);
        if (i >= 0 && !g_baseOff) wantDepth = nestDepth(*g_clips[i], at - cs);
        for (auto& tr : g_over) {
            if (!tr->visible) continue;
            for (auto& c : tr->clips) {
                if (c->skip || at < c->start || at >= c->start + c->duration) continue;
                int d = nestDepth(*c, at - c->start);
                if (d > wantDepth) wantDepth = d;
            }
        }
        if (wantDepth > PROJ_MAX_DEPTH) wantDepth = PROJ_MAX_DEPTH;
    }
    if (!EnsureProjectorTargets(outW, outH, 4 + 3 * wantDepth)) return nullptr;

    ID3D11RenderTargetView* oldRTV = nullptr;
    ID3D11DepthStencilView* oldDSV = nullptr;
    g_d3dContext->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    D3D11_VIEWPORT vp = { 0, 0, (float)outW, (float)outH, 0, 1 };
    const float clear[4] = { 0, 0, 0, 0 };
    const float blend[4] = { 0, 0, 0, 0 };
    g_d3dContext->OMSetRenderTargets(1, &g_projRTV[0], nullptr);
    g_d3dContext->ClearRenderTargetView(g_projRTV[0], clear);
    g_d3dContext->RSSetViewports(1, &vp);
    g_d3dContext->RSSetState(g_rsNone);
    g_d3dContext->OMSetDepthStencilState(g_dsOff, 0);
    g_d3dContext->OMSetBlendState(g_blendAlpha, blend, 0xffffffff);
    g_d3dContext->IASetInputLayout(nullptr);
    g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_d3dContext->VSSetConstantBuffers(0, 1, &g_cb);
    g_d3dContext->PSSetConstantBuffers(0, 1, &g_cb);
    g_d3dContext->PSSetSamplers(0, 1, &g_sampLinear);
    g_d3dContext->GSSetShader(nullptr, nullptr, 0);

    double ph = g_playhead.load();
    double clipStart = 0;
    int ci = ClipAt(ph, &clipStart);

    // The canvas ping-pongs between targets 0 and 1: each element is drawn into
    // target 2 on its own, then a blend pass mixes it into the canvas with the
    // same maths ffmpeg's blend filter uses, so the preview matches the export.
    int curAt[PROJ_MAX_DEPTH + 1] = {};
    auto blendElement = [&](int depth, ID3D11ShaderResourceView* srv, float ar, int mode,
                            float opacity, const Grade* grade, bool fitCover,
                            int bed = LFIT_INSIDE, int look = LOOK_PROJECTOR,
                            bool pillar = false, double effectTime = 0.0,
                            int anchor = LANCHOR_CENTER, bool throughPlate = false) {
        if (!srv || opacity <= 0.001f) return;
        const int T = TargetBase(depth);
        const int stage = T + 2;
        // 1. the element alone, fitted, graded, alpha 1 where it covers
        g_d3dContext->OMSetRenderTargets(1, &g_projRTV[stage], nullptr);
        const float bedCol[4] = { 0, 0, 0, 1 };
        g_d3dContext->ClearRenderTargetView(g_projRTV[stage],
                                            bed == LFIT_BLACK ? bedCol : clear);
        g_d3dContext->OMSetBlendState(nullptr, blend, 0xffffffff);
        g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        // The blurred bed is a cover-fitted copy of the layer under the whole frame.
        // The preview does not blur it - the export's gblur is the real thing - but it
        // is opaque and the right colours, so the cut underneath stays hidden here too.
        // Through the projector the plate's part of the canvas is the frame for every
        // fit, the bed included. A black bed can stay full canvas: past the plate's
        // part nothing reaches the gate.
        float rx = 1.0f, ry = 1.0f;
        if (throughPlate && applyFilm)
            PlateCropRegion((float)outW / (float)outH, &rx, &ry);
        if (bed == LFIT_BLUR)
            CompositeQuad(srv, ar, true, 1.0f, outW, outH, grade, anchor, rx, ry);
        CompositeQuad(srv, ar, fitCover, 1.0f, outW, outH, grade, anchor, rx, ry);

        // 1b. a shot on its own screen runs the look here, on the element alone, so the
        // treatment lands before track blending rather than over the finished frame.
        ID3D11ShaderResourceView* element = g_projSRV[stage];
        if (look != LOOK_PROJECTOR) {
            // No plate, no gate, no wall: a set needs only the frame, the clock and
            // the strength.
            ProjCB fx = {};
            fx.outSize[0] = (float)outW;
            fx.outSize[1] = (float)outH;
            fx.time = (float)(effectTime + g_projTimeOffset);
            fx.intensity = g_projIntensity < 0 ? 0 : (g_projIntensity > 1 ? 1 : g_projIntensity);
            fx.lookMode = (float)look;
            fx.lookPillar = pillar ? 1.0f : 0.0f;
            SetCB(fx);
            ID3D11ShaderResourceView* empty[2] = { nullptr, nullptr };
            g_d3dContext->PSSetShaderResources(0, 2, empty);
            g_d3dContext->OMSetRenderTargets(1, &g_projRTV[3], nullptr);
            g_d3dContext->ClearRenderTargetView(g_projRTV[3], clear);
            g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_d3dContext->VSSetShader(g_fsVS, nullptr, 0);
            g_d3dContext->PSSetShader(g_projPS, nullptr, 0);
            g_d3dContext->PSSetShaderResources(0, 1, &element);
            g_d3dContext->Draw(3, 0);
            g_d3dContext->PSSetShaderResources(0, 2, empty);
            element = g_projSRV[3];
        }

        // 2. blend it over the canvas into the other target
        int dst = curAt[depth] ^ 1;
        ProjCB cb = {};
        cb.blendMode = (float)mode;
        cb.blendOpacity = opacity;
        SetCB(cb);
        ID3D11ShaderResourceView* none2[2] = { nullptr, nullptr };
        g_d3dContext->PSSetShaderResources(0, 2, none2);
        g_d3dContext->OMSetRenderTargets(1, &g_projRTV[T + dst], nullptr);
        ID3D11ShaderResourceView* srvs[2] = { element, g_projSRV[T + curAt[depth]] };
        g_d3dContext->PSSetShaderResources(0, 2, srvs);
        g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_d3dContext->VSSetShader(g_fsVS, nullptr, 0);
        g_d3dContext->PSSetShader(g_blendPS, nullptr, 0);
        g_d3dContext->Draw(3, 0);
        g_d3dContext->PSSetShaderResources(0, 2, none2);
        curAt[depth] = dst;
    };

    // A clip is its picture (mode/opacity of its track) followed by its double
    // exposure, blended in its own mode at its own amount.
    // `claimed` means a sequence above this clip already put it on a screen of its
    // own, so the clip's own look is not applied a second time inside it.
    std::function<void(Clip&, double, int, float, int, bool)> compositeClip;
    compositeClip = [&](Clip& c, double local, int mode, float opacity, int depth,
                        bool claimed) {
        const int look = claimed ? LOOK_PROJECTOR : c.look;
        const bool claims = claimed || c.look != LOOK_PROJECTOR;
        if (c.kind == Clip::Nest) {
            // A sequence is one picture, not a bag of clips. Its cut and its own
            // layers composite onto a canvas of its own - each with the mode it was
            // given inside the sequence - and that finished canvas is blended into
            // the parent once, in the nest's mode. Stamping the nest's mode onto
            // every clip inside it instead is what made a folded sequence look
            // different from the same sequence opened up.
            std::vector<NestHit> kids;
            NestChildren(c, local, kids);
            if (kids.empty()) return;
            int d2 = depth + 1;
            if (d2 > PROJ_MAX_DEPTH || 4 + 3 * d2 > g_projRtN) {
                // Deeper than we have canvases for: fall back to the flat walk rather
                // than drop the picture entirely.
                std::vector<NestHit> flat;
                NestResolve(c, local, flat);
                for (auto& h : flat)
                    compositeClip(*h.clip, h.local, h.mode ? h.mode : mode,
                                  opacity * h.opacity, depth, claims);
                return;
            }
            const int T2 = TargetBase(d2);
            curAt[d2] = 0;
            g_d3dContext->ClearRenderTargetView(g_projRTV[T2], clear);
            g_d3dContext->ClearRenderTargetView(g_projRTV[T2 + 1], clear);
            for (auto& k : kids)
                compositeClip(*k.clip, k.local, k.mode, k.opacity, d2, claims);
            // The canvas already matches the output frame, so it goes in 1:1. If the
            // sequence owns a screen, the finished canvas is what plays on it.
            blendElement(depth, g_projSRV[T2 + curAt[d2]], (float)outW / (float)outH,
                         mode, opacity, nullptr, true, LFIT_INSIDE, look, c.look43, local,
                         c.lanchor);
            return;
        }
        ID3D11ShaderResourceView* srv = nullptr;
        float ar = 1.0f;
        if (c.kind == Clip::Video && c.vid) {
            double at = c.reversed ? c.duration - local : local;
            srv = ProxyFrame(*c.vid, c.trimIn + at);
            ar = c.vid->aspect > 0 ? c.vid->aspect : 1.0f;
        } else if (c.kind == Clip::Image) {
            srv = c.tex;
            ar = c.texAspect;
        }
        const bool plate = !claimed && c.look == LOOK_PROJECTOR;   // seen through the gate
        if (srv) blendElement(depth, srv, ar, mode, opacity, &c.grade,
                              c.lfit == LFIT_FILL, c.lfit, look, c.look43, local, c.lanchor,
                              plate);
        if (c.dxOn) {
            ID3D11ShaderResourceView* lay = c.dxTex;
            float la = c.dxAspect;
            if (c.dxIsVideo && c.dxVid) {
                lay = ProxyFrame(*c.dxVid, c.dxTrimIn + local);
                la = c.dxVid->aspect > 0 ? c.dxVid->aspect : 1.0f;
            }
            // the double-exposure list starts at "screen", the layer list at "normal"
            if (lay) blendElement(depth, lay, la, c.dxBlend + 1, opacity * c.dxAmount,
                                  &c.grade, c.lfit == LFIT_FILL, c.lfit, look, c.look43, local,
                                  c.lanchor, plate);
        }
    };

    g_d3dContext->ClearRenderTargetView(g_projRTV[1], clear);
    if (ci >= 0 && !g_baseOff) compositeClip(*g_clips[ci], ph - clipStart, 0, 1.0f, 0, false);
    for (auto& tr : g_over) {
        if (!tr->visible) continue;
        for (auto& c : tr->clips)
            if (!c->skip && ph >= c->start && ph < c->start + c->duration)
                compositeClip(*c, ph - c->start, c->lblend, c->lopacity, 0, false);
    }
    int cur = curAt[0];

    if (g_grade.On()) {
        // One more pass over the finished composite: the shot grades are already
        // baked into it, so this is the film's own look sitting on top of them.
        int dst = cur ^ 1;
        g_d3dContext->OMSetRenderTargets(1, &g_projRTV[dst], nullptr);
        g_d3dContext->ClearRenderTargetView(g_projRTV[dst], clear);
        g_d3dContext->OMSetBlendState(nullptr, blend, 0xffffffff);
        g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11ShaderResourceView* none1[2] = { nullptr, nullptr };
        g_d3dContext->PSSetShaderResources(0, 2, none1);
        CompositeQuad(g_projSRV[cur], (float)outW / (float)outH, true, 1.0f,
                      outW, outH, &g_grade);
        g_d3dContext->PSSetShaderResources(0, 2, none1);
        cur = dst;
    }

    // A shot playing on its own screen is not being projected: the look already ran
    // on it during compositing, so no plate, gate or wall goes over the top.
    int lookNow = LOOK_PROJECTOR, pillarNow = 0;
    LookAtTime(ph, &lookNow, &pillarNow);

    if (!applyFilm || lookNow != LOOK_PROJECTOR) {   // compositing only, no film look
        g_d3dContext->OMSetRenderTargets(1, &oldRTV, oldDSV);
        if (oldRTV) oldRTV->Release();
        if (oldDSV) oldDSV->Release();
        return g_projSRV[cur];
    }

    // ---- the film pass, over the composited canvas
    ProjCB cb = {};
    cb.outSize[0] = (float)outW;
    cb.outSize[1] = (float)outH;
    float boxW = outW * g_projMargin, boxH = outH * g_projMargin;
    float pAr = PlateAspect();
    float ar = pAr > 0.01f ? pAr : (float)outW / (float)outH;
    float pw = boxW, phh = boxW / ar;
    if (phh > boxH) { phh = boxH; pw = phh * ar; }
    cb.plateOrg[0] = (outW - pw) * 0.5f;
    cb.plateOrg[1] = (outH - phh) * 0.5f;
    cb.plateSize[0] = pw;
    cb.plateSize[1] = phh;
    // how the canvas fills the plate — the canvas is already canvas-aspect, so this
    // only bites when the plate aspect is overridden
    float srcAr = (float)outW / (float)outH, plateAr = pw / phh;
    float sx = 1.0f, sy = 1.0f;
    if (g_projFit == 2) { sx = sy = 1.0f; }                        // stretch
    else if (g_projFit == 1) {                                     // contain
        if (srcAr > plateAr) sy = srcAr / plateAr; else sx = plateAr / srcAr;
    } else {                                                       // cover
        if (srcAr > plateAr) sx = plateAr / srcAr; else sy = srcAr / plateAr;
    }
    cb.srcScale[0] = sx; cb.srcScale[1] = sy;
    cb.srcOffset[0] = (1.0f - sx) * 0.5f;
    cb.srcOffset[1] = (1.0f - sy) * 0.5f;
    cb.time = (float)(t + g_projTimeOffset);
    cb.intensity = g_projIntensity < 0 ? 0 : (g_projIntensity > 1 ? 1 : g_projIntensity);
    cb.grain = g_projGrain < 0 ? 0 : g_projGrain;
    cb.grainFps = (float)g_fps;            // one grain per film frame, as the export
    float apW = pw - 2.0f * g_projGateInset, apH = phh - 2.0f * g_projGateInset;
    cb.ap[0] = (apW < 2 ? 2 : apW) * 0.5f;
    cb.ap[1] = (apH < 2 ? 2 : apH) * 0.5f;
    cb.wall[0] = g_projWall[0]; cb.wall[1] = g_projWall[1]; cb.wall[2] = g_projWall[2];
    cb.gateOn = g_projGate ? 1.0f : 0.0f;
    cb.fxWeave = g_fxWeave;       cb.fxRipple = g_fxRipple;
    cb.fxAber = g_fxAber;         cb.fxHalation = g_fxHalation;
    cb.fxFlicker = g_fxFlicker;   cb.fxDust = g_fxDust;
    cb.fxHair = g_fxHair;         cb.fxScratch = g_fxScratch;
    cb.fxVignette = g_fxVignette;
    SetCB(cb);

    ID3D11ShaderResourceView* none = nullptr;
    g_d3dContext->PSSetShaderResources(0, 1, &none);
    g_d3dContext->OMSetRenderTargets(1, &g_projRTV[3], nullptr);
    g_d3dContext->ClearRenderTargetView(g_projRTV[3], clear);
    g_d3dContext->OMSetBlendState(nullptr, blend, 0xffffffff);
    g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3dContext->VSSetShader(g_fsVS, nullptr, 0);
    g_d3dContext->PSSetShader(g_projPS, nullptr, 0);
    g_d3dContext->PSSetShaderResources(0, 1, &g_projSRV[cur]);
    g_d3dContext->Draw(3, 0);

    g_d3dContext->PSSetShaderResources(0, 1, &none);
    g_d3dContext->OMSetRenderTargets(1, &oldRTV, oldDSV);
    if (oldRTV) oldRTV->Release();
    if (oldDSV) oldDSV->Release();
    return g_projSRV[3];
}

// -------------------------------------------------------------- preview draw

// Fill `a`..`b` with a texture, cropping (cover) or letterboxing (contain).
static void DrawFitted(ImDrawList* dl, ImTextureID tex, float aspect,
                       ImVec2 a, ImVec2 b, bool cover, ImU32 tint,
                       int anchor = LANCHOR_CENTER) {
    float bw = b.x - a.x, bh = b.y - a.y;
    if (bw <= 1 || bh <= 1 || aspect <= 0) return;
    float boxA = bw / bh;
    if (cover) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        float ax, ay;
        AnchorFrac(anchor, &ax, &ay);
        if (aspect > boxA) { float f = boxA / aspect; uv0.x = (1.0f - f) * ax; uv1.x = uv0.x + f; }
        else               { float f = aspect / boxA; uv0.y = (1.0f - f) * ay; uv1.y = uv0.y + f; }
        dl->AddImage(tex, a, b, uv0, uv1, tint);
    } else {
        float w = bw, h = w / aspect;
        if (h > bh) { h = bh; w = h * aspect; }
        ImVec2 q0(a.x + (bw - w) * 0.5f, a.y + (bh - h) * 0.5f);
        dl->AddImage(tex, q0, ImVec2(q0.x + w, q0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
}

// One block of centred lines, drawn the way drawtext will lay them out.
static ImVec2 MeasureTextBlock(const std::string& text, float pxIn, std::vector<std::string>* out) {
    float px = SafePx(pxIn);
    std::vector<std::string> lines;
    for (size_t b = 0, e; ; b = e + 1) {
        e = text.find('\n', b);
        lines.push_back(text.substr(b, e == std::string::npos ? e : e - b));
        if (e == std::string::npos) break;
    }
    ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
    float w = 0;
    for (auto& ln : lines) {
        ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, ln.c_str());
        if (ts.x > w) w = ts.x;
    }
    if (out) *out = lines;
    return ImVec2(w, px * 1.25f * lines.size());
}

static void DrawTextBlock(ImDrawList* dl, const std::string& text, float pxIn, ImVec2 center,
                          ImU32 col, bool shadow) {
    float px = SafePx(pxIn);
    std::vector<std::string> lines;
    ImVec2 sz = MeasureTextBlock(text, px, &lines);
    ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
    float lineH = px * 1.25f;
    float y = center.y - sz.y * 0.5f;
    for (auto& ln : lines) {
        ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, ln.c_str());
        ImVec2 at(center.x - ts.x * 0.5f, y);
        if (shadow) dl->AddText(tf, px, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 140),
                                ln.c_str());
        dl->AddText(tf, px, at, col, ln.c_str());
        y += lineH;
    }
}

// The preview pane: canvas rect at the export aspect, the base clip inside it, then
// every visible overlay track on top, then the text overlays — which can be dragged.
static int g_txtDragUid = -1;              // clip whose text block is being dragged

static void DrawPreviewArea(ImVec2 size) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(10, 10, 10, 255));
    ImGui::InvisibleButton("##preview", size);
    bool hovered = ImGui::IsItemHovered();
    bool activeBtn = ImGui::IsItemActive();

    // canvas rect
    float fa = PresetAspect();
    float fh = size.y - 12, fw = fh * fa;
    if (fw > size.x - 12) { fw = size.x - 12; fh = fw / fa; }
    ImVec2 f0(p0.x + (size.x - fw) * 0.5f, p0.y + (size.y - fh) * 0.5f);
    ImVec2 f1(f0.x + fw, f0.y + fh);
    dl->AddRectFilled(f0, f1, IM_COL32(0, 0, 0, 255));
    dl->PushClipRect(f0, f1, true);

    double ph = g_playhead.load();
    double tot = TotalDuration();
    double clipStart = 0;
    int ci = ClipAt(ph, &clipStart);
    // Live film look: composite this frame offscreen and run the projector shader
    // over it, at the pixel size it will be shown at so the grain stays crisp.
    // The compositor runs whether or not the film look is on, so layer blend modes
    // and double exposures always look like what ffmpeg will produce.
    ID3D11ShaderResourceView* projSRV = nullptr;
    // Anything on any track is enough. A sequence folded out of layer clips alone has
    // an empty base cut, and gating the compositor on the base cut meant standing
    // inside such a sequence showed its layers flat, with no blend mode applied at
    // all - while the same sequence seen from outside blended correctly.
    bool anyPicture = !g_clips.empty();
    for (auto& tr : g_over) {
        if (!tr->visible) continue;
        for (auto& c : tr->clips) if (!c->skip) { anyPicture = true; break; }
    }
    if (anyPicture && fw > 4 && fh > 4) {
        int rw = (int)fw, rh = (int)fh;
        if (rw > 1920) { rh = (int)(rh * 1920.0f / rw); rw = 1920; }
        rw += rw & 1; rh += rh & 1;
        projSRV = RenderProjectorGPU(rw, rh, g_playhead.load(), g_projLive);
    }
    bool projFrame = projSRV != nullptr;

    // one clip's picture, at `local` seconds into it
    auto drawPicture = [&](Clip& c, double local, float alphaMul) {
        int a = (int)(alphaMul * 255.0f);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        ImU32 tint = IM_COL32(255, 255, 255, a);
        if (c.kind == Clip::Video && c.vid) {
            double at = c.reversed ? c.duration - local : local;
            ID3D11ShaderResourceView* fr = ProxyFrame(*c.vid, c.trimIn + at);
            if (fr) DrawFitted(dl, (ImTextureID)fr, c.vid->aspect > 0 ? c.vid->aspect : 1.0f,
                               f0, f1, c.lfit == LFIT_FILL, tint, c.lanchor);
            else {
                const char* msg = c.vid->probed.load() ? "building preview…" : "reading video…";
                ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText(ImVec2(f0.x + (fw - ts.x) * 0.5f, f0.y + (fh - ts.y) * 0.5f),
                            IM_COL32(130, 130, 130, 255), msg);
            }
        } else if (c.kind == Clip::Image && c.tex) {
            DrawFitted(dl, (ImTextureID)c.tex, c.texAspect, f0, f1, c.lfit == LFIT_FILL, tint,
                       c.lanchor);
        } else if (c.kind == Clip::Text) {
            dl->AddRectFilled(f0, f1, IM_COL32(0, 0, 0, a));
        }
        if (c.dxOn) {                      // double exposure, approximated with alpha
            ID3D11ShaderResourceView* lay = c.dxTex;
            float la = c.dxAspect;
            if (c.dxIsVideo && c.dxVid) {
                lay = ProxyFrame(*c.dxVid, c.dxTrimIn + local);
                la = c.dxVid->aspect > 0 ? c.dxVid->aspect : 1.0f;
            }
            if (lay) {
                int da = (int)(c.dxAmount * alphaMul * 255.0f);
                DrawFitted(dl, (ImTextureID)lay, la, f0, f1, c.lfit == LFIT_FILL,
                           IM_COL32(255, 255, 255, da < 0 ? 0 : da > 255 ? 255 : da));
            }
        }
    };

    // a clip's text — card words or burned overlay — plus drag-to-place. Words inside a
    // sequence are drawn but not dragged here: they belong to the level they sit on.
    auto drawText = [&](Clip& c, int selTrack, int selIdx, int myTrack, int myIdx,
                        bool interactive = true) {
        bool card = c.kind == Clip::Text && !c.text.empty();
        bool ovl  = c.ovlOn && !c.ovlText.empty();
        if (!card && !ovl) return;
        const std::string& txt = card ? c.text : c.ovlText;
        float px = SafePx((card ? c.textScale : c.ovlScale) * fh);
        ImVec2 center(f0.x + c.ovlX * fw, f0.y + c.ovlY * fh);
        ImU32 col = IM_COL32((int)(c.ovlCol[0] * 255), (int)(c.ovlCol[1] * 255),
                             (int)(c.ovlCol[2] * 255), (int)(c.ovlAlpha * 255));
        DrawTextBlock(dl, txt, px, center, col, c.ovlShadow);
        if (!interactive) return;

        ImVec2 sz = MeasureTextBlock(txt, px, nullptr);
        ImVec2 a(center.x - sz.x * 0.5f - 6, center.y - sz.y * 0.5f - 6);
        ImVec2 b(center.x + sz.x * 0.5f + 6, center.y + sz.y * 0.5f + 6);
        bool over = io.MousePos.x >= a.x && io.MousePos.x <= b.x &&
                    io.MousePos.y >= a.y && io.MousePos.y <= b.y;
        if (hovered && over) {
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 150), 3.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (ImGui::IsItemActivated() && over) {
            g_txtDragUid = c.uid;
            g_sel = myIdx; g_selTrack = myTrack;
        }
        if (!activeBtn && g_txtDragUid == c.uid) g_txtDragUid = -1;
        if (g_txtDragUid == c.uid && fw > 1 && fh > 1) {
            c.ovlX += io.MouseDelta.x / fw;
            c.ovlY += io.MouseDelta.y / fh;
            c.ovlX = c.ovlX < 0.0f ? 0.0f : (c.ovlX > 1.0f ? 1.0f : c.ovlX);
            c.ovlY = c.ovlY < 0.0f ? 0.0f : (c.ovlY > 1.0f ? 1.0f : c.ovlY);
            ImGui::SetTooltip("x %.3f  y %.3f", c.ovlX, c.ovlY);
        }
        (void)selTrack; (void)selIdx;
    };
    // The words of everything a sequence shows at the playhead, nested sequences
    // included, under the sequence's own - the order the export burns them in.
    std::function<void(Clip&, double)> drawInnerText = [&](Clip& n, double local) {
        std::vector<NestHit> kids;
        NestChildren(n, local, kids);
        for (auto& k : kids) {
            drawInnerText(*k.clip, k.local);
            drawText(*k.clip, -1, -1, -1, -1, false);
        }
    };

    if (projFrame) {                        // the shader already composited the picture
        dl->AddImage((ImTextureID)projSRV, f0, f1);
        if (ci >= 0 && !g_baseOff) {
            drawInnerText(*g_clips[ci], ph - clipStart);
            drawText(*g_clips[ci], g_selTrack, g_sel, -1, ci);
        }
    } else if (ci >= 0 && !g_baseOff) {
        Clip& c = *g_clips[ci];
        drawPicture(c, ph - clipStart, 1.0f);
        drawInnerText(c, ph - clipStart);
        drawText(c, g_selTrack, g_sel, -1, ci);
    } else if (g_clips.empty()) {
        const char* msg  = "Drop footage, stills or sound";
        const char* msg2 = "";
        ImVec2 ts  = ImGui::CalcTextSize(msg);
        ImVec2 ts2 = ImGui::CalcTextSize(msg2);
        float cy = f0.y + fh * 0.5f;
        dl->AddText(ImVec2(f0.x + (fw - ts.x) * 0.5f, cy - ts.y - 4),
                    IM_COL32(140, 140, 140, 255), msg);
        dl->AddText(ImVec2(f0.x + (fw - ts2.x) * 0.5f, cy + 6),
                    IM_COL32(95, 95, 95, 255), msg2);
    }

    // overlay tracks, bottom track first. Their pictures are already in the shader
    // composite, so with the film look on only the text is drawn here.
    for (int t = 0; t < (int)g_over.size(); t++) {
        if (!g_over[t]->visible) continue;
        auto& v = g_over[t]->clips;
        for (int i = 0; i < (int)v.size(); i++) {
            Clip& c = *v[i];
            if (ph < c.start || ph >= c.start + c.duration) continue;
            if (!projFrame) drawPicture(c, ph - c.start, c.lopacity);
            drawInnerText(c, ph - c.start);
            drawText(c, g_selTrack, g_sel, t, i);
        }
    }

    // A soft vignette, so the canvas sits in the frame the way a gate does.
    {
        const int N = 7;
        for (int i = 0; i < N; i++) {
            float t = (float)i / N;
            int a = (int)(30 * (1.0f - t) * (1.0f - t));
            float in = 3.0f + t * fh * 0.16f;
            dl->AddRect(ImVec2(f0.x + in * (fw / fh), f0.y + in),
                        ImVec2(f1.x - in * (fw / fh), f1.y - in),
                        IM_COL32(0, 0, 0, a), 0, 0, 3.0f);
        }
    }
    dl->PopClipRect();

    // The gate: a thin frame with the corners cut, like a projector aperture plate.
    dl->AddRect(f0, f1, IM_COL32(255, 255, 255, 34));
    {
        float k = 14.0f;
        ImU32 mk = IM_COL32(255, 255, 255, 90);
        dl->AddLine(ImVec2(f0.x, f0.y + k), ImVec2(f0.x + k, f0.y), mk);
        dl->AddLine(ImVec2(f1.x - k, f0.y), ImVec2(f1.x, f0.y + k), mk);
        dl->AddLine(ImVec2(f0.x, f1.y - k), ImVec2(f0.x + k, f1.y), mk);
        dl->AddLine(ImVec2(f1.x - k, f1.y), ImVec2(f1.x, f1.y - k), mk);
    }

    // Footage counter in the typewriter face — seconds + frame, the way a slate reads.
    {
        ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
        float px = SafePx(17.0f);
        int fr = (int)(fmod(ph, 1.0) * g_fps);
        char tc[96];
        snprintf(tc, sizeof(tc), "%02d:%02d+%02d   /   %02d:%02d   %d fps%s",
                 (int)ph / 60, (int)fmod(ph, 60.0), fr,
                 (int)tot / 60, (int)fmod(tot, 60.0), g_fps,
                 projFrame ? "   film look" : "");
        bool run = g_playing.load();
        float cx = p0.x + 16, cy = p0.y + 12;
        dl->AddCircleFilled(ImVec2(cx, cy + px * 0.45f), run ? 4.0f : 3.0f,
                            run ? IM_COL32(235, 235, 235, 235) : IM_COL32(120, 120, 120, 200));
        dl->AddText(tf, px, ImVec2(cx + 12, cy), IM_COL32(235, 232, 224, 210), tc);
    }
}

// ------------------------------------------------------------- side panels

// Which grade controls were touched this frame. Only the touched fields are
// pushed onto the rest of the selection, so shots keep the settings the edit did
// not name.
enum GradeField { GF_MONO = 1, GF_SAT = 2, GF_BRIGHT = 4, GF_CONTRAST = 8,
                  GF_TEMP = 16, GF_ALL = 31 };

static int GradeControls(Grade& g, const char* id) {
    ImGui::PushID(id);
    int ch = 0;
    Prop("monochrome");
    if (ImGui::Checkbox("black and white", &g.mono)) ch |= GF_MONO;
    ImGui::BeginDisabled(g.mono);
    Prop("saturation");
    if (ImGui::SliderFloat("##sat", &g.sat, 0.0f, 2.0f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp)) ch |= GF_SAT;
    ImGui::EndDisabled();
    Prop("brightness");
    if (ImGui::SliderFloat("##bright", &g.bright, -0.5f, 0.5f, "%+.2f",
                           ImGuiSliderFlags_AlwaysClamp)) ch |= GF_BRIGHT;
    Prop("contrast");
    if (ImGui::SliderFloat("##contrast", &g.contrast, 0.0f, 2.0f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp)) ch |= GF_CONTRAST;
    Prop("warmth");
    if (ImGui::SliderFloat("##temp", &g.temp, -1.0f, 1.0f, "%+.2f",
                           ImGuiSliderFlags_AlwaysClamp)) ch |= GF_TEMP;
    Prop("");
    if (ImGui::Button("neutral", ImVec2(-1, 0))) { g = Grade(); ch = GF_ALL; }
    ImGui::PopID();
    return ch;
}

// Copy just the fields named by `fields` from src onto dst.
static void ApplyGradeFields(Grade& dst, const Grade& src, int fields) {
    if (fields & GF_MONO)     dst.mono = src.mono;
    if (fields & GF_SAT)      dst.sat = src.sat;
    if (fields & GF_BRIGHT)   dst.bright = src.bright;
    if (fields & GF_CONTRAST) dst.contrast = src.contrast;
    if (fields & GF_TEMP)     dst.temp = src.temp;
}

static void DrawClipInspector() {
    // an aspect point is selected
    if (g_selTrack == -3) {
        if (g_sel < 0 || g_sel >= (int)g_aspects.size()) { ImGui::TextDisabled("nothing selected"); return; }
        AspectPoint& a = *g_aspects[g_sel];
        ImGui::TextUnformatted("Aspect point");
        ImGui::TextDisabled("holds until the next point");
        Prop("start");
        float off = (float)a.offset;
        if (ImGui::InputFloat("##aoff", &off, 0.1f, 1.0f, "%.3f s")) {
            a.offset = off < 0 ? 0 : off;
            SortAspects();
        }
        Prop("aspect");
        ImGui::SliderFloat("##aar", &a.aspect, 0.0f, 3.0f,
                           a.aspect < 0.01f ? "source" : "%.3f");
        Prop("");
        if (ImGui::Button("16:9")) a.aspect = 16.0f / 9.0f; ImGui::SameLine();
        if (ImGui::Button("9:16")) a.aspect = 9.0f / 16.0f; ImGui::SameLine();
        if (ImGui::Button("4:3"))  a.aspect = 4.0f / 3.0f;  ImGui::SameLine();
        if (ImGui::Button("1:1"))  a.aspect = 1.0f;
        Prop("");
        if (ImGui::Button("delete point", ImVec2(-1, 0))) {
            g_aspects.erase(g_aspects.begin() + g_sel);
            g_sel = -1; g_selTrack = -1;
            g_selUids.clear();
        }
        return;
    }
    // an audio block is selected: its own small panel
    if (g_selTrack == -2) {
        if (g_selAT < 0 || g_selAT >= (int)g_atracks.size() ||
            g_sel < 0 || g_sel >= (int)g_atracks[g_selAT]->blocks.size()) {
            ImGui::TextDisabled("nothing selected");
            return;
        }
        AudioTrack& tr = *g_atracks[g_selAT];
        Song& s = *tr.blocks[g_sel];
        ImGui::TextUnformatted(s.label.c_str());
        ImGui::TextDisabled("%s / %.2f s", tr.name.c_str(),
                            s.gen ? s.trimEnd - s.trimStart : s.duration);
        {
            int nSel = 0;                            // edits below land on every picked block
            for (auto& t : g_atracks) for (auto& o : t->blocks) if (SelHas(o->uid)) nSel++;
            if (!SelHas(s.uid)) nSel++;
            if (nSel > 1)
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                                   "editing %d sound blocks%s", nSel,
                                   s.group ? " (grouped)" : "");
        }
        if (s.gen) {
            // A texture block: a noise bed is its chain heard on its own; a chain
            // region runs its chain over another track for as long as it lasts.
            Prop("kind");
            if (ImGui::RadioButton("noise bed", s.gen == 1)) s.gen = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("chain region", s.gen == 2)) {
                s.gen = 2;
                if (s.fx <= AFX_NONE || s.fx >= AFX_COUNT) s.fx = AFX_PHONE;
            }
            if (s.gen == 2) {
                Prop("runs over");
                const char* cur = s.target >= 0 && s.target < (int)g_atracks.size()
                                      ? g_atracks[s.target]->name.c_str() : "(pick a track)";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##rtarget", cur)) {
                    for (int i = 0; i < (int)g_atracks.size(); i++) {
                        if (i == g_selAT) continue;              // not its own track
                        ImGui::PushID(i);
                        if (ImGui::Selectable(g_atracks[i]->name.c_str(), s.target == i))
                            s.target = i;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        Prop("start");
        float off = (float)s.offset;
        if (ImGui::InputFloat("##aoff", &off, 0.1f, 1.0f, "%.3f s")) s.offset = off;
        Prop("trim");
        float a = (float)s.trimStart, b = (float)s.trimEnd;
        if (ImGui::DragFloatRange2("##atrim", &a, &b, 0.01f, 0.0f, (float)s.duration,
                                   "%.2f", "%.2f")) {
            s.trimStart = a < 0 ? 0 : a;
            s.trimEnd = b > s.duration ? s.duration : b;
            if (s.trimEnd < s.trimStart + 0.1) s.trimEnd = s.trimStart + 0.1;
        }
        // This block's own chain: split a block to change the sound partway, and
        // give a later block the same chain to bring it back.
        // A noise bed plays a texture; a chain region and a plain block carry a chain.
        if (s.gen == 1) {
            Prop("sound");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bedtex", &s.tex, kTexNames, TEX_COUNT))
                ForEachOtherSelectedSong(s, [&](Song& o) { if (o.gen == 1) o.tex = s.tex; });
        } else {
            Prop(s.gen == 2 ? "chain" : "clip chain");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bfx", &s.fx, kAfxNames, AFX_COUNT))
                ForEachOtherSelectedSong(s, [&](Song& o) { o.fx = s.fx; });
        }
        if (s.gen == 1 || s.fx > AFX_NONE) {
            Prop(s.gen == 1 ? "level" : s.gen == 2 ? "amount" : "clip amount");
            if (ImGui::SliderFloat("##bfxmix", &s.fxMix, 0.0f, 1.0f, "%.2f"))
                ForEachOtherSelectedSong(s, [&](Song& o) { o.fxMix = s.fxMix; });
        }
        if (s.gen != 2) {                        // a region makes no sound to fade
            Prop("clip volume");
            // In decibels, stored as a linear gain. The bottom of the travel is silence.
            const float DB_MIN = -60.0f, DB_MAX = 6.0f;
            float db = s.volume <= 0.001f ? DB_MIN : 20.0f * log10f(s.volume);
            db = std::clamp(db, DB_MIN, DB_MAX);
            const char* dbFmt = db <= DB_MIN + 0.01f ? "-inf dB" : "%+.1f dB";
            bool volSet = ImGui::SliderFloat("##bvol", &db, DB_MIN, DB_MAX, dbFmt);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { db = 0.0f; volSet = true; }
            if (volSet) {
                s.volume = db <= DB_MIN + 0.01f ? 0.0f : std::min(powf(10.0f, db / 20.0f), 2.0f);
                ForEachOtherSelectedSong(s, [&](Song& o) { if (o.gen != 2) o.volume = s.volume; });
            }
            float blen = (float)(s.trimEnd - s.trimStart);
            float fmaxS = std::min(10.0f, std::max(blen, 0.0f));
            Prop("fade in");
            if (ImGui::SliderFloat("##bfin", &s.fadeIn, 0.0f, fmaxS, "%.2f s")) {
                s.fadeIn = std::clamp(s.fadeIn, 0.0f, fmaxS);
                ForEachOtherSelectedSong(s, [&](Song& o) { if (o.gen != 2) o.fadeIn = s.fadeIn; });
            }
            Prop("fade out");
            if (ImGui::SliderFloat("##bfout", &s.fadeOut, 0.0f, fmaxS, "%.2f s")) {
                s.fadeOut = std::clamp(s.fadeOut, 0.0f, fmaxS);
                ForEachOtherSelectedSong(s, [&](Song& o) { if (o.gen != 2) o.fadeOut = s.fadeOut; });
            }
        }
        // The track's own settings (chain, level, mute) live in the tracks panel,
        // not on a block: they belong to the track whether or not this block is on it.
        ImGui::TextDisabled("track chain, level and mute: tracks panel");
        Prop("");
        if (ImGui::Button("remove block", ImVec2(-1, 0))) {
            { MixGuard lock; tr.blocks.erase(tr.blocks.begin() + g_sel); }
            g_sel = -1;
        }
        return;
    }

    Clip* sc = SelectedClip();
    if (!sc) {
        ImGui::TextDisabled("nothing selected");
        return;
    }
    Clip& c = *sc;
    bool isLayer = g_selTrack >= 0;

    if (c.kind == Clip::Nest) {              // a folded sequence: its own small panel
        Prop("direction");
        if (ImGui::Checkbox("play backwards", &c.reversed))
            ForEachOtherSelected(c, [&](Clip& o) { o.reversed = c.reversed; });
        Prop("mute");
        if (ImGui::Checkbox("skip this sequence", &c.skip)) {
            ForEachOtherSelected(c, [&](Clip& o) { o.skip = c.skip; });
            RefreshNestDurations();
        }
        if (int gch = GradeControls(c.grade, "seq"))
            ForEachOtherSelected(c, [&](Clip& o) { ApplyGradeFields(o.grade, c.grade, gch); });
        Sequence* q = FindSeq(c.nest);
        if (!q) { ImGui::TextDisabled("sequence is gone"); return; }
        ImGui::Text("#%d  %s", g_sel + 1, q->name.c_str());
        ImGui::TextDisabled("sequence / %.3f s / %d shots / %d layers",
                            c.duration, (int)q->clips.size(), (int)q->over.size());
        Prop("name");
        char nb[64];
        snprintf(nb, sizeof(nb), "%s", q->name.c_str());
        if (ImGui::InputText("##seqname", nb, sizeof(nb))) {
            q->name = nb;
            c.label = nb;
        }
        Prop("");
        float half = ColW(2);
        if (ImGui::Button("open", ImVec2(half, 0))) g_navEnter = c.nest;
        ImGui::SameLine();
        if (ImGui::Button("unfold here", ImVec2(half, 0))) {
            g_navUnfold = g_sel;
            g_navUnfoldTrack = g_selTrack;      // a layer nest unfolds on its layer
        }
        return;
    }

    ImGui::Text("#%d  %s%s", g_sel + 1, c.label.c_str(), c.skip ? "   [muted]" : "");
    ImGui::TextDisabled("%s / %.3f s / %s", c.kind == Clip::Video ? "video"
                        : c.kind == Clip::Text ? "card" : "still", c.duration,
                        isLayer ? g_over[g_selTrack]->name.c_str() : "picture");
    int nSel = SelCount();
    if (nSel > 1)
        ImGui::TextDisabled("editing %d shots — every setting below lands on all of them",
                            nSel);

    Prop("mute");
    if (ImGui::Checkbox("skip this shot", &c.skip))
        ForEachOtherSelected(c, [&](Clip& o) { o.skip = c.skip; });
    if (int gch = GradeControls(c.grade, "shot"))
        ForEachOtherSelected(c, [&](Clip& o) { ApplyGradeFields(o.grade, c.grade, gch); });
    Prop("");
    if (ImGui::Button("this whole colour to all selected", ImVec2(-1, 0))) {
        int n = 0;
        ForEachOtherSelected(c, [&](Clip& o) { o.grade = c.grade; n++; });
        char buf[48];
        snprintf(buf, sizeof(buf), "colour copied to %d shot%s", n, n == 1 ? "" : "s");
        g_intakeStatus = buf;
    }
    Prop("duration");
    double d = c.duration;
    if (ImGui::InputDouble("##dur", &d, 0.1, 1.0, "%.3f s")) {
        double mx = MaxDuration(c);
        c.duration = d < MinClipDur() ? MinClipDur() : (d > mx ? mx : d);
        if (nSel > 1) TrimSelectionTo(d);             // the rest of the run follows
    }
    if (g_hcLive && nSel <= 1) HyperCutPanel();   // a live preview keeps its controls
    if (nSel > 1) {                     // one length over the whole selection
        Prop("even length");
        static float evenLen = 2.0f;
        float half = ColW(2);
        ImGui::SetNextItemWidth(half);
        ImGui::DragFloat("##evenlen", &evenLen, 0.01f, (float)MinClipDur(), 120.0f, "%.2f s",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::Button("trim all to this", ImVec2(-1, 0))) {
            int n = TrimSelectionTo(evenLen);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d shots trimmed to %.2f s", n, evenLen);
            g_intakeStatus = buf;
        }
        Prop("");
        if (ImGui::Button("match this shot's length  (T)", ImVec2(-1, 0))) {
            evenLen = (float)c.duration;
            int n = TrimSelectionTo(c.duration);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d shots trimmed to %.2f s", n, c.duration);
            g_intakeStatus = buf;
        }
        HyperCutPanel();
        ImGui::TextDisabled("shift+wheel over a shot slips the picture inside it");
    }
    if (c.kind == Clip::Video && c.vid) {
        Prop("in point");
        float in = (float)c.trimIn;
        float mx = (float)(c.vid->duration > 0 ? c.vid->duration : 1.0);
        if (ImGui::SliderFloat("##inpt", &in, 0.0f, mx, "%.3f s")) {
            double was = c.trimIn;
            double lim = c.vid->duration - MinClipDur();
            c.trimIn = in < 0 ? 0 : (in > lim ? lim : in);
            double m = MaxDuration(c);
            if (c.duration > m) c.duration = m;
            double dIn = c.trimIn - was;              // the rest of the run slips along
            ForEachOtherSelected(c, [&](Clip& o) {
                if (o.kind != Clip::Video || !o.vid || o.vid->duration <= 0) return;
                double olim = o.vid->duration - o.duration;
                if (olim < 0) olim = 0;
                double t = o.trimIn + dIn;
                o.trimIn = t < 0 ? 0 : (t > olim ? olim : t);
            });
        }
        Prop("sound");
        ImGui::BeginDisabled(!c.vid->hasAudio);
        if (ImGui::Checkbox("keep this clip's own audio", &c.useAudio))
            ForEachOtherSelected(c, [&](Clip& o) { o.useAudio = c.useAudio; });
        if (c.useAudio) {
            Prop("volume");
            if (ImGui::SliderFloat("##vol", &c.volume, 0.0f, 2.0f, "%.2f"))
                ForEachOtherSelected(c, [&](Clip& o) { o.volume = c.volume; });
        }
        ImGui::EndDisabled();
        Prop("direction");
        if (ImGui::Checkbox("play backwards", &c.reversed))
            ForEachOtherSelected(c, [&](Clip& o) { o.reversed = c.reversed; });
    }
    if (c.kind == Clip::Text) {
        // The words live right here: type into the panel and the card follows,
        // no trip through the popup. The buffer is refilled whenever the
        // selection moves to a different card.
        static char cbuf[1024];
        static int  cbufFor = -1;
        if (cbufFor != c.uid) {
            snprintf(cbuf, sizeof(cbuf), "%s", c.text.c_str());
            cbufFor = c.uid;
        }
        Prop("words");
        if (ImGui::InputTextMultiline("##cardtxt", cbuf, sizeof(cbuf), ImVec2(-1, 76))) {
            c.text = cbuf;
            c.label = FirstLine(c.text);
        }
        Prop("card size");
        ImGui::SliderFloat("##csz", &c.textScale, 0.03f, 0.30f, "%.3f");
        Prop("");
        if (ImGui::Button("edit in a bigger box", ImVec2(-1, 0))) {
            g_tl.editIndex = g_sel;
            g_tl.editTrack = g_selTrack;
            g_tl.editOpenText = true;      // the root window owns the popup
            cbufFor = -1;                  // popup may rewrite it: refill on the way back
        }
    }

    if (c.kind != Clip::Text) {             // how this clip meets an off-aspect canvas
        ImGui::SeparatorText("frame");
        if (SegRow("off-canvas", &c.lfit, LFIT_ITEMS))
            ForEachOtherSelected(c, [&](Clip& o) { o.lfit = c.lfit; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What fills the canvas where this clip does not reach.\n"
                              "Inside: nothing - a layer shows the cut underneath,\n"
                              "  a base cut shows the projector wall.\n"
                              "Fill: crop this clip until it covers.\n"
                              "Blur bed / Black bed: keep this clip whole and hide\n"
                              "  what is behind it, cropping neither.");

        // Filling crops, so say which part of the shot survives. Only the axis that
        // actually overflows can move, which is why one row or column of the grid
        // will look inert on a clip that only overflows the other way.
        ImGui::BeginDisabled(c.lfit != LFIT_FILL);
        Prop("keep");
        float cell = ImGui::GetFrameHeight();
        ImGui::BeginGroup();
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                int a = row * 3 + col;
                if (col) ImGui::SameLine(0.0f, 2.0f);
                ImGui::PushID(a);
                bool on = c.lanchor == a;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("##anchor", ImVec2(cell, cell))) {
                    c.lanchor = a;
                    ForEachOtherSelected(c, [&](Clip& o) { o.lanchor = a; });
                }
                if (on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", LANCHOR_ITEMS[a]);
                ImGui::PopID();
            }
        }
        ImGui::EndGroup();
        ImGui::EndDisabled();
    }

    {                                       // which screen this shot plays on
        ImGui::SeparatorText("screen");
        if (SegRow("look", &c.look, LOOK_ITEMS))
            ForEachOtherSelected(c, [&](Clip& o) { o.look = c.look; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Projector: the film treatment, as the projector panel sets it.\n"
                              "CRT: play this shot on the glass of a tube set.\n"
                              "CCTV: a low-bandwidth monochrome camera feed.\n"
                              "CCTV + CRT: that feed shown on the tube.\n"
                              "The tube looks bypass the plate and gate entirely.\n"
                              "On a sequence, the screen it picks overrides every\n"
                              "clip inside it.");
        ImGui::BeginDisabled(c.look == LOOK_PROJECTOR);
        Prop("4:3 glass");
        if (ImGui::Checkbox("##look43", &c.look43))
            ForEachOtherSelected(c, [&](Clip& o) { o.look43 = c.look43; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A tube and a CCTV monitor are 4:3 objects. On, the set keeps the\n"
                              "full frame height and crops the frame to a centred 4:3 window,\n"
                              "so the side edges are lost and nothing is rescaled.\n"
                              "Off, the set fills the whole frame.");
        ImGui::EndDisabled();
    }

    if (isLayer) {                          // how this layer sits over the cut
        ImGui::SeparatorText("layer");
        Prop("start");
        float st = (float)c.start;
        if (ImGui::InputFloat("##lstart", &st, 0.1f, 1.0f, "%.3f s")) c.start = st < 0 ? 0 : st;
        if (SegRow("blend", &c.lblend, LAYER_ITEMS))
            ForEachOtherSelected(c, [&](Clip& o) { o.lblend = c.lblend; });
        Prop("opacity");
        if (ImGui::SliderFloat("##lop", &c.lopacity, 0.0f, 1.0f, "%.2f"))
            ForEachOtherSelected(c, [&](Clip& o) { o.lopacity = c.lopacity; });
        Prop("");
        float half = ColW(2);
        if (ImGui::Button("to playhead", ImVec2(half, 0))) c.start = g_playhead.load();
        ImGui::SameLine();
        if (ImGui::Button("to picture", ImVec2(-1, 0))) {
            auto& v = g_over[g_selTrack]->clips;
            auto mv = std::move(v[g_sel]);
            v.erase(v.begin() + g_sel);
            g_clips.push_back(std::move(mv));
            g_sel = (int)g_clips.size() - 1;
            g_selTrack = -1;
            return;
        }
    }

    ImGui::SeparatorText("scene split");
    Prop("threshold");
    ImGui::SliderFloat("##sthr", &g_sceneThresh, 0.05f, 0.90f, "%.2f");
    Prop("min shot");
    ImGui::InputDouble("##smin", &g_sceneMinLen, 0.05, 0.5, "%.2f s");
    Prop("");
    ImGui::BeginDisabled(c.kind != Clip::Video || g_sceneJob.active);
    if (ImGui::Button("detect & split", ImVec2(-1, 0))) StartSceneSplit(g_selTrack, g_sel);
    ImGui::EndDisabled();
    if (!g_sceneJob.status.empty()) ImGui::TextDisabled("%s", g_sceneJob.status.c_str());

    ImGui::SeparatorText("text");
    Prop("overlay");
    ImGui::Checkbox("burn text onto this clip", &c.ovlOn);
    if (c.ovlOn || c.kind == Clip::Text) {
        if (c.kind != Clip::Text) {
            static char buf[1024];
            static int  bufFor = -1;
            if (bufFor != g_sel) { snprintf(buf, sizeof(buf), "%s", c.ovlText.c_str());
                                   bufFor = g_sel; }
            Prop("words");
            if (ImGui::InputTextMultiline("##ovl", buf, sizeof(buf), ImVec2(-1, 64)))
                c.ovlText = buf;
            Prop("size");
            ImGui::SliderFloat("##osz", &c.ovlScale, 0.02f, 0.30f, "%.3f");
        }
        Prop("x");
        ImGui::SliderFloat("##ox", &c.ovlX, 0.0f, 1.0f, "%.3f");
        Prop("y");
        ImGui::SliderFloat("##oy", &c.ovlY, 0.0f, 1.0f, "%.3f");
        Prop("place");
        float half = ColW(2);
        if (ImGui::Button("centre", ImVec2(half, 0))) { c.ovlX = 0.5f; c.ovlY = 0.5f; }
        ImGui::SameLine();
        if (ImGui::Button("lower third", ImVec2(-1, 0))) { c.ovlX = 0.5f; c.ovlY = 0.78f; }
        Prop("colour");
        ImGui::ColorEdit3("##ocol", c.ovlCol, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("shadow", &c.ovlShadow);
        Prop("opacity");
        ImGui::SliderFloat("##oal", &c.ovlAlpha, 0.0f, 1.0f, "%.2f");
        Prop("");
        if (ImGui::Button("copy placement to every clip", ImVec2(-1, 0))) {
            std::vector<Clip*> all;
            for (auto& o : g_clips) all.push_back(o.get());
            for (auto& t : g_over) for (auto& o : t->clips) all.push_back(o.get());
            for (Clip* o : all) {
                o->ovlX = c.ovlX; o->ovlY = c.ovlY; o->ovlScale = c.ovlScale;
                o->ovlAlpha = c.ovlAlpha; o->ovlShadow = c.ovlShadow;
                memcpy(o->ovlCol, c.ovlCol, sizeof(c.ovlCol));
            }
        }
    }

    ImGui::SeparatorText("double exposure");
    Prop("layer");
    {
        float half = ColW(2);
        if (ImGui::Button(c.dxOn ? "change" : "pick a file", ImVec2(c.dxOn ? half : -1, 0))) {
            auto f = PickFiles(false, L"Image or video",
                               L"*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.webp;*.mp4;*.mov;*.mkv;"
                               L"*.webm;*.avi;*.m4v");
            if (!f.empty()) SetDoubleExposure(c, f[0]);
        }
        if (c.dxOn) {
            ImGui::SameLine();
            if (ImGui::Button("remove", ImVec2(-1, 0))) ClearDoubleExposure(c);
        }
    }
    if (c.dxOn) {
        Prop("");
        ImGui::TextDisabled("%s", c.dxLabel.c_str());
        SegRow("blend", &c.dxBlend, BLEND_ITEMS);
        Prop("amount");
        ImGui::SliderFloat("##dxam", &c.dxAmount, 0.0f, 1.0f, "%.2f");
        if (c.dxIsVideo && c.dxVid && c.dxVid->duration > 0) {
            Prop("layer in");
            float in = (float)c.dxTrimIn;
            if (ImGui::SliderFloat("##dxin", &in, 0.0f, (float)c.dxVid->duration, "%.2f s"))
                c.dxTrimIn = in;
        }
    }
}

static void DrawTracksPanel() {
    ImGui::SeparatorText("picture");
    for (int t = (int)g_over.size() - 1; t >= 0; t--) {
        VideoTrack& tr = *g_over[t];
        ImGui::PushID(t + 1000);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", tr.name.c_str());
        ImGui::SetNextItemWidth(LabelW() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputText("##name", buf, sizeof(buf))) tr.name = buf;
        ImGui::SameLine();
        ImGui::Checkbox("visible", &tr.visible);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu", tr.clips.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(g_over.size() < 2);
        if (ImGui::Button("remove", ImVec2(-1, 0))) {
            for (auto& c : tr.clips) {
                if (c->kind != Clip::Video) RetireTexture(c->tex);
                ClearDoubleExposure(*c);
            }
            g_over.erase(g_over.begin() + t);
            if (g_selTrack == t) { g_sel = -1; g_selTrack = -1; }
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (g_over.empty()) ImGui::TextDisabled("(none)");
    Prop("");
    ImGui::BeginDisabled(!SelectedClip() || g_selTrack != -1);
    if (ImGui::Button("lift selected clip onto a new track", ImVec2(-1, 0))) {
        double s = 0;
        { std::vector<BaseSpan> lay; BaseLayout(lay);
          if (g_sel >= 0 && g_sel < (int)lay.size()) s = lay[g_sel].start; }
        int t = NewOverlayTrack();
        auto mv = std::move(g_clips[g_sel]);
        mv->start = s;                     // it keeps the time it had in the cut
        g_clips.erase(g_clips.begin() + g_sel);
        g_over[t]->clips.push_back(std::move(mv));
        g_selTrack = t;
        g_sel = (int)g_over[t]->clips.size() - 1;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("sound");
    for (int t = 0; t < (int)g_atracks.size(); t++) {
        AudioTrack& tr = *g_atracks[t];
        ImGui::PushID(t + 2000);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", tr.name.c_str());
        ImGui::SetNextItemWidth(LabelW() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputText("##aname", buf, sizeof(buf))) tr.name = buf;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##level", &tr.volume, 0.0f, 2.0f, "%.2f");
        Prop("chain");
        ImGui::SetNextItemWidth(tr.fx > AFX_NONE ? ColW(2) : -1);
        ImGui::Combo("##afx", &tr.fx, kAfxNames, AFX_COUNT);
        if (tr.fx > AFX_NONE) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##afxmix", &tr.fxMix, 0.0f, 1.0f, "%.2f");
        }
        Prop("");
        float third = ColW(3);
        ImGui::Checkbox("mute", &tr.mute);
        ImGui::SameLine(ImGui::GetCursorPosX() + third);
        ImGui::BeginDisabled(g_songLoading.load());
        if (ImGui::Button("load audio", ImVec2(third, 0))) {
            auto f = PickFiles(false, L"Audio",
                               L"*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus;*.wma");
            if (!f.empty()) StartSongLoad(f[0], t, g_playhead.load());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(g_atracks.size() < 2);
        if (ImGui::Button("remove", ImVec2(-1, 0))) {
            { MixGuard lock; g_atracks.erase(g_atracks.begin() + t); }
            if (g_selTrack == -2 && g_selAT == t) g_sel = -1;
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        // Texture blocks: cut, trim, move and splice like any block on this track.
        // Pick what it is first; the block panel changes it afterwards.
        Prop("add");
        float halfT = ColW(2);
        if (ImGui::Button("noise bed...", ImVec2(halfT, 0))) ImGui::OpenPopup("##pickbed");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("a texture on this track, at the playhead");
        ImGui::SameLine();
        if (ImGui::Button("chain region...", ImVec2(-1, 0))) ImGui::OpenPopup("##pickregion");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("a chain over another track, from the playhead");
        if (ImGui::BeginPopup("##pickbed")) {
            for (int k = 0; k < TEX_COUNT; k++)
                if (ImGui::MenuItem(kTexNames[k])) AddGenBlock(t, 1, k);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##pickregion")) {
            for (int k = AFX_NONE + 1; k < AFX_COUNT; k++)
                if (ImGui::MenuItem(kAfxNames[k])) AddGenBlock(t, 2, k);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (g_atracks.empty()) ImGui::TextDisabled("(none)");
    Prop("");
    float halfN = ColW(2);
    if (ImGui::Button("new noise bed track...", ImVec2(halfN, 0))) ImGui::OpenPopup("##newbed");
    ImGui::SameLine();
    if (ImGui::Button("new chain track...", ImVec2(-1, 0))) ImGui::OpenPopup("##newregion");
    if (ImGui::BeginPopup("##newbed")) {
        for (int k = 0; k < TEX_COUNT; k++)
            if (ImGui::MenuItem(kTexNames[k])) {
                int nt = NewAudioTrack();
                g_atracks[nt]->name = kTexNames[k];
                AddGenBlock(nt, 1, k);
            }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##newregion")) {
        for (int k = AFX_NONE + 1; k < AFX_COUNT; k++)
            if (ImGui::MenuItem(kAfxNames[k])) {
                int nt = NewAudioTrack();
                g_atracks[nt]->name = kAfxNames[k];
                AddGenBlock(nt, 2, k);
            }
        ImGui::EndPopup();
    }
}

static void AddGenBlock(int track, int gen, int kind) {
    if (track < 0 || track >= (int)g_atracks.size()) return;
    UndoCapture();
    auto s = std::make_unique<Song>();
    s->gen = gen;
    s->loaded = true;
    s->duration = GEN_MAX;
    s->trimStart = 0.0;
    s->trimEnd = 10.0;
    s->offset = g_playhead.load();
    if (gen == 1) s->tex = std::clamp(kind, 0, TEX_COUNT - 1);
    else          s->fx = std::clamp(kind, AFX_NONE + 1, AFX_COUNT - 1);
    s->fxMix = 1.0f;
    s->label = gen == 1 ? "noise bed" : "chain region";
    if (gen == 2)                              // runs over the first other track to hand
        for (int i = 0; i < (int)g_atracks.size(); i++)
            if (i != track) { s->target = i; break; }
    int uid = s->uid;
    { MixGuard lock; g_atracks[track]->blocks.push_back(std::move(s)); }
    SelSet(uid);
    SelPrimaryTo(uid);
}

static void DrawProjectorPanel() {
    Prop("film look");
    ImGui::Checkbox("preview", &g_projLive);
    ImGui::SameLine();
    ImGui::Checkbox("render", &g_projOn);
    Prop("intensity");
    ImGui::SliderFloat("##pint", &g_projIntensity, 0.0f, 1.0f, "%.2f");
    Prop("margin");
    ImGui::SliderFloat("##pmar", &g_projMargin, 0.50f, 1.0f, "%.3f");
    SegRow("plate fit", &g_projFit, PROJ_FIT_ITEMS);
    Prop("grain");
    ImGui::SliderFloat("##pgr", &g_projGrain, 0.0f, 0.6f, "%.3f");
    Prop("grain fps");
    ImGui::TextDisabled("%d - follows the film", g_fps);
    Prop("gate");
    ImGui::Checkbox("ragged", &g_projGate);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_projGate);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##pgi", &g_projGateInset, 0.0f, 60.0f, "inset %.1f px");
    ImGui::EndDisabled();
    // The plate aspect. With a point on the aspect track in force this edits that
    // point, so the value is time-bound; with none it is the plain global.
    Prop("aspect");
    float par = PlateAspect();
    bool onPoint = AspectAt(g_playhead.load()) >= 0;
    if (ImGui::SliderFloat("##par", &par, 0.0f, 3.0f, par < 0.01f ? "source" : "%.3f"))
        SetPlateAspect(par);
    Prop("");
    if (ImGui::Button("16:9")) SetPlateAspect(16.0f / 9.0f); ImGui::SameLine();
    if (ImGui::Button("9:16")) SetPlateAspect(9.0f / 16.0f); ImGui::SameLine();
    if (ImGui::Button("4:3"))  SetPlateAspect(4.0f / 3.0f);  ImGui::SameLine();
    if (ImGui::Button("1:1"))  SetPlateAspect(1.0f);
    Prop("");
    if (ImGui::Button("set point here", ImVec2(-1, 0))) {
        AddAspectPoint(g_playhead.load());
        g_aspectsVisible = true;
        g_intakeStatus = "aspect point added";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("holds this aspect from the playhead until the next point");
    ImGui::TextDisabled(onPoint ? "editing the point in force" : "no point here - global");
    Prop("wall");
    ImGui::ColorEdit3("##pwall", g_projWall, ImGuiColorEditFlags_NoInputs);
    static bool oWear = false;
    if (!Reel("wear", &oWear)) { ImGui::Dummy(ImVec2(1, 2)); return; }
    Prop("scratch");
    ImGui::SliderFloat("##fxscr", &g_fxScratch, 0.0f, 3.0f, "%.2f");
    Prop("dust");
    ImGui::SliderFloat("##fxdust", &g_fxDust, 0.0f, 3.0f, "%.2f");
    Prop("hair");
    ImGui::SliderFloat("##fxhair", &g_fxHair, 0.0f, 3.0f, "%.2f");
    Prop("flicker");
    ImGui::SliderFloat("##fxflk", &g_fxFlicker, 0.0f, 3.0f, "%.2f");
    Prop("weave");
    ImGui::SliderFloat("##fxwv", &g_fxWeave, 0.0f, 4.0f, "%.2f");
    Prop("ripple");
    ImGui::SliderFloat("##fxrip", &g_fxRipple, 0.0f, 4.0f, "%.2f");
    Prop("aberration");
    ImGui::SliderFloat("##fxab", &g_fxAber, 0.0f, 4.0f, "%.2f");
    Prop("halation");
    ImGui::SliderFloat("##fxhal", &g_fxHalation, 0.0f, 3.0f, "%.2f");
    Prop("vignette");
    ImGui::SliderFloat("##fxvig", &g_fxVignette, 0.0f, 2.0f, "%.2f");
    Prop("");
    if (ImGui::Button("reset wear", ImVec2(-1, 0))) {
        g_fxWeave = g_fxRipple = g_fxAber = g_fxHalation = 1.0f;
        g_fxFlicker = g_fxDust = g_fxHair = g_fxScratch = g_fxVignette = 1.0f;
    }

    Prop("time offset");
    ImGui::SliderFloat("##pto", &g_projTimeOffset, 0.0f, 30.0f, "%.2f s");
    Prop("shader crf");
    ImGui::SliderInt("##pcrf", &g_projCrf, 10, 28, "%d");
    Prop("python");
    ImGui::InputText("##ppy", g_pythonExe, sizeof(g_pythonExe));

    if (g_gpuFailed)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.5f, 1), "shader failed to compile - %s",
                           g_gpuError.c_str());
}

// ---------------------------------------------------------------- project i/o
//
// A .slidecut file is plain UTF-8 text: one [section] per track / clip / block,
// key=value lines inside it. Media is referenced by path — the project holds the
// edit, never the footage.

static std::wstring g_projectPath;         // "" until saved once
static std::string  g_projectStatus;
static std::atomic<bool> g_projectLoading(false);
static bool   g_projectDirty = false;
static size_t g_autoHash = 0;              // last text the autosaver wrote
static size_t g_savedHash = 0;             // last text an explicit save wrote
static std::future<int> g_songLoadFut;

static std::string EscVal(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}
static std::string UnescVal(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        o += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    }
    return o;
}

struct KV {
    std::vector<std::pair<std::string, std::string>> v;
    const std::string* find(const char* k) const {
        for (auto& p : v) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string str(const char* k, const char* d = "") const {
        const std::string* p = find(k); return p ? *p : std::string(d);
    }
    double num(const char* k, double d = 0) const {
        const std::string* p = find(k); return p ? atof(p->c_str()) : d;
    }
    int i(const char* k, int d = 0) const {
        const std::string* p = find(k); return p ? atoi(p->c_str()) : d;
    }
    bool b(const char* k, bool d = false) const { return i(k, d ? 1 : 0) != 0; }
    bool has(const char* k) const { return find(k) != nullptr; }
};

static void Put(std::string& out, const char* k, const std::string& v) {
    out += k; out += '='; out += EscVal(v); out += "\r\n";
}
static void PutW(std::string& out, const char* k, const std::wstring& v) {
    Put(out, k, Narrow(v));
}
static void PutN(std::string& out, const char* k, double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", v);
    out += k; out += '='; out += buf; out += "\r\n";
}
static void PutI(std::string& out, const char* k, int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    out += k; out += '='; out += buf; out += "\r\n";
}

static void WriteSong(std::string& o, const Song& b, int track, int seq = 0) {
    o += "[song]\r\n";
    PutI(o, "seq", seq);
    PutI(o, "track", track);
    PutW(o, "path", b.path);
    Put(o, "label", b.label);
    PutN(o, "offset", b.offset);
    PutN(o, "trimStart", b.trimStart);
    PutN(o, "trimEnd", b.trimEnd);
    PutI(o, "reversed", b.reversed);
    PutI(o, "group", b.group);
    PutI(o, "fx", b.fx);
    PutN(o, "fxMix", b.fxMix);
    PutI(o, "gen", b.gen);
    PutI(o, "target", b.target);
    PutI(o, "tex", b.tex);
    PutN(o, "volume", b.volume);
    PutN(o, "fadeIn", b.fadeIn);
    PutN(o, "fadeOut", b.fadeOut);
}
static void WriteClip(std::string& o, const Clip& c, int track, int seq = 0) {
    o += "[clip]\r\n";
    PutI(o, "seq", seq);
    PutI(o, "track", track);
    PutI(o, "nest", c.nest);
    PutI(o, "skip", c.skip);
    PutI(o, "lanchor", c.lanchor);
    PutI(o, "look", c.look);
    PutI(o, "look43", c.look43);
    PutN(o, "gBright", c.grade.bright);
    PutN(o, "gContrast", c.grade.contrast);
    PutN(o, "gSat", c.grade.sat);
    PutN(o, "gTemp", c.grade.temp);
    PutI(o, "gMono", c.grade.mono);
    PutI(o, "kind", (int)c.kind);
    PutW(o, "path", c.path);
    Put(o, "label", c.label);
    PutN(o, "duration", c.duration);
    PutI(o, "srcW", c.srcW);
    PutI(o, "srcH", c.srcH);
    PutN(o, "trimIn", c.trimIn);
    PutN(o, "xfade", c.xfade);
    PutI(o, "reversed", c.reversed);
    PutI(o, "group", c.group);
    PutI(o, "useAudio", c.useAudio);
    PutN(o, "volume", c.volume);
    Put(o, "text", c.text);
    PutN(o, "textScale", c.textScale);
    PutN(o, "start", c.start);
    PutI(o, "lblend", c.lblend);
    PutN(o, "lopacity", c.lopacity);
    PutI(o, "lfit", c.lfit);
    PutI(o, "ovlOn", c.ovlOn);
    Put(o, "ovlText", c.ovlText);
    PutN(o, "ovlScale", c.ovlScale);
    PutN(o, "ovlX", c.ovlX);
    PutN(o, "ovlY", c.ovlY);
    PutN(o, "ovlR", c.ovlCol[0]);
    PutN(o, "ovlG", c.ovlCol[1]);
    PutN(o, "ovlB", c.ovlCol[2]);
    PutN(o, "ovlAlpha", c.ovlAlpha);
    PutI(o, "ovlShadow", c.ovlShadow);
    PutI(o, "dxOn", c.dxOn);
    PutW(o, "dxPath", c.dxPath);
    PutI(o, "dxBlend", c.dxBlend);
    PutN(o, "dxAmount", c.dxAmount);
    PutN(o, "dxTrimIn", c.dxTrimIn);
}

#include "edit_workspace_storage.h"

static std::string ProjectToText(bool undoMode = false, bool includeBranches = true) {
    SyncLibrary();
    CommitLevel();                          // put the working copy back first
    std::string navPath;
    std::string o = "slidecut 1\r\n";
    o += "[settings]\r\n";
    PutI(o, "fps", g_fps);
    PutI(o, "fpsAuto", g_fpsAuto);
    PutI(o, "preset", g_preset);
    PutI(o, "customW", g_customW);
    PutI(o, "customH", g_customH);
    PutI(o, "lfitAll", g_fit);
    PutI(o, "vcodec", g_vcodec);
    PutI(o, "speed", g_speed);
    PutI(o, "rateMode", g_rateMode);
    PutI(o, "crf", g_crf);
    PutN(o, "targetMbps", g_targetMbps);
    PutI(o, "container", g_container);
    PutI(o, "abrIdx", g_abrIdx);
    PutI(o, "loudnorm", g_loudnorm);
    PutI(o, "faststart", g_faststart);
    PutI(o, "keepGrain", g_keepGrain);
    PutN(o, "fadeIn", g_fadeIn);
    PutN(o, "fadeOut", g_fadeOut);
    PutN(o, "rangeIn", g_rangeIn);
    PutN(o, "rangeOut", g_rangeOut);
    PutI(o, "preview", g_preview);
    PutW(o, "namePrefix", g_namePrefix);
    PutW(o, "lastExportDir", g_lastExportDir);
    PutN(o, "sceneThresh", g_sceneThresh);
    PutN(o, "sceneMinLen", g_sceneMinLen);
    if (!undoMode) {
        PutN(o, "playhead", g_playhead.load());
        PutN(o, "pps", g_tl.pps);
        PutN(o, "scrollSec", g_tl.scrollSec);
    }
    // projector
    PutI(o, "projOn", g_projOn);
    PutI(o, "projLive", g_projLive);
    PutN(o, "projMargin", g_projMargin);
    PutN(o, "projIntensity", g_projIntensity);
    PutN(o, "projGrain", g_projGrain);
    PutN(o, "projGrainFps", g_projGrainFps);
    PutI(o, "projFit", g_projFit);
    PutI(o, "projGate", g_projGate);
    PutN(o, "projGateInset", g_projGateInset);
    PutI(o, "aspectsVisible", g_aspectsVisible);
    PutN(o, "projPlateAr", g_projPlateAr);
    PutN(o, "projWallR", g_projWall[0]);
    PutN(o, "projWallG", g_projWall[1]);
    PutN(o, "projWallB", g_projWall[2]);
    PutN(o, "projTimeOffset", g_projTimeOffset);
    PutN(o, "baseRowH", g_baseH);
    PutN(o, "filmBright", g_grade.bright);
    PutN(o, "filmContrast", g_grade.contrast);
    PutN(o, "filmSat", g_grade.sat);
    PutN(o, "filmTemp", g_grade.temp);
    PutI(o, "filmMono", g_grade.mono);
    PutN(o, "fxWeave", g_fxWeave);
    PutN(o, "fxRipple", g_fxRipple);
    PutN(o, "fxAber", g_fxAber);
    PutN(o, "fxHalation", g_fxHalation);
    PutN(o, "fxFlicker", g_fxFlicker);
    PutN(o, "fxDust", g_fxDust);
    PutN(o, "fxHair", g_fxHair);
    PutN(o, "fxScratch", g_fxScratch);
    PutN(o, "fxVignette", g_fxVignette);
    PutI(o, "projCrf", g_projCrf);
    Put(o, "pythonExe", g_pythonExe);

    // Every level of the film, root first. The working copy goes home before the
    // walk so there is exactly one place each clip lives.
    for (size_t i = 0; i < g_nav.size(); i++) {
        char b[16];
        snprintf(b, sizeof(b), i ? ",%d" : "%d", g_nav[i]);
        navPath += b;
    }
    Put(o, "nav", navPath);

    for (auto& q : g_seqs) {
        o += "[seq]\r\n";
        PutI(o, "id", q->id);
        Put(o, "name", q->name);
        if (!undoMode) PutN(o, "playhead", q->playhead);
        for (auto& t : q->over) {
            o += "[vtrack]\r\n";
            PutI(o, "seq", q->id);
            Put(o, "name", t->name);
            PutI(o, "visible", t->visible);
            PutN(o, "height", t->height);
        }
        for (auto& t : q->atracks) {
            o += "[atrack]\r\n";
            PutI(o, "seq", q->id);
            Put(o, "name", t->name);
            PutN(o, "volume", t->volume);
            PutI(o, "mute", t->mute);
            PutI(o, "fx", t->fx);
            PutN(o, "fxMix", t->fxMix);
            PutN(o, "height", t->height);
        }
        for (auto& c : q->clips) WriteClip(o, *c, -1, q->id);
        for (int t = 0; t < (int)q->over.size(); t++)
            for (auto& c : q->over[t]->clips) WriteClip(o, *c, t, q->id);
        for (int t = 0; t < (int)q->atracks.size(); t++) {
            for (auto& b : q->atracks[t]->blocks) {
                WriteSong(o, *b, t, q->id);
            }
        }
    }
    for (auto& a : g_aspects) {
        o += "[aspect]\r\n";
        PutN(o, "offset", a->offset);
        PutN(o, "aspect", a->aspect);
    }
    WriteWorkspace(o, includeBranches);
    LoadLevel(CurSeqId(), false);
    return o;
}

static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(f);
    return ok && wr == data.size();
}

// Defined with the rest of the vault, below: a save wants to shelf the version
// it is about to replace.
static bool VaultPut(const std::string& text, const wchar_t* tag, bool evenIfSame = false);

static bool SaveProjectTo(const std::wstring& path) {
    {   // the version being replaced goes on the shelf before it is gone
        std::string prev = ReadTextFile(path);
        if (!prev.empty()) VaultPut(prev, L"presave", true);
    }
    if (!WriteWholeFile(path, ProjectToText())) {
        g_projectStatus = "could not write the project file";
        return false;
    }
    g_projectPath = path;
    g_projectDirty = false;
    g_savedHash = std::hash<std::string>{}(ProjectToText());
    g_projectStatus = "Saved " + Narrow(BaseName(path));
    DeleteFileW((path + L".autosave").c_str());
    return true;
}

// ---- loading

// ---- undo keeps what is already loaded
// A restore rebuilds the project from text. With g_keepMedia set, ClearProject
// parks decoded stills and sound blocks here, and keeps the video sources and their
// proxy frames, so the restored clips pick them straight back up instead of
// decoding everything again. Whatever the step no longer uses is freed afterwards.
static bool g_keepMedia = false;
static std::multimap<std::wstring, std::unique_ptr<Song>> g_songStash;
static std::multimap<std::wstring, std::pair<ID3D11ShaderResourceView*, float>> g_texStash;

static void FlushMediaStash() {
    for (auto& t : g_texStash) RetireTexture(t.second.first);
    g_texStash.clear();
    g_songStash.clear();
}

static void ClearProject() {
    ClearWorkspacePreviews();
    g_library.clear();
    g_editMarkers.clear();
    g_branches.clear();
    g_activeBranch = -1;
    g_audition = false;
    bool wasPlaying = g_playing.exchange(false);
    if (g_songLoadFut.valid()) g_songLoadFut.wait();    // let the last restore land
    if (!g_keepMedia) Sleep(20);
    {
        std::vector<Clip*> all;
        auto sweep = [&](std::vector<std::unique_ptr<Clip>>& v) {
            for (auto& c : v) all.push_back(c.get());
        };
        sweep(g_clips);
        for (auto& t : g_over) sweep(t->clips);
        for (auto& q : g_seqs) {             // every folded level owns textures too
            sweep(q->clips);
            for (auto& t : q->over) sweep(t->clips);
        }
        for (Clip* c : all) {
            if (g_keepMedia && c->kind == Clip::Image && c->tex && !c->pending.valid()) {
                g_texStash.emplace(c->path, std::make_pair(c->tex, c->texAspect));
                c->tex = nullptr;
            } else if (c->kind != Clip::Video) RetireTexture(c->tex);
            ClearDoubleExposure(*c);
        }
    }
    g_clips.clear();
    g_over.clear();
    g_aspects.clear();
    {
        MixGuard lock;
        if (g_keepMedia) {
            auto park = [](std::vector<std::unique_ptr<AudioTrack>>& ts) {
                for (auto& t : ts)
                    for (auto& b : t->blocks)
                        if (b && b->loaded) { std::wstring p = b->path; g_songStash.emplace(p, std::move(b)); }
            };
            park(g_atracks);
            for (auto& q : g_seqs) park(q->atracks);
        }
        g_atracks.clear(); g_seqs.clear();
    }
    g_baseOff = false;                      // monitor state, never part of a project
    g_nav.assign(1, 0);
    g_seqNext = 1;
    EnsureRootSeq();
    if (!g_keepMedia) {                     // an undo keeps sources and their proxy frames
        for (auto& v : g_videoSources) ReleaseProxyCache(*v);
        g_videoSources.clear();
    }
    g_sel = -1; g_selTrack = -1; g_selAT = -1;
    g_playhead.store(0.0);
    g_playing.store(wasPlaying && false);
    g_export.message.clear();
    g_intakeStatus.clear();
    g_sceneJob.status.clear();
}

static void NewProject() {
    ClearProject();
    g_projectPath.clear();
    g_projectDirty = false;
    g_projectStatus = "New project";
}

static Clip* MakeClipFromKV(const KV& kv) {
    auto c = std::make_unique<Clip>();
    c->kind = (Clip::Kind)kv.i("kind", 0);
    c->path = Widen(kv.str("path"));
    c->label = kv.str("label");
    c->duration = kv.num("duration", 3.0);
    c->srcW = kv.i("srcW");
    c->srcH = kv.i("srcH");
    c->trimIn = kv.num("trimIn");
    c->xfade = fmax(0.0, kv.num("xfade"));
    c->reversed = kv.b("reversed");
    c->group = kv.i("group", 0);
    c->nest = kv.i("nest", 0);
    c->skip = kv.b("skip");
    c->lanchor = std::clamp(kv.i("lanchor", LANCHOR_CENTER), 0, 8);
    c->look = std::clamp(kv.i("look"), 0, (int)LOOK_CCTV_CRT);
    c->look43 = kv.b("look43");
    c->grade.bright = (float)kv.num("gBright", 0.0);
    c->grade.contrast = (float)kv.num("gContrast", 1.0);
    c->grade.sat = (float)kv.num("gSat", 1.0);
    c->grade.temp = (float)kv.num("gTemp", 0.0);
    c->grade.mono = kv.b("gMono");
    if (c->group >= g_groupNext) g_groupNext = c->group + 1;
    c->useAudio = kv.b("useAudio", true);
    c->volume = (float)kv.num("volume", 1.0);
    c->text = kv.str("text");
    c->textScale = (float)kv.num("textScale", 0.13);
    c->start = kv.num("start");
    c->lblend = kv.i("lblend");
    c->lopacity = (float)kv.num("lopacity", 1.0);
    c->lfit = kv.i("lfit", kv.b("lfill") ? LFIT_FILL : -1);   // -1: fill in from g_fit
    c->ovlOn = kv.b("ovlOn");
    c->ovlText = kv.str("ovlText");
    c->ovlScale = (float)kv.num("ovlScale", 0.07);
    c->ovlX = (float)kv.num("ovlX", 0.5);
    c->ovlY = (float)kv.num("ovlY", 0.5);
    c->ovlCol[0] = (float)kv.num("ovlR", 1.0);
    c->ovlCol[1] = (float)kv.num("ovlG", 1.0);
    c->ovlCol[2] = (float)kv.num("ovlB", 1.0);
    c->ovlAlpha = (float)kv.num("ovlAlpha", 1.0);
    c->ovlShadow = kv.b("ovlShadow", true);

    if (c->kind == Clip::Video && !c->path.empty()) c->vid = GetVideoSource(c->path);
    else if (c->kind == Clip::Image && !c->path.empty()) {
        auto st = g_texStash.find(c->path);     // an undo: the still is already on the GPU
        if (st != g_texStash.end()) {
            c->tex = st->second.first;
            c->texAspect = st->second.second;
            g_texStash.erase(st);
        } else {
            c->pending = std::async(std::launch::async, DecodeImage, c->path);
        }
    }

    if (kv.b("dxOn") && !kv.str("dxPath").empty()) {
        SetDoubleExposure(*c, Widen(kv.str("dxPath")));   // resets the knobs, so set after
        c->dxBlend = kv.i("dxBlend");
        c->dxAmount = (float)kv.num("dxAmount", 0.5);
        c->dxTrimIn = kv.num("dxTrimIn");
    }
    return c.release();
}

static void ApplySettings(const KV& kv, double savedPh, float savedPps, float savedScroll) {
    g_fps = kv.i("fps", g_fps);
    g_fpsAuto = kv.b("fpsAuto", false);
    g_preset = kv.i("preset", g_preset);
    g_customW = kv.i("customW", g_customW);
    g_customH = kv.i("customH", g_customH);
    // "fit" is the old global, in FitMode terms; "lfitAll" is this one, in LFIT terms.
    g_fit = kv.i("lfitAll", kv.has("fit") ? LfitFromOldFit(kv.i("fit")) : g_fit);
    g_vcodec = kv.i("vcodec", g_vcodec);
    g_speed = kv.i("speed", g_speed);
    g_rateMode = kv.i("rateMode", g_rateMode);
    g_crf = kv.i("crf", g_crf);
    g_targetMbps = (float)kv.num("targetMbps", g_targetMbps);
    g_container = kv.i("container", g_container);
    g_abrIdx = kv.i("abrIdx", g_abrIdx);
    g_loudnorm = kv.b("loudnorm");
    g_faststart = kv.b("faststart", true);
    g_keepGrain = kv.b("keepGrain");
    g_fadeIn = (float)kv.num("fadeIn", g_fadeIn);
    g_fadeOut = (float)kv.num("fadeOut", g_fadeOut);
    g_rangeIn = kv.num("rangeIn", -1);
    g_rangeOut = kv.num("rangeOut", -1);
    g_preview = kv.i("preview", g_preview);
    g_namePrefix = Widen(kv.str("namePrefix", Narrow(g_namePrefix).c_str()));
    g_lastExportDir = Widen(kv.str("lastExportDir"));
    g_sceneThresh = (float)kv.num("sceneThresh", g_sceneThresh);
    g_sceneMinLen = kv.num("sceneMinLen", g_sceneMinLen);
    g_playhead.store(kv.num("playhead", savedPh));
    g_tl.pps = (float)kv.num("pps", savedPps);
    g_tl.scrollSec = (float)kv.num("scrollSec", savedScroll);
    g_projOn = kv.b("projOn");
    g_projLive = kv.b("projLive");
    g_projMargin = (float)kv.num("projMargin", g_projMargin);
    g_projIntensity = (float)kv.num("projIntensity", g_projIntensity);
    g_projGrain = (float)kv.num("projGrain", g_projGrain);
    g_projGrainFps = (float)kv.num("projGrainFps", g_projGrainFps);
    g_projFit = kv.i("projFit", g_projFit);
    g_projGate = kv.b("projGate", true);
    g_projGateInset = (float)kv.num("projGateInset");
    g_aspectsVisible = kv.b("aspectsVisible", true);
    g_projPlateAr = (float)kv.num("projPlateAr");
    g_projWall[0] = (float)kv.num("projWallR");
    g_projWall[1] = (float)kv.num("projWallG");
    g_projWall[2] = (float)kv.num("projWallB");
    g_projTimeOffset = (float)kv.num("projTimeOffset");
    g_baseH = (float)kv.num("baseRowH", 58.0);
    g_grade.bright = (float)kv.num("filmBright", 0.0);
    g_grade.contrast = (float)kv.num("filmContrast", 1.0);
    g_grade.sat = (float)kv.num("filmSat", 1.0);
    g_grade.temp = (float)kv.num("filmTemp", 0.0);
    g_grade.mono = kv.b("filmMono");
    // Projects written before the wear knobs existed have no keys: default to 1.
    g_fxWeave    = (float)kv.num("fxWeave", 1.0);
    g_fxRipple   = (float)kv.num("fxRipple", 1.0);
    g_fxAber     = (float)kv.num("fxAber", 1.0);
    g_fxHalation = (float)kv.num("fxHalation", 1.0);
    g_fxFlicker  = (float)kv.num("fxFlicker", 1.0);
    g_fxDust     = (float)kv.num("fxDust", 1.0);
    g_fxHair     = (float)kv.num("fxHair", 1.0);
    g_fxScratch  = (float)kv.num("fxScratch", 1.0);
    g_fxVignette = (float)kv.num("fxVignette", 1.0);
    g_projCrf = kv.i("projCrf", g_projCrf);
snprintf(g_pythonExe, sizeof(g_pythonExe), "%s", kv.str("pythonExe", "python").c_str());
}

static bool LoadProjectFromText(const std::string& text, bool syncAudio = false) {
    if (text.compare(0, 8, "slidecut") != 0) {
        g_projectStatus = "not a SlideCut project file";
        return false;
    }
    double savedPh = g_playhead.load();
    float savedPps = g_tl.pps;
    float savedScroll = g_tl.scrollSec;
    ClearProject();

    struct SongReq { std::wstring path; std::string label; int track; int seq;
                     double offset, trimStart, trimEnd; bool reversed; int group;
                     int fx; float fxMix; int gen; int target; int tex;
                     float fadeIn, fadeOut, volume; };
    std::vector<SongReq> songs;

    std::string section;
    std::string navPath;
    KV kv;
    // Everything lands in g_seqs, keyed by the seq= it carries; the level you were
    // standing on is pulled into the working copy once the whole file is read.
    // Files written before sequences existed have no seq= at all, so they default
    // to 0 and load as the root reel.
    auto seqFor = [&](int id) -> Sequence* {
        Sequence* q = FindSeq(id);
        if (q) return q;
        auto n = std::make_unique<Sequence>();
        n->id = id;
        n->name = "seq " + std::to_string(id);
        q = n.get();
        g_seqs.push_back(std::move(n));
        if (id >= g_seqNext) g_seqNext = id + 1;
        return q;
    };
    auto commit = [&]() {
        if (ReadWorkspaceSection(section, kv)) {}
        else if (section == "settings") { ApplySettings(kv, savedPh, savedPps, savedScroll); navPath = kv.str("nav"); }
        else if (section == "seq") {
            Sequence* q = seqFor(kv.i("id", 0));
            q->name = kv.str("name", q->name.c_str());
            q->playhead = kv.num("playhead");
        } else if (section == "vtrack") {
            auto t = std::make_unique<VideoTrack>();
            t->name = kv.str("name", "Video");
            t->visible = kv.b("visible", true);
            t->height = (float)kv.num("height", 58.0);
            seqFor(kv.i("seq", 0))->over.push_back(std::move(t));
        } else if (section == "atrack") {
            auto t = std::make_unique<AudioTrack>();
            t->name = kv.str("name", "Audio");
            t->volume = (float)kv.num("volume", 1.0);
            t->mute = kv.b("mute");
            t->fx = kv.i("fx", AFX_NONE);
            if (t->fx < 0 || t->fx >= AFX_COUNT) t->fx = AFX_NONE;
            t->fxMix = (float)kv.num("fxMix", 1.0);
            t->height = (float)kv.num("height", 44.0);
            MixGuard lock;
            seqFor(kv.i("seq", 0))->atracks.push_back(std::move(t));
        } else if (section == "clip") {
            int tr = kv.i("track", -1);
            Sequence* q = seqFor(kv.i("seq", 0));
            std::unique_ptr<Clip> c(MakeClipFromKV(kv));
            if (c->nest >= g_seqNext) g_seqNext = c->nest + 1;
            if (tr < 0 || tr >= (int)q->over.size()) q->clips.push_back(std::move(c));
            else q->over[tr]->clips.push_back(std::move(c));
        } else if (section == "aspect") {
            auto a = std::make_unique<AspectPoint>();
            a->offset = kv.num("offset");
            a->aspect = (float)kv.num("aspect", 1.777);
            g_aspects.push_back(std::move(a));
        } else if (section == "song") {
            songs.push_back({ Widen(kv.str("path")), kv.str("label"), kv.i("track", 0),
                              kv.i("seq", 0),
                              kv.num("offset"), kv.num("trimStart"), kv.num("trimEnd"), kv.b("reversed"), kv.i("group"),
                              std::clamp(kv.i("fx", AFX_NONE), 0, AFX_COUNT - 1),
                              (float)kv.num("fxMix", 1.0),
                              std::clamp(kv.i("gen", 0), 0, 2), kv.i("target", -1),
                              std::clamp(kv.i("tex", 0), 0, TEX_COUNT - 1),
                              (float)std::max(0.0, kv.num("fadeIn")),
                              (float)std::max(0.0, kv.num("fadeOut")),
                              (float)std::clamp(kv.num("volume", 1.0), 0.0, 2.0) });
            g_groupNext = std::max(g_groupNext, kv.i("group") + 1);
        }
        kv.v.clear();
    };

    size_t b = text.find('\n');             // skip the header line
    for (size_t p = (b == std::string::npos ? text.size() : b + 1); p < text.size(); ) {
        size_t e = text.find('\n', p);
        std::string line = text.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            commit();
            size_t close = line.find(']');
            section = line.substr(1, close == std::string::npos ? close : close - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv.v.push_back({ line.substr(0, eq), UnescVal(line.substr(eq + 1)) });
    }
    commit();

    // The nav path names the level to stand on. Anything stale in it is dropped, so
    // a hand-edited or half-written path still opens on the root reel.
    EnsureRootSeq();
    g_nav.assign(1, 0);
    for (size_t i = 0; i < navPath.size(); ) {
        size_t comma = navPath.find(',', i);
        std::string tok = navPath.substr(i, comma == std::string::npos ? comma : comma - i);
        i = comma == std::string::npos ? navPath.size() : comma + 1;
        int id = atoi(tok.c_str());
        if (id == 0 || !FindSeq(id)) continue;
        bool dup = false;
        for (int n : g_nav) if (n == id) dup = true;
        if (!dup) g_nav.push_back(id);
    }
    LoadLevel(g_nav.back());
    RefreshNestDurations();
    SortAspects();

    // Clips saved before each one carried its own off-canvas fill get the project's
    // old global, which is what they were exported with.
    {
        auto fill = [](std::vector<std::unique_ptr<Clip>>& v) {
            for (auto& c : v) if (c->lfit < 0) c->lfit = g_fit;
        };
        fill(g_clips);
        for (auto& t : g_over) fill(t->clips);
        for (auto& q : g_seqs) {
            fill(q->clips);
            for (auto& t : q->over) fill(t->clips);
        }
    }

    // Rebuild the sound blocks. A block whose track index runs past the list grows
    // the list rather than being dropped, which is how a restore used to lose audio.
    auto restoreSongs = [](const std::vector<SongReq>& reqs) {
        int ok = 0;
        for (auto& r : reqs) {
            // An undo hands back the samples the timeline already held; only a
            // block that wasn't there a moment ago is decoded.
            std::unique_ptr<Song> sp;
            if (r.gen) {                         // a texture block has nothing to decode
                sp = std::make_unique<Song>();
                sp->loaded = true;
                sp->duration = GEN_MAX;
                sp->label = r.gen == 1 ? "noise bed" : "chain region";
            } else {
                auto st = g_songStash.find(r.path);
                if (st != g_songStash.end()) { sp = std::move(st->second); g_songStash.erase(st); }
                else sp = DecodeSongFile(r.path);
            }
            if (!sp || r.track < 0) continue;
            sp->fx = r.fx;
            sp->fxMix = r.fxMix;
            sp->gen = r.gen;
            sp->target = r.target;
            sp->tex = r.tex;
            sp->fadeIn = r.fadeIn;
            sp->fadeOut = r.fadeOut;
            sp->volume = r.volume;
            sp->offset = r.offset;
            sp->trimStart = r.trimStart;
            sp->trimEnd = r.trimEnd > r.trimStart ? r.trimEnd : sp->duration;
            sp->reversed = r.reversed;
            sp->group = r.group;
            if (!r.label.empty()) sp->label = r.label;
            MixGuard lock;
            auto* tracks = ATracksOf(r.seq);
            if (!tracks) continue;
            while (r.track >= (int)tracks->size()) {
                auto t = std::make_unique<AudioTrack>();
                t->name = "sound " + std::to_string(tracks->size() + 1);
                tracks->push_back(std::move(t));
            }
            (*tracks)[r.track]->blocks.push_back(std::move(sp));
            ok++;
        }
        return ok;
    };

    // Opening a project decodes off the UI thread, so the window stays alive. Undo
    // is one step and must not half-apply, so it restores in place: the blocks it
    // wants were on the timeline a moment ago, so the song cache is already warm.
    if (!songs.empty() && syncAudio) {
        restoreSongs(songs);
    } else if (!songs.empty()) {
        g_projectLoading.store(true);
        g_songLoadFut = std::async(std::launch::async, [songs, restoreSongs] {
            int ok = restoreSongs(songs);
            g_projectLoading.store(false);
            return ok;
        });
    }
    g_projectDirty = false;
    return true;
}

static bool g_undoBusy = false;            // set while a restore is running

// Flatten, hand the plain film to ffmpeg, then put the project back exactly as it
// was. StartExport builds the whole command and spawns before it returns, so the
// restore cannot race it.
static void ExportFlattened(const std::wstring& outPath) {
    bool nested = false;
    auto hasNest = [&](const std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& c : v) if (c->kind == Clip::Nest) return true;
        return false;
    };
    nested = hasNest(g_clips);
    for (auto& t : g_over) if (hasNest(t->clips)) nested = true;
    if (!nested) { StartExport(outPath); return; }

    std::string snap = ProjectToText();
    g_undoBusy = true;
    FlattenNestsHere();
    StartExport(outPath);
    // Sync: an async song restore would still be pushing blocks into the project
    // after this returns, and the caller may tear the whole thing down again. The
    // blocks were on the timeline a moment ago, so the decode cache is warm.
    LoadProjectFromText(snap, true);
    g_undoBusy = false;
}

// Bringing a copy back is itself an edit worth keeping, so the state being replaced
// is shelved first. Nothing on the shelf is ever consumed: restoring only reads.
static bool RestoreFromVault(const std::wstring& path);

static bool LoadProjectFile(const std::wstring& path) {
    std::string text = ReadTextFile(path);
    if (text.empty()) { g_projectStatus = "could not read the project file"; return false; }
    if (!LoadProjectFromText(text)) return false;
    g_projectPath = path;
    g_projectStatus = "Opened " + Narrow(BaseName(path));
    return true;
}

// ---- autosave: %LOCALAPPDATA%\SlideCut\autosave.slidecut, rewritten on a timer
// and on exit, reloaded at startup so a crash costs nothing.

static std::wstring AutosavePath() {
    wchar_t buf[MAX_PATH];
    std::wstring dir;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) dir = buf;
    else { GetTempPathW(MAX_PATH, buf); dir = buf; }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"SlideCut";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\autosave.slidecut";
}


// ---- crash dumps: %LOCALAPPDATA%\SlideCut\crash\
// A crash in the preview is a pointer that outlived what it pointed at, and the
// only way to see which one is a dump written at the moment it happens. Every
// unhandled fault lands here as a .dmp next to a .txt naming the fault, so a
// debugger can be pointed at the exact frame that died.

static std::wstring CrashDir() {
    std::wstring dir = AutosavePath();
    dir.resize(dir.rfind(L'\\'));           // strip \autosave.slidecut
    dir += L"\\crash";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring CrashStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// What the fault was, in plain words, so the .txt is readable without a debugger.
static const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
    case 0xE06D7363:                      return "unhandled C++ exception";
    default:                              return "unknown";
    }
}

static std::wstring g_lastCrashDump;        // shown in the panel on the next run

static void WriteCrashNote(const std::wstring& stem, EXCEPTION_POINTERS* ep,
                           const char* what) {
    std::string note = "SlideCut crash\r\n";
    note += "when: " + Narrow(CrashStamp()) + "\r\n";
    note += std::string("what: ") + what + "\r\n";
    if (ep && ep->ExceptionRecord) {
        char line[256];
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        snprintf(line, sizeof(line), "code: 0x%08lX (%s)\r\n",
                 (unsigned long)code, ExceptionName(code));
        note += line;
        snprintf(line, sizeof(line), "address: %p\r\n", ep->ExceptionRecord->ExceptionAddress);
        note += line;
        // On an access violation the record says whether it was a read or a write
        // and which address was touched - usually enough to name the bug alone.
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
            snprintf(line, sizeof(line), "operation: %s\r\naddress touched: 0x%llX\r\n",
                     kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                     (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
            note += line;
        }
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod) && mod) {
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(mod, path, MAX_PATH);
            note += "module: " + Narrow(path) + "\r\n";
            snprintf(line, sizeof(line), "module base: %p\r\noffset: 0x%llX\r\n",
                     (void*)mod,
                     (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress -
                                          (char*)mod));
            note += line;
        }
    }
    // The frames that led there, as module+offset. addr2line over SlideCut.exe turns
    // our own offsets back into file:line, which is the whole point of keeping them.
    if (ep && ep->ContextRecord) {
        note += "stack:\r\n";
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(proc, nullptr, TRUE);
        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 fr = {};
        fr.AddrPC.Offset    = ctx.Rip; fr.AddrPC.Mode    = AddrModeFlat;
        fr.AddrFrame.Offset = ctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
        fr.AddrStack.Offset = ctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 40; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &fr, &ctx,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            DWORD64 pc = fr.AddrPC.Offset;
            if (!pc) break;
            HMODULE m = nullptr;
            wchar_t mp[MAX_PATH] = L"";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)(uintptr_t)pc, &m) && m)
                GetModuleFileNameW(m, mp, MAX_PATH);
            std::string base = mp[0] ? Narrow(BaseName(mp)) : "?";
            unsigned char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
            auto* si = (SYMBOL_INFO*)symbuf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            const char* nm = SymFromAddr(proc, pc, &disp, si) ? si->Name : "";
            char fl[512];
            snprintf(fl, sizeof(fl), "  %2d %s+0x%llX %s\r\n", i, base.c_str(),
                     (unsigned long long)(m ? pc - (DWORD64)(uintptr_t)m : pc), nm);
            note += fl;
        }
    }

    char line[128];
    snprintf(line, sizeof(line), "thread: %lu\r\n", (unsigned long)GetCurrentThreadId());
    note += line;
    note += "playing: " + std::string(g_playing.load() ? "yes" : "no") + "\r\n";
    snprintf(line, sizeof(line), "playhead: %.3f s\r\n", g_playhead.load());
    note += line;
    snprintf(line, sizeof(line), "clips: %d  layers: %d  sound tracks: %d\r\n",
             (int)g_clips.size(), (int)g_over.size(), (int)g_atracks.size());
    note += line;
    WriteWholeFile(stem + L".txt", note);
}

static bool WriteMiniDump(const std::wstring& stem, EXCEPTION_POINTERS* ep) {
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return false;
    typedef BOOL (WINAPI *WriteFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                   PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION,
                                   PMINIDUMP_CALLBACK_INFORMATION);
    WriteFn writeDump = (WriteFn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!writeDump) { FreeLibrary(dbg); return false; }

    std::wstring path = stem + L".dmp";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { FreeLibrary(dbg); return false; }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // Stacks, handles and referenced memory: enough to see which freed object a
    // pointer landed in, without dumping the whole heap.
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                         MiniDumpWithDataSegs |
                                         MiniDumpWithHandleData |
                                         MiniDumpWithThreadInfo |
                                         MiniDumpWithUnloadedModules);
    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                        ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    FreeLibrary(dbg);
    if (!ok) DeleteFileW(path.c_str());
    return ok != FALSE;
}

// Keep the folder from growing without end: oldest dumps fall off.
static void TrimCrashDumps(size_t keep) {
    std::wstring dir = CrashDir();
    std::vector<std::wstring> stems;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { stems.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (stems.size() <= keep) return;
    std::sort(stems.begin(), stems.end());          // names are timestamps
    for (size_t i = 0; i + keep < stems.size(); i++) {
        std::wstring base = dir + L"\\" + stems[i];
        DeleteFileW(base.c_str());
        base.resize(base.size() - 4);
        DeleteFileW((base + L".txt").c_str());
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    g_playing.store(false);                 // stop the mixer touching anything else
    std::wstring stem = CrashDir() + L"\\" + CrashStamp();
    WriteCrashNote(stem, ep, "unhandled exception");
    WriteMiniDump(stem, ep);
    TrimCrashDumps(20);
    std::wstring msg = L"SlideCut hit a bug and has to close.\n\nA crash report was "
                       L"written to:\n" + stem + L".dmp\n\nYour work is in BACKUPS.";
    MessageBoxW(nullptr, msg.c_str(), L"SlideCut crashed", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The runtime swallows some faults before the filter above ever sees them, so the
// paths that bypass it are pointed back at it.
static void TerminateHandler() {
    CrashHandler(nullptr);
    _exit(3);
}

static void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*,
                                    unsigned int, uintptr_t) {
    CrashHandler(nullptr);
    _exit(3);
}

static void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashHandler);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::set_terminate(TerminateHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    // Note the newest report, so the last run's crash is visible in the app.
    std::wstring dir = CrashDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    std::wstring newest;
    if (h != INVALID_HANDLE_VALUE) {
        do { if (std::wstring(fd.cFileName) > newest) newest = fd.cFileName; }
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!newest.empty()) g_lastCrashDump = dir + L"\\" + newest;
}

// ---- the vault: a rolling shelf of timestamped copies
// The recovery file is one file that keeps being overwritten, which is no good if
// the thing you want back is twenty minutes old. Every so often, and at every
// moment worth marking — a save, a close, a restore, a keypress — the project also
// goes onto a shelf under its own timestamp. Oldest fall off the end.
static const size_t VAULT_KEEP = 80;
static const double VAULT_EVERY = 120.0;     // seconds between unattended copies

static std::wstring VaultDir() {
    std::wstring dir = AutosavePath();
    dir.resize(dir.rfind(L'\\'));            // strip \autosave.slidecut
    dir += L"\\backups";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Name is sortable first and readable second: 20260817-053012_close_reel.slidecut
static std::wstring VaultName(const wchar_t* tag) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring stem = L"untitled";
    if (!g_projectPath.empty()) {
        stem = BaseName(g_projectPath);
        size_t dot = stem.rfind(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
    }
    for (auto& ch : stem)                    // the shelf is flat: no separators
        if (ch == L'\\' || ch == L'/' || ch == L'_' || ch == L' ') ch = L'-';
    wchar_t buf[MAX_PATH];
    swprintf(buf, MAX_PATH, L"%04d%02d%02d-%02d%02d%02d_%ls_%ls.slidecut",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             tag, stem.c_str());
    return buf;
}

static void VaultPrune() {
    std::wstring dir = VaultDir();
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.slidecut").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.size() <= VAULT_KEEP) return;
    std::sort(names.begin(), names.end());   // timestamp first, so this is oldest first
    size_t drop = names.size() - VAULT_KEEP;
    for (size_t i = 0; i < drop; i++) DeleteFileW((dir + L"\\" + names[i]).c_str());
}

static std::string g_vaultStatus;
static size_t g_vaultLastHash = 0;
static bool VaultPut(const std::string& text, const wchar_t* tag, bool evenIfSame) {
    if (text.empty()) return false;
    size_t h = std::hash<std::string>{}(text);
    if (!evenIfSame && h == g_vaultLastHash) return false;   // nothing changed since
    std::wstring path = VaultDir() + L"\\" + VaultName(tag);
    if (!WriteWholeFile(path, text)) return false;
    g_vaultLastHash = h;
    VaultPrune();
    g_vaultStatus = "kept " + Narrow(BaseName(path));
    return true;
}

// What is on the shelf, newest first. Cheap enough to rescan while the panel is
// open, so a copy made this second shows up without asking.
struct VaultItem {
    std::wstring path;
    std::string  when;                       // 17 Aug 05:30
    std::string  tag;
    size_t       bytes = 0;
};
static std::vector<VaultItem> g_vault;
static double g_vaultScanned = -1e9;

static void VaultScan(bool force = false) {
    double now = (double)GetTickCount64() / 1000.0;
    if (!force && now - g_vaultScanned < 2.0) return;
    g_vaultScanned = now;
    g_vault.clear();
    std::wstring dir = VaultDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.slidecut").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    static const char* MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.size() < 16) continue;
        VaultItem it;
        it.path = dir + L"\\" + name;
        it.bytes = fd.nFileSizeLow;
        std::string n = Narrow(name);
        int mon = atoi(n.substr(4, 2).c_str());
        char when[48];
        snprintf(when, sizeof(when), "%s %s  %s:%s",
                 n.substr(6, 2).c_str(), (mon >= 1 && mon <= 12) ? MON[mon - 1] : "???",
                 n.substr(9, 2).c_str(), n.substr(11, 2).c_str());
        it.when = when;
        size_t u1 = n.find('_'), u2 = n.find('_', u1 == std::string::npos ? 0 : u1 + 1);
        if (u1 != std::string::npos && u2 != std::string::npos)
            it.tag = n.substr(u1 + 1, u2 - u1 - 1);
        g_vault.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_vault.begin(), g_vault.end(),
              [](const VaultItem& a, const VaultItem& b) { return a.path > b.path; });
}

static bool RestoreFromVault(const std::wstring& path) {
    std::string text = ReadTextFile(path);
    if (text.empty()) { g_vaultStatus = "could not read that copy"; return false; }
    VaultPut(ProjectToText(), L"prerestore", true);
    if (!LoadProjectFromText(text)) return false;
    g_projectDirty = true;                  // it is not the saved file any more
    g_vaultStatus = "restored " + Narrow(BaseName(path));
    g_projectStatus = g_vaultStatus;
    VaultScan(true);
    return true;
}

// The shelf, newest at the top. Every row is one click from being the project
// again, and taking that click shelves what you have now, so there is no way to
// lose work by looking through it.
// The film's own grade, on top of whatever each shot is already doing.
static void DrawColorPanel() {
    // The picked shot comes first: its own colour, pushed onto the rest of the
    // selection only. The film grade underneath is labelled as reaching every shot,
    // so it can't be mistaken for the shot's.
    ImGui::SeparatorText("this shot");
    if (Clip* c = SelectedClip()) {
        int nSel = SelCount();
        if (nSel > 1) ImGui::TextDisabled("editing %d selected shots", nSel);
        else          ImGui::TextDisabled("%s", c->label.c_str());
        if (int gch = GradeControls(c->grade, "pick"))
            ForEachOtherSelected(*c, [&](Clip& o) { ApplyGradeFields(o.grade, c->grade, gch); });
    } else {
        ImGui::TextDisabled("pick a shot to grade it on its own");
    }

    ImGui::SeparatorText("whole film");
    ImGui::TextDisabled("over every shot at once");
    GradeControls(g_grade, "film");
    Prop("");
    if (ImGui::Button("apply to every shot", ImVec2(-1, 0))) {
        // Push the film grade down onto the shots and neutralise it, so what you
        // tuned globally becomes each shot's own starting point.
        auto push = [&](std::vector<std::unique_ptr<Clip>>& v) {
            for (auto& c : v) c->grade = g_grade;
        };
        push(g_clips);
        for (auto& t : g_over) push(t->clips);
        for (auto& q : g_seqs) {
            push(q->clips);
            for (auto& t : q->over) push(t->clips);
        }
        g_grade = Grade();
        g_intakeStatus = "grade pushed onto every shot";
    }
}

// The beat section: what the cuts are being pulled onto, how far they may travel
// and where they land relative to the transient.
static void DrawBeatPanel() {
    std::vector<double> grid;
    BeatGrid(grid);
    ImGui::TextDisabled("%d beat%s under the film, from %d unmuted sound track%s",
                        (int)grid.size(), grid.size() == 1 ? "" : "s",
                        (int)std::count_if(g_atracks.begin(), g_atracks.end(),
                                           [](const std::unique_ptr<AudioTrack>& t){ return !t->mute; }),
                        g_atracks.size() == 1 ? "" : "s");
    Prop("window");
    ImGui::DragFloat("##qwin", &g_quantWin, 0.005f, 0.01f, 2.0f, "%.3f s",
                     ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("an edge further than this from a beat is left where it is");

    Prop("offset");
    ImGui::DragFloat("##qoff", &g_quantOff, 0.002f, -0.5f, 0.5f, "%+.3f s",
                     ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("where the cut lands against the beat: negative is ahead of it, positive behind");
    Prop("");
    {
        float f = (float)MinClipDur();                 // one frame at the export rate
        float w = ColW(5);
        if (ImGui::Button("-2f", ImVec2(w, 0))) g_quantOff -= 2 * f;
        ImGui::SameLine();
        if (ImGui::Button("-1f", ImVec2(w, 0))) g_quantOff -= f;
        ImGui::SameLine();
        if (ImGui::Button("on", ImVec2(w, 0)))  g_quantOff = 0.0f;
        ImGui::SameLine();
        if (ImGui::Button("+1f", ImVec2(w, 0))) g_quantOff += f;
        ImGui::SameLine();
        if (ImGui::Button("+2f", ImVec2(-1, 0))) g_quantOff += 2 * f;
        if (g_quantOff < -0.5f) g_quantOff = -0.5f;
        if (g_quantOff >  0.5f) g_quantOff =  0.5f;
    }
    ImGui::TextDisabled(fabsf(g_quantOff) < 1e-4f ? "cuts land on the beat"
                        : (g_quantOff < 0 ? "cuts land %.0f ms ahead of the beat"
                                          : "cuts land %.0f ms behind the beat"),
                        fabsf(g_quantOff) * 1000.0f);

    Prop("");
    int nSel = SelCount();
    ImGui::BeginDisabled(nSel == 0 || grid.empty());
    if (ImGui::Button(nSel > 1 ? "quantize the selected shots" : "quantize this shot", ImVec2(-1, 0))) {
        std::string err;
        int n = QuantizeSelectionToBeats(g_quantWin, g_quantOff, err);
        char buf[80];
        if (n) snprintf(buf, sizeof(buf), "%d edge%s pulled onto the beat", n, n == 1 ? "" : "s");
        else   snprintf(buf, sizeof(buf), "%s", err.c_str());
        g_intakeStatus = buf;
    }
    ImGui::EndDisabled();
    if (nSel == 0) ImGui::TextDisabled("pick a shot on the timeline first");
    else if (grid.empty()) ImGui::TextDisabled("no beats: load a sound track, unmute it, let it decode");
    ImGui::TextDisabled("both edges of every selected shot move; the base track ripples, "
                        "a layer shot just slides");
}

static void DrawVaultPanel() {
    VaultScan();
    float w = ImGui::GetContentRegionAvail().x;
    float bw = ImGui::CalcTextSize("restore").x + ImGui::GetStyle().FramePadding.x * 2 + 6;

    if (ImGui::Button("SNAPSHOT NOW", ImVec2(w * 0.62f, 0))) {
        if (VaultPut(ProjectToText(), L"mark", true)) VaultScan(true);
        else g_vaultStatus = "nothing to keep yet";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ctrl+b");
    ImGui::SameLine();
    if (ImGui::Button("FOLDER", ImVec2(-1, 0))) {
        std::wstring dir = VaultDir();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (g_vault.empty()) {
        ImGui::TextDisabled("no copies yet");
        return;
    }
    ImGui::TextDisabled("%d kept, oldest drops at %d", (int)g_vault.size(), (int)VAULT_KEEP);

    int shown = 0;
    for (auto& it : g_vault) {
        if (shown++ >= 14) break;            // the rest are in the folder
        ImGui::PushID(shown);
        char row[96];
        snprintf(row, sizeof(row), "%s   %s   %d KB", it.when.c_str(), it.tag.c_str(),
                 (int)((it.bytes + 1023) / 1024));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row);
        ImGui::SameLine(w - bw);
        if (ImGui::Button("restore", ImVec2(bw, 0))) g_vaultRestore = it.path;
        ImGui::PopID();
    }
    if (g_vault.size() > 14)
        ImGui::TextDisabled("+ %d older in the folder", (int)g_vault.size() - 14);
    if (!g_vaultStatus.empty()) ImGui::TextDisabled("%s", g_vaultStatus.c_str());
}

static std::wstring StartupMarkPath() {
    std::wstring p = AutosavePath();
    p.resize(p.rfind(L'\\'));
    return p + L"\\startup.mark";
}
static bool StartupMarkExists() {
    return GetFileAttributesW(StartupMarkPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void StartupMarkSet()   { WriteWholeFile(StartupMarkPath(), "loading"); }
static void StartupMarkClear() { DeleteFileW(StartupMarkPath().c_str()); }

static bool ProjectIsEmpty() {
    if (!g_clips.empty()) return false;
    for (auto& t : g_over) if (!t->clips.empty()) return false;
    for (auto& t : g_atracks) if (!t->blocks.empty()) return false;
    for (auto& q : g_seqs) {              // every folded level counts as content
        if (!q->clips.empty()) return false;
        for (auto& t : q->over) if (!t->clips.empty()) return false;
        for (auto& t : q->atracks) if (!t->blocks.empty()) return false;
    }
    return true;
}

// ---- undo / redo
//
// The project already round-trips through text, so history is just a stack of
// those strings. A snapshot is taken once per frame when the text has changed and
// no gesture is running, which makes one drag one undo step.

static std::vector<std::string> g_undo, g_redo;
static std::string g_undoBase;             // the state the stack was built from
static const size_t UNDO_MAX = 120;

static void UndoCapture() {
    if (g_undoBusy || g_hcLive) return;                // a preview is not a step
    if (g_projectLoading.load() || g_playing.load(std::memory_order_relaxed)) return;
    if (g_tl.drag != TimelineState::None) return;      // mid-gesture, wait for the drop
    std::string now = ProjectToText(true);
    if (g_undoBase.empty()) { g_undoBase = now; return; }
    if (now == g_undoBase) return;
    g_undo.push_back(g_undoBase);
    if (g_undo.size() > UNDO_MAX) g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_undoBase = now;
}

static void UndoStep(bool redo) {
    std::vector<std::string>& from = redo ? g_redo : g_undo;
    if (from.empty()) return;
    std::string text = from.back();
    from.pop_back();
    (redo ? g_undo : g_redo).push_back(g_undoBase);
    g_undoBusy = true;
    g_keepMedia = true;                    // reuse what's loaded; decode only what's new
    LoadProjectFromText(text, true);       // audio comes back with the step
    g_keepMedia = false;
    FlushMediaStash();
    g_undoBusy = false;
    g_undoBase = text;
    g_selUids.clear();
    g_projectStatus = redo ? "redo" : "undo";
}

// ---- hypercut preview
//
// The chop is a real edit, so previewing one means keeping the project text and
// putting it back before every re-chop. Undo stays out of the way until commit,
// which leaves exactly one step between the film before and the film after.

static void HcRestore() {
    if (g_hcSnap.empty()) return;
    g_undoBusy = true;
    LoadProjectFromText(g_hcSnap, true);
    g_undoBusy = false;
}

static void HyperCutTick() {
    int req = g_hcReq;
    g_hcReq = 0;
    if (!req) return;
    if (req == 1) {
        std::string err;
        if (!HcGather(err)) { g_hcMsg = err; g_intakeStatus = err; return; }
        g_hcSnap = ProjectToText(true);
        g_hcLive = true;
        HcChop();
    } else if (req == 2 && g_hcLive) {
        HcRestore();
        HcChop();
    } else if (req == 3 && g_hcLive) {
        char buf[96];
        if (g_hcByFrames)
            snprintf(buf, sizeof(buf), "hypercut kept - %d frame beat, %d slices", g_hcFrames, g_hcSlices);
        else
            snprintf(buf, sizeof(buf), "hypercut kept - %.3f s beat, %d slices", g_hcSlice, g_hcSlices);
        g_intakeStatus = buf;
        g_hcLive = false;
        g_hcSnap.clear();
        g_hcParts.clear();
        g_hcMsg.clear();
        return;
    } else if (req == 4 && g_hcLive) {
        HcRestore();
        g_hcLive = false;
        g_hcSnap.clear();
        g_hcParts.clear();
        g_hcMsg.clear();
        g_intakeStatus = "hypercut dropped";
        return;
    }
    if (g_hcLive) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d slices of %.3f s over %.2f s", g_hcSlices, HcSlice(), g_hcB - g_hcA);
        g_hcMsg = buf;
    }
}


// ---- clipboard: the same [clip] sections the project file uses
static std::string g_clipboard;

static void CopySelection() {
    std::string o;
    int n = 0;
    for (auto& c : g_clips)
        if (SelHas(c->uid)) { WriteClip(o, *c, -1); n++; }
    for (int t = 0; t < (int)g_over.size(); t++)
        for (auto& c : g_over[t]->clips)
            if (SelHas(c->uid)) { WriteClip(o, *c, t); n++; }
    for (int t = 0; t < (int)g_atracks.size(); t++)
        for (auto& b : g_atracks[t]->blocks)
            if (SelHas(b->uid)) { WriteSong(o, *b, t); n++; }
            
    if (!n) {                                // nothing multi-picked: use the primary
        if (g_selTrack == -2) {
            if (g_selAT >= 0 && g_selAT < (int)g_atracks.size() && g_sel >= 0 && g_sel < (int)g_atracks[g_selAT]->blocks.size()) {
                WriteSong(o, *g_atracks[g_selAT]->blocks[g_sel], g_selAT);
                n = 1;
            }
        } else {
            Clip* c = SelectedClip();
            if (c) {
                WriteClip(o, *c, g_selTrack);
                n = 1;
            }
        }
    }
    if (!n) return;
    g_clipboard = o;
    char buf[64];
    snprintf(buf, sizeof(buf), "copied %d item%s", n, n == 1 ? "" : "s");
    g_projectStatus = buf;
}

// ---- handing a cut to the rest of Windows
// Throw away everything that is not in `uids` and pull what is left back to zero, so
// what gets rendered is the picked stretch on its own. The caller works on a project
// snapshot and puts the real one back the moment the render has been spawned.
static void TrimToSelection(const std::vector<int>& uids) {
    auto keep = [&](int uid) {
        for (int u : uids) if (u == uid) return true;
        return false;
    };

    double t0 = 0;
    bool   has = false;
    double acc = 0;
    for (auto& c : g_clips) {                      // the base track packs: walk it up
        if (keep(c->uid) && !has) { t0 = acc; has = true; }
        acc += c->duration;
    }
    for (auto& t : g_over)
        for (auto& c : t->clips)
            if (keep(c->uid) && (!has || c->start < t0)) { t0 = c->start; has = true; }
    for (auto& t : g_atracks)
        for (auto& b : t->blocks)
            if (keep(b->uid) && (!has || b->offset < t0)) { t0 = b->offset; has = true; }
    if (!has) return;

    MixGuard lock;                                 // the mixer must not be in these lists
    for (size_t i = 0; i < g_clips.size(); )
        if (keep(g_clips[i]->uid)) i++; else g_clips.erase(g_clips.begin() + i);
    for (auto& t : g_over)
        for (size_t i = 0; i < t->clips.size(); ) {
            if (keep(t->clips[i]->uid)) { t->clips[i]->start -= t0; i++; }
            else t->clips.erase(t->clips.begin() + i);
        }
    for (auto& t : g_atracks)
        for (size_t i = 0; i < t->blocks.size(); ) {
            if (keep(t->blocks[i]->uid)) { t->blocks[i]->offset -= t0; i++; }
            else t->blocks.erase(t->blocks.begin() + i);
        }

    // Nothing on the base cut: the pick was layers and sound only. Those still make a
    // film, so lay a blank card under them long enough to hold the whole stretch.
    if (g_clips.empty()) {
        double end = 0;
        for (auto& t : g_over)
            for (auto& c : t->clips) end = fmax(end, c->start + c->duration);
        for (auto& t : g_atracks)
            for (auto& b : t->blocks) end = fmax(end, b->offset + (b->trimEnd - b->trimStart));
        if (end <= 0.001) return;
        auto blank = std::make_unique<Clip>();
        blank->kind = Clip::Text;              // an empty card is a plain black frame
        blank->label = "blank";
        blank->duration = end;
        g_clips.push_back(std::move(blank));
    }
}

// Cut the whole project down to [r0, r1) and pull it back to zero, so a render of what
// is left is that stretch of the real film. Shots crossing an edge are trimmed, not
// dropped: the source in-point moves with the head (or the tail, when reversed), and
// sound blocks the same way. Runs on a snapshot, after nests on the base are opened.
static void TrimToRange(double r0, double r1) {
    std::vector<BaseSpan> lay;
    BaseLayout(lay);
    MixGuard lock;                                 // the mixer must not be in these lists
    auto cut = [](Clip& c, double head, double tail) {
        if (c.kind == Clip::Video) c.trimIn += c.reversed ? tail : head;
        // A sequence has no in-point: it renders whole and the export trims the picture,
        // which is already the right way round, so head is head even when reversed.
        if (c.kind == Clip::Nest) { c.nestHead += head; c.nestTail += tail; }
        if (c.dxOn && c.dxIsVideo) c.dxTrimIn += head;
        c.duration -= head + tail;
    };

    std::vector<std::unique_ptr<Clip>> kept;
    for (size_t i = 0; i < g_clips.size(); i++) {
        Clip& c = *g_clips[i];
        if (c.skip || lay[i].end <= r0 + 1e-6 || lay[i].start >= r1 - 1e-6) continue;
        double head = fmax(0.0, r0 - lay[i].start);
        // The range opens inside a dissolve: the shot fading out has lost the same head,
        // so the overlap left between the two is only what remains of it.
        if (head > 0 && !kept.empty()) kept.back()->xfade = fmax(0.0, kept.back()->xfade - head);
        cut(c, head, fmax(0.0, lay[i].end - r1));
        kept.push_back(std::move(g_clips[i]));
    }
    if (!kept.empty()) kept.back()->xfade = 0;     // nothing after it to dissolve into
    g_clips = std::move(kept);

    // The film is as long as the range: a base cut that ends early gets a blank card,
    // so layers and sound past its end still make it in.
    double len = r1 - r0, baseEnd = TotalDuration();
    if (baseEnd < len - 1e-3) {
        auto blank = std::make_unique<Clip>();
        blank->kind = Clip::Text;                  // an empty card is a plain black frame
        blank->label = "blank";
        blank->duration = len - baseEnd;
        g_clips.push_back(std::move(blank));
    }

    for (auto& t : g_over)
        for (size_t i = 0; i < t->clips.size(); ) {
            Clip& c = *t->clips[i];
            double s = c.start, e = c.start + c.duration;
            if (e <= r0 + 1e-6 || s >= r1 - 1e-6) { t->clips.erase(t->clips.begin() + i); continue; }
            cut(c, fmax(0.0, r0 - s), fmax(0.0, e - r1));
            c.start = fmax(s, r0) - r0;
            i++;
        }

    for (auto& t : g_atracks)
        for (size_t i = 0; i < t->blocks.size(); ) {
            Song& b = *t->blocks[i];
            double s = b.offset, e = s + (b.trimEnd - b.trimStart);
            if (e <= r0 + 1e-6 || s >= r1 - 1e-6) { t->blocks.erase(t->blocks.begin() + i); continue; }
            double head = fmax(0.0, r0 - s), tail = fmax(0.0, e - r1);
            if (b.reversed) { b.trimEnd -= head; b.trimStart += tail; }
            else            { b.trimStart += head; b.trimEnd -= tail; }
            b.offset = fmax(s, r0) - r0;
            i++;
        }

    // Aspect points onto the range's clock. A point before the range pins to 0 and
    // keeps its order, so the one in force when the range opens still holds from its
    // first frame.
    for (auto& a : g_aspects) a->offset = fmax(0.0, a->offset - r0);
}

// The delivery name with the range in it, so a test render is never mistaken for the film.
static std::wstring RangeOutputName() {
    std::wstring n = DefaultOutputName();
    wchar_t tag[64];
    swprintf(tag, 64, L"_range_%dm%02d-%dm%02d",
             (int)g_rangeIn / 60, (int)g_rangeIn % 60, (int)g_rangeOut / 60, (int)g_rangeOut % 60);
    size_t dot = n.rfind(L'.');
    n.insert(dot == std::wstring::npos ? n.size() : dot, tag);
    return n;
}

// EXPORT RANGE: the marked stretch through the real export - same encoder, projector
// pass, screens, layers and sound - then the project put back exactly as it was.
static void ExportRange(const std::wstring& outPath) {
    if (!HasRange()) return;
    std::string snap = ProjectToText();
    g_undoBusy = true;
    FlattenNestsHere();
    TrimToRange(g_rangeIn, g_rangeOut);
    StartExport(outPath);                          // spawns before it returns
    LoadProjectFromText(snap, true);               // sync, as in ExportFlattened
    g_undoBusy = false;
}

// Ctrl+Shift+C: put the picked shots on the Windows clipboard as a file, so anything
// that takes a dropped file takes a paste from here. A single untouched still goes
// over as its own source file; everything else is rendered to a temp film first and
// the export progress bar runs until it lands.
static void CopyToOSClipboard() {
    if (g_export.active) {
        if (g_export.toClipboard) { CancelClipboardRender(); return; }
        g_projectStatus = "a render is already running";
        return;
    }

    std::vector<int> uids;
    for (auto& c : g_clips) if (SelHas(c->uid)) uids.push_back(c->uid);
    for (auto& t : g_over) for (auto& c : t->clips) if (SelHas(c->uid)) uids.push_back(c->uid);
    for (auto& t : g_atracks) for (auto& b : t->blocks) if (SelHas(b->uid)) uids.push_back(b->uid);
    if (uids.empty()) {                            // nothing multi-picked: the primary
        if (g_selTrack == -2) {
            if (g_selAT >= 0 && g_selAT < (int)g_atracks.size() &&
                g_sel >= 0 && g_sel < (int)g_atracks[g_selAT]->blocks.size())
                uids.push_back(g_atracks[g_selAT]->blocks[g_sel]->uid);
        } else if (Clip* c = SelectedClip()) uids.push_back(c->uid);
    }
    if (uids.empty()) { g_projectStatus = "pick the shots you want to copy first"; return; }

    if (uids.size() == 1) {                        // a plain still needs no render at all
        Clip* only = nullptr;
        for (auto& c : g_clips) if (c->uid == uids[0]) only = c.get();
        for (auto& t : g_over) for (auto& c : t->clips) if (c->uid == uids[0]) only = c.get();
        if (only && only->kind == Clip::Image && !only->path.empty() &&
            !only->grade.On() && !only->ovlOn && !only->dxOn) {
            g_export.failed = !SetClipboardFile(only->path);
            g_export.message = g_export.failed ? "The clipboard would not take that file."
                                               : "Copied " + only->label + " to the clipboard";
            return;
        }
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[80];
    swprintf(name, 80, L"slidecut_clip_%u.mp4", (unsigned)GetTickCount());
    std::wstring out = std::wstring(tmp) + name;

    std::string snap = ProjectToText();
    g_undoBusy = true;
    TrimToSelection(uids);
    bool empty = g_clips.empty();
    if (!empty) {
        // A copy is a hand-off, not a master: h264 at a sane CRF encodes in a fraction
        // of the time an x265 delivery takes, and every app that eats a pasted file
        // eats h264. The delivery settings come straight back from the snapshot.
        // A copy is a hand-off, not a master: h264 at a sane CRF encodes in a fraction
        // of the time an x265 delivery takes, every app that eats a pasted file eats
        // h264, and the projector shader pass is a whole second render on top.
        int oc = g_vcodec, os = g_speed, orm = g_rateMode, ocrf = g_crf, oct = g_container;
        bool op = g_projOn;
        g_projOn = false;                          // the shader pass is a whole second render
        g_vcodec = VC_X264;
        g_speed = 4;                               // "fast"
        g_rateMode = RM_CRF;
        g_crf = 20;
        g_container = CT_MP4;
        ExportFlattened(out);                      // spawns before it returns
        g_vcodec = oc; g_speed = os; g_rateMode = orm; g_crf = ocrf; g_container = oct;
        g_projOn = op;
    }
    LoadProjectFromText(snap, true);               // sync, for the same reason
    g_undoBusy = false;

    if (empty) { g_projectStatus = "nothing in that pick to render"; return; }
    if (g_export.active) {
        g_export.toClipboard = true;
        g_export.message.clear();
        g_clipTemps.push_back(out);
    }
}

// Paste lands at the playhead: base clips go in at the cut under it, layer clips
// keep their spacing relative to the earliest one copied.
static void PasteClipboard() {
    if (g_clipboard.empty()) return;
    std::vector<KV> blocks;
    KV kv;
    bool inClip = false;
    std::istringstream in(g_clipboard);
    std::string line;
    auto flush = [&]() { if (inClip && !kv.v.empty()) blocks.push_back(kv); kv.v.clear(); };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') { flush(); inClip = line.substr(0, 6) == "[clip]" || line.substr(0, 6) == "[song]"; continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos)
            kv.v.push_back({ line.substr(0, eq), UnescVal(line.substr(eq + 1)) });
    }
    flush();
    if (blocks.empty()) return;

    double ph = g_playhead.load();
    double first = 1e18;
    for (auto& b : blocks) if (b.i("track", -1) >= 0) {
        double st = b.num("start");
        if (st < first) first = st;
    }
    int at = (int)g_clips.size();
    {   // base clips drop in at the cut nearest the playhead
        double acc = 0;
        std::vector<BaseSpan> layP;
        BaseLayout(layP);
        for (int i = 0; i < (int)g_clips.size(); i++) {
            if (ph < (layP[i].start + layP[i].end) * 0.5) { at = i; break; }
            (void)acc;
        }
    }
    int made = 0, lastTrack = -1, lastIdx = -1, skipped = 0;
    for (auto& b : blocks) {
        bool isSong = b.find("offset") != nullptr;
        if (isSong) {
            auto s = std::make_unique<Song>();
            s->path = Widen(b.str("path"));
            s->label = b.str("label");
            s->offset = b.num("offset");
            s->trimStart = b.num("trimStart");
            s->trimEnd = b.num("trimEnd");
            s->reversed = b.i("reversed");
            s->group = b.i("group");
            s->fx = std::clamp(b.i("fx", AFX_NONE), 0, AFX_COUNT - 1);
            s->fxMix = (float)b.num("fxMix", 1.0);
            s->gen = std::clamp(b.i("gen", 0), 0, 2);
            s->target = b.i("target", -1);
            s->tex = std::clamp(b.i("tex", 0), 0, TEX_COUNT - 1);
            s->fadeIn = (float)std::max(0.0, b.num("fadeIn"));
            s->fadeOut = (float)std::max(0.0, b.num("fadeOut"));
            s->volume = (float)std::clamp(b.num("volume", 1.0), 0.0, 2.0);
            if (s->gen) { s->loaded = true; s->duration = GEN_MAX; }

            int tr = b.i("track", -1);
            if (tr < 0) tr = 0;
            while (tr >= (int)g_atracks.size()) NewAudioTrack();
            
            s->offset = ph + (first < 1e17 ? s->offset - first : 0.0);
            
            MixGuard lock;
            g_atracks[tr]->blocks.push_back(std::move(s));
            made++;
            continue;
        }
        
        if (b.i("kind", 0) == (int)Clip::Nest) { skipped++; continue; }
        std::unique_ptr<Clip> c(MakeClipFromKV(b));
        if (!c) continue;
        int tr = b.i("track", -1);
        if (tr < 0) {
            lastIdx = at;
            g_clips.insert(g_clips.begin() + at, std::move(c));
            at++;
            lastTrack = -1;
        } else {
            while (tr >= (int)g_over.size()) NewOverlayTrack();
            c->start = ph + (first < 1e17 ? c->start - first : 0.0);
            if (c->start < 0) c->start = 0;
            g_over[tr]->clips.push_back(std::move(c));
            lastTrack = tr;
            lastIdx = (int)g_over[tr]->clips.size() - 1;
        }
        made++;
    }
    if (made) {
        g_sel = lastIdx;
        g_selTrack = lastTrack;
        g_selUids.clear();
        char buf[64];
        if (skipped)
        snprintf(buf, sizeof(buf), "pasted %d clip%s — %d folded sequence%s skipped",
                 made, made == 1 ? "" : "s", skipped, skipped == 1 ? "" : "s");
    else
        snprintf(buf, sizeof(buf), "pasted %d clip%s", made, made == 1 ? "" : "s");
        g_projectStatus = buf;
    }
}

// Tie the selected shots into one sequence. They keep their own trims and blend
// settings; the id only means "these travel together".
static void GroupSelection() {
    if (g_selUids.size() < 2) { g_intakeStatus = "pick two or more shots first"; return; }
    int g = g_groupNext++;
    int n = 0;
    auto tag = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& c : v) if (SelHas(c->uid)) { c->group = g; n++; }
    };
    tag(g_clips);
    for (auto& t : g_over) tag(t->clips);
    char buf[64];
    snprintf(buf, sizeof(buf), "sequence %d — %d shots", g, n);
    g_intakeStatus = buf;
}

static void UngroupSelection() {
    int n = 0;
    auto clear = [&](std::vector<std::unique_ptr<Clip>>& v) {
        for (auto& c : v) if (SelHas(c->uid) && c->group) { c->group = 0; n++; }
    };
    clear(g_clips);
    for (auto& t : g_over) clear(t->clips);
    char buf[64];
    snprintf(buf, sizeof(buf), "ungrouped %d shot%s", n, n == 1 ? "" : "s");
    g_intakeStatus = buf;
}

static double g_lastAutosave = 0;
static double g_lastVault = 0;
static const double AUTOSAVE_EVERY = 8.0;    // seconds
// Rewrites the recovery file only when the project actually changed since the
// previous pass, which also keeps the dirty marker honest without hooking edits.
static void Autosave(bool force = false) {
    if (g_projectLoading.load()) return;
    if (ProjectIsEmpty()) return;
    double now = (double)GetTickCount64() / 1000.0;
    if (!force && now - g_lastAutosave < AUTOSAVE_EVERY) return;
    g_lastAutosave = now;
    std::string text = ProjectToText();
    size_t h = std::hash<std::string>{}(text);
    g_projectDirty = h != g_savedHash;
    if (h == g_autoHash && !force) return;
    g_autoHash = h;
    WriteWholeFile(AutosavePath(), text);
    if (!g_projectPath.empty() && g_projectDirty)
        WriteWholeFile(g_projectPath + L".autosave", text);   // sidecar next to the project

    // The recovery file only ever holds the latest state. A copy also goes on the
    // shelf: rarely while you work, always on the way out.
    if (force) VaultPut(text, L"close");
    else if (now - g_lastVault >= VAULT_EVERY) { g_lastVault = now; VaultPut(text, L"auto"); }
}

static std::vector<std::wstring> PickFiles(bool multi, const wchar_t* filterName,
                                           const wchar_t* filterSpec);

static void OpenProjectDialog() {
    auto f = PickFiles(false, L"SlideCut project", L"*.slidecut");
    if (!f.empty()) LoadProjectFile(f[0]);
}

static void SaveProjectDialog(bool forceAsk) {
    if (!forceAsk && !g_projectPath.empty()) { SaveProjectTo(g_projectPath); return; }
    std::wstring def = g_projectPath.empty() ? L"project.slidecut" : BaseName(g_projectPath);
    std::wstring dir = g_projectPath.empty() ? SuggestExportDir() : DirName(g_projectPath);
    std::wstring out = PickSaveVideo(def, L"slidecut", dir, L" project");
    if (!out.empty()) SaveProjectTo(out);
}

// A video layer exists only while it holds something: the last clip leaving it takes
// the row with it. Audio tracks stay put when emptied - their name, volume, mute and
// fx are set up by hand - and go only through "remove" in the tracks panel.
static void PruneEmptyTracks() {
    if (g_tl.drag != TimelineState::None) return;
    if (g_projectLoading.load()) return;   // blocks are still arriving off-thread
    for (int i = (int)g_over.size() - 1; i >= 0; i--) {
        if (!g_over[i]->clips.empty()) continue;
        g_over.erase(g_over.begin() + i);
        if (g_selTrack == i) { g_sel = -1; g_selTrack = -1; }
        else if (g_selTrack > i) g_selTrack--;
    }
}

// ----------------------------------------------------------------- main UI

// The tools that act on the cut sit against the timeline, not up in the title bar.
static void ClipToolBar(bool doAdd) {
    ImVec2 md(ImGui::CalcTextSize("ADD MEDIA").x + ImGui::GetStyle().FramePadding.x * 2, 0);
    auto nextTool = [&] {
        float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + 3 + md.x <= right) ImGui::SameLine(0, 3);
    };
    if (ImGui::Button("ADD MEDIA", md) || doAdd) {
        auto files = PickFiles(true, L"Media",
            L"*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff;*.webp;*.gif;"
            L"*.mp4;*.mov;*.m4v;*.mkv;*.webm;*.avi;*.wmv;*.mpg;*.mpeg;*.m2ts;*.ts;"
            L"*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus;*.wma");
        if (!files.empty()) AddFiles(files);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ctrl+i");
    nextTool();
    if (ImGui::Button("TITLE CARD", md)) {
        g_textBuf[0] = 0;
        g_textScale = 0.13f;
        g_textDur = 2.0;
        g_textTarget = -1;
        g_textTargetTrack = -1;
        // This bar is inside the stage child; popup ids are per-window, so ask the
        // root window to open it rather than calling OpenPopup from in here.
        g_textOpenNew = true;
    }
    nextTool();
    ImGui::BeginDisabled(g_clips.empty());
    if (ImGui::Button("SPLICE", md)) SplitPoint(g_playhead.load());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("s");
    ImGui::EndDisabled();
    nextTool();
    {   // fold a run of shots away into a sequence you can open and work inside
        bool any = !g_selUids.empty() || (g_selTrack == -1 && g_sel >= 0);
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("FOLD", md)) g_navFold = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ctrl+f — the selected shots become one sequence");
        ImGui::EndDisabled();
    }
    nextTool();
    {   // a sequence with nothing in it yet, opened straight away
        bool nestSel = g_selTrack == -1 && g_sel >= 0 && g_sel < (int)g_clips.size() &&
                       g_clips[g_sel]->kind == Clip::Nest;
        if (nestSel) {
            if (ImGui::Button("OPEN", md)) g_navEnter = g_clips[g_sel]->nest;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("ctrl+down, or double click it");
        } else {
            if (ImGui::Button("NEW SEQ", md)) g_navNew = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("start an empty sequence at the playhead and work inside it");
        }
    }
    nextTool();
    {   // flips the selected shots so they play backwards; nothing moves
        bool any = SelectedClip() != nullptr || !g_selUids.empty();
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("REVERSE", md)) {
            int n = 0;
            auto flip = [&](std::vector<std::unique_ptr<Clip>>& v) {
                for (auto& c : v)
                    if (SelHas(c->uid)) {
                        c->reversed = !c->reversed; n++;
                    }
            };
            flip(g_clips);
            for (auto& t : g_over) flip(t->clips);
            if (!n) {
                Clip* c = SelectedClip();
                if (c) { c->reversed = !c->reversed; n = 1; }
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "reversed %d shot%s", n, n == 1 ? "" : "s");
            g_intakeStatus = buf;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("play the selected shots backwards");
        ImGui::EndDisabled();
    }
    nextTool();
    {   // mute: the shot stays where it is, the film runs past it
        bool any = SelectedClip() != nullptr || !g_selUids.empty();
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("MUTE", md)) g_muteToggle = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("m — skip the selected shots without removing them");
        ImGui::EndDisabled();
    }
    nextTool();
    {   // hush: every clip's sound off in the preview, the edit itself untouched
        bool hush = g_hushClips.load();
        if (hush) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(hush ? "UNHUSH" : "HUSH", md)) g_hushClips.store(!hush);
        if (hush) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(hush ? "shift+m — clip sound is off in the preview (music tracks still play)"
                                   : "shift+m — silence every clip's own sound while editing (export unaffected)");
    }
    nextTool();
    {   // ripple: trims and deletes close the film up, or leave a gap and move nothing
        bool off = !g_rippleOn;
        if (off) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(off ? "NO RIPPLE" : "RIPPLE", md)) g_rippleOn = !g_rippleOn;
        if (off) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(off ? "ripple is off — overwrite editing: nothing after an edit moves\n"
                                    "drag a shot anywhere and it covers what it lands on\n"
                                    "stretch an edge and it eats into the shot beside it\n"
                                    "trims and deletes leave empty film (hover it, delete to close)"
                                  : "ripple is on — trims and deletes close the film up and everything after follows");
    }

    // ---- where you are: the reel, then every sequence you stepped into. Each
    // one steps back to that level, so the depth is never a guess.
    {
        EnsureRootSeq();
        std::vector<std::string> crumbs;
        for (int id : g_nav) {
            Sequence* q = FindSeq(id);
            crumbs.push_back(q ? q->name : std::string("?"));
        }
        float need = 0;
        for (size_t i = 0; i < crumbs.size(); i++)
            need += ImGui::CalcTextSize(crumbs[i].c_str()).x +
                    ImGui::GetStyle().FramePadding.x * 2 + (i ? 14.0f : 0.0f);
        if (crumbs.size() > 1) need += ImGui::CalcTextSize("OUT").x +
                                      ImGui::GetStyle().FramePadding.x * 2 + 8.0f;
        float x = ImGui::GetWindowContentRegionMax().x - need - 4;
        float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 8;
        if (x >= used) ImGui::SameLine(x);

        if (crumbs.size() > 1) {
            if (ImGui::Button("OUT")) g_navDepth = (int)g_nav.size() - 2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("ctrl+up — back one level");
            ImGui::SameLine(0, 8);
        }
        for (size_t i = 0; i < crumbs.size(); i++) {
            if (i) { ImGui::SameLine(0, 4);
                     ImGui::TextDisabled(">");
                     ImGui::SameLine(0, 4); }
            bool here = i + 1 == crumbs.size();
            ImGui::PushID((int)i);
            if (here) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            else      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.61f, 0.59f, 1));
            if (ImGui::Button(crumbs[i].c_str()) && !here) g_navDepth = (int)i;
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
}

#include "edit_workspace_ui.h"

static void DrawApp() {
    SyncLibrary();
    PruneEmptyTracks();
    SelSync();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // Two flat rows, nothing folded away: every control is on screen and one click
    // deep. The keyboard duplicates the common ones, it never replaces them.
    double tot = TimelineEnd();
    bool songBusy = g_songLoading.load();
    ImGuiIO& kio = ImGui::GetIO();
    bool chord = kio.KeyCtrl && !kio.WantTextInput;
    bool doNew    = chord && ImGui::IsKeyPressed(ImGuiKey_N, false);
    bool doOpen   = chord && ImGui::IsKeyPressed(ImGuiKey_O, false);
    bool doSave   = chord && !kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false);
    bool doSaveAs = chord &&  kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false);
    bool doExport = chord && ImGui::IsKeyPressed(ImGuiKey_E, false);
    bool doAdd    = chord && ImGui::IsKeyPressed(ImGuiKey_I, false);
    if (chord && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        if (VaultPut(ProjectToText(), L"mark", true)) VaultScan(true);
    }

    // ---- row 1: the reel — project, sources, cutting
    if (g_titleFont) {                      // wordmark, drawn by hand so it stays small
        float px = ImGui::GetFontSize() * 1.5f;
        ImVec2 ts = g_titleFont->CalcTextSizeA(px, FLT_MAX, 0, "SLIDECUT");
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_titleFont, px,
                                            ImVec2(at.x, at.y - 1),
                                            IM_COL32(232, 228, 218, 255), "SLIDECUT");
        ImGui::Dummy(ImVec2(ts.x + 4, ImGui::GetFrameHeight()));
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("16mm");
        ImGui::SameLine(0, 14);
    }
    // One width per group, so the toolbar reads as blocks instead of a ragged line.
    ImVec2 pj(ImGui::CalcTextSize("SAVE AS").x + ImGui::GetStyle().FramePadding.x * 2, 0);
    ImVec2 md(ImGui::CalcTextSize("ADD MEDIA").x + ImGui::GetStyle().FramePadding.x * 2, 0);
    if (ImGui::Button("NEW", pj))      doNew = true;
    ImGui::SameLine(0, 3);
    if (ImGui::Button("OPEN", pj))     doOpen = true;
    ImGui::SameLine(0, 3);
    if (ImGui::Button("SAVE", pj))     doSave = true;
    ImGui::SameLine(0, 3);
    if (ImGui::Button("SAVE AS", pj))  doSaveAs = true;
    ImGui::SameLine(0, 3);
    ImGui::BeginDisabled(g_projectPath.empty());
    if (ImGui::Button("RELOAD", pj)) {      // throw away edits since the last save
        std::wstring p = g_projectPath;
        LoadProjectFile(p);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", g_projectPath.empty() ? "untitled"
                                                      : Narrow(BaseName(g_projectPath)).c_str(),
                        g_projectDirty ? " *" : "");

    if (ImGui::GetItemRectMax().x + 510 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
        ImGui::SameLine(0, 16);
    static char ytUrl[512] = "";
    ImGui::BeginDisabled(songBusy);
    ImGui::SetNextItemWidth(220);
    bool ytGo = ImGui::InputTextWithHint("##yt", "YouTube URL for a track…", ytUrl,
                                         sizeof(ytUrl), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 3);
    ytGo |= ImGui::Button("FETCH", pj);
    ImGui::EndDisabled();
    if (ytGo && ytUrl[0] && !songBusy) {
        g_songLoading.store(true);
        g_songStatus = "downloading from YouTube…";
        g_songPending = std::async(std::launch::async, [u = std::string(ytUrl)] {
            bool ok = LoadSongFromYouTube(u);
            g_songLoading.store(false);
            return ok;
        });
        ytUrl[0] = 0;
    }

    ImGui::SameLine(0, 16);
    {   // muted shots are counted apart: they are in the cut but not in the film
        int muted = 0;
        for (auto& c : g_clips) if (c->skip) muted++;
        if (muted)
            ImGui::TextDisabled("%d shots (%d muted) · %d:%05.2f",
                                (int)g_clips.size() - muted, muted,
                                (int)tot / 60, fmod(tot, 60.0));
        else
            ImGui::TextDisabled("%d shots · %d:%05.2f", (int)g_clips.size(),
                                (int)tot / 60, fmod(tot, 60.0));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "space  play / pause     s       splice\n"
            "arrows one frame         shift+  one second\n"
            "alt+arrows nudge clip    home / end\n"
            "ctrl+z / y  undo redo    ctrl+c / v  copy paste\n"
            "ctrl+a select all        esc  clear\n"
            "ctrl+g group             ctrl+shift+g ungroup\n"
            "ctrl+f fold sequence     ctrl+u unfold\n"
            "ctrl+down step in        ctrl+up  step out\n"
            "m      mute / unmute\n"
            "i / o  range in / out    alt+x  clear range\n"
            "ctrl+b snapshot to backups\n"
            "ctrl-click add to sel    shift-click range\n"
            "del    remove hovered    ctrl+i  add media\n"
            "ctrl+n / o / s / shift+s / e\n"
            "shift  fine trim   alt  no snap   ctrl+wheel  zoom");
        ImGui::EndTooltip();
    }

    // ---- row 2: the lab — preview resolution, playback, and the export button
    ImGui::TextDisabled("proxy");
    ImGui::SameLine(0, 6);
    ImGui::BeginDisabled(AnyProxyBuilding());
    if (SegW("##pv", &g_preview, "360p\0" "540p\0" "720p\0", 190.0f)) RebuildProxies();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("film look");
    ImGui::SameLine(0, 6);
    ImGui::Checkbox("preview##live", &g_projLive);
    ImGui::SameLine(0, 6);
    ImGui::Checkbox("render##proj", &g_projOn);

    {   // right-aligned: what the encoder will be told, then the export button
        int W, H;
        ResolveCanvas(&W, &H);
        const char* codec = g_vcodec == VC_X264 ? "H.264" : g_vcodec == VC_X265 ? "H.265"
                          : g_vcodec == VC_NVENC_H264 ? "H.264 NVENC" : "H.265 NVENC";
        char spec[160];
        if (g_rateMode == RM_CRF)
            snprintf(spec, sizeof(spec), "%dx%d · %dfps · %s crf %d", W, H, g_fps, codec, g_crf);
        else
            snprintf(spec, sizeof(spec), "%dx%d · %dfps · %s %.1f Mbps",
                     W, H, g_fps, codec, g_targetMbps);
        const char* exp = g_container == CT_MOV ? "EXPORT MOV"
                        : g_container == CT_MKV ? "EXPORT MKV" : "EXPORT MP4";
        const char* rexp = "EXPORT RANGE";
        float need = ImGui::CalcTextSize(spec).x + ImGui::CalcTextSize(exp).x +
                     ImGui::CalcTextSize(rexp).x + ImGui::CalcTextSize("x").x +
                     ImGui::GetStyle().FramePadding.x * 6 +
                     ImGui::GetStyle().ItemSpacing.x * 4 + 12;
        float x = ImGui::GetWindowContentRegionMax().x - need;
        if (x > ImGui::GetCursorPosX() + 12) ImGui::SameLine(x); else ImGui::SameLine();
        ImGui::TextDisabled("%s", spec);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_clips.empty() || g_export.active || !HasRange());
        if (ImGui::Button(rexp)) {
            std::wstring out = PickSaveVideo(RangeOutputName(), ContainerExt(g_container),
                                             SuggestExportDir());
            if (!out.empty()) ExportRange(out);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (HasRange())
                ImGui::SetTooltip("render %d:%05.2f - %d:%05.2f (%.2f s) with the export settings",
                                  (int)g_rangeIn / 60, fmod(g_rangeIn, 60.0),
                                  (int)g_rangeOut / 60, fmod(g_rangeOut, 60.0),
                                  g_rangeOut - g_rangeIn);
            else
                ImGui::SetTooltip("press i / o on the timeline to mark a range");
        }
        ImGui::SameLine(0, 3);
        // Always laid out, only live with a range: the row is right-aligned, so a
        // button that came and went would shove everything beside it.
        ImGui::BeginDisabled(!HasRange());
        if (ImGui::Button("x##range")) g_rangeIn = g_rangeOut = -1;
        ImGui::EndDisabled();
        if (HasRange() && ImGui::IsItemHovered()) ImGui::SetTooltip("clear the range (alt+x)");
        ImGui::SameLine();
        ImGui::BeginDisabled(g_clips.empty() || g_export.active);
        if (ImGui::Button(exp) || (doExport && !g_clips.empty() && !g_export.active)) {
            std::wstring out = PickSaveVideo(DefaultOutputName(), ContainerExt(g_container),
                                             SuggestExportDir());
            if (!out.empty()) ExportFlattened(out);
        }
        ImGui::EndDisabled();
    }

    if (doNew)    NewProject();
    if (doOpen)   OpenProjectDialog();
    if (doSave)   SaveProjectDialog(false);
    if (doSaveAs) SaveProjectDialog(true);

    // ---- one status line, shared by every background job
    {
        const char* msg = nullptr;
        if (songBusy || !g_songStatus.empty())
            msg = songBusy && g_songStatus.empty() ? "loading audio…" : g_songStatus.c_str();
        else if (!g_intakeStatus.empty())  msg = g_intakeStatus.c_str();
        else if (!g_projectStatus.empty()) msg = g_projectStatus.c_str();

        if (g_export.active) {
            // Which stage of the run this is, above the bar: screen passes, the encode,
            // then the projector look - numbered out of the stages this export has.
            {
                int total = 1 + (g_export.clipShaders ? 1 : 0) + (g_export.wantProjector ? 1 : 0);
                int at;
                std::string what;
                if (g_export.stage == 2) {
                    at = total;
                    what = "Projector shader pass";
                } else if (g_export.clipShaders && !g_export.encoding) {
                    at = 1;
                    what = g_export.shaderStep.empty() ? "Clip shaders: starting"
                                                       : g_export.shaderStep;
                } else {
                    at = g_export.clipShaders ? 2 : 1;
                    char pct[32] = "";
                    if (g_export.progress >= 0)
                        snprintf(pct, sizeof(pct), " (%d%%)", (int)(g_export.progress * 100));
                    what = std::string("Encoding with ffmpeg") + pct;
                }
                if (!g_export.stallNote.empty()) what += "  - " + g_export.stallNote;
                ImGui::TextDisabled("Stage %d/%d: %s%s%s%s", at, total, what.c_str(),
                                    g_export.toClipboard ? "  - for the clipboard" : "",
                                    g_export.encNote.empty() ? "" : "  - ",
                                    g_export.encNote.c_str());
            }
            // Stage 2 has no progress feed, so run the bar as an indeterminate sweep.
            float p = g_export.progress < 0 ? -1.0f * (float)ImGui::GetTime() : g_export.progress;
            ImGui::ProgressBar(p, ImVec2(-1, 6),
                               g_export.stage == 2 ? "projector shader pass"
                               : g_export.toClipboard ? "rendering for the clipboard" : "");
        } else if (!g_export.message.empty()) {
            ImGui::TextColored(g_export.failed ? ImVec4(0.95f, 0.42f, 0.35f, 1)
                                               : ImVec4(0.85f, 0.85f, 0.82f, 1),
                               "%s", g_export.message.c_str());
            if (!g_export.cmd.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("copy ffmpeg command"))
                    ImGui::SetClipboardText(g_export.cmd.c_str());
                if (g_export.failed && !g_export.logFile.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("open log"))
                        ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                      g_export.logFile.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
        } else if (msg) {
            ImGui::TextDisabled("%s", msg);
            ImGui::SameLine();
            if (ImGui::SmallButton("x##status")) {
                g_songStatus.clear();
                g_intakeStatus.clear();
                g_projectStatus.clear();
            }
        }
    }
    ImGui::Separator();

    // ---- stage: preview + timeline on the left, inspector on the right
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float panelW = g_panelWUser > 0 ? g_panelWUser : ImGui::GetFontSize() * 25.0f;
    if (g_panelWUser <= 0 && panelW > avail.x * 0.38f) panelW = avail.x * 0.38f;
    panelW = std::clamp(panelW, 180.0f, fmaxf(200.0f, avail.x - 320.0f));

    // Tracks grow downward, so give the timeline what it needs up to half the
    // stage — unless the splitter above the toolbar has been dragged, in which
    // case that height wins, up to leaving the preview a usable strip.
    float tlAuto = 34.0f + 42.0f + g_baseH + 3.0f + 6.0f;
    for (auto& t : g_over)    tlAuto += t->height + 3.0f;
    for (auto& t : g_atracks) tlAuto += t->height + 3.0f;
    const float SPLIT_H = 10.0f;
    float tlHeight = g_tlHeightUser > 0 ? g_tlHeightUser : tlAuto;
    float maxTl = g_tlHeightUser > 0 ? avail.y - 260.0f : avail.y * 0.43f;
    if (maxTl < 120.0f) maxTl = 120.0f;
    if (tlHeight > maxTl) tlHeight = maxTl;
    if (tlHeight < 120.0f) tlHeight = 120.0f;

    ImGui::BeginChild("##stage", ImVec2(avail.x - panelW - 8, 0), false);
    {
        ImVec2 inner = ImGui::GetContentRegionAvail();
        WorkspaceTransport();
        float toolsWidth = (ImGui::CalcTextSize("ADD MEDIA").x + ImGui::GetStyle().FramePadding.x * 2 + 3) * 8 + 65;
        float extraTools = (ceilf(toolsWidth / fmaxf(1, inner.x)) - 1) * ImGui::GetFrameHeightWithSpacing();
        float previewH = fmaxf(70, inner.y - tlHeight - 174 - SPLIT_H - extraTools);
        bool edgePreview = g_tl.drag == TimelineState::LeftEdge || g_tl.drag == TimelineState::RightEdge;
        if (g_cutView || edgePreview) DrawCutViewer(ImVec2(inner.x, previewH));
        else DrawPreviewArea(ImVec2(inner.x, previewH));
        ImGui::TextDisabled("RHYTHM   shots / quiet / markers / scenes");
        DrawRhythm();
        {   // the grab bar: drag to set the timeline height, double-click for auto
            ImVec2 sp = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##tlsplit", ImVec2(inner.x, SPLIT_H));
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive())                 // up = a taller timeline
                g_tlHeightUser = tlHeight - ImGui::GetIO().MouseDelta.y;
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                g_tlHeightUser = 0.0f;
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            float cy = sp.y + SPLIT_H * 0.5f, cx = sp.x + inner.x * 0.5f;
            ImU32 col = hot ? IM_COL32(220, 220, 220, 220) : IM_COL32(110, 110, 110, 130);
            for (int i = -2; i <= 2; i++)
                sdl->AddRectFilled(ImVec2(cx + i * 9 - 3, cy - 1),
                                   ImVec2(cx + i * 9 + 3, cy + 1), col);
            if (hot && ImGui::IsItemHovered())
                ImGui::SetTooltip("drag to resize the timeline · double-click for auto");
        }
        ClipToolBar(doAdd);
        ImGui::BeginChild("##tl", ImVec2(0, tlHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawTimeline();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    {   // drag to set the side panel width, double-click to go back to auto
        ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##panelsplit", ImVec2(8, fmaxf(1, avail.y)));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())            // left = a wider panel
            g_panelWUser = panelW - ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            g_panelWUser = 0.0f;
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        float cx = sp.x + 4, cy = sp.y + avail.y * 0.5f;
        ImU32 col = hot ? IM_COL32(220, 220, 220, 220) : IM_COL32(110, 110, 110, 130);
        for (int i = -2; i <= 2; i++)
            sdl->AddRectFilled(ImVec2(cx - 1, cy + i * 9 - 3), ImVec2(cx + 1, cy + i * 9 + 3), col);
        if (hot) ImGui::SetTooltip("drag to resize the panel · double-click for auto");
    }
    ImGui::SameLine(0, 0);
    // The tabs live outside the panel: a rail of book tabs hanging off its left
    // edge. Each rests tucked in; the live one slides out, runs under the panel
    // border and takes its colour, so tab and page read as one sheet.
    // One panel at a time, so the sidebar never becomes a long scroll. The rail
    // carries short codes to stay narrow; hovering one names it in full.
    struct PanelTab { const char* code; const char* name; };
    static const PanelTab kPanels[] = {
        {"SHT", "Shot"},   {"BEA", "Beat"},   {"TRK", "Tracks"},
        {"COL", "Colour"}, {"PRJ", "Projector"}, {"OUT", "Output"},
        {"BAK", "Backups"}, {"MED", "Media"}, {"CUT", "Cut"}, {"VER", "Versions"},
    };
    const int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
    static int panelTab = 0;
    static float slid[16] = {0};
    // The existing requests name the old tabs: inspect, media, cut, versions.
    const int kRequested[] = {0, 7, 8, 9};
    if (g_workspaceTabRequest >= 0 && g_workspaceTabRequest < 4)
        panelTab = kRequested[g_workspaceTabRequest];
    g_workspaceTabRequest = -1;
    float codeW = ImGui::CalcTextSize("PRJ").x;
    const float slide = 9, railW = codeW + 12 + slide, tabH = ImGui::GetFontSize() + 12;
    ImU32 pageCol = ImGui::GetColorU32(ImGuiCol_ChildBg);
    if ((pageCol >> IM_COL32_A_SHIFT) < 8) pageCol = ImGui::GetColorU32(ImGuiCol_WindowBg);
    ImVec2 railP = ImGui::GetCursorScreenPos();
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kPanelCount; i++) {
        ImGui::PushID(i);
        ImVec2 p(railP.x, railP.y + i * (tabH + 3));
        ImGui::SetCursorScreenPos(p);
        bool on = panelTab == i;
        if (ImGui::InvisibleButton("##tab", ImVec2(railW, tabH))) panelTab = i;
        bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", kPanels[i].name);
        float want = on ? 1.f : hot ? .5f : 0.f;
        slid[i] += (want - slid[i]) * std::clamp(ImGui::GetIO().DeltaTime * 16.f, 0.f, 1.f);
        float x0 = p.x + slide * (1 - slid[i]);
        ImVec2 q(p.x + railW + (on ? 8.f : 0.f), p.y + tabH);
        rdl->AddRectFilled(ImVec2(x0, p.y), q,
            on ? pageCol : IM_COL32(26, 30, 38, (int)(120 + 90 * slid[i])),
            7, ImDrawFlags_RoundCornersLeft);
        rdl->AddText(ImVec2(x0 + (railW - codeW) * .5f, p.y + (tabH - ImGui::GetFontSize()) * .5f),
            on ? IM_COL32(236, 244, 255, 255) :
            hot ? IM_COL32(200, 214, 230, 255) : IM_COL32(126, 140, 156, 255), kPanels[i].code);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(railP.x + railW, railP.y));
    // A borderless child drops its horizontal padding unless asked, and the page
    // needs that margin to breathe against the tabs.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::BeginChild("##panel", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        ImGui::PushTextWrapPos(0.0f);
        bool edgePreview = g_tl.drag == TimelineState::LeftEdge || g_tl.drag == TimelineState::RightEdge;
        if (edgePreview)
            DrawConsequences(g_tl.rippleAt, -g_tl.holdShift, g_tl.dragIndex + 1, g_tl.holdFrom >= 0);
        ImGui::SeparatorText(kPanels[panelTab].name);
        switch (panelTab) {
        case 0: DrawClipInspector();  break;
        case 1: DrawBeatPanel();      break;
        case 2: DrawTracksPanel();    break;
        case 3: DrawColorPanel();     break;
        case 4: DrawProjectorPanel(); break;
        case 5: DrawOutputPanel();    break;
        case 6: DrawVaultPanel();     break;
        case 7: DrawLibrary();        break;
        case 8:
            g_cutView = true;
            DrawCutControls();
            ImGui::SeparatorText("Markers & scenes");
            DrawMarkerPanel();
            break;
        case 9: DrawBranches();       break;
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(1, 12));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (g_textOpenNew) {                    // asked for by the bar over the timeline
        g_textOpenNew = false;
        ImGui::OpenPopup("Text card");
    }
    if (g_tl.editOpenText) {
        g_tl.editOpenText = false;
        auto* tv = TrackClips(g_tl.editTrack);
        if (tv && g_tl.editIndex >= 0 && g_tl.editIndex < (int)tv->size()) {
            Clip& c = *(*tv)[g_tl.editIndex];
            snprintf(g_textBuf, sizeof(g_textBuf), "%s", c.text.c_str());
            g_textScale = c.textScale;
            g_textDur = c.duration;
            g_textTarget = c.uid;           // by uid: indices can shift while it's open
            g_textTargetTrack = g_tl.editTrack;
            ImGui::OpenPopup("Text card");
        }
    }
    DrawTextCardPopup();

    // ---- keys that act on the edit
    if (!kio.WantTextInput) {
        if (chord && ImGui::IsKeyPressed(ImGuiKey_Z, false)) UndoStep(kio.KeyShift);
        if (chord && ImGui::IsKeyPressed(ImGuiKey_Y, false)) UndoStep(true);
        if (chord && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (kio.KeyShift) CopyToOSClipboard(); else CopySelection();
        }
        if (chord && ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard();
        if (chord && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            g_selUids.clear();              // everything on every picture track
            for (auto& c : g_clips) g_selUids.push_back(c->uid);
            for (auto& t : g_over) for (auto& c : t->clips) g_selUids.push_back(c->uid);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_selUids.clear();
        if (chord && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            if (kio.KeyShift) UngroupSelection(); else GroupSelection();
        }
        if (chord && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_navFold = true;
        if (chord && ImGui::IsKeyPressed(ImGuiKey_U, false)) {
            if (g_selTrack == -1 && g_sel >= 0 && g_sel < (int)g_clips.size() &&
                g_clips[g_sel]->kind == Clip::Nest) {
                g_navUnfold = g_sel; g_navUnfoldTrack = -1;
            } else if (g_selTrack >= 0 && g_selTrack < (int)g_over.size() &&
                       g_sel >= 0 && g_sel < (int)g_over[g_selTrack]->clips.size() &&
                       g_over[g_selTrack]->clips[g_sel]->kind == Clip::Nest) {
                g_navUnfold = g_sel; g_navUnfoldTrack = g_selTrack;
            } else g_intakeStatus = "pick a folded sequence to unfold";
        }
        if (chord && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            if (g_selTrack == -1 && g_sel >= 0 && g_sel < (int)g_clips.size() &&
                g_clips[g_sel]->kind == Clip::Nest) g_navEnter = g_clips[g_sel]->nest;
        }
        if (chord && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
            g_navDepth = (int)g_nav.size() - 2;

        // Arrows: a frame at a time, a second with shift. With alt they nudge the
        // selected layer clip or audio block instead of the playhead.
        int dir = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) ? 1
                : ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) ? -1 : 0;
        if (dir) {
            double step = kio.KeyShift ? 1.0 : 1.0 / g_fps;
            if (kio.KeyAlt) {
                if (g_selTrack >= 0 && g_selTrack < (int)g_over.size() &&
                    g_sel >= 0 && g_sel < (int)g_over[g_selTrack]->clips.size()) {
                    for (auto& t : g_over)
                        for (auto& c : t->clips)
                            if (SelHas(c->uid) || (&*c == &*g_over[g_selTrack]->clips[g_sel])) {
                                c->start += dir * step;
                                if (c->start < 0) c->start = 0;
                            }
                } else if (g_selTrack == -2 && g_selAT >= 0 && g_selAT < (int)g_atracks.size() &&
                           g_sel >= 0 && g_sel < (int)g_atracks[g_selAT]->blocks.size()) {
                    MixGuard lock;
                    for (auto& t : g_atracks)
                        for (auto& b : t->blocks)
                            if (SelHas(b->uid) || (&*b == &*g_atracks[g_selAT]->blocks[g_sel]))
                                b->offset += dir * step;
                }
            } else {
                double np = g_playhead.load() + dir * step;
                if (np < 0) np = 0;
                if (np > tot) np = tot;
                g_playhead.store(np);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) g_playhead.store(0.0);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))  g_playhead.store(tot);
    }

    if (g_tl.drag == TimelineState::None && !g_aspects.empty()) SortAspects();

    // ---- transport
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) {
        if (!g_playing.load() && g_playhead.load() >= tot - 1e-6) g_playhead.store(0.0);
        g_playing.store(!g_playing.load());
    }
    if (g_playing.load() && !g_audioReady)
        g_playhead.store(g_playhead.load() + ImGui::GetIO().DeltaTime);
    if (g_audition && g_playing.load() && g_playhead.load() >= g_auditionOut) {
        MixGuard lock;
        if (g_auditionLoop) g_playhead.store(g_auditionIn);
        else { g_playhead.store(g_auditionOut); g_playing.store(false); g_audition = false; }
    }
    if (g_playing.load() && g_playhead.load() >= tot) {
        g_playing.store(false);
        g_playhead.store(tot);
    }

    ApplyNavRequests();                     // stepping levels rebuilds the clip lists
    HyperCutTick();                         // arm, re-chop or settle a hypercut
    RefreshNestDurations();
    UndoCapture();                          // one snapshot per frame, once idle

    {
        MixGuard lock;
        g_videoAudio.clear();
        std::vector<BaseSpan> audioLayout;
        BaseLayout(audioLayout);
        for (size_t i = 0; i < g_clips.size(); ++i) {
            auto& c = g_clips[i];
            if (!g_baseOff && !c->skip && c->useAudio && c->kind == Clip::Video && c->volume > 0.0f && c->vid && c->vid->apeaksReady.load() && c->vid->audio && c->vid->audio->loaded) {
                g_videoAudio.push_back(VideoAudioBlock{ c->vid->audio, audioLayout[i].start, c->trimIn, c->duration, c->volume, c->reversed });
            }
        }
        for (auto& t : g_over) {
            if (!t->visible) continue;
            for (auto& c : t->clips) {
                if (!c->skip && c->useAudio && c->kind == Clip::Video && c->volume > 0.0f && c->vid && c->vid->apeaksReady.load() && c->vid->audio && c->vid->audio->loaded) {
                    g_videoAudio.push_back(VideoAudioBlock{ c->vid->audio, c->start, c->trimIn, c->duration, c->volume, c->reversed });
                }
            }
        }
    }

    ImGui::End();
}

static void ApplyStyle() {
    // Monochrome: everything is a grey, contrast alone carries the hierarchy. The only
    // saturated pixels in the app are the footage itself.
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 5;
    s.GrabRounding = 2;
    s.PopupRounding = 4;
    s.TabRounding = 3;
    s.ScrollbarRounding = 2;
    s.ScrollbarSize = 15;
    s.GrabMinSize = 16;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(16, 3);
    auto g = [](float v, float a = 1.0f) { return ImVec4(v, v, v, a); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]             = ImVec4(0.895f, 0.885f, 0.860f, 1);   // bone, not paper
    c[ImGuiCol_TextDisabled]     = ImVec4(0.64f, 0.68f, 0.73f, 1);
    c[ImGuiCol_WindowBg]         = ImVec4(0.045f, 0.058f, 0.077f, 1);
    c[ImGuiCol_ChildBg]          = ImVec4(0.067f, 0.083f, 0.105f, 1);
    c[ImGuiCol_PopupBg]          = ImVec4(0.075f, 0.075f, 0.075f, 0.98f);
    c[ImGuiCol_Border]           = g(1.0f, 0.09f);
    c[ImGuiCol_FrameBg]          = g(0.145f);
    c[ImGuiCol_FrameBgHovered]   = g(0.215f);
    c[ImGuiCol_FrameBgActive]    = g(0.275f);
    c[ImGuiCol_TitleBg]          = g(0.071f);
    c[ImGuiCol_TitleBgActive]    = g(0.110f);
    c[ImGuiCol_Button]           = g(0.165f);
    c[ImGuiCol_ButtonHovered]    = g(0.290f);
    c[ImGuiCol_ButtonActive]     = g(0.430f);
    c[ImGuiCol_SliderGrab]       = g(0.62f);
    c[ImGuiCol_SliderGrabActive] = g(0.92f);
    c[ImGuiCol_CheckMark]        = g(0.92f);
    c[ImGuiCol_Header]           = g(0.200f);
    c[ImGuiCol_HeaderHovered]    = g(0.290f);
    c[ImGuiCol_HeaderActive]     = g(0.360f);
    c[ImGuiCol_Tab]              = g(0.090f);
    c[ImGuiCol_TabHovered]       = g(0.250f);
    c[ImGuiCol_TabActive]        = g(0.185f);
    c[ImGuiCol_TabUnfocused]     = g(0.090f);
    c[ImGuiCol_TabUnfocusedActive] = g(0.150f);
    c[ImGuiCol_Separator]        = g(1.0f, 0.09f);
    c[ImGuiCol_SeparatorHovered] = g(1.0f, 0.25f);
    c[ImGuiCol_ResizeGrip]       = g(1.0f, 0.10f);
    c[ImGuiCol_ResizeGripHovered]= g(1.0f, 0.25f);
    c[ImGuiCol_ScrollbarBg]      = g(0.043f);
    c[ImGuiCol_ScrollbarGrab]    = g(0.230f);
    c[ImGuiCol_ScrollbarGrabHovered] = g(0.330f);
    c[ImGuiCol_ScrollbarGrabActive]  = g(0.430f);
    c[ImGuiCol_PlotHistogram]    = g(0.78f);
    c[ImGuiCol_PlotHistogramHovered] = g(0.92f);
    c[ImGuiCol_PlotLines]        = g(0.70f);
    c[ImGuiCol_NavHighlight]     = ImVec4(0.49f, 0.86f, 0.79f, 1);
    c[ImGuiCol_CheckMark]        = ImVec4(0.49f, 0.86f, 0.79f, 1);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.38f, 0.72f, 0.67f, 1);
    c[ImGuiCol_DragDropTarget]   = g(1.0f, 0.70f);
}

// ------------------------------------------------------------- win32 shell

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static bool CreateDevice(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             fls, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                             &g_d3dDevice, &fl, &g_d3dContext)))
        return false;
    ID3D11Texture2D* bb = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_d3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRTV);
    bb->Release();
    return true;
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_DROPFILES: {
        // Shell drops land on the UI thread, so the clip list can be touched directly.
        HDROP drop = (HDROP)wParam;
        // Where it landed decides the track: the cached timeline layout from the
        // last frame maps the client point onto a row and a time.
        ResetDropTarget();
        POINT pt = {};
        if (DragQueryPoint(drop, &pt) || true) {
            if (g_geom.valid && pt.x > g_geom.trackX) {
                double t = (pt.x - g_geom.trackX) / g_geom.pps + g_geom.scroll;
                for (auto& r : g_geom.rows) {
                    if (pt.y < r.y0 || pt.y > r.y1) continue;
                    if (r.kind == 1) { g_dropVTrack = r.idx; g_dropTime = t < 0 ? 0 : t; }
                    else if (r.kind == 2) { g_dropATrack = r.idx; g_dropTime = t < 0 ? 0 : t; }
                    else if (r.kind == 3) { g_dropVTrack = -2; g_dropTime = t < 0 ? 0 : t; }
                    else if (r.kind == 4) { g_dropATrack = -2; g_dropTime = t < 0 ? 0 : t; }
                    break;
                }
            }
        }
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        paths.reserve(n);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (!len) continue;
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);   // len excludes the terminator
            paths.push_back(p);
        }
        DragFinish(drop);
        if (!paths.empty()) AddFiles(std::move(paths));
        ResetDropTarget();
        SetForegroundWindow(hWnd);
        return 0;
    }
    case WM_SIZE:
        if (g_d3dDevice && wParam != SIZE_MINIMIZED) {
            if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                       DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* bb = nullptr;
            g_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
            g_d3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRTV);
            bb->Release();
        }
        return 0;
    case WM_CLOSE:
        Autosave(true);                    // before anything is torn down
        DestroyWindow(hWnd);
        return 0;
    case WM_QUERYENDSESSION:
        Autosave(true);
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) Autosave(true);
        return 0;
    case WM_ACTIVATEAPP:
        if (!wParam) Autosave(true);       // clicked away: park the work now
        return 0;
    case WM_DESTROY:
        Autosave(true);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

#include "edit_workspace_tests.h"

// --headless-export <out> [--range <in> <out>]: the autosaved project through the real
// export with the window hidden, each stage timed into %TEMP%\slidecut_headless.txt.
// The process exits when the export does: 0 done, 1 failed, 2 never started.
struct HeadlessRun {
    int         phase = 0;                 // 0 waiting for the project to settle, 1 exporting
    ULONGLONG   settle = 0, t0 = 0, beat = 0;
    std::string step;
    bool        enc = false, fin = false;
    std::string stall;
    int         stage = 1;
    int         code = 0;
    FILE*       log = nullptr;
};

static bool HeadlessTick(HeadlessRun& h, const std::wstring& out, double rin, double rout) {
    ULONGLONG now = GetTickCount64();
    auto note = [&](const std::string& what) {
        if (!h.log) return;
        fprintf(h.log, "%9.1f s  %s\n", (now - h.t0) / 1000.0, what.c_str());
        fflush(h.log);
    };
    if (h.phase == 0) {
        if (g_projectLoading.load() || g_songPending.valid()) { h.settle = 0; return false; }
        if (!h.settle) h.settle = now;
        if (now - h.settle < 2000) return false;   // let pending media loads land
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        h.log = _wfopen((std::wstring(tmp) + L"slidecut_headless.txt").c_str(), L"w");
        h.t0 = h.beat = now;
        if (rin >= 0 && rout > rin) {
            g_rangeIn = rin; g_rangeOut = rout;
            note("range export " + std::to_string(rin) + " - " + std::to_string(rout));
            ExportRange(out);
        } else {
            note("whole film, base track ends at " + std::to_string(TotalDuration()) +
                 " s, timeline at " + std::to_string(TimelineEnd()) + " s");
            ExportFlattened(out);
        }
        if (!g_export.active) {
            note("did not start: " + g_export.message);
            h.code = 2;
            return true;
        }
        note("stage 1 started");
        h.phase = 1;
        return false;
    }
    if (g_export.shaderStep != h.step) { h.step = g_export.shaderStep; note(h.step); }
    if (g_export.finalAt && !h.fin) { h.fin = true; note("final ffmpeg started"); }
    if (g_export.encoding && !h.enc) { h.enc = true; note("final ffmpeg reporting progress"); }
    if (g_export.stallNote != h.stall) {
        h.stall = g_export.stallNote;
        note(h.stall.empty() ? "progress resumed" : "STALL: " + h.stall);
    }
    if (g_export.active && g_export.stage != h.stage) {
        h.stage = g_export.stage;
        note("stage 2 (projector) started");
    }
    if (g_export.active && now - h.beat >= 30000) {
        h.beat = now;
        char b[64];
        snprintf(b, 64, "  progress %.1f%%", g_export.progress * 100.0f);
        note(b);
    }
    if (g_export.active) return false;
    note((g_export.failed ? "FAILED: " : "done: ") + g_export.message);
    h.code = g_export.failed ? 1 : 0;
    fclose(h.log);
    h.log = nullptr;
    return true;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (wcsstr(GetCommandLineW(), L"--workspace-test")) return RunWorkspaceTests();
    if (wcsstr(GetCommandLineW(), L"--workspace-ui")) return RunWorkspaceUITests(hInst);
    if (wcsstr(GetCommandLineW(), L"--workspace-media")) return RunWorkspaceUITests(hInst, true);
    InstallCrashHandler();          // before anything that can fault
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring hlOut;
    double hlIn = -1, hlEnd = -1;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; i++) {
            if (!wcscmp(argv[i], L"--headless-export") && i + 1 < argc) hlOut = argv[++i];
            else if (!wcscmp(argv[i], L"--range") && i + 2 < argc) {
                hlIn = _wtof(argv[++i]);
                hlEnd = _wtof(argv[++i]);
            }
        }
        if (argv) LocalFree(argv);
    }
    const bool headless = !hlOut.empty();
    HeadlessRun hl;
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                       nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                       L"SlideCut", nullptr };
    RegisterClassExW(&wc);
    HWND hWnd = CreateWindowW(wc.lpszClassName, L"SlideCut", WS_OVERLAPPEDWINDOW,
                              80, 60, 1440, 860, nullptr, nullptr, hInst, nullptr);
    if (!CreateDevice(hWnd)) return 1;
    DragAcceptFiles(hWnd, TRUE);           // media drops, handled in WM_DROPFILES
    {   // Elevated processes get drops blocked by UIPI unless the filter is opened.
        // Resolved at runtime so the build does not need a _WIN32_WINNT bump.
        typedef BOOL (WINAPI *PFilterEx)(HWND, UINT, DWORD, void*);
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            auto f = (PFilterEx)GetProcAddress(u32, "ChangeWindowMessageFilterEx");
            if (f) {
                f(hWnd, WM_DROPFILES, 1 /*MSGFLT_ALLOW*/, nullptr);
                f(hWnd, 0x0049 /*WM_COPYGLOBALDATA*/, 1, nullptr);
            }
        }
    }
    ShowWindow(hWnd, headless ? SW_HIDE : SW_SHOWMAXIMIZED);
    UpdateWindow(hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd);
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpi);
    if (!font) io.Fonts->AddFontDefault();
    {   // Special Elite for title cards: rasterize big once, scale down when drawn
        std::wstring tp = AssetPath(TITLE_FONT_FILE);
        if (!tp.empty())
            g_titleFont = io.Fonts->AddFontFromFileTTF(Narrow(tp).c_str(), TITLE_FONT_PX);
    }
    ApplyStyle();
    ImGui::GetStyle().ScaleAllSizes(dpi);

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);
    InitAudio();
    StartProxyPrefetch();

    bool crashLoop = StartupMarkExists();   // the last run died before its first frame
    {   // pick the session back up where it stopped, crash or clean exit alike
        std::wstring auto_ = AutosavePath();
        if (crashLoop) {
            // Do not touch the file: it is still on disk, and every backup is too.
            g_projectStatus = "Last session crashed on load — started empty. "
                              "Your work is under BACKUPS.";
        } else if (GetFileAttributesW(auto_.c_str()) != INVALID_FILE_ATTRIBUTES) {
            StartupMarkSet();
            std::string text = ReadTextFile(auto_);
            if (!text.empty() && LoadProjectFromText(text))
                g_projectStatus = "Restored the last session (autosave)";
        }
    }
    int framesDrawn = 0;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        PumpPendingLoads();
        PumpProxyPrefetch();               // frames the prefetch thread finished
        PumpSceneSplit();
        if (!headless) Autosave();         // a headless run must not touch the session
        PumpExport();
        if (g_songPending.valid() &&
            g_songPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            g_songPending.get();
        if (headless && HeadlessTick(hl, hlOut, hlIn, hlEnd)) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawApp();
        ImGui::Render();

        const float clear[4] = { 0.03f, 0.03f, 0.04f, 1.0f };
        g_d3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_d3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
        PumpRetiredTextures();             // frames old enough that nothing draws them
        if (++framesDrawn == 3) StartupMarkClear();   // it draws: no longer suspect
    }

    if (!crashLoop && !headless) Autosave(true);   // last word before the window goes away
    SweepClipboardTemps();                 // the clipboard's temp films die with us
    StartupMarkClear();
    g_playing.store(false);
    StopProxyPrefetch();                   // no decoding into a device about to die
    if (g_audioReady) ma_device_uninit(&g_audioDevice);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    {
        std::vector<Clip*> all;
        for (auto& c : g_clips) all.push_back(c.get());
        for (auto& t : g_over) for (auto& c : t->clips) all.push_back(c.get());
        for (Clip* c : all) {
            if (c->kind != Clip::Video) RetireTexture(c->tex);
            if (!c->dxIsVideo) RetireTexture(c->dxTex);
        }
        for (auto& r : g_retire) r.first->Release();
        g_retire.clear();
    }
    if (g_projPS) g_projPS->Release();
    if (g_quadPS) g_quadPS->Release();
    if (g_fsVS) g_fsVS->Release();
    if (g_quadVS) g_quadVS->Release();
    if (g_cb) g_cb->Release();
    if (g_sampLinear) g_sampLinear->Release();
    if (g_blendAlpha) g_blendAlpha->Release();
    if (g_rsNone) g_rsNone->Release();
    if (g_dsOff) g_dsOff->Release();
    for (int i = 0; i < 4; i++) {
        if (g_projSRV[i]) g_projSRV[i]->Release();
        if (g_projRTV[i]) g_projRTV[i]->Release();
        if (g_projTex[i]) g_projTex[i]->Release();
    }
    if (g_blendPS) g_blendPS->Release();
    for (auto& v : g_videoSources) {
        for (auto& kv : v->cache) if (kv.second) kv.second->Release();
        v->cache.clear();
    }
    if (g_mainRTV) g_mainRTV->Release();
    if (g_swapChain) g_swapChain->Release();
    if (g_d3dContext) g_d3dContext->Release();
    if (g_d3dDevice) g_d3dDevice->Release();
    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    CoUninitialize();
    return headless ? hl.code : 0;
}
