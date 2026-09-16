#pragma once

/*
 * Vendored declarations for process-loopback audio capture.
 *
 * mingw-w64 ships <mmdeviceapi.h> with ActivateAudioInterfaceAsync and provides
 * libmmdevapi.a, but it does not ship <audioclientactivationparams.h> at all, so
 * cross-compiling anything that activates a process-loopback client fails on the
 * activation structures rather than on the API itself.
 *
 * These match the Windows SDK (10.0.19041.0 and later, which is also the floor
 * for the feature -- Windows 10 2004). MSVC picks up the real header from the
 * SDK instead of this one, since ambient/compat is only on the include path for
 * the mingw build; the MSVC compile in .github/workflows/ambient-msvc.yml is
 * what checks these against the genuine definitions.
 */

#include <windows.h>

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#ifndef __AUDIOCLIENT_ACTIVATION_TYPE_DEFINED__
#define __AUDIOCLIENT_ACTIVATION_TYPE_DEFINED__
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;
#endif

#ifndef __PROCESS_LOOPBACK_MODE_DEFINED__
#define __PROCESS_LOOPBACK_MODE_DEFINED__
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
#endif

#ifndef __AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS_DEFINED__
#define __AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS_DEFINED__
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
#endif

#ifndef __AUDIOCLIENT_ACTIVATION_PARAMS_DEFINED__
#define __AUDIOCLIENT_ACTIVATION_PARAMS_DEFINED__
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif
