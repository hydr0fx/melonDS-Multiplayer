/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef DETERMINISTICMP_H
#define DETERMINISTICMP_H

#include <array>
#include <deque>
#include "types.h"
#include "MPInterface.h"

namespace melonDS
{

// Purely deterministic, lock-free MPInterface driven by emulated SysTimestamp.
// Packets sent at emulated time T become visible to receivers at T + kAirDelayCycles,
// guaranteeing independence from OS thread scheduling and instance iteration order.
class DeterministicMP : public MPInterface
{
public:
    static constexpr u32 kMaxInstances = 16;
    static constexpr u32 kMaxPacketSize = 0x948;
    static constexpr u32 kMaxReplySize = 1024;
    static constexpr size_t kMaxQueueLen = 256;
    // Matches one co-scheduler slice (~8 us at 33.513982 MHz)
    static constexpr u64 kAirDelayCycles = 268;

    DeterministicMP() noexcept;
    ~DeterministicMP() noexcept override = default;

    void Process() override {}
    [[nodiscard]] bool IsDeterministic() const noexcept override { return true; }

    void Begin(int inst) override;
    void End(int inst) override;

    int SendPacket(int inst, u8* data, int len, u64 timestamp) override;
    int RecvPacket(int inst, u8* data, u64* timestamp) override;
    int SendCmd(int inst, u8* data, int len, u64 timestamp) override;
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) override;
    int SendAck(int inst, u8* data, int len, u64 timestamp) override;
    int RecvHostPacket(int inst, u8* data, u64* timestamp) override;
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) override;

private:
    struct QueuedPacket
    {
        u32 SenderID = 0;
        u32 Type = 0;
        u32 Length = 0;
        u64 Timestamp = 0;
        u64 SendSysTime = 0;
        u64 DeliverSysTime = 0;
        std::array<u8, kMaxPacketSize> Data {};
    };

    struct ReplySlot
    {
        bool Valid = false;
        u32 SenderID = 0;
        u32 Length = 0;
        u64 Timestamp = 0;
        u64 DeliverSysTime = 0;
        std::array<u8, kMaxReplySize> Data {};
    };

    static u64 CurrentSysTime() noexcept;
    int EnqueuePacket(int inst, u32 type, u8* data, int len, u64 timestamp) noexcept;
    int DequeuePacket(int inst, u8* data, u64* timestamp, bool checkHost) noexcept;

    u16 ConnectedMask = 0;
    int CmdHostID = -1;
    std::array<int, kMaxInstances> LastHostID {};
    std::array<std::deque<QueuedPacket>, kMaxInstances> RxQueues {};
    std::array<ReplySlot, kMaxInstances> ReplySlots {};
};

}

#endif // DETERMINISTICMP_H
