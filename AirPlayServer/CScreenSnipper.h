#pragma once

// 初始化区域截图服务，并注册全局快捷键 Ctrl+Shift+C。
bool ScreenSnipperInitialize();

// 关闭区域截图服务。
void ScreenSnipperShutdown();

// 启动区域截图。完成后直接复制到剪贴板，不保存文件。
void ScreenSnipperStartCapture();
