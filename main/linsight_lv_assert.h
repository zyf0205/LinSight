/*
 * Included by LVGL through LV_ASSERT_HANDLER_INCLUDE. The default handler is
 * `while(1)`, which freezes the UI silently while CAN/SD keep running.
 * abort() reaches the panic handler, which prints a backtrace and reboots.
 *
 * The include path in sdkconfig is relative to lvgl's src/misc/lv_assert.h,
 * so the header resolves from every component, not only the lvgl target.
 */
#pragma once

#include <stdlib.h>

#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();
