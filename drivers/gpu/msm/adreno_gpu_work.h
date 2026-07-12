/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Keyur Maru
 */
#ifndef __ADRENO_GPU_WORK_H
#define __ADRENO_GPU_WORK_H

#include <linux/types.h>

/**
 * adreno_gpu_work_init - set up per-UID GPU work-period accounting
 *
 * Called once when the dispatcher is initialised.
 */
void adreno_gpu_work_init(void);

/**
 * adreno_gpu_work_close - tear down per-UID GPU work-period accounting
 *
 * Called once when the dispatcher is closed.
 */
void adreno_gpu_work_close(void);

/**
 * adreno_gpu_work_period_account - record GPU-active time for a UID
 * @uid: application UID that submitted the retired work
 * @active_ticks: GPU always-on-timer (19.2MHz) ticks the GPU spent executing
 *		  the retired command object
 *
 * Aggregates the active time into a per-UID window and emits
 * power/gpu_work_period events at window boundaries. Safe to call from the
 * dispatcher retire path.
 */
void adreno_gpu_work_period_account(uid_t uid, u64 active_ticks);

#endif /* __ADRENO_GPU_WORK_H */
