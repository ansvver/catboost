#pragma once

#include "udp_recv_packet.h"

#include <library/threading/chunk_queue/queue.h>

#include <util/network/init.h>
#include <util/system/atomic.h>
#include <util/system/event.h>
#include <util/system/yassert.h>
#include <utility>

namespace NNetlibaSocket {
    struct TPacketMeta {
        sockaddr_in6 RemoteAddr;
        sockaddr_in6 MyAddr;
    };

    template <size_t TTNumWriterThreads>
    class TLockFreePacketQueue {
    private:
        enum { MAX_PACKETS_IN_QUEUE = INT_MAX,
               CMD_QUEUE_RESERVE = 1 << 20,
               MAX_DATA_IN_QUEUE = 32 << 20 };

        typedef std::pair<TUdpRecvPacket*, TPacketMeta> TPacket;
        typedef std::conditional_t<TTNumWriterThreads == 1, NThreading::TOneOneQueue<TPacket>, NThreading::TManyOneQueue<TPacket, TTNumWriterThreads>> TImpl;

        mutable TImpl Queue;
        mutable Event QueueEvent;

        mutable TAtomic NumPackets;
        TAtomic DataSize;

    public:
        TLockFreePacketQueue()
            : NumPackets(0)
            , DataSize(0)
        {
        }

        ~TLockFreePacketQueue() {
            TPacket packet;
            while (Queue.Dequeue(packet)) {
                delete packet.first;
            }
        }

        bool IsDataPartFull() const {
            return (NumPackets >= MAX_PACKETS_IN_QUEUE || DataSize >= MAX_DATA_IN_QUEUE - CMD_QUEUE_RESERVE);
        }

        bool Push(TUdpRecvPacket* packet, const TPacketMeta& meta) {
            // simulate OS behavior on buffer overflow - drop packets.
            // yeah it contains small data race (we can add little bit more packets, but nobody cares)
            if (NumPackets >= MAX_PACKETS_IN_QUEUE || DataSize >= MAX_DATA_IN_QUEUE) {
                return false;
            }
            AtomicAdd(NumPackets, 1);
            AtomicAdd(DataSize, packet->DataSize);
            Y_ASSERT(packet->DataStart == 0);

            Queue.Enqueue(TPacket(std::make_pair(packet, meta)));
            QueueEvent.Signal();
            return true;
        }

        bool Pop(TUdpRecvPacket** packet, sockaddr_in6* srcAddr, sockaddr_in6* dstAddr) {
            TPacket p;
            if (!Queue.Dequeue(p)) {
                QueueEvent.Reset();
                if (!Queue.Dequeue(p)) {
                    return false;
                }
                QueueEvent.Signal();
            }
            *packet = p.first;
            *srcAddr = p.second.RemoteAddr;
            *dstAddr = p.second.MyAddr;

            AtomicSub(NumPackets, 1);
            AtomicSub(DataSize, (*packet)->DataSize);
            Y_ASSERT(NumPackets >= 0 && DataSize >= 0);

            return true;
        }

        bool IsEmpty() const {
            return AtomicAdd(NumPackets, 0) == 0;
        }

        Event& GetEvent() const {
            return QueueEvent;
        }
    };
}
