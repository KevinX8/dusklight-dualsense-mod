// Audio engine internals (DuskDsp.hpp). On Windows, game data is only reachable through
// __declspec(dllimport) declarations, which DuskDsp.hpp's externs lack. Rename its declarations out
// of the way and redeclare the ones we use as dllimport. Elsewhere the header is used as-is.
#pragma once

#ifdef _WIN32
#define ChannelAux ds_unused_ChannelAux
#define MasterVolume ds_unused_MasterVolume
#define PrevMasterVolume ds_unused_PrevMasterVolume
#define EnableReverb ds_unused_EnableReverb
#define DumpAudio ds_unused_DumpAudio
#define EnableHrtf ds_unused_EnableHrtf
#define OutChannelCount ds_unused_OutChannelCount
#include "dusk/audio/DuskDsp.hpp"
#undef ChannelAux
#undef MasterVolume
#undef PrevMasterVolume
#undef EnableReverb
#undef DumpAudio
#undef EnableHrtf
#undef OutChannelCount

namespace dusk::audio {
__declspec(dllimport) extern ChannelAuxData ChannelAux[DSP_CHANNELS];
__declspec(dllimport) extern f32 MasterVolume;
__declspec(dllimport) extern f32 PrevMasterVolume;
__declspec(dllimport) extern bool EnableReverb;
__declspec(dllimport) extern bool DumpAudio;
__declspec(dllimport) extern bool EnableHrtf;
__declspec(dllimport) extern u8 OutChannelCount;
}  // namespace dusk::audio
#else
#include "dusk/audio/DuskDsp.hpp"
#endif
