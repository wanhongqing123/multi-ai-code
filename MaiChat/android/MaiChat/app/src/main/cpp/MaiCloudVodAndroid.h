#pragma once

#include <cstdint>

// The handle belongs to the Java cloud-video session. The FFplay worker borrows
// it until nativeStop has joined that worker.
int maiCloudVodAndroidReadAt(void* handle, uint64_t offset,
                            uint8_t* output, int capacity);
uint64_t maiCloudVodAndroidSize(void* handle);
void maiCloudVodAndroidInterruptRead(void* handle);
