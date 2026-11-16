cmake_minimum_required(VERSION 3.16)

project(OS_Control_Center VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(os_control_center
    main.cpp
    MainWindow.h
    MainWindow.cpp
    process/ProcessManager.h
    process/ProcessManager.cpp
    memory/MemoryManager.h
    memory/MemoryManager.cpp
    io/IOManager.h
    io/IOManager.cpp
    files/FileManager.h
    files/FileManager.cpp
    ipc/IPCManager.h
    ipc/IPCManager.cpp
    ipc/IPCProtocol.h
    dashboard/Dashboard.h
    dashboard/Dashboard.cpp
)

target_compile_options(os_control_center PRIVATE -Wall -Wextra)

# rt: POSIX shared memory and message queues (shm_open, mq_open)
target_link_libraries(os_control_center
    PRIVATE Qt6::Widgets rt
)

# IPC child program: the "other process" the GUI creates with fork() + execv().
# It is built next to os_control_center, in the same folder.
add_executable(os-control-ipc-child
    ipc/IPCChild.cpp
)

target_compile_options(os-control-ipc-child PRIVATE -Wall -Wextra)
target_link_libraries(os-control-ipc-child PRIVATE rt)

# Privileged helper: separate, tiny, no Qt. Installed by privileged/install.sh
add_executable(os-control-helper
    privileged/PrivilegedHelper.cpp
)

target_compile_options(os-control-helper PRIVATE -Wall -Wextra)