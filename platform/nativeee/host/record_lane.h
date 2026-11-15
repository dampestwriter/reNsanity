#pragma once
#include <cstddef>
#include <cstdint>

namespace te::gfx {

enum class RecordCall { Rigid, Placed, Skin, Blend };
bool CaptureRecord(RecordCall call, const uint32_t* args);
void DrainRecordLane();
void StopRecordLane();
bool RecordTask(void (*task)());

bool RecordFast();
void RecordFastBoundary();
uint8_t* RecordGuestPointer(uint32_t address, size_t size);
}
