#pragma once
namespace NativeObserver {
#ifdef FF_NATIVE_OBSERVER
bool Requested();
bool Active();
bool Request(float x, float y);
void Entered();
void Finished();
void Position(float& x, float& y);
void Log(const char* event);
void Sample(float x, float y, float z);
#else
inline bool Requested() { return false; }
inline bool Active() { return false; }
#endif
}
