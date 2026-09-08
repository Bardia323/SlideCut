// Headless regression checks: synthetic projects only, no session/autosave access.
static int RunWorkspaceTests() {
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* label) {
        if (ok) ++passed; else ++failed;
        printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    };
    auto nearlyEqual = [](double a, double b) { return fabs(a - b) < 1e-5; };
    ImGui::CreateContext();
    const std::string legacy =
        "slidecut 1\r\n[settings]\r\nfps=25\r\n"
        "[clip]\r\nkind=1\r\nlabel=Opening\r\ntext=One\\nTwo\r\nduration=3\r\n"
        "[clip]\r\nkind=1\r\nlabel=Closing\r\ntext=End\r\nduration=4\r\n";
    check(LoadProjectFromText(legacy, true), "legacy project loads without new sections");
    check(g_clips.size() == 2 && CurSeqId() == 0 && nearlyEqual(TimelineEnd(), 7), "legacy timeline preserved");
    check(g_clips[0]->text == "One\nTwo" && nearlyEqual(g_clips[0]->volume, 1), "legacy text and audio defaults preserved");
    check(g_branches.empty() && g_editMarkers.empty(), "optional workspace defaults");
    SyncLibrary();
    check(g_library.size() == 2, "legacy sources discovered automatically");
    g_clips[0]->xfade = .5;
    g_clips[0]->ovlOn = true;
    g_clips[0]->ovlText = "Caption\nwith \\ paths = yes";
    g_clips[0]->grade.sat = .75f;
    g_editMarkers.push_back({0, 3, false, "Beat\nA"});
    g_editMarkers.push_back({0, 6, true, "Finale"});
    std::string saved = ProjectToText(true);
    check(LoadProjectFromText(saved, true), "extended project reloads");
    check(nearlyEqual(g_clips[0]->xfade, .5) && nearlyEqual(TimelineEnd(), 6.5), "dissolve survives save/reload");
    check(g_clips[0]->ovlText == "Caption\nwith \\ paths = yes" && nearlyEqual(g_clips[0]->grade.sat, .75),
          "captions and grades survive");
    check(g_editMarkers.size() == 2 && g_editMarkers[0].label == "Beat\nA", "markers and escaped labels survive");
    check(ProjectToText(true) == saved, "extended serialization round-trip is stable");
    auto layer = std::make_unique<VideoTrack>();
    auto caption = std::make_unique<Clip>();
    caption->kind = Clip::Text; caption->text = "Subtitle"; caption->start = 4; caption->duration = 1;
    layer->clips.push_back(std::move(caption)); g_over.push_back(std::move(layer));
    g_editMarkers.push_back({9, 6, true, "Other sequence"});
    RippleOthers(3, -.5);
    check(nearlyEqual(g_over[0]->clips[0]->start, 3.5) && nearlyEqual(g_editMarkers[0].time, 2.5) &&
          nearlyEqual(g_editMarkers[1].time, 5.5), "ripple moves captions and markers by identical delta");
    check(nearlyEqual(g_editMarkers[2].time, 6), "ripple respects sequence scope");
    SyncLibrary();
    auto sourceCount = g_library.size();
    g_clips.erase(g_clips.begin());
    check(g_library.size() == sourceCount, "timeline deletion retains media");
    saved = ProjectToText(true);
    LoadProjectFromText(saved, true);
    check(g_library.size() == sourceCount, "deleted source retention persists after reopen");
    if (g_library.size() > 1) std::swap(g_library.front(), g_library.back());
    auto firstKey = g_library.front().key;
    saved = ProjectToText(true); LoadProjectFromText(saved, true);
    check(g_library.front().key == firstKey, "manual media ordering persists");
    g_undo.clear(); g_redo.clear(); g_undoBase.clear(); UndoCapture();
    double original = g_clips[0]->duration;
    g_clips[0]->duration = original + 1; UndoCapture();
    UndoStep(false);
    check(nearlyEqual(g_clips[0]->duration, original), "trim undo restores original duration");
    UndoStep(true);
    check(nearlyEqual(g_clips[0]->duration, original + 1), "trim redo restores changed duration");
    g_branches.clear();
    for (int i = 0; i < 5; ++i) {
        g_clips[0]->duration = 2 + i;
        g_branches.push_back({"Version " + std::to_string(i), ProjectToText(true, false)});
    }
    g_activeBranch = 4;
    saved = ProjectToText(true); LoadProjectFromText(saved, true);
    check(g_branches.size() == 5 && g_activeBranch == 4, "five branches survive save/reopen");
    for (const auto& branch : g_branches)
        check(branch.snapshot.find("[branch]") == std::string::npos, "branch snapshots do not recursively embed branches");
    SwitchBranch(1);
    check(nearlyEqual(g_clips[0]->duration, 3) && g_activeBranch == 1, "branch switch restores chosen cut");
    g_clips[0]->duration = 8;
    SwitchBranch(3); SwitchBranch(1);
    check(nearlyEqual(g_clips[0]->duration, 8), "switching away saves unfinished branch edits");
    check(g_library.size() >= sourceCount, "library shared across branch switches");
    auto summary = SummarizeBranch(g_branches[3].snapshot, 0);
    check(!summary.shots.empty() && nearlyEqual(summary.shots[0].num("duration"), 5), "branch comparison reads saved timing");
    Clip a, b;
    a.kind = b.kind = Clip::Video;
    a.vid = std::make_shared<VideoSource>(); b.vid = std::make_shared<VideoSource>();
    a.vid->duration = b.vid->duration = 10; a.vid->fps = b.vid->fps = 25;
    a.vid->probed.store(true); b.vid->probed.store(true);
    a.trimIn = 2; a.duration = 4; b.trimIn = 1; b.duration = 3;
    check(nearlyEqual(TrimLimit(a, &b, 9, 0), 3 - MinClipDur()), "roll cannot consume incoming shot");
    check(nearlyEqual(TrimLimit(a, &b, -9, 0), -1), "roll respects incoming source handle");
    check(nearlyEqual(TrimLimit(a, &b, 9, 1), 4), "ripple respects outgoing source end");
    check(nearlyEqual(TrimLimit(a, &b, -9, 2), -2), "slip respects source start");
    a.reversed = true;
    check(nearlyEqual(TrimLimit(a, &b, 9, 1), 2), "reversed outgoing handle is bounded");
    check(nearlyEqual(SourceTime(a, 0, 4, 2), 5.96), "reverse incoming preview excludes source out-point");
    check(nearlyEqual(SourceTime(a, 4, 4, 2), 2), "reverse outgoing preview reaches source in-point");
    b.reversed = true;
    check(nearlyEqual(TrimLimit(a, &b, -9, 0), MinClipDur() - 4), "reverse roll bounds preserve source range");
    check(LibraryKey(2, L"C:/Media/SHOT.MP4", "") == LibraryKey(2, L"c:\\media\\shot.mp4", ""),
          "media deduplicates Windows case and separators");
    ClearProject();
    ImGui::DestroyContext();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

static bool WorkspaceScreenshot(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc; back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &staging))) { back->Release(); return false; }
    g_d3dContext->CopyResource(staging, back); back->Release();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) { staging->Release(); return false; }
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
    info.biSize = sizeof(info); info.biWidth = desc.Width; info.biHeight = -(LONG)desc.Height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::string data(file.bfSize, 0);
    memcpy(data.data(), &file, sizeof(file)); memcpy(data.data() + sizeof(file), &info, sizeof(info));
    for (UINT y = 0; y < desc.Height; y++) {
        auto src = (const unsigned char*)mapped.pData + y * mapped.RowPitch;
        auto dst = (unsigned char*)data.data() + file.bfOffBits + y * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; x++) {
            dst[x*4] = src[x*4+2]; dst[x*4+1] = src[x*4+1]; dst[x*4+2] = src[x*4]; dst[x*4+3] = 255;
        }
    }
    g_d3dContext->Unmap(staging, 0); staging->Release();
    return WriteWholeFile(path, data);
}
// Renders the real Windows/D3D interface into a hidden test window. It never
// opens the user's project, starts autosave, or touches their active session.
static int RunWorkspaceUITests(HINSTANCE instance, bool withMedia = false) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = instance; wc.lpszClassName = L"SlideCutWorkspaceTest";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Workspace test", WS_OVERLAPPEDWINDOW,
                              0, 0, 1440, 900, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(hwnd)) return 2;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17);
    auto titlePath = AssetPath(TITLE_FONT_FILE);
    if (!titlePath.empty()) g_titleFont = io.Fonts->AddFontFromFileTTF(Narrow(titlePath).c_str(), TITLE_FONT_PX);
    ApplyStyle(); ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);
    std::string fixture = "slidecut 1\n[settings]\nfps=25\n";
    const char* names[] = {"Arrival", "A quiet street", "The conversation", "A second look", "Departure", "End"};
    const double lengths[] = {2.5, 1.2, 4.0, .8, 3.2, 2.0};
    std::wstring sourcePath;
    if (withMedia) {
        if (!RunHidden(L"ffmpeg -v error -nostdin -y -f lavfi -i testsrc2=size=320x180:rate=25 "
                       L"-f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 -c:v libx264 -pix_fmt yuv420p "
                       L"-c:a aac workspace-test-source.mp4")) return 3;
        wchar_t source[MAX_PATH];
        GetFullPathNameW(L"workspace-test-source.mp4", MAX_PATH, source, nullptr);
        sourcePath = source;
    }
    for (int i = 0; i < 6; i++) {
        fixture += "[clip]\n";
        PutI(fixture, "kind", withMedia && i < 2 ? Clip::Video : Clip::Text);
        if (withMedia && i < 2) { PutW(fixture, "path", sourcePath); PutN(fixture, "trimIn", i); }
        Put(fixture, "label", names[i]); Put(fixture, "text", names[i]); PutN(fixture, "duration", lengths[i]);
    }
    LoadProjectFromText(fixture, true);
    if (withMedia) {
        auto video = GetVideoSource(sourcePath);
        for (int attempt = 0; attempt < 1500 && (!video->ready.load() || !video->apeaksReady.load()); attempt++) Sleep(10);
        if (!video->ready.load() || !video->apeaksReady.load()) return 4;
        PumpPendingLoads();
        printf("PASS real video proxy and audio extraction\n");
    }
    g_sel = 2; g_selTrack = -1; SelSet(g_clips[2]->uid); g_playhead.store(5.0);
    g_editMarkers = {{0, 0, true, "Arrival"}, {0, 3.7, false, "Dialogue begins"}, {0, 8.5, true, "Departure"}};
    for (int i = 0; i < 5; i++) {
        g_clips[2]->duration = 3.0 + i * .4;
        g_branches.push_back({"Scene " + std::to_string(i + 1), ProjectToText(true, false)});
    }
    g_activeBranch = 4; g_compareBranch = 0;
    if (withMedia) { g_sel = 0; SelSet(g_clips[0]->uid); }
    int failures = 0;
    for (int size = 0; size < 2; size++) {
        int width = size ? 1024 : 1440, height = size ? 768 : 900;
        SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client; GetClientRect(hwnd, &client);
        g_mainRTV->Release(); g_mainRTV = nullptr;
        g_swapChain->ResizeBuffers(0, client.right, client.bottom, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* back = nullptr;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        g_d3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV); back->Release();
        for (int tab = 0; tab < 4; tab++) {
            for (int frame = 0; frame < (withMedia ? 30 : 3); frame++) {
                g_workspaceTabRequest = tab;
                g_cutView = tab == 2; g_cutClips = false;
                ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
                DrawApp(); ImGui::Render();
                const float clear[4] = {.04f, .05f, .07f, 1};
                g_d3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
                g_d3dContext->ClearRenderTargetView(g_mainRTV, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                PumpRetiredTextures();
                if (withMedia) Sleep(10);
            }
            auto file = std::wstring(withMedia ? L"workspace-media-ui-" : L"workspace-ui-") +
                std::to_wstring(width) + L"-" + std::to_wstring(tab) + L".bmp";
            if (!WorkspaceScreenshot(file)) failures++;
            printf("Rendered %dx%d panel %d\n", width, height, tab);
        }
    }
    if (withMedia) {
        if (!g_exactFrames[0].tex || !g_exactFrames[1].tex) { printf("FAIL source frame decoding\n"); failures++; }
        else printf("PASS both source frames decoded\n");
        std::vector<float> output(1024);
        g_playhead.store(.1); g_playing.store(true);
        AudioCallback(nullptr, output.data(), nullptr, 512);
        float peak = 0; for (float sample : output) peak = fmaxf(peak, fabsf(sample));
        if (peak <= .001f || fabs(g_playhead.load() - (.1 + 512.0 / 48000)) > 1e-5) {
            printf("FAIL video audio playback/clock\n"); failures++;
        } else printf("PASS video audio playback and sample clock\n");
        g_hushClips.store(true);
        AudioCallback(nullptr, output.data(), nullptr, 512);
        peak = 0; for (float sample : output) peak = fmaxf(peak, fabsf(sample));
        if (peak > 1e-6f) { printf("FAIL preview hush\n"); failures++; }
        else printf("PASS preview hush\n");
        g_hushClips.store(false); g_playing.store(false);
    }
    ClearProject();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    g_titleFont = nullptr;
    DestroyWindow(hwnd); CoUninitialize();
    return failures ? 1 : 0;
}
