// os-control-helper
//
// A tiny program that runs as root (started through pkexec) and does exactly
// ONE thing: send SIGTERM, SIGKILL, SIGSTOP or SIGCONT to one process.
//
// Usage:   os-control-helper signal <pid> <start-time> <TERM|KILL|STOP|CONT>
//
// It is NOT a shell and never runs other programs. Every argument is checked.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/syscall.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

namespace {

// Exit codes the GUI can understand
enum ExitCode
{
    ExitOk         = 0,
    ExitUsage      = 2,
    ExitInvalid    = 3,
    ExitProtected  = 4,
    ExitGone       = 5,
    ExitReused     = 6,
    ExitKillFailed = 7,
    ExitNotRoot    = 8
};

constexpr unsigned long long kMaxPid = 4194304ULL;   // Linux upper limit (2^22)

struct AllowedSignal
{
    const char *name;
    int number;
};

// The complete list of signals this helper is willing to send
const AllowedSignal kAllowedSignals[] = {
    {"TERM", SIGTERM},
    {"KILL", SIGKILL},
    {"STOP", SIGSTOP},
    {"CONT", SIGCONT},
};

// Accept only plain digits: no spaces, no '-', no '+', no hex, no empty text.
bool parseNumber(const char *text, unsigned long long maxValue, unsigned long long &result)
{
    if (text == nullptr || *text == '\0')
        return false;

    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value > maxValue)
        return false;

    result = value;
    return true;
}

// Read parent PID (field 4) and start time (field 22) from /proc/<pid>/stat.
// The process name is in brackets and may contain spaces, so we split after
// the LAST ')'. After it, token 0 is field 3, so ppid = token 1, start = token 19.
bool readProcessFields(pid_t pid, pid_t &parent, unsigned long long &startTime)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));

    FILE *file = fopen(path, "r");
    if (file == nullptr)
        return false;

    char buffer[8192];
    const size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);

    if (bytes == 0)
        return false;

    buffer[bytes] = '\0';

    char *closeParen = strrchr(buffer, ')');
    if (closeParen == nullptr)
        return false;

    char *save = nullptr;
    char *token = strtok_r(closeParen + 1, " \n", &save);

    for (int index = 0; token != nullptr; ++index, token = strtok_r(nullptr, " \n", &save))
    {
        if (index == 1)
            parent = static_cast<pid_t>(atoi(token));

        if (index == 19)
        {
            startTime = strtoull(token, nullptr, 10);
            return true;
        }
    }

    return false;
}

int fail(int code, const char *who, const char *fmt, const char *arg = nullptr)
{
    char message[256];
    snprintf(message, sizeof(message), fmt, arg ? arg : "");
    fprintf(stderr, "%s\n", message);
    syslog(LOG_WARNING, "requested by uid=%s: REFUSED: %s", who, message);
    return code;
}

} // namespace

int main(int argc, char *argv[])
{
    openlog("os-control-helper", LOG_PID, LOG_AUTHPRIV);

    // pkexec sets PKEXEC_UID to the user who asked. Used for the audit log only.
    const char *requester = getenv("PKEXEC_UID");
    if (requester == nullptr)
        requester = "unknown";

    if (geteuid() != 0)
    {
        fprintf(stderr, "This helper must be started through pkexec.\n");
        return ExitNotRoot;
    }

    if (argc != 5 || strcmp(argv[1], "signal") != 0)
    {
        fprintf(stderr, "Usage: os-control-helper signal <pid> <start-time> <TERM|KILL|STOP|CONT>\n");
        return ExitUsage;
    }

    // ---- 1. Validate every argument ----
    unsigned long long pidValue = 0;
    unsigned long long expectedStart = 0;

    if (!parseNumber(argv[2], kMaxPid, pidValue) || pidValue == 0)
        return fail(ExitInvalid, requester, "Invalid PID.");

    if (!parseNumber(argv[3], ~0ULL, expectedStart))
        return fail(ExitInvalid, requester, "Invalid start time.");

    int signalNumber = 0;
    for (const AllowedSignal &allowed : kAllowedSignals)
    {
        if (strcmp(argv[4], allowed.name) == 0)
            signalNumber = allowed.number;
    }

    if (signalNumber == 0)
        return fail(ExitInvalid, requester, "Signal not allowed. Only TERM, KILL, STOP, CONT.");

    const pid_t pid = static_cast<pid_t>(pidValue);

    // ---- 2. Protected targets ----
    if (pid == 1 || pid == getpid())
        return fail(ExitProtected, requester, "Refused: PID 1 and the helper itself are protected.");

    // ---- 3. Pin the process, then check it is the one the user selected ----
    // A pidfd is a handle to ONE specific process, so the PID cannot be reused
    // between our check and the signal. (Linux 5.3+; we fall back to kill().)
    const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));

    if (pidfd < 0 && errno == ESRCH)
        return fail(ExitGone, requester, "The process no longer exists.");

    pid_t parent = 0;
    unsigned long long actualStart = 0;

    if (!readProcessFields(pid, parent, actualStart))
    {
        if (pidfd >= 0)
            close(pidfd);
        return fail(ExitGone, requester, "The process no longer exists.");
    }

    if (actualStart != expectedStart)
    {
        if (pidfd >= 0)
            close(pidfd);
        return fail(ExitReused, requester, "This PID now belongs to a different process.");
    }

    if (pid == 2 || parent == 2)
    {
        if (pidfd >= 0)
            close(pidfd);
        return fail(ExitProtected, requester, "Refused: kernel threads are protected.");
    }

    // ---- 4. Send the signal ----
    int result;
    if (pidfd >= 0)
    {
        result = static_cast<int>(syscall(SYS_pidfd_send_signal, pidfd, signalNumber, nullptr, 0));
        const int savedErrno = errno;
        close(pidfd);
        errno = savedErrno;
    }
    else
    {
        result = kill(pid, signalNumber);
    }

    if (result != 0)
    {
        const int savedErrno = errno;
        char message[256];
        snprintf(message, sizeof(message), "Signal failed: %s", strerror(savedErrno));
        fprintf(stderr, "%s\n", message);
        syslog(LOG_WARNING, "requested by uid=%s: pid %d SIG%s FAILED: %s",
               requester, static_cast<int>(pid), argv[4], strerror(savedErrno));
        return ExitKillFailed;
    }

    syslog(LOG_NOTICE, "requested by uid=%s: sent SIG%s to pid %d",
           requester, argv[4], static_cast<int>(pid));
    printf("OK: SIG%s sent to PID %d\n", argv[4], static_cast<int>(pid));
    return ExitOk;
}