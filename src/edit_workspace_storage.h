static std::wstring LibraryKey(int kind, const std::wstring& path, const std::string& text) {
    std::wstring key = path.empty() ? L"text:" + Widen(text) : path;
    if (!path.empty()) for (auto& c : key) {
        if (c == L'/') c = L'\\';
        c = (wchar_t)towlower(c);
    }
    return std::to_wstring(kind) + L":" + key;
}

static void SyncLibrary() {
    std::map<std::wstring, bool> known;
    for (const auto& item : g_library) known[item.key] = true;
    auto clips = [&](const std::vector<std::unique_ptr<Clip>>& list) {
        for (const auto& c : list) {
            if (c->kind == Clip::Nest) continue;
            auto key = LibraryKey(c->kind, c->path, c->text);
            if (known.emplace(key, true).second) {
                LibraryItem item;
                item.key = key; item.path = c->path; item.label = c->label; item.kind = c->kind;
                WriteClip(item.clipText, *c, -1);
                g_library.push_back(std::move(item));
            }
            if (c->dxOn && !c->dxPath.empty()) {
                int kind = c->dxIsVideo ? Clip::Video : Clip::Image;
                auto dxKey = LibraryKey(kind, c->dxPath, "");
                if (known.emplace(dxKey, true).second)
                    g_library.push_back({dxKey, c->dxPath, c->dxLabel, "", kind});
            }
        }
    };
    auto audio = [&](const std::vector<std::unique_ptr<AudioTrack>>& tracks) {
        for (const auto& t : tracks) for (const auto& b : t->blocks) {
            auto key = LibraryKey(4, b->path, "");
            if (known.emplace(key, true).second)
                g_library.push_back({key, b->path, b->label, "", 4});
        }
    };
    clips(g_clips);
    for (const auto& t : g_over) clips(t->clips);
    audio(g_atracks);
    for (const auto& q : g_seqs) {
        clips(q->clips);
        for (const auto& t : q->over) clips(t->clips);
        audio(q->atracks);
    }
}

static void WriteWorkspace(std::string& o, bool includeBranches) {
    o += "[workspace]\r\n";
    PutI(o, "activeBranch", includeBranches ? g_activeBranch : -1);
    for (const auto& item : g_library) {
        o += "[media]\r\n";
        PutW(o, "key", item.key); PutW(o, "path", item.path);
        Put(o, "label", item.label); PutI(o, "kind", item.kind); Put(o, "clipText", item.clipText);
    }
    for (const auto& m : g_editMarkers) {
        o += "[marker]\r\n";
        PutI(o, "seq", m.seq); PutN(o, "time", m.time);
        PutI(o, "scene", m.scene); Put(o, "label", m.label);
    }
    if (includeBranches) for (const auto& b : g_branches) {
        o += "[branch]\r\n";
        Put(o, "name", b.name); Put(o, "snapshot", b.snapshot);
    }
}

static bool ReadWorkspaceSection(const std::string& section, const KV& kv) {
    if (section == "workspace") g_activeBranch = kv.i("activeBranch", -1);
    else if (section == "media") {
        LibraryItem item;
        item.path = Widen(kv.str("path")); item.kind = kv.i("kind");
        item.label = kv.str("label"); item.clipText = kv.str("clipText");
        item.key = Widen(kv.str("key"));
        if (item.key.empty()) item.key = LibraryKey(item.kind, item.path, item.clipText);
        g_library.push_back(std::move(item));
    } else if (section == "marker") {
        double time = kv.num("time");
        if (std::isfinite(time))
            g_editMarkers.push_back({kv.i("seq"), fmax(0.0, time), kv.b("scene"), kv.str("label")});
    } else if (section == "branch") {
        std::string snap = kv.str("snapshot");
        if (snap.compare(0, 8, "slidecut") == 0)
            g_branches.push_back({kv.str("name", "Version"), std::move(snap)});
    } else return false;
    return true;
}

// Parse snapshots without restoring the project or starting decoders.
static std::vector<std::pair<std::string, KV>> WorkspaceRecords(const std::string& text) {
    std::vector<std::pair<std::string, KV>> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') result.push_back({line.substr(1, line.find(']') - 1), {}});
        else if (!result.empty()) {
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                result.back().second.v.push_back({line.substr(0, eq), UnescVal(line.substr(eq + 1))});
        }
    }
    return result;
}
