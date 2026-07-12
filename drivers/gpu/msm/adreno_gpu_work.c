// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-UID GPU active-time accounting for the power/gpu_work_period tracepoint.
 *
 * Copyright (C) 2026 Keyur Maru
 *
 * KGSL only tracks device-global GPU busy time, so this file adds the per-UID
 * notion the Android gpuWork.bpf program needs. Every retired command object
 * carries the GPU always-on-timer ticks at which the GPU started and finished
 * executing it (see cmdobj_profile_ticks() in adreno_dispatch.c). The retire
 * path hands that GPU-active tick delta here, tagged with the submitting
 * process' UID; we aggregate it into a bounded per-UID window and emit one
 * gpu_work_period event per UID per window.
 *
 * The tracepoint contract (see gpuWork.c in the platform tree) requires:
 *   - a non-zero period no longer than one second,
 *   - total_active_duration <= period duration,
 *   - per-UID periods that do not overlap and whose start times strictly
 *     increase.
 * Window start/end are sampled from CLOCK_MONOTONIC_RAW (ktime_get_raw_ns) as
 * the tracepoint requires; the active duration is derived from the GPU timer.
 */

#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <trace/events/gpu_power.h>

#include "adreno_gpu_work.h"

/* Single physical GPU: a fixed id is all gpuWork needs. */
#define ADRENO_GPU_WORK_GPU_ID		0

/*
 * The drawobj profiling timestamps come from the GPU always-on timer, which
 * runs at the 19.2 MHz XO rate (KGSL_XO_CLK_FREQ in kgsl_pwrctrl.h; that header
 * is not standalone-includable here, so mirror the constant).
 */
#define ADRENO_GPU_WORK_TICK_HZ		19200000U

/* Longest period the tracepoint contract allows. */
#define ADRENO_GPU_WORK_WINDOW_NS	NSEC_PER_SEC

/* Distinct UIDs tracked at once. Android rarely has more than a few dozen. */
#define ADRENO_GPU_WORK_MAX_UIDS	128

/* Cadence at which idle/partial windows are flushed. */
#define ADRENO_GPU_WORK_FLUSH_MS	1000

struct adreno_gpu_work_uid {
	bool used;
	uid_t uid;
	u64 win_start_ns;	/* window open time (CLOCK_MONOTONIC_RAW) */
	u64 win_end_ns;		/* last activity time (CLOCK_MONOTONIC_RAW) */
	u64 active_ns;		/* GPU-active ns accumulated in the window */
	u64 last_emit_end_ns;	/* end of previously emitted period for the UID */
};

static struct {
	spinlock_t lock;
	bool enabled;
	bool warned_full;
	struct adreno_gpu_work_uid uids[ADRENO_GPU_WORK_MAX_UIDS];
	struct delayed_work flush_work;
} gpu_work;

/* Find the slot tracking @uid, or claim a free/idle one. Caller holds lock. */
static struct adreno_gpu_work_uid *adreno_gpu_work_lookup(uid_t uid)
{
	struct adreno_gpu_work_uid *idle = NULL;
	int i;

	for (i = 0; i < ADRENO_GPU_WORK_MAX_UIDS; i++) {
		struct adreno_gpu_work_uid *e = &gpu_work.uids[i];

		if (e->used && e->uid == uid)
			return e;
		if (!e->used && !idle)
			idle = e;
		else if (e->used && e->active_ns == 0 && !idle)
			/* Bound to a UID for ordering only; reusable. */
			idle = e;
	}

	if (!idle) {
		if (!gpu_work.warned_full) {
			pr_warn("adreno gpu_work: all %d uid slots busy, dropping sample\n",
				ADRENO_GPU_WORK_MAX_UIDS);
			gpu_work.warned_full = true;
		}
		return NULL;
	}

	idle->used = true;
	idle->uid = uid;
	idle->win_start_ns = 0;
	idle->win_end_ns = 0;
	idle->active_ns = 0;
	idle->last_emit_end_ns = 0;
	return idle;
}

/*
 * Emit the accumulated window for @e (if it holds anything sensible) and reset
 * its accumulation, keeping the slot bound to the UID so period ordering is
 * preserved across windows. Caller holds lock.
 */
static void adreno_gpu_work_emit(struct adreno_gpu_work_uid *e)
{
	u64 start, end, active, period;

	if (!e->active_ns)
		goto reset;

	end = e->win_end_ns;
	active = e->active_ns;
	if (active > ADRENO_GPU_WORK_WINDOW_NS)
		active = ADRENO_GPU_WORK_WINDOW_NS;

	/*
	 * The period must be long enough to contain the GPU-active time and at
	 * least span the observed wall-clock activity, but never longer than one
	 * second. GPU work retires at ~end, so anchor the period at @end and
	 * extend it backwards.
	 */
	period = e->win_end_ns - e->win_start_ns;
	if (period < active)
		period = active;
	if (period > ADRENO_GPU_WORK_WINDOW_NS)
		period = ADRENO_GPU_WORK_WINDOW_NS;

	start = end - period;

	/* Keep per-UID periods non-overlapping and increasing in start time. */
	if (start < e->last_emit_end_ns)
		start = e->last_emit_end_ns;

	if (start >= end)
		goto reset;

	if (active > end - start)
		active = end - start;

	if (active) {
		trace_gpu_work_period(ADRENO_GPU_WORK_GPU_ID, e->uid,
				start, end, active);
		e->last_emit_end_ns = end;
	}

reset:
	e->win_start_ns = 0;
	e->win_end_ns = 0;
	e->active_ns = 0;
}

void adreno_gpu_work_period_account(uid_t uid, u64 active_ticks)
{
	struct adreno_gpu_work_uid *e;
	unsigned long flags;
	u64 now_ns, active_ns;

	if (!gpu_work.enabled || !active_ticks)
		return;

	/* GPU always-on timer ticks (19.2MHz) -> nanoseconds. */
	active_ns = div_u64(active_ticks * NSEC_PER_SEC, ADRENO_GPU_WORK_TICK_HZ);
	if (!active_ns)
		return;

	now_ns = ktime_get_raw_ns();

	spin_lock_irqsave(&gpu_work.lock, flags);

	e = adreno_gpu_work_lookup(uid);
	if (!e) {
		spin_unlock_irqrestore(&gpu_work.lock, flags);
		return;
	}

	if (!e->win_start_ns)
		e->win_start_ns = now_ns;
	e->win_end_ns = now_ns;
	e->active_ns += active_ns;

	/* Close the window once it has spanned about one second. */
	if (now_ns - e->win_start_ns >= ADRENO_GPU_WORK_WINDOW_NS)
		adreno_gpu_work_emit(e);

	if (!delayed_work_pending(&gpu_work.flush_work))
		schedule_delayed_work(&gpu_work.flush_work,
			msecs_to_jiffies(ADRENO_GPU_WORK_FLUSH_MS));

	spin_unlock_irqrestore(&gpu_work.lock, flags);
}

static void adreno_gpu_work_flush(struct work_struct *work)
{
	unsigned long flags;
	bool any = false;
	int i;

	spin_lock_irqsave(&gpu_work.lock, flags);

	for (i = 0; i < ADRENO_GPU_WORK_MAX_UIDS; i++) {
		struct adreno_gpu_work_uid *e = &gpu_work.uids[i];

		if (e->used && e->active_ns) {
			adreno_gpu_work_emit(e);
			any = true;
		}
	}

	/* Keep polling while there is still activity to close out. */
	if (gpu_work.enabled && any)
		schedule_delayed_work(&gpu_work.flush_work,
			msecs_to_jiffies(ADRENO_GPU_WORK_FLUSH_MS));

	spin_unlock_irqrestore(&gpu_work.lock, flags);
}

void adreno_gpu_work_init(void)
{
	spin_lock_init(&gpu_work.lock);
	memset(gpu_work.uids, 0, sizeof(gpu_work.uids));
	gpu_work.warned_full = false;
	INIT_DELAYED_WORK(&gpu_work.flush_work, adreno_gpu_work_flush);
	gpu_work.enabled = true;
}

void adreno_gpu_work_close(void)
{
	gpu_work.enabled = false;
	cancel_delayed_work_sync(&gpu_work.flush_work);
}
