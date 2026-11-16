// Automated test of the IPC child program with a REAL parent process.
// It checks all four mechanisms: pipe, FIFO, shared memory, message queue.
//
//   g++ -std=c++17 -o ipc_child_test ipc_child_test.cpp -lrt
//   ./ipc_child_test <path to os-control-ipc-child>
//
// Exit code 0 = every check passed.

#include "../ipc/IPCProtocol.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>

#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <new>

static int failures = 0;
static int checks = 0;
static const char *childProgram = nullptr;

static void check(bool condition, const char *what)
{
    ++checks;
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

static void sleepMs(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }

static pid_t spawn(char *const argv[], int srcA = -1, int dstA = 0, int srcB = -1, int dstB = 0)
{
    const pid_t pid = fork();
    if (pid == 0)
    {
        if (srcA >= 0) dup2(srcA, dstA);
        if (srcB >= 0) dup2(srcB, dstB);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

// Read one line (up to the newline) from a non-blocking descriptor, waiting up to timeoutMs.
static std::string readLine(int fd, int timeoutMs)
{
    std::string line;
    for (int waited = 0; waited < timeoutMs; waited += 10)
    {
        char c;
        const ssize_t n = read(fd, &c, 1);
        if (n == 1)
        {
            if (c == '\n')
                return line;
            line += c;
        }
        else
        {
            sleepMs(10);
        }
    }
    return line;
}

static int childExitCode(pid_t pid)
{
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

static void testPipe()
{
    printf("Anonymous pipe\n");
    int a[2], b[2];
    pipe2(a, O_CLOEXEC);
    pipe2(b, O_CLOEXEC);
    const int childRead = fcntl(a[0], F_DUPFD_CLOEXEC, 10);
    const int toChild = fcntl(a[1], F_DUPFD_CLOEXEC, 10);
    const int fromChild = fcntl(b[0], F_DUPFD_CLOEXEC, 10);
    const int childWrite = fcntl(b[1], F_DUPFD_CLOEXEC, 10);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    fcntl(fromChild, F_SETFL, O_NONBLOCK);

    char *argv[] = {const_cast<char *>(childProgram), const_cast<char *>("pipe"),
                    const_cast<char *>("3"), const_cast<char *>("4"), nullptr};
    const pid_t child = spawn(argv, childRead, 3, childWrite, 4);
    close(childRead);
    close(childWrite);

    check(write(toChild, "hello pipe\n", 11) == 11, "parent wrote a message");
    const std::string reply = readLine(fromChild, 2000);
    check(contains(reply, "HELLO PIPE"), "child answered in uppercase");
    check(contains(reply, "child PID"), "answer names the child's PID");

    write(toChild, "quit\n", 5);
    readLine(fromChild, 2000);
    check(childExitCode(child) == 0, "child exited with code 0 after quit");
    close(toChild);
    close(fromChild);
}

static void testFifo()
{
    printf("Named pipe (FIFO)\n");
    const char *toPath = "/tmp/occ_test_fifo_to";
    const char *fromPath = "/tmp/occ_test_fifo_from";
    unlink(toPath); unlink(fromPath);
    check(mkfifo(toPath, 0600) == 0 && mkfifo(fromPath, 0600) == 0, "mkfifo created both FIFOs");

    struct stat st;
    check(stat(toPath, &st) == 0 && S_ISFIFO(st.st_mode), "the path really is a FIFO");

    const int fromChild = open(fromPath, O_RDONLY | O_NONBLOCK);
    char *argv[] = {const_cast<char *>(childProgram), const_cast<char *>("fifo"),
                    const_cast<char *>(toPath), const_cast<char *>(fromPath), nullptr};
    const pid_t child = spawn(argv);

    int toChild = -1;
    for (int i = 0; i < 200 && toChild < 0; ++i)
    {
        toChild = open(toPath, O_WRONLY | O_NONBLOCK);
        if (toChild < 0) sleepMs(20);
    }
    check(toChild >= 0, "parent opened the writing end once the child was ready");

    write(toChild, "hello fifo\n", 11);
    check(contains(readLine(fromChild, 2000), "HELLO FIFO"), "child answered through the FIFO");

    write(toChild, "quit\n", 5);
    readLine(fromChild, 2000);
    check(childExitCode(child) == 0, "child exited with code 0 after quit");

    close(toChild); close(fromChild);
    unlink(toPath); unlink(fromPath);
}

static void testSharedMemory()
{
    printf("Shared memory\n");
    const char *name = "/occ_test_shm";
    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    check(fd >= 0, "shm_open created the object");
    if (fd < 0) return;

    ftruncate(fd, sizeof(SharedBlock));
    void *memory = mmap(nullptr, sizeof(SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    SharedBlock *block = new (memory) SharedBlock();

    char *argv[] = {const_cast<char *>(childProgram), const_cast<char *>("shm"),
                    const_cast<char *>(name), nullptr};
    const pid_t child = spawn(argv);

    uint32_t sent = 0, received = 0;
    std::vector<std::string> replies;

    auto exchange = [&](const char *text) {
        for (int i = 0; i < 300 && block->toChildAck.load() != sent; ++i) sleepMs(10);
        strncpy(block->toChild, text, kIpcMessageSize - 1);
        block->toChildSeq.store(++sent, std::memory_order_release);
        for (int i = 0; i < 300; ++i)
        {
            const uint32_t seq = block->toParentSeq.load(std::memory_order_acquire);
            if (seq != received)
            {
                replies.push_back(block->toParent);
                received = seq;
                block->toParentAck.store(seq, std::memory_order_release);
                return;
            }
            sleepMs(10);
        }
        replies.push_back("");
    };

    exchange("hello shm");
    exchange("second message");
    exchange("quit");

    check(replies.size() == 3 && contains(replies[0], "HELLO SHM"), "first message echoed through shared memory");
    check(replies.size() == 3 && contains(replies[1], "SECOND MESSAGE"), "second message echoed (stop-and-wait works)");
    check(block->childAttached.load() == 1, "child attached to the same memory");
    check(childExitCode(child) == 0, "child exited with code 0 after quit");

    munmap(memory, sizeof(SharedBlock));
    shm_unlink(name);
}

static void testMessageQueue()
{
    printf("Message queue\n");
    const char *toName = "/occ_test_mq_to";
    const char *fromName = "/occ_test_mq_from";
    mq_unlink(toName); mq_unlink(fromName);

    mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = kIpcMessageSize;

    const mqd_t toQ = mq_open(toName, O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attr);
    const mqd_t fromQ = mq_open(fromName, O_RDONLY | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attr);

    if (toQ == static_cast<mqd_t>(-1) || fromQ == static_cast<mqd_t>(-1))
    {
        printf("  [SKIP] POSIX message queues are not available here (%s)\n", strerror(errno));
        return;
    }

    char *argv[] = {const_cast<char *>(childProgram), const_cast<char *>("mq"),
                    const_cast<char *>(toName), const_cast<char *>(fromName), nullptr};
    const pid_t child = spawn(argv);
    sleepMs(300);

    kill(child, SIGSTOP);
    for (int i = 0; i < 200; ++i)
    {
        char path[64], text[512];
        snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(child));
        FILE *file = fopen(path, "r");
        if (file)
        {
            const size_t n = fread(text, 1, sizeof(text) - 1, file);
            fclose(file);
            text[n] = '\0';
            const char *paren = strrchr(text, ')');
            if (paren && paren[2] == 'T') break;
        }
        sleepMs(5);
    }

    mq_send(toQ, "low", 3, 1);
    mq_send(toQ, "high", 4, 9);
    mq_send(toQ, "medium", 6, 5);

    mq_attr current;
    mq_getattr(toQ, &current);
    check(current.mq_curmsgs == 3, "three messages wait in the kernel queue while the child is suspended");

    kill(child, SIGCONT);

    std::vector<std::string> replies;
    for (int i = 0; i < 300 && replies.size() < 3; ++i)
    {
        char buffer[kIpcMessageSize + 1];
        unsigned int priority;
        const ssize_t n = mq_receive(fromQ, buffer, kIpcMessageSize, &priority);
        if (n >= 0) { buffer[n] = '\0'; replies.push_back(buffer); }
        else sleepMs(10);
    }

    check(replies.size() == 3, "child answered all three messages");
    check(replies.size() == 3 && contains(replies[0], "(priority 9)") &&
          contains(replies[1], "(priority 5)") && contains(replies[2], "(priority 1)"),
          "messages were delivered highest priority first (9, 5, 1)");

    mq_send(toQ, "quit", 4, 31);
    check(childExitCode(child) == 0, "child exited with code 0 after quit");

    mq_close(toQ); mq_close(fromQ);
    mq_unlink(toName); mq_unlink(fromName);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <path to os-control-ipc-child>\n", argv[0]);
        return 2;
    }

    childProgram = argv[1];

    testPipe();
    testFifo();
    testSharedMemory();
    testMessageQueue();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}