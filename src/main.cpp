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
#include <shlwapi.h>
#include <d3d11.h>
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
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

// A decoded low-res frame ladder for one video file, shared by every clip cut from
// it. Frames are jpegs on disk (written progressively by a background ffmpeg) and
// uploaded to the GPU on first use.
struct VideoSource {
    std::wstring path;
    std::wstring proxyDir;                 // holds %06d.jpg at PROXY_FPS
    double  duration = 0;                  // full source length, seconds
    int     w = 0, h = 0;                  // source pixel size
    double  fps = 0;                       // source frame rate
    bool    hasAudio = false;
    std::atomic<bool> probed{ false };     // duration/size known
    std::atomic<bool> ready{ false };      // proxy extraction finished
    std::atomic<int>  framesOnDisk{ 0 };   // grows while extracting
    std::map<int, ID3D11ShaderResourceView*> cache;   // frame index -> texture
    size_t  cacheBytes = 0;
    size_t  frameBytes = 0;                // uploaded size of one proxy frame
    std::atomic<bool> building{ false };
    float   aspect = 1.0f;
};

struct Clip {
    enum Kind { Image, Text, Video } kind = Image;
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
    bool         useAudio = true;          // mix this clip's own audio into the export
};

struct Song {
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
};

static const int SAMPLE_RATE = 48000;

static std::vector<std::unique_ptr<Clip>> g_clips;
static Song                g_song;
static std::atomic<double> g_playhead(0.0);
static std::atomic<bool>   g_playing(false);
static ma_device           g_audioDevice;
static bool                g_audioReady = false;

static double TotalDuration() {
    double t = 0;
    for (auto& c : g_clips) t += c->duration;
    return t;
}
static int ClipAt(double t, double* clipStart = nullptr) {
    double s = 0;
    for (int i = 0; i < (int)g_clips.size(); i++) {
        double e = s + g_clips[i]->duration;
        if (t < e || i == (int)g_clips.size() - 1) { if (clipStart) *clipStart = s; return i; }
        s = e;
    }
    return -1;
}

// ------------------------------------------------------------------- audio

static void AudioCallback(ma_device*, void* out, const void*, ma_uint32 frames) {
    float* o = (float*)out;
    memset(o, 0, sizeof(float) * frames * 2);
    if (!g_playing.load(std::memory_order_relaxed)) return;
    double ph = g_playhead.load(std::memory_order_relaxed);
    if (g_song.loaded) {
        long long start = llround((ph - g_song.offset + g_song.trimStart) * SAMPLE_RATE);
        long long lo = llround(g_song.trimStart * SAMPLE_RATE);
        long long hi = llround(g_song.trimEnd * SAMPLE_RATE);
        long long total = (long long)(g_song.pcm.size() / 2);
        if (hi > total) hi = total;
        for (ma_uint32 i = 0; i < frames; i++) {
            long long idx = start + i;
            if (idx >= lo && idx < hi) {
                o[i * 2 + 0] = g_song.pcm[idx * 2 + 0] * 0.9f;
                o[i * 2 + 1] = g_song.pcm[idx * 2 + 1] * 0.9f;
            }
        }
    }
    g_playhead.store(ph + (double)frames / SAMPLE_RATE, std::memory_order_relaxed);
}

static void InitAudio() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = SAMPLE_RATE;
    cfg.dataCallback = AudioCallback;
    g_audioReady = ma_device_init(nullptr, &cfg, &g_audioDevice) == MA_SUCCESS &&
                   ma_device_start(&g_audioDevice) == MA_SUCCESS;
}

static std::future<bool> g_songPending;
static std::atomic<bool> g_songLoading(false);
static std::string       g_songStatus;
static bool LoadSongBlocking(const std::wstring& path) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, SAMPLE_RATE);
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &dc, &dec) != MA_SUCCESS) return false;
    std::vector<float> pcm;
    float buf[4096 * 2];
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        if (got == 0) break;
        pcm.insert(pcm.end(), buf, buf + got * 2);
    }
    ma_decoder_uninit(&dec);
    if (pcm.empty()) return false;

    Song s;
    s.pcm = std::move(pcm);
    s.duration = (double)(s.pcm.size() / 2) / SAMPLE_RATE;
    size_t nFrames = s.pcm.size() / 2;
    size_t nPeaks = nFrames / s.framesPerPeak + 1;
    s.peaks.resize(nPeaks * 2);
    for (size_t p = 0; p < nPeaks; p++) {
        float lo = 0, hi = 0;
        size_t a = p * s.framesPerPeak, b = a + s.framesPerPeak;
        if (b > nFrames) b = nFrames;
        for (size_t f = a; f < b; f++) {
            float v = (s.pcm[f * 2] + s.pcm[f * 2 + 1]) * 0.5f;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        s.peaks[p * 2] = lo; s.peaks[p * 2 + 1] = hi;
    }
    s.path = path;
    s.label = Narrow(BaseName(path));
    s.offset = 0.0;
    s.trimStart = 0.0;
    s.trimEnd = s.duration;
    s.loaded = true;
    // Swap in while audio callback isn't reading: pause briefly.
    bool wasPlaying = g_playing.exchange(false);
    Sleep(30);                             // let in-flight callback finish
    g_song = std::move(s);
    g_playing.store(wasPlaying);
    return true;
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
        g_song.label = "YouTube · " + (slash == std::string::npos ? url : url.substr(slash + 1));
        g_songStatus.clear();
    } else {
        g_songStatus = "downloaded but could not decode audio";
    }
    return ok;
}

static void StartSongLoad(const std::wstring& path) {
    if (g_songLoading.load()) return;
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

static const size_t PROXY_CACHE_BYTES = 320u * 1024 * 1024;   // GPU budget per source

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
                   L"-show_entries format=duration -of default=nw=1 \"" + vs->path + L"\"",
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

// Frame nearest to t seconds into the source; nullptr while it is not on disk yet.
static ID3D11ShaderResourceView* ProxyFrame(VideoSource& vs, double t) {
    if (t < 0) t = 0;
    int idx = (int)(t * PreviewFps()) + 1;               // ffmpeg numbers from 1
    int have = vs.framesOnDisk.load();
    if (have > 0 && idx > have) idx = have;
    auto it = vs.cache.find(idx);
    if (it != vs.cache.end()) return it->second;

    wchar_t name[32];
    swprintf(name, 32, L"%06d.jpg", idx);
    FILE* f = _wfopen((vs.proxyDir + name).c_str(), L"rb");
    if (!f) return nullptr;
    int w, h, comp;
    unsigned char* px = stbi_load_from_file(f, &w, &h, &comp, 4);
    fclose(f);
    if (!px) return nullptr;
    ID3D11ShaderResourceView* tex = CreateTextureRGBA(px, w, h);
    stbi_image_free(px);
    if (vs.aspect <= 0 || vs.w == 0) vs.aspect = (float)w / (float)h;
    vs.frameBytes = (size_t)w * h * 4;

    if (vs.cacheBytes + vs.frameBytes > PROXY_CACHE_BYTES) {   // drop frames far from here
        for (int span = 240; span >= 15 && vs.cacheBytes + vs.frameBytes > PROXY_CACHE_BYTES;
             span /= 2) {
            for (auto i = vs.cache.begin(); i != vs.cache.end(); ) {
                if (abs(i->first - idx) > span) {
                    if (i->second) i->second->Release();
                    vs.cacheBytes -= vs.frameBytes;
                    i = vs.cache.erase(i);
                } else ++i;
            }
        }
    }
    vs.cache[idx] = tex;
    vs.cacheBytes += vs.frameBytes;
    return tex;
}

static void ReleaseProxyCache(VideoSource& vs) {
    for (auto& kv : vs.cache) if (kv.second) kv.second->Release();
    vs.cache.clear();
    vs.cacheBytes = 0;
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

static void StartSongLoad(const std::wstring& path);

static std::string g_intakeStatus;         // "12 images · 2 videos · 1 song" after a drop

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
    if (!imgs.empty()) AddImages(imgs);
    if (!vids.empty()) AddVideos(vids);
    if (!song.empty()) StartSongLoad(song);

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

// Cut clip i at `off` seconds into it; the tail becomes a new clip at i+1.
static void SplitClip(int i, double off) {
    Clip& c = *g_clips[i];
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
    t->tex = c.tex;
    // still thumbnails are refcounted per clip; video posters belong to the proxy cache
    if (t->tex && t->kind != Clip::Video) t->tex->AddRef();
    t->trimIn = c.trimIn + (c.kind == Clip::Video ? off : 0.0);
    t->duration = c.duration - off;
    c.duration = off;
    g_clips.insert(g_clips.begin() + i + 1, std::move(t));
}

// Timeline index where a clip inserted at time t would go, splitting whatever
// clip straddles t. Used for injecting cards mid-shot.
static int SplitPoint(double t) {
    const double EPS = 1e-4;
    double s = 0;
    for (int i = 0; i < (int)g_clips.size(); i++) {
        double e = s + g_clips[i]->duration;
        if (t <= s + EPS) return i;
        if (t < e - EPS) { SplitClip(i, t - s); return i + 1; }
        s = e;
    }
    return (int)g_clips.size();
}

static void PumpPendingLoads() {          // main thread: turn decoded pixels into textures
    for (auto& c : g_clips) {
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
                                  const std::wstring& suggestDir) {
    std::wstring out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) return out;
    std::wstring filterName = std::wstring(ext) + L" video";
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

// ------------------------------------------------------------------- export

struct ExportJob {
    HANDLE       process = nullptr;
    std::wstring progressFile;
    std::wstring logFile;
    std::wstring outPath;
    std::string  cmd;                     // utf8 copy of the ffmpeg command, for "Copy"
    double       totalDur = 0;
    float        progress = 0;            // 0..1
    bool         active = false;
    bool         failed = false;
    std::string  message;
} g_export;

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
enum FitMode { FIT_BLUR = 0, FIT_BARS, FIT_CROP };
// Video encoder. NVENC entries need an NVIDIA GPU; they fall back to nothing, so
// the export just fails with ffmpeg's own message if the encoder is missing.
enum VCodec { VC_X264 = 0, VC_X265, VC_NVENC_H264, VC_NVENC_HEVC };
enum RateMode { RM_CRF = 0, RM_BITRATE };
enum Container { CT_MP4 = 0, CT_MOV, CT_MKV };

static int   g_preset = PRESET_TIKTOK;
static int   g_customW = 1080, g_customH = 1920;
static int   g_fit = FIT_BLUR;
static int   g_vcodec = VC_X264;
static int   g_speed = 2;                 // index into SPEED_NAMES
static int   g_rateMode = RM_CRF;
static int   g_crf = 15;                  // lower = better; 15 is visually lossless-ish
static float g_targetMbps = 20.0f;        // used when g_rateMode == RM_BITRATE
static int   g_container = CT_MP4;
static int   g_abrIdx = 3;                // index into AUDIO_KBPS
static bool  g_loudnorm = false;          // one-pass EBU R128 normalise of the final mix
static bool  g_faststart = true;          // move the moov atom up front (mp4/mov)
static float g_fadeIn = 0.0f;             // seconds of fade from black / silence
static float g_fadeOut = 0.0f;
static float g_musicVol = 0.8f;           // song level in the mix (clip audio stays at 1.0)

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

    bool needFont = false;
    for (auto& c : g_clips) if (c->kind == Clip::Text) needFont = true;
    if (needFont) {
        std::wstring src = AssetPath(TITLE_FONT_FILE);
        if (src.empty()) return false;
        CopyFileW(src.c_str(), (g_workDir + L"font.ttf").c_str(), FALSE);
    }
    for (size_t i = 0; i < g_clips.size(); i++) {
        if (g_clips[i]->kind != Clip::Text) continue;
        wchar_t name[64];
        swprintf(name, 64, L"t%zu.txt", i);
        HANDLE f = CreateFileW((g_workDir + name).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wr = 0;                      // raw UTF-8, no BOM: drawtext reads it as-is
        WriteFile(f, g_clips[i]->text.data(), (DWORD)g_clips[i]->text.size(), &wr, nullptr);
        CloseHandle(f);
    }
    return true;
}

static void StartExport(const std::wstring& outPath) {
    if (g_clips.empty()) return;

    int W = 0, H = 0;
    ResolveCanvas(&W, &H);
    const int FPS = g_fps;
    const bool fitCanvas = g_preset != PRESET_ORIGINAL;
    const int fit = fitCanvas ? g_fit : FIT_BARS;   // "Original" never needs a fill

    bool anyPending = false, anyProbing = false;
    for (auto& c : g_clips) {
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
    std::wstring cmd = L"ffmpeg -y -hide_banner -nostdin -loglevel error -nostats -progress \"" +
                       g_export.progressFile + L"\"";
    for (auto& c : g_clips) {
        wchar_t seg[128];
        if (c->kind == Clip::Text) {
            swprintf(seg, 128, L" -f lavfi -t %.4f -i color=c=black:s=%dx%d:r=%d",
                     c->duration, W, H, FPS);
            cmd += seg;
        } else if (c->kind == Clip::Video) {
            swprintf(seg, 128, L" -ss %.4f -t %.4f -i ", c->trimIn, c->duration);
            cmd += seg;
            cmd += L"\"" + c->path + L"\"";
        } else {
            swprintf(seg, 128, L" -loop 1 -framerate %d -t %.4f -i ", FPS, c->duration);
            cmd += seg;
            cmd += L"\"" + c->path + L"\"";
        }
    }
    bool music = g_song.loaded;
    if (music) cmd += L" -i \"" + g_song.path + L"\"";

    // Any clip contributing its own audio forces the concat to carry an audio pad,
    // so every other clip needs a matching block of silence.
    bool clipAudio = false;
    for (auto& c : g_clips)
        if (c->kind == Clip::Video && c->useAudio && c->vid && c->vid->hasAudio) clipAudio = true;
    bool audio = music || clipAudio;

    std::wstring fc;
    for (size_t i = 0; i < g_clips.size(); i++) {
        wchar_t seg[768];
        if (g_clips[i]->kind == Clip::Text) {
            int fs = (int)(g_clips[i]->textScale * H);
            if (fs < 8) fs = 8;
            swprintf(seg, 768,
                     L"[%zu:v]drawtext=fontfile=font.ttf:textfile=t%zu.txt:fontcolor=white:"
                     L"fontsize=%d:line_spacing=%d:x=(w-text_w)/2:y=(h-text_h)/2,"
                     L"setsar=1[v%zu];",
                     i, i, fs, fs / 4, i);
        } else if (fit == FIT_BLUR) {
            swprintf(seg, 768,
                     L"[%zu:v]fps=%d,setpts=PTS-STARTPTS,split=2[b%zu][f%zu];"
                     L"[b%zu]scale=%d:%d:force_original_aspect_ratio=increase,"
                     L"crop=%d:%d,gblur=sigma=%d[bb%zu];"
                     L"[f%zu]scale=%d:%d:force_original_aspect_ratio=decrease:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int[ff%zu];"
                     L"[bb%zu][ff%zu]overlay=(W-w)/2:(H-h)/2,setsar=1[v%zu];",
                     i, FPS, i, i,
                     i, W, H, W, H, H / 48, i,
                     i, W, H, i,
                     i, i, i);
        } else if (fit == FIT_CROP) {
            swprintf(seg, 768,
                     L"[%zu:v]fps=%d,setpts=PTS-STARTPTS,"
                     L"scale=%d:%d:force_original_aspect_ratio=increase:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int,"
                     L"crop=%d:%d,setsar=1[v%zu];",
                     i, FPS, W, H, W, H, i);
        } else {
            swprintf(seg, 768,
                     L"[%zu:v]fps=%d,setpts=PTS-STARTPTS,"
                     L"scale=%d:%d:force_original_aspect_ratio=decrease:"
                     L"flags=lanczos+accurate_rnd+full_chroma_int,"
                     L"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,setsar=1[v%zu];",
                     i, FPS, W, H, W, H, i);
        }
        fc += seg;
    }
    if (clipAudio) {                       // one audio block per clip, exact length
        for (size_t i = 0; i < g_clips.size(); i++) {
            Clip& c = *g_clips[i];
            bool own = c.kind == Clip::Video && c.useAudio && c.vid && c.vid->hasAudio;
            wchar_t seg2[512];
            if (own) {
                swprintf(seg2, 512,
                         L"[%zu:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,"
                         L"asetpts=PTS-STARTPTS,apad,atrim=end=%.4f[a%zu];",
                         i, c.duration, i);
            } else {
                swprintf(seg2, 512,
                         L"anullsrc=r=48000:cl=stereo,atrim=end=%.4f,asetpts=PTS-STARTPTS[a%zu];",
                         c.duration, i);
            }
            fc += seg2;
        }
    }
    for (size_t i = 0; i < g_clips.size(); i++) {
        wchar_t seg[64];
        if (clipAudio) swprintf(seg, 64, L"[v%zu][a%zu]", i, i);
        else           swprintf(seg, 64, L"[v%zu]", i);
        fc += seg;
    }
    {   // NOTE: never %s a wide literal through swprintf — MinGW reads %s as char*
        // and silently truncates it (that is what turned "[ac]" into "["). Concatenate.
        wchar_t seg[128];
        swprintf(seg, 128, L"concat=n=%zu:v=1:a=%d[vc]", g_clips.size(), clipAudio ? 1 : 0);
        fc += seg;
        fc += clipAudio ? L"[ac];" : L";";
    }
    double total = TotalDuration();
    {   // tag bt709 in the graph: output-side -color_primaries/-color_trc alone do not stick
        wchar_t vt[256];
        swprintf(vt, 256, L"[vc]fps=%d,format=yuv420p,"
                          L"setparams=color_primaries=bt709:color_trc=bt709:"
                          L"colorspace=bt709:range=tv", FPS);
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
        fc += L"[v]";
    }

    if (music) {
        double effStart = g_song.trimStart + (g_song.offset < 0 ? -g_song.offset : 0.0);
        double effEnd = g_song.trimEnd;
        int delayMs = g_song.offset > 0 ? (int)llround(g_song.offset * 1000.0) : 0;
        int volPct = (int)lround(g_musicVol * 100.0f);
        wchar_t af[420];
        if (effStart >= effEnd) {
            swprintf(af, 420, L";[%zu:a]atrim=end=0,asetpts=PTS-STARTPTS,apad[m]",
                     g_clips.size());          // song trimmed entirely off-screen: silence
        } else {
            swprintf(af, 420,
                     L";[%zu:a]atrim=start=%.4f:end=%.4f,asetpts=PTS-STARTPTS,"
                     L"adelay=%d|%d:all=1,volume=%d/100,apad[m]",
                     g_clips.size(), effStart, effEnd, delayMs, delayMs, volPct);
        }
        fc += af;
        // music over the clips' own audio, or on its own when nothing else has sound
        if (clipAudio) fc += L";[ac][m]amix=inputs=2:duration=first:normalize=0[apre]";
        else           fc += L";[m]anull[apre]";
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
        if (!any) fc += L"anull";
        fc += L"[a]";
    }

    wchar_t tbuf[64];
    swprintf(tbuf, 64, L"%.4f", total);
    cmd += L" -filter_complex \"" + fc + L"\" -map \"[v]\"";
    // Platforms re-encode everything on upload. The master they transcode from should
    // be native resolution, native frame rate, high bitrate and fixed-GOP: that is what
    // keeps their encoder from spending its budget on our compression artifacts.
    if (audio) {
        wchar_t ab[64];
        swprintf(ab, 64, L" -map \"[a]\" -c:a aac -b:a %dk -ar 48000 -ac 2",
                 AUDIO_KBPS[g_abrIdx]);
        cmd += ab;
    }

    const wchar_t* enc_name =
        g_vcodec == VC_X264 ? L"libx264" : g_vcodec == VC_X265 ? L"libx265"
      : g_vcodec == VC_NVENC_H264 ? L"h264_nvenc" : L"hevc_nvenc";
    const wchar_t* prof = (g_vcodec == VC_X264 || g_vcodec == VC_NVENC_H264) ? L"high" : L"main";
    cmd += L" -c:v " + std::wstring(enc_name) + L" -pix_fmt yuv420p -profile:v " + prof +
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
    if (IsNvenc(g_vcodec)) {
        if (g_rateMode == RM_CRF)
            swprintf(enc, 384, L" -preset %ls -tune hq -rc vbr -cq %d -b:v 0"
                               L" -maxrate %lldk -bufsize %lldk -bf 3",
                     NVENC_SPEEDS[g_speed], g_crf, maxrate / 1000, maxrate / 500);
        else
            swprintf(enc, 384, L" -preset %ls -tune hq -rc vbr -b:v %lldk"
                               L" -maxrate %lldk -bufsize %lldk -bf 3",
                     NVENC_SPEEDS[g_speed], maxrate / 1000, maxrate / 1000, maxrate / 500);
    } else if (g_rateMode == RM_CRF) {
        swprintf(enc, 384, L" -preset %ls -crf %d -maxrate %lldk -bufsize %lldk",
                 SPEED_NAMES[g_speed], g_crf, maxrate / 1000, maxrate / 500);
    } else {
        swprintf(enc, 384, L" -preset %ls -b:v %lldk -maxrate %lldk -bufsize %lldk",
                 SPEED_NAMES[g_speed], maxrate / 1000, maxrate / 1000, maxrate / 500);
    }
    cmd += enc;
    if (g_vcodec == VC_X264) {             // x264-only knobs; the others reject or ignore them
        wchar_t x[128];
        swprintf(x, 128, L" -level %ls -sc_threshold 0 -bf 3 -refs 4", FPS >= 50 ? L"5.1" : L"4.2");
        cmd += x;
    }
    if ((g_vcodec == VC_X265 || g_vcodec == VC_NVENC_HEVC) && g_container != CT_MKV)
        cmd += L" -tag:v hvc1";            // QuickTime/Apple players need the hvc1 brand
    wchar_t rate[96];
    swprintf(rate, 96, L" -r %d -g %d -keyint_min %d", FPS, gop, gop);
    cmd += rate;

    cmd += L" -t " + std::wstring(tbuf);
    if (g_faststart && g_container != CT_MKV) cmd += L" -movflags +faststart";
    cmd += L" \"" + outPath + L"\"";

    g_export.cmd = Narrow(cmd);
    g_export.outPath = outPath;
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
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, g_workDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        g_export.failed = true;
        g_export.message = "Could not launch ffmpeg (is it on PATH?)";
        return;
    }
    CloseHandle(pi.hThread);
    g_export.process = pi.hProcess;
    g_export.totalDur = total;
    g_export.progress = 0;
    g_export.active = true;
    g_export.failed = false;
    g_export.message = "Encoding…";
}

static void PumpExport() {
    if (!g_export.active) return;
    // Progress: last out_time_us= line in the progress file.
    HANDLE f = CreateFileW(g_export.progressFile.c_str(), GENERIC_READ,
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
                if (g_export.totalDur > 0)
                    g_export.progress = (float)fmin(us / 1e6 / g_export.totalDur, 1.0);
            }
        }
        CloseHandle(f);
    }
    if (WaitForSingleObject(g_export.process, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(g_export.process, &code);
        CloseHandle(g_export.process);
        g_export.process = nullptr;
        g_export.active = false;
        g_export.failed = code != 0;
        if (code == 0) {
            g_lastExportDir = DirName(g_export.outPath);
            g_export.message = "Done — " + Narrow(BaseName(g_export.outPath));
            g_export.progress = 1.0f;
        } else {
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
    enum DragKind { None, LeftEdge, RightEdge, Move, Audio, AudioLeft, AudioRight, Scrub } drag = None;
    int    dragIndex = -1;
    double dragStartVal = 0;              // duration or offset at drag start
    double dragStartVal2 = 0;             // trimStart/trimEnd at drag start
    float  dragStartMouseX = 0;
    int    editIndex = -1;                // duration-edit popup target
    double editValue = 0;
    bool   editOpenText = false;          // request to open the text-card editor
} g_tl;

// text-card editor state, shared by "Add Text" and double-click-to-edit
static char  g_textBuf[1024] = "";
static float g_textScale = 0.13f;
static double g_textDur = 2.0;
static int   g_textTarget = -1;           // -1 = creating a new card
static bool  g_textInsert = true;         // drop the new card at the playhead


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
        if (g_textTarget >= 0 && g_textTarget < (int)g_clips.size()) {
            Clip& c = *g_clips[g_textTarget];
            c.text = g_textBuf;
            c.textScale = g_textScale;
            c.duration = d;
            c.label = FirstLine(c.text);
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

    const float RULER_H = 22.0f, CLIP_H = 64.0f, AUDIO_H = 46.0f, GAP = 6.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float totalH = RULER_H + CLIP_H + GAP + AUDIO_H + 8;
    if (avail.y < totalH) avail.y = totalH;

    ImGui::InvisibleButton("timeline", ImVec2(avail.x, avail.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    auto SecToX = [&](double t) { return origin.x + (float)((t - g_tl.scrollSec) * g_tl.pps); };
    auto XToSec = [&](float x) { return (double)(x - origin.x) / g_tl.pps + g_tl.scrollSec; };

    // zoom (ctrl+wheel around cursor) & pan (wheel / middle-drag)
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            double anchor = XToSec(io.MousePos.x);
            g_tl.pps *= powf(1.25f, io.MouseWheel);
            g_tl.pps = g_tl.pps < 4 ? 4 : (g_tl.pps > 2000 ? 2000 : g_tl.pps);
            g_tl.scrollSec = (float)(anchor - (io.MousePos.x - origin.x) / g_tl.pps);
        } else {
            g_tl.scrollSec -= io.MouseWheel * 60.0f / g_tl.pps;
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        g_tl.scrollSec -= io.MouseDelta.x / g_tl.pps;
    if (g_tl.scrollSec < -2) g_tl.scrollSec = -2;

    // ---- ruler
    float rulerY = origin.y;
    dl->AddRectFilled(ImVec2(origin.x, rulerY), ImVec2(origin.x + avail.x, rulerY + RULER_H),
                      IM_COL32(24, 24, 28, 255));
    double step = 1.0;
    while (step * g_tl.pps < 70) step *= 2;
    while (step * g_tl.pps > 220) step /= 2;
    double t0 = floor(g_tl.scrollSec / step) * step;
    for (double t = t0; SecToX(t) < origin.x + avail.x; t += step) {
        float x = SecToX(t);
        if (x < origin.x) continue;
        dl->AddLine(ImVec2(x, rulerY + RULER_H - 7), ImVec2(x, rulerY + RULER_H),
                    IM_COL32(120, 120, 130, 255));
        char lbl[32];
        if (step >= 1.0) snprintf(lbl, 32, "%d:%02d", (int)t / 60, (int)t % 60);
        else snprintf(lbl, 32, "%.2fs", t);
        dl->AddText(ImVec2(x + 4, rulerY + 3), IM_COL32(150, 150, 160, 255), lbl);
    }

    // ---- clips row
    float clipY = rulerY + RULER_H + 4;
    dl->AddRectFilled(ImVec2(origin.x, clipY), ImVec2(origin.x + avail.x, clipY + CLIP_H),
                      IM_COL32(18, 18, 22, 255));

    const float EDGE = 7.0f;
    int hotEdgeClip = -1, hotEdgeSide = 0, hotBody = -1;
    double start = 0;
    for (int i = 0; i < (int)g_clips.size(); i++) {
        Clip& c = *g_clips[i];
        float x0 = SecToX(start), x1 = SecToX(start + c.duration);
        start += c.duration;
        if (x1 < origin.x || x0 > origin.x + avail.x) continue;

        if (c.kind == Clip::Video && c.vid)   // poster frame follows the in-point
            c.tex = ProxyFrame(*c.vid, c.trimIn);

        bool isDragged = g_tl.drag == TimelineState::Move && g_tl.dragIndex == i;
        ImU32 fill = c.kind == Clip::Text
                   ? (isDragged ? IM_COL32(30, 30, 34, 255) : IM_COL32(12, 12, 14, 255))
                   : (isDragged ? IM_COL32(70, 82, 110, 255) : IM_COL32(46, 52, 66, 255));
        dl->AddRectFilled(ImVec2(x0 + 1, clipY + 2), ImVec2(x1 - 1, clipY + CLIP_H - 2), fill, 5.0f);

        if (c.tex) {                       // thumbnail, cover-cropped into the clip box
            float bw = x1 - x0 - 2, bh = CLIP_H - 4;
            if (bw > 8) {
                float boxA = bw / bh;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (c.texAspect > boxA) {  // crop horizontally
                    float f = boxA / c.texAspect;
                    uv0.x = 0.5f - f * 0.5f; uv1.x = 0.5f + f * 0.5f;
                } else {
                    float f = c.texAspect / boxA;
                    uv0.y = 0.5f - f * 0.5f; uv1.y = 0.5f + f * 0.5f;
                }
                dl->AddImageRounded((ImTextureID)c.tex, ImVec2(x0 + 1, clipY + 2),
                                    ImVec2(x1 - 1, clipY + CLIP_H - 2), uv0, uv1,
                                    IM_COL32(255, 255, 255, isDragged ? 160 : 220), 5.0f);
            }
        } else if (c.kind == Clip::Text) {
            std::string one = FirstLine(c.text);
            ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
            float px = 20.0f;
            ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, one.c_str());
            float bw = x1 - x0 - 10;
            if (ts.x > bw && ts.x > 0) { px *= bw / ts.x; ts = tf->CalcTextSizeA(px, FLT_MAX, 0, one.c_str()); }
            if (bw > 10)
                dl->AddText(tf, px, ImVec2(x0 + (x1 - x0 - ts.x) * 0.5f,
                                           clipY + (CLIP_H - ts.y) * 0.5f - 4),
                            IM_COL32(245, 240, 230, 255), one.c_str());
        }
        dl->AddRect(ImVec2(x0 + 1, clipY + 2), ImVec2(x1 - 1, clipY + CLIP_H - 2),
                    IM_COL32(255, 255, 255, 40), 5.0f);

        char dur[32];
        snprintf(dur, 32, "%.2fs", c.duration);
        ImVec2 ds = ImGui::CalcTextSize(dur);
        if (x1 - x0 > ds.x + 12) {
            dl->AddRectFilled(ImVec2(x0 + 5, clipY + CLIP_H - ds.y - 9),
                              ImVec2(x0 + 11 + ds.x, clipY + CLIP_H - 5),
                              IM_COL32(0, 0, 0, 170), 3.0f);
            dl->AddText(ImVec2(x0 + 8, clipY + CLIP_H - ds.y - 7),
                        IM_COL32(240, 240, 245, 255), dur);
        }

        // hit zones — a cut between two clips always grabs the RIGHT edge of the
        // left clip; interior left edges are not grabbable (only clip 0's).
        // A video's in-point is trimmable too: the right half of a cut grabs the
        // next clip's left edge when that clip is video.
        if (hovered && io.MousePos.y >= clipY && io.MousePos.y <= clipY + CLIP_H) {
            if (hotEdgeClip == -1 && fabsf(io.MousePos.x - x1) <= EDGE) {
                bool nextIsVideo = i + 1 < (int)g_clips.size() &&
                                   g_clips[i + 1]->kind == Clip::Video;
                if (nextIsVideo && io.MousePos.x > x1) { hotEdgeClip = i + 1; hotEdgeSide = -1; }
                else { hotEdgeClip = i; hotEdgeSide = +1; }
            } else if (hotEdgeClip == -1 && i == 0 && fabsf(io.MousePos.x - x0) <= EDGE) {
                hotEdgeClip = 0; hotEdgeSide = -1;
            } else if (hotEdgeClip == -1 && io.MousePos.x > x0 && io.MousePos.x < x1) {
                hotBody = i;
            }
        }
    }

    if (hotEdgeClip >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // ---- audio row
    float audioY = clipY + CLIP_H + GAP;
    dl->AddRectFilled(ImVec2(origin.x, audioY), ImVec2(origin.x + avail.x, audioY + AUDIO_H),
                      IM_COL32(18, 18, 22, 255));
    bool hotAudio = false;
    int hotAudioEdge = 0;                  // -1 left, +1 right
    if (g_song.loaded) {
        double blockLen = g_song.trimEnd - g_song.trimStart;
        float ax0 = SecToX(g_song.offset), ax1 = SecToX(g_song.offset + blockLen);
        bool dragging = g_tl.drag == TimelineState::Audio ||
                        g_tl.drag == TimelineState::AudioLeft ||
                        g_tl.drag == TimelineState::AudioRight;
        dl->AddRectFilled(ImVec2(ax0, audioY + 2), ImVec2(ax1, audioY + AUDIO_H - 2),
                          dragging ? IM_COL32(58, 96, 74, 255) : IM_COL32(40, 70, 54, 255), 5.0f);
        // waveform
        float cy = audioY + AUDIO_H * 0.5f;
        float amp = (AUDIO_H - 10) * 0.5f;
        float px0 = ax0 > origin.x ? ax0 : origin.x;
        float px1 = ax1 < origin.x + avail.x ? ax1 : origin.x + avail.x;
        size_t nPeaks = g_song.peaks.size() / 2;
        for (float x = px0; x < px1; x += 1.0f) {
            double tIn = XToSec(x) - g_song.offset + g_song.trimStart;
            size_t p = (size_t)(tIn * SAMPLE_RATE / g_song.framesPerPeak);
            if (p >= nPeaks) break;
            float lo = g_song.peaks[p * 2], hi = g_song.peaks[p * 2 + 1];
            dl->AddLine(ImVec2(x, cy - hi * amp), ImVec2(x, cy - lo * amp),
                        IM_COL32(140, 220, 170, 200));
        }
        dl->AddRect(ImVec2(ax0, audioY + 2), ImVec2(ax1, audioY + AUDIO_H - 2),
                    IM_COL32(255, 255, 255, 40), 5.0f);
        dl->AddText(ImVec2((ax0 > origin.x ? ax0 : origin.x) + 6, audioY + 5),
                    IM_COL32(230, 245, 235, 220), g_song.label.c_str());
        if (hovered && io.MousePos.y >= audioY && io.MousePos.y <= audioY + AUDIO_H) {
            if (fabsf(io.MousePos.x - ax0) <= EDGE) { hotAudio = true; hotAudioEdge = -1; }
            else if (fabsf(io.MousePos.x - ax1) <= EDGE) { hotAudio = true; hotAudioEdge = +1; }
            else if (io.MousePos.x > ax0 && io.MousePos.x < ax1) hotAudio = true;
        }
        if (hotAudioEdge != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else {
        dl->AddText(ImVec2(origin.x + 8, audioY + AUDIO_H * 0.5f - 8),
                    IM_COL32(110, 110, 120, 255), "No music — add a song with the toolbar");
    }

    // ---- interactions
    bool overRuler = hovered && io.MousePos.y >= rulerY && io.MousePos.y <= rulerY + RULER_H;
    if (ImGui::IsItemActivated()) {
        if (hotEdgeClip >= 0) {
            g_tl.drag = hotEdgeSide < 0 ? TimelineState::LeftEdge : TimelineState::RightEdge;
            g_tl.dragIndex = hotEdgeClip;
            g_tl.dragStartVal = g_clips[hotEdgeClip]->duration;
            g_tl.dragStartVal2 = g_clips[hotEdgeClip]->trimIn;
            g_tl.dragStartMouseX = io.MousePos.x;
        } else if (hotAudio) {
            g_tl.drag = hotAudioEdge < 0 ? TimelineState::AudioLeft
                      : hotAudioEdge > 0 ? TimelineState::AudioRight
                                         : TimelineState::Audio;
            g_tl.dragStartVal = g_song.offset;
            g_tl.dragStartVal2 = hotAudioEdge > 0 ? g_song.trimEnd : g_song.trimStart;
            g_tl.dragStartMouseX = io.MousePos.x;
        } else if (hotBody >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            g_tl.editIndex = hotBody;
            g_tl.editValue = g_clips[hotBody]->duration;
            if (g_clips[hotBody]->kind == Clip::Text) {
                g_tl.editOpenText = true;   // opened below, outside the InvisibleButton
            } else {
                ImGui::OpenPopup("edit_duration");
            }
        } else if (hotBody >= 0) {
            g_tl.drag = TimelineState::Move;
            g_tl.dragIndex = hotBody;
            g_tl.dragStartMouseX = io.MousePos.x;
        } else if (overRuler || hovered) {
            g_tl.drag = TimelineState::Scrub;
        }
    }

    if (active && g_tl.drag != TimelineState::None) {
        double dSec = (double)(io.MousePos.x - g_tl.dragStartMouseX) / g_tl.pps;
        switch (g_tl.drag) {
        case TimelineState::RightEdge: {
            Clip& c = *g_clips[g_tl.dragIndex];
            double d = SnapDuration(g_tl.dragStartVal + dSec);
            double mx = MaxDuration(c);
            c.duration = d > mx ? mx : d;
            ImGui::SetTooltip("%.3f s", c.duration);
            break;
        }
        case TimelineState::LeftEdge: {
            Clip& c = *g_clips[g_tl.dragIndex];
            if (c.kind == Clip::Video) {    // move the in-point, keep the out-point
                double outPoint = g_tl.dragStartVal2 + g_tl.dragStartVal;
                double in = g_tl.dragStartVal2 + dSec;
                if (in < 0) in = 0;
                if (in > outPoint - MinClipDur()) in = outPoint - MinClipDur();
                c.trimIn = in;
                c.duration = outPoint - in;
                ImGui::SetTooltip("in %.3f s  ·  %.3f s", c.trimIn, c.duration);
            } else {
                c.duration = SnapDuration(g_tl.dragStartVal - dSec);
                ImGui::SetTooltip("%.3f s", c.duration);
            }
            break;
        }
        case TimelineState::Move: {
            // reorder when the cursor crosses a neighbor's midpoint
            double t = XToSec(io.MousePos.x);
            double s = 0;
            int target = (int)g_clips.size() - 1;
            for (int i = 0; i < (int)g_clips.size(); i++) {
                double mid = s + g_clips[i]->duration * 0.5;
                if (t < mid) { target = i; break; }
                s += g_clips[i]->duration;
            }
            if (target != g_tl.dragIndex) {
                auto mv = std::move(g_clips[g_tl.dragIndex]);
                g_clips.erase(g_clips.begin() + g_tl.dragIndex);
                g_clips.insert(g_clips.begin() + target, std::move(mv));
                g_tl.dragIndex = target;
            }
            break;
        }
        case TimelineState::Audio:
        case TimelineState::AudioLeft:
        case TimelineState::AudioRight: {
            double blockLen0 = g_song.trimEnd - g_song.trimStart;
            // snap a timeline position to 0 and clip cuts
            auto snapPos = [&](double pos, double lenForEndSnap) {
                if (io.KeyAlt) return pos;
                double snapPx = 8.0 / g_tl.pps;
                double best = 1e18, bestTo = pos;
                auto consider = [&](double target) {
                    double d = fabs(pos - target);
                    if (d < snapPx && d < best) { best = d; bestTo = target; }
                };
                double s = 0;
                consider(0.0);
                for (auto& c : g_clips) { s += c->duration; consider(s); }
                if (lenForEndSnap > 0) consider(s - lenForEndSnap); // block end on video end
                return bestTo;
            };
            if (g_tl.drag == TimelineState::Audio) {
                g_song.offset = snapPos(g_tl.dragStartVal + dSec, blockLen0);
                ImGui::SetTooltip("offset %+.3f s", g_song.offset);
            } else if (g_tl.drag == TimelineState::AudioLeft) {
                // move the block's left edge: trims the song head, keeps content in place
                double edge = snapPos(g_tl.dragStartVal + dSec, 0);
                double newTrim = g_tl.dragStartVal2 + (edge - g_tl.dragStartVal);
                if (newTrim < 0) newTrim = 0;
                if (newTrim > g_song.trimEnd - 0.1) newTrim = g_song.trimEnd - 0.1;
                g_song.offset = g_tl.dragStartVal + (newTrim - g_tl.dragStartVal2);
                g_song.trimStart = newTrim;
                ImGui::SetTooltip("trim in %.3f s", newTrim);
            } else {
                double edge = snapPos(g_song.offset +
                                      (g_tl.dragStartVal2 - g_song.trimStart) + dSec, 0);
                double newEnd = edge - g_song.offset + g_song.trimStart;
                if (newEnd > g_song.duration) newEnd = g_song.duration;
                if (newEnd < g_song.trimStart + 0.1) newEnd = g_song.trimStart + 0.1;
                g_song.trimEnd = newEnd;
                ImGui::SetTooltip("trim out %.3f s", newEnd);
            }
            break;
        }
        case TimelineState::Scrub: {
            double t = XToSec(io.MousePos.x);
            double tot = TotalDuration();
            if (t < 0) t = 0;
            if (t > tot) t = tot;
            g_playhead.store(t);
            break;
        }
        default: break;
        }
    }
    if (ImGui::IsItemDeactivated()) { g_tl.drag = TimelineState::None; g_tl.dragIndex = -1; }

    // split the clip under the playhead
    if (g_tl.drag == TimelineState::None && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_S, false) && !g_clips.empty())
        SplitPoint(g_playhead.load());

    // delete hovered clip or song
    if (g_tl.drag == TimelineState::None &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        if (hotBody >= 0) {
            if (g_clips[hotBody]->tex && g_clips[hotBody]->kind != Clip::Video)
                g_clips[hotBody]->tex->Release();
            g_clips.erase(g_clips.begin() + hotBody);
        } else if (hotAudio) {
            bool wasPlaying = g_playing.exchange(false);
            Sleep(30);                     // let the audio callback drain before freeing pcm
            g_song = Song{};
            g_playing.store(wasPlaying);
        }
    }

    // exact-duration popup
    if (ImGui::BeginPopup("edit_duration")) {
        if (g_tl.editIndex >= 0 && g_tl.editIndex < (int)g_clips.size()) {
            Clip& ec = *g_clips[g_tl.editIndex];
            if (ec.kind == Clip::Video) {
                ImGui::Text("in %.3f s  ·  source %.2f s", ec.trimIn,
                            ec.vid ? ec.vid->duration : 0.0);
                ImGui::BeginDisabled(!ec.vid || !ec.vid->hasAudio);
                ImGui::Checkbox("keep this clip's audio", &ec.useAudio);
                ImGui::EndDisabled();
                if (ec.vid && !ec.vid->hasAudio) ImGui::TextDisabled("(no audio track)");
                ImGui::Separator();
            }
        }
        ImGui::TextUnformatted("Duration (seconds)");
        ImGui::SetNextItemWidth(140);
        if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0)) ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputDouble("##dur", &g_tl.editValue, 0, 0, "%.3f",
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        if (commit && g_tl.editIndex >= 0 && g_tl.editIndex < (int)g_clips.size()) {
            Clip& ec = *g_clips[g_tl.editIndex];
            double v = g_tl.editValue < MinClipDur() ? MinClipDur() : g_tl.editValue;
            double mx = MaxDuration(ec);
            ec.duration = v > mx ? mx : v;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_tl.editOpenText) {
        g_tl.editOpenText = false;
        Clip& c = *g_clips[g_tl.editIndex];
        snprintf(g_textBuf, sizeof(g_textBuf), "%s", c.text.c_str());
        g_textScale = c.textScale;
        g_textDur = c.duration;
        g_textTarget = g_tl.editIndex;
        ImGui::OpenPopup("Text card");
    }
    DrawTextCardPopup();

    // ---- playhead
    double ph = g_playhead.load();
    float phx = SecToX(ph);
    if (phx >= origin.x && phx <= origin.x + avail.x) {
        dl->AddLine(ImVec2(phx, rulerY), ImVec2(phx, audioY + AUDIO_H),
                    IM_COL32(255, 90, 90, 255), 2.0f);
        dl->AddTriangleFilled(ImVec2(phx - 6, rulerY), ImVec2(phx + 6, rulerY),
                              ImVec2(phx, rulerY + 8), IM_COL32(255, 90, 90, 255));
    }
    // keep playhead in view while playing
    if (g_playing.load()) {
        float viewW = avail.x / g_tl.pps;
        if (ph > g_tl.scrollSec + viewW * 0.95 || ph < g_tl.scrollSec)
            g_tl.scrollSec = (float)ph - viewW * 0.1f;
    }
}

// ------------------------------------------------------- export settings UI

// Everything that shapes the encode lives here so the toolbar stays one row.
static void DrawExportSettingsPopup() {
    if (!ImGui::BeginPopupModal("Export settings", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::SeparatorText("Canvas");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("target", &g_preset, PRESET_ITEMS);
    if (g_preset == PRESET_CUSTOM) {
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##cw", &g_customW, 0, 0);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted("x");
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##ch", &g_customH, 0, 0);
        g_customW = g_customW < 16 ? 16 : (g_customW > 7680 ? 7680 : g_customW);
        g_customH = g_customH < 16 ? 16 : (g_customH > 7680 ? 7680 : g_customH);
        ImGui::SameLine();
        ImGui::TextDisabled("pixels");
    }
    ImGui::BeginDisabled(g_preset == PRESET_ORIGINAL);
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("fit", &g_fit, FIT_ITEMS);
    ImGui::EndDisabled();
    if (g_fit == FIT_CROP && g_preset != PRESET_ORIGINAL)
        ImGui::TextDisabled("Crop to fill loses the edges of off-aspect sources.");

    ImGui::SetNextItemWidth(260);
    static const int FPS_CHOICES[] = { 24, 25, 30, 50, 60 };
    int fpsIdx = 2;
    for (int i = 0; i < 5; i++) if (FPS_CHOICES[i] == g_fps) fpsIdx = i;
    if (ImGui::Combo("frame rate", &fpsIdx, "24 fps\0" "25 fps\0" "30 fps\0" "50 fps\0" "60 fps\0")) {
        g_fps = FPS_CHOICES[fpsIdx];
        g_fpsAuto = false;
    }

    ImGui::SeparatorText("Video");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("codec", &g_vcodec, CODEC_ITEMS);
    if (IsNvenc(g_vcodec))
        ImGui::TextDisabled("NVENC needs an NVIDIA GPU — much faster, slightly larger files.");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("encoder speed", &g_speed, SPEED_ITEMS);
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("rate control", &g_rateMode, "Constant quality (CRF)\0" "Target bitrate\0");
    if (g_rateMode == RM_CRF) {
        ImGui::SetNextItemWidth(260);
        ImGui::SliderInt("quality", &g_crf, 10, 30, "crf %d");
        ImGui::TextDisabled("lower = better and bigger · 15 archival · 20 upload-ready");
    } else {
        ImGui::SetNextItemWidth(260);
        ImGui::SliderFloat("bitrate", &g_targetMbps, 2.0f, 120.0f, "%.1f Mbps");
    }
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("container", &g_container, CONTAINER_ITEMS);
    ImGui::BeginDisabled(g_container == CT_MKV);
    ImGui::Checkbox("web fast start (moov atom first)", &g_faststart);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Audio");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("aac bitrate", &g_abrIdx, AUDIO_KBPS_ITEMS);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("music level", &g_musicVol, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("normalise loudness to -14 LUFS", &g_loudnorm);
    if (g_loudnorm) ImGui::TextDisabled("Matches what streaming platforms target.");

    ImGui::SeparatorText("Fades");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("fade in", &g_fadeIn, 0.0f, 3.0f, "%.2f s");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("fade out", &g_fadeOut, 0.0f, 3.0f, "%.2f s");

    ImGui::SeparatorText("File name");
    static char prefix[128] = "";
    static bool prefixSynced = false;
    if (!prefixSynced) { snprintf(prefix, sizeof(prefix), "%s", Narrow(g_namePrefix).c_str());
                         prefixSynced = true; }
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("name", "(first clip's name)", prefix, sizeof(prefix)))
        g_namePrefix = Widen(prefix);
    ImGui::TextDisabled("Save dialog opens with: %s", Narrow(DefaultOutputName()).c_str());

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ----------------------------------------------------------------- main UI

static void DrawApp() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // ---- toolbar
    if (ImGui::Button("  Add Images  ")) {
        auto files = PickFiles(true, L"Images",
                               L"*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff;*.webp;*.gif");
        if (!files.empty()) AddFiles(files);
    }
    ImGui::SameLine();
    if (ImGui::Button("  Add Video  ")) {
        auto files = PickFiles(true, L"Video",
                               L"*.mp4;*.mov;*.m4v;*.mkv;*.webm;*.avi;*.wmv;*.mpg;*.mpeg;*.m2ts;*.ts");
        if (!files.empty()) AddFiles(files);
    }
    ImGui::SameLine();
    if (ImGui::Button("  Add Text  ")) {
        g_textBuf[0] = 0;
        g_textScale = 0.13f;
        g_textDur = 2.0;
        g_textTarget = -1;
        ImGui::OpenPopup("Text card");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_clips.size() < 2);
    if (ImGui::Button("  Reverse  ")) {
        std::reverse(g_clips.begin(), g_clips.end());
        g_playhead.store(0.0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool songBusy = g_songLoading.load();
    ImGui::BeginDisabled(songBusy);
    if (ImGui::Button("  Add Music  ")) {
        auto f = PickFiles(false, L"Audio", L"*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus;*.wma");
        if (!f.empty()) StartSongLoad(f[0]);
    }
    ImGui::SameLine();
    static char ytUrl[512] = "";
    ImGui::SetNextItemWidth(280);
    bool ytGo = ImGui::InputTextWithHint("##yt", "paste YouTube URL…", ytUrl,
                                         sizeof(ytUrl), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 2);
    ytGo |= ImGui::Button("Get Song");
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
    ImGui::SameLine();
    double tot = TotalDuration();
    ImGui::TextDisabled("|  %d clips · %d:%05.2f  |  space: play · drag edges: trim ·"
                        " double-click: exact time / edit text · s: split at playhead ·"
                        " shift: fine · alt: no snap · del: remove clip/song",
                        (int)g_clips.size(), (int)tot / 60, fmod(tot, 60.0));

    // ---- export row
    ImGui::SetNextItemWidth(230);
    ImGui::Combo("##preset", &g_preset, PRESET_ITEMS);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    static const int FPS_CHOICES[] = { 24, 25, 30, 50, 60 };
    int fpsIdx = 2;
    for (int i = 0; i < 5; i++) if (FPS_CHOICES[i] == g_fps) fpsIdx = i;
    if (ImGui::Combo("##fps", &fpsIdx, "24 fps\0" "25 fps\0" "30 fps\0" "50 fps\0" "60 fps\0")) {
        g_fps = FPS_CHOICES[fpsIdx];
        g_fpsAuto = false;                 // explicit choice wins over incoming footage
    }
    ImGui::SameLine();
    if (ImGui::Button("  Settings…  ")) ImGui::OpenPopup("Export settings");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::BeginDisabled(AnyProxyBuilding());
    if (ImGui::Combo("##pv", &g_preview, "Preview 360p\0Preview 540p\0Preview 720p\0"))
        RebuildProxies();
    ImGui::EndDisabled();
    if (g_song.loaded) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("##mvol", &g_musicVol, 0.0f, 1.0f, "music %.2f");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_clips.empty() || g_export.active);
    {
        const char* btn = g_container == CT_MOV ? "  Export MOV  "
                        : g_container == CT_MKV ? "  Export MKV  " : "  Export MP4  ";
        if (ImGui::Button(btn)) {
            std::wstring out = PickSaveVideo(DefaultOutputName(), ContainerExt(g_container),
                                             SuggestExportDir());
            if (!out.empty()) StartExport(out);
        }
    }
    ImGui::EndDisabled();
    {   // what the encoder will actually be told to do
        int W, H;
        ResolveCanvas(&W, &H);
        const char* codec = g_vcodec == VC_X264 ? "H.264" : g_vcodec == VC_X265 ? "H.265"
                          : g_vcodec == VC_NVENC_H264 ? "H.264 NVENC" : "H.265 NVENC";
        ImGui::SameLine();
        if (g_rateMode == RM_CRF)
            ImGui::TextDisabled("|  %dx%d @%dfps · %s crf %d · AAC %dk%s",
                                W, H, g_fps, codec, g_crf, AUDIO_KBPS[g_abrIdx],
                                g_loudnorm ? " · -14 LUFS" : "");
        else
            ImGui::TextDisabled("|  %dx%d @%dfps · %s %.1f Mbps · AAC %dk%s",
                                W, H, g_fps, codec, g_targetMbps, AUDIO_KBPS[g_abrIdx],
                                g_loudnorm ? " · -14 LUFS" : "");
    }
    DrawExportSettingsPopup();

    if (songBusy || !g_songStatus.empty()) {
        ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "%s",
                           songBusy && g_songStatus.empty() ? "loading audio…"
                                                            : g_songStatus.c_str());
    }
    if (!g_intakeStatus.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), "%s", g_intakeStatus.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##intake")) g_intakeStatus.clear();
    }
    if (g_export.active || !g_export.message.empty()) {
        if (g_export.active) {
            ImGui::ProgressBar(g_export.progress, ImVec2(-1, 0));
        } else {
            ImGui::TextColored(g_export.failed ? ImVec4(1, 0.45f, 0.45f, 1)
                                               : ImVec4(0.5f, 1, 0.6f, 1),
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
        }
    }
    ImGui::Separator();

    // ---- preview
    float tlHeight = 170.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 prevSize(avail.x, avail.y - tlHeight);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + prevSize.x, p0.y + prevSize.y), IM_COL32(8, 8, 10, 255));

    double ph = g_playhead.load();
    double clipStart = 0;
    int ci = ClipAt(ph, &clipStart);
    if (ci >= 0 && g_clips[ci]->kind == Clip::Video && g_clips[ci]->vid) {
        Clip& c = *g_clips[ci];
        ID3D11ShaderResourceView* fr = ProxyFrame(*c.vid, c.trimIn + (ph - clipStart));
        if (fr) {
            float ar = c.vid->aspect > 0 ? c.vid->aspect : 1.0f;
            float w = prevSize.x, h = w / ar;
            if (h > prevSize.y) { h = prevSize.y; w = h * ar; }
            ImVec2 q0(p0.x + (prevSize.x - w) * 0.5f, p0.y + (prevSize.y - h) * 0.5f);
            dl->AddImage((ImTextureID)fr, q0, ImVec2(q0.x + w, q0.y + h));
        } else {
            const char* msg = c.vid->probed.load() ? "building preview…" : "reading video…";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(p0.x + (prevSize.x - ts.x) * 0.5f,
                               p0.y + (prevSize.y - ts.y) * 0.5f),
                        IM_COL32(140, 140, 150, 255), msg);
        }
    } else if (ci >= 0 && g_clips[ci]->kind == Clip::Text) {
        // black frame at the export aspect, text centred, same metrics as drawtext
        float fa = PresetAspect();
        float fh = prevSize.y, fw = fh * fa;
        if (fw > prevSize.x) { fw = prevSize.x; fh = fw / fa; }
        ImVec2 f0(p0.x + (prevSize.x - fw) * 0.5f, p0.y + (prevSize.y - fh) * 0.5f);
        dl->AddRectFilled(f0, ImVec2(f0.x + fw, f0.y + fh), IM_COL32(0, 0, 0, 255));

        Clip& c = *g_clips[ci];
        ImFont* tf = g_titleFont ? g_titleFont : ImGui::GetFont();
        float px = c.textScale * fh;
        float lineH = px * 1.25f;
        std::vector<std::string> lines;
        for (size_t b = 0, e; ; b = e + 1) {
            e = c.text.find('\n', b);
            lines.push_back(c.text.substr(b, e == std::string::npos ? e : e - b));
            if (e == std::string::npos) break;
        }
        float blockH = lineH * lines.size();
        float y = f0.y + (fh - blockH) * 0.5f;
        for (auto& ln : lines) {
            ImVec2 ts = tf->CalcTextSizeA(px, FLT_MAX, 0, ln.c_str());
            dl->AddText(tf, px, ImVec2(f0.x + (fw - ts.x) * 0.5f, y),
                        IM_COL32(255, 255, 255, 255), ln.c_str());
            y += lineH;
        }
    } else if (ci >= 0 && g_clips[ci]->tex) {
        Clip& c = *g_clips[ci];
        float w = prevSize.x, h = w / c.texAspect;
        if (h > prevSize.y) { h = prevSize.y; w = h * c.texAspect; }
        ImVec2 q0(p0.x + (prevSize.x - w) * 0.5f, p0.y + (prevSize.y - h) * 0.5f);
        dl->AddImage((ImTextureID)c.tex, q0, ImVec2(q0.x + w, q0.y + h));
    } else if (g_clips.empty()) {
        const char* msg  = "Drop images, video, folders or a song anywhere on this window";
        const char* msg2 = "…or use the buttons above";
        ImVec2 ts  = ImGui::CalcTextSize(msg);
        ImVec2 ts2 = ImGui::CalcTextSize(msg2);
        float cy = p0.y + prevSize.y * 0.5f;
        dl->AddText(ImVec2(p0.x + (prevSize.x - ts.x) * 0.5f, cy - ts.y - 4),
                    IM_COL32(150, 150, 165, 255), msg);
        dl->AddText(ImVec2(p0.x + (prevSize.x - ts2.x) * 0.5f, cy + 6),
                    IM_COL32(110, 110, 120, 255), msg2);
    }
    // timecode overlay
    char tc[64];
    snprintf(tc, 64, "%s  %d:%05.2f / %d:%05.2f", g_playing.load() ? "▶" : "⏸",
             (int)ph / 60, fmod(ph, 60.0), (int)tot / 60, fmod(tot, 60.0));
    dl->AddText(ImVec2(p0.x + 10, p0.y + 8), IM_COL32(255, 255, 255, 200), tc);
    ImGui::Dummy(prevSize);

    // ---- timeline
    DrawTimeline();

    // ---- transport
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) {
        if (!g_playing.load() && g_playhead.load() >= tot - 1e-6) g_playhead.store(0.0);
        g_playing.store(!g_playing.load());
    }
    if (g_playing.load() && g_playhead.load() >= tot) {
        g_playing.store(false);
        g_playhead.store(tot);
    }

    ImGui::End();
}

static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.055f, 0.055f, 0.07f, 1);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.18f, 0.24f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.28f, 0.38f, 1);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.30f, 0.36f, 0.50f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.35f, 0.65f, 0.45f, 1);
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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
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
        PumpExport();
        if (g_songPending.valid() &&
            g_songPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            g_songPending.get();

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
    }

    g_playing.store(false);
    if (g_audioReady) ma_device_uninit(&g_audioDevice);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    for (auto& c : g_clips) if (c->tex && c->kind != Clip::Video) c->tex->Release();
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
    return 0;
}
