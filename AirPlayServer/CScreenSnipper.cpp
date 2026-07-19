#include "CScreenSnipper.h"

#include <Windows.h>
#include <Windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace
{
    constexpr int kHotkeyId = 0x4152;
    constexpr UINT kStartCaptureMessage = WM_APP + 0x452;
    constexpr int kMinSelection = 4;
    constexpr int kMosaicSize = 14;

    enum class CaptureState
    {
        Selecting,
        Editing
    };

    enum class Tool
    {
        Rectangle,
        Ellipse,
        Triangle,
        Arrow,
        Brush,
        Mosaic
    };

    enum class ButtonAction
    {
        Tool,
        Undo,
        Finish,
        Cancel
    };

    struct Annotation
    {
        Tool tool = Tool::Rectangle;
        POINT start = {};
        POINT end = {};
        std::vector<POINT> points;
        COLORREF color = RGB(255, 72, 72);
        int thickness = 3;
    };

    struct ToolbarButton
    {
        RECT rect = {};
        const wchar_t* text = L"";
        Tool tool = Tool::Rectangle;
        ButtonAction action = ButtonAction::Tool;
    };

    LONG MinLong(LONG a, LONG b)
    {
        return a < b ? a : b;
    }

    LONG MaxLong(LONG a, LONG b)
    {
        return a > b ? a : b;
    }

    LONG ClampLong(LONG value, LONG minimum, LONG maximum)
    {
        if (value < minimum) {
            return minimum;
        }
        if (value > maximum) {
            return maximum;
        }
        return value;
    }

    RECT NormalizeRect(POINT first, POINT second)
    {
        RECT rect = {};
        rect.left = MinLong(first.x, second.x);
        rect.top = MinLong(first.y, second.y);
        rect.right = MaxLong(first.x, second.x);
        rect.bottom = MaxLong(first.y, second.y);
        return rect;
    }

    int RectWidth(const RECT& rect)
    {
        return static_cast<int>(rect.right - rect.left);
    }

    int RectHeight(const RECT& rect)
    {
        return static_cast<int>(rect.bottom - rect.top);
    }

    bool ValidSelection(const RECT& rect)
    {
        return RectWidth(rect) >= kMinSelection &&
               RectHeight(rect) >= kMinSelection;
    }

    bool ContainsPoint(const RECT& rect, POINT point)
    {
        return point.x >= rect.left &&
               point.x < rect.right &&
               point.y >= rect.top &&
               point.y < rect.bottom;
    }

    class ScreenSnipper
    {
    public:
        ScreenSnipper()
            : m_thread(NULL)
            , m_readyEvent(NULL)
            , m_threadId(0)
            , m_hotkeyRegistered(false)
            , m_window(NULL)
            , m_screenDC(NULL)
            , m_screenBitmap(NULL)
            , m_oldScreenBitmap(NULL)
            , m_screenBits(NULL)
            , m_dimDC(NULL)
            , m_dimBitmap(NULL)
            , m_oldDimBitmap(NULL)
            , m_dimBits(NULL)
            , m_stride(0)
            , m_virtualX(0)
            , m_virtualY(0)
            , m_virtualWidth(0)
            , m_virtualHeight(0)
            , m_state(CaptureState::Selecting)
            , m_dragging(false)
            , m_drawing(false)
            , m_activeTool(Tool::Rectangle)
            , m_captureActive(false)
        {
            m_dragStart = {};
            m_dragCurrent = {};
            m_selection = {};
        }

        ~ScreenSnipper()
        {
            Shutdown();
        }

        bool Initialize()
        {
            if (m_thread != NULL) {
                return true;
            }

            m_readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (m_readyEvent == NULL) {
                return false;
            }

            m_thread = CreateThread(
                NULL,
                0,
                ThreadEntry,
                this,
                0,
                &m_threadId);

            if (m_thread == NULL) {
                CloseHandle(m_readyEvent);
                m_readyEvent = NULL;
                m_threadId = 0;
                return false;
            }

            return WaitForSingleObject(m_readyEvent, 5000) ==
                   WAIT_OBJECT_0;
        }

        void Shutdown()
        {
            if (m_thread == NULL) {
                return;
            }

            if (m_threadId != 0) {
                PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
            }

            WaitForSingleObject(m_thread, 3000);
            CloseHandle(m_thread);
            m_thread = NULL;
            m_threadId = 0;

            if (m_readyEvent != NULL) {
                CloseHandle(m_readyEvent);
                m_readyEvent = NULL;
            }
        }

        void StartCapture()
        {
            if (!Initialize() || m_threadId == 0) {
                return;
            }

            PostThreadMessageW(
                m_threadId,
                kStartCaptureMessage,
                0,
                0);
        }

    private:
        static DWORD WINAPI ThreadEntry(LPVOID parameter)
        {
            ScreenSnipper* self =
                static_cast<ScreenSnipper*>(parameter);
            self->ThreadMain();
            return 0;
        }

        void ThreadMain()
        {
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            SetThreadDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

            MSG message = {};
            PeekMessageW(
                &message,
                NULL,
                WM_USER,
                WM_USER,
                PM_NOREMOVE);

            m_hotkeyRegistered =
                RegisterHotKey(
                    NULL,
                    kHotkeyId,
                    MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                    'C') != FALSE;

            if (m_readyEvent != NULL) {
                SetEvent(m_readyEvent);
            }

            while (GetMessageW(&message, NULL, 0, 0) > 0) {
                if ((message.message == WM_HOTKEY &&
                     message.wParam == kHotkeyId) ||
                    message.message == kStartCaptureMessage) {
                    RunCapture();
                    continue;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (m_hotkeyRegistered) {
                UnregisterHotKey(NULL, kHotkeyId);
                m_hotkeyRegistered = false;
            }

            ReleaseDesktop();
        }

        bool CreateTopDownDib(
            HDC referenceDC,
            int width,
            int height,
            HDC& memoryDC,
            HBITMAP& bitmap,
            HGDIOBJ& oldBitmap,
            BYTE*& bits)
        {
            memoryDC = CreateCompatibleDC(referenceDC);
            if (memoryDC == NULL) {
                return false;
            }

            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            bitmap = CreateDIBSection(
                referenceDC,
                &info,
                DIB_RGB_COLORS,
                reinterpret_cast<void**>(&bits),
                NULL,
                0);

            if (bitmap == NULL || bits == NULL) {
                if (bitmap != NULL) {
                    DeleteObject(bitmap);
                    bitmap = NULL;
                }
                DeleteDC(memoryDC);
                memoryDC = NULL;
                bits = NULL;
                return false;
            }

            oldBitmap = SelectObject(memoryDC, bitmap);
            return true;
        }

        bool CaptureDesktop()
        {
            ReleaseDesktop();

            m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            m_virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            m_virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            if (m_virtualWidth <= 0 || m_virtualHeight <= 0) {
                return false;
            }

            HDC desktopDC = GetDC(NULL);
            if (desktopDC == NULL) {
                return false;
            }

            bool ok = CreateTopDownDib(
                desktopDC,
                m_virtualWidth,
                m_virtualHeight,
                m_screenDC,
                m_screenBitmap,
                m_oldScreenBitmap,
                m_screenBits);

            if (ok) {
                ok = CreateTopDownDib(
                    desktopDC,
                    m_virtualWidth,
                    m_virtualHeight,
                    m_dimDC,
                    m_dimBitmap,
                    m_oldDimBitmap,
                    m_dimBits);
            }

            if (ok) {
                ok = BitBlt(
                    m_screenDC,
                    0,
                    0,
                    m_virtualWidth,
                    m_virtualHeight,
                    desktopDC,
                    m_virtualX,
                    m_virtualY,
                    SRCCOPY | CAPTUREBLT) != FALSE;
            }

            ReleaseDC(NULL, desktopDC);

            if (!ok) {
                ReleaseDesktop();
                return false;
            }

            m_stride = m_virtualWidth * 4;
            const SIZE_T byteCount =
                static_cast<SIZE_T>(m_stride) *
                static_cast<SIZE_T>(m_virtualHeight);

            std::memcpy(m_dimBits, m_screenBits, byteCount);

            // Precompute a gently darkened frozen desktop image.
            // Paint operations later only use BitBlt, which avoids flashing.
            for (SIZE_T offset = 0;
                 offset + 3 < byteCount;
                 offset += 4) {
                m_dimBits[offset + 0] =
                    static_cast<BYTE>(
                        static_cast<unsigned int>(
                            m_dimBits[offset + 0]) * 62U / 100U);
                m_dimBits[offset + 1] =
                    static_cast<BYTE>(
                        static_cast<unsigned int>(
                            m_dimBits[offset + 1]) * 62U / 100U);
                m_dimBits[offset + 2] =
                    static_cast<BYTE>(
                        static_cast<unsigned int>(
                            m_dimBits[offset + 2]) * 62U / 100U);
                m_dimBits[offset + 3] = 255;
            }

            return true;
        }

        void ReleaseDesktop()
        {
            if (m_screenDC != NULL &&
                m_oldScreenBitmap != NULL) {
                SelectObject(m_screenDC, m_oldScreenBitmap);
            }
            if (m_dimDC != NULL &&
                m_oldDimBitmap != NULL) {
                SelectObject(m_dimDC, m_oldDimBitmap);
            }

            m_oldScreenBitmap = NULL;
            m_oldDimBitmap = NULL;

            if (m_screenBitmap != NULL) {
                DeleteObject(m_screenBitmap);
                m_screenBitmap = NULL;
            }
            if (m_dimBitmap != NULL) {
                DeleteObject(m_dimBitmap);
                m_dimBitmap = NULL;
            }
            if (m_screenDC != NULL) {
                DeleteDC(m_screenDC);
                m_screenDC = NULL;
            }
            if (m_dimDC != NULL) {
                DeleteDC(m_dimDC);
                m_dimDC = NULL;
            }

            m_screenBits = NULL;
            m_dimBits = NULL;
            m_stride = 0;
        }

        void ResetState()
        {
            m_state = CaptureState::Selecting;
            m_dragging = false;
            m_drawing = false;
            m_dragStart = {};
            m_dragCurrent = {};
            m_selection = {};
            m_activeTool = Tool::Rectangle;
            m_annotations.clear();
            m_current = Annotation();
        }

        void RunCapture()
        {
            bool expected = false;
            if (!m_captureActive.compare_exchange_strong(
                    expected,
                    true)) {
                return;
            }

            if (!CaptureDesktop()) {
                m_captureActive = false;
                return;
            }

            ResetState();

            static const wchar_t* className =
                L"AirPlayServerChineseCaptureOverlay";

            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.style =
                CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = GetModuleHandleW(NULL);
            windowClass.hCursor = LoadCursorW(NULL, IDC_CROSS);
            windowClass.hbrBackground = NULL;
            windowClass.lpszClassName = className;

            if (RegisterClassExW(&windowClass) == 0 &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                ReleaseDesktop();
                m_captureActive = false;
                return;
            }

            m_window = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                className,
                L"区域截图",
                WS_POPUP,
                m_virtualX,
                m_virtualY,
                m_virtualWidth,
                m_virtualHeight,
                NULL,
                NULL,
                GetModuleHandleW(NULL),
                this);

            if (m_window == NULL) {
                ReleaseDesktop();
                m_captureActive = false;
                return;
            }

            ShowWindow(m_window, SW_SHOW);
            SetForegroundWindow(m_window);
            SetFocus(m_window);
            RedrawWindow(
                m_window,
                NULL,
                NULL,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);

            MSG message = {};
            while (m_window != NULL && IsWindow(m_window)) {
                const BOOL result =
                    GetMessageW(&message, NULL, 0, 0);

                if (result <= 0) {
                    if (result == 0) {
                        PostQuitMessage(
                            static_cast<int>(message.wParam));
                    }
                    break;
                }

                if (message.message == WM_HOTKEY ||
                    message.message == kStartCaptureMessage) {
                    continue;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            ReleaseDesktop();
            m_captureActive = false;
        }

        HFONT CreateUiFont(int height, int weight)
        {
            return CreateFontW(
                -height,
                0,
                0,
                0,
                weight,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Microsoft YaHei UI");
        }

        void Repaint()
        {
            if (m_window != NULL) {
                RedrawWindow(
                    m_window,
                    NULL,
                    NULL,
                    RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
        }

        void DrawArrow(
            HDC dc,
            POINT start,
            POINT end,
            COLORREF color,
            int thickness)
        {
            HPEN pen = CreatePen(PS_SOLID, thickness, color);
            HGDIOBJ oldPen = SelectObject(dc, pen);

            MoveToEx(dc, start.x, start.y, NULL);
            LineTo(dc, end.x, end.y);

            const double dx =
                static_cast<double>(end.x - start.x);
            const double dy =
                static_cast<double>(end.y - start.y);
            const double length = std::sqrt(dx * dx + dy * dy);

            if (length > 1.0) {
                const double angle = std::atan2(dy, dx);
                double head = length * 0.18;
                if (head < 12.0) {
                    head = 12.0;
                }
                if (head > 24.0) {
                    head = 24.0;
                }

                const double spread = 0.55;
                POINT left = {
                    static_cast<LONG>(
                        end.x - head * std::cos(angle - spread)),
                    static_cast<LONG>(
                        end.y - head * std::sin(angle - spread))
                };
                POINT right = {
                    static_cast<LONG>(
                        end.x - head * std::cos(angle + spread)),
                    static_cast<LONG>(
                        end.y - head * std::sin(angle + spread))
                };

                MoveToEx(dc, end.x, end.y, NULL);
                LineTo(dc, left.x, left.y);
                MoveToEx(dc, end.x, end.y, NULL);
                LineTo(dc, right.x, right.y);
            }

            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        void DrawMosaicPoint(
            HDC dc,
            POINT source,
            int offsetX,
            int offsetY)
        {
            const LONG x =
                ClampLong(
                    source.x,
                    0,
                    static_cast<LONG>(m_virtualWidth - 1));
            const LONG y =
                ClampLong(
                    source.y,
                    0,
                    static_cast<LONG>(m_virtualHeight - 1));

            COLORREF color = GetPixel(m_screenDC, x, y);
            if (color == CLR_INVALID) {
                color = RGB(128, 128, 128);
            }

            const int half = kMosaicSize / 2;
            RECT block = {
                source.x - half + offsetX,
                source.y - half + offsetY,
                source.x - half + kMosaicSize + offsetX,
                source.y - half + kMosaicSize + offsetY
            };

            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &block, brush);
            DeleteObject(brush);
        }

        void DrawAnnotation(
            HDC dc,
            const Annotation& annotation,
            int offsetX,
            int offsetY)
        {
            POINT start = {
                annotation.start.x + offsetX,
                annotation.start.y + offsetY
            };
            POINT end = {
                annotation.end.x + offsetX,
                annotation.end.y + offsetY
            };

            if (annotation.tool == Tool::Arrow) {
                DrawArrow(
                    dc,
                    start,
                    end,
                    annotation.color,
                    annotation.thickness);
                return;
            }

            if (annotation.tool == Tool::Mosaic) {
                for (const POINT& point : annotation.points) {
                    DrawMosaicPoint(dc, point, offsetX, offsetY);
                }
                return;
            }

            HPEN pen = CreatePen(
                PS_SOLID,
                annotation.thickness,
                annotation.color);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush =
                SelectObject(dc, GetStockObject(NULL_BRUSH));

            switch (annotation.tool) {
            case Tool::Rectangle:
            {
                const RECT rect = NormalizeRect(start, end);
                Rectangle(
                    dc,
                    rect.left,
                    rect.top,
                    rect.right,
                    rect.bottom);
                break;
            }

            case Tool::Ellipse:
            {
                const RECT rect = NormalizeRect(start, end);
                Ellipse(
                    dc,
                    rect.left,
                    rect.top,
                    rect.right,
                    rect.bottom);
                break;
            }

            case Tool::Triangle:
            {
                const RECT rect = NormalizeRect(start, end);
                POINT triangle[4] = {
                    {
                        rect.left +
                            (rect.right - rect.left) / 2,
                        rect.top
                    },
                    { rect.left, rect.bottom },
                    { rect.right, rect.bottom },
                    {
                        rect.left +
                            (rect.right - rect.left) / 2,
                        rect.top
                    }
                };
                Polyline(dc, triangle, 4);
                break;
            }

            case Tool::Brush:
                if (annotation.points.size() >= 2) {
                    std::vector<POINT> points;
                    points.reserve(annotation.points.size());
                    for (const POINT& point : annotation.points) {
                        points.push_back({
                            point.x + offsetX,
                            point.y + offsetY
                        });
                    }
                    Polyline(
                        dc,
                        points.data(),
                        static_cast<int>(points.size()));
                }
                break;

            default:
                break;
            }

            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        std::vector<ToolbarButton> ToolbarButtons() const
        {
            constexpr int buttonWidth = 58;
            constexpr int buttonHeight = 34;
            constexpr int gap = 4;
            constexpr int count = 9;
            constexpr int totalWidth =
                count * buttonWidth + (count - 1) * gap;

            LONG maximumX =
                static_cast<LONG>(m_virtualWidth - totalWidth - 8);
            if (maximumX < 8) {
                maximumX = 8;
            }

            LONG x = ClampLong(m_selection.left, 8, maximumX);
            LONG y = m_selection.bottom + 10;

            if (y + buttonHeight + 8 > m_virtualHeight) {
                y = m_selection.top - buttonHeight - 10;
            }

            LONG maximumY =
                static_cast<LONG>(m_virtualHeight - buttonHeight - 8);
            if (maximumY < 8) {
                maximumY = 8;
            }
            y = ClampLong(y, 8, maximumY);

            struct Definition
            {
                const wchar_t* text;
                Tool tool;
                ButtonAction action;
            };

            const Definition definitions[count] = {
                { L"矩形", Tool::Rectangle, ButtonAction::Tool },
                { L"圆形", Tool::Ellipse, ButtonAction::Tool },
                { L"三角", Tool::Triangle, ButtonAction::Tool },
                { L"箭头", Tool::Arrow, ButtonAction::Tool },
                { L"画笔", Tool::Brush, ButtonAction::Tool },
                { L"马赛克", Tool::Mosaic, ButtonAction::Tool },
                { L"撤销", Tool::Rectangle, ButtonAction::Undo },
                { L"完成", Tool::Rectangle, ButtonAction::Finish },
                { L"取消", Tool::Rectangle, ButtonAction::Cancel }
            };

            std::vector<ToolbarButton> buttons;
            buttons.reserve(count);

            for (int index = 0; index < count; ++index) {
                ToolbarButton button;
                button.text = definitions[index].text;
                button.tool = definitions[index].tool;
                button.action = definitions[index].action;
                button.rect = {
                    x + index * (buttonWidth + gap),
                    y,
                    x + index * (buttonWidth + gap) + buttonWidth,
                    y + buttonHeight
                };
                buttons.push_back(button);
            }

            return buttons;
        }

        void DrawToolbar(HDC dc)
        {
            const std::vector<ToolbarButton> buttons =
                ToolbarButtons();

            HFONT font = CreateUiFont(16, FW_SEMIBOLD);
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);

            for (const ToolbarButton& button : buttons) {
                const bool selected =
                    button.action == ButtonAction::Tool &&
                    button.tool == m_activeTool;

                HBRUSH background = CreateSolidBrush(
                    selected
                        ? RGB(52, 129, 246)
                        : RGB(247, 247, 247));
                HPEN border = CreatePen(
                    PS_SOLID,
                    1,
                    selected
                        ? RGB(52, 129, 246)
                        : RGB(180, 180, 180));

                HGDIOBJ oldBrush = SelectObject(dc, background);
                HGDIOBJ oldPen = SelectObject(dc, border);

                RoundRect(
                    dc,
                    button.rect.left,
                    button.rect.top,
                    button.rect.right,
                    button.rect.bottom,
                    8,
                    8);

                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(background);
                DeleteObject(border);

                SetTextColor(
                    dc,
                    selected
                        ? RGB(255, 255, 255)
                        : RGB(35, 35, 35));

                RECT textRect = button.rect;
                DrawTextW(
                    dc,
                    button.text,
                    -1,
                    &textRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SelectObject(dc, oldFont);
            DeleteObject(font);
        }

        void DrawSelectionFrame(HDC dc, const RECT& selection)
        {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(65, 145, 255));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush =
                SelectObject(dc, GetStockObject(NULL_BRUSH));

            Rectangle(
                dc,
                selection.left,
                selection.top,
                selection.right,
                selection.bottom);

            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);

            wchar_t sizeText[64] = {};
            swprintf_s(
                sizeText,
                L"%d × %d",
                RectWidth(selection),
                RectHeight(selection));

            LONG labelTop =
                selection.top > 32L
                    ? selection.top - 30L
                    : selection.top + 4L;

            RECT label = {
                selection.left + 4L,
                labelTop,
                selection.left + 180L,
                labelTop + 26L
            };

            HBRUSH background = CreateSolidBrush(RGB(34, 34, 34));
            FillRect(dc, &label, background);
            DeleteObject(background);

            HFONT font = CreateUiFont(16, FW_SEMIBOLD);
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(
                dc,
                sizeText,
                -1,
                &label,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }

        void DrawHint(HDC dc)
        {
            RECT area = {
                0,
                16,
                static_cast<LONG>(m_virtualWidth),
                58
            };

            const wchar_t* message =
                m_state == CaptureState::Selecting
                    ? L"按住鼠标左键拖动选择截图区域，Esc 或右键取消"
                    : L"选择工具进行标注，点击“完成”复制到剪贴板";

            HFONT font = CreateUiFont(20, FW_SEMIBOLD);
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(
                dc,
                message,
                -1,
                &area,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }

        void Paint(HWND window)
        {
            PAINTSTRUCT paint = {};
            HDC windowDC = BeginPaint(window, &paint);

            RECT client = {};
            GetClientRect(window, &client);
            const int width = RectWidth(client);
            const int height = RectHeight(client);

            HDC bufferDC = CreateCompatibleDC(windowDC);
            HBITMAP bufferBitmap =
                CreateCompatibleBitmap(windowDC, width, height);
            HGDIOBJ oldBufferBitmap =
                SelectObject(bufferDC, bufferBitmap);

            // Frozen darkened desktop background.
            BitBlt(
                bufferDC,
                0,
                0,
                width,
                height,
                m_dimDC,
                0,
                0,
                SRCCOPY);

            RECT visibleSelection = m_selection;
            if (m_state == CaptureState::Selecting &&
                m_dragging) {
                visibleSelection =
                    NormalizeRect(m_dragStart, m_dragCurrent);
            }

            if (ValidSelection(visibleSelection)) {
                // Restore full brightness only inside the selection.
                BitBlt(
                    bufferDC,
                    visibleSelection.left,
                    visibleSelection.top,
                    RectWidth(visibleSelection),
                    RectHeight(visibleSelection),
                    m_screenDC,
                    visibleSelection.left,
                    visibleSelection.top,
                    SRCCOPY);

                const int saved = SaveDC(bufferDC);
                IntersectClipRect(
                    bufferDC,
                    visibleSelection.left,
                    visibleSelection.top,
                    visibleSelection.right,
                    visibleSelection.bottom);

                for (const Annotation& annotation : m_annotations) {
                    DrawAnnotation(bufferDC, annotation, 0, 0);
                }
                if (m_drawing) {
                    DrawAnnotation(bufferDC, m_current, 0, 0);
                }

                RestoreDC(bufferDC, saved);
                DrawSelectionFrame(bufferDC, visibleSelection);

                if (m_state == CaptureState::Editing) {
                    DrawToolbar(bufferDC);
                }
            }

            DrawHint(bufferDC);

            BitBlt(
                windowDC,
                0,
                0,
                width,
                height,
                bufferDC,
                0,
                0,
                SRCCOPY);

            SelectObject(bufferDC, oldBufferBitmap);
            DeleteObject(bufferBitmap);
            DeleteDC(bufferDC);
            EndPaint(window, &paint);
        }

        bool ToolbarClick(POINT point)
        {
            const std::vector<ToolbarButton> buttons =
                ToolbarButtons();

            for (const ToolbarButton& button : buttons) {
                if (!ContainsPoint(button.rect, point)) {
                    continue;
                }

                switch (button.action) {
                case ButtonAction::Tool:
                    m_activeTool = button.tool;
                    m_drawing = false;
                    m_current = Annotation();
                    Repaint();
                    return true;

                case ButtonAction::Undo:
                    if (!m_annotations.empty()) {
                        m_annotations.pop_back();
                    }
                    Repaint();
                    return true;

                case ButtonAction::Finish:
                    Finish();
                    return true;

                case ButtonAction::Cancel:
                    if (m_window != NULL) {
                        DestroyWindow(m_window);
                    }
                    return true;
                }
            }

            return false;
        }

        void BeginDrawing(POINT point)
        {
            if (!ContainsPoint(m_selection, point)) {
                return;
            }

            m_drawing = true;
            m_current = Annotation();
            m_current.tool = m_activeTool;
            m_current.start = point;
            m_current.end = point;
            m_current.color = RGB(255, 72, 72);
            m_current.thickness =
                m_activeTool == Tool::Brush ? 4 : 3;

            if (m_activeTool == Tool::Brush ||
                m_activeTool == Tool::Mosaic) {
                m_current.points.push_back(point);
            }

            SetCapture(m_window);
            Repaint();
        }

        void UpdateDrawing(POINT point)
        {
            if (!m_drawing) {
                return;
            }

            point.x = ClampLong(
                point.x,
                m_selection.left,
                m_selection.right - 1);
            point.y = ClampLong(
                point.y,
                m_selection.top,
                m_selection.bottom - 1);

            m_current.end = point;

            if (m_current.tool == Tool::Brush ||
                m_current.tool == Tool::Mosaic) {
                bool addPoint = m_current.points.empty();

                if (!addPoint) {
                    const POINT& previous =
                        m_current.points.back();
                    const LONG dx = point.x - previous.x;
                    const LONG dy = point.y - previous.y;
                    addPoint =
                        dx >= 2 || dx <= -2 ||
                        dy >= 2 || dy <= -2;
                }

                if (addPoint) {
                    m_current.points.push_back(point);
                }
            }

            Repaint();
        }

        void EndDrawing(POINT point)
        {
            if (!m_drawing) {
                return;
            }

            UpdateDrawing(point);
            m_drawing = false;
            ReleaseCapture();

            bool valid = true;
            if (m_current.tool == Tool::Brush ||
                m_current.tool == Tool::Mosaic) {
                valid = m_current.points.size() >= 2;
            } else {
                const LONG dx =
                    m_current.end.x - m_current.start.x;
                const LONG dy =
                    m_current.end.y - m_current.start.y;
                valid =
                    dx >= 2 || dx <= -2 ||
                    dy >= 2 || dy <= -2;
            }

            if (valid) {
                m_annotations.push_back(m_current);
            }

            m_current = Annotation();
            Repaint();
        }

        bool CopyToClipboard()
        {
            if (!ValidSelection(m_selection) ||
                m_screenDC == NULL) {
                return false;
            }

            const int width = RectWidth(m_selection);
            const int height = RectHeight(m_selection);

            HDC desktopDC = GetDC(NULL);
            if (desktopDC == NULL) {
                return false;
            }

            HDC outputDC = NULL;
            HBITMAP outputBitmap = NULL;
            HGDIOBJ oldOutputBitmap = NULL;
            BYTE* outputBits = NULL;

            const bool created = CreateTopDownDib(
                desktopDC,
                width,
                height,
                outputDC,
                outputBitmap,
                oldOutputBitmap,
                outputBits);

            ReleaseDC(NULL, desktopDC);

            if (!created) {
                return false;
            }

            BitBlt(
                outputDC,
                0,
                0,
                width,
                height,
                m_screenDC,
                m_selection.left,
                m_selection.top,
                SRCCOPY);

            const int saved = SaveDC(outputDC);
            IntersectClipRect(outputDC, 0, 0, width, height);

            for (const Annotation& annotation : m_annotations) {
                DrawAnnotation(
                    outputDC,
                    annotation,
                    -m_selection.left,
                    -m_selection.top);
            }

            RestoreDC(outputDC, saved);

            const SIZE_T rowBytes =
                static_cast<SIZE_T>(width) * 4;
            const SIZE_T imageBytes =
                rowBytes * static_cast<SIZE_T>(height);
            const SIZE_T totalBytes =
                sizeof(BITMAPINFOHEADER) + imageBytes;

            HGLOBAL memory =
                GlobalAlloc(GMEM_MOVEABLE, totalBytes);

            if (memory == NULL) {
                SelectObject(outputDC, oldOutputBitmap);
                DeleteObject(outputBitmap);
                DeleteDC(outputDC);
                return false;
            }

            BYTE* destination =
                static_cast<BYTE*>(GlobalLock(memory));

            if (destination == NULL) {
                GlobalFree(memory);
                SelectObject(outputDC, oldOutputBitmap);
                DeleteObject(outputBitmap);
                DeleteDC(outputDC);
                return false;
            }

            BITMAPINFOHEADER* header =
                reinterpret_cast<BITMAPINFOHEADER*>(destination);
            ZeroMemory(header, sizeof(*header));
            header->biSize = sizeof(BITMAPINFOHEADER);
            header->biWidth = width;
            header->biHeight = height;
            header->biPlanes = 1;
            header->biBitCount = 32;
            header->biCompression = BI_RGB;
            header->biSizeImage =
                static_cast<DWORD>(imageBytes);

            BYTE* clipboardBits =
                destination + sizeof(BITMAPINFOHEADER);

            for (int row = 0; row < height; ++row) {
                const BYTE* sourceRow =
                    outputBits +
                    static_cast<SIZE_T>(
                        height - 1 - row) * rowBytes;
                BYTE* targetRow =
                    clipboardBits +
                    static_cast<SIZE_T>(row) * rowBytes;

                std::memcpy(targetRow, sourceRow, rowBytes);

                for (int x = 0; x < width; ++x) {
                    targetRow[
                        static_cast<SIZE_T>(x) * 4 + 3] = 255;
                }
            }

            GlobalUnlock(memory);

            SelectObject(outputDC, oldOutputBitmap);
            DeleteObject(outputBitmap);
            DeleteDC(outputDC);

            bool opened = false;
            for (int attempt = 0;
                 attempt < 10 && !opened;
                 ++attempt) {
                opened = OpenClipboard(m_window) != FALSE;
                if (!opened) {
                    Sleep(10);
                }
            }

            if (!opened) {
                GlobalFree(memory);
                return false;
            }

            if (!EmptyClipboard()) {
                CloseClipboard();
                GlobalFree(memory);
                return false;
            }

            if (SetClipboardData(CF_DIB, memory) == NULL) {
                CloseClipboard();
                GlobalFree(memory);
                return false;
            }

            CloseClipboard();
            return true;
        }

        void Finish()
        {
            if (CopyToClipboard() && m_window != NULL) {
                DestroyWindow(m_window);
            }
        }

        LRESULT HandleMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            switch (message) {
            case WM_ERASEBKGND:
                return 1;

            case WM_SETCURSOR:
                SetCursor(LoadCursorW(NULL, IDC_CROSS));
                return TRUE;

            case WM_LBUTTONDOWN:
            {
                POINT point = {
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };

                if (m_state == CaptureState::Selecting) {
                    m_dragging = true;
                    m_dragStart = point;
                    m_dragCurrent = point;
                    SetCapture(window);
                    Repaint();
                    return 0;
                }

                if (ToolbarClick(point)) {
                    return 0;
                }

                BeginDrawing(point);
                return 0;
            }

            case WM_MOUSEMOVE:
            {
                POINT point = {
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };

                if (m_state == CaptureState::Selecting &&
                    m_dragging) {
                    m_dragCurrent = point;
                    Repaint();
                    return 0;
                }

                if (m_state == CaptureState::Editing &&
                    m_drawing) {
                    UpdateDrawing(point);
                    return 0;
                }

                return 0;
            }

            case WM_LBUTTONUP:
            {
                POINT point = {
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };

                if (m_state == CaptureState::Selecting &&
                    m_dragging) {
                    m_dragCurrent = point;
                    m_dragging = false;
                    ReleaseCapture();

                    m_selection =
                        NormalizeRect(m_dragStart, m_dragCurrent);

                    if (ValidSelection(m_selection)) {
                        m_state = CaptureState::Editing;
                    } else {
                        m_selection = {};
                    }

                    Repaint();
                    return 0;
                }

                if (m_state == CaptureState::Editing &&
                    m_drawing) {
                    EndDrawing(point);
                    return 0;
                }

                return 0;
            }

            case WM_LBUTTONDBLCLK:
                if (m_state == CaptureState::Editing) {
                    Finish();
                    return 0;
                }
                break;

            case WM_RBUTTONDOWN:
                DestroyWindow(window);
                return 0;

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    if (m_drawing) {
                        m_drawing = false;
                        m_current = Annotation();
                        ReleaseCapture();
                        Repaint();
                    } else {
                        DestroyWindow(window);
                    }
                    return 0;
                }

                if (wParam == VK_RETURN &&
                    m_state == CaptureState::Editing) {
                    Finish();
                    return 0;
                }

                if ((wParam == 'Z' &&
                     (GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
                    wParam == VK_BACK) {
                    if (!m_annotations.empty()) {
                        m_annotations.pop_back();
                        Repaint();
                    }
                    return 0;
                }
                break;

            case WM_PAINT:
                Paint(window);
                return 0;

            case WM_NCDESTROY:
                if (m_window == window) {
                    m_window = NULL;
                }
                return 0;
            }

            return DefWindowProcW(
                window,
                message,
                wParam,
                lParam);
        }

        static LRESULT CALLBACK WindowProc(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            ScreenSnipper* self =
                reinterpret_cast<ScreenSnipper*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));

            if (message == WM_NCCREATE) {
                CREATESTRUCTW* create =
                    reinterpret_cast<CREATESTRUCTW*>(lParam);
                self =
                    static_cast<ScreenSnipper*>(
                        create->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(self));
            }

            if (self != NULL) {
                return self->HandleMessage(
                    window,
                    message,
                    wParam,
                    lParam);
            }

            return DefWindowProcW(
                window,
                message,
                wParam,
                lParam);
        }

        HANDLE m_thread;
        HANDLE m_readyEvent;
        DWORD m_threadId;
        bool m_hotkeyRegistered;

        HWND m_window;

        HDC m_screenDC;
        HBITMAP m_screenBitmap;
        HGDIOBJ m_oldScreenBitmap;
        BYTE* m_screenBits;

        HDC m_dimDC;
        HBITMAP m_dimBitmap;
        HGDIOBJ m_oldDimBitmap;
        BYTE* m_dimBits;

        int m_stride;
        int m_virtualX;
        int m_virtualY;
        int m_virtualWidth;
        int m_virtualHeight;

        CaptureState m_state;
        bool m_dragging;
        bool m_drawing;
        POINT m_dragStart;
        POINT m_dragCurrent;
        RECT m_selection;

        Tool m_activeTool;
        Annotation m_current;
        std::vector<Annotation> m_annotations;

        std::atomic<bool> m_captureActive;
    };

    ScreenSnipper g_snipper;
}

bool ScreenSnipperInitialize()
{
    return g_snipper.Initialize();
}

void ScreenSnipperShutdown()
{
    g_snipper.Shutdown();
}

void ScreenSnipperStartCapture()
{
    g_snipper.StartCapture();
}
