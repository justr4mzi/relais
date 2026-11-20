// Relais - launcher window: pick the capture card, its format, the sound and the picture settings
// SPDX-License-Identifier: MIT
#include "app.h"

#include <commctrl.h>
#include <mfapi.h>
#include <objbase.h>

#include <algorithm>

enum {
    IDC_VIDEO = 100, IDC_REFRESH, IDC_FORMAT, IDC_AUDIO,
    IDC_RANGE, IDC_MATRIX,
    IDC_BRIGHT, IDC_CONTRAST, IDC_SAT, IDC_SHARP, IDC_RESET,
    IDC_BRIGHT_V, IDC_CONTRAST_V, IDC_SAT_V, IDC_SHARP_V,
    IDC_EXCL, IDC_VSYNC, IDC_DUP, IDC_DUPTH, IDC_DUPTH_V,
    IDC_STATUS, IDC_START, IDC_HELPBTN, IDC_SUBTITLE, IDC_HEADER,
};

static HINSTANCE g_inst;
static HWND g_wnd;
static Settings g_s;
static std::vector<DeviceInfo> g_videoDevs, g_audioDevs;
static std::vector<VideoMode> g_modes;
static HFONT g_font, g_fontBold, g_fontTitle;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------
static std::wstring IniPath() {
    wchar_t buf[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = std::wstring(buf) + L"\\Relais";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\relais.ini";
}

static std::wstring ReadStr(const std::wstring& ini, const wchar_t* key) {
    wchar_t buf[2048] = {0};
    GetPrivateProfileStringW(L"Relais", key, L"", buf, 2048, ini.c_str());
    return buf;
}

void LoadSettings(Settings& s) {
    std::wstring ini = IniPath();
    auto num = [&](const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"Relais", k, def, ini.c_str()); };
    s.videoDevice = ReadStr(ini, L"VideoDevice");
    s.videoMode = ReadStr(ini, L"VideoMode");
    s.audioDevice = ReadStr(ini, L"AudioDevice");
    s.range = std::clamp(num(L"Range", s.range), 0, 2);
    s.matrix = std::clamp(num(L"Matrix", s.matrix), 0, 2);
    s.brightness = std::clamp(num(L"Brightness", s.brightness + 1000) - 1000, -100, 100);
    s.contrast = std::clamp(num(L"Contrast", s.contrast), 0, 200);
    s.saturation = std::clamp(num(L"Saturation", s.saturation), 0, 200);
    s.sharpness = std::clamp(num(L"Sharpness", s.sharpness), 0, 100);
    s.exclusive = num(L"Exclusive", s.exclusive) != 0;
    s.vsync = num(L"VSync", s.vsync) != 0;
    s.dropDuplicates = num(L"DropDuplicates", s.dropDuplicates) != 0;
    s.dupThreshold = std::clamp(num(L"DuplicateThreshold", s.dupThreshold), 1, 60);
}

void SaveSettings(const Settings& s) {
    std::wstring ini = IniPath();
    auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"Relais", k, v.c_str(), ini.c_str()); };
    auto num = [&](const wchar_t* k, int v) { put(k, std::to_wstring(v)); };
    put(L"VideoDevice", s.videoDevice);
    put(L"VideoMode", s.videoMode);
    put(L"AudioDevice", s.audioDevice);
    num(L"Range", s.range);
    num(L"Matrix", s.matrix);
    num(L"Brightness", s.brightness + 1000);   // stored shifted: GetPrivateProfileInt does not read negatives
    num(L"Contrast", s.contrast);
    num(L"Saturation", s.saturation);
    num(L"Sharpness", s.sharpness);
    num(L"Exclusive", s.exclusive);
    num(L"VSync", s.vsync);
    num(L"DropDuplicates", s.dropDuplicates);
    num(L"DuplicateThreshold", s.dupThreshold);
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}
static HWND Item(int id) { return GetDlgItem(g_wnd, id); }
static int ComboSel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }
static void ComboClear(int id) { SendMessageW(Item(id), CB_RESETCONTENT, 0, 0); }
static void ComboAdd(int id, const std::wstring& t) { SendMessageW(Item(id), CB_ADDSTRING, 0, (LPARAM)t.c_str()); }
static void ComboSet(int id, int i) { SendMessageW(Item(id), CB_SETCURSEL, i, 0); }
static bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void Check(int id, bool v) { SendMessageW(Item(id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
static int SliderPos(int id) { return (int)SendMessageW(Item(id), TBM_GETPOS, 0, 0); }
static void SliderSet(int id, int lo, int hi, int v) {
    SendMessageW(Item(id), TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(Item(id), TBM_SETPOS, TRUE, v);
}
static void SetStatus(const std::wstring& t) { SetWindowTextW(Item(IDC_STATUS), t.c_str()); }

static void UpdateSliderLabels() {
    SetWindowTextW(Item(IDC_BRIGHT_V), Format(L"%+d", SliderPos(IDC_BRIGHT)).c_str());
    SetWindowTextW(Item(IDC_CONTRAST_V), Format(L"%d %%", SliderPos(IDC_CONTRAST)).c_str());
    SetWindowTextW(Item(IDC_SAT_V), Format(L"%d %%", SliderPos(IDC_SAT)).c_str());
    SetWindowTextW(Item(IDC_SHARP_V), Format(L"%d", SliderPos(IDC_SHARP)).c_str());
    SetWindowTextW(Item(IDC_DUPTH_V), Format(L"%.1f", SliderPos(IDC_DUPTH) / 10.0).c_str());
    EnableWindow(Item(IDC_DUPTH), Checked(IDC_DUP));
}

static void FillModes() {
    ComboClear(IDC_FORMAT);
    g_modes.clear();
    int dev = ComboSel(IDC_VIDEO);
    if (dev < 0 || dev >= (int)g_videoDevs.size()) return;
    g_modes = EnumVideoModes(g_videoDevs[dev].id);
    int pick = -1, best = -1;
    for (size_t i = 0; i < g_modes.size(); ++i) {
        ComboAdd(IDC_FORMAT, g_modes[i].label);
        if (g_modes[i].label == g_s.videoMode && g_videoDevs[dev].id == g_s.videoDevice) pick = (int)i;
        if (best < 0 && g_modes[i].height == 1080 && g_modes[i].fps() >= 59.0) best = (int)i;
    }
    if (pick < 0) pick = best >= 0 ? best : (g_modes.empty() ? -1 : 0);
    ComboSet(IDC_FORMAT, pick);
    if (g_modes.empty()) SetStatus(L"Cette carte ne propose aucun format vidéo. Elle est peut-être déjà utilisée par une autre appli.");
}

static void FillDevices() {
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    g_videoDevs = EnumVideoDevices();
    g_audioDevs = EnumAudioDevices();

    ComboClear(IDC_VIDEO);
    int pick = 0;
    for (size_t i = 0; i < g_videoDevs.size(); ++i) {
        ComboAdd(IDC_VIDEO, g_videoDevs[i].name);
        if (g_videoDevs[i].id == g_s.videoDevice) pick = (int)i;
    }
    ComboSet(IDC_VIDEO, g_videoDevs.empty() ? -1 : pick);

    ComboClear(IDC_AUDIO);
    ComboAdd(IDC_AUDIO, L"Aucun (pas de son)");
    int apick = 0;
    for (size_t i = 0; i < g_audioDevs.size(); ++i) {
        ComboAdd(IDC_AUDIO, g_audioDevs[i].name);
        if (g_audioDevs[i].id == g_s.audioDevice) apick = (int)i + 1;
    }
    ComboSet(IDC_AUDIO, apick);

    if (g_videoDevs.empty()) {
        ComboClear(IDC_FORMAT);
        SetStatus(L"Aucune carte de capture trouvée. Branche-la puis clique sur ↻.");
    } else {
        SetStatus(L"Prêt. Échap pour quitter le plein écran.");
        FillModes();
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

static void ApplySettingsToUi() {
    ComboSet(IDC_RANGE, g_s.range);
    ComboSet(IDC_MATRIX, g_s.matrix);
    SliderSet(IDC_BRIGHT, -100, 100, g_s.brightness);
    SliderSet(IDC_CONTRAST, 0, 200, g_s.contrast);
    SliderSet(IDC_SAT, 0, 200, g_s.saturation);
    SliderSet(IDC_SHARP, 0, 100, g_s.sharpness);
    SliderSet(IDC_DUPTH, 1, 60, g_s.dupThreshold);
    Check(IDC_EXCL, g_s.exclusive);
    Check(IDC_VSYNC, g_s.vsync);
    Check(IDC_DUP, g_s.dropDuplicates);
    UpdateSliderLabels();
}

static void ReadUiIntoSettings() {
    int dev = ComboSel(IDC_VIDEO);
    if (dev >= 0 && dev < (int)g_videoDevs.size()) g_s.videoDevice = g_videoDevs[dev].id;
    int fmt = ComboSel(IDC_FORMAT);
    if (fmt >= 0 && fmt < (int)g_modes.size()) g_s.videoMode = g_modes[fmt].label;
    int au = ComboSel(IDC_AUDIO);
    g_s.audioDevice = (au > 0 && au - 1 < (int)g_audioDevs.size()) ? g_audioDevs[au - 1].id : L"";
    g_s.range = std::max(0, ComboSel(IDC_RANGE));
    g_s.matrix = std::max(0, ComboSel(IDC_MATRIX));
    g_s.brightness = SliderPos(IDC_BRIGHT);
    g_s.contrast = SliderPos(IDC_CONTRAST);
    g_s.saturation = SliderPos(IDC_SAT);
    g_s.sharpness = SliderPos(IDC_SHARP);
    g_s.exclusive = Checked(IDC_EXCL);
    g_s.vsync = Checked(IDC_VSYNC);
    g_s.dropDuplicates = Checked(IDC_DUP);
    g_s.dupThreshold = SliderPos(IDC_DUPTH);
}

static void Launch() {
    ReadUiIntoSettings();
    int fmt = ComboSel(IDC_FORMAT);
    if (ComboSel(IDC_VIDEO) < 0 || fmt < 0 || fmt >= (int)g_modes.size()) {
        SetStatus(L"Choisis d'abord une carte de capture et un format.");
        return;
    }
    SaveSettings(g_s);
    VideoMode mode = g_modes[fmt];
    ShowWindow(g_wnd, SW_HIDE);
    SessionStats st = RunViewer(g_inst, g_s, mode);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SaveSettings(g_s);          // hotkeys may have changed some settings
    ApplySettingsToUi();

    if (!st.error.empty() && st.captured == 0) { SetStatus(st.error); return; }
    double secs = st.seconds > 0.01 ? st.seconds : 0.01;
    std::wstring msg = Format(L"Dernière session : %.1f images/s reçues, %.1f affichées",
                              st.captured / secs, st.presented / secs);
    if (st.duplicates) msg += Format(L" (%llu doublons ignorés)", st.duplicates);
    msg += Format(L", %d:%02d.", int(secs) / 60, int(secs) % 60);
    if (!st.error.empty()) msg += L"\n" + st.error;
    SetStatus(msg);
}

static void ShowHelp() {
    MessageBoxW(g_wnd,
        L"Relais affiche ta carte de capture en plein écran avec le moins de latence possible.\n\n"
        L"Pendant l'affichage :\n"
        L"  Échap\tRevenir à cette fenêtre\n"
        L"  D\tIgnorer les doublons (on/off)\n"
        L"  R\tPlage de couleurs suivante (Auto, Limitée, Pleine)\n"
        L"  M\tMatrice suivante (Auto, BT.601, BT.709)\n"
        L"  + / -\tLuminosité\n"
        L"  V\tVSync (on/off)\n"
        L"  Alt+Entrée\tPlein écran exclusif / fenêtre\n\n"
        L"AFMF (RX 6000) : active Fluid Motion Frames dans AMD Adrenalin, coche « Plein écran exclusif », "
        L"et pour un jeu à 30 FPS capturé en 60, laisse « Ignorer les images en double » coché.\n\n"
        L"Couleurs délavées (noirs gris) : mets la plage sur « Limitée ». Noirs écrasés : « Pleine ».",
        L"Relais " RELAIS_VERSION L" - Aide", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
static void BuildUi() {
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    const DWORD slider = TBS_HORZ | TBS_NOTICKS | WS_TABSTOP;
    const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
    int y = 14;
    Ctl(L"STATIC", L"Relais", 0, 18, y, 300, 34, IDC_HEADER, g_fontTitle);
    Ctl(L"STATIC", L"Ta console en plein écran, avec le moins de latence possible.", 0, 20, y + 36, 480, 20, IDC_SUBTITLE);

    y = 84;
    Ctl(L"STATIC", L"Source", 0, 18, y, 200, 20, 0, g_fontBold);
    Ctl(L"STATIC", L"Vidéo", 0, 20, y + 30, 100, 20, 0);
    Ctl(L"COMBOBOX", L"", combo, 130, y + 26, 340, 300, IDC_VIDEO);
    Ctl(L"BUTTON", L"↻", BS_PUSHBUTTON | WS_TABSTOP, 476, y + 25, 36, 26, IDC_REFRESH);
    Ctl(L"STATIC", L"Format", 0, 20, y + 64, 100, 20, 0);
    Ctl(L"COMBOBOX", L"", combo, 130, y + 60, 382, 300, IDC_FORMAT);
    Ctl(L"STATIC", L"Son", 0, 20, y + 98, 100, 20, 0);
    Ctl(L"COMBOBOX", L"", combo, 130, y + 94, 382, 300, IDC_AUDIO);

    y = 222;
    Ctl(L"STATIC", L"Image", 0, 18, y, 200, 20, 0, g_fontBold);
    Ctl(L"STATIC", L"Plage de couleurs", 0, 20, y + 30, 110, 20, 0);
    Ctl(L"COMBOBOX", L"", combo, 130, y + 26, 382, 200, IDC_RANGE);
    Ctl(L"STATIC", L"Matrice", 0, 20, y + 64, 100, 20, 0);
    Ctl(L"COMBOBOX", L"", combo, 130, y + 60, 382, 200, IDC_MATRIX);
    const struct { const wchar_t* name; int id, vid; } sliders[] = {
        {L"Luminosité", IDC_BRIGHT, IDC_BRIGHT_V}, {L"Contraste", IDC_CONTRAST, IDC_CONTRAST_V},
        {L"Saturation", IDC_SAT, IDC_SAT_V}, {L"Netteté", IDC_SHARP, IDC_SHARP_V}};
    for (int i = 0; i < 4; ++i) {
        int sy = y + 98 + i * 32;
        Ctl(L"STATIC", sliders[i].name, 0, 20, sy + 4, 100, 20, 0);
        Ctl(TRACKBAR_CLASSW, L"", slider, 124, sy, 330, 28, sliders[i].id);
        Ctl(L"STATIC", L"", SS_RIGHT, 456, sy + 4, 56, 20, sliders[i].vid);
    }
    Ctl(L"BUTTON", L"Réinitialiser l'image", BS_PUSHBUTTON | WS_TABSTOP, 130, y + 228, 170, 28, IDC_RESET);

    y = 494;
    Ctl(L"STATIC", L"Affichage", 0, 18, y, 200, 20, 0, g_fontBold);
    Ctl(L"BUTTON", L"Plein écran exclusif (nécessaire pour AFMF sur RX 6000)", check, 20, y + 26, 492, 22, IDC_EXCL);
    Ctl(L"BUTTON", L"VSync (moins de déchirures, un peu plus de latence)", check, 20, y + 52, 492, 22, IDC_VSYNC);
    Ctl(L"BUTTON", L"Ignorer les images en double (jeux à 30 FPS capturés en 60)", check, 20, y + 78, 492, 22, IDC_DUP);
    Ctl(L"STATIC", L"Seuil", 0, 40, y + 110, 80, 20, 0);
    Ctl(TRACKBAR_CLASSW, L"", slider, 124, y + 106, 330, 28, IDC_DUPTH);
    Ctl(L"STATIC", L"", SS_RIGHT, 456, y + 110, 56, 20, IDC_DUPTH_V);

    Ctl(L"STATIC", L"", SS_LEFT, 20, 648, 492, 40, IDC_STATUS);
    Ctl(L"BUTTON", L"Aide", BS_PUSHBUTTON | WS_TABSTOP, 20, 698, 90, 34, IDC_HELPBTN);
    Ctl(L"BUTTON", L"Lancer", BS_DEFPUSHBUTTON | WS_TABSTOP, 332, 698, 180, 34, IDC_START, g_fontBold);

    const wchar_t* ranges[] = {L"Auto", L"Limitée (16-235) : si les noirs sont gris", L"Pleine (0-255) : si les noirs sont écrasés"};
    for (auto r : ranges) ComboAdd(IDC_RANGE, r);
    const wchar_t* matrices[] = {L"Auto", L"BT.601 (SD, JPEG)", L"BT.709 (HD)"};
    for (auto m : matrices) ComboAdd(IDC_MATRIX, m);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_START || id == IDOK) Launch();
        else if (id == IDC_REFRESH) { ReadUiIntoSettings(); FillDevices(); }
        else if (id == IDC_HELPBTN) ShowHelp();
        else if (id == IDC_VIDEO && code == CBN_SELCHANGE) FillModes();
        else if (id == IDC_DUP) UpdateSliderLabels();
        else if (id == IDC_RESET) {
            Settings def;
            SliderSet(IDC_BRIGHT, -100, 100, def.brightness);
            SliderSet(IDC_CONTRAST, 0, 200, def.contrast);
            SliderSet(IDC_SAT, 0, 200, def.saturation);
            SliderSet(IDC_SHARP, 0, 100, def.sharpness);
            ComboSet(IDC_RANGE, def.range);
            ComboSet(IDC_MATRIX, def.matrix);
            UpdateSliderLabels();
        }
        return 0;
    }
    case WM_HSCROLL:
        UpdateSliderLabels();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_SUBTITLE || id == IDC_STATUS) SetTextColor(dc, RGB(96, 96, 110));
        else if (id == IDC_HEADER) SetTextColor(dc, RGB(108, 63, 196));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE:
        ReadUiIntoSettings();
        SaveSettings(g_s);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        MessageBoxW(nullptr, L"Media Foundation est indisponible. Sur Windows N, installe le Media Feature Pack.", L"Relais", MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    auto font = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    g_font = font(9, FW_NORMAL);
    g_fontBold = font(9, FW_SEMIBOLD);
    g_fontTitle = font(18, FW_BOLD);

    LoadSettings(g_s);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RelaisMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&wc);

    RECT rc{0, 0, S(532), S(748)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Relais " RELAIS_VERSION, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    BuildUi();
    ApplySettingsToUi();
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    FillDevices();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}
