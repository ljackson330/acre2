// mingw-w64's x3daudio.h has two gaps versus the legacy DirectX SDK header:
// it omits the math constants, and it does not wrap its declarations in
// extern "C". Without the latter, C++ translation units mangle the names and
// fail to resolve against libx3daudio.a, which exports plain C symbols.
#pragma once

extern "C" {
#include_next <x3daudio.h>
}

#ifndef X3DAUDIO_PI
#define X3DAUDIO_PI  3.141592654f
#endif

#ifndef X3DAUDIO_2PI
#define X3DAUDIO_2PI 6.283185307f
#endif
