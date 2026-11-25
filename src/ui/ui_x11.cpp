#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <X11/Xft/Xft.h>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/app_controller.h"
#include "core/common.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"

namespace {

namespace fs = std::filesystem;

constexpr int kBaseWidth = 980;
constexpr int kBaseHeight = 650;
constexpr int kMinWidth = 800;
constexpr int kMinHeight = 520;
constexpr unsigned int kPollIntervalMs = 150;
constexpr int kMaxDropBytes = 16 * 1024 * 1024;
constexpr double kPi = 3.14159265358979323846;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

struct UiColor {
    XftColor color{};
    unsigned long pixel = 0;
};

enum class FieldFocus { NoField, Target, Receive };

enum class HitKind {
    Nothing,
    TargetField,
    ReceiveField,
    PeerDropdown,
    PeerMenuItem,
    PickFiles,
    PickFolder,
    ClearSelection,
    Send,
    DropZone,
    ReceiveCheck,
    ChangeReceive,
    OpenReceive,
    CancelJob,
    ClearDone,
    ColumnDivider,
    TransferRow,
};

struct Hit {
    HitKind kind = HitKind::Nothing;
    int index = -1;

    bool operator==(const Hit& other) const {
        return kind == other.kind && index == other.index;
    }
    bool operator!=(const Hit& other) const { return !(*this == other); }
    explicit operator bool() const { return kind != HitKind::Nothing; }
};

struct PeerMenuItem {
    Rect rect;
    int index = 0;
};

struct PendingSend {
    uint64_t jobId = 0;
    std::vector<std::string> pathKeys;
};

class LinuxUi {
public:
    LinuxUi(landrop::AppController& controller, landrop::AppSettings settings,
            std::string startupWarning, int argc, char** argv)
        : controller_(controller),
          settings_(std::move(settings)),
          startupWarning_(std::move(startupWarning)),
          argc_(argc),
          argv_(argv) {}

    ~LinuxUi() { cleanup(); }

    bool initialize(std::string& error);
    int run();

private:
    int scale(int value) const {
        return static_cast<int>(std::lround(value * uiScale_));
    }

    bool openDisplay(std::string& error);
    void determineScale();
    void setupAtoms();
    bool createWindow(std::string& error);
    void createInputContext();
    void createColors();
    void createFonts();
    void createCursors();
    void cleanup();

    void computeLayout();
    void render();
    void renderToPixmap();

    void drawHeader(XftDraw* draw);
    void drawSendCard(XftDraw* draw);
    void drawReceiveCard(XftDraw* draw);
    void drawActivityCard(XftDraw* draw);
    void drawFooter(XftDraw* draw);
    void drawCardBackground(const Rect& rect);
    void drawButton(XftDraw* draw, const Rect& rect, const std::string& label,
                    bool primary, bool enabled, HitKind kind);
    void drawField(XftDraw* draw, const Rect& rect, const std::string& text,
                   bool focused, const std::string& placeholder,
                   FieldFocus field);
    void drawPeerSelector(XftDraw* draw);
    void drawPeerMenu(XftDraw* draw);
    void drawCheckbox(XftDraw* draw);
    void drawDropZone(XftDraw* draw);
    void drawTable(XftDraw* draw);
    void drawProgress(XftDraw* draw);
    std::string peerSelectorLabel() const;
    int peerMenuMaxScroll() const;
    void updateTableColumns();

    void fillRect(const Rect& rect, const UiColor& color);
    void fillRounded(const Rect& rect, int radius, const UiColor& color);
    void strokeRounded(const Rect& rect, int radius, const UiColor& color,
                       bool dashed = false);
    void drawLine(int x1, int y1, int x2, int y2, const UiColor& color);
    int textWidth(XftFont* font, const std::string& text) const;
    std::string ellipsize(XftFont* font, const std::string& text,
                          int maxWidth) const;
    void drawText(XftDraw* draw, XftFont* font, const UiColor& color, int x,
                  int baseline, const std::string& text) const;
    void drawTextInRect(XftDraw* draw, XftFont* font, const UiColor& color,
                        const Rect& rect, const std::string& text,
                        bool centered, bool rightAligned = false) const;
    int baselineFor(const Rect& rect, XftFont* font) const;

    void poll();
    void processPendingSends();
    void applySnapshotChanges();
    void handleAction(Hit hit);
    void setFocus(FieldFocus focus);
    void applyReceivePath(bool notify);
    void addPaths(const std::vector<fs::path>& paths);
    void addPathsFromUris(const std::string& data);
    void sendCurrentSelection();
    void saveSettingsNow();
    void setToast(const std::string& message);

    void handleEvent(XEvent& event);
    void handleKey(XKeyEvent& event);
    void handleButtonPress(XButtonEvent& event);
    void handleButtonRelease(XButtonEvent& event);
    void handleMotion(XMotionEvent& event);
    void handleClientMessage(XClientMessageEvent& event);
    void handleSelectionNotify(XSelectionEvent& event);
    void handlePropertyNotify(XPropertyEvent& event);

    Hit hitTest(int x, int y) const;
    void updateHover(int x, int y, bool updateCursor);

    bool readProperty(Atom property, std::string& data, Atom& actualType);
    void beginIncremental(Atom property, bool forDrop);
    void finishIncremental();
    bool processDropData(const std::string& data);
    void finishDrop(bool success);
    bool validDragSource(Window source) const;
    bool dragSourceOffersUriList(Window source, bool mayHaveMoreTypes,
                                 const long* types) const;
    void sendXdndStatus(bool accept);
    void sendXdndFinished(bool accepted);
    void requestClipboard(Time time);
    void insertIntoField(FieldFocus field, const std::string& text);

    std::string snapshotSignature() const;
    std::string transferStateText(landrop::TransferState state) const;
    std::string buildProgressText() const;
    std::vector<std::string> cellTexts(
        const landrop::TransferView& job) const;
    int visibleRowCount() const;
    void clampScroll();
    uint64_t selectedJob() const;
    bool selectedJobCancelable() const;

    static std::string findExecutable(const std::string& name);
    static bool runChooser(const std::string& executable,
                           const std::vector<std::string>& arguments,
                           std::string& output, int& exitCode);
    std::vector<fs::path> chooseFilesFromDialog();
    fs::path chooseFolderFromDialog(const fs::path& initial,
                                    const char* title);

    landrop::AppController& controller_;
    landrop::AppSettings settings_;
    std::string startupWarning_;
    int argc_ = 0;
    char** argv_ = nullptr;

    Display* display_ = nullptr;
    int screen_ = 0;
    Visual* visual_ = nullptr;
    Colormap colormap_ = 0;
    GC gc_ = nullptr;
    Window window_ = 0;
    XIM inputMethod_ = nullptr;
    XIC inputContext_ = nullptr;
    Pixmap backBuffer_ = 0;
    XftDraw* xftDraw_ = nullptr;
    int windowWidth_ = 0;
    int windowHeight_ = 0;
    double uiScale_ = 1.0;
    bool running_ = true;
    bool dirty_ = true;
    bool mapped_ = false;

    XftFont* fontTitle_ = nullptr;
    XftFont* fontSection_ = nullptr;
    XftFont* fontBody_ = nullptr;
    XftFont* fontBold_ = nullptr;
    XftFont* fontSmall_ = nullptr;
    XftFont* fontButton_ = nullptr;

    Cursor cursorArrow_ = 0;
    Cursor cursorHand_ = 0;
    Cursor cursorText_ = 0;
    Cursor cursorHResize_ = 0;
    Cursor currentCursor_ = 0;

    UiColor colorPage_{};
    UiColor colorCard_{};
    UiColor colorField_{};
    UiColor colorText_{};
    UiColor colorMuted_{};
    UiColor colorBorder_{};
    UiColor colorAccent_{};
    UiColor colorAccentDark_{};
    UiColor colorAccentTint_{};
    UiColor colorAccentBorder_{};
    UiColor colorSuccess_{};
    UiColor colorSuccessTint_{};
    UiColor colorDanger_{};
    UiColor colorDangerTint_{};
    UiColor colorWhite_{};
    UiColor colorShadow_{};
    UiColor colorDisabled_{};
    UiColor colorDisabledFill_{};

    Atom atomWmDelete_ = 0;
    Atom atomWmProtocols_ = 0;
    Atom atomWmTakeFocus_ = 0;
    Atom atomNetWmName_ = 0;
    Atom atomNetWmPid_ = 0;
    Atom atomUtf8String_ = 0;
    Atom atomXdndAware_ = 0;
    Atom atomXdndEnter_ = 0;
    Atom atomXdndPosition_ = 0;
    Atom atomXdndStatus_ = 0;
    Atom atomXdndLeave_ = 0;
    Atom atomXdndDrop_ = 0;
    Atom atomXdndFinished_ = 0;
    Atom atomXdndSelection_ = 0;
    Atom atomXdndActionCopy_ = 0;
    Atom atomXdndTypeList_ = 0;
    Atom atomTextUriList_ = 0;
    Atom atomIncr_ = 0;
    Atom atomClipboard_ = 0;
    Atom atomClipboardProperty_ = 0;
    Atom atomDropProperty_ = 0;

    Rect sendCard_;
    Rect receiveCard_;
    Rect activityCard_;
    Rect targetLabel_;
    Rect targetField_;
    Rect peerDropRect_;
    Rect peerMenuRect_;
    Rect pickFilesButton_;
    Rect pickFolderButton_;
    Rect clearButton_;
    Rect sendButton_;
    Rect dropZone_;
    Rect receiveCheckRow_;
    Rect receiveSaveLabel_;
    Rect receiveField_;
    Rect changeReceiveButton_;
    Rect openReceiveButton_;
    Rect receiveHint_;
    Rect cancelButton_;
    Rect clearDoneButton_;
    Rect tableHeader_;
    Rect tableBody_;
    Rect progressLabel_;
    Rect progressBar_;
    Rect footer_;

    std::vector<PeerMenuItem> peerMenuRects_;
    std::vector<landrop::PeerView> peerMenuItems_;
    std::string selectedPeerId_;
    bool peerMenuOpen_ = false;
    int peerMenuHover_ = -1;
    int peerMenuScroll_ = 0;
    int peerMenuVisibleRows_ = 0;

    std::array<double, 6> columnRatios_ = {0.07, 0.20, 0.22, 0.23, 0.10, 0.18};
    std::array<int, 7> tableColumnEdges_{};
    std::array<int, 6> tableColumnWidths_{};
    int tableTotalWidth_ = 0;
    int columnResizeIndex_ = -1;
    int columnResizeStartX_ = 0;
    std::array<double, 6> columnResizeStartRatios_{};

    std::vector<PendingSend> pendingSends_;

    Hit hover_;
    Hit pressed_;
    FieldFocus focus_ = FieldFocus::Target;

    std::string targetText_;
    std::string receiveText_;
    std::size_t targetCursor_ = 0;
    std::size_t receiveCursor_ = 0;

    std::vector<fs::path> selectedPaths_;
    landrop::UiSnapshot snapshot_;
    uint64_t lastLogSequence_ = 0;
    uint64_t selectedJobId_ = 0;
    int scrollOffset_ = 0;
    int rowHeight_ = 28;
    int visibleRows_ = 0;

    bool dragActive_ = false;
    Window dragSource_ = 0;
    int dragVersion_ = 0;
    bool dropAccepted_ = false;
    Time dropTime_ = CurrentTime;

    enum class SelectionPurpose { NoPurpose, Drop, Clipboard };
    SelectionPurpose selectionPurpose_ = SelectionPurpose::NoPurpose;
    Atom incrementalProperty_ = 0;
    std::string incrementalData_;
    bool incrementalDrop_ = false;

    std::string toast_;
    std::chrono::steady_clock::time_point toastUntil_{};
    std::string lastSignature_;
    FieldFocus clipboardFocus_ = FieldFocus::NoField;
};

namespace {

volatile std::sig_atomic_t g_terminateRequested = 0;

void requestTermination(int) { g_terminateRequested = 1; }

int handleX11Error(Display*, XErrorEvent*) {
    // Drag-and-drop sources routinely disappear while a drag is in flight.
    // Xlib's default handler terminates the process on BadWindow; for this
    // UI, X errors are non-fatal and an invalid DnD source is simply dropped.
    return 0;
}

double readXftDpi(Display* display) {
    XrmInitialize();
    char* resources = XResourceManagerString(display);
    if (resources == nullptr) {
        return 96.0;
    }
    XrmDatabase database = XrmGetStringDatabase(resources);
    if (database == nullptr) {
        return 96.0;
    }
    char* type = nullptr;
    XrmValue value{};
    double dpi = 96.0;
    if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) &&
        value.addr != nullptr) {
        dpi = std::strtod(value.addr, nullptr);
        if (!std::isfinite(dpi) || dpi < 72.0 || dpi > 384.0) {
            dpi = 96.0;
        }
    }
    XrmDestroyDatabase(database);
    return dpi;
}

std::string trimNewlines(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool percentDecode(const std::string& input, std::string& output) {
    output.clear();
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            output.push_back(input[i]);
            continue;
        }
        if (i + 2 >= input.size()) {
            return false;
        }
        const int hi = hex(input[i + 1]);
        const int lo = hex(input[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        output.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

bool pathFromFileUri(const std::string& uri, fs::path& result) {
    const std::string prefix = "file://";
    if (uri.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    std::string rest = uri.substr(prefix.size());
    std::string authority;
    std::string encodedPath;
    if (!rest.empty() && rest.front() == '/') {
        encodedPath = rest;
    } else {
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            return false;
        }
        authority = rest.substr(0, slash);
        encodedPath = rest.substr(slash);
    }
    if (!authority.empty() && !equalsIgnoreCase(authority, "localhost")) {
        char hostname[256]{};
        if (gethostname(hostname, sizeof(hostname) - 1) != 0 ||
            !equalsIgnoreCase(authority, hostname)) {
            return false;
        }
    }
    std::string decoded;
    if (!percentDecode(encodedPath, decoded) || decoded.empty() ||
        decoded.front() != '/') {
        return false;
    }
    if (!landrop::isValidUtf8(decoded)) {
        return false;
    }
    result = fs::path(decoded);
    return true;
}

std::size_t previousUtf8Boundary(const std::string& text, std::size_t index) {
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 &&
           (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80) {
        --index;
    }
    return index;
}

std::size_t nextUtf8Boundary(const std::string& text, std::size_t index) {
    if (index >= text.size()) {
        return text.size();
    }
    ++index;
    while (index < text.size() &&
           (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80) {
        ++index;
    }
    return index;
}

std::string removeUtf8Chrome(std::string text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (unsigned char c : text) {
        if (c == '\r' || c == '\n' || c == '\t' || c == 0) {
            continue;
        }
        if (c < 0x20) {
            continue;
        }
        cleaned.push_back(static_cast<char>(c));
    }
    return cleaned;
}

}  // namespace

bool LinuxUi::openDisplay(std::string& error) {
    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
        error = "无法打开 X11 显示。请在图形桌面中运行 LanDrop。";
        return false;
    }
    screen_ = DefaultScreen(display_);
    visual_ = DefaultVisual(display_, screen_);
    colormap_ = DefaultColormap(display_, screen_);
    return true;
}

void LinuxUi::determineScale() {
    const double dpi = readXftDpi(display_);
    uiScale_ = std::clamp(dpi / 96.0, 0.75, 3.0);
}

void LinuxUi::setupAtoms() {
    atomWmDelete_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    atomWmProtocols_ = XInternAtom(display_, "WM_PROTOCOLS", False);
    atomWmTakeFocus_ = XInternAtom(display_, "WM_TAKE_FOCUS", False);
    atomNetWmName_ = XInternAtom(display_, "_NET_WM_NAME", False);
    atomNetWmPid_ = XInternAtom(display_, "_NET_WM_PID", False);
    atomUtf8String_ = XInternAtom(display_, "UTF8_STRING", False);
    atomXdndAware_ = XInternAtom(display_, "XdndAware", False);
    atomXdndEnter_ = XInternAtom(display_, "XdndEnter", False);
    atomXdndPosition_ = XInternAtom(display_, "XdndPosition", False);
    atomXdndStatus_ = XInternAtom(display_, "XdndStatus", False);
    atomXdndLeave_ = XInternAtom(display_, "XdndLeave", False);
    atomXdndDrop_ = XInternAtom(display_, "XdndDrop", False);
    atomXdndFinished_ = XInternAtom(display_, "XdndFinished", False);
    atomXdndSelection_ = XInternAtom(display_, "XdndSelection", False);
    atomXdndActionCopy_ = XInternAtom(display_, "XdndActionCopy", False);
    atomXdndTypeList_ = XInternAtom(display_, "XdndTypeList", False);
    atomTextUriList_ = XInternAtom(display_, "text/uri-list", False);
    atomIncr_ = XInternAtom(display_, "INCR", False);
    atomClipboard_ = XInternAtom(display_, "CLIPBOARD", False);
    atomClipboardProperty_ =
        XInternAtom(display_, "LANDROP_CLIPBOARD", False);
    atomDropProperty_ = XInternAtom(display_, "LANDROP_DROP", False);
}

bool LinuxUi::createWindow(std::string& error) {
    const int screenWidth = DisplayWidth(display_, screen_);
    const int screenHeight = DisplayHeight(display_, screen_);
    const int availableWidth = std::max(scale(480), screenWidth - scale(20));
    const int availableHeight = std::max(scale(360), screenHeight - scale(40));
    windowWidth_ = std::min(scale(kBaseWidth), availableWidth);
    windowHeight_ = std::min(scale(kBaseHeight), availableHeight);

    const int x = std::max(0, (screenWidth - windowWidth_) / 2);
    const int y = std::max(0, (screenHeight - windowHeight_) / 3);
    window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen_), x, y,
                                  static_cast<unsigned int>(windowWidth_),
                                  static_cast<unsigned int>(windowHeight_), 0,
                                  colorBorder_.pixel, colorPage_.pixel);
    if (window_ == 0) {
        error = "无法创建 X11 窗口";
        return false;
    }
    XStoreName(display_, window_, "LanDrop");
    const std::string title = "LanDrop — 局域网文件快传";
    XChangeProperty(display_, window_, atomNetWmName_, atomUtf8String_, 8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title.data()),
                    static_cast<int>(title.size()));
    XClassHint classHint{};
    classHint.res_name = const_cast<char*>("landrop");
    classHint.res_class = const_cast<char*>("LanDrop");
    XSetClassHint(display_, window_, &classHint);
    const long pid = static_cast<long>(getpid());
    XChangeProperty(display_, window_, atomNetWmPid_, XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&pid), 1);

    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = std::min(scale(kMinWidth), availableWidth);
    hints.min_height = std::min(scale(kMinHeight), availableHeight);
    XSetWMNormalHints(display_, window_, &hints);

    Atom protocols[] = {atomWmDelete_, atomWmTakeFocus_};
    XSetWMProtocols(display_, window_, protocols, 2);
    unsigned long xdndVersion = 5;
    XChangeProperty(display_, window_, atomXdndAware_, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(&xdndVersion), 1);

    XSelectInput(display_, window_,
                 ExposureMask | StructureNotifyMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                     FocusChangeMask | PropertyChangeMask);
    XMapWindow(display_, window_);
    XFlush(display_);
    return true;
}

void LinuxUi::createInputContext() {
    // XIM is what lets fcitx/ibus/etc. commit Chinese (and other IME) text
    // into the directory field. It is optional: without an input method
    // server we simply fall back to XLookupString in handleKey().
    inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    if (inputMethod_ == nullptr) {
        return;
    }
    inputContext_ = XCreateIC(
        inputMethod_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
        XNClientWindow, window_, XNFocusWindow, window_,
        static_cast<void*>(nullptr));
    if (inputContext_ == nullptr) {
        XCloseIM(inputMethod_);
        inputMethod_ = nullptr;
    }
}

void LinuxUi::createColors() {
    auto alloc = [&](UiColor& target, int r, int g, int b) {
        XRenderColor render{};
        render.red = static_cast<unsigned short>(r * 257);
        render.green = static_cast<unsigned short>(g * 257);
        render.blue = static_cast<unsigned short>(b * 257);
        render.alpha = 0xffff;
        XftColorAllocValue(display_, visual_, colormap_, &render,
                           &target.color);
        target.pixel = target.color.pixel;
    };
    alloc(colorPage_, 246, 248, 252);
    alloc(colorCard_, 255, 255, 255);
    alloc(colorField_, 250, 251, 254);
    alloc(colorText_, 17, 24, 39);
    alloc(colorMuted_, 100, 116, 139);
    alloc(colorBorder_, 226, 232, 240);
    alloc(colorAccent_, 37, 99, 235);
    alloc(colorAccentDark_, 29, 78, 216);
    alloc(colorAccentTint_, 239, 246, 255);
    alloc(colorAccentBorder_, 147, 180, 245);
    alloc(colorSuccess_, 5, 150, 105);
    alloc(colorSuccessTint_, 236, 253, 245);
    alloc(colorDanger_, 220, 38, 38);
    alloc(colorDangerTint_, 254, 242, 242);
    alloc(colorWhite_, 255, 255, 255);
    alloc(colorShadow_, 235, 239, 247);
    alloc(colorDisabled_, 148, 163, 184);
    alloc(colorDisabledFill_, 241, 245, 249);
}

XftFont* openUiFont(Display* display, int screen, int pixels, bool bold) {
    std::string pattern = "sans:pixelsize=" + std::to_string(pixels) +
                          ":weight=" + (bold ? "bold" : "normal") +
                          ":lang=zh-cn:antialias=true";
    XftFont* font = XftFontOpenName(display, screen, pattern.c_str());
    if (font == nullptr) {
        pattern = "sans:pixelsize=" + std::to_string(pixels);
        font = XftFontOpenName(display, screen, pattern.c_str());
    }
    if (font == nullptr) {
        font = XftFontOpenName(display, screen, "fixed");
    }
    return font;
}

void LinuxUi::createFonts() {
    fontTitle_ = openUiFont(display_, screen_, scale(29), true);
    fontSection_ = openUiFont(display_, screen_, scale(15), true);
    fontBody_ = openUiFont(display_, screen_, scale(12), false);
    fontBold_ = openUiFont(display_, screen_, scale(12), true);
    fontSmall_ = openUiFont(display_, screen_, scale(11), false);
    fontButton_ = openUiFont(display_, screen_, scale(12), true);
}

void LinuxUi::createCursors() {
    cursorArrow_ = XCreateFontCursor(display_, XC_left_ptr);
    cursorHand_ = XCreateFontCursor(display_, XC_hand2);
    cursorText_ = XCreateFontCursor(display_, XC_xterm);
    cursorHResize_ = XCreateFontCursor(display_, XC_sb_h_double_arrow);
    currentCursor_ = cursorArrow_;
    XDefineCursor(display_, window_, currentCursor_);
}

bool LinuxUi::initialize(std::string& error) {
    std::setlocale(LC_ALL, "");
    if (!openDisplay(error)) {
        return false;
    }
    XSetErrorHandler(handleX11Error);
    std::signal(SIGINT, requestTermination);
    std::signal(SIGTERM, requestTermination);
    const int displayFdFlags = fcntl(ConnectionNumber(display_), F_GETFD, 0);
    if (displayFdFlags >= 0) {
        fcntl(ConnectionNumber(display_), F_SETFD,
              displayFdFlags | FD_CLOEXEC);
    }
    determineScale();
    setupAtoms();
    createColors();
    createFonts();
    if (fontTitle_ == nullptr || fontBody_ == nullptr ||
        fontSmall_ == nullptr || fontButton_ == nullptr) {
        error = "无法加载可用字体，请安装 fontconfig 字体。";
        return false;
    }
    if (!createWindow(error)) {
        return false;
    }
    createInputContext();
    gc_ = XCreateGC(display_, window_, 0, nullptr);
    if (gc_ == nullptr) {
        error = "无法创建绘图上下文";
        return false;
    }
    createCursors();
    receiveText_ = landrop::pathToUtf8(controller_.receiveDirectory());
    targetText_.clear();
    targetCursor_ = 0;
    receiveCursor_ = receiveText_.size();
    if (!startupWarning_.empty()) {
        setToast(startupWarning_);
    }
    for (int i = 1; i < argc_; ++i) {
        if (argv_[i] == nullptr || argv_[i][0] == '-') {
            continue;
        }
        std::error_code ec;
        const fs::path candidate(argv_[i]);
        if (fs::exists(candidate, ec)) {
            addPaths({candidate});
        }
    }
    poll();
    dirty_ = true;
    return true;
}

void LinuxUi::cleanup() {
    if (display_ != nullptr) {
        if (inputContext_ != nullptr) {
            XDestroyIC(inputContext_);
            inputContext_ = nullptr;
        }
        if (inputMethod_ != nullptr) {
            XCloseIM(inputMethod_);
            inputMethod_ = nullptr;
        }
        if (xftDraw_ != nullptr) {
            XftDrawDestroy(xftDraw_);
            xftDraw_ = nullptr;
        }
        if (backBuffer_ != 0) {
            XFreePixmap(display_, backBuffer_);
            backBuffer_ = 0;
        }
        if (gc_ != nullptr) {
            XFreeGC(display_, gc_);
            gc_ = nullptr;
        }
        XftFont** fonts[] = {&fontTitle_, &fontSection_, &fontBody_,
                             &fontBold_, &fontSmall_, &fontButton_};
        for (XftFont** font : fonts) {
            if (*font != nullptr) {
                XftFontClose(display_, *font);
                *font = nullptr;
            }
        }
        UiColor* colors[] = {
            &colorPage_,       &colorCard_,       &colorField_,
            &colorText_,       &colorMuted_,      &colorBorder_,
            &colorAccent_,     &colorAccentDark_, &colorAccentTint_,
            &colorAccentBorder_, &colorSuccess_,  &colorSuccessTint_,
            &colorDanger_,     &colorDangerTint_, &colorWhite_,
            &colorShadow_,     &colorDisabled_,   &colorDisabledFill_};
        for (UiColor* color : colors) {
            XftColorFree(display_, visual_, colormap_, &color->color);
        }
        if (cursorArrow_ != 0) XFreeCursor(display_, cursorArrow_);
        if (cursorHand_ != 0) XFreeCursor(display_, cursorHand_);
        if (cursorText_ != 0) XFreeCursor(display_, cursorText_);
        if (cursorHResize_ != 0) XFreeCursor(display_, cursorHResize_);
        if (window_ != 0) {
            XDestroyWindow(display_, window_);
            window_ = 0;
        }
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

void LinuxUi::computeLayout() {
    const int width = windowWidth_;
    const int height = windowHeight_;
    int margin = scale(22);
    const int maxContent = scale(1180);
    if (width - margin * 2 > maxContent) {
        margin = (width - maxContent) / 2;
    }
    const int contentWidth = std::max(scale(320), width - margin * 2);
    const int headerHeight = scale(84);
    const int cardY = headerHeight + scale(14);
    const int cardHeight = scale(216);
    const int gap = scale(14);
    const int leftWidth = (contentWidth - gap) * 58 / 100;
    const int rightWidth = contentWidth - gap - leftWidth;
    sendCard_ = {margin, cardY, leftWidth, cardHeight};
    receiveCard_ = {margin + leftWidth + gap, cardY, rightWidth, cardHeight};

    const int pad = scale(16);
    const int innerX = sendCard_.x + pad;
    const int innerW = sendCard_.w - pad * 2;
    const int targetY = cardY + scale(46);
    const int labelWidth = scale(44);
    const int smallGap = scale(6);
    const int availableTargetWidth = innerW - labelWidth - smallGap * 2;
    const int comboWidth =
        std::max(scale(105),
                 std::min(scale(200), availableTargetWidth * 42 / 100));
    const int editWidth =
        std::max(scale(80), availableTargetWidth - comboWidth);
    targetLabel_ = {innerX, targetY, labelWidth, scale(30)};
    targetField_ = {innerX + labelWidth + smallGap, targetY, editWidth,
                    scale(30)};
    peerDropRect_ = {targetField_.right() + smallGap, targetY, comboWidth,
                     scale(30)};
    const int buttonY = cardY + scale(86);
    const int buttonH = scale(30);
    pickFilesButton_ = {innerX, buttonY, scale(92), buttonH};
    pickFolderButton_ = {innerX + scale(92) + scale(8), buttonY, scale(106),
                         buttonH};
    clearButton_ = {innerX + scale(92) + scale(8) + scale(106) + scale(8),
                    buttonY, scale(54), buttonH};
    sendButton_ = {innerX + innerW - scale(112), buttonY, scale(112),
                   buttonH};
    dropZone_ = {innerX, cardY + scale(124), innerW, scale(76)};

    const int recvX = receiveCard_.x + pad;
    const int recvW = receiveCard_.w - pad * 2;
    receiveCheckRow_ = {recvX, cardY + scale(44), recvW, scale(26)};
    receiveSaveLabel_ = {recvX, cardY + scale(78), recvW, scale(20)};
    receiveField_ = {recvX, cardY + scale(100), recvW, scale(28)};
    changeReceiveButton_ = {recvX, cardY + scale(136), scale(112),
                            scale(28)};
    openReceiveButton_ = {recvX + scale(112) + scale(8), cardY + scale(136),
                          scale(86), scale(28)};
    receiveHint_ = {recvX, cardY + scale(176), recvW, scale(22)};

    const int activityTop = cardY + cardHeight + gap;
    const int activityBottom = std::max(activityTop + scale(140),
                                        height - scale(30));
    activityCard_ = {margin, activityTop, contentWidth,
                     activityBottom - activityTop};
    cancelButton_ = {activityCard_.right() - pad - scale(88),
                     activityTop + scale(10), scale(88), scale(28)};
    clearDoneButton_ = {cancelButton_.x - scale(6) - scale(102),
                        activityTop + scale(10), scale(102), scale(28)};

    tableHeader_ = {activityCard_.x + scale(14), activityTop + scale(44),
                    activityCard_.w - scale(28), scale(25)};
    progressBar_ = {activityCard_.x + scale(16),
                    activityCard_.bottom() - scale(20),
                    activityCard_.w - scale(32), scale(8)};
    progressLabel_ = {activityCard_.x + scale(16),
                      progressBar_.y - scale(24),
                      activityCard_.w - scale(32), scale(20)};
    tableBody_ = {tableHeader_.x, tableHeader_.bottom(),
                  tableHeader_.w,
                  std::max(0, progressLabel_.y - tableHeader_.bottom() -
                                  scale(4))};
    rowHeight_ = scale(30);
    visibleRows_ = std::max(0, tableBody_.h / rowHeight_);
    footer_ = {0, height - scale(22), width, scale(22)};
    updateTableColumns();
    clampScroll();
}

void LinuxUi::clampScroll() {
    const int total = static_cast<int>(snapshot_.transfers.size());
    const int maxOffset = std::max(0, total - visibleRows_);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxOffset);
}

void LinuxUi::updateTableColumns() {
    const int totalWidth = std::max(scale(420), tableHeader_.w - scale(18));
    tableTotalWidth_ = totalWidth;
    const int minWidth = scale(40);
    double sum = 0.0;
    for (double ratio : columnRatios_) {
        sum += ratio;
    }
    if (!(sum > 0.0)) {
        columnRatios_ = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
        sum = 6.0;
    }
    int widths[6]{};
    int used = 0;
    for (int i = 0; i < 5; ++i) {
        widths[i] = std::max(
            minWidth,
            static_cast<int>(std::lround(
                static_cast<double>(totalWidth) * columnRatios_[i] / sum)));
        used += widths[i];
    }
    widths[5] = std::max(0, totalWidth - used);
    while (widths[5] < minWidth) {
        int widest = 0;
        for (int i = 1; i < 5; ++i) {
            if (widths[i] > widths[widest]) {
                widest = i;
            }
        }
        if (widths[widest] <= minWidth) {
            break;
        }
        --widths[widest];
        ++widths[5];
    }
    int edge = tableHeader_.x + scale(4);
    tableColumnEdges_[0] = edge;
    for (int i = 0; i < 6; ++i) {
        tableColumnWidths_[i] = widths[i];
        edge += widths[i];
        tableColumnEdges_[i + 1] = edge;
    }
}

std::string LinuxUi::snapshotSignature() const {
    std::string signature = snapshot_.deviceName;
    signature += '|';
    for (const auto& ip : snapshot_.localIps) {
        signature += ip;
        signature += ',';
    }
    signature += '|';
    signature += std::to_string(snapshot_.listenPort);
    signature += snapshot_.serverListening ? '1' : '0';
    signature += snapshot_.discoveryActive ? '1' : '0';
    signature += snapshot_.allowIncoming ? '1' : '0';
    signature += snapshot_.serviceMessage;
    signature += landrop::pathToUtf8(snapshot_.receiveDirectory);
    for (const auto& peer : snapshot_.peers) {
        signature += peer.id + peer.name + peer.ip +
                     std::to_string(peer.port) + ';';
    }
    for (const auto& job : snapshot_.transfers) {
        signature += std::to_string(job.id);
        signature += job.outgoing ? 'O' : 'I';
        signature += job.peer;
        signature += job.currentFile;
        signature += job.status;
        signature += job.error;
        signature += std::to_string(job.bytesDone);
        signature += '/';
        signature += std::to_string(job.totalBytes);
        signature += '@';
        signature += std::to_string(static_cast<int>(job.state));
        signature += ';';
    }
    return signature;
}

void LinuxUi::poll() {
    snapshot_ = controller_.snapshot();
    processPendingSends();
    const auto logs = controller_.logsAfter(lastLogSequence_);
    for (const auto& entry : logs) {
        lastLogSequence_ = std::max(lastLogSequence_, entry.sequence);
        if (entry.level == landrop::LogLevel::Error ||
            entry.level == landrop::LogLevel::Warning ||
            entry.level == landrop::LogLevel::LogSuccess) {
            setToast(entry.text);
        }
    }
    if (focus_ != FieldFocus::Receive) {
        const std::string controllerPath =
            landrop::pathToUtf8(controller_.receiveDirectory());
        if (receiveText_ != controllerPath) {
            receiveText_ = controllerPath;
            receiveCursor_ = receiveText_.size();
            dirty_ = true;
        }
    }
    if (selectedJobId_ != 0) {
        bool found = false;
        for (const auto& job : snapshot_.transfers) {
            if (job.id == selectedJobId_) {
                found = true;
                break;
            }
        }
        if (!found) {
            selectedJobId_ = 0;
            dirty_ = true;
        }
    }
    clampScroll();
    const std::string signature = snapshotSignature();
    if (signature != lastSignature_) {
        lastSignature_ = signature;
        dirty_ = true;
    }
    if (!toast_.empty() &&
        std::chrono::steady_clock::now() > toastUntil_) {
        toast_.clear();
        dirty_ = true;
    }
}

void LinuxUi::processPendingSends() {
    if (pendingSends_.empty()) {
        return;
    }
    bool selectionChanged = false;
    for (auto it = pendingSends_.begin(); it != pendingSends_.end();) {
        const landrop::TransferView* job = nullptr;
        for (const auto& candidate : snapshot_.transfers) {
            if (candidate.id == it->jobId) {
                job = &candidate;
                break;
            }
        }
        if (job == nullptr) {
            // The user cleared the finished job before this poll; keep the
            // selection rather than guessing the outcome.
            it = pendingSends_.erase(it);
            continue;
        }
        if (job->state == landrop::TransferState::Completed) {
            const std::set<std::string> sentKeys(it->pathKeys.begin(),
                                                 it->pathKeys.end());
            const std::size_t before = selectedPaths_.size();
            selectedPaths_.erase(
                std::remove_if(
                    selectedPaths_.begin(), selectedPaths_.end(),
                    [&](const fs::path& path) {
                        return sentKeys.count(
                                   landrop::pathCollisionKey(path)) != 0;
                    }),
                selectedPaths_.end());
            if (selectedPaths_.size() != before) {
                selectionChanged = true;
            }
            it = pendingSends_.erase(it);
            continue;
        }
        if (job->state == landrop::TransferState::Failed ||
            job->state == landrop::TransferState::Canceled) {
            // Keep the selection so the user can retry after a failure.
            it = pendingSends_.erase(it);
            continue;
        }
        ++it;
    }
    if (selectionChanged) {
        dirty_ = true;
    }
}

int LinuxUi::textWidth(XftFont* font, const std::string& text) const {
    if (font == nullptr || text.empty()) {
        return 0;
    }
    XGlyphInfo info{};
    XftTextExtentsUtf8(display_, font,
                       reinterpret_cast<const FcChar8*>(text.data()),
                       static_cast<int>(text.size()), &info);
    return info.xOff;
}

std::string LinuxUi::ellipsize(XftFont* font, const std::string& text,
                               int maxWidth) const {
    if (font == nullptr || maxWidth <= 0) {
        return {};
    }
    if (textWidth(font, text) <= maxWidth) {
        return text;
    }
    const std::string ellipsis = "…";
    const int ellipsisWidth = textWidth(font, ellipsis);
    if (ellipsisWidth > maxWidth) {
        return {};
    }
    // Find the longest codepoint prefix that still fits with an ellipsis.
    // Measuring each shrinking prefix is O(n) Xft shaping calls per cell; use
    // codepoint boundaries plus binary search instead.
    std::vector<std::size_t> boundaries;
    boundaries.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size();) {
        boundaries.push_back(i);
        i = nextUtf8Boundary(text, i);
    }
    boundaries.push_back(text.size());
    std::size_t low = 0;
    std::size_t high = boundaries.size() - 2;
    std::size_t best = 0;
    while (low <= high) {
        const std::size_t mid = low + (high - low) / 2;
        const std::size_t cut = boundaries[mid];
        if (textWidth(font, text.substr(0, cut)) + ellipsisWidth <=
            maxWidth) {
            best = cut;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return text.substr(0, best) + ellipsis;
}

void LinuxUi::drawText(XftDraw* draw, XftFont* font, const UiColor& color,
                       int x, int baseline, const std::string& text) const {
    if (font == nullptr || text.empty()) {
        return;
    }
    XftDrawStringUtf8(draw, &color.color, font, x, baseline,
                      reinterpret_cast<const FcChar8*>(text.data()),
                      static_cast<int>(text.size()));
}

int LinuxUi::baselineFor(const Rect& rect, XftFont* font) const {
    if (font == nullptr) {
        return rect.y + rect.h / 2;
    }
    return rect.y + (rect.h - (font->ascent + font->descent)) / 2 +
           font->ascent;
}

void LinuxUi::drawTextInRect(XftDraw* draw, XftFont* font,
                             const UiColor& color, const Rect& rect,
                             const std::string& text, bool centered,
                             bool rightAligned) const {
    const std::string visible = ellipsize(font, text, rect.w);
    const int width = textWidth(font, visible);
    int x = rect.x;
    if (centered) {
        x = rect.x + (rect.w - width) / 2;
    } else if (rightAligned) {
        x = rect.right() - width;
    }
    drawText(draw, font, color, x, baselineFor(rect, font), visible);
}

void LinuxUi::fillRect(const Rect& rect, const UiColor& color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    XSetForeground(display_, gc_, color.pixel);
    XFillRectangle(display_, backBuffer_, gc_, rect.x, rect.y,
                   static_cast<unsigned int>(rect.w),
                   static_cast<unsigned int>(rect.h));
}

std::vector<XPoint> roundedPoints(const Rect& rect, int radius) {
    const int r = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    std::vector<XPoint> points;
    if (r == 0) {
        points.push_back({static_cast<short>(rect.x),
                          static_cast<short>(rect.y)});
        points.push_back({static_cast<short>(rect.right()),
                          static_cast<short>(rect.y)});
        points.push_back({static_cast<short>(rect.right()),
                          static_cast<short>(rect.bottom())});
        points.push_back({static_cast<short>(rect.x),
                          static_cast<short>(rect.bottom())});
        return points;
    }
    constexpr int kSteps = 6;
    auto addArc = [&](int cx, int cy, double from, double to) {
        for (int i = 0; i <= kSteps; ++i) {
            const double angle =
                from + (to - from) * static_cast<double>(i) / kSteps;
            points.push_back(
                {static_cast<short>(std::lround(cx + r * std::cos(angle))),
                 static_cast<short>(
                     std::lround(cy + r * std::sin(angle)))});
        }
    };
    addArc(rect.x + r, rect.y + r, kPi, 1.5 * kPi);
    addArc(rect.right() - r, rect.y + r, 1.5 * kPi, 2.0 * kPi);
    addArc(rect.right() - r, rect.bottom() - r, 0.0, 0.5 * kPi);
    addArc(rect.x + r, rect.bottom() - r, 0.5 * kPi, kPi);
    return points;
}

void LinuxUi::fillRounded(const Rect& rect, int radius,
                          const UiColor& color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    auto points = roundedPoints(rect, radius);
    XSetForeground(display_, gc_, color.pixel);
    XFillPolygon(display_, backBuffer_, gc_, points.data(),
                 static_cast<int>(points.size()), Convex, CoordModeOrigin);
}

void LinuxUi::strokeRounded(const Rect& rect, int radius,
                            const UiColor& color, bool dashed) {
    if (rect.w <= 0 || rect.h <= 0) return;
    auto points = roundedPoints(rect, radius);
    points.push_back(points.front());
    XSetForeground(display_, gc_, color.pixel);
    if (dashed) {
        static const char dashes[] = {3, 3};
        XSetLineAttributes(display_, gc_, 1, LineOnOffDash, CapButt,
                           JoinMiter);
        XSetDashes(display_, gc_, 0, dashes, 2);
    } else {
        XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    }
    XDrawLines(display_, backBuffer_, gc_, points.data(),
               static_cast<int>(points.size()), CoordModeOrigin);
    if (dashed) {
        XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    }
}

void LinuxUi::drawLine(int x1, int y1, int x2, int y2,
                       const UiColor& color) {
    XSetForeground(display_, gc_, color.pixel);
    XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    XDrawLine(display_, backBuffer_, gc_, x1, y1, x2, y2);
}

void LinuxUi::drawCardBackground(const Rect& rect) {
    const int radius = scale(10);
    fillRounded({rect.x + 1, rect.y + 2, rect.w, rect.h}, radius,
                colorShadow_);
    fillRounded(rect, radius, colorCard_);
    strokeRounded(rect, radius, colorBorder_);
}

void LinuxUi::drawHeader(XftDraw* draw) {
    const int headerHeight = scale(84);
    fillRect({0, 0, windowWidth_, headerHeight}, colorCard_);
    drawLine(0, headerHeight - 1, windowWidth_, headerHeight - 1,
             colorBorder_);

    const int margin = sendCard_.x;
    const int logoSize = scale(40);
    Rect logo{margin, (headerHeight - logoSize) / 2, logoSize, logoSize};
    fillRounded(logo, scale(10), colorAccent_);
    XSetForeground(display_, gc_, colorWhite_.pixel);
    XSetLineAttributes(display_, gc_, std::max(2, scale(2)), LineSolid,
                       CapRound, JoinRound);
    const int left = logo.x + logo.w / 4;
    const int right = logo.x + logo.w * 3 / 4;
    const int top = logo.y + logo.h * 35 / 100;
    const int bottom = logo.y + logo.h * 65 / 100;
    XDrawLine(display_, backBuffer_, gc_, left, top, right, top);
    XDrawLine(display_, backBuffer_, gc_, right - scale(5), top - scale(4),
              right, top);
    XDrawLine(display_, backBuffer_, gc_, right - scale(5), top + scale(4),
              right, top);
    XDrawLine(display_, backBuffer_, gc_, right, bottom, left, bottom);
    XDrawLine(display_, backBuffer_, gc_, left + scale(5), bottom - scale(4),
              left, bottom);
    XDrawLine(display_, backBuffer_, gc_, left + scale(5), bottom + scale(4),
              left, bottom);

    Rect titleRect{margin + scale(54), scale(10), windowWidth_ / 2, scale(38)};
    drawTextInRect(draw, fontTitle_, colorText_, titleRect, "LanDrop", false);
    Rect subtitleRect{margin + scale(55), scale(45), windowWidth_ / 2,
                      scale(22)};
    drawTextInRect(draw, fontSmall_, colorMuted_, subtitleRect,
                   "局域网文件直传  ·  拖放即发送", false);

    std::string serviceText;
    UiColor serviceColor = colorMuted_;
    UiColor serviceFill = colorField_;
    if (!snapshot_.serverListening) {
        serviceText = "接收服务未启动";
        serviceColor = colorDanger_;
        serviceFill = colorDangerTint_;
    } else if (!snapshot_.allowIncoming) {
        serviceText = "接收已关闭";
    } else {
        serviceText = snapshot_.discoveryActive ? "接收已开启 · 可被发现"
                                                : "接收已开启";
        serviceColor = colorSuccess_;
        serviceFill = colorSuccessTint_;
    }
    const int pillPad = scale(12);
    const int pillHeight = scale(24);
    const int textW = textWidth(fontSmall_, serviceText);
    const int dot = scale(6);
    const int pillW = textW + pillPad * 2 + dot + scale(8);
    Rect pill{windowWidth_ - margin - pillW, scale(16), pillW, pillHeight};
    fillRounded(pill, pillHeight / 2, serviceFill);
    XSetForeground(display_, gc_, serviceColor.pixel);
    XFillArc(display_, backBuffer_, gc_, pill.x + pillPad,
             pill.y + (pillHeight - dot) / 2, dot, dot, 0, 360 * 64);
    Rect serviceRect{pill.x + pillPad + dot + scale(8), pill.y,
                     textW + scale(2), pillHeight};
    drawTextInRect(draw, fontSmall_, serviceColor, serviceRect, serviceText,
                   false);

    std::string info;
    if (snapshot_.localIps.empty()) {
        info = "未检测到局域网 IPv4 地址";
    } else {
        info = "本机 ";
        for (std::size_t i = 0;
             i < snapshot_.localIps.size() && i < 2; ++i) {
            if (i != 0) info += " / ";
            info += snapshot_.localIps[i];
        }
        if (snapshot_.localIps.size() > 2) info += " 等";
        if (snapshot_.listenPort != 0) {
            info += ":" + std::to_string(snapshot_.listenPort);
        }
    }
    if (!snapshot_.deviceName.empty()) {
        info += "  ·  " + snapshot_.deviceName;
    }
    Rect infoRect{windowWidth_ / 2, scale(47),
                  windowWidth_ - margin - windowWidth_ / 2, scale(22)};
    drawTextInRect(draw, fontSmall_, colorMuted_, infoRect, info, false, true);
}

void LinuxUi::drawButton(XftDraw* draw, const Rect& rect,
                         const std::string& label, bool primary, bool enabled,
                         HitKind kind) {
    const bool hovered = hover_.kind == kind;
    const bool pressed = pressed_.kind == kind;
    UiColor fill = colorCard_;
    UiColor border = colorBorder_;
    UiColor text = colorText_;
    if (primary) {
        fill = enabled ? (pressed ? colorAccentDark_ : colorAccent_)
                       : colorDisabledFill_;
        border = fill;
        text = enabled ? colorWhite_ : colorMuted_;
    } else {
        fill = enabled ? (pressed ? colorAccentTint_
                                  : (hovered ? colorField_ : colorCard_))
                       : colorDisabledFill_;
        border = enabled ? (hovered ? colorAccentBorder_ : colorBorder_)
                         : colorBorder_;
        text = enabled ? colorText_ : colorDisabled_;
    }
    fillRounded(rect, scale(8), fill);
    strokeRounded(rect, scale(8), border);
    drawTextInRect(draw, fontButton_, text, rect, label, true);
}

void LinuxUi::drawField(XftDraw* draw, const Rect& rect,
                        const std::string& text, bool focused,
                        const std::string& placeholder, FieldFocus field) {
    fillRounded(rect, scale(6), colorField_);
    strokeRounded(rect, scale(6),
                  focused ? colorAccent_ : colorBorder_);
    const int pad = scale(8);
    Rect textRect{rect.x + pad, rect.y, rect.w - pad * 2, rect.h};
    const bool showPlaceholder = text.empty();
    const std::string visible = showPlaceholder ? placeholder : text;
    const std::string clipped =
        ellipsize(fontBody_, visible, std::max(0, textRect.w - scale(2)));
    drawText(draw, fontBody_,
             showPlaceholder ? colorMuted_ : colorText_, textRect.x,
             baselineFor(textRect, fontBody_), clipped);
    if (focused && !showPlaceholder) {
        const std::string& active =
            field == FieldFocus::Target ? targetText_ : receiveText_;
        const std::size_t cursor =
            field == FieldFocus::Target ? targetCursor_ : receiveCursor_;
        const std::string prefix = active.substr(0, cursor);
        int caret = textRect.x + textWidth(fontBody_, prefix);
        caret = std::min(caret, textRect.right() - scale(2));
        drawLine(caret, rect.y + scale(6), caret, rect.bottom() - scale(6),
                 colorAccent_);
    } else if (focused && showPlaceholder) {
        drawLine(textRect.x, rect.y + scale(6), textRect.x,
                 rect.bottom() - scale(6), colorAccent_);
    }
}

std::string LinuxUi::peerSelectorLabel() const {
    if (!snapshot_.discoveryActive) {
        return "未启用设备发现";
    }
    if (snapshot_.peers.empty()) {
        return "未发现其他设备";
    }
    if (!selectedPeerId_.empty()) {
        for (const auto& peer : snapshot_.peers) {
            if (peer.id != selectedPeerId_) {
                continue;
            }
            std::string address = peer.ip;
            if (peer.port != landrop::kDefaultTcpPort) {
                address += ":" + std::to_string(peer.port);
            }
            if (targetText_ == address) {
                return peer.name.empty()
                           ? address
                           : peer.name + "  ·  " + address;
            }
            break;
        }
    }
    return "选择发现的设备…";
}

int LinuxUi::peerMenuMaxScroll() const {
    return std::max(0, static_cast<int>(peerMenuItems_.size()) -
                           peerMenuVisibleRows_);
}

void LinuxUi::drawPeerSelector(XftDraw* draw) {
    const bool canDiscover = snapshot_.discoveryActive;
    const bool hasPeers = canDiscover && !snapshot_.peers.empty();
    const bool hovered = hover_.kind == HitKind::PeerDropdown;
    fillRounded(peerDropRect_, scale(6), colorField_);
    strokeRounded(peerDropRect_, scale(6),
                  (peerMenuOpen_ || hovered) ? colorAccent_ : colorBorder_);
    const int pad = scale(8);
    Rect textRect{peerDropRect_.x + pad, peerDropRect_.y,
                  peerDropRect_.w - pad * 2 - scale(18), peerDropRect_.h};
    drawTextInRect(draw, fontBody_, hasPeers ? colorText_ : colorMuted_,
                   textRect, peerSelectorLabel(), false);

    const int cx = peerDropRect_.right() - scale(12);
    const int cy = peerDropRect_.y + peerDropRect_.h / 2;
    const int arrow = scale(4);
    XSetForeground(display_, gc_,
                   (hasPeers ? colorMuted_ : colorDisabled_).pixel);
    XSetLineAttributes(display_, gc_, std::max(1, scale(1)), LineSolid,
                       CapRound, JoinRound);
    XDrawLine(display_, backBuffer_, gc_, cx - arrow, cy - arrow / 2, cx,
              cy + arrow / 2);
    XDrawLine(display_, backBuffer_, gc_, cx, cy + arrow / 2, cx + arrow,
              cy - arrow / 2);
    XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
}

void LinuxUi::drawPeerMenu(XftDraw* draw) {
    peerMenuRects_.clear();
    peerMenuVisibleRows_ = 0;
    if (!peerMenuOpen_ || peerMenuItems_.empty()) {
        return;
    }
    const int rowHeight = scale(26);
    const int pad = scale(4);
    const std::size_t maxRows = 8;
    peerMenuVisibleRows_ = static_cast<int>(
        std::min<std::size_t>(peerMenuItems_.size(), maxRows));

    int menuWidth = std::max(peerDropRect_.w, scale(240));
    menuWidth = std::min(menuWidth, windowWidth_ - scale(8));
    int menuX = peerDropRect_.x;
    if (menuX + menuWidth > windowWidth_ - scale(8)) {
        menuX = windowWidth_ - scale(8) - menuWidth;
    }
    const int menuHeight = peerMenuVisibleRows_ * rowHeight + pad * 2;
    int menuY = peerDropRect_.bottom() + scale(2);
    if (menuY + menuHeight > windowHeight_ - scale(6)) {
        menuY = std::max(scale(6),
                         peerDropRect_.y - menuHeight - scale(2));
    }
    peerMenuRect_ = {menuX, menuY, menuWidth, menuHeight};
    fillRounded({menuX + 1, menuY + 2, menuWidth, menuHeight}, scale(8),
                colorShadow_);
    fillRounded(peerMenuRect_, scale(8), colorCard_);
    strokeRounded(peerMenuRect_, scale(8), colorBorder_);

    const int maxScroll = peerMenuMaxScroll();
    peerMenuScroll_ = std::clamp(peerMenuScroll_, 0, maxScroll);
    for (int row = 0; row < peerMenuVisibleRows_; ++row) {
        const int index = peerMenuScroll_ + row;
        if (index >= static_cast<int>(peerMenuItems_.size())) {
            break;
        }
        const auto& peer = peerMenuItems_[static_cast<std::size_t>(index)];
        Rect rowRect{menuX + pad, menuY + pad + row * rowHeight,
                     menuWidth - pad * 2, rowHeight};
        if (peerMenuHover_ == index) {
            fillRounded(rowRect, scale(5), colorAccentTint_);
        }
        std::string label = peer.name.empty() ? "未知设备" : peer.name;
        std::string address = peer.ip;
        if (peer.port != landrop::kDefaultTcpPort) {
            address += ":" + std::to_string(peer.port);
        }
        label += "  ·  " + address;
        Rect labelRect{rowRect.x + scale(8), rowRect.y,
                       rowRect.w - scale(16), rowRect.h};
        drawTextInRect(draw, fontBody_, colorText_, labelRect, label, false);
        peerMenuRects_.push_back({rowRect, index});
    }
    if (maxScroll > 0) {
        const int trackW = scale(4);
        Rect track{peerMenuRect_.right() - scale(8),
                   peerMenuRect_.y + scale(6), trackW,
                   peerMenuRect_.h - scale(12)};
        fillRounded(track, trackW / 2, colorBorder_);
        const int thumbH = std::max(scale(20), track.h / 3);
        const int travel = std::max(1, track.h - thumbH);
        const int thumbY = track.y + travel * peerMenuScroll_ / maxScroll;
        fillRounded({track.x, thumbY, trackW, thumbH}, trackW / 2,
                    colorMuted_);
    }
}

void LinuxUi::drawDropZone(XftDraw* draw) {
    const bool hasSelection = !selectedPaths_.empty();
    const bool active = dragActive_ || hasSelection;
    fillRounded(dropZone_, scale(9),
                active ? colorAccentTint_ : colorField_);
    if (dragActive_) {
        strokeRounded(dropZone_, scale(9), colorAccent_);
    } else {
        strokeRounded(dropZone_, scale(9),
                      hasSelection ? colorAccentBorder_ : colorBorder_, true);
    }
    Rect titleRect = dropZone_;
    titleRect.y += scale(7);
    titleRect.h = dropZone_.h * 52 / 100;
    std::string title;
    if (dragActive_ && !dropAccepted_) {
        title = "不支持拖入的内容";
    } else if (dragActive_) {
        title = "松开以添加文件";
    } else if (hasSelection) {
        title = "已选择 " + std::to_string(selectedPaths_.size()) + " 项";
    } else {
        title = "将文件或文件夹拖到这里";
    }
    const UiColor& titleColor =
        (dragActive_ && !dropAccepted_)
            ? colorDanger_
            : (active ? colorAccent_ : colorText_);
    drawTextInRect(draw, fontBold_, titleColor, titleRect, title, true);
    Rect subtitleRect = dropZone_;
    subtitleRect.y = titleRect.bottom() - scale(2);
    subtitleRect.h = dropZone_.bottom() - subtitleRect.y - scale(4);
    std::string subtitle;
    if (dragActive_ && !dropAccepted_) {
        subtitle = "请拖入文件或文件夹";
    } else if (hasSelection) {
        for (std::size_t i = 0;
             i < selectedPaths_.size() && i < 3; ++i) {
            if (i != 0) subtitle += "  ·  ";
            subtitle += selectedPaths_[i].filename().string();
        }
        if (selectedPaths_.size() > 3) subtitle += "  ·  …";
        subtitle += "   （点击可继续添加）";
    } else {
        subtitle = "或点击此处选择文件  ·  支持大文件、文件夹与断点续传";
    }
    drawTextInRect(draw, fontSmall_,
                   hasSelection ? colorMuted_ : colorMuted_, subtitleRect,
                   subtitle, true);
}

void LinuxUi::drawCheckbox(XftDraw* draw) {
    const bool allowed = snapshot_.allowIncoming;
    const int boxSize = scale(18);
    Rect box{receiveCheckRow_.x, receiveCheckRow_.y +
                                     (receiveCheckRow_.h - boxSize) / 2,
             boxSize, boxSize};
    fillRounded(box, scale(4), allowed ? colorAccent_ : colorCard_);
    strokeRounded(box, scale(4),
                  allowed ? colorAccent_ : colorBorder_);
    if (allowed) {
        XSetForeground(display_, gc_, colorWhite_.pixel);
        XSetLineAttributes(display_, gc_, std::max(2, scale(2)), LineSolid,
                           CapRound, JoinRound);
        XDrawLine(display_, backBuffer_, gc_, box.x + scale(4),
                  box.y + box.h / 2, box.x + box.w / 2,
                  box.bottom() - scale(5));
        XDrawLine(display_, backBuffer_, gc_, box.x + box.w / 2,
                  box.bottom() - scale(5), box.right() - scale(4),
                  box.y + scale(5));
    }
    Rect label{box.right() + scale(9), receiveCheckRow_.y,
               receiveCheckRow_.w - box.w - scale(9), receiveCheckRow_.h};
    drawTextInRect(draw, fontBody_, colorText_, label,
                   "允许其他设备发送文件", false);
}

void LinuxUi::drawSendCard(XftDraw* draw) {
    drawCardBackground(sendCard_);
    Rect title{sendCard_.x + scale(16), sendCard_.y + scale(10),
               sendCard_.w - scale(32), scale(26)};
    drawTextInRect(draw, fontSection_, colorText_, title, "发送文件", false);

    drawTextInRect(draw, fontSmall_, colorMuted_, targetLabel_, "目标 IP",
                   false);
    drawField(draw, targetField_, targetText_,
              focus_ == FieldFocus::Target, "192.168.1.5 或 IP:端口",
              FieldFocus::Target);
    drawPeerSelector(draw);

    const bool hasSelection = !selectedPaths_.empty();
    drawButton(draw, pickFilesButton_, "选择文件", false, true,
               HitKind::PickFiles);
    drawButton(draw, pickFolderButton_, "选择文件夹", false, true,
               HitKind::PickFolder);
    drawButton(draw, clearButton_, "清空", false, hasSelection,
               HitKind::ClearSelection);
    drawButton(draw, sendButton_, "发送", true, hasSelection,
               HitKind::Send);
    drawDropZone(draw);
}

void LinuxUi::drawReceiveCard(XftDraw* draw) {
    drawCardBackground(receiveCard_);
    Rect title{receiveCard_.x + scale(16), receiveCard_.y + scale(10),
               receiveCard_.w - scale(32), scale(26)};
    drawTextInRect(draw, fontSection_, colorText_, title, "接收文件", false);
    drawCheckbox(draw);
    drawTextInRect(draw, fontSmall_, colorMuted_, receiveSaveLabel_,
                   "接收目录", false);
    drawField(draw, receiveField_, receiveText_,
              focus_ == FieldFocus::Receive, "输入保存路径后按 Enter",
              FieldFocus::Receive);
    drawButton(draw, changeReceiveButton_, "更改…", false, true,
               HitKind::ChangeReceive);
    drawButton(draw, openReceiveButton_, "打开", false, true,
               HitKind::OpenReceive);
    drawTextInRect(draw, fontSmall_, colorMuted_, receiveHint_,
                   "断点续传与 SHA-256 校验默认开启", false);
}

std::string LinuxUi::transferStateText(
    landrop::TransferState state) const {
    switch (state) {
        case landrop::TransferState::Preparing:
            return "准备中";
        case landrop::TransferState::Connecting:
            return "连接中";
        case landrop::TransferState::Transferring:
            return "传输中";
        case landrop::TransferState::Verifying:
            return "校验中";
        case landrop::TransferState::Completed:
            return "已完成";
        case landrop::TransferState::Failed:
            return "失败";
        case landrop::TransferState::Canceled:
            return "已取消";
    }
    return {};
}

std::vector<std::string> LinuxUi::cellTexts(
    const landrop::TransferView& job) const {
    std::vector<std::string> cells(6);
    cells[0] = job.outgoing ? "↑ 发送" : "↓ 接收";
    cells[1] = job.peer;
    if (!job.currentFile.empty()) {
        cells[2] = job.currentFile;
    } else if (job.fileCount > 0) {
        cells[2] = std::to_string(job.fileCount) + " 个文件";
    } else {
        cells[2] = std::to_string(job.itemCount) + " 个项目";
    }
    if (job.totalBytes > 0) {
        const uint64_t percent =
            std::min<uint64_t>(100, job.bytesDone * 100 / job.totalBytes);
        cells[3] = std::to_string(percent) + "%  ·  " +
                   landrop::formatBytes(job.bytesDone) + " / " +
                   landrop::formatBytes(job.totalBytes);
    } else if (landrop::isActiveState(job.state)) {
        cells[3] = "准备中";
    } else {
        cells[3] = "—";
    }
    cells[4] = landrop::isActiveState(job.state)
                   ? landrop::formatRate(job.bytesPerSecond)
                   : "—";
    if (job.state == landrop::TransferState::Failed && !job.error.empty()) {
        cells[5] = "失败：" + job.error;
    } else {
        cells[5] = job.status.empty() ? transferStateText(job.state)
                                      : job.status;
    }
    return cells;
}

uint64_t LinuxUi::selectedJob() const {
    if (selectedJobId_ == 0) {
        return 0;
    }
    return selectedJobId_;
}

bool LinuxUi::selectedJobCancelable() const {
    const uint64_t id = selectedJob();
    if (id == 0) return false;
    for (const auto& job : snapshot_.transfers) {
        if (job.id == id && job.outgoing &&
            landrop::isActiveState(job.state)) {
            return true;
        }
    }
    return false;
}

void LinuxUi::drawTable(XftDraw* draw) {
    updateTableColumns();
    fillRect(tableHeader_, colorField_);
    drawLine(tableHeader_.x, tableHeader_.y, tableHeader_.x,
             tableHeader_.bottom(), colorBorder_);
    drawLine(tableHeader_.x, tableHeader_.bottom() - 1, tableHeader_.right(),
             tableHeader_.bottom() - 1, colorBorder_);

    const std::string headers[6] = {"方向", "设备", "当前项目",
                                    "进度", "速度", "状态"};
    for (int i = 0; i < 6; ++i) {
        Rect cell{tableColumnEdges_[i], tableHeader_.y,
                  std::max(0, tableColumnWidths_[i] - scale(6)),
                  tableHeader_.h};
        drawTextInRect(draw, fontSmall_, colorMuted_, cell, headers[i], false);
    }
    for (int i = 1; i < 6; ++i) {
        const bool active =
            (hover_.kind == HitKind::ColumnDivider && hover_.index == i - 1) ||
            columnResizeIndex_ == i - 1;
        drawLine(tableColumnEdges_[i], tableHeader_.y + scale(5),
                 tableColumnEdges_[i], tableHeader_.bottom() - scale(5),
                 active ? colorAccent_ : colorBorder_);
    }

    if (snapshot_.transfers.empty()) {
        Rect empty = tableBody_;
        empty.y += scale(16);
        drawTextInRect(draw, fontBody_, colorMuted_, empty,
                       "暂无传输任务 · 拖入文件即可开始", true);
        return;
    }
    clampScroll();
    const int count = std::min<int>(
        visibleRows_, static_cast<int>(snapshot_.transfers.size()) -
                          scrollOffset_);
    for (int row = 0; row < count; ++row) {
        const int index = scrollOffset_ + row;
        const auto& job = snapshot_.transfers[static_cast<std::size_t>(index)];
        Rect rowRect{tableBody_.x, tableBody_.y + row * rowHeight_,
                     tableBody_.w, rowHeight_};
        if (job.id == selectedJobId_) {
            fillRect(rowRect, colorAccentTint_);
        }
        drawLine(rowRect.x, rowRect.bottom() - 1, rowRect.right(),
                 rowRect.bottom() - 1, colorBorder_);
        const auto cells = cellTexts(job);
        UiColor textColor = colorText_;
        if (job.state == landrop::TransferState::Failed) {
            textColor = colorDanger_;
        } else if (job.state == landrop::TransferState::Completed) {
            textColor = colorSuccess_;
        } else if (job.state == landrop::TransferState::Canceled) {
            textColor = colorMuted_;
        }
        for (int column = 0; column < 6; ++column) {
            Rect cell{tableColumnEdges_[column], rowRect.y,
                      std::max(0, tableColumnWidths_[column] - scale(6)),
                      rowRect.h};
            drawTextInRect(draw, fontBody_, textColor, cell, cells[column],
                           false);
        }
    }
    if (static_cast<int>(snapshot_.transfers.size()) > visibleRows_) {
        const int maxOffset =
            static_cast<int>(snapshot_.transfers.size()) - visibleRows_;
        const int trackX = tableBody_.right() - scale(3);
        Rect track{trackX, tableBody_.y, scale(4), tableBody_.h};
        fillRounded(track, scale(2), colorBorder_);
        if (maxOffset > 0) {
            const int thumbH =
                std::max(scale(24), tableBody_.h * visibleRows_ /
                                        static_cast<int>(snapshot_.transfers.size()));
            const int thumbY =
                tableBody_.y +
                (tableBody_.h - thumbH) * scrollOffset_ / maxOffset;
            fillRounded({trackX, thumbY, scale(4), thumbH}, scale(2),
                        colorMuted_);
        }
    }
}

std::string LinuxUi::buildProgressText() const {
    const landrop::TransferView* active = nullptr;
    for (const auto& job : snapshot_.transfers) {
        if (landrop::isActiveState(job.state)) {
            active = &job;
            break;
        }
    }
    if (active != nullptr) {
        std::string text = active->outgoing ? "发送到 " : "接收自 ";
        text += active->peer;
        if (!active->currentFile.empty()) {
            text += "  ·  " + active->currentFile;
        }
        text += "  ·  ";
        text += active->status.empty() ? transferStateText(active->state)
                                       : active->status;
        int others = 0;
        for (const auto& job : snapshot_.transfers) {
            if (&job != active && landrop::isActiveState(job.state)) {
                ++others;
            }
        }
        if (others > 0) {
            text += "（另有 " + std::to_string(others) + " 个任务）";
        }
        return text;
    }
    if (!toast_.empty()) {
        return toast_;
    }
    if (!snapshot_.transfers.empty()) {
        const auto& latest = snapshot_.transfers.front();
        if (latest.state == landrop::TransferState::Completed) {
            return "上次传输完成  ·  " + latest.peer;
        }
        if (latest.state == landrop::TransferState::Failed) {
            return "上次传输失败  ·  " +
                   (latest.error.empty() ? latest.status : latest.error);
        }
        if (latest.state == landrop::TransferState::Canceled) {
            return "上次传输已取消";
        }
    }
    if (!snapshot_.serviceMessage.empty() &&
        snapshot_.serviceMessage != "接收服务运行中") {
        return snapshot_.serviceMessage;
    }
    return "就绪  ·  拖入文件或文件夹即可开始发送";
}

void LinuxUi::drawProgress(XftDraw* draw) {
    const std::string text = buildProgressText();
    drawTextInRect(draw, fontSmall_, colorMuted_, progressLabel_, text, false);
    fillRounded(progressBar_, progressBar_.h / 2, colorBorder_);
    const landrop::TransferView* active = nullptr;
    for (const auto& job : snapshot_.transfers) {
        if (landrop::isActiveState(job.state)) {
            active = &job;
            break;
        }
    }
    int ratio = 0;
    if (active != nullptr) {
        if (active->totalBytes > 0) {
            ratio = static_cast<int>(std::min<uint64_t>(
                1000, active->bytesDone * 1000 / active->totalBytes));
        }
    } else if (!snapshot_.transfers.empty() &&
               snapshot_.transfers.front().state ==
                   landrop::TransferState::Completed) {
        ratio = 1000;
    }
    if (ratio > 0) {
        const int fillWidth = std::max(progressBar_.h,
                                       progressBar_.w * ratio / 1000);
        fillRounded({progressBar_.x, progressBar_.y, fillWidth,
                     progressBar_.h},
                    progressBar_.h / 2, colorAccent_);
    }
}

void LinuxUi::drawActivityCard(XftDraw* draw) {
    drawCardBackground(activityCard_);
    Rect title{activityCard_.x + scale(16), activityCard_.y + scale(10),
               activityCard_.w - scale(240), scale(26)};
    drawTextInRect(draw, fontSection_, colorText_, title, "传输活动", false);
    drawButton(draw, cancelButton_, "取消任务", false,
               selectedJobCancelable(), HitKind::CancelJob);
    drawButton(draw, clearDoneButton_, "清除已结束", false, true,
               HitKind::ClearDone);
    drawTable(draw);
    drawProgress(draw);
}

void LinuxUi::drawFooter(XftDraw* draw) {
    drawTextInRect(draw, fontSmall_, colorMuted_, footer_,
                   "SHA-256 完整性校验  ·  自动断点续传  ·  大文件分块传输",
                   true);
}

void LinuxUi::renderToPixmap() {
    if (!mapped_ || windowWidth_ <= 0 || windowHeight_ <= 0) {
        return;
    }
    if (backBuffer_ == 0) {
        backBuffer_ = XCreatePixmap(display_, window_,
                                    static_cast<unsigned int>(windowWidth_),
                                    static_cast<unsigned int>(windowHeight_),
                                    static_cast<unsigned int>(
                                        DefaultDepth(display_, screen_)));
        if (xftDraw_ != nullptr) {
            XftDrawDestroy(xftDraw_);
        }
        xftDraw_ = XftDrawCreate(display_, backBuffer_, visual_, colormap_);
    }
    fillRect({0, 0, windowWidth_, windowHeight_}, colorPage_);
    computeLayout();
    drawHeader(xftDraw_);
    drawSendCard(xftDraw_);
    drawReceiveCard(xftDraw_);
    drawActivityCard(xftDraw_);
    drawFooter(xftDraw_);
    // The discovered-device menu is an overlay: draw it last so it covers the
    // drop zone and activity card.
    drawPeerMenu(xftDraw_);
    XCopyArea(display_, backBuffer_, window_, gc_, 0, 0,
              static_cast<unsigned int>(windowWidth_),
              static_cast<unsigned int>(windowHeight_), 0, 0);
    XFlush(display_);
}

void LinuxUi::render() {
    renderToPixmap();
    dirty_ = false;
}

Hit LinuxUi::hitTest(int x, int y) const {
    if (peerMenuOpen_) {
        for (const auto& item : peerMenuRects_) {
            if (item.rect.contains(x, y)) {
                return {HitKind::PeerMenuItem, item.index};
            }
        }
        if (peerMenuRect_.contains(x, y)) {
            return {};
        }
    }
    if (peerDropRect_.contains(x, y)) return {HitKind::PeerDropdown, -1};
    if (targetField_.contains(x, y)) return {HitKind::TargetField, -1};
    if (receiveField_.contains(x, y)) return {HitKind::ReceiveField, -1};
    if (pickFilesButton_.contains(x, y)) return {HitKind::PickFiles, -1};
    if (pickFolderButton_.contains(x, y)) return {HitKind::PickFolder, -1};
    if (clearButton_.contains(x, y) && !selectedPaths_.empty())
        return {HitKind::ClearSelection, -1};
    if (sendButton_.contains(x, y) && !selectedPaths_.empty())
        return {HitKind::Send, -1};
    if (dropZone_.contains(x, y)) return {HitKind::DropZone, -1};
    if (receiveCheckRow_.contains(x, y))
        return {HitKind::ReceiveCheck, -1};
    if (changeReceiveButton_.contains(x, y))
        return {HitKind::ChangeReceive, -1};
    if (openReceiveButton_.contains(x, y))
        return {HitKind::OpenReceive, -1};
    if (cancelButton_.contains(x, y) && selectedJobCancelable())
        return {HitKind::CancelJob, -1};
    if (clearDoneButton_.contains(x, y))
        return {HitKind::ClearDone, -1};
    if (y >= tableHeader_.y && y < tableHeader_.bottom()) {
        for (int i = 0; i < 5; ++i) {
            if (std::abs(x - tableColumnEdges_[i + 1]) <= scale(3)) {
                return {HitKind::ColumnDivider, i};
            }
        }
    }
    if (tableBody_.contains(x, y) && !snapshot_.transfers.empty()) {
        const int row = (y - tableBody_.y) / std::max(1, rowHeight_);
        const int index = scrollOffset_ + row;
        if (row >= 0 && row < visibleRows_ &&
            index < static_cast<int>(snapshot_.transfers.size())) {
            return {HitKind::TransferRow, index};
        }
    }
    return {};
}

void LinuxUi::updateHover(int x, int y, bool updateCursor) {
    const Hit hit = hitTest(x, y);
    Cursor desired = cursorArrow_;
    switch (hit.kind) {
        case HitKind::TargetField:
        case HitKind::ReceiveField:
            desired = cursorText_;
            break;
        case HitKind::PeerDropdown:
        case HitKind::PeerMenuItem:
        case HitKind::PickFiles:
        case HitKind::PickFolder:
        case HitKind::ClearSelection:
        case HitKind::Send:
        case HitKind::DropZone:
        case HitKind::ReceiveCheck:
        case HitKind::ChangeReceive:
        case HitKind::OpenReceive:
        case HitKind::CancelJob:
        case HitKind::ClearDone:
            desired = cursorHand_;
            break;
        case HitKind::ColumnDivider:
            desired = cursorHResize_;
            break;
        default:
            break;
    }
    if (updateCursor && desired != 0 && desired != currentCursor_) {
        currentCursor_ = desired;
        XDefineCursor(display_, window_, currentCursor_);
    }
    const int peerHover =
        (peerMenuOpen_ && hit.kind == HitKind::PeerMenuItem) ? hit.index : -1;
    if (peerHover != peerMenuHover_) {
        peerMenuHover_ = peerHover;
        dirty_ = true;
    }
    if (hit != hover_) {
        hover_ = hit;
        dirty_ = true;
    }
}

void LinuxUi::setFocus(FieldFocus focus) {
    if (focus_ == FieldFocus::Receive &&
        focus != FieldFocus::Receive) {
        applyReceivePath(true);
    }
    if (focus == focus_) {
        return;
    }
    focus_ = focus;
    if (focus == FieldFocus::Target) {
        targetCursor_ = targetText_.size();
    } else if (focus == FieldFocus::Receive) {
        receiveCursor_ = receiveText_.size();
    }
    dirty_ = true;
}

void LinuxUi::applyReceivePath(bool notify) {
    if (receiveText_.empty()) {
        if (notify) setToast("接收目录不能为空，已恢复原路径");
        receiveText_ = landrop::pathToUtf8(controller_.receiveDirectory());
        receiveCursor_ = receiveText_.size();
        dirty_ = true;
        return;
    }
    if (!landrop::isValidUtf8(receiveText_)) {
        if (notify) setToast("接收目录不是有效的 UTF-8 路径");
        receiveText_ = landrop::pathToUtf8(controller_.receiveDirectory());
        receiveCursor_ = receiveText_.size();
        dirty_ = true;
        return;
    }
    if (receiveText_ == landrop::pathToUtf8(controller_.receiveDirectory())) {
        return;
    }
    std::string error;
    if (!controller_.setReceiveDirectory(landrop::pathFromUtf8(receiveText_),
                                         error)) {
        if (notify) setToast(error);
        receiveText_ = landrop::pathToUtf8(controller_.receiveDirectory());
        receiveCursor_ = receiveText_.size();
    } else {
        receiveText_ = landrop::pathToUtf8(controller_.receiveDirectory());
        receiveCursor_ = receiveText_.size();
        settings_.receiveDirectory = controller_.receiveDirectory();
        saveSettingsNow();
        if (notify) setToast("接收目录已更新");
    }
    dirty_ = true;
}

void LinuxUi::saveSettingsNow() {
    settings_.receiveDirectory = controller_.receiveDirectory();
    std::string error;
    landrop::SettingsStore::save(settings_, error);
}

void LinuxUi::setToast(const std::string& message) {
    if (message.empty()) {
        return;
    }
    toast_ = message;
    toastUntil_ = std::chrono::steady_clock::now() +
                  std::chrono::seconds(7);
    dirty_ = true;
}

void LinuxUi::addPaths(const std::vector<fs::path>& paths) {
    std::set<std::string> existingKeys;
    for (const auto& existing : selectedPaths_) {
        existingKeys.insert(landrop::pathCollisionKey(existing));
    }
    bool changed = false;
    for (const auto& input : paths) {
        if (input.empty()) continue;
        std::error_code ec;
        if (!fs::exists(input, ec)) continue;
        fs::path normalized = fs::absolute(input, ec);
        if (ec) normalized = input;
        normalized = normalized.lexically_normal();
        const std::string key = landrop::pathCollisionKey(normalized);
        if (existingKeys.insert(key).second) {
            selectedPaths_.push_back(normalized);
            changed = true;
        }
    }
    if (changed) {
        dirty_ = true;
    }
}

void LinuxUi::sendCurrentSelection() {
    if (selectedPaths_.empty()) {
        setToast("请先选择要发送的文件或文件夹");
        return;
    }
    if (targetText_.empty()) {
        setToast("请输入目标 IP 地址，或点击发现的设备");
        setFocus(FieldFocus::Target);
        return;
    }
    std::string error;
    const uint64_t id =
        controller_.send(targetText_, selectedPaths_, error);
    if (id == 0) {
        setToast(error);
        return;
    }
    PendingSend pending;
    pending.jobId = id;
    pending.pathKeys.reserve(selectedPaths_.size());
    for (const auto& path : selectedPaths_) {
        pending.pathKeys.push_back(landrop::pathCollisionKey(path));
    }
    pendingSends_.push_back(std::move(pending));
    setToast("已加入发送队列");
}

void LinuxUi::handleAction(Hit hit) {
    if (!hit) {
        return;
    }
    switch (hit.kind) {
        case HitKind::PickFiles:
        case HitKind::DropZone: {
            setFocus(FieldFocus::NoField);
            addPaths(chooseFilesFromDialog());
            break;
        }
        case HitKind::PickFolder: {
            setFocus(FieldFocus::NoField);
            const fs::path folder = chooseFolderFromDialog(
                controller_.receiveDirectory(), "选择要发送的文件夹");
            if (!folder.empty()) addPaths({folder});
            break;
        }
        case HitKind::ClearSelection:
            selectedPaths_.clear();
            dirty_ = true;
            break;
        case HitKind::Send:
            sendCurrentSelection();
            break;
        case HitKind::ReceiveCheck: {
            const bool next = !snapshot_.allowIncoming;
            controller_.setAllowIncoming(next);
            settings_.allowIncoming = next;
            saveSettingsNow();
            setToast(next ? "已允许接收文件" : "已关闭文件接收");
            poll();
            break;
        }
        case HitKind::ChangeReceive: {
            setFocus(FieldFocus::NoField);
            const fs::path folder = chooseFolderFromDialog(
                controller_.receiveDirectory(), "选择接收文件夹");
            if (!folder.empty()) {
                std::string error;
                if (!controller_.setReceiveDirectory(folder, error)) {
                    setToast(error);
                } else {
                    receiveText_ =
                        landrop::pathToUtf8(controller_.receiveDirectory());
                    receiveCursor_ = receiveText_.size();
                    settings_.receiveDirectory =
                        controller_.receiveDirectory();
                    saveSettingsNow();
                    setToast("接收目录已更新");
                }
            }
            break;
        }
        case HitKind::OpenReceive: {
            std::string error;
            if (!landrop::platform::openInFileManager(
                    controller_.receiveDirectory(), error)) {
                setToast(error);
            }
            break;
        }
        case HitKind::CancelJob:
            if (selectedJobId_ != 0) {
                controller_.cancel(selectedJobId_);
                setToast("正在取消任务");
            }
            break;
        case HitKind::ClearDone:
            controller_.clearFinished();
            selectedJobId_ = 0;
            poll();
            break;
        case HitKind::PeerDropdown:
            if (snapshot_.discoveryActive && !snapshot_.peers.empty()) {
                if (peerMenuOpen_) {
                    peerMenuOpen_ = false;
                    peerMenuHover_ = -1;
                } else {
                    peerMenuItems_ = snapshot_.peers;
                    peerMenuScroll_ = 0;
                    peerMenuHover_ = -1;
                    peerMenuOpen_ = true;
                }
                dirty_ = true;
            }
            break;
        case HitKind::PeerMenuItem:
            if (hit.index >= 0 &&
                static_cast<std::size_t>(hit.index) <
                    peerMenuItems_.size()) {
                const auto& peer =
                    peerMenuItems_[static_cast<std::size_t>(hit.index)];
                targetText_ = peer.ip;
                if (peer.port != landrop::kDefaultTcpPort) {
                    targetText_ += ":" + std::to_string(peer.port);
                }
                targetCursor_ = targetText_.size();
                selectedPeerId_ = peer.id;
                peerMenuOpen_ = false;
                peerMenuHover_ = -1;
                setFocus(FieldFocus::Target);
                dirty_ = true;
            }
            break;
        case HitKind::ColumnDivider:
            break;
        case HitKind::TransferRow:
            if (hit.index >= 0 &&
                static_cast<std::size_t>(hit.index) <
                    snapshot_.transfers.size()) {
                selectedJobId_ =
                    snapshot_.transfers[hit.index].id;
                dirty_ = true;
            }
            break;
        default:
            break;
    }
}

void LinuxUi::insertIntoField(FieldFocus field, const std::string& text) {
    if (field != focus_ || field == FieldFocus::NoField) {
        return;
    }
    std::string cleaned = removeUtf8Chrome(text);
    if (cleaned.empty()) return;
    if (field == FieldFocus::Target) {
        if (targetText_.size() + cleaned.size() > 320) {
            cleaned.resize(320 - std::min<std::size_t>(targetText_.size(), 320));
            while (!cleaned.empty() && !landrop::isValidUtf8(cleaned)) {
                cleaned.pop_back();
            }
        }
        targetText_.insert(targetCursor_, cleaned);
        targetCursor_ += cleaned.size();
    } else {
        if (receiveText_.size() + cleaned.size() > 4096) {
            cleaned.resize(4096 - std::min<std::size_t>(receiveText_.size(), 4096));
            while (!cleaned.empty() && !landrop::isValidUtf8(cleaned)) {
                cleaned.pop_back();
            }
        }
        receiveText_.insert(receiveCursor_, cleaned);
        receiveCursor_ += cleaned.size();
    }
    dirty_ = true;
}

void LinuxUi::handleKey(XKeyEvent& event) {
    KeySym symbol = NoSymbol;
    std::string typedText;
    bool hasText = false;
    if (inputContext_ != nullptr) {
        std::vector<char> buffer(64);
        for (;;) {
            Status status = 0;
            const int length = Xutf8LookupString(
                inputContext_, &event, buffer.data(),
                static_cast<int>(buffer.size()), &symbol, &status);
            if (status == XBufferOverflow && length > 0) {
                buffer.resize(static_cast<std::size_t>(length) + 1);
                continue;
            }
            if (length > 0) {
                typedText.assign(buffer.data(),
                                 static_cast<std::size_t>(length));
                hasText = true;
            }
            break;
        }
    } else {
        char fallback[64]{};
        const int length = XLookupString(&event, fallback, sizeof(fallback),
                                         &symbol, nullptr);
        if (length > 0) {
            typedText.assign(fallback, static_cast<std::size_t>(length));
            hasText = true;
        }
    }
    // When an input method consumes the key down to committed text only,
    // Xutf8LookupString reports NoSymbol. Keysym-based shortcuts and
    // navigation keys still need the physical keysym.
    const KeySym keySymbol =
        symbol != NoSymbol ? symbol : XLookupKeysym(&event, 0);
    const bool control = (event.state & ControlMask) != 0;

    if (keySymbol == XK_Tab || keySymbol == XK_ISO_Left_Tab) {
        const bool reverse = (event.state & ShiftMask) != 0;
        if (reverse) {
            if (focus_ == FieldFocus::Target) {
                setFocus(FieldFocus::Receive);
            } else {
                setFocus(FieldFocus::NoField);
            }
        } else {
            if (focus_ == FieldFocus::NoField) {
                setFocus(FieldFocus::Target);
            } else if (focus_ == FieldFocus::Target) {
                setFocus(FieldFocus::Receive);
            } else {
                setFocus(FieldFocus::Target);
            }
        }
        dirty_ = true;
        return;
    }
    if (keySymbol == XK_Escape) {
        if (peerMenuOpen_) {
            peerMenuOpen_ = false;
            peerMenuHover_ = -1;
        }
        setFocus(FieldFocus::NoField);
        dirty_ = true;
        return;
    }
    if (focus_ == FieldFocus::NoField) {
        return;
    }
    if (control && (keySymbol == XK_v || keySymbol == XK_V)) {
        clipboardFocus_ = focus_;
        requestClipboard(event.time);
        return;
    }
    if (keySymbol == XK_Return || keySymbol == XK_KP_Enter) {
        if (focus_ == FieldFocus::Receive) {
            applyReceivePath(true);
        } else {
            sendCurrentSelection();
        }
        return;
    }
    if (keySymbol == XK_BackSpace || keySymbol == XK_Delete) {
        std::string& text = focus_ == FieldFocus::Target ? targetText_
                                                         : receiveText_;
        std::size_t& cursor = focus_ == FieldFocus::Target ? targetCursor_
                                                           : receiveCursor_;
        if (keySymbol == XK_BackSpace && cursor > 0) {
            const std::size_t previous = previousUtf8Boundary(text, cursor);
            text.erase(previous, cursor - previous);
            cursor = previous;
            dirty_ = true;
        } else if (keySymbol == XK_Delete && cursor < text.size()) {
            const std::size_t next = nextUtf8Boundary(text, cursor);
            text.erase(cursor, next - cursor);
            dirty_ = true;
        }
        return;
    }
    if (keySymbol == XK_Left || keySymbol == XK_Right ||
        keySymbol == XK_Home || keySymbol == XK_End) {
        std::string& text = focus_ == FieldFocus::Target ? targetText_
                                                         : receiveText_;
        std::size_t& cursor = focus_ == FieldFocus::Target ? targetCursor_
                                                           : receiveCursor_;
        if (keySymbol == XK_Left) {
            cursor = previousUtf8Boundary(text, cursor);
        } else if (keySymbol == XK_Right) {
            cursor = nextUtf8Boundary(text, cursor);
        } else if (keySymbol == XK_Home) {
            cursor = 0;
        } else {
            cursor = text.size();
        }
        dirty_ = true;
        return;
    }
    if (!control && (event.state & Mod1Mask) == 0 && hasText) {
        std::string typed;
        bool printable = true;
        for (unsigned char c : typedText) {
            if (c < 0x20 || c == 0x7f) {
                printable = false;
                break;
            }
            typed.push_back(static_cast<char>(c));
        }
        if (printable) {
            insertIntoField(focus_, typed);
        }
    }
}

void LinuxUi::handleButtonPress(XButtonEvent& event) {
    if (event.button == Button4 || event.button == Button5) {
        if (peerMenuOpen_ && peerMenuRect_.contains(event.x, event.y)) {
            const int direction = event.button == Button4 ? -1 : 1;
            peerMenuScroll_ = std::clamp(peerMenuScroll_ + direction, 0,
                                         peerMenuMaxScroll());
            dirty_ = true;
            return;
        }
        if (tableBody_.contains(event.x, event.y)) {
            const int direction = event.button == Button4 ? -3 : 3;
            scrollOffset_ = std::max(0, scrollOffset_ + direction);
            clampScroll();
            dirty_ = true;
        }
        return;
    }
    if (event.button != Button1) {
        return;
    }
    const Hit hit = hitTest(event.x, event.y);
    if (peerMenuOpen_ && hit.kind != HitKind::PeerMenuItem &&
        hit.kind != HitKind::PeerDropdown &&
        !peerMenuRect_.contains(event.x, event.y)) {
        peerMenuOpen_ = false;
        peerMenuHover_ = -1;
        dirty_ = true;
    }
    if (hit.kind == HitKind::ColumnDivider) {
        columnResizeIndex_ = hit.index;
        columnResizeStartX_ = event.x;
        columnResizeStartRatios_ = columnRatios_;
        pressed_ = {};
        setFocus(FieldFocus::NoField);
        dirty_ = true;
        return;
    }
    if (hit.kind == HitKind::TargetField) {
        setFocus(FieldFocus::Target);
        targetCursor_ = targetText_.size();
        pressed_ = {};
        dirty_ = true;
        return;
    }
    if (hit.kind == HitKind::ReceiveField) {
        setFocus(FieldFocus::Receive);
        receiveCursor_ = receiveText_.size();
        pressed_ = {};
        dirty_ = true;
        return;
    }
    if (hit.kind == HitKind::TransferRow) {
        setFocus(FieldFocus::NoField);
        handleAction(hit);
        pressed_ = {};
        return;
    }
    if (hit.kind != HitKind::Nothing) {
        setFocus(FieldFocus::NoField);
    }
    pressed_ = hit;
    dirty_ = true;
}

void LinuxUi::handleButtonRelease(XButtonEvent& event) {
    if (event.button != Button1) {
        return;
    }
    if (columnResizeIndex_ >= 0) {
        columnResizeIndex_ = -1;
        updateHover(event.x, event.y, true);
        dirty_ = true;
        return;
    }
    const Hit hit = hitTest(event.x, event.y);
    if (pressed_ == hit && pressed_) {
        handleAction(hit);
    }
    pressed_ = {};
    updateHover(event.x, event.y, true);
    dirty_ = true;
}

void LinuxUi::handleMotion(XMotionEvent& event) {
    if (columnResizeIndex_ >= 0) {
        const int index = columnResizeIndex_;
        const int total = std::max(1, tableTotalWidth_);
        const double pair = columnResizeStartRatios_[index] +
                            columnResizeStartRatios_[index + 1];
        const double minRatio = std::min(
            pair * 0.5, static_cast<double>(scale(40)) / total);
        const double delta =
            static_cast<double>(event.x - columnResizeStartX_) / total;
        const double left = std::clamp(
            columnResizeStartRatios_[index] + delta, minRatio,
            pair - minRatio);
        columnRatios_[index] = left;
        columnRatios_[index + 1] = pair - left;
        updateTableColumns();
        dirty_ = true;
        return;
    }
    updateHover(event.x, event.y, true);
}

void LinuxUi::requestClipboard(Time time) {
    selectionPurpose_ = SelectionPurpose::Clipboard;
    // Use the triggering key event's timestamp. Some clipboard managers
    // reject CurrentTime for ICCCM-compliant selection requests.
    XConvertSelection(display_, atomClipboard_, atomUtf8String_,
                      atomClipboardProperty_, window_,
                      time != 0 ? time : CurrentTime);
    XFlush(display_);
}

bool LinuxUi::validDragSource(Window source) const {
    if (source == 0) {
        return false;
    }
    XWindowAttributes attributes{};
    return XGetWindowAttributes(display_, source, &attributes) != 0;
}

bool LinuxUi::dragSourceOffersUriList(Window source, bool mayHaveMoreTypes,
                                      const long* types) const {
    for (int i = 0; i < 3; ++i) {
        if (types[i] != 0 &&
            static_cast<Atom>(types[i]) == atomTextUriList_) {
            return true;
        }
    }
    if (!mayHaveMoreTypes || source == 0) {
        return false;
    }
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* value = nullptr;
    if (XGetWindowProperty(display_, source, atomXdndTypeList_, 0, 128, False,
                           XA_ATOM, &type, &format, &count, &after,
                           &value) != Success ||
        value == nullptr || format != 32) {
        if (value != nullptr) {
            XFree(value);
        }
        return false;
    }
    bool offers = false;
    const auto* atoms = reinterpret_cast<const long*>(value);
    for (unsigned long i = 0; i < count; ++i) {
        if (static_cast<Atom>(atoms[i]) == atomTextUriList_) {
            offers = true;
            break;
        }
    }
    XFree(value);
    return offers;
}

void LinuxUi::sendXdndStatus(bool accept) {
    if (dragSource_ == 0) return;
    if (!validDragSource(dragSource_)) {
        dragSource_ = 0;
        dragActive_ = false;
        dirty_ = true;
        return;
    }
    XEvent response{};
    response.xclient.type = ClientMessage;
    response.xclient.display = display_;
    response.xclient.window = dragSource_;
    response.xclient.message_type = atomXdndStatus_;
    response.xclient.format = 32;
    response.xclient.data.l[0] = static_cast<long>(window_);
    response.xclient.data.l[1] = accept ? 1 : 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    XTranslateCoordinates(display_, window_,
                          DefaultRootWindow(display_), 0, 0, &rootX, &rootY,
                          &child);
    response.xclient.data.l[2] =
        (static_cast<long>(rootX & 0xffff) << 16) |
        static_cast<long>(rootY & 0xffff);
    response.xclient.data.l[3] =
        (static_cast<long>(windowWidth_ & 0xffff) << 16) |
        static_cast<long>(windowHeight_ & 0xffff);
    response.xclient.data.l[4] =
        accept ? static_cast<long>(atomXdndActionCopy_) : 0;
    XSendEvent(display_, dragSource_, False, NoEventMask, &response);
    XFlush(display_);
}

void LinuxUi::sendXdndFinished(bool accepted) {
    if (dragSource_ == 0) return;
    if (!validDragSource(dragSource_)) {
        dragSource_ = 0;
        dragActive_ = false;
        dirty_ = true;
        return;
    }
    XEvent response{};
    response.xclient.type = ClientMessage;
    response.xclient.display = display_;
    response.xclient.window = dragSource_;
    response.xclient.message_type = atomXdndFinished_;
    response.xclient.format = 32;
    response.xclient.data.l[0] = static_cast<long>(window_);
    response.xclient.data.l[1] = accepted ? 1 : 0;
    response.xclient.data.l[2] =
        accepted ? static_cast<long>(atomXdndActionCopy_) : 0;
    XSendEvent(display_, dragSource_, False, NoEventMask, &response);
    XFlush(display_);
}

void LinuxUi::finishDrop(bool success) {
    sendXdndFinished(success);
    dragSource_ = 0;
    dragActive_ = false;
    selectionPurpose_ = SelectionPurpose::NoPurpose;
    incrementalProperty_ = 0;
    incrementalData_.clear();
    dirty_ = true;
}

bool LinuxUi::readProperty(Atom property, std::string& data,
                           Atom& actualType) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* value = nullptr;
    const int result = XGetWindowProperty(
        display_, window_, property, 0, 4 * 1024 * 1024, False,
        AnyPropertyType, &type, &format, &count, &after, &value);
    if (result != Success || type == None) {
        if (value != nullptr) XFree(value);
        return false;
    }
    actualType = type;
    if (format == 8 && value != nullptr) {
        data.assign(reinterpret_cast<char*>(value), count);
    }
    if (value != nullptr) XFree(value);
    if (type == atomIncr_) {
        XDeleteProperty(display_, window_, property);
        XFlush(display_);
    } else {
        XDeleteProperty(display_, window_, property);
    }
    return true;
}

void LinuxUi::beginIncremental(Atom property, bool forDrop) {
    incrementalProperty_ = property;
    incrementalData_.clear();
    incrementalDrop_ = forDrop;
}

void LinuxUi::finishIncremental() {
    const bool isDrop = incrementalDrop_;
    const std::string data = std::move(incrementalData_);
    incrementalProperty_ = 0;
    incrementalData_.clear();
    if (isDrop) {
        const bool added = processDropData(data);
        finishDrop(added);
    } else {
        insertIntoField(clipboardFocus_, data);
        selectionPurpose_ = SelectionPurpose::NoPurpose;
    }
}

bool LinuxUi::processDropData(const std::string& data) {
    const std::size_t before = selectedPaths_.size();
    std::vector<fs::path> paths;
    std::string line;
    for (std::size_t i = 0; i <= data.size(); ++i) {
        if (i < data.size() && data[i] != '\n') {
            if (data[i] != '\r') line.push_back(data[i]);
            continue;
        }
        const std::string trimmed = landrop::trim(line);
        line.clear();
        if (trimmed.empty() || trimmed.front() == '#') continue;
        fs::path path;
        if (pathFromFileUri(trimmed, path)) {
            paths.push_back(path);
        }
    }
    addPaths(paths);
    const bool added = selectedPaths_.size() > before;
    setToast(added ? "已添加拖入的文件"
                   : "未能识别拖入的文件路径");
    return added;
}

void LinuxUi::handleClientMessage(XClientMessageEvent& event) {
    if (event.message_type == atomXdndEnter_) {
        const Window source = static_cast<Window>(event.data.l[0]);
        if (!validDragSource(source)) {
            dragSource_ = 0;
            dragActive_ = false;
            dirty_ = true;
            return;
        }
        dragSource_ = source;
        dragVersion_ = static_cast<int>((event.data.l[1] >> 24) & 0xff);
        dropAccepted_ = true;
        if (dragVersion_ >= 1) {
            const long types[3] = {event.data.l[2], event.data.l[3],
                                   event.data.l[4]};
            dropAccepted_ = dragSourceOffersUriList(
                source, (event.data.l[1] & 1) != 0, types);
        }
        dragActive_ = true;
        pressed_ = {};
        dirty_ = true;
        return;
    }
    if (event.message_type == atomXdndPosition_) {
        const Window source = static_cast<Window>(event.data.l[0]);
        if (dragSource_ == 0 || source != dragSource_ ||
            !validDragSource(source)) {
            return;
        }
        dragActive_ = true;
        dirty_ = true;
        sendXdndStatus(dropAccepted_);
        return;
    }
    if (event.message_type == atomXdndLeave_) {
        if (static_cast<Window>(event.data.l[0]) != dragSource_) {
            return;
        }
        dragActive_ = false;
        dragSource_ = 0;
        dirty_ = true;
        return;
    }
    if (event.message_type == atomXdndDrop_) {
        const Window source = static_cast<Window>(event.data.l[0]);
        if (dragSource_ == 0 || source != dragSource_ ||
            !validDragSource(source)) {
            return;
        }
        dropTime_ = static_cast<Time>(event.data.l[2]);
        if (!dropAccepted_) {
            finishDrop(false);
            return;
        }
        selectionPurpose_ = SelectionPurpose::Drop;
        XConvertSelection(display_, atomXdndSelection_, atomTextUriList_,
                          atomDropProperty_, window_, dropTime_);
        XFlush(display_);
        return;
    }
}

void LinuxUi::handleSelectionNotify(XSelectionEvent& event) {
    if (selectionPurpose_ == SelectionPurpose::Drop) {
        if (event.property == None ||
            event.property != atomDropProperty_) {
            finishDrop(false);
            return;
        }
        std::string data;
        Atom actualType = None;
        if (!readProperty(atomDropProperty_, data, actualType)) {
            finishDrop(false);
            return;
        }
        if (actualType == atomIncr_) {
            beginIncremental(atomDropProperty_, true);
            return;
        }
        const bool added = processDropData(data);
        finishDrop(added);
        return;
    }
    if (selectionPurpose_ == SelectionPurpose::Clipboard) {
        if (event.property == None) {
            selectionPurpose_ = SelectionPurpose::NoPurpose;
            return;
        }
        if (event.property != atomClipboardProperty_) {
            return;
        }
        std::string data;
        Atom actualType = None;
        if (!readProperty(atomClipboardProperty_, data, actualType)) {
            selectionPurpose_ = SelectionPurpose::NoPurpose;
            return;
        }
        if (actualType == atomIncr_) {
            beginIncremental(atomClipboardProperty_, false);
            return;
        }
        insertIntoField(clipboardFocus_,
                        landrop::trim(removeUtf8Chrome(data)));
        selectionPurpose_ = SelectionPurpose::NoPurpose;
        dirty_ = true;
    }
}

void LinuxUi::handlePropertyNotify(XPropertyEvent& event) {
    if (incrementalProperty_ == 0 || event.atom != incrementalProperty_ ||
        event.state != PropertyNewValue) {
        return;
    }
    std::string data;
    Atom actualType = None;
    if (!readProperty(event.atom, data, actualType)) {
        return;
    }
    if (data.empty()) {
        finishIncremental();
        return;
    }
    if (incrementalData_.size() + data.size() > kMaxDropBytes) {
        if (incrementalDrop_) {
            finishDrop(false);
        } else {
            selectionPurpose_ = SelectionPurpose::NoPurpose;
        }
        incrementalProperty_ = 0;
        incrementalData_.clear();
        return;
    }
    incrementalData_ += data;
}

void LinuxUi::handleEvent(XEvent& event) {
    switch (event.type) {
        case Expose:
            if (event.xexpose.count == 0) {
                dirty_ = true;
            }
            break;
        case ConfigureNotify:
            if (event.xconfigure.width != windowWidth_ ||
                event.xconfigure.height != windowHeight_) {
                windowWidth_ = event.xconfigure.width;
                windowHeight_ = event.xconfigure.height;
                if (backBuffer_ != 0) {
                    if (xftDraw_ != nullptr) {
                        XftDrawDestroy(xftDraw_);
                        xftDraw_ = nullptr;
                    }
                    XFreePixmap(display_, backBuffer_);
                    backBuffer_ = 0;
                }
                computeLayout();
                dirty_ = true;
            }
            break;
        case MapNotify:
            mapped_ = true;
            dirty_ = true;
            // Give the window keyboard focus as soon as it is mapped so the
            // user can type an address without clicking first. Well-behaved
            // window managers may still take their time; WM_TAKE_FOCUS is
            // also advertised in createWindow().
            XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
            if (inputContext_ != nullptr) {
                XSetICFocus(inputContext_);
            }
            break;
        case KeyPress:
            handleKey(event.xkey);
            break;
        case ButtonPress:
            handleButtonPress(event.xbutton);
            break;
        case ButtonRelease:
            handleButtonRelease(event.xbutton);
            break;
        case MotionNotify:
            handleMotion(event.xmotion);
            break;
        case ClientMessage:
            if (event.xclient.message_type == atomWmProtocols_) {
                const Atom protocol =
                    static_cast<Atom>(event.xclient.data.l[0]);
                if (protocol == atomWmDelete_) {
                    running_ = false;
                } else if (protocol == atomWmTakeFocus_) {
                    const Time timestamp =
                        static_cast<Time>(event.xclient.data.l[1]);
                    XSetInputFocus(display_, window_, RevertToParent,
                                   timestamp != 0 ? timestamp : CurrentTime);
                    if (inputContext_ != nullptr) {
                        XSetICFocus(inputContext_);
                    }
                }
            } else {
                handleClientMessage(event.xclient);
            }
            break;
        case SelectionNotify:
            handleSelectionNotify(event.xselection);
            break;
        case PropertyNotify:
            handlePropertyNotify(event.xproperty);
            break;
        case FocusIn:
            if (inputContext_ != nullptr) {
                XSetICFocus(inputContext_);
            }
            dirty_ = true;
            break;
        case FocusOut:
            if (inputContext_ != nullptr) {
                XUnsetICFocus(inputContext_);
            }
            break;
        default:
            break;
    }
}

int LinuxUi::run() {
    poll();
    render();
    auto nextPoll = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kPollIntervalMs);
    while (running_ && g_terminateRequested == 0) {
        while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);
            // Let the input method consume keys it is composing before the
            // application sees them (e.g. fcitx/ibus Chinese input).
            if (XFilterEvent(&event, None)) {
                continue;
            }
            handleEvent(event);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll) {
            poll();
            nextPoll = now + std::chrono::milliseconds(kPollIntervalMs);
        }
        if (!toast_.empty() && now > toastUntil_) {
            toast_.clear();
            dirty_ = true;
        }
        if (dirty_) {
            render();
        }
        XFlush(display_);
        if (!running_) break;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(ConnectionNumber(display_), &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        const int selected = select(ConnectionNumber(display_) + 1, &readSet,
                                    nullptr, nullptr, &timeout);
        if (selected < 0 && errno != EINTR) {
            break;
        }
    }
    applyReceivePath(false);
    saveSettingsNow();
    return 0;
}

std::string LinuxUi::findExecutable(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return {};
    std::string paths(path);
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t colon = paths.find(':', start);
        const std::string directory =
            paths.substr(start, colon == std::string::npos
                                    ? std::string::npos
                                    : colon - start);
        if (!directory.empty()) {
            const std::string candidate = directory + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {};
}

bool LinuxUi::runChooser(const std::string& executable,
                         const std::vector<std::string>& arguments,
                         std::string& output, int& exitCode) {
    output.clear();
    exitCode = -1;
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return false;
    }
    // Prepare every allocation before fork(). The process has other active
    // network/UI threads, and only async-signal-safe calls are allowed in the
    // child between fork() and exec().
    std::vector<std::string> owned;
    owned.reserve(1 + arguments.size());
    owned.push_back(executable);
    for (const auto& argument : arguments) owned.push_back(argument);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& value : owned) argv.push_back(value.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }
    if (pid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    close(pipeFds[1]);
    char buffer[4096];
    for (;;) {
        const ssize_t got = read(pipeFds[0], buffer, sizeof(buffer));
        if (got > 0) {
            if (output.size() < 4 * 1024 * 1024) {
                output.append(buffer, static_cast<std::size_t>(got));
            }
            continue;
        }
        if (got == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    }
    return true;
}

std::vector<fs::path> LinuxUi::chooseFilesFromDialog() {
    std::vector<fs::path> result;
    std::string output;
    int exitCode = -1;
    const std::string zenity = findExecutable("zenity");
    if (!zenity.empty()) {
        runChooser(zenity,
                   {"--file-selection", "--multiple", "--separator=\n",
                    "--title=选择要发送的文件"},
                   output, exitCode);
    } else {
        const std::string kdialog = findExecutable("kdialog");
        if (!kdialog.empty()) {
            runChooser(kdialog,
                       {"--getopenfilename", ".", "*", "--multiple",
                        "--separate-output"},
                       output, exitCode);
        }
    }
    if (exitCode != 0 && output.empty()) {
        if (exitCode == 127 || (zenity.empty() && findExecutable("kdialog").empty())) {
            setToast("未找到 zenity/kdialog，可直接将文件拖入窗口");
        }
        return result;
    }
    std::size_t start = 0;
    while (start <= output.size()) {
        const std::size_t newline = output.find('\n', start);
        std::string line =
            output.substr(start, newline == std::string::npos
                                     ? std::string::npos
                                     : newline - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty() && landrop::isValidUtf8(line)) {
            std::error_code ec;
            const fs::path path(line);
            if (fs::exists(path, ec)) result.push_back(path);
        }
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    if (!result.empty()) {
        setToast("已添加 " + std::to_string(result.size()) + " 个文件");
    }
    return result;
}

fs::path LinuxUi::chooseFolderFromDialog(const fs::path& initial,
                                         const char* title) {
    std::string output;
    int exitCode = -1;
    const std::string zenity = findExecutable("zenity");
    if (!zenity.empty()) {
        std::string startArg = "--filename=";
        if (!initial.empty()) startArg += initial.string() + "/";
        runChooser(zenity,
                   {"--file-selection", "--directory", startArg,
                    std::string("--title=") + title},
                   output, exitCode);
    } else {
        const std::string kdialog = findExecutable("kdialog");
        if (!kdialog.empty()) {
            runChooser(kdialog,
                       {"--getexistingdirectory",
                        initial.empty() ? "." : initial.string(),
                        std::string("--title=") + title},
                       output, exitCode);
        }
    }
    if (exitCode != 0 && output.empty()) {
        if (exitCode == 127 || (zenity.empty() && findExecutable("kdialog").empty())) {
            setToast("未找到目录选择器，请直接编辑接收目录后按 Enter");
        }
        return {};
    }
    std::string pathText = landrop::trim(trimNewlines(output));
    if (!pathText.empty() && landrop::isValidUtf8(pathText)) {
        std::error_code ec;
        fs::path path(pathText);
        if (fs::is_directory(path, ec)) {
            return path;
        }
    }
    return {};
}

}  // namespace

int runLinuxApp(int argc, char** argv) {
    landrop::AppSettings settings = landrop::SettingsStore::load();
    landrop::AppController controller;
    landrop::ControllerOptions options;
    options.receiveDirectory = settings.receiveDirectory;
    options.deviceName = landrop::platform::hostName();
    options.allowIncoming = settings.allowIncoming;
    options.enableDiscovery = true;
    std::string warning;
    if (!controller.start(options, warning)) {
        std::fprintf(stderr, "LanDrop: %s\n", warning.c_str());
        return 1;
    }

    LinuxUi ui(controller, settings, warning, argc, argv);
    std::string error;
    if (!ui.initialize(error)) {
        std::fprintf(stderr, "LanDrop: %s\n", error.c_str());
        controller.stop();
        return 1;
    }
    const int result = ui.run();
    controller.stop();
    return result;
}
