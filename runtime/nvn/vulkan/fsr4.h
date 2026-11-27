// [XCDE] Native FSR 4.1.2 Vulkan integration (Linux: auz466/fsr4up-vk; Windows: AMD FidelityFX SDK 2.4.0).
#pragma once
#include "fsr4_api.h"
#include <stdbool.h>
#if defined(__linux__) || defined(_WIN32)
extern bool fsr4_device_ok;
extern bool fsr4_fp8_ok;
bool fsr4_available(void);
bool fsr4_record(const MonadoFsr4Frame *frame);
#else
static inline bool fsr4_available(void) { return false; }
static inline bool fsr4_record(const MonadoFsr4Frame *frame) { (void)frame;return false; }
#endif
