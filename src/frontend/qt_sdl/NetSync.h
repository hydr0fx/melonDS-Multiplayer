/*
    NetSync: input-delay netplay for melonDS (local-wireless / DS Download Play style games).

    Every PC runs ALL DS instances locally in deterministic emulated-time lockstep
    (connected through DeterministicMP, zero OS thread timing dependency),
    and only the controller/touch inputs are exchanged over the network, applied a
    fixed number of frames later on every PC.

    Configured through environment variables (see Start()):
      MELONDS_NETSYNC         = host | client
      MELONDS_NETSYNC_ADDR    = host address (client only)
      MELONDS_NETSYNC_PORT    = port (default 7090)
      MELONDS_NETSYNC_PLAYERS = number of players (host only, 2-4, default 2)
      MELONDS_NETSYNC_DELAY   = input delay in frames (host only, default 4)
      MELONDS_NETSYNC_JIT     = 1 | 0 (host only, default 1)
*/

#ifndef NETSYNC_H
#define NETSYNC_H

#include <string>
#include <vector>
#include "types.h"

namespace CLI { struct CommandLineOptions; }

namespace NetSync
{

const int kLanDiscoveryPort = 7091;

void ApplyCLIOptions(const CLI::CommandLineOptions& opts);
bool Configured();
bool Active();
bool Aborted();
bool IsHost();
bool UseJIT();
bool MultiCard();
int LocalSlot();
int NumPlayers();
int Delay();
std::string PlayerName(int slot);

std::string EncodeRoomID(melonDS::u32 ipv4HostOrder, melonDS::u16 port);
bool DecodeRoomID(const std::string& text, std::string& outIp, melonDS::u16& outPort);
bool DiscoverPublicEndpoint(melonDS::u16 localUdpPort, std::string& outIp, melonDS::u16& outMappedPort, std::string& outNatNote, melonDS::u16* outLocalPort = nullptr);
std::string BestLocalIP();

// Blocking connection handshake (shows a lobby/progress dialog). Call before creating emu instances.
bool Start(const std::string& romPath, std::string& error);

// After all local instances were booted (and are blocked at frame 0):
// client sends READY, host waits for every READY and then starts the session.
bool WaitAndGo(std::string& error);
void FinishLoading();

void Stop();

// Unblocks every emu thread waiting for input (used when a window is closed).
void Abort();

const std::vector<melonDS::u8>& HostSave();

// Called by the master co-scheduler once per frame per instance, before running the frame.
// Blocks until the inputs for (inst, frame) are known.
void FrameInput(int inst, melonDS::u32 frame, melonDS::u32& keymask, bool& touching, melonDS::u16& tx, melonDS::u16& ty, bool& mic);

// Desync detection: hash of main RAM at a given frame.
void ReportRAMHash(int inst, melonDS::u32 frame, melonDS::u32 hash);

std::string StatusText();
int ConsumeLagSlot();

void Log(const char* fmt, ...);

}

#endif
