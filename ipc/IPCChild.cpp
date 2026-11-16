// os-control-ipc-child
//
// The "other process" in the IPC demonstration. The GUI starts it with
// fork() + execv(). It receives messages from the GUI, and answers each one with
//     ECHO from child PID <pid>: <MESSAGE IN UPPERCASE>
// The message "quit" makes it answer and exit.
//
// Usage (started by the GUI, not by hand):
//   os-control-ipc-child pipe <readFd> <writeFd>
//   os-control-ipc-child fifo <pathFromParent> <pathToParent>
//   os-control-ipc-child shm  <name>
//   os-control-ipc-child mq   <queueFromParent> <queueToParent>

#include "IPCProtocol.h"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const size_t kReplySize = kIpcMessageSize + 128;

void sleepMilliseconds(long ms)
{
    timespec pause;
    pause.tv_sec = ms / 1000;
    pause.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&pause, nullptr);
}

// Build the answer text. priority < 0 means "no priority" (pipes, shared memory).
void makeReply(const char *message, int priority, char *out, size_t outSize)
{
    char upper[kIpcMessageSize];
    size_t length = 0;

    for (; message[length] != '\0' && length < sizeof(upper) - 1; ++length)
        upper[length] = static_cast<char>(toupper(static_cast<unsigned char>(message[length])));
    upper[length] = '\0';

    if (priority >= 0)
        snprintf(out, outSize, "ECHO (priority %d) from child PID %d: %s",
                 priority, static_cast<int>(getpid()), upper);
    else
        snprintf(out, outSize, "ECHO from child PID %d: %s",
                 static_cast<int>(getpid()), upper);
}

bool writeFully(int fd, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        const ssize_t result = write(fd, data + written, length - written);

        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }

        written += static_cast<size_t>(result);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pipes and FIFOs: a stream of bytes. Messages are separated by '\n'.
// ---------------------------------------------------------------------------

// Returns false when the conversation is over (quit received or write failed).
bool handleLine(const char *line, int outFd)
{
    char reply[kReplySize];
    const bool quit = (strcmp(line, "quit") == 0);

    if (quit)
        snprintf(reply, sizeof(reply), "child PID %d exiting (quit received)",
                 static_cast<int>(getpid()));
    else
        makeReply(line, -1, reply, sizeof(reply));

    strncat(reply, "\n", sizeof(reply) - strlen(reply) - 1);

    if (!writeFully(outFd, reply, strlen(reply)))
        return false;

    return !quit;
}

int runStream(int inFd, int outFd)
{
    char buffer[4096];
    size_t used = 0;

    for (;;)
    {
        const ssize_t count = read(inFd, buffer + used, sizeof(buffer) - 1 - used);

        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            perror("child: read");
            return 1;
        }

        if (count == 0)
            return 0;           // the parent closed its end: end of conversation

        used += static_cast<size_t>(count);
        buffer[used] = '\0';

        char *start = buffer;
        char *newline;

        while ((newline = static_cast<char *>(memchr(start, '\n', static_cast<size_t>(buffer + used - start)))))
        {
            *newline = '\0';

            if (!handleLine(start, outFd))
                return 0;

            start = newline + 1;
        }

        used = static_cast<size_t>(buffer + used - start);
        memmove(buffer, start, used);

        if (used >= sizeof(buffer) - 1)
            used = 0;           // an impossibly long line: drop it
    }
}

int runPipe(const char *readFdText, const char *writeFdText)
{
    return runStream(atoi(readFdText), atoi(writeFdText));
}

int runFifo(const char *pathFromParent, const char *pathToParent)
{
    // The parent already holds the read end of "pathToParent", so this open succeeds.
    const int outFd = open(pathToParent, O_WRONLY);
    if (outFd < 0)
    {
        perror("child: open fifo for writing");
        return 1;
    }

    // This blocks until the parent opens the other FIFO for writing.
    const int inFd = open(pathFromParent, O_RDONLY);
    if (inFd < 0)
    {
        perror("child: open fifo for reading");
        return 1;
    }

    const int result = runStream(inFd, outFd);
    close(inFd);
    close(outFd);
    return result;
}

// ---------------------------------------------------------------------------
// Shared memory: both processes see the same bytes. We poll the sequence numbers.
// ---------------------------------------------------------------------------
int runSharedMemory(const char *name)
{
    const int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0)
    {
        perror("child: shm_open");
        return 1;
    }

    void *memory = mmap(nullptr, sizeof(SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (memory == MAP_FAILED)
    {
        perror("child: mmap");
        return 1;
    }

    SharedBlock *block = static_cast<SharedBlock *>(memory);
    block->childAttached.store(1, std::memory_order_release);

    uint32_t lastSeen = 0;
    uint32_t sentSeq = 0;
    int result = 0;

    for (;;)
    {
        const uint32_t seq = block->toChildSeq.load(std::memory_order_acquire);

        if (seq == lastSeen)
        {
            sleepMilliseconds(20);
            continue;
        }

        // Copy the message out, then acknowledge it.
        char message[kIpcMessageSize];
        memcpy(message, block->toChild, sizeof(message));
        message[sizeof(message) - 1] = '\0';

        lastSeen = seq;
        block->toChildAck.store(seq, std::memory_order_release);

        const bool quit = (strcmp(message, "quit") == 0);

        char reply[kReplySize];
        if (quit)
            snprintf(reply, sizeof(reply), "child PID %d exiting (quit received)",
                     static_cast<int>(getpid()));
        else
            makeReply(message, -1, reply, sizeof(reply));

        // Wait until the parent has read our previous reply (stop-and-wait).
        for (int waited = 0; waited < 200 &&
             block->toParentAck.load(std::memory_order_acquire) !=
             block->toParentSeq.load(std::memory_order_acquire); ++waited)
            sleepMilliseconds(10);

        strncpy(block->toParent, reply, kIpcMessageSize - 1);
        block->toParent[kIpcMessageSize - 1] = '\0';
        block->toParentSeq.store(++sentSeq, std::memory_order_release);

        if (quit)
            break;
    }

    munmap(memory, sizeof(SharedBlock));
    return result;
}

// ---------------------------------------------------------------------------
// Message queue: the kernel keeps whole messages, highest priority first.
// ---------------------------------------------------------------------------
int runMessageQueue(const char *nameFromParent, const char *nameToParent, pid_t parentPid)
{
    const mqd_t in = mq_open(nameFromParent, O_RDONLY);
    if (in == static_cast<mqd_t>(-1))
    {
        perror("child: mq_open (receive)");
        return 1;
    }

    const mqd_t out = mq_open(nameToParent, O_WRONLY);
    if (out == static_cast<mqd_t>(-1))
    {
        perror("child: mq_open (send)");
        mq_close(in);
        return 1;
    }

    char buffer[kIpcMessageSize + 1];

    for (;;)
    {
        timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;

        unsigned int priority = 0;
        const ssize_t count = mq_timedreceive(in, buffer, kIpcMessageSize, &priority, &deadline);

        if (count < 0)
        {
            if (errno == ETIMEDOUT || errno == EINTR)
            {
                if (getppid() != parentPid)
                    break;          // the parent is gone
                continue;
            }

            perror("child: mq_receive");
            break;
        }

        buffer[count] = '\0';

        const bool quit = (strcmp(buffer, "quit") == 0);

        char reply[kReplySize];
        if (quit)
            snprintf(reply, sizeof(reply), "child PID %d exiting (quit received)",
                     static_cast<int>(getpid()));
        else
            makeReply(buffer, static_cast<int>(priority), reply, sizeof(reply));

        if (mq_send(out, reply, strnlen(reply, kIpcMessageSize), 0) != 0)
        {
            perror("child: mq_send");
            break;
        }

        if (quit)
            break;
    }

    mq_close(in);
    mq_close(out);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    // If the GUI dies, the kernel sends us SIGTERM, so we never become an orphan.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    const pid_t parentPid = getppid();

    // A write to a closed pipe must give an error, not kill us.
    signal(SIGPIPE, SIG_IGN);

    if (argc < 3)
    {
        fprintf(stderr, "os-control-ipc-child: started by the OS Control Center. "
                        "Usage: pipe|fifo|shm|mq <arguments>\n");
        return 2;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "pipe") == 0 && argc == 4)
        return runPipe(argv[2], argv[3]);

    if (strcmp(mode, "fifo") == 0 && argc == 4)
        return runFifo(argv[2], argv[3]);

    if (strcmp(mode, "shm") == 0 && argc == 3)
        return runSharedMemory(argv[2]);

    if (strcmp(mode, "mq") == 0 && argc == 4)
        return runMessageQueue(argv[2], argv[3], parentPid);

    fprintf(stderr, "os-control-ipc-child: bad arguments\n");
    return 2;
}