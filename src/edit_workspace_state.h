// Optional metadata; absent in older projects.
struct LibraryItem { std::wstring key, path; std::string label, clipText; int kind = 0; };
struct EditMarker { int seq = 0; double time = 0; bool scene = false; std::string label; };
struct EditBranch { std::string name, snapshot; };
static std::vector<LibraryItem> g_library;
static std::vector<EditMarker> g_editMarkers;
static std::vector<EditBranch> g_branches;
static int g_activeBranch = -1;
static int g_workspaceTabRequest = -1;
static bool g_cutView = false, g_cutClips = false;
static bool g_audition = false, g_auditionLoop = true;
static double g_auditionIn = 0, g_auditionOut = 0;
static void SyncLibrary();
static void ClearWorkspacePreviews();
