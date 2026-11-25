#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <cwchar>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/app_controller.h"
#include "core/common.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"
#include "../../resources/resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"LanDropMainWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 200;
constexpr int kMinClientWidthDip = 800;
constexpr int kMinClientHeightDip = 520;

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

enum ControlId : int {
    IDC_TARGET_LABEL = 1000,
    IDC_TARGET_EDIT,
    IDC_PEER_COMBO,
    IDC_PICK_FILES,
    IDC_PICK_FOLDER,
    IDC_CLEAR_SELECTION,
    IDC_SEND,
    IDC_DROP_ZONE,
    IDC_RECV_CHECK,
    IDC_SAVE_LABEL,
    IDC_RECV_PATH,
    IDC_CHANGE_RECV,
    IDC_OPEN_RECV,
    IDC_RECV_HINT,
    IDC_TRANSFER_LIST,
    IDC_CANCEL_JOB,
    IDC_VIEW_LOGS,
    IDC_CLEAR_DONE,
    IDC_PROGRESS_LABEL,
    IDC_PROGRESS_BAR,
};

constexpr COLORREF kPageColor = RGB(246, 248, 252);
constexpr COLORREF kCardColor = RGB(255, 255, 255);
constexpr COLORREF kFieldColor = RGB(250, 251, 254);
constexpr COLORREF kTextColor = RGB(17, 24, 39);
constexpr COLORREF kMutedColor = RGB(100, 116, 139);
constexpr COLORREF kBorderColor = RGB(226, 232, 240);
constexpr COLORREF kAccentColor = RGB(37, 99, 235);
constexpr COLORREF kAccentDark = RGB(29, 78, 216);
constexpr COLORREF kAccentTint = RGB(239, 246, 255);
constexpr COLORREF kAccentBorder = RGB(147, 180, 245);
constexpr COLORREF kSuccessColor = RGB(5, 150, 105);
constexpr COLORREF kSuccessTint = RGB(236, 253, 245);
constexpr COLORREF kDangerColor = RGB(220, 38, 38);
constexpr COLORREF kDangerTint = RGB(254, 242, 242);
constexpr COLORREF kDisabledColor = RGB(148, 163, 184);

std::wstring widen(const std::string& utf8) {
    return landrop::platform::utf8ToWide(utf8);
}

std::string narrow(const std::wstring& text) {
    return landrop::platform::wideToUtf8(text);
}

std::wstring getWindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

void setWindowTextIfChanged(HWND control, const std::wstring& text) {
    if (getWindowText(control) != text) {
        SetWindowTextW(control, text.c_str());
    }
}

void showError(HWND owner, const std::wstring& message,
               const wchar_t* title = L"LanDrop") {
    MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

std::vector<landrop::fs::path> chooseFiles(HWND owner) {
    std::vector<landrop::fs::path> result;
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"选择要发送的文件";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = nullptr;
    if (!GetOpenFileNameW(&ofn)) {
        const DWORD code = CommDlgExtendedError();
        if (code != 0) {
            showError(owner, L"无法打开文件选择器（错误码 " +
                                 std::to_wstring(code) + L"）");
        }
        return result;
    }
    const wchar_t* current = buffer.data();
    if (*current == L'\0') {
        return result;
    }
    const std::wstring first(current);
    current += first.size() + 1;
    if (*current == L'\0') {
        result.emplace_back(first);
        return result;
    }
    const landrop::fs::path directory(first);
    while (*current != L'\0') {
        const std::wstring name(current);
        result.push_back(directory / landrop::fs::path(name));
        current += name.size() + 1;
    }
    return result;
}

landrop::fs::path chooseFolder(HWND owner, const landrop::fs::path& initial,
                               const wchar_t* title) {
    landrop::fs::path result;
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                  reinterpret_cast<void**>(&dialog));
    if (SUCCEEDED(hr) && dialog != nullptr) {
        FILEOPENDIALOGOPTIONS options{};
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS |
                               FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dialog->SetTitle(title);
        if (!initial.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initial.wstring().c_str(), nullptr, IID_IShellItem,
                    reinterpret_cast<void**>(&item))) &&
                item != nullptr) {
                dialog->SetFolder(item);
                item->Release();
            }
        }
        hr = dialog->Show(owner);
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                    path != nullptr) {
                    result = landrop::fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return {};
        }
    }

    if (result.empty()) {
        wchar_t displayName[MAX_PATH]{};
        BROWSEINFOW browse{};
        browse.hwndOwner = owner;
        browse.pszDisplayName = displayName;
        browse.lpszTitle = title;
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
        if (item != nullptr) {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item, path)) {
                result = landrop::fs::path(path);
            }
            CoTaskMemFree(item);
        }
    }
    return result;
}

struct UiMetrics {
    RECT header{};
    RECT sendCard{};
    RECT receiveCard{};
    RECT activityCard{};
    RECT progressBar{};
    RECT progressLabel{};
    RECT footer{};
};

struct RowCache {
    uint64_t id = 0;
    std::array<std::wstring, 6> cells;
};

struct AppWindow {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;
    landrop::AppController* controller = nullptr;
    landrop::AppSettings settings;
    std::string startupWarning;
    landrop::UiSnapshot snapshot;
    std::vector<landrop::fs::path> selectedPaths;
    std::vector<landrop::LogEntry> logs;
    uint64_t lastLogSequence = 0;
    UiMetrics metrics;
    std::vector<RowCache> rows;
    std::vector<landrop::PeerView> comboPeers;
    std::wstring peerSignature = L"<uninitialized>";
    std::wstring headerSignature;
    std::wstring progressTextCache;
    int dpi = 96;
    bool syncingControls = false;

    HWND targetLabel = nullptr;
    HWND targetEdit = nullptr;
    HWND peerCombo = nullptr;
    HWND pickFiles = nullptr;
    HWND pickFolder = nullptr;
    HWND clearSelection = nullptr;
    HWND sendButton = nullptr;
    HWND dropZone = nullptr;
    HWND receiveCheck = nullptr;
    HWND saveLabel = nullptr;
    HWND receivePath = nullptr;
    HWND changeReceive = nullptr;
    HWND openReceive = nullptr;
    HWND receiveHint = nullptr;
    HWND transferList = nullptr;
    HWND cancelJob = nullptr;
    HWND viewLogs = nullptr;
    HWND clearDone = nullptr;
    HWND progressLabel = nullptr;
    HWND progressBar = nullptr;

    HFONT fontTitle = nullptr;
    HFONT fontSection = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontBodyBold = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontButton = nullptr;
    HFONT fontList = nullptr;

    HBRUSH brushPage = nullptr;
    HBRUSH brushCard = nullptr;
    HBRUSH brushField = nullptr;
    HBRUSH brushAccent = nullptr;
    HBRUSH brushSuccessTint = nullptr;
    HBRUSH brushDangerTint = nullptr;

    int scale(int value) const { return MulDiv(value, dpi, 96); }
};

AppWindow* appFromWindow(HWND hwnd) {
    return reinterpret_cast<AppWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

HFONT createFont(int dpi, int points, int weight) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void deleteFont(HFONT& font) {
    if (font != nullptr) {
        DeleteObject(font);
        font = nullptr;
    }
}

void deleteBrush(HBRUSH& brush) {
    if (brush != nullptr) {
        DeleteObject(brush);
        brush = nullptr;
    }
}

void ensureBrushes(AppWindow& app) {
    if (app.brushPage == nullptr) app.brushPage = CreateSolidBrush(kPageColor);
    if (app.brushCard == nullptr) app.brushCard = CreateSolidBrush(kCardColor);
    if (app.brushField == nullptr) app.brushField = CreateSolidBrush(kFieldColor);
    if (app.brushAccent == nullptr)
        app.brushAccent = CreateSolidBrush(kAccentColor);
    if (app.brushSuccessTint == nullptr)
        app.brushSuccessTint = CreateSolidBrush(kSuccessTint);
    if (app.brushDangerTint == nullptr)
        app.brushDangerTint = CreateSolidBrush(kDangerTint);
}

void destroyResources(AppWindow& app) {
    deleteFont(app.fontTitle);
    deleteFont(app.fontSection);
    deleteFont(app.fontBody);
    deleteFont(app.fontBodyBold);
    deleteFont(app.fontSmall);
    deleteFont(app.fontButton);
    deleteFont(app.fontList);
    deleteBrush(app.brushPage);
    deleteBrush(app.brushCard);
    deleteBrush(app.brushField);
    deleteBrush(app.brushAccent);
    deleteBrush(app.brushSuccessTint);
    deleteBrush(app.brushDangerTint);
}

void createFonts(AppWindow& app) {
    deleteFont(app.fontTitle);
    deleteFont(app.fontSection);
    deleteFont(app.fontBody);
    deleteFont(app.fontBodyBold);
    deleteFont(app.fontSmall);
    deleteFont(app.fontButton);
    deleteFont(app.fontList);
    app.fontTitle = createFont(app.dpi, 22, FW_BOLD);
    app.fontSection = createFont(app.dpi, 11, 600);
    app.fontBody = createFont(app.dpi, 9, FW_NORMAL);
    app.fontBodyBold = createFont(app.dpi, 9, 600);
    app.fontSmall = createFont(app.dpi, 8, FW_NORMAL);
    app.fontButton = createFont(app.dpi, 9, 600);
    app.fontList = createFont(app.dpi, 9, FW_NORMAL);
}

void sendFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
    }
}

void applyFonts(AppWindow& app) {
    sendFont(app.targetLabel, app.fontSmall);
    sendFont(app.targetEdit, app.fontBody);
    sendFont(app.peerCombo, app.fontBody);
    sendFont(app.pickFiles, app.fontButton);
    sendFont(app.pickFolder, app.fontButton);
    sendFont(app.clearSelection, app.fontButton);
    sendFont(app.sendButton, app.fontButton);
    sendFont(app.dropZone, app.fontBody);
    sendFont(app.receiveCheck, app.fontBody);
    sendFont(app.saveLabel, app.fontSmall);
    sendFont(app.receivePath, app.fontBody);
    sendFont(app.changeReceive, app.fontButton);
    sendFont(app.openReceive, app.fontButton);
    sendFont(app.receiveHint, app.fontSmall);
    sendFont(app.transferList, app.fontList);
    sendFont(app.cancelJob, app.fontButton);
    sendFont(app.viewLogs, app.fontButton);
    sendFont(app.clearDone, app.fontButton);
    sendFont(app.progressLabel, app.fontSmall);
}

void addSelectedPaths(AppWindow& app,
                      const std::vector<landrop::fs::path>& paths) {
    std::set<std::string> existing;
    for (const auto& path : app.selectedPaths) {
        existing.insert(landrop::pathCollisionKey(path));
    }
    bool changed = false;
    for (const auto& input : paths) {
        if (input.empty()) {
            continue;
        }
        std::error_code ec;
        if (!landrop::fs::exists(input, ec)) {
            continue;
        }
        landrop::fs::path absolute = landrop::fs::absolute(input, ec);
        if (ec) {
            absolute = input;
        }
        absolute = absolute.lexically_normal();
        const std::string key = landrop::pathCollisionKey(absolute);
        if (existing.insert(key).second) {
            app.selectedPaths.push_back(absolute);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    EnableWindow(app.sendButton, TRUE);
    EnableWindow(app.clearSelection, TRUE);
    InvalidateRect(app.dropZone, nullptr, TRUE);
}

std::wstring selectedSubtitle(const AppWindow& app) {
    if (app.selectedPaths.empty()) {
        return L"或点击此处选择文件 · 支持大文件与断点续传";
    }
    std::wstring summary;
    const std::size_t limit = std::min<std::size_t>(app.selectedPaths.size(), 3);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            summary += L"  ·  ";
        }
        summary += app.selectedPaths[i].filename().wstring();
    }
    if (app.selectedPaths.size() > limit) {
        summary += L"  ·  …";
    }
    return summary;
}

void updateSelectionControls(AppWindow& app) {
    const bool hasSelection = !app.selectedPaths.empty();
    EnableWindow(app.sendButton, hasSelection);
    EnableWindow(app.clearSelection, hasSelection);
    InvalidateRect(app.dropZone, nullptr, TRUE);
}

void saveCurrentSettings(AppWindow& app) {
    if (app.controller == nullptr) {
        return;
    }
    landrop::AppSettings settings;
    settings.receiveDirectory = app.controller->receiveDirectory();
    settings.allowIncoming =
        app.controller->snapshot().allowIncoming;
    std::string error;
    if (landrop::SettingsStore::save(settings, error)) {
        app.settings = settings;
        return;
    }
    landrop::LogEntry entry;
    entry.sequence = 0;
    entry.unixTime = static_cast<int64_t>(std::time(nullptr));
    entry.level = landrop::LogLevel::Error;
    entry.text = "保存设置失败：" + error;
    app.logs.push_back(std::move(entry));
    while (app.logs.size() > 1000) {
        app.logs.erase(app.logs.begin());
    }
}

void createChildControls(AppWindow& app) {
    HINSTANCE instance = app.instance;
    HWND parent = app.hwnd;

    auto createStatic = [&](const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                               parent, reinterpret_cast<HMENU>(
                                           static_cast<INT_PTR>(id)),
                               instance, nullptr);
    };
    auto createButton = [&](const wchar_t* text, int id, bool ownerDraw) {
        return CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                (ownerDraw ? BS_OWNERDRAW : BS_PUSHBUTTON),
            0, 0, 10, 10, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
            nullptr);
    };

    app.targetLabel = createStatic(
        L"目标 IP", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_TARGET_LABEL);
    app.targetEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TARGET_EDIT)),
        instance, nullptr);
    SendMessageW(app.targetEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"192.168.1.5 或 IP:端口"));
    SendMessageW(app.targetEdit, EM_SETLIMITTEXT, 253 + 6, 0);
    SendMessageW(app.targetEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(MulDiv(7, app.dpi, 96),
                            MulDiv(7, app.dpi, 96)));

    app.peerCombo = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 10, 200, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PEER_COMBO)), instance,
        nullptr);

    app.pickFiles = createButton(L"选择文件", IDC_PICK_FILES, true);
    app.pickFolder = createButton(L"选择文件夹", IDC_PICK_FOLDER, true);
    app.clearSelection = createButton(L"清空", IDC_CLEAR_SELECTION, true);
    app.sendButton = createButton(L"发送", IDC_SEND, true);
    app.dropZone = createStatic(
        L"", SS_OWNERDRAW | SS_NOTIFY | SS_NOPREFIX, IDC_DROP_ZONE);

    app.receiveCheck = CreateWindowExW(
        0, L"BUTTON", L"允许其他设备发送文件",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECV_CHECK)),
        instance, nullptr);
    app.saveLabel = createStatic(L"接收目录", SS_LEFT | SS_CENTERIMAGE |
                                                SS_NOPREFIX,
                                  IDC_SAVE_LABEL);
    app.receivePath = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 10, 10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECV_PATH)), instance,
        nullptr);
    SendMessageW(app.receivePath, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(MulDiv(7, app.dpi, 96),
                            MulDiv(7, app.dpi, 96)));
    app.changeReceive = createButton(L"更改…", IDC_CHANGE_RECV, true);
    app.openReceive = createButton(L"打开", IDC_OPEN_RECV, true);
    app.receiveHint = createStatic(
        L"断点续传与 SHA-256 校验默认开启", SS_LEFT | SS_NOPREFIX,
        IDC_RECV_HINT);

    app.transferList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        0, 0, 10, 10, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TRANSFER_LIST)),
        instance, nullptr);
    ListView_SetExtendedListViewStyle(
        app.transferList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                              LVS_EX_LABELTIP);
    ListView_SetBkColor(app.transferList, kCardColor);
    ListView_SetTextBkColor(app.transferList, kCardColor);
    ListView_SetTextColor(app.transferList, kTextColor);

    static const wchar_t* kColumns[] = {L"方向", L"设备", L"当前项目",
                                        L"进度", L"速度", L"状态"};
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(kColumns[i]);
        column.iSubItem = i;
        column.fmt = i == 0 ? LVCFMT_LEFT : LVCFMT_LEFT;
        ListView_InsertColumn(app.transferList, i, &column);
    }

    app.cancelJob = createButton(L"取消任务", IDC_CANCEL_JOB, true);
    app.viewLogs = createButton(L"日志", IDC_VIEW_LOGS, true);
    app.clearDone = createButton(L"清除已结束", IDC_CLEAR_DONE, true);
    app.progressLabel = createStatic(L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                                     IDC_PROGRESS_LABEL);
    app.progressBar = CreateWindowExW(
        0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS_BAR)),
        instance, nullptr);
    SendMessageW(app.progressBar, PBM_SETRANGE32, 0, 1000);

    applyFonts(app);
    SendMessageW(app.receiveCheck, BM_SETCHECK,
                 app.settings.allowIncoming ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SetWindowTextW(app.receivePath,
                   app.controller->receiveDirectory().wstring().c_str());
    EnableWindow(app.sendButton, FALSE);
    EnableWindow(app.clearSelection, FALSE);
    EnableWindow(app.cancelJob, FALSE);
}

void layoutWindow(AppWindow& app) {
    if (app.hwnd == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(app.hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;

    const int maxContent = app.scale(1180);
    int margin = app.scale(22);
    const int available = width - margin * 2;
    if (available > maxContent) {
        margin = (width - maxContent) / 2;
    }
    const int contentWidth = std::max(app.scale(320), width - margin * 2);
    const int headerHeight = app.scale(84);
    const int cardY = headerHeight + app.scale(14);
    const int cardHeight = app.scale(216);
    const int gap = app.scale(14);
    const int leftWidth = (contentWidth - gap) * 58 / 100;

    app.metrics.header = RECT{0, 0, width, headerHeight};
    app.metrics.sendCard =
        RECT{margin, cardY, margin + leftWidth, cardY + cardHeight};
    app.metrics.receiveCard =
        RECT{margin + leftWidth + gap, cardY, margin + contentWidth,
             cardY + cardHeight};

    const int activityTop = cardY + cardHeight + gap;
    const int footerHeight = app.scale(22);
    const int activityBottom = std::max(activityTop + app.scale(140),
                                        height - footerHeight - app.scale(8));
    app.metrics.activityCard =
        RECT{margin, activityTop, margin + contentWidth, activityBottom};
    app.metrics.footer =
        RECT{0, height - footerHeight, width, height};

    const int pad = app.scale(16);
    const int sendInnerX = app.metrics.sendCard.left + pad;
    const int sendInnerWidth =
        (app.metrics.sendCard.right - app.metrics.sendCard.left) - pad * 2;
    const int rowY = cardY + app.scale(46);
    const int labelWidth = app.scale(44);
    const int smallGap = app.scale(6);
    const int availableTargetWidth =
        sendInnerWidth - labelWidth - smallGap * 2;
    const int comboWidth =
        std::max(app.scale(105),
                 std::min(app.scale(200), availableTargetWidth * 42 / 100));
    const int editWidth = availableTargetWidth - comboWidth;
    MoveWindow(app.targetLabel, sendInnerX, rowY, labelWidth,
               app.scale(30), TRUE);
    MoveWindow(app.targetEdit, sendInnerX + labelWidth + smallGap, rowY,
               editWidth, app.scale(30), TRUE);
    MoveWindow(app.peerCombo,
               sendInnerX + labelWidth + smallGap + editWidth + smallGap,
               rowY, comboWidth, app.scale(200), TRUE);
    SendMessageW(app.peerCombo, CB_SETDROPPEDWIDTH,
                 std::max(app.scale(180), comboWidth), 0);

    const int buttonsY = cardY + app.scale(86);
    const int buttonHeight = app.scale(30);
    const int buttonGap = app.scale(8);
    const int pickFilesWidth = app.scale(92);
    const int pickFolderWidth = app.scale(106);
    const int clearWidth = app.scale(54);
    const int sendWidth = app.scale(112);
    int buttonX = sendInnerX;
    MoveWindow(app.pickFiles, buttonX, buttonsY, pickFilesWidth, buttonHeight,
               TRUE);
    buttonX += pickFilesWidth + buttonGap;
    MoveWindow(app.pickFolder, buttonX, buttonsY, pickFolderWidth,
               buttonHeight, TRUE);
    buttonX += pickFolderWidth + buttonGap;
    MoveWindow(app.clearSelection, buttonX, buttonsY, clearWidth, buttonHeight,
               TRUE);
    MoveWindow(app.sendButton,
               sendInnerX + sendInnerWidth - sendWidth, buttonsY, sendWidth,
               buttonHeight, TRUE);
    MoveWindow(app.dropZone, sendInnerX, cardY + app.scale(124),
               sendInnerWidth, app.scale(76), TRUE);

    const int recvX = app.metrics.receiveCard.left + pad;
    const int recvWidth =
        (app.metrics.receiveCard.right - app.metrics.receiveCard.left) -
        pad * 2;
    MoveWindow(app.receiveCheck, recvX, cardY + app.scale(46), recvWidth,
               app.scale(24), TRUE);
    MoveWindow(app.saveLabel, recvX, cardY + app.scale(78), recvWidth,
               app.scale(20), TRUE);
    MoveWindow(app.receivePath, recvX, cardY + app.scale(100), recvWidth,
               app.scale(28), TRUE);
    const int recvButtonsY = cardY + app.scale(136);
    MoveWindow(app.changeReceive, recvX, recvButtonsY, app.scale(112),
               app.scale(28), TRUE);
    MoveWindow(app.openReceive, recvX + app.scale(112) + app.scale(8),
               recvButtonsY, app.scale(86), app.scale(28), TRUE);
    MoveWindow(app.receiveHint, recvX, cardY + app.scale(176), recvWidth,
               app.scale(22), TRUE);

    const int actPad = app.scale(14);
    const int actX = app.metrics.activityCard.left;
    const int actRight = app.metrics.activityCard.right;
    const int actionY = activityTop + app.scale(10);
    const int actionHeight = app.scale(28);
    const int actionGap = app.scale(6);
    const int clearDoneWidth = app.scale(100);
    const int logsWidth = app.scale(64);
    const int cancelWidth = app.scale(88);
    MoveWindow(app.clearDone, actRight - actPad - clearDoneWidth, actionY,
               clearDoneWidth, actionHeight, TRUE);
    MoveWindow(app.viewLogs,
               actRight - actPad - clearDoneWidth - actionGap - logsWidth,
               actionY, logsWidth, actionHeight, TRUE);
    MoveWindow(app.cancelJob,
               actRight - actPad - clearDoneWidth - actionGap - logsWidth -
                   actionGap - cancelWidth,
               actionY, cancelWidth, actionHeight, TRUE);

    const int listTop = activityTop + app.scale(44);
    const int progressBarTop = activityBottom - app.scale(22);
    const int progressBarHeight = app.scale(8);
    const int progressLabelTop = progressBarTop - app.scale(21);
    const int listBottom = progressLabelTop - app.scale(6);
    app.metrics.progressBar =
        RECT{actX + actPad, progressBarTop,
             actRight - actPad, progressBarTop + progressBarHeight};
    app.metrics.progressLabel =
        RECT{actX + actPad, progressLabelTop, actRight - actPad,
             progressBarTop - app.scale(3)};
    MoveWindow(app.transferList, actX + actPad, listTop,
               (actRight - actX) - actPad * 2,
               std::max(app.scale(40), listBottom - listTop), TRUE);
    MoveWindow(app.progressLabel, app.metrics.progressLabel.left,
               app.metrics.progressLabel.top,
               app.metrics.progressLabel.right - app.metrics.progressLabel.left,
               app.metrics.progressLabel.bottom -
                   app.metrics.progressLabel.top,
               TRUE);
    MoveWindow(app.progressBar, app.metrics.progressBar.left,
               app.metrics.progressBar.top,
               app.metrics.progressBar.right - app.metrics.progressBar.left,
               app.metrics.progressBar.bottom - app.metrics.progressBar.top,
               TRUE);

    const int listWidth = (actRight - actX) - actPad * 2;
    const int usable = std::max(app.scale(420), listWidth - app.scale(20));
    const int widthDirection = usable * 7 / 100;
    const int widthDevice = usable * 20 / 100;
    const int widthCurrent = usable * 22 / 100;
    const int widthProgress = usable * 23 / 100;
    const int widthSpeed = usable * 10 / 100;
    const int widthState = usable - widthDirection - widthDevice -
                           widthCurrent - widthProgress - widthSpeed;
    ListView_SetColumnWidth(app.transferList, 0, widthDirection);
    ListView_SetColumnWidth(app.transferList, 1, widthDevice);
    ListView_SetColumnWidth(app.transferList, 2, widthCurrent);
    ListView_SetColumnWidth(app.transferList, 3, widthProgress);
    ListView_SetColumnWidth(app.transferList, 4, widthSpeed);
    ListView_SetColumnWidth(app.transferList, 5, widthState);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

COLORREF stateColor(landrop::TransferState state) {
    switch (state) {
        case landrop::TransferState::Completed:
            return kSuccessColor;
        case landrop::TransferState::Failed:
            return kDangerColor;
        case landrop::TransferState::Canceled:
            return kMutedColor;
        default:
            return kTextColor;
    }
}

std::wstring transferStateLabel(landrop::TransferState state) {
    switch (state) {
        case landrop::TransferState::Preparing:
            return L"准备中";
        case landrop::TransferState::Connecting:
            return L"连接中";
        case landrop::TransferState::Transferring:
            return L"传输中";
        case landrop::TransferState::Verifying:
            return L"校验中";
        case landrop::TransferState::Completed:
            return L"已完成";
        case landrop::TransferState::Failed:
            return L"失败";
        case landrop::TransferState::Canceled:
            return L"已取消";
    }
    return L"";
}

std::array<std::wstring, 6> buildRow(const landrop::TransferView& job) {
    std::array<std::wstring, 6> cells;
    cells[0] = job.outgoing ? L"↑ 发送" : L"↓ 接收";
    cells[1] = widen(job.peer);
    if (!job.currentFile.empty()) {
        cells[2] = widen(job.currentFile);
    } else if (job.fileCount > 0) {
        cells[2] = std::to_wstring(job.fileCount) + L" 个文件";
    } else {
        cells[2] = std::to_wstring(job.itemCount) + L" 个项目";
    }
    if (job.totalBytes > 0) {
        const unsigned int percent = static_cast<unsigned int>(
            std::min<uint64_t>(100, job.bytesDone * 100 / job.totalBytes));
        std::string progress = std::to_string(percent) + "%  ·  " +
                               landrop::formatBytes(job.bytesDone) + " / " +
                               landrop::formatBytes(job.totalBytes);
        cells[3] = widen(progress);
    } else if (landrop::isActiveState(job.state)) {
        cells[3] = L"准备中";
    } else {
        cells[3] = L"—";
    }
    cells[4] = landrop::isActiveState(job.state)
                   ? widen(landrop::formatRate(job.bytesPerSecond))
                   : L"—";
    if (job.state == landrop::TransferState::Failed && !job.error.empty()) {
        cells[5] = L"失败：" + widen(job.error);
    } else {
        cells[5] = job.status.empty() ? transferStateLabel(job.state)
                                      : widen(job.status);
    }
    return cells;
}

uint64_t selectedJobId(const AppWindow& app) {
    if (app.transferList == nullptr) {
        return 0;
    }
    const int item = ListView_GetNextItem(app.transferList, -1, LVNI_SELECTED);
    if (item < 0 || static_cast<std::size_t>(item) >= app.rows.size()) {
        return 0;
    }
    return app.rows[static_cast<std::size_t>(item)].id;
}

void updateActionButtons(AppWindow& app) {
    const uint64_t id = selectedJobId(app);
    bool canCancel = false;
    if (id != 0) {
        for (const auto& job : app.snapshot.transfers) {
            if (job.id == id && job.outgoing &&
                landrop::isActiveState(job.state)) {
                canCancel = true;
                break;
            }
        }
    }
    EnableWindow(app.cancelJob, canCancel ? TRUE : FALSE);
}

void rebuildTransferRows(AppWindow& app,
                         const std::vector<landrop::TransferView>& transfers) {
    const uint64_t previousSelection = selectedJobId(app);
    SendMessageW(app.transferList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.transferList);
    app.rows.clear();
    for (const auto& job : transfers) {
        RowCache row;
        row.id = job.id;
        row.cells = buildRow(job);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(app.rows.size());
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(row.cells[0].c_str());
        const int index =
            ListView_InsertItem(app.transferList, &item);
        for (int column = 1; column < 6; ++column) {
            ListView_SetItemText(
                app.transferList, index, column,
                const_cast<wchar_t*>(row.cells[column].c_str()));
        }
        app.rows.push_back(std::move(row));
    }
    if (previousSelection != 0) {
        for (std::size_t i = 0; i < app.rows.size(); ++i) {
            if (app.rows[i].id == previousSelection) {
                ListView_SetItemState(app.transferList,
                                      static_cast<int>(i),
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(app.transferList,
                                       static_cast<int>(i), FALSE);
                break;
            }
        }
    }
    SendMessageW(app.transferList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.transferList, nullptr, TRUE);
    updateActionButtons(app);
}

void updateTransferRows(AppWindow& app,
                        const std::vector<landrop::TransferView>& transfers) {
    if (app.rows.size() != transfers.size()) {
        rebuildTransferRows(app, transfers);
        return;
    }
    for (std::size_t i = 0; i < transfers.size(); ++i) {
        if (app.rows[i].id != transfers[i].id) {
            rebuildTransferRows(app, transfers);
            return;
        }
    }
    for (std::size_t i = 0; i < transfers.size(); ++i) {
        const auto cells = buildRow(transfers[i]);
        for (int column = 0; column < 6; ++column) {
            if (app.rows[i].cells[static_cast<std::size_t>(column)] !=
                cells[static_cast<std::size_t>(column)]) {
                ListView_SetItemText(
                    app.transferList, static_cast<int>(i), column,
                    const_cast<wchar_t*>(
                        cells[static_cast<std::size_t>(column)].c_str()));
                app.rows[i].cells[static_cast<std::size_t>(column)] =
                    cells[static_cast<std::size_t>(column)];
            }
        }
    }
    updateActionButtons(app);
}

void updatePeerCombo(AppWindow& app) {
    if (SendMessageW(app.peerCombo, CB_GETDROPPEDSTATE, 0, 0) != 0) {
        return;
    }
    std::wstring signature;
    for (const auto& peer : app.snapshot.peers) {
        signature += widen(peer.id);
        signature += L"|";
        signature += widen(peer.name);
        signature += L"|";
        signature += widen(peer.ip);
        signature += L"|";
        signature += std::to_wstring(peer.port);
        signature += L";";
    }
    if (signature == app.peerSignature) {
        return;
    }
    app.peerSignature = signature;
    SendMessageW(app.peerCombo, CB_RESETCONTENT, 0, 0);
    app.comboPeers.clear();
    if (app.snapshot.peers.empty()) {
        SendMessageW(
            app.peerCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"未发现其他设备（可手动输入）"));
        SendMessageW(app.peerCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(app.peerCombo, FALSE);
        return;
    }
    SendMessageW(app.peerCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"选择发现的设备…"));
    for (const auto& peer : app.snapshot.peers) {
        std::wstring text = widen(peer.name);
        text += L"  ·  ";
        text += widen(peer.ip);
        if (peer.port != landrop::kDefaultTcpPort) {
            text += L":";
            text += std::to_wstring(peer.port);
        }
        SendMessageW(app.peerCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
    // Keep the exact list that produced the visible combo items. snapshot_
    // may change while the dropdown is open, but its refreshed order must not
    // be used to interpret an item chosen from the older list.
    app.comboPeers = app.snapshot.peers;
    EnableWindow(app.peerCombo, TRUE);
    SendMessageW(app.peerCombo, CB_SETCURSEL, 0, 0);
}

void updateProgressArea(AppWindow& app) {
    const landrop::TransferView* active = nullptr;
    for (const auto& job : app.snapshot.transfers) {
        if (landrop::isActiveState(job.state)) {
            active = &job;
            break;
        }
    }
    std::wstring label;
    int position = 0;
    if (active != nullptr) {
        label = active->outgoing ? L"发送到 " : L"接收自 ";
        label += widen(active->peer);
        if (!active->currentFile.empty()) {
            label += L"  ·  ";
            label += widen(active->currentFile);
        }
        label += L"  ·  ";
        label += active->status.empty() ? transferStateLabel(active->state)
                                        : widen(active->status);
        int otherActive = 0;
        for (const auto& job : app.snapshot.transfers) {
            if (&job != active && landrop::isActiveState(job.state)) {
                ++otherActive;
            }
        }
        if (otherActive > 0) {
            label += L"（另有 " + std::to_wstring(otherActive) +
                     L" 个任务）";
        }
        if (active->totalBytes != 0) {
            position = static_cast<int>(std::min<uint64_t>(
                1000, active->bytesDone * 1000 / active->totalBytes));
        }
    } else if (!app.snapshot.transfers.empty()) {
        const auto& latest = app.snapshot.transfers.front();
        if (latest.state == landrop::TransferState::Completed) {
            label = L"上次传输完成  ·  " + widen(latest.peer);
            position = 1000;
        } else if (latest.state == landrop::TransferState::Failed) {
            label = L"上次传输失败  ·  " +
                    (latest.error.empty() ? widen(latest.status)
                                          : widen(latest.error));
            position = 0;
        } else if (latest.state == landrop::TransferState::Canceled) {
            label = L"上次传输已取消";
            position = 0;
        } else {
            label = L"就绪";
        }
    } else {
        label = L"就绪  ·  拖入文件或文件夹即可开始发送";
    }
    if (label != app.progressTextCache) {
        app.progressTextCache = label;
        SetWindowTextW(app.progressLabel, label.c_str());
    }
    SendMessageW(app.progressBar, PBM_SETPOS, position, 0);
}

void refreshUi(AppWindow& app) {
    if (app.controller == nullptr || app.hwnd == nullptr) {
        return;
    }
    app.snapshot = app.controller->snapshot();
    const auto newLogs =
        app.controller->logsAfter(app.lastLogSequence);
    for (const auto& entry : newLogs) {
        app.lastLogSequence = std::max(app.lastLogSequence, entry.sequence);
        app.logs.push_back(entry);
    }
    while (app.logs.size() > 1000) {
        app.logs.erase(app.logs.begin());
    }

    setWindowTextIfChanged(app.receivePath,
                           app.snapshot.receiveDirectory.wstring());
    const bool allowed = app.snapshot.allowIncoming;
    const bool shouldCheck = allowed && app.snapshot.serverListening;
    const bool checked =
        SendMessageW(app.receiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (shouldCheck != checked) {
        app.syncingControls = true;
        SendMessageW(app.receiveCheck, BM_SETCHECK,
                     shouldCheck ? BST_CHECKED : BST_UNCHECKED, 0);
        app.syncingControls = false;
    }
    EnableWindow(app.receiveCheck,
                 app.snapshot.serverListening ? TRUE : FALSE);

    updatePeerCombo(app);
    updateTransferRows(app, app.snapshot.transfers);
    updateProgressArea(app);

    std::wstring signature;
    signature += widen(app.snapshot.deviceName);
    signature += L"|";
    for (const auto& ip : app.snapshot.localIps) {
        signature += widen(ip);
        signature += L",";
    }
    signature += L"|" + std::to_wstring(app.snapshot.listenPort);
    signature += L"|" + std::to_wstring(app.snapshot.serverListening);
    signature += L"|" + std::to_wstring(app.snapshot.discoveryActive);
    signature += L"|" + std::to_wstring(app.snapshot.allowIncoming);
    signature += L"|" + widen(app.snapshot.serviceMessage);
    if (signature != app.headerSignature) {
        app.headerSignature = signature;
        InvalidateRect(app.hwnd, &app.metrics.header, FALSE);
    }
}

void showLogs(AppWindow& app) {
    std::wstring text;
    const std::size_t maxEntries = 80;
    const std::size_t start =
        app.logs.size() > maxEntries ? app.logs.size() - maxEntries : 0;
    for (std::size_t i = start; i < app.logs.size(); ++i) {
        const auto& entry = app.logs[i];
        const wchar_t* level = L"信息";
        if (entry.level == landrop::LogLevel::LogSuccess) level = L"完成";
        if (entry.level == landrop::LogLevel::Warning) level = L"警告";
        if (entry.level == landrop::LogLevel::Error) level = L"错误";
        text += L"[";
        text += widen(landrop::formatClockTime(entry.unixTime));
        text += L"] [";
        text += level;
        text += L"] ";
        text += widen(entry.text);
        text += L"\r\n";
    }
    if (text.empty()) {
        text = L"暂无日志。";
    } else {
        text = L"最近 " +
               std::to_wstring(app.logs.size() - start) +
               L" 条日志（按 Ctrl+C 可复制）：\r\n\r\n" + text;
    }
    MessageBoxW(app.hwnd, text.c_str(), L"LanDrop 日志",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void drawLogo(HDC dc, int x, int y, int size) {
    HBRUSH brush = CreateSolidBrush(kAccentColor);
    HPEN pen = CreatePen(PS_SOLID, 1, kAccentColor);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, x, y, x + size, y + size, size / 4, size / 4);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    DeleteObject(pen);

    HPEN arrowPen =
        CreatePen(PS_SOLID, std::max(2, size / 16), RGB(255, 255, 255));
    oldPen = SelectObject(dc, arrowPen);
    const int left = x + size / 4;
    const int right = x + size * 3 / 4;
    const int top = y + size * 34 / 100;
    const int bottom = y + size * 64 / 100;
    MoveToEx(dc, left, top, nullptr);
    LineTo(dc, right, top);
    MoveToEx(dc, right - size / 7, top - size / 10, nullptr);
    LineTo(dc, right, top);
    LineTo(dc, right - size / 7, top + size / 10);
    MoveToEx(dc, right, bottom, nullptr);
    LineTo(dc, left, bottom);
    MoveToEx(dc, left + size / 7, bottom - size / 10, nullptr);
    LineTo(dc, left, bottom);
    LineTo(dc, left + size / 7, bottom + size / 10);
    SelectObject(dc, oldPen);
    DeleteObject(arrowPen);
}

void drawHeader(AppWindow& app, HDC dc) {
    RECT client{};
    GetClientRect(app.hwnd, &client);
    const int margin = app.metrics.sendCard.left;
    const int headerHeight = app.metrics.header.bottom;

    HBRUSH white = app.brushCard;
    FillRect(dc, &app.metrics.header, white);
    HPEN border = CreatePen(PS_SOLID, 1, kBorderColor);
    HGDIOBJ oldPen = SelectObject(dc, border);
    MoveToEx(dc, 0, headerHeight - 1, nullptr);
    LineTo(dc, client.right, headerHeight - 1);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    const int logoSize = app.scale(40);
    drawLogo(dc, margin, (headerHeight - logoSize) / 2, logoSize);

    SetBkMode(dc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, app.fontTitle));
    SetTextColor(dc, kTextColor);
    RECT titleRect{margin + app.scale(54), app.scale(12), client.right,
                   app.scale(48)};
    DrawTextW(dc, L"LanDrop", -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, app.fontSmall);
    SetTextColor(dc, kMutedColor);
    RECT subtitleRect{margin + app.scale(55), app.scale(46),
                      client.right, app.scale(68)};
    DrawTextW(dc, L"局域网文件直传  ·  拖放即发送", -1, &subtitleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    std::wstring serviceText;
    COLORREF serviceColor = kMutedColor;
    COLORREF serviceFill = RGB(241, 245, 249);
    if (!app.snapshot.serverListening) {
        serviceText = L"接收服务未启动";
        serviceColor = kDangerColor;
        serviceFill = kDangerTint;
    } else if (!app.snapshot.allowIncoming) {
        serviceText = L"接收已关闭";
        serviceColor = kMutedColor;
        serviceFill = RGB(241, 245, 249);
    } else {
        serviceText = app.snapshot.discoveryActive ? L"接收已开启 · 可被发现"
                                                   : L"接收已开启";
        serviceColor = kSuccessColor;
        serviceFill = kSuccessTint;
    }

    oldFont = static_cast<HFONT>(SelectObject(dc, app.fontSmall));
    SIZE textSize{};
    GetTextExtentPoint32W(dc, serviceText.c_str(),
                          static_cast<int>(serviceText.size()), &textSize);
    const int pillHeight = app.scale(24);
    const int pillPadding = app.scale(12);
    const int dotSpace = app.scale(14);
    const int pillWidth = textSize.cx + pillPadding * 2 + dotSpace;
    const int pillX = client.right - margin - pillWidth;
    const int pillY = app.scale(16);

    HBRUSH pillBrush = CreateSolidBrush(serviceFill);
    HPEN pillPen = CreatePen(PS_SOLID, 1, serviceFill);
    HGDIOBJ oldBrush = SelectObject(dc, pillBrush);
    oldPen = SelectObject(dc, pillPen);
    RoundRect(dc, pillX, pillY, pillX + pillWidth, pillY + pillHeight,
              pillHeight, pillHeight);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pillPen);
    DeleteObject(pillBrush);

    HBRUSH dotBrush = CreateSolidBrush(serviceColor);
    oldBrush = SelectObject(dc, dotBrush);
    SelectObject(dc, GetStockObject(NULL_PEN));
    const int dot = app.scale(6);
    Ellipse(dc, pillX + pillPadding, pillY + (pillHeight - dot) / 2,
            pillX + pillPadding + dot, pillY + (pillHeight + dot) / 2);
    SelectObject(dc, oldBrush);
    DeleteObject(dotBrush);

    SetTextColor(dc, serviceColor);
    RECT serviceRect{pillX + pillPadding + dotSpace, pillY,
                     pillX + pillWidth - pillPadding, pillY + pillHeight};
    DrawTextW(dc, serviceText.c_str(), -1, &serviceRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    std::wstring line2;
    if (app.snapshot.localIps.empty()) {
        line2 = L"未检测到局域网 IPv4 地址";
    } else {
        line2 = L"本机 ";
        int count = 0;
        for (const auto& ip : app.snapshot.localIps) {
            if (count >= 2) {
                line2 += L" 等";
                break;
            }
            if (count != 0) line2 += L" / ";
            line2 += widen(ip);
            ++count;
        }
        if (app.snapshot.listenPort != 0) {
            line2 += L":" + std::to_wstring(app.snapshot.listenPort);
        }
    }
    if (!app.snapshot.deviceName.empty()) {
        line2 += L"  ·  " + widen(app.snapshot.deviceName);
    }
    SetTextColor(dc, kMutedColor);
    RECT infoRect{client.right / 2, app.scale(48),
                  client.right - margin, app.scale(70)};
    DrawTextW(dc, line2.c_str(), -1, &infoRect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void drawCard(HDC dc, const RECT& card, const wchar_t* title,
              HFONT titleFont, int dpi) {
    const int radius = MulDiv(10, dpi, 96);
    const int shadowOffsetX = MulDiv(1, dpi, 96);
    const int shadowOffsetY = MulDiv(2, dpi, 96);
    const RECT shadow{card.left + shadowOffsetX, card.top + shadowOffsetY,
                      card.right + shadowOffsetX,
                      card.bottom + shadowOffsetY};
    HBRUSH shadowBrush = CreateSolidBrush(RGB(235, 239, 247));
    HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, shadowBrush);
    HGDIOBJ oldPen = SelectObject(dc, nullPen);
    RoundRect(dc, shadow.left, shadow.top, shadow.right, shadow.bottom,
              radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(shadowBrush);

    HBRUSH white = CreateSolidBrush(kCardColor);
    HPEN border = CreatePen(PS_SOLID, 1, kBorderColor);
    oldBrush = SelectObject(dc, white);
    oldPen = SelectObject(dc, border);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, radius,
              radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);
    DeleteObject(white);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kTextColor);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, titleFont));
    RECT titleRect{card.left + MulDiv(16, dpi, 96),
                   card.top + MulDiv(10, dpi, 96),
                   card.right - MulDiv(120, dpi, 96),
                   card.top + MulDiv(36, dpi, 96)};
    DrawTextW(dc, title, -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void paintWindow(AppWindow& app) {
    if (app.brushPage == nullptr) {
        ensureBrushes(app);
    }
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(app.hwnd, &paint);
    RECT client{};
    GetClientRect(app.hwnd, &client);
    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap =
        CreateCompatibleBitmap(target, std::max<int>(1, client.right),
                               std::max<int>(1, client.bottom));
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    FillRect(memory, &client, app.brushPage);
    drawHeader(app, memory);
    drawCard(memory, app.metrics.sendCard, L"发送文件", app.fontSection,
             app.dpi);
    drawCard(memory, app.metrics.receiveCard, L"接收文件", app.fontSection,
             app.dpi);
    drawCard(memory, app.metrics.activityCard, L"传输活动", app.fontSection,
             app.dpi);

    SetBkMode(memory, TRANSPARENT);
    HFONT oldFont =
        static_cast<HFONT>(SelectObject(memory, app.fontSmall));
    SetTextColor(memory, kMutedColor);
    RECT footer = app.metrics.footer;
    DrawTextW(memory,
              L"SHA-256 完整性校验  ·  自动断点续传  ·  大文件分块传输",
              -1, &footer,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(memory, oldFont);

    BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(app.hwnd, &paint);
}

void drawOwnerButton(AppWindow& app, const DRAWITEMSTRUCT& item) {
    const bool primary = item.CtlID == IDC_SEND;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    HDC dc = item.hDC;
    RECT rc = item.rcItem;
    FillRect(dc, &rc, app.brushCard);

    COLORREF fill = kCardColor;
    COLORREF border = kBorderColor;
    COLORREF text = kTextColor;
    if (primary) {
        fill = disabled ? RGB(191, 204, 232)
                        : (pressed ? kAccentDark : kAccentColor);
        border = fill;
        text = disabled ? RGB(245, 247, 252) : RGB(255, 255, 255);
    } else {
        fill = disabled ? RGB(248, 250, 253)
                        : (pressed ? kAccentTint : kCardColor);
        border = disabled ? kBorderColor : RGB(201, 212, 227);
        text = disabled ? kDisabledColor : kTextColor;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1,
              app.scale(8), app.scale(8));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HFONT oldFont =
        static_cast<HFONT>(SelectObject(dc, app.fontButton));
    RECT textRect = rc;
    DrawTextW(dc, getWindowText(item.hwndItem).c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -app.scale(4), -app.scale(4));
        DrawFocusRect(dc, &focus);
    }
}

void drawDropZone(AppWindow& app, const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    RECT rc = item.rcItem;
    FillRect(dc, &rc, app.brushCard);
    const bool hasSelection = !app.selectedPaths.empty();
    const COLORREF fill = hasSelection ? kAccentTint : RGB(250, 252, 255);
    const COLORREF border =
        hasSelection ? kAccentBorder : RGB(203, 213, 225);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1,
              app.scale(9), app.scale(9));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, hasSelection ? kAccentColor : kTextColor);
    HFONT oldFont =
        static_cast<HFONT>(SelectObject(dc, app.fontBodyBold));
    RECT titleRect = rc;
    titleRect.bottom = rc.top + (rc.bottom - rc.top) * 55 / 100;
    const std::wstring title =
        hasSelection
            ? L"已选择 " + std::to_wstring(app.selectedPaths.size()) + L" 项"
            : L"将文件或文件夹拖到这里";
    DrawTextW(dc, title.c_str(), -1, &titleRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, app.fontSmall);
    SetTextColor(dc, hasSelection ? RGB(59, 91, 156) : kMutedColor);
    RECT subtitleRect = rc;
    subtitleRect.top = titleRect.bottom - app.scale(2);
    DrawTextW(dc, selectedSubtitle(app).c_str(), -1, &subtitleRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void sendSelected(AppWindow& app) {
    if (app.selectedPaths.empty()) {
        showError(app.hwnd, L"请先选择要发送的文件或文件夹。");
        return;
    }
    const std::string target = landrop::trim(narrow(getWindowText(app.targetEdit)));
    if (target.empty()) {
        showError(app.hwnd, L"请输入接收方 IP 地址，或从发现的设备中选择。");
        SetFocus(app.targetEdit);
        return;
    }
    std::string error;
    if (app.controller->send(target, app.selectedPaths, error) == 0) {
        showError(app.hwnd, widen(error));
    }
}

void recreateFontsAndLayout(AppWindow& app) {
    createFonts(app);
    applyFonts(app);
    layoutWindow(app);
}

LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    AppWindow* app = appFromWindow(hwnd);
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = reinterpret_cast<AppWindow*>(create->lpCreateParams);
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(app));
            return TRUE;
        }
        case WM_CREATE: {
            if (app == nullptr) {
                return -1;
            }
            HDC dc = GetDC(hwnd);
            if (dc != nullptr) {
                app->dpi = GetDeviceCaps(dc, LOGPIXELSY);
                ReleaseDC(hwnd, dc);
            }
            ensureBrushes(*app);
            createFonts(*app);
            createChildControls(*app);
            layoutWindow(*app);
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, kRefreshTimer, kRefreshIntervalMs, nullptr);
            refreshUi(*app);
            return 0;
        }
        case WM_SIZE: {
            if (app != nullptr) {
                layoutWindow(*app);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const int dpi = app != nullptr ? app->dpi : 96;
            RECT rc{0, 0, MulDiv(kMinClientWidthDip, dpi, 96),
                    MulDiv(kMinClientHeightDip, dpi, 96)};
            AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
            info->ptMinTrackSize.x = rc.right - rc.left;
            info->ptMinTrackSize.y = rc.bottom - rc.top;
            // On small or heavily scaled screens the logical work area can
            // be shorter than the design minimum. Never force a window that
            // cannot fit on screen.
            const HMONITOR monitor =
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (GetMonitorInfoW(monitor, &monitorInfo)) {
                info->ptMinTrackSize.x =
                    std::min<int>(info->ptMinTrackSize.x,
                                  monitorInfo.rcWork.right -
                                      monitorInfo.rcWork.left);
                info->ptMinTrackSize.y =
                    std::min<int>(info->ptMinTrackSize.y,
                                  monitorInfo.rcWork.bottom -
                                      monitorInfo.rcWork.top);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (app != nullptr) {
                app->dpi = LOWORD(wParam);
                recreateFontsAndLayout(*app);
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == kRefreshTimer && app != nullptr) {
                refreshUi(*app);
            }
            return 0;
        }
        case WM_DROPFILES: {
            if (app != nullptr) {
                HDROP drop = reinterpret_cast<HDROP>(wParam);
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                std::vector<landrop::fs::path> paths;
                paths.reserve(count);
                for (UINT i = 0; i < count; ++i) {
                    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                    if (length == 0) {
                        continue;
                    }
                    std::wstring path(static_cast<std::size_t>(length) + 1,
                                      L'\0');
                    DragQueryFileW(drop, i, path.data(), length + 1);
                    path.resize(length);
                    paths.emplace_back(path);
                }
                DragFinish(drop);
                addSelectedPaths(*app, paths);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            if (app != nullptr) {
                auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_BUTTON) {
                    drawOwnerButton(*app, *item);
                    return TRUE;
                }
                if (item->CtlType == ODT_STATIC &&
                    item->CtlID == IDC_DROP_ZONE) {
                    drawDropZone(*app, *item);
                    return TRUE;
                }
            }
            return FALSE;
        }
        case WM_CTLCOLORSTATIC: {
            if (app != nullptr) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
                if (id == IDC_RECV_PATH) {
                    SetTextColor(dc, kTextColor);
                    SetBkColor(dc, kFieldColor);
                    return reinterpret_cast<LRESULT>(app->brushField);
                }
                if (id == IDC_TARGET_LABEL || id == IDC_SAVE_LABEL ||
                    id == IDC_RECV_HINT || id == IDC_PROGRESS_LABEL) {
                    SetTextColor(dc, kMutedColor);
                } else {
                    SetTextColor(dc, kTextColor);
                }
                return reinterpret_cast<LRESULT>(app->brushCard);
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            if (app != nullptr) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kTextColor);
                SetBkColor(dc, kCardColor);
                return reinterpret_cast<LRESULT>(app->brushCard);
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            if (app != nullptr) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, kTextColor);
                SetBkColor(dc, kCardColor);
                return reinterpret_cast<LRESULT>(app->brushCard);
            }
            break;
        }
        case WM_CTLCOLORBTN: {
            if (app != nullptr) {
                return reinterpret_cast<LRESULT>(app->brushCard);
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (app != nullptr) {
                paintWindow(*app);
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            if (app != nullptr) {
                auto* header = reinterpret_cast<NMHDR*>(lParam);
                if (header->idFrom == IDC_TRANSFER_LIST) {
                    if (header->code == LVN_ITEMCHANGED) {
                        updateActionButtons(*app);
                    } else if (header->code == NM_CUSTOMDRAW) {
                        auto* info =
                            reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                        if (info->nmcd.dwDrawStage == CDDS_PREPAINT) {
                            if (app->rows.empty()) {
                                HDC dc = info->nmcd.hdc;
                                RECT area = info->nmcd.rc;
                                FillRect(dc, &area, app->brushCard);
                                HFONT oldFont = static_cast<HFONT>(
                                    SelectObject(dc, app->fontBody));
                                SetBkMode(dc, TRANSPARENT);
                                SetTextColor(dc, kMutedColor);
                                RECT textArea = area;
                                textArea.top += app->scale(22);
                                DrawTextW(
                                    dc,
                                    L"暂无传输任务  ·  拖入文件或文件夹即可开始",
                                    -1, &textArea,
                                    DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                                        DT_NOPREFIX);
                                SelectObject(dc, oldFont);
                                return CDRF_SKIPDEFAULT;
                            }
                            return CDRF_NOTIFYITEMDRAW;
                        }
                        if (info->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                            const std::size_t index =
                                static_cast<std::size_t>(info->nmcd.dwItemSpec);
                            if (index < app->rows.size()) {
                                const uint64_t id = app->rows[index].id;
                                for (const auto& job :
                                     app->snapshot.transfers) {
                                    if (job.id == id) {
                                        info->clrText = stateColor(job.state);
                                        break;
                                    }
                                }
                            }
                            return CDRF_DODEFAULT;
                        }
                    }
                }
            }
            break;
        }
        case WM_COMMAND: {
            if (app == nullptr) {
                break;
            }
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            switch (id) {
                case IDC_SEND:
                    if (notification == BN_CLICKED) {
                        sendSelected(*app);
                    }
                    break;
                case IDC_PICK_FILES:
                case IDC_DROP_ZONE:
                    if (notification == BN_CLICKED ||
                        notification == STN_CLICKED) {
                        addSelectedPaths(*app, chooseFiles(hwnd));
                    }
                    break;
                case IDC_PICK_FOLDER:
                    if (notification == BN_CLICKED) {
                        const landrop::fs::path folder =
                            chooseFolder(hwnd, {}, L"选择要发送的文件夹");
                        if (!folder.empty()) {
                            addSelectedPaths(*app, {folder});
                        }
                    }
                    break;
                case IDC_CLEAR_SELECTION:
                    if (notification == BN_CLICKED) {
                        app->selectedPaths.clear();
                        updateSelectionControls(*app);
                    }
                    break;
                case IDC_PEER_COMBO:
                    if (notification == CBN_SELCHANGE) {
                        const int selection = static_cast<int>(
                            SendMessageW(app->peerCombo, CB_GETCURSEL, 0, 0));
                        if (selection > 0 &&
                            static_cast<std::size_t>(selection - 1) <
                                app->comboPeers.size()) {
                            const auto& peer =
                                app->comboPeers[static_cast<std::size_t>(
                                    selection - 1)];
                            std::wstring target = widen(peer.ip);
                            if (peer.port != landrop::kDefaultTcpPort) {
                                target += L":";
                                target += std::to_wstring(peer.port);
                            }
                            SetWindowTextW(app->targetEdit, target.c_str());
                            SetFocus(app->targetEdit);
                        }
                    }
                    break;
                case IDC_RECV_CHECK:
                    if (notification == BN_CLICKED &&
                        !app->syncingControls) {
                        const bool checked =
                            SendMessageW(app->receiveCheck, BM_GETCHECK, 0, 0) ==
                            BST_CHECKED;
                        app->controller->setAllowIncoming(checked);
                        saveCurrentSettings(*app);
                    }
                    break;
                case IDC_CHANGE_RECV:
                    if (notification == BN_CLICKED) {
                        const landrop::fs::path folder = chooseFolder(
                            hwnd, app->controller->receiveDirectory(),
                            L"选择接收文件夹");
                        if (!folder.empty()) {
                            std::string error;
                            if (!app->controller->setReceiveDirectory(folder,
                                                                      error)) {
                                showError(hwnd, widen(error));
                            } else {
                                setWindowTextIfChanged(
                                    app->receivePath,
                                    app->controller->receiveDirectory()
                                        .wstring());
                                saveCurrentSettings(*app);
                            }
                        }
                    }
                    break;
                case IDC_OPEN_RECV:
                    if (notification == BN_CLICKED) {
                        std::string error;
                        if (!landrop::platform::openInFileManager(
                                app->controller->receiveDirectory(), error)) {
                            showError(hwnd, widen(error));
                        }
                    }
                    break;
                case IDC_CANCEL_JOB:
                    if (notification == BN_CLICKED) {
                        const uint64_t id = selectedJobId(*app);
                        if (id != 0) {
                            app->controller->cancel(id);
                        }
                    }
                    break;
                case IDC_VIEW_LOGS:
                    if (notification == BN_CLICKED) {
                        showLogs(*app);
                    }
                    break;
                case IDC_CLEAR_DONE:
                    if (notification == BN_CLICKED) {
                        app->controller->clearFinished();
                        refreshUi(*app);
                    }
                    break;
                case IDC_TARGET_EDIT:
                    if (notification == EN_CHANGE) {
                        // Nothing needed; the button is enabled by selection.
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_CLOSE: {
            if (app != nullptr) {
                bool active = false;
                const landrop::UiSnapshot latest =
                    app->controller->snapshot();
                for (const auto& job : latest.transfers) {
                    if (landrop::isActiveState(job.state)) {
                        active = true;
                        break;
                    }
                }
                if (active) {
                    const int answer = MessageBoxW(
                        hwnd,
                        L"当前仍有传输任务，退出会中断传输。\n\n确定退出吗？",
                        L"LanDrop", MB_YESNO | MB_ICONWARNING |
                                         MB_DEFBUTTON2);
                    if (answer != IDYES) {
                        return 0;
                    }
                }
                KillTimer(hwnd, kRefreshTimer);
                saveCurrentSettings(*app);
                HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                EnableWindow(hwnd, FALSE);
                app->controller->stop();
                SetCursor(previous);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int runWindowsApp(HINSTANCE instance, int showCommand) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                           ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);

    landrop::AppSettings settings = landrop::SettingsStore::load();
    landrop::AppController controller;
    landrop::ControllerOptions options;
    options.receiveDirectory = settings.receiveDirectory;
    options.deviceName = landrop::platform::hostName();
    options.allowIncoming = settings.allowIncoming;
    options.enableDiscovery = true;

    std::string warning;
    if (!controller.start(options, warning)) {
        showError(nullptr, widen(warning));
        CoUninitialize();
        return 1;
    }

    AppWindow app;
    app.instance = instance;
    app.controller = &controller;
    app.settings = settings;
    app.startupWarning = warning;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon =
        LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    if (windowClass.hIcon == nullptr) {
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    windowClass.hIconSm =
        LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    if (windowClass.hIconSm == nullptr) {
        windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    }
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        controller.stop();
        CoUninitialize();
        return 1;
    }

    HDC screenDc = GetDC(nullptr);
    const int screenDpi =
        screenDc != nullptr ? GetDeviceCaps(screenDc, LOGPIXELSY) : 96;
    if (screenDc != nullptr) {
        ReleaseDC(nullptr, screenDc);
    }
    RECT desired{0, 0, MulDiv(980, screenDpi, 96),
                 MulDiv(650, screenDpi, 96)};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int windowWidth = desired.right - desired.left;
    int windowHeight = desired.bottom - desired.top;
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    windowWidth = std::min<int>(windowWidth, workArea.right - workArea.left - 16);
    windowHeight =
        std::min<int>(windowHeight, workArea.bottom - workArea.top - 16);
    const int x = workArea.left +
                  std::max<int>(0, (workArea.right - workArea.left -
                                    windowWidth) /
                                       2);
    const int y = workArea.top +
                  std::max<int>(0, (workArea.bottom - workArea.top -
                                    windowHeight) /
                                       2);

    HWND hwnd = CreateWindowExW(
        WS_EX_CONTROLPARENT, kWindowClass, L"LanDrop — 局域网文件快传",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, windowWidth, windowHeight,
        nullptr, nullptr, instance, &app);
    if (hwnd == nullptr) {
        controller.stop();
        CoUninitialize();
        return 1;
    }
    SetWindowTextW(hwnd, L"LanDrop — 局域网文件快传");
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);
    SetFocus(app.targetEdit);

    if (!app.startupWarning.empty()) {
        MessageBoxW(hwnd, widen(app.startupWarning).c_str(), L"LanDrop 启动提示",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments != nullptr) {
        std::vector<landrop::fs::path> initialPaths;
        for (int i = 1; i < argumentCount; ++i) {
            if (arguments[i] == nullptr || arguments[i][0] == L'-') {
                continue;
            }
            initialPaths.emplace_back(arguments[i]);
        }
        LocalFree(arguments);
        addSelectedPaths(app, initialPaths);
    }

    MSG message{};
    int exitCode = 0;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            if (focus == app.targetEdit || focus == app.sendButton) {
                SendMessageW(hwnd, WM_COMMAND,
                             MAKEWPARAM(IDC_SEND, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    controller.stop();
    destroyResources(app);
    CoUninitialize();
    return exitCode;
}
