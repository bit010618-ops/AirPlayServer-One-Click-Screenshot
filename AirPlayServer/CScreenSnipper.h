#pragma once

// Starts the screenshot service and registers Ctrl+Shift+C globally.
bool ScreenSnipperInitialize();

// Stops the screenshot service.
void ScreenSnipperShutdown();

// Opens the free rectangular desktop selection overlay.
// The selected image is copied to the Windows clipboard only.
void ScreenSnipperStartCapture();
