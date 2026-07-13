// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Google LLC */
#include <linux/bpf.h>
#include <linux/dma-buf.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>

static void *dmabuf_iter_seq_start(struct seq_file *seq, loff_t *pos)
{
	if (*pos)
		return NULL;

	return dma_buf_iter_begin();
}

static void *dmabuf_iter_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct dma_buf *dmabuf = v;

	++*pos;

	return dma_buf_iter_next(dmabuf);
}

struct bpf_iter__dmabuf {
	__bpf_md_ptr(struct bpf_iter_meta *, meta);
	__bpf_md_ptr(struct dma_buf *, dmabuf);
};

DEFINE_BPF_ITER_FUNC(dma_buf, struct bpf_iter_meta *meta, struct dma_buf *dmabuf)

static int __dmabuf_seq_show(struct seq_file *seq, void *v, bool in_stop)
{
	struct bpf_iter__dmabuf ctx;
	struct bpf_iter_meta meta;
	struct bpf_prog *prog;

	meta.seq = seq;
	prog = bpf_iter_get_info(&meta, in_stop);
	if (!prog)
		return 0;

	ctx.meta = &meta;
	ctx.dmabuf = v;
	return bpf_iter_run_prog(prog, &ctx);
}

static int dmabuf_iter_seq_show(struct seq_file *seq, void *v)
{
	return __dmabuf_seq_show(seq, v, false);
}

static void dmabuf_iter_seq_stop(struct seq_file *seq, void *v)
{
	struct dma_buf *dmabuf = v;

	if (dmabuf)
		dma_buf_put(dmabuf);
}

static const struct seq_operations dmabuf_iter_seq_ops = {
	.start	= dmabuf_iter_seq_start,
	.next	= dmabuf_iter_seq_next,
	.stop	= dmabuf_iter_seq_stop,
	.show	= dmabuf_iter_seq_show,
};

static int __init dmabuf_iter_init(void)
{
	struct bpf_iter_reg reg_info = {
		.target			= "dma_buf",
		.seq_ops		= &dmabuf_iter_seq_ops,
		.init_seq_private	= NULL,
		.fini_seq_private	= NULL,
		.seq_priv_size		= 0,
	};

	return bpf_iter_reg_target(&reg_info);
}

late_initcall(dmabuf_iter_init);
