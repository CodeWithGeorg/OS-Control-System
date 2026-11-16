# OS Control Center

A desktop application for Linux that manages **real operating-system resources** through a graphical interface.
Every operation uses actual system calls. Nothing is simulated.

**Language:** C++17  **GUI:** Qt 6 Widgets  **Build:** CMake  **Platform:** Linux (developed on Kali Linux)

---

## 1. What it does

| Module | What you can do | Main system calls and APIs |
|---|---|---|
| **Dashboard** | Live system information, module cards, readiness checklist | `uname`, `/proc`, `statvfs`, `stat` |
| **Process Management** | List processes, start, stop, force stop, suspend, resume, Super Admin mode | `fork`, `execvp`, `waitpid`, `kill`, `/proc`, `pkexec` |
| **Memory Management** | Allocate, write, read, fill, clear, resize, free, inspect mappings | `malloc`, `calloc`, `realloc`, `free`, `mmap`, `mremap`, `munmap`, `memset`, `memcpy` |
| **I/O Management** | Open, read, write, seek, duplicate, close descriptors, pipes | `open`, `read`, `write`, `lseek`, `dup`, `pipe2`, `fcntl`, `ioctl`, `close` |
| **File Subsystem** | Browse, create, read, save, append, rename, delete, change permissions, properties | `opendir`, `readdir`, `lstat`, `stat`, `fstat`, `open`, `mkdir`, `rename`, `unlink`, `rmdir`, `chmod`, `access` |
| **IPC Management** | Real conversation with a child process | `pipe`, `mkfifo`, `shm_open`, `mmap`, `mq_open`, `mq_send`, `mq_receive`, `fork`, `execv` |

---

## 2. Requirements

```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev qt6-base-dev-tools git pkg-config
```

For Super Admin mode you also need PolicyKit (`pkexec`) and a graphical authentication agent
(on Kali XFCE one is normally already running).

---

## 3. Build and run

```bash
cd ~/George2_0/OS-Control-Center
cmake -S . -B build
cmake --build build
./build/os_control_center
```

The build creates **three** programs in `build/`:

| Program | Purpose |
|---|---|
| `os_control_center` | the graphical application |
| `os-control-helper` | the small privileged helper used by Super Admin mode |
| `os-control-ipc-child` | the second process used by the IPC module |

### Install Super Admin mode (once)

```bash
bash privileged/install.sh          # install
bash privileged/install.sh --uninstall   # remove again
```

This copies the helper to `/usr/local/libexec/os-control-center/` (owned by root) and installs the
PolicyKit rule. The dashboard checklist shows whether everything is in place.

### Run all automatic checks

```bash
bash tests/run_all_checks.sh
```

It builds from scratch, counts warnings, starts the application headless, tests the helper's refusal
behaviour, runs a real multi-process IPC test of all four mechanisms and confirms that nothing is left behind.

---

## 4. Architecture

```text
OS-Control-Center/
├── CMakeLists.txt
├── main.cpp                  starts Qt
├── MainWindow.h / .cpp       sidebar + stacked pages
├── dashboard/                Dashboard (live information)
├── process/                  ProcessManager
├── memory/                   MemoryManager
├── io/                       IOManager
├── files/                    FileManager
├── ipc/                      IPCManager, IPCChild (second program), IPCProtocol.h
├── privileged/               PrivilegedHelper.cpp, PolicyKit rule, install.sh
└── tests/                    run_all_checks.sh, ipc_child_test.cpp
```

Each module is one `QWidget` in its own folder. `MainWindow` only owns the navigation, so modules do not depend on each other.

### Design decisions worth explaining

* **The GUI never runs as root.** Privileged work is done by a separate helper started through
  `pkexec`. PolicyKit asks for the password; the application never sees it.
* **The helper is deliberately tiny.** It accepts exactly: `signal <pid> <start-time> <TERM|KILL|STOP|CONT>`.
  It validates every argument, refuses PID 1, kernel threads and itself, and writes an audit line to syslog.
* **PID reuse is handled.** A process is identified by PID **and** start time, and the helper pins the
  process with a `pidfd`, so a signal can never hit a different process that received the same PID.
* **The kernel enforces permissions.** The application does not guess ownership; it calls `kill()` and reports
  the real error (`EPERM`, `ESRCH`) and only then offers Super Admin mode.
* **Nothing blocks the window.** Descriptors, pipes and queues are non-blocking, and long waits use timers.
* **No leaks.** Every module releases what it created (memory, descriptors, FIFOs, shared memory, queues, child processes),
  also when the application closes.
* **Safe `fork()` from a GUI.** The IPC child is started with `fork()` followed immediately by `execv()`,
  so only simple system calls run in the forked copy of a multi-threaded program.

---

## 5. Demonstration script (about 10 minutes)

Run `./build/os_control_center` and keep a terminal beside it.

**1. Dashboard (1 min).** Show the live numbers and the readiness checklist. Click a card to jump to a module.

**2. Process Management (2 min).**

```bash
sleep 600 &
```

Select its PID. Click **Suspend** and show `ps -o pid,stat,comm -p <PID>`: state `T`. Click **Resume**, then **Stop**.
Then select a process owned by root, click **Suspend**, accept the question, enter your password in the PolicyKit dialog,
and show the audit log: `journalctl -t os-control-helper -n 5 --no-pager`.

**3. Memory Management (2 min).** Allocate 200 MiB with `mmap()`: *VmSize* rises, *VmRSS* does not. Click **Fill Block**: *VmRSS* jumps,
because the kernel only provides real RAM when pages are touched. Click **Test Allocation Failure** to show `ENOMEM`.
Use **Show in /proc/self/maps** to prove where the block lives.

**4. I/O Management (1.5 min).** Open `/tmp/occ_demo.txt`, write, seek, read. Use **dup()** and show that both descriptors share one position.
Create a pipe and use **Fill Pipe Until Full**: 65536 bytes, then `EAGAIN`. Click **Show /proc/self/fd** and compare with `ls -l /proc/<pid>/fd`.

**5. File Subsystem (1.5 min).** Create a file, type text, save, append, change permissions with the checkbox grid,
open **Properties** (stat, fstat, access). Show `EEXIST`, `EACCES` and `ENOTEMPTY` errors.

**6. IPC Management (2 min).** Start a **Message queue** channel. Click **Priority Demo**: the child is suspended with `SIGSTOP`,
three messages are queued with priorities 1, 9, 5, and the child answers **9, 5, 1**. Show `ls -l /dev/mqueue` and
`ps -o pid,ppid,stat,cmd --ppid <parent PID>`. Repeat briefly with shared memory and `ls -l /dev/shm`.

---

## 6. Troubleshooting

| Problem | Cause and fix |
|---|---|
| Dashboard shows **FIX** for the helper or the policy | Run `bash privileged/install.sh`. If the policy file is empty (0 bytes), recreate it and install again. |
| No password dialog appears | No PolicyKit agent is running. Check `ps aux \| grep -i polkit`, then install and start one (for example `policykit-1-gnome`). |
| `os-control-ipc-child` not found | Rebuild everything: `cmake --build build`. It must be in the same folder as `os_control_center`. |
| `/dev/mqueue` is empty | Only for inspection: `sudo mount -t mqueue none /dev/mqueue`. The application works without it. |
| A build error mentions `close` or `write` | Inside a `QWidget` class, call system functions as `::close(...)`, because `QWidget::close()` hides the global name. |

---

## 7. Known limits

* Linux only. Several features use Linux-specific interfaces (`/proc`, `pidfd`, `pipe2`, POSIX queues).
* Memory Management works on blocks created by the application itself. Linux does not allow modifying another process's private memory.
* Super Admin mode currently covers process signals only. It is built so further operations can be added to the helper one by one.