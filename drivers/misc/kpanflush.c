// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 DeviceNexus
/*
 * kpanflush - channel bring-up debug: periodically flush the kernel log to the
 * Moto "kpan" flash partition (mmcblk0p40) so it survives a hard/cold reset that
 * wipes DRAM (ramoops). Reads the newest dmesg via kmsg_dump and writes it to
 * sector 0 of the kpan partition via a raw bio (no /dev dependency).
 *
 * Also registers a kmsg_dumper so that on a kernel PANIC we get one last flush
 * with the panic reason+backtrace, written from inside panic() BEFORE the other
 * CPUs are stopped. If the ~6.5s A17 reset is a Linux panic, kpan will hold a
 * "KPANFLUSH-PANIC" frame; if it stays "KPANFLUSH-LIVE" only, the reset is below
 * Linux (secure/hardware) and no kernel log can catch it.
 * Debug only - remove for release.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/kmsg_dump.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/err.h>

#define KPAN_DEVT     MKDEV(259, 8)     /* mmcblk0p40 = by-name/kpan */
#define KPAN_BUFBYTES (128 * 1024)      /* newest 128KB - small = fast write to catch the last lines before a reset */

static struct task_struct *kpan_task;
static struct block_device *kpan_bdev;   /* set once the eMMC partition is up */
static char *kpan_buf;                   /* periodic + panic share this (no concurrency: panic stops the kthread) */

static void kpan_write(struct block_device *bdev, char *buf, size_t len, gfp_t gfp)
{
	size_t total = (len + 511) & ~((size_t)511);
	int npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
	struct bio *bio;
	int i;

	if (total == 0 || !bdev)
		return;
	bio = bio_alloc(gfp, npages);
	if (!bio)
		return;
	bio->bi_bdev = bdev;
	bio->bi_iter.bi_sector = 0;
	for (i = 0; i < npages; i++)
		bio_add_page(bio, virt_to_page(buf + i * PAGE_SIZE),
			     PAGE_SIZE, 0);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC);
	submit_bio_wait(bio);
	bio_put(bio);
}

/*
 * kmsg_dump callback - runs inside panic()/oops before smp_send_stop(), so other
 * CPUs are still alive to service the mmc completion IRQ. Best-effort flash write
 * of the panic log with a distinct marker.
 */
static void kpan_dump(struct kmsg_dumper *dumper, enum kmsg_dump_reason reason)
{
	size_t hdr, body = 0;

	if (!kpan_bdev || !kpan_buf)
		return;
	if (reason != KMSG_DUMP_PANIC && reason != KMSG_DUMP_OOPS &&
	    reason != KMSG_DUMP_EMERG)
		return;

	memset(kpan_buf, 0, KPAN_BUFBYTES);
	hdr = scnprintf(kpan_buf, 64, "KPANFLUSH-PANIC reason=%d\n", reason);
	kmsg_dump_rewind(dumper);
	kmsg_dump_get_buffer(dumper, true, kpan_buf + hdr,
			     KPAN_BUFBYTES - hdr - 1, &body);
	kpan_write(kpan_bdev, kpan_buf, hdr + body, GFP_ATOMIC);
}

static struct kmsg_dumper kpan_dumper = {
	.dump = kpan_dump,
};

static int kpan_fn(void *data)
{
	struct kmsg_dumper dumper;
	struct block_device *bdev;

	kpan_buf = kmalloc(KPAN_BUFBYTES, GFP_KERNEL);
	if (!kpan_buf)
		return 0;

	/* wait for the eMMC partition to be probed */
	for (;;) {
		bdev = blkdev_get_by_dev(KPAN_DEVT, FMODE_WRITE, kpan_fn);
		if (!IS_ERR(bdev))
			break;
		if (kthread_should_stop()) {
			kfree(kpan_buf);
			return 0;
		}
		msleep(10);
	}
	kpan_bdev = bdev;
	pr_err("kpanflush: capturing dmesg to kpan (mmcblk0p40)\n");

	/* now that we can write, arm the panic-time dumper */
	kmsg_dump_register(&kpan_dumper);

	memset(&dumper, 0, sizeof(dumper));
	dumper.active = true;

	while (!kthread_should_stop()) {
		size_t hdr, body = 0;

		memset(kpan_buf, 0, KPAN_BUFBYTES);
		hdr = scnprintf(kpan_buf, 64, "KPANFLUSH-LIVE\n");
		kmsg_dump_rewind(&dumper);
		kmsg_dump_get_buffer(&dumper, true, kpan_buf + hdr,
				     KPAN_BUFBYTES - hdr - 1, &body);
		kpan_write(bdev, kpan_buf, hdr + body, GFP_KERNEL);
		msleep(2);
	}

	kmsg_dump_unregister(&kpan_dumper);
	blkdev_put(bdev, FMODE_WRITE);
	kfree(kpan_buf);
	return 0;
}

static int __init kpanflush_init(void)
{
	kpan_task = kthread_run(kpan_fn, NULL, "kpanflush");
	return 0;
}
/*
 * Start the kthread EARLY (subsys_initcall) so it is already spinning on
 * blkdev_get() and flushes the very first frame the instant the eMMC
 * partition appears (~3.4s) - well before init (~3.56s). late_initcall was a
 * photo-finish with the A17 init crash and lost the race every time.
 */
subsys_initcall(kpanflush_init);
