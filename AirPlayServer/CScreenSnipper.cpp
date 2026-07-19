#include "CScreenSnipper.h"

#include <Windows.h>
#include <Windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{
    constexpr int kHotkeyId = 0x4152;
    constexpr UINT kStartCaptureMessage = WM_APP + 0x452;

    RECT NormalizeRect(POINT first, POINT second)
    {
        RECT result = {};
        result.left = (std::min)(first.x, second.x);
        result.top = (std::min)(first.y, second.y);
        result.right = (std::max)(first.x, second.x);
        result.bottom = (std::max)(first.y, second.y);
        return result;
    }

    class CScreenSnipper
    {
    public:
        CScreenSnipper()
  : m_thread(NULL)
  , m_readyEvent(NULL)
  , m_threadId(0)
  , m_hotkeyRegistered(false)
  , m_overlayWindow(NULL)
  , m_captureDC(NULL)
  , m_captureBitmap(NULL)
  , m_oldCaptureBitmap(NULL)
  , m_captureBits(NULL)
  , m_captureStride(0)
  , m_virtualX(0)
  , m_virtualY(0)
  , m_virtualWidth(0)
  , m_virtualHeight(0)
  , m_dragging(false)
  , m_captureActive(false)
        {
  m_dragStart = {};
  m_dragCurrent = {};
        }

        ~CScreenSnipper()
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
      &CScreenSnipper::ThreadEntry,
      this,
      0,
      &m_threadId);

  if (m_thread == NULL) {
      CloseHandle(m_readyEvent);
      m_readyEvent = NULL;
      m_threadId = 0;
      return false;
  }

  const DWORD waitResult =
      WaitForSingleObject(m_readyEvent, 5000);
  return waitResult == WAIT_OBJECT_0;
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
  CScreenSnipper* self =
      static_cast<CScreenSnipper*>(parameter);
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
          CaptureSelection();
          continue;
      }

      TranslateMessage(&message);
      DispatchMessageW(&message);
  }

  if (m_hotkeyRegistered) {
      UnregisterHotKey(NULL, kHotkeyId);
      m_hotkeyRegistered = false;
  }

  ReleaseDesktopCapture();
        }

        bool CaptureDesktop()
        {
  ReleaseDesktopCapture();

  m_virtualX =
      GetSystemMetrics(SM_XVIRTUALSCREEN);
  m_virtualY =
      GetSystemMetrics(SM_YVIRTUALSCREEN);
  m_virtualWidth =
      GetSystemMetrics(SM_CXVIRTUALSCREEN);
  m_virtualHeight =
      GetSystemMetrics(SM_CYVIRTUALSCREEN);

  if (m_virtualWidth <= 0 ||
      m_virtualHeight <= 0) {
      return false;
  }

  HDC screenDC = GetDC(NULL);
  if (screenDC == NULL) {
      return false;
  }

  m_captureDC = CreateCompatibleDC(screenDC);
  if (m_captureDC == NULL) {
      ReleaseDC(NULL, screenDC);
      return false;
  }

  BITMAPINFO bitmapInfo = {};
  bitmapInfo.bmiHeader.biSize =
      sizeof(BITMAPINFOHEADER);
  bitmapInfo.bmiHeader.biWidth =
      m_virtualWidth;
  bitmapInfo.bmiHeader.biHeight =
      -m_virtualHeight;
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  m_captureBitmap = CreateDIBSection(
      screenDC,
      &bitmapInfo,
      DIB_RGB_COLORS,
      reinterpret_cast<void**>(&m_captureBits),
      NULL,
      0);

  if (m_captureBitmap == NULL ||
      m_captureBits == NULL) {
      ReleaseDC(NULL, screenDC);
      ReleaseDesktopCapture();
      return false;
  }

  m_oldCaptureBitmap =
      SelectObject(
          m_captureDC,
          m_captureBitmap);
  m_captureStride = m_virtualWidth * 4;

  const BOOL copied = BitBlt(
      m_captureDC,
      0,
      0,
      m_virtualWidth,
      m_virtualHeight,
      screenDC,
      m_virtualX,
      m_virtualY,
      SRCCOPY | CAPTUREBLT);

  ReleaseDC(NULL, screenDC);

  if (!copied) {
      ReleaseDesktopCapture();
      return false;
  }

  return true;
        }

        void ReleaseDesktopCapture()
        {
  if (m_captureDC != NULL &&
      m_oldCaptureBitmap != NULL) {
      SelectObject(
          m_captureDC,
          m_oldCaptureBitmap);
  }

  m_oldCaptureBitmap = NULL;

  if (m_captureBitmap != NULL) {
      DeleteObject(m_captureBitmap);
      m_captureBitmap = NULL;
  }

  if (m_captureDC != NULL) {
      DeleteDC(m_captureDC);
      m_captureDC = NULL;
  }

  m_captureBits = NULL;
  m_captureStride = 0;
        }

        void CaptureSelection()
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

  static const wchar_t* className =
      L"AirPlayServerRegionCaptureOverlay";

  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc =
      &CScreenSnipper::WindowProcedure;
  windowClass.hInstance =
      GetModuleHandleW(NULL);
  windowClass.hCursor =
      LoadCursorW(NULL, IDC_CROSS);
  windowClass.hbrBackground =
      static_cast<HBRUSH>(
          GetStockObject(BLACK_BRUSH));
  windowClass.lpszClassName = className;

  if (RegisterClassExW(&windowClass) == 0 &&
      GetLastError() !=
          ERROR_CLASS_ALREADY_EXISTS) {
      ReleaseDesktopCapture();
      m_captureActive = false;
      return;
  }

  m_dragging = false;
  m_dragStart = {};
  m_dragCurrent = {};

  m_overlayWindow = CreateWindowExW(
      WS_EX_TOPMOST |
          WS_EX_TOOLWINDOW |
          WS_EX_LAYERED,
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

  if (m_overlayWindow == NULL) {
      ReleaseDesktopCapture();
      m_captureActive = false;
      return;
  }

  SetLayeredWindowAttributes(
      m_overlayWindow,
      0,
      105,
      LWA_ALPHA);

  ShowWindow(m_overlayWindow, SW_SHOW);
  UpdateWindow(m_overlayWindow);
  SetForegroundWindow(m_overlayWindow);
  SetFocus(m_overlayWindow);

  MSG message = {};
  while (m_overlayWindow != NULL &&
         IsWindow(m_overlayWindow)) {
      const BOOL result =
          GetMessageW(
              &message,
              NULL,
              0,
              0);

      if (result <= 0) {
          if (result == 0) {
              PostQuitMessage(
                  static_cast<int>(
                      message.wParam));
          }
          break;
      }

      if (message.message == WM_HOTKEY ||
          message.message ==
              kStartCaptureMessage) {
          continue;
      }

      TranslateMessage(&message);
      DispatchMessageW(&message);
  }

  ReleaseDesktopCapture();
  m_captureActive = false;
        }

        bool CopySelectionToClipboard(
  const RECT& selection)
        {
  const int width =
      selection.right - selection.left;
  const int height =
      selection.bottom - selection.top;

  if (width < 3 || height < 3 ||
      m_captureBits == NULL) {
      return false;
  }

  const SIZE_T rowBytes =
      static_cast<SIZE_T>(width) * 4;
  const SIZE_T imageBytes =
      rowBytes *
      static_cast<SIZE_T>(height);
  const SIZE_T totalBytes =
      sizeof(BITMAPINFOHEADER) +
      imageBytes;

  HGLOBAL memory = GlobalAlloc(
      GMEM_MOVEABLE,
      totalBytes);
  if (memory == NULL) {
      return false;
  }

  BYTE* destination =
      static_cast<BYTE*>(
          GlobalLock(memory));
  if (destination == NULL) {
      GlobalFree(memory);
      return false;
  }

  BITMAPINFOHEADER* header =
      reinterpret_cast<
          BITMAPINFOHEADER*>(
              destination);
  ZeroMemory(
      header,
      sizeof(BITMAPINFOHEADER));
  header->biSize =
      sizeof(BITMAPINFOHEADER);
  header->biWidth = width;
  header->biHeight = height;
  header->biPlanes = 1;
  header->biBitCount = 32;
  header->biCompression = BI_RGB;
  header->biSizeImage =
      static_cast<DWORD>(imageBytes);

  BYTE* outputBits =
      destination +
      sizeof(BITMAPINFOHEADER);

  for (int outputRow = 0;
       outputRow < height;
       ++outputRow) {
      const int sourceY =
          selection.top +
          (height - 1 - outputRow);

      const BYTE* sourceRow =
          m_captureBits +
          static_cast<SIZE_T>(
              sourceY) *
              m_captureStride +
          static_cast<SIZE_T>(
              selection.left) *
              4;

      BYTE* outputRowPointer =
          outputBits +
          static_cast<SIZE_T>(
              outputRow) *
              rowBytes;

      std::memcpy(
          outputRowPointer,
          sourceRow,
          rowBytes);

      for (int x = 0; x < width; ++x) {
          outputRowPointer[
              static_cast<SIZE_T>(x) *
              4 + 3] = 255;
      }
  }

  GlobalUnlock(memory);

  bool opened = false;
  for (int attempt = 0;
       attempt < 10 && !opened;
       ++attempt) {
      opened =
          OpenClipboard(
              m_overlayWindow) != FALSE;
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

  if (SetClipboardData(
          CF_DIB,
          memory) == NULL) {
      CloseClipboard();
      GlobalFree(memory);
      return false;
  }

  CloseClipboard();
  return true;
        }

        void PaintOverlay(HWND window)
        {
  PAINTSTRUCT paint = {};
  HDC deviceContext =
      BeginPaint(window, &paint);

  RECT client = {};
  GetClientRect(window, &client);
  FillRect(
      deviceContext,
      &client,
      static_cast<HBRUSH>(
          GetStockObject(
              BLACK_BRUSH)));

  SetBkMode(
      deviceContext,
      TRANSPARENT);
  SetTextColor(
      deviceContext,
      RGB(255, 255, 255));

  HFONT font = CreateFontW(
      -22,
      0,
      0,
      0,
      FW_SEMIBOLD,
      FALSE,
      FALSE,
      FALSE,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      DEFAULT_PITCH,
      L"Segoe UI");

  HFONT oldFont =
      static_cast<HFONT>(
          SelectObject(
              deviceContext,
              font));

  RECT hintArea = {
      0,
      20,
      client.right,
      60
  };
  DrawTextW(
      deviceContext,
      L"拖动鼠标选择截图区域   Esc 或右键取消",
      -1,
      &hintArea,
      DT_CENTER |
          DT_SINGLELINE |
          DT_VCENTER);

  const RECT selection =
      NormalizeRect(
          m_dragStart,
          m_dragCurrent);

  const int selectedWidth =
      selection.right -
      selection.left;
  const int selectedHeight =
      selection.bottom -
      selection.top;

  if ((m_dragging ||
       selectedWidth > 0 ||
       selectedHeight > 0) &&
      selectedWidth > 0 &&
      selectedHeight > 0) {
      BitBlt(
          deviceContext,
          selection.left,
          selection.top,
          selectedWidth,
          selectedHeight,
          m_captureDC,
          selection.left,
          selection.top,
          SRCCOPY);

      HPEN pen = CreatePen(
          PS_SOLID,
          3,
          RGB(255, 255, 255));
      HGDIOBJ oldPen =
          SelectObject(
              deviceContext,
              pen);
      HGDIOBJ oldBrush =
          SelectObject(
              deviceContext,
              GetStockObject(
                  NULL_BRUSH));

      Rectangle(
          deviceContext,
          selection.left,
          selection.top,
          selection.right,
          selection.bottom);

      SelectObject(
          deviceContext,
          oldBrush);
      SelectObject(
          deviceContext,
          oldPen);
      DeleteObject(pen);

      wchar_t sizeText[64] = {};
      swprintf_s(
          sizeText,
          L"%d × %d",
          selectedWidth,
          selectedHeight);

      const LONG labelTop =
          selection.top > 34L
              ? selection.top - 34L
              : 0L;

      RECT label = {
          selection.left,
          labelTop,
          selection.left + 180L,
          selection.top
      };

      DrawTextW(
          deviceContext,
          sizeText,
          -1,
          &label,
          DT_LEFT |
              DT_SINGLELINE |
              DT_VCENTER);
  }

  SelectObject(
      deviceContext,
      oldFont);
  DeleteObject(font);
  EndPaint(window, &paint);
        }

        LRESULT HandleWindowMessage(
  HWND window,
  UINT message,
  WPARAM wParam,
  LPARAM lParam)
        {
  switch (message) {
  case WM_ERASEBKGND:
      return 1;

  case WM_SETCURSOR:
      SetCursor(
          LoadCursorW(
              NULL,
              IDC_CROSS));
      return TRUE;

  case WM_LBUTTONDOWN:
      m_dragging = true;
      m_dragStart.x =
          GET_X_LPARAM(lParam);
      m_dragStart.y =
          GET_Y_LPARAM(lParam);
      m_dragCurrent =
          m_dragStart;
      SetCapture(window);
      InvalidateRect(
          window,
          NULL,
          TRUE);
      return 0;

  case WM_MOUSEMOVE:
      if (m_dragging) {
          m_dragCurrent.x =
              GET_X_LPARAM(lParam);
          m_dragCurrent.y =
              GET_Y_LPARAM(lParam);
          InvalidateRect(
              window,
              NULL,
              TRUE);
      }
      return 0;

  case WM_LBUTTONUP:
      if (m_dragging) {
          m_dragCurrent.x =
              GET_X_LPARAM(lParam);
          m_dragCurrent.y =
              GET_Y_LPARAM(lParam);
          m_dragging = false;
          ReleaseCapture();

          const RECT selection =
              NormalizeRect(
                  m_dragStart,
                  m_dragCurrent);

          CopySelectionToClipboard(
              selection);
          DestroyWindow(window);
      }
      return 0;

  case WM_RBUTTONDOWN:
      DestroyWindow(window);
      return 0;

  case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
          DestroyWindow(window);
          return 0;
      }
      break;

  case WM_PAINT:
      PaintOverlay(window);
      return 0;

  case WM_NCDESTROY:
      if (m_overlayWindow == window) {
          m_overlayWindow = NULL;
      }
      return 0;
  }

  return DefWindowProcW(
      window,
      message,
      wParam,
      lParam);
        }

        static LRESULT CALLBACK WindowProcedure(
  HWND window,
  UINT message,
  WPARAM wParam,
  LPARAM lParam)
        {
  CScreenSnipper* self =
      reinterpret_cast<
          CScreenSnipper*>(
              GetWindowLongPtrW(
                  window,
                  GWLP_USERDATA));

  if (message == WM_NCCREATE) {
      CREATESTRUCTW* create =
          reinterpret_cast<
              CREATESTRUCTW*>(lParam);
      self =
          static_cast<
              CScreenSnipper*>(
                  create->lpCreateParams);
      SetWindowLongPtrW(
          window,
          GWLP_USERDATA,
          reinterpret_cast<
              LONG_PTR>(self));
  }

  if (self != NULL) {
      return self->
          HandleWindowMessage(
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

        HWND m_overlayWindow;
        HDC m_captureDC;
        HBITMAP m_captureBitmap;
        HGDIOBJ m_oldCaptureBitmap;
        BYTE* m_captureBits;
        int m_captureStride;

        int m_virtualX;
        int m_virtualY;
        int m_virtualWidth;
        int m_virtualHeight;

        bool m_dragging;
        POINT m_dragStart;
        POINT m_dragCurrent;
        std::atomic<bool> m_captureActive;
    };

    CScreenSnipper g_screenSnipper;
}

bool ScreenSnipperInitialize()
{
    return g_screenSnipper.Initialize();
}

void ScreenSnipperShutdown()
{
    g_screenSnipper.Shutdown();
}

void ScreenSnipperStartCapture()
{
    g_screenSnipper.StartCapture();
}
