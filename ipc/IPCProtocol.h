#ifndef IPCPROTOCOL_H
#define IPCPROTOCOL_H

// Shared by the GUI (IPCManager) and the child program (IPCChild).
// No Qt in this file, because the child program does not use Qt.

#include <atomic>
#include <cstddef>
#include <cstdint>

// Largest message (bytes) used by shared memory and message queues
constexpr size_t kIpcMessageSize = 1024;

// The memory layout both processes see after mmap() of the shared-memory object.
//
// Protocol (stop-and-wait, one message at a time in each direction):
//   writer: copy the text, THEN store the new sequence number (release)
//   reader: see the sequence number change (acquire), read the text, THEN
//           store the same number in the "Ack" field
//   writer: may send the next message only after Ack == Seq
struct SharedBlock
{
    std::atomic<uint32_t> toChildSeq;
    std::atomic<uint32_t> toChildAck;
    std::atomic<uint32_t> toParentSeq;
    std::atomic<uint32_t> toParentAck;
    std::atomic<uint32_t> childAttached;

    char toChild[kIpcMessageSize];
    char toParent[kIpcMessageSize];
};

#endif