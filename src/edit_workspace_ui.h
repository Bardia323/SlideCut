// Editor workspace: source browsing, deliberate trims, rhythm and branch comparison.
// Included after the existing editor functions so the original workflows stay intact.
struct SourceThumb {
    std::future<LoadedImage> pending;
    ID3D11ShaderResourceView* tex = nullptr;
    float aspect = 1.777f;
    double touched = 0;
    ~SourceThumb() { RetireTexture(tex); }
};
static std::map<std::wstring, std::unique_ptr<SourceThumb>> g_sourceThumbs;
struct ExactFrame {
    std::future<LoadedImage> pending;
    std::wstring requested, completed;
    ID3D11ShaderResourceView* tex = nullptr;
    float aspect = 1.777f;
};
static ExactFrame g_exactFrames[2];
static int g_cutIndex = -1, g_trimMode = 0, g_trimUid = -1;
static float g_trimDelta = 0;
static int g_compareBranch = -1;
static std::wstring g_libraryPick;
static void ClearWorkspacePreviews() {
    g_sourceThumbs.clear();
    g_libraryPick.clear();
    g_trimUid = -1; g_trimDelta = 0; g_cutIndex = -1;
    for (auto& f : g_exactFrames) {
        f.completed.clear();
        RetireTexture(f.tex); f.tex = nullptr;
    }
}

static KV LibraryKV(const LibraryItem& item) {
    auto records = WorkspaceRecords(item.clipText);
    for (auto& r : records) if (r.first == "clip") return r.second;
    KV kv;
    kv.v = {{"path", Narrow(item.path)}, {"kind", std::to_string(item.kind)},
            {"label", item.label}, {"duration", "3"}};
    return kv;
}
static void SetKV(KV& kv, const char* name, const std::string& value) {
    for (auto& p : kv.v) if (p.first == name) { p.second = value; return; }
    kv.v.push_back({name, value});
}
static ID3D11ShaderResourceView* StillThumb(const std::wstring& path, float& aspect) {
    if (path.empty()) return nullptr;
    auto& ptr = g_sourceThumbs[path];
    if (!ptr) {
        ptr = std::make_unique<SourceThumb>();
        ptr->pending = std::async(std::launch::async, DecodeImage, path);
    }
    auto& t = *ptr;
    t.touched = ImGui::GetTime();
    if (t.pending.valid() && t.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto img = t.pending.get();
        if (img.ok) {
            t.tex = CreateTextureRGBA(img.px.data(), img.w, img.h);
            t.aspect = (float)img.w / img.h;
        }
    }
    aspect = t.aspect;
    return t.tex;
}
// Only two source-frame jobs may run at once; stale results never replace the
// frame currently requested. Proxy playback remains available during decoding.
static ID3D11ShaderResourceView* ExactSourceFrame(int slot, const std::wstring& path,
                                                 double at, float& aspect) {
    auto& f = g_exactFrames[slot];
    auto key = path + L"@" + std::to_wstring(llround(at * 1000000));
    if (f.pending.valid() && f.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto img = f.pending.get();
        RetireTexture(f.tex); f.tex = nullptr;
        f.completed = f.requested;
        if (img.ok) {
            f.tex = CreateTextureRGBA(img.px.data(), img.w, img.h);
            f.aspect = (float)img.w / img.h;
        }
    }
    if (!f.pending.valid() && f.completed != key) {
        f.requested = key;
        f.pending = std::async(std::launch::async, [path, at] {
            wchar_t dir[MAX_PATH], temp[MAX_PATH];
            LoadedImage img;
            if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"scf", 0, temp)) return img;
            std::wstring cmd = L"ffmpeg -v error -nostdin -y -ss " + std::to_wstring(at) +
                L" -i \"" + path + L"\" -frames:v 1 -an -vf scale=-2:540 -c:v png -f image2 \"" + temp + L"\"";
            if (RunHidden(cmd)) img = DecodeImage(temp);
            DeleteFileW(temp);
            return img;
        });
    }
    if (f.completed != key) return nullptr;
    aspect = f.aspect;
    return f.tex;
}

static void SourcePicture(const std::wstring& path, int kind, const std::string& text,
                          double at, ImVec2 size, int exactSlot = -1) {
    size.x = fmaxf(1, size.x); size.y = fmaxf(1, size.y);
    ImVec2 p = ImGui::GetCursorScreenPos(), end(p.x + size.x, p.y + size.y);
    auto dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, end, IM_COL32(12, 16, 22, 255), 6);
    ImGui::Dummy(size);
    ID3D11ShaderResourceView* tex = nullptr;
    float aspect = 1.777f;
    if (kind == Clip::Video && !path.empty()) {
        auto v = GetVideoSource(path);
        if (v->probed.load()) {
            at = std::clamp(at, 0.0, fmax(0.0, v->duration - 1.0 / fmax(1.0, v->fps)));
            aspect = v->aspect;
        }
        tex = exactSlot >= 0 ? ExactSourceFrame(exactSlot, path, at, aspect) : ProxyFrame(*v, at);
    } else if (kind == Clip::Image) tex = StillThumb(path, aspect);
    if (tex) DrawFitted(dl, (ImTextureID)tex, aspect, p, end, false, IM_COL32_WHITE);
    else {
        std::string message = kind == Clip::Text ? text :
            kind == 4 ? "AUDIO SOURCE" : kind == Clip::Nest ? "Nested sequence - use program playback" :
            exactSlot >= 0 ? "Decoding source frame..." : "Preparing preview...";
        dl->PushClipRect(p, end, true);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 12, p.y + 12),
                    IM_COL32(204, 214, 224, 255), message.c_str(), nullptr, size.x - 24);
        dl->PopClipRect();
    }
}
static void SeekWorkspace(double at) {
    MixGuard lock;
    g_playing.store(false);
    g_audition = false;
    g_playhead.store(std::clamp(at, 0.0, TimelineEnd()));
    g_tl.scrollSec = (float)fmax(0.0, at - 1);
}
static void AuditionRange(double a, double b) {
    if (b <= a) return;
    MixGuard lock;
    g_auditionIn = fmax(0.0, a); g_auditionOut = fmin(TimelineEnd(), b);
    g_audition = true; g_playhead.store(g_auditionIn); g_playing.store(true);
}
static int CurrentCut() {
    int i = g_selTrack == -1 ? g_sel : -1;
    if (i < 0 || i >= (int)g_clips.size()) i = ClipAt(g_playhead.load());
    return i;
}
static double SourceTime(const Clip& c, double local, double duration, double trim) {
    double frame = 1.0 / (c.vid && c.vid->fps > 0 ? c.vid->fps : fmax(1, g_fps));
    local = std::clamp(local, 0.0, fmax(0.0, duration - frame));
    return trim + (c.reversed ? fmax(0.0, duration - frame - local) : local);
}
static double TrimLimit(const Clip& a, const Clip* b, double delta, int mode) {
    double lo = MinClipDur() - a.duration, hi = 3600.0;
    auto known = [](const Clip& c) { return c.kind == Clip::Video && c.vid && c.vid->probed.load(); };
    if (mode == 2) {
        if (!known(a)) return 0;
        return std::clamp(delta, -a.trimIn, fmax(-a.trimIn, a.vid->duration - a.trimIn - a.duration));
    }
    if (a.kind == Clip::Nest || (a.kind == Clip::Video && !known(a))) return 0;
    if (known(a)) hi = a.reversed ? a.trimIn : a.vid->duration - a.trimIn - a.duration;
    if (mode == 0) {
        if (!b || b->kind == Clip::Nest || (b->kind == Clip::Video && !known(*b))) return 0;
        hi = fmin(hi, b->duration - MinClipDur());
        if (known(*b)) lo = fmax(lo, b->reversed ?
            b->trimIn + b->duration - b->vid->duration : -b->trimIn);
    }
    // Preserve the effective length of existing dissolves. This makes a roll
    // truly timing-neutral and a ripple's displayed delta exact.
    auto minimumForFades = [&](const Clip& c) {
        for (int i = 0; i < (int)g_clips.size(); ++i) if (g_clips[i].get() == &c) {
            double minimum = MinClipDur();
            int prev = PrevVisibleClip(i), next = NextVisibleClip(i);
            if (prev >= 0) minimum = fmax(minimum,
                ClampFade(g_clips[prev]->xfade, g_clips[prev]->duration, c.duration) / .9);
            if (next >= 0) minimum = fmax(minimum,
                ClampFade(c.xfade, c.duration, g_clips[next]->duration) / .9);
            return minimum;
        }
        return MinClipDur();
    };
    lo = fmax(lo, minimumForFades(a) - a.duration);
    if (mode == 0 && b) hi = fmin(hi, b->duration - minimumForFades(*b));
    return hi < lo ? 0 : std::clamp(delta, lo, hi);
}
static void DrawConsequences(double from, double delta, int after, bool ripple) {
    ImGui::Text("Consequence preview  %+.3f s", ripple ? delta : 0.0);
    if (!ripple || fabs(delta) < 1e-7) {
        ImGui::TextWrapped("Downstream clips, markers, captions and audio stay in place.");
        return;
    }
    if (ImGui::BeginChild("##consequences", ImVec2(0, 112), true)) {
        for (int i = after; i < (int)g_clips.size(); ++i)
            if (!g_clips[i]->skip) ImGui::Text("%s  %+.3f s%s", g_clips[i]->label.c_str(), delta,
                g_clips[i]->ovlOn ? " (including caption)" : "");
        for (auto& t : g_over) for (auto& c : t->clips) {
            if (c->start >= from - 1e-9)
                ImGui::Text("%s  %+.3f s", c->label.c_str(), fmax(0.0, c->start + delta) - c->start);
            else if (c->start + c->duration > from)
                ImGui::Text("%s  stays; spans edit", c->label.c_str());
        }
        for (auto& t : g_atracks) for (auto& b : t->blocks) {
            if (b->offset >= from - 1e-9) ImGui::Text("Audio: %s  %+.3f s", b->label.c_str(), delta);
            else if (b->offset + b->trimEnd - b->trimStart > from)
                ImGui::Text("Audio: %s  stays; spans edit", b->label.c_str());
        }
        for (auto& m : g_editMarkers) if (m.seq == CurSeqId() && m.time >= from - 1e-9)
            ImGui::Text("%s: %s  %+.3f s", m.scene ? "Scene" : "Marker", m.label.c_str(),
                        fmax(0.0, m.time + delta) - m.time);
        ImGui::TextWrapped("Aspect automation stays at its absolute time. Captions attached to clips follow their clips.");
    }
    ImGui::EndChild();
}
static void DrawCutControls() {
    int i = CurrentCut();
    if (i < 0 || i >= (int)g_clips.size()) {
        ImGui::TextWrapped("Select a base-track shot to inspect its outgoing cut.");
        return;
    }
    auto& a = *g_clips[i];
    int next = NextVisibleClip(i);
    Clip* b = next >= 0 ? g_clips[next].get() : nullptr;
    if (g_trimUid != a.uid) { g_trimUid = a.uid; g_trimDelta = 0; }
    ImGui::TextWrapped("OUT  %s", a.label.c_str());
    ImGui::TextWrapped("IN   %s", b ? b->label.c_str() : "End of film");
    if (ImGui::Button("Previous cut")) {
        int previous = PrevVisibleClip(i);
        if (previous >= 0) { g_sel = previous; g_selTrack = -1; SelSet(g_clips[previous]->uid); }
        g_trimDelta = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next cut") && next >= 0) {
        g_sel = next; g_selTrack = -1; SelSet(g_clips[next]->uid); g_trimDelta = 0;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##trimMode", &g_trimMode, "Roll cut (keep timing)\0Ripple out-point\0Slip source (keep timing)\0"))
        g_trimDelta = 0;
    ImGui::TextWrapped("Stage a change, inspect both sides, then Apply. Reset discards the proposal.");
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##proposal", &g_trimDelta, 1.0f / fmax(1, g_fps), -3600, 3600, "%+.3f s");
    if (ImGui::Button("-1 frame")) g_trimDelta -= 1.0f / fmax(1, g_fps);
    ImGui::SameLine();
    if (ImGui::Button("+1 frame")) g_trimDelta += 1.0f / fmax(1, g_fps);
    g_trimDelta = (float)TrimLimit(a, b, g_trimDelta, g_trimMode);
    std::vector<BaseSpan> layout; BaseLayout(layout);
    bool ripple = g_trimMode == 1;
    DrawConsequences(layout[i].end, g_trimDelta, i + 1, ripple);
    ImGui::BeginDisabled(fabs(g_trimDelta) < 1e-7 || g_tl.drag != TimelineState::None);
    if (ImGui::Button("Apply trim")) {
        g_playing.store(false); g_audition = false;
        UndoCapture();
        double d = g_trimDelta;
        if (g_trimMode == 2) a.trimIn += d;
        else {
            a.duration += d;
            if (a.kind == Clip::Video && a.reversed) a.trimIn -= d;
            if (!ripple && b) {
                b->duration -= d;
                if (b->kind == Clip::Video && !b->reversed) b->trimIn += d;
            } else RippleOthers(layout[i].end, d);
        }
        g_trimDelta = 0;
        UndoCapture();
        g_projectStatus = "Trim applied - Ctrl+Z to reconsider";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset proposal")) g_trimDelta = 0;
    if (ImGui::Button("Audition cut")) AuditionRange(layout[i].end - 1.5, layout[i].end + 1.5);
    ImGui::SameLine(); ImGui::Checkbox("Loop", &g_auditionLoop);
    ImGui::TextDisabled("Audition plays the applied edit with timeline audio.");
    if (g_tl.drag == TimelineState::LeftEdge || g_tl.drag == TimelineState::RightEdge)
        DrawConsequences(g_tl.rippleAt, -g_tl.holdShift, g_tl.dragIndex + 1, g_tl.holdFrom >= 0);
}
static void DrawCutViewer(ImVec2 size) {
    int i = CurrentCut();
    if (g_tl.drag == TimelineState::LeftEdge || g_tl.drag == TimelineState::RightEdge)
        i = g_tl.previewEdge < 0 ? PrevVisibleClip(g_tl.dragIndex) : g_tl.dragIndex;
    if (i < 0 || i >= (int)g_clips.size()) {
        ImGui::TextWrapped("Select a shot or trim a seam to compare outgoing and incoming sources.");
        ImGui::Dummy(ImVec2(size.x, fmaxf(20, size.y - 30)));
        return;
    }
    int j = NextVisibleClip(i);
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float width = fmaxf(30, (size.x - gap) * .5f);
    bool proposal = g_trimUid == g_clips[i]->uid && g_tl.drag == TimelineState::None;
    double delta = proposal ? TrimLimit(*g_clips[i], j >= 0 ? g_clips[j].get() : nullptr, g_trimDelta, g_trimMode) : 0;
    for (int side = 0; side < 2; side++) {
        if (side) ImGui::SameLine();
        ImGui::BeginChild(side ? "##incoming" : "##outgoing", ImVec2(width, size.y), true);
        int at = side ? j : i;
        if (at >= 0) {
            auto& c = *g_clips[at];
            double duration = c.duration, trim = c.trimIn;
            if (!side) {
                if (g_trimMode == 2) trim += delta;
                else { duration += delta; if (c.reversed && c.kind == Clip::Video) trim -= delta; }
            } else if (g_trimMode == 0) {
                duration -= delta; if (!c.reversed && c.kind == Clip::Video) trim += delta;
            }
            ImGui::TextUnformatted(side ? "INCOMING" : "OUTGOING");
            double local = side ? 0 : duration;
            if (g_cutClips) {
                double span = fmin(3.0, duration);
                local = (side ? 0 : duration - span) + fmod(ImGui::GetTime(), fmax(.04, span));
            }
            double source = SourceTime(c, local, duration, trim);
            SourcePicture(c.path, c.kind, c.text, source,
                ImVec2(ImGui::GetContentRegionAvail().x, fmaxf(24, size.y - 112)), g_cutClips ? -1 : side);
            ImGui::Text("%.3f s  |  %s", source, c.reversed ? "reverse" : "forward");
            if (c.kind == Clip::Video && c.vid && c.vid->probed.load()) {
                double head = trim, tail = fmax(0, c.vid->duration - trim - duration);
                ImGui::Text("Handles  in %.2fs / out %.2fs", c.reversed ? tail : head, c.reversed ? head : tail);
            } else ImGui::TextDisabled(c.kind == Clip::Video ? "Probing source handles..." : "Still / generated source");
        } else ImGui::TextUnformatted("END OF FILM");
        ImGui::EndChild();
    }
}
static float SourcePeak(const Song& song, double time) {
    if (!song.loaded || song.peaks.empty()) return -1;
    long long bucket = (long long)(time * SAMPLE_RATE / fmax(1, song.framesPerPeak));
    if (bucket < 0 || bucket * 2 + 1 >= (long long)song.peaks.size()) return -1;
    return fmaxf(fabsf(song.peaks[(size_t)bucket * 2]), fabsf(song.peaks[(size_t)bucket * 2 + 1]));
}
static void DrawRhythm() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = fmaxf(1, ImGui::GetContentRegionAvail().x);
    float height = 46;
    ImGui::InvisibleButton("##rhythm", ImVec2(width, height));
    bool hover = ImGui::IsItemHovered();
    double end = fmax(.001, TimelineEnd());
    auto x = [&](double t) { return p.x + (float)std::clamp(t / end, 0.0, 1.0) * width; };
    auto dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(20, 26, 35, 255), 4);
    std::vector<BaseSpan> lay; BaseLayout(lay);
    for (int i = 0; i < (int)g_clips.size(); i++) {
        if (g_clips[i]->skip) continue;
        float left = x(lay[i].start), right = x(lay[i].end);
        dl->AddRectFilled(ImVec2(left, p.y + 3), ImVec2(fmaxf(left + 1, right - 1), p.y + 27),
            i % 2 ? IM_COL32(76, 113, 130, 255) : IM_COL32(55, 84, 102, 255), 2);
        if (right - left > 48) {
            char label[24]; snprintf(label, sizeof(label), "%.1fs", g_clips[i]->duration);
            dl->AddText(ImVec2(left + 4, p.y + 5), IM_COL32_WHITE, label);
        }
    }
    // Sample a bounded number of waveform buckets, independent of zoom/length.
    // Amber means unavailable analysis, dark means below -40 dB in source peaks.
    int samples = std::min(360, (int)width);
    static std::vector<ImU32> audioStrip;
    static double analyzedAt = -1, analyzedEnd = -1;
    static int analyzedSeq = -1;
    bool refresh = (int)audioStrip.size() != samples || analyzedEnd != end ||
        analyzedSeq != CurSeqId() || ImGui::GetTime() - analyzedAt > .35;
    if (refresh) {
    audioStrip.resize(samples);
    for (int n = 0; n < samples; ++n) {
        double time = end * (n + .5) / samples;
        float peak = 0; bool unknown = false;
        for (int i = 0; i < (int)g_clips.size(); ++i) {
            auto& c = *g_clips[i];
            if (c.skip || !c.useAudio || c.volume <= 0 || c.kind != Clip::Video ||
                time < lay[i].start || time >= lay[i].end || !c.vid) continue;
            if (!c.vid->probed.load()) { unknown = true; continue; }
            if (!c.vid->hasAudio) continue;
            auto s = c.vid->apeaksReady.load() ? c.vid->audio : nullptr;
            float v = s ? SourcePeak(*s, SourceTime(c, time - lay[i].start, c.duration, c.trimIn)) : -1;
            if (v < 0) unknown = true; else peak = fmaxf(peak, v * c.volume);
        }
        for (auto& t : g_over) if (t->visible) for (auto& c : t->clips) {
            if (c->skip || !c->useAudio || c->volume <= 0 || c->kind != Clip::Video || !c->vid ||
                time < c->start || time >= c->start + c->duration) continue;
            if (!c->vid->probed.load()) { unknown = true; continue; }
            if (!c->vid->hasAudio) continue;
            auto s = c->vid->apeaksReady.load() ? c->vid->audio : nullptr;
            float v = s ? SourcePeak(*s, SourceTime(*c, time - c->start, c->duration, c->trimIn)) : -1;
            if (v < 0) unknown = true; else peak = fmaxf(peak, v * c->volume);
        }
        for (auto& t : g_atracks) if (!t->mute && t->volume > 0) for (auto& b : t->blocks) {
            double local = time - b->offset, len = b->trimEnd - b->trimStart;
            if (local < 0 || local >= len) continue;
            float v = SourcePeak(*b, b->reversed ? b->trimEnd - local : b->trimStart + local);
            if (v < 0) unknown = true; else peak = fmaxf(peak, v * t->volume);
        }
        ImU32 col = unknown ? IM_COL32(167, 127, 65, 255) : peak < .01f ?
            IM_COL32(39, 45, 54, 255) : IM_COL32(100, 187, 168, 255);
        audioStrip[n] = col;
    }
    analyzedAt = ImGui::GetTime(); analyzedEnd = end; analyzedSeq = CurSeqId();
    }
    for (int n = 0; n < samples; ++n) {
        dl->AddRectFilled(ImVec2(p.x + width * n / samples, p.y + 31),
                          ImVec2(p.x + width * (n + 1) / samples, p.y + 39), audioStrip[n]);
    }
    for (auto& m : g_editMarkers) if (m.seq == CurSeqId()) {
        float mx = x(m.time);
        dl->AddLine(ImVec2(mx, p.y), ImVec2(mx, p.y + height),
            m.scene ? IM_COL32(222, 181, 113, 255) : IM_COL32(172, 160, 229, 255), m.scene ? 3 : 1);
    }
    dl->AddLine(ImVec2(x(g_playhead.load()), p.y), ImVec2(x(g_playhead.load()), p.y + height), IM_COL32_WHITE, 2);
    if (hover) {
        double time = std::clamp((ImGui::GetIO().MousePos.x - p.x) / width, 0.0f, 1.0f) * end;
        ImGui::SetTooltip("Rhythm  %.2fs / %.2fs\nShots above; source audio below (dark: quiet, amber: unknown).\nMarkers: violet. Scene boundaries: gold. Click to navigate.", time, end);
        if (ImGui::IsMouseClicked(0)) { g_cutIndex = -1; SeekWorkspace(time); }
    }
}
static void DrawMarkerPanel() {
    static char name[160] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##markerName", "Marker / scene name", name, sizeof(name));
    if (ImGui::Button("Add marker")) g_editMarkers.push_back({CurSeqId(), g_playhead.load(), false, name[0] ? name : "Marker"});
    ImGui::SameLine();
    if (ImGui::Button("Scene boundary")) g_editMarkers.push_back({CurSeqId(), g_playhead.load(), true, name[0] ? name : "Scene"});
    int remove = -1;
    for (int i = 0; i < (int)g_editMarkers.size(); ++i) {
        auto& m = g_editMarkers[i]; if (m.seq != CurSeqId()) continue;
        ImGui::PushID(i);
        char label[240]; snprintf(label, sizeof(label), "%s %.2fs  %s", m.scene ? "Scene" : "Mark", m.time, m.label.c_str());
        if (ImGui::Selectable(label, false, 0, ImVec2(fmaxf(40, ImGui::GetContentRegionAvail().x - 78), 0))) SeekWorkspace(m.time);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove marker")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) g_editMarkers.erase(g_editMarkers.begin() + remove);
}

// rowKind is where it lands: 0 the base cut, 1 a layer track, 2 an audio track,
// 3 / 4 the strips that spawn a new video / audio track. t < 0 means the playhead.
static void InsertLibraryItem(const LibraryItem& item, int rowKind = 0, int track = -1,
                              double t = -1) {
    if (t < 0) t = g_playhead.load();
    if (item.kind == 4) {
        StartSongLoad(item.path, rowKind == 2 ? track : rowKind == 4 ? -2 : -1, t);
        return;
    }
    if (rowKind == 2 || rowKind == 4) {
        g_projectStatus = "Pictures go on a video track; sound on an audio track";
        return;
    }
    g_playing.store(false); g_audition = false;
    UndoCapture();
    KV kv = LibraryKV(item);
    if (item.kind == Clip::Video) {
        auto v = GetVideoSource(item.path);
        if (!v->probed.load()) { g_projectStatus = "Reading source metadata; insert when its preview is ready"; return; }
        double available = v->duration - kv.num("trimIn");
        if (available < MinClipDur()) { g_projectStatus = "No available source handles"; return; }
        double duration = kv.num("duration");
        SetKV(kv, "duration", std::to_string(duration > 0 ? fmin(duration, available) : available));
    }
    SetKV(kv, "skip", "0"); SetKV(kv, "group", "0"); SetKV(kv, "xfade", "0");
    auto c = std::unique_ptr<Clip>(MakeClipFromKV(kv));
    int uid = c->uid;
    if (rowKind == 1 || rowKind == 3) {    // a layer: sits at t, nothing moves
        if (rowKind == 3 || track < 0 || track >= (int)g_over.size()) track = NewOverlayTrack();
        c->start = t;
        auto& v = g_over[track]->clips;
        v.push_back(std::move(c));
        g_selTrack = track; g_sel = (int)v.size() - 1; SelSet(uid);
        UndoCapture();
        return;
    }
    int at = SplitPoint(t);
    RippleOthers(t, c->duration);
    g_clips.insert(g_clips.begin() + at, std::move(c));
    g_selTrack = -1; g_sel = at; SelSet(uid); g_cutIndex = -1;
    UndoCapture();
}
static void DropLibraryItem(int lib, int rowKind, int track, double t) {
    if (lib < 0 || lib >= (int)g_library.size()) return;
    InsertLibraryItem(g_library[lib], rowKind, track, t);
}
static void DrawLibrary() {
    static ImGuiTextFilter filter;
    ImGui::TextDisabled("Search sources");
    filter.Draw("##searchSources", -1);
    ImGui::TextDisabled("%zu sources  |  retained after timeline deletion", g_library.size());
    ImGui::TextWrapped("Move across a card to scrub its source. Drag to reorder, or use Move earlier/later.");
    int moveFrom = -1, moveTo = -1, insert = -1;
    ImGui::BeginChild("##libraryList", ImVec2(0, fmaxf(140, ImGui::GetContentRegionAvail().y * .52f)), true);
    // A list clipper avoids decoding or drawing off-screen sources.
    std::vector<int> visible;
    for (int i = 0; i < (int)g_library.size(); i++)
        if (filter.PassFilter(g_library[i].label.c_str())) visible.push_back(i);
    // Cards divide the panel rather than claiming a fixed width, so a narrow bin
    // still holds two per row instead of collapsing back into a single column.
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float room = fmaxf(80, ImGui::GetContentRegionAvail().x);
    int cols = (int)fmaxf(2, floorf((room + spacing) / (116 + spacing)));
    const float cardW = fmaxf(56, (room - spacing * (cols - 1)) / cols);
    const float thumbH = floorf(cardW * .62f) + 8, cardH = thumbH + 30;
    int rows = ((int)visible.size() + cols - 1) / cols;
    ImGuiListClipper clipper;
    clipper.Begin(rows, cardH + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        // Cards are placed by hand along the row, then one dummy of the row's
        // full size advances the layout, so the clipper still measures rows.
        ImVec2 rowP = ImGui::GetCursorScreenPos();
        for (int col = 0; col < cols; ++col) {
            int n = row * cols + col;
            if (n >= (int)visible.size()) break;
            ImVec2 p(rowP.x + col * (cardW + spacing), rowP.y);
            ImGui::SetCursorScreenPos(p);
            int i = visible[n]; auto& item = g_library[i];
            ImGui::PushID(i);
            bool selected = item.key == g_libraryPick;
            if (ImGui::Selectable("##source", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cardW, cardH))) {
                g_libraryPick = item.key;
                if (ImGui::IsMouseDoubleClicked(0)) insert = i;
            }
            bool hover = ImGui::IsItemHovered();
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SLIDECUT_MEDIA_ORDER", &i, sizeof(i));
                ImGui::TextUnformatted(item.label.c_str()); ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (auto payload = ImGui::AcceptDragDropPayload("SLIDECUT_MEDIA_ORDER")) {
                    moveFrom = *(const int*)payload->Data; moveTo = i;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Insert at playhead")) insert = i;
                if (ImGui::MenuItem("Move earlier", nullptr, false, i > 0)) { moveFrom = i; moveTo = i - 1; }
                if (ImGui::MenuItem("Move later", nullptr, false, i + 1 < (int)g_library.size())) { moveFrom = i; moveTo = i + 1; }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x + 4, p.y + 4));
            // Cards never run on their own: the frame shown is the one the pointer
            // picks out along the card, so a still bin stays a still bin.
            double time = 0;
            if (item.kind == Clip::Video && hover) {
                auto v = GetVideoSource(item.path);
                if (v->probed.load())
                    time = std::clamp((ImGui::GetIO().MousePos.x - p.x) / cardW, 0.0f, 1.0f) * v->duration;
            }
            KV kv = LibraryKV(item);
            SourcePicture(item.path, item.kind, kv.str("text"), time, ImVec2(cardW - 8, thumbH - 8));
            auto dl = ImGui::GetWindowDrawList();
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 6, p.y + thumbH),
                IM_COL32(235, 240, 246, 255), item.label.c_str(), nullptr, cardW - 12);
            dl->AddText(ImVec2(p.x + 6, p.y + thumbH + 15),
                IM_COL32(164, 181, 196, 255), item.kind == 2 ? "VIDEO" : item.kind == 4 ? "AUDIO" : item.kind == 1 ? "TEXT" : "IMAGE");
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(rowP);
        ImGui::Dummy(ImVec2(cols * (cardW + spacing) - spacing, cardH));
    }
    ImGui::EndChild();
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        auto item = std::move(g_library[moveFrom]);
        g_library.erase(g_library.begin() + moveFrom);
        g_library.insert(g_library.begin() + moveTo, std::move(item));
    } else if (insert >= 0) InsertLibraryItem(g_library[insert]);
    int picked = -1;
    for (int i = 0; i < (int)g_library.size(); i++) if (g_library[i].key == g_libraryPick) picked = i;
    if (picked < 0) return;
    auto& item = g_library[picked];
    ImGui::TextWrapped("%s", item.label.c_str());
    if (ImGui::Button("Insert at playhead")) InsertLibraryItem(item);
    ImGui::SameLine();
    if (ImGui::Button("Move earlier") && picked > 0) {
        std::swap(g_library[picked], g_library[picked - 1]); return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Move later") && picked + 1 < (int)g_library.size()) {
        std::swap(g_library[picked], g_library[picked + 1]); return;
    }
    Clip* current = SelectedClip();
    bool canTry = current && current->kind != Clip::Nest && item.kind != 4;
    ImGui::BeginDisabled(!canTry);
    if (ImGui::Button("Compare alternative...")) ImGui::OpenPopup("Alternative");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Alternative", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (current && canTry) {
            KV replacement = LibraryKV(item);
            double sourceIn = replacement.num("trimIn");
            double duration = current->duration;
            bool fits = true;
            if (item.kind == Clip::Video) {
                auto v = GetVideoSource(item.path);
                fits = v->probed.load() && v->duration - sourceIn >= duration;
            }
            ImGui::TextUnformatted("CURRENT"); ImGui::SameLine(280); ImGui::TextUnformatted("ALTERNATIVE");
            SourcePicture(current->path, current->kind, current->text,
                SourceTime(*current, fmod(ImGui::GetTime(), fmax(.04, duration)), duration, current->trimIn), ImVec2(260, 146));
            ImGui::SameLine();
            SourcePicture(item.path, item.kind, replacement.str("text"), sourceIn + fmod(ImGui::GetTime(), fmax(.04, duration)), ImVec2(260, 146));
            ImGui::Text("Same %.3fs duration. Downstream timing stays in place.", duration);
            if (!fits) ImGui::TextWrapped("Waiting for source metadata, or this source has insufficient handles.");
            ImGui::BeginDisabled(!fits);
            if (ImGui::Button("Use alternative")) {
                g_playing.store(false); g_audition = false;
                UndoCapture();
                // Keep placement, caption, grade, blend and duration from the edit;
                // replace only its source. The previous source remains in the bin.
                std::string original; WriteClip(original, *current, g_selTrack);
                KV target = WorkspaceRecords(original).front().second;
                SetKV(target, "kind", std::to_string(item.kind));
                SetKV(target, "path", Narrow(item.path)); SetKV(target, "label", item.label);
                SetKV(target, "trimIn", std::to_string(sourceIn)); SetKV(target, "reversed", replacement.str("reversed", "0"));
                SetKV(target, "text", replacement.str("text")); SetKV(target, "srcW", "0"); SetKV(target, "srcH", "0");
                auto fresh = std::unique_ptr<Clip>(MakeClipFromKV(target));
                auto* list = g_selTrack < 0 ? &g_clips : &g_over[g_selTrack]->clips;
                if (current->kind != Clip::Video) RetireTexture(current->tex);
                ClearDoubleExposure(*current);
                (*list)[g_sel] = std::move(fresh);
                SelSet((*list)[g_sel]->uid);
                g_cutIndex = -1;
                UndoCapture();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        } else ImGui::TextUnformatted("Select a timeline shot first.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

struct BranchSummary {
    std::vector<KV> shots;
    double duration = 0;
    int audio = 0, captions = 0, markers = 0;
};
static BranchSummary SummarizeBranch(const std::string& snapshot, int seq) {
    BranchSummary s;
    for (auto& r : WorkspaceRecords(snapshot)) {
        if (r.second.i("seq") != seq) continue;
        auto& kv = r.second;
        if (r.first == "clip") {
            if (kv.b("ovlOn") || kv.i("kind") == Clip::Text) s.captions++;
            if (kv.i("track", -1) == -1 && !kv.b("skip")) {
                double d = kv.num("duration");
                if (!s.shots.empty()) {
                    auto& prev = s.shots.back();
                    s.duration -= ClampFade(prev.num("xfade"), prev.num("duration"), d);
                }
                s.duration += d;
                s.shots.push_back(kv);
            }
        } else if (r.first == "song") s.audio++;
        else if (r.first == "marker") s.markers++;
    }
    return s;
}
static void SwitchBranch(int index) {
    if (index < 0 || index >= (int)g_branches.size() || index == g_activeBranch) return;
    g_playing.store(false); g_audition = false;
    UndoCapture();
    std::string working = ProjectToText(true, false);
    if (g_activeBranch >= 0 && g_activeBranch < (int)g_branches.size())
        g_branches[g_activeBranch].snapshot = working;
    auto branches = g_branches;
    auto library = g_library;
    std::string target = branches[index].snapshot;
    if (!LoadProjectFromText(target, true)) return;
    // Sources discovered in any branch remain in the project-wide library.
    std::map<std::wstring, bool> known;
    for (const auto& item : g_library) known[item.key] = true;
    for (const auto& item : library) if (known.emplace(item.key, true).second) g_library.push_back(item);
    g_branches = std::move(branches); g_activeBranch = index;
    g_projectStatus = "Editing " + g_branches[index].name + " - previous branch saved";
    UndoCapture();
}
static void DrawBranches() {
    static char name[128] = "";
    ImGui::TextWrapped("Named versions keep the complete project, including nested scenes, sound and captions. Switching saves your current branch.");
    ImGui::InputTextWithHint("##branchName", "Name this version", name, sizeof(name));
    ImGui::BeginDisabled(g_projectLoading.load() || g_hcLive || g_tl.drag != TimelineState::None);
    if (ImGui::Button("New branch")) {
        g_playing.store(false); g_audition = false;
        UndoCapture();
        std::string snap = ProjectToText(true, false);
        if (g_branches.empty()) { g_branches.push_back({"Main", snap}); g_activeBranch = 0; }
        if (g_activeBranch >= 0 && g_activeBranch < (int)g_branches.size())
            g_branches[g_activeBranch].snapshot = snap;
        g_branches.push_back({name[0] ? name : "Version " + std::to_string(g_branches.size()), snap});
        g_activeBranch = (int)g_branches.size() - 1;
        name[0] = 0; UndoCapture();
    }
    if (g_activeBranch >= 0 && g_activeBranch < (int)g_branches.size()) {
        ImGui::SameLine();
        if (ImGui::Button("Rename") && name[0]) { g_branches[g_activeBranch].name = name; name[0] = 0; }
    }
    int switchTo = -1;
    for (int i = 0; i < (int)g_branches.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(g_branches[i].name.c_str(), i == g_activeBranch, 0,
            ImVec2(fmaxf(40, ImGui::GetContentRegionAvail().x - 86), 0))) switchTo = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("Compare")) g_compareBranch = i;
        ImGui::PopID();
    }
    if (switchTo >= 0) SwitchBranch(switchTo);
    ImGui::EndDisabled();
    if (g_compareBranch < 0 || g_compareBranch >= (int)g_branches.size()) return;
    ImGui::Separator();
    // Cache parsing by snapshot content, rather than restoring media per frame.
    static std::string previousWorking, previousOther;
    static int previousSeq = -1;
    static BranchSummary a, b;
    std::string working = ProjectToText(true, false);
    auto& other = g_branches[g_compareBranch];
    if (working != previousWorking || other.snapshot != previousOther || previousSeq != CurSeqId()) {
        previousWorking = working; previousOther = other.snapshot; previousSeq = CurSeqId();
        a = SummarizeBranch(working, previousSeq); b = SummarizeBranch(other.snapshot, previousSeq);
    }
    ImGui::TextWrapped("Working / %s  (current sequence)", other.name.c_str());
    if (ImGui::BeginTable("##branchStats", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Measure"); ImGui::TableSetupColumn("Working"); ImGui::TableSetupColumn("Saved");
        ImGui::TableHeadersRow();
        auto row = [&](const char* label, double av, double bv) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", av); ImGui::TableNextColumn(); ImGui::Text("%.2f", bv);
        };
        row("Base duration", a.duration, b.duration); row("Shots", a.shots.size(), b.shots.size());
        row("Audio blocks", a.audio, b.audio); row("Captions", a.captions, b.captions); row("Markers", a.markers, b.markers);
        ImGui::EndTable();
    }
    static int shot = 0; static float position = .5f; static bool play = false;
    int count = (int)std::max(a.shots.size(), b.shots.size());
    if (!count) { ImGui::TextWrapped("No base shots in this sequence; switch branches to audition its layers and audio."); return; }
    shot = std::clamp(shot, 0, count - 1);
    ImGui::SliderInt("Shot", &shot, 0, count - 1);
    ImGui::SliderFloat("Position", &position, 0, 1, "%.2f");
    ImGui::Checkbox("Play sources side by side", &play);
    float width = fmaxf(32, (ImGui::GetContentRegionAvail().x - 10) / 2);
    for (int side = 0; side < 2; side++) {
        if (side) ImGui::SameLine();
        ImGui::BeginGroup();
        auto& summary = side ? b : a;
        if (shot < (int)summary.shots.size()) {
            auto& kv = summary.shots[shot];
            double duration = fmax(.04, kv.num("duration"));
            double local = play ? fmod(ImGui::GetTime(), duration) : position * fmax(0, duration - MinClipDur());
            if (kv.b("reversed")) local = fmax(0, duration - MinClipDur() - local);
            SourcePicture(Widen(kv.str("path")), kv.i("kind"), kv.str("text"), kv.num("trimIn") + local, ImVec2(width, width * .5625f));
            ImGui::Text("%.3fs / in %.3fs", duration, kv.num("trimIn"));
        } else { ImGui::Dummy(ImVec2(width, width * .5625f)); ImGui::TextUnformatted("No matching shot"); }
        ImGui::EndGroup();
    }
    if (shot < (int)a.shots.size() && shot < (int)b.shots.size()) {
        auto& av = a.shots[shot]; auto& bv = b.shots[shot];
        if (av.str("path") != bv.str("path")) ImGui::TextWrapped("Source changed: %s / %s", av.str("label").c_str(), bv.str("label").c_str());
        for (auto field : {"duration", "trimIn", "reversed", "xfade", "volume", "text", "ovlText", "gBright", "gContrast", "gSat"})
            if (av.str(field) != bv.str(field)) ImGui::TextWrapped("%s: %s / %s", field, av.str(field).c_str(), bv.str(field).c_str());
    }
    ImGui::TextWrapped("Source comparison is silent and aligned by shot position. Switch versions for the complete composite and audio.");
}
static void WorkspaceTransport() {
    if (ImGui::Button(g_playing.load() ? "Pause" : "Play")) {
        MixGuard lock;
        if (g_playhead.load() >= TimelineEnd()) g_playhead.store(0);
        g_playing.store(!g_playing.load());
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { g_playing.store(false); g_audition = false; }
    ImGui::SameLine();
    if (ImGui::Button("< frame")) SeekWorkspace(g_playhead.load() - 1.0 / fmax(1, g_fps));
    ImGui::SameLine();
    if (ImGui::Button("frame >")) SeekWorkspace(g_playhead.load() + 1.0 / fmax(1, g_fps));
    ImGui::SameLine();
    ImGui::Checkbox("Cut view", &g_cutView);
    if (g_cutView) {
        ImGui::SameLine();
        ImGui::Checkbox("Clip mode", &g_cutClips);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: source frames. On: silent looping clips. Use Audition cut for timeline audio.");
    }
    // Image previews are bounded; decoded video frames use the existing LRU.
    for (auto it = g_sourceThumbs.begin(); it != g_sourceThumbs.end();) {
        if (g_sourceThumbs.size() > 24 && ImGui::GetTime() - it->second->touched > 2 &&
            (!it->second->pending.valid() || it->second->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
            it = g_sourceThumbs.erase(it);
        else ++it;
    }
}
