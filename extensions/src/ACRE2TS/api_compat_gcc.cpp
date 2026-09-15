// GCC/mingw replacement for api_compat.asm.
//
// The MASM original is a tail-call trampoline: it exports
// ts3plugin_onPluginCommandEventProc and jumps to either the plain or the
// _v23 handler depending on the onPluginCommandEvent_v23 flag, forwarding
// arguments untouched in their registers.
//
// A plain C++ function is equivalent here. ts3plugin_onPluginCommandEvent_v23
// forwards only its first three arguments to ts3plugin_onPluginCommandEvent
// anyway, so declaring the trampoline with the wider _v23 signature covers
// both cases. When the client is pre-v23 it passes three arguments and the
// trailing three are never read.
#ifndef _MSC_VER

#include "compat.h"

#include "teamspeak/public_errors.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

#include "TsCallbacks.h"

extern "C" PLUGINS_EXPORTDLL void ts3plugin_onPluginCommandEventProc(
    uint64 serverConnectionHandlerID,
    const char* pluginName,
    const char* pluginCommand,
    anyID invokerClientID,
    const char* invokerName,
    const char* invokerUniqueIdentity) {

    if (onPluginCommandEvent_v23) {
        ts3plugin_onPluginCommandEvent_v23(serverConnectionHandlerID, pluginName,
                                           pluginCommand, invokerClientID,
                                           invokerName, invokerUniqueIdentity);
    } else {
        ts3plugin_onPluginCommandEvent(serverConnectionHandlerID, pluginName,
                                       pluginCommand);
    }
}

#endif  // _MSC_VER
