// Not libnx. Just enough of its shape, with the signatures taken verbatim
// from the real header, to type-check the __SWITCH__ paths on a machine with
// no devkitPro. Catches exactly the class of mistake that broke
// the Switch build: an argument spelled as the integer underneath an enum.
#pragma once
#include <cstddef>
#include <cstdint>

typedef uint32_t Result;
typedef int32_t s32;
typedef uint32_t u32;

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)

typedef enum { HidNpadIdType_No1 = 0, HidNpadIdType_Handheld = 0x20 } HidNpadIdType;
typedef enum {
    HidNpadStyleTag_NpadFullKey = (1U << 0),
    HidNpadStyleTag_NpadHandheld = (1U << 1),
    HidNpadStyleTag_NpadJoyDual = (1U << 2),
} HidNpadStyleTag;

typedef struct { u32 type_holder[4]; } HidSixAxisSensorHandle;
typedef struct { float x, y, z; } HidVector;
typedef struct {
    uint64_t delta_time;
    uint64_t sampling_number;
    HidVector acceleration;
    HidVector angular_velocity;
    HidVector angle;
    float direction[9];
    u32 attributes;
    u32 reserved;
} HidSixAxisSensorState;

Result hidGetSixAxisSensorHandles(HidSixAxisSensorHandle* handles, s32 total_handles,
                                  HidNpadIdType id, HidNpadStyleTag style);
Result hidStartSixAxisSensor(HidSixAxisSensorHandle handle);
Result hidStopSixAxisSensor(HidSixAxisSensorHandle handle);
size_t hidGetSixAxisSensorStates(HidSixAxisSensorHandle handle, HidSixAxisSensorState* states,
                                 size_t count);

// The applet: whether the console still wants this program running.
bool appletMainLoop(void);
typedef enum {
    AppletOperationMode_Handheld = 0,
    AppletOperationMode_Console = 1,
} AppletOperationMode;
AppletOperationMode appletGetOperationMode(void);
Result appletLockExit(void);
Result appletUnlockExit(void);

// The network the console is on.
typedef enum { NifmServiceType_User = 0 } NifmServiceType;
Result nifmInitialize(NifmServiceType service_type);
void nifmExit(void);
Result nifmGetCurrentIpConfigInfo(u32* current_addr, u32* subnet_mask, u32* gateway,
                                  u32* primary_dns_server, u32* secondary_dns_server);

// hbloader's hand-off -- how a program asks to be started again.
bool envHasNextLoad(void);
Result envSetNextLoad(const char* path, const char* argv);

// Which Switch this is -- see src/input/switch_console.hpp.
typedef enum {
    SetSysProductModel_Invalid = 0,
    SetSysProductModel_Nx = 1,
    SetSysProductModel_Copper = 2,
    SetSysProductModel_Iowa = 3,
    SetSysProductModel_Hoag = 4,
    SetSysProductModel_Calcio = 5,
    SetSysProductModel_Aula = 6,
} SetSysProductModel;
Result setsysInitialize(void);
void setsysExit(void);
Result setsysGetProductModel(SetSysProductModel* model);

// The kernel, for which cores this process may use and putting a thread on one.
typedef uint64_t u64;
typedef u32 Handle;
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define CUR_THREAD_HANDLE 0xFFFF8000
typedef enum { InfoType_CoreMask = 0 } InfoType;
Result svcGetInfo(u64* out, u32 id0, Handle handle, u64 id1);
Result svcSetThreadCoreMask(Handle handle, s32 core_id, u32 affinity_mask);
u32 svcGetCurrentProcessorNumber(void);

// The CPU clock: pcv before system version 8.0.0, clkrst from it on.
typedef enum { PcvModule_CpuBus = 0 } PcvModule;
typedef enum {
    PcvModuleId_CpuBus = 0x40000001,
    PcvModuleId_GPU = 0x40000002,
    PcvModuleId_EMC = 0x40000039,
} PcvModuleId;
Result pcvInitialize(void);
void pcvExit(void);
Result pcvGetClockRate(PcvModule module, u32* out_hz);
Result pcvSetClockRate(PcvModule module, u32 hz);
typedef struct { u32 service_holder[4]; } ClkrstSession;
Result clkrstInitialize(void);
void clkrstExit(void);
Result clkrstOpenSession(ClkrstSession* session_out, PcvModuleId module_id, u32 unk);
void clkrstCloseSession(ClkrstSession* session);
Result clkrstSetClockRate(ClkrstSession* session, u32 hz);
Result clkrstGetClockRate(ClkrstSession* session, u32* out_hz);
static inline bool hosversionBefore(u32 major, u32 minor, u32 micro) {
    (void)major; (void)minor; (void)micro;
    return false;
}

// Told when the console docks, undocks or comes back to this program -- any of
// which can put the clocks back where the system wants them.
typedef enum {
    AppletHookType_OnFocusState = 0,
    AppletHookType_OnOperationMode,
    AppletHookType_OnPerformanceMode,
    AppletHookType_OnExitRequest,
    AppletHookType_OnResume,
} AppletHookType;
typedef void (*AppletHookFn)(AppletHookType hook, void* param);
typedef struct AppletHookCookie AppletHookCookie;
struct AppletHookCookie {
    AppletHookCookie* next;
    AppletHookFn callback;
    void* param;
};
void appletHook(AppletHookCookie* cookie, AppletHookFn callback, void* param);
void appletUnhook(AppletHookCookie* cookie);
