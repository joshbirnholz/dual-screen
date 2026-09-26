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
