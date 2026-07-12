// SPDX-License-Identifier: GPL-2.0
/*
 * GPU power/work trace points
 *
 * Copyright (C) 2026 Keyur Maru
 */

#include <linux/module.h>

#define CREATE_TRACE_POINTS
#include <trace/events/gpu_power.h>

EXPORT_TRACEPOINT_SYMBOL(gpu_work_period);
