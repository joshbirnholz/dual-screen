// SPDX-License-Identifier: GPL-3.0-or-later
//
// What melonDS asks of the program around it (Platform.h): files, threads and
// locks, and the multiplayer and network hooks, which are left unimplemented
// as melonDS's own Switch frontend leaves them. Threads go through pthreads,
// so the app's own thread placement (companion/source/local_play.cpp) moves
// them off the core the emulator runs on, as it does every other thread.

#include "melonds_deko.hpp"

#include <pthread.h>
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "Config.h"
#include "Platform.h"
#include "frontend/switch/Profiler.h"

// patches/Savestate-memory-stream.patch
void SavestateUseMemory(std::FILE* file, uint8_t* data, uint32_t capacity, uint32_t size);

namespace melonds_deko {

const char* const kStateStreamName = "dualscreen:state-in-memory";

namespace {

std::string g_system_directory;
uint8_t* g_state_target = nullptr;
std::size_t g_state_target_capacity = 0;
const uint8_t* g_state_source = nullptr;
std::size_t g_state_source_size = 0;

bool is_state_stream(const char* path) {
    return path != nullptr && std::strcmp(path, kStateStreamName) == 0;
}

// A stream over the state buffer rather than a file. Savestate, patched (see
// patches/Savestate-memory-stream.patch), then reads and writes the buffer
// itself rather than through the stream, which made saving a state take over
// 20 ms -- a dropped frame every time the app wrote a resume point. The
// stream is still opened, so that anything that does go through it works.
std::FILE* open_state_stream(const char* mode) {
    if (mode != nullptr && mode[0] == 'w') {
        if (g_state_target == nullptr || g_state_target_capacity == 0) return nullptr;
        std::FILE* file = fmemopen(g_state_target, g_state_target_capacity, "wb");
        if (file != nullptr) {
            SavestateUseMemory(file, g_state_target, static_cast<uint32_t>(g_state_target_capacity), 0);
        }
        return file;
    }
    if (g_state_source == nullptr || g_state_source_size == 0) return nullptr;
    uint8_t* source = const_cast<uint8_t*>(g_state_source);
    std::FILE* file = fmemopen(source, g_state_source_size, "rb");
    if (file != nullptr) {
        SavestateUseMemory(file, source, static_cast<uint32_t>(g_state_source_size),
                           static_cast<uint32_t>(g_state_source_size));
    }
    return file;
}

}  // namespace

void state_target(uint8_t* buffer, std::size_t capacity) {
    g_state_target = buffer;
    g_state_target_capacity = capacity;
}

void state_source(const uint8_t* data, std::size_t size) {
    g_state_source = data;
    g_state_source_size = size;
}

void set_system_directory(const std::string& directory) { g_system_directory = directory; }

}  // namespace melonds_deko

// melonDS's Config.cpp reads and writes a config file through a table the
// frontend defines. This program never has it load or save one -- the settings
// that matter are set directly (see libretro.cpp) -- so the table is empty.
namespace Config {
ConfigEntry PlatformConfigFile[] = {
    {"", -1, nullptr, 0, nullptr, 0},
};
}

// Timing sections the 3D renderer marks for the port's own profiler overlay.
namespace Profiler {
void StartSection() {}
void EndSection() {}
}

namespace Platform {

void Init(int, char**) {}
void DeInit() {}
void StopEmu() {}

FILE* OpenFile(const char* path, const char* mode, bool mustexist) {
    if (melonds_deko::is_state_stream(path)) return melonds_deko::open_state_stream(mode);
    if (mustexist) {
        FILE* file = std::fopen(path, "rb");
        return file != nullptr ? std::freopen(path, mode, file) : nullptr;
    }
    return std::fopen(path, mode);
}

// A path given in full is used as it is; a bare name is looked for in the
// frontend's system directory and then where melonDS's own Switch builds keep
// BIOS and firmware.
FILE* OpenLocalFile(const char* path, const char* mode) {
    if (melonds_deko::is_state_stream(path)) return melonds_deko::open_state_stream(mode);
    if (path == nullptr || path[0] == '\0') return nullptr;
    if (path[0] == '/' || std::strchr(path, ':') != nullptr) return std::fopen(path, mode);
    for (const std::string& directory :
         {melonds_deko::g_system_directory, std::string("/switch/melonds"), std::string("/melonds")}) {
        if (directory.empty()) continue;
        const std::string candidate = directory + "/" + path;
        if (FILE* file = std::fopen(candidate.c_str(), mode)) return file;
    }
    return nullptr;
}

FILE* OpenDataFile(const char* path) { return OpenLocalFile(path, "rb"); }

void Sleep(u64 usecs) { svcSleepThread(static_cast<s64>(usecs) * 1000); }

// --- threads -----------------------------------------------------------------

struct Thread {
    pthread_t handle;
    std::function<void()> body;
};

namespace {
void* run_thread(void* raw) {
    static_cast<Thread*>(raw)->body();
    return nullptr;
}
}  // namespace

Thread* Thread_Create(std::function<void()> func) {
    Thread* thread = new Thread{pthread_t{}, std::move(func)};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // What melonDS's own Switch frontend gives its threads.
    pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);
    const int result = pthread_create(&thread->handle, &attributes, run_thread, thread);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        delete thread;
        return nullptr;
    }
    return thread;
}

void Thread_Free(Thread* thread) { delete thread; }

void Thread_Wait(Thread* thread) {
    if (thread != nullptr) pthread_join(thread->handle, nullptr);
}

// --- locks ---------------------------------------------------------------------

struct Semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int count;
};

Semaphore* Semaphore_Create() {
    Semaphore* semaphore = new Semaphore;
    pthread_mutex_init(&semaphore->mutex, nullptr);
    pthread_cond_init(&semaphore->condition, nullptr);
    semaphore->count = 0;
    return semaphore;
}

void Semaphore_Free(Semaphore* semaphore) {
    if (semaphore == nullptr) return;
    pthread_cond_destroy(&semaphore->condition);
    pthread_mutex_destroy(&semaphore->mutex);
    delete semaphore;
}

void Semaphore_Reset(Semaphore* semaphore) {
    pthread_mutex_lock(&semaphore->mutex);
    semaphore->count = 0;
    pthread_mutex_unlock(&semaphore->mutex);
}

void Semaphore_Wait(Semaphore* semaphore) {
    pthread_mutex_lock(&semaphore->mutex);
    while (semaphore->count == 0) pthread_cond_wait(&semaphore->condition, &semaphore->mutex);
    --semaphore->count;
    pthread_mutex_unlock(&semaphore->mutex);
}

void Semaphore_Post(Semaphore* semaphore, int count) {
    if (count <= 0) return;
    pthread_mutex_lock(&semaphore->mutex);
    semaphore->count += count;
    pthread_mutex_unlock(&semaphore->mutex);
    pthread_cond_broadcast(&semaphore->condition);
}

struct Mutex {
    pthread_mutex_t mutex;
};

Mutex* Mutex_Create() {
    Mutex* mutex = new Mutex;
    pthread_mutex_init(&mutex->mutex, nullptr);
    return mutex;
}

void Mutex_Free(Mutex* mutex) {
    if (mutex == nullptr) return;
    pthread_mutex_destroy(&mutex->mutex);
    delete mutex;
}

void Mutex_Lock(Mutex* mutex) { pthread_mutex_lock(&mutex->mutex); }
void Mutex_Unlock(Mutex* mutex) { pthread_mutex_unlock(&mutex->mutex); }
bool Mutex_TryLock(Mutex* mutex) { return pthread_mutex_trylock(&mutex->mutex) == 0; }

// --- local multiplayer and wifi, which this does not offer ------------------------

bool MP_Init() { return false; }
void MP_DeInit() {}
int MP_SendPacket(u8*, int) { return 0; }
int MP_RecvPacket(u8*, bool) { return 0; }
bool LAN_Init() { return false; }
void LAN_DeInit() {}
int LAN_SendPacket(u8*, int) { return 0; }
int LAN_RecvPacket(u8*) { return 0; }

}  // namespace Platform
