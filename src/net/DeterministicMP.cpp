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

#include <cstring>
#include "DeterministicMP.h"
#include "NDS.h"
#include "Platform.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

DeterministicMP::DeterministicMP() noexcept
{
    LastHostID.fill(-1);
    Log(LogLevel::Info, "DeterministicMP comm init OK\n");
}

u64 DeterministicMP::CurrentSysTime() noexcept
{
    return NDS::Current ? NDS::Current->GetSysTimestamp() : 0;
}

void DeterministicMP::Begin(int inst)
{
    if (inst < 0 || inst >= (int)kMaxInstances) return;
    ConnectedMask |= (1u << inst);
    RxQueues[inst].clear();
    LastHostID[inst] = -1;
    Log(LogLevel::Info, "DeterministicMP: Begin inst=%d mask=%04X\n", inst, ConnectedMask);
}

void DeterministicMP::End(int inst)
{
    if (inst < 0 || inst >= (int)kMaxInstances) return;
    ConnectedMask &= ~(1u << inst);
    RxQueues[inst].clear();
    LastHostID[inst] = -1;
    Log(LogLevel::Info, "DeterministicMP: End inst=%d mask=%04X\n", inst, ConnectedMask);
}

int DeterministicMP::EnqueuePacket(int inst, u32 type, u8* data, int len, u64 timestamp) noexcept
{
    if (inst < 0 || inst >= (int)kMaxInstances) return 0;
    if (len < 0 || len > (int)kMaxPacketSize)
    {
        Log(LogLevel::Warn, "wifi: attempting to send frame too big (len=%d max=%u)\n", len, kMaxPacketSize);
        return 0;
    }

    u64 now = CurrentSysTime();
    u64 deliverTime = now + kAirDelayCycles;

    if (type == 1)
    {
        CmdHostID = inst;
        for (auto& slot : ReplySlots)
            slot.Valid = false;
    }

    for (u32 dst = 0; dst < kMaxInstances; dst++)
    {
        if ((int)dst == inst) continue;
        if (!(ConnectedMask & (1u << dst))) continue;

        auto& q = RxQueues[dst];
        if (q.size() >= kMaxQueueLen)
        {
            Log(LogLevel::Warn, "DeterministicMP: overflow dst=%u sender=%d type=%u\n", dst, inst, type);
            q.pop_front();
        }

        q.emplace_back();
        QueuedPacket& pkt = q.back();
        pkt.SenderID = (u32)inst;
        pkt.Type = type;
        pkt.Length = (u32)len;
        pkt.Timestamp = timestamp;
        pkt.SendSysTime = now;
        pkt.DeliverSysTime = deliverTime;
        if (len > 0 && data)
            memcpy(pkt.Data.data(), data, len);
    }

    return len;
}

int DeterministicMP::DequeuePacket(int inst, u8* data, u64* timestamp, bool checkHost) noexcept
{
    if (inst < 0 || inst >= (int)kMaxInstances) return 0;

    auto& q = RxQueues[inst];
    if (checkHost && LastHostID[inst] != -1)
    {
        if (!(ConnectedMask & (1u << LastHostID[inst])) && q.empty())
        {
            Log(LogLevel::Warn, "DeterministicMP: host %d gone for inst=%d\n", LastHostID[inst], inst);
            return -1;
        }
    }

    if (q.empty())
        return 0;

    u64 now = CurrentSysTime();
    const QueuedPacket& front = q.front();
    if (front.DeliverSysTime > now)
        return 0;

    int len = (int)front.Length;
    if (len > 0 && data)
        memcpy(data, front.Data.data(), len);
    if (front.Type == 1)
        LastHostID[inst] = (int)front.SenderID;
    if (timestamp)
    {
        if (front.Timestamp != 0 && now > front.SendSysTime)
        {
            u64 deltaUs = ((now - front.SendSysTime) * 1000000ULL + 16756991ULL) / 33513982ULL;
            *timestamp = front.Timestamp + deltaUs;
        }
        else
        {
            *timestamp = front.Timestamp;
        }
    }

    q.pop_front();
    return len;
}

int DeterministicMP::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    return EnqueuePacket(inst, 0, data, len, timestamp);
}

int DeterministicMP::RecvPacket(int inst, u8* data, u64* timestamp)
{
    return DequeuePacket(inst, data, timestamp, false);
}

int DeterministicMP::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    return EnqueuePacket(inst, 1, data, len, timestamp);
}

int DeterministicMP::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    if (inst < 0 || inst >= (int)kMaxInstances) return 0;
    if (len <= 0 || !data || aid < 1 || aid >= kMaxInstances)
        return 0;
    if (len > (int)kMaxReplySize)
        len = (int)kMaxReplySize;

    ReplySlot& slot = ReplySlots[aid];
    slot.Valid = true;
    slot.SenderID = (u32)inst;
    slot.Length = (u32)len;
    slot.Timestamp = timestamp;
    slot.DeliverSysTime = CurrentSysTime() + kAirDelayCycles;
    memcpy(slot.Data.data(), data, len);
    return len;
}

int DeterministicMP::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    return EnqueuePacket(inst, 3, data, len, timestamp);
}

int DeterministicMP::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    return DequeuePacket(inst, data, timestamp, true);
}

u16 DeterministicMP::RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask)
{
    (void)inst;
    (void)timestamp;
    u64 now = CurrentSysTime();
    u16 ret = 0;

    for (u32 aid = 1; aid < kMaxInstances; aid++)
    {
        if (!(aidmask & (1u << aid))) continue;
        ReplySlot& slot = ReplySlots[aid];
        if (!slot.Valid) continue;
        if (slot.DeliverSysTime > now) continue;

        if (data && slot.Length > 0)
            memcpy(&data[(aid - 1) * kMaxReplySize], slot.Data.data(), slot.Length);

        slot.Valid = false;
        ret |= (u16)(1u << aid);
    }

    return ret;
}

}
