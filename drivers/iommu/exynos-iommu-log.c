/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Data structure definition for Exynos IOMMU driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>

#include "exynos-iommu-log.h"

int exynos_iommu_init_event_log(struct exynos_iommu_event_log *log,
				unsigned int log_len)
{
	struct page *page;
	struct page **pages;
	int i, order;
	size_t fit_size = PAGE_ALIGN(sizeof(*(log->log)) * log_len);
	int fit_pages = fit_size / PAGE_SIZE;

	/* log_len must be power of 2 */
	BUG_ON((log_len - 1) & log_len);

	atomic_set(&log->index, 0);
	order = get_order(fit_size);
	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	split_page(page, order);
	if ((1 << order) > fit_pages) {
		int extra = (1 << order) - fit_pages;

		for (i = 0; i < extra; i++)
			__free_pages(page + fit_pages + i, 0);
	}

	pages = kmalloc(fit_pages * sizeof(*pages), GFP_KERNEL);
	if (!pages)
		goto err_kmalloc;

	for (i = 0; i < fit_pages; i++)
		pages[i] = &page[i];

	dma_sync_single_for_device(NULL, page_to_phys(page),
			fit_size, DMA_BIDIRECTIONAL);
	dma_sync_single_for_cpu(NULL, page_to_phys(page),
			fit_size, DMA_BIDIRECTIONAL);

	log->log = vmap(pages, fit_pages, VM_MAP,
			pgprot_writecombine(PAGE_KERNEL));
	if (!log->log)
		goto err_vmap;

	kfree(pages);

	log->log_len = log_len;
	log->page_log = page;

	return 0;
err_vmap:
	kfree(pages);
err_kmalloc:
	for (i = 0; i < fit_pages; i++)
		__free_pages(page + i, 0);
	return -ENOMEM;
}

static const char * const sysmmu_event_name[] = {
	"n/a", /* not an event */
	"ENABLE",
	"DISABLE",
	"TLB_INV_RANGE",
	"TLB_INV_VPN",
	"TLB_INV_ALL",
	"FLPD_FLUSH",
	"DF",
	"DF_UNLOCK",
	"DF_UNLOCK_ALL",
	"PBLMM",
	"PBSET",
	"BLOCK",
	"UNBLOCK",
#ifdef CONFIG_PM_RUNTIME
	"POWERON",
	"POWEROFF",
#endif
	"IOMMU_ATTACH",
	"IOMMU_DETACH",
	"IOMMU_MAP",
	"IOMMU_UNMAP",
	"IOMMU_ALLOCSLPD",
	"IOMMU_FREESLPD",
	"IOVMM_MAP",
	"IOVMM_UNMAP"
};

static void exynos_iommu_debug_log_show(struct seq_file *s,
					struct sysmmu_event_log *log)
{
	struct timeval tv = ktime_to_timeval(log->timestamp);

	if (log->event == EVENT_SYSMMU_NONE)
		return;

	seq_printf(s, "%06ld.%06ld: %15s", tv.tv_sec, tv.tv_usec,
			sysmmu_event_name[log->event]);

	switch (log->event) {
	case EVENT_SYSMMU_ENABLE:
	case EVENT_SYSMMU_DISABLE:
	case EVENT_SYSMMU_TLB_INV_ALL:
	case EVENT_SYSMMU_DF_UNLOCK_ALL:
	case EVENT_SYSMMU_BLOCK:
	case EVENT_SYSMMU_UNBLOCK:
#ifdef CONFIG_PM_RUNTIME
	case EVENT_SYSMMU_POWERON:
	case EVENT_SYSMMU_POWEROFF:
#endif
		seq_puts(s, "\n");
		break;
	case EVENT_SYSMMU_TLB_INV_VPN:
	case EVENT_SYSMMU_FLPD_FLUSH:
	case EVENT_SYSMMU_DF:
	case EVENT_SYSMMU_DF_UNLOCK:
	case EVENT_SYSMMU_IOMMU_ALLOCSLPD:
	case EVENT_SYSMMU_IOMMU_FREESLPD:
		seq_printf(s, " @ %#010x\n", log->eventdata.addr);
		break;
	case EVENT_SYSMMU_TLB_INV_RANGE:
	case EVENT_SYSMMU_IOMMU_UNMAP:
	case EVENT_SYSMMU_IOVMM_UNMAP:
		seq_printf(s, " @ [%#010x, %#010x)\n",
				log->eventdata.range.start,
				log->eventdata.range.end);
		break;
	case EVENT_SYSMMU_PBLMM:
		seq_printf(s, " -> LMM %u, BUF_NUM %u\n",
				log->eventdata.pblmm.lmm,
				log->eventdata.pblmm.buf_num);
		break;
	case EVENT_SYSMMU_PBSET:
		seq_printf(s, " with %#010x, [%#010x, %#010x]\n",
				log->eventdata.pbset.config,
				log->eventdata.pbset.start,
				log->eventdata.pbset.end);
		break;
	case EVENT_SYSMMU_IOVMM_MAP:
		seq_printf(s, " [%#010x, %#010x(+%#x))\n",
				log->eventdata.iovmm.start,
				log->eventdata.iovmm.end,
				log->eventdata.iovmm.dummy);
		break;
	case EVENT_SYSMMU_IOMMU_MAP:
		seq_printf(s, " [%#010x, %#010x) for PFN %#x\n",
				log->eventdata.iommu.start,
				log->eventdata.iommu.end,
				log->eventdata.iommu.pfn);
		break;
	case EVENT_SYSMMU_IOMMU_ATTACH:
	case EVENT_SYSMMU_IOMMU_DETACH:
		seq_printf(s, " of %s\n", dev_name(log->eventdata.dev));
		break;
	default:
		BUG();
	}
}

static int exynos_iommu_debugfs_log_show(struct seq_file *s, void *unused)
{
	struct exynos_iommu_event_log *plog = s->private;
	unsigned int index = atomic_read(&plog->index) % plog->log_len;
	unsigned int begin = index;

	do {
		exynos_iommu_debug_log_show(s, &plog->log[index++]);
		if (index == plog->log_len)
			index = 0;
	} while (index != begin);

	return 0;
}

static int exynos_iommu_debugfs_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_iommu_debugfs_log_show,
				inode->i_private);
}

#define SYSMMU_DENTRY_LOG_ROOT_NAME "eventlog"
static struct dentry *sysmmu_debugfs_log_root;
static struct dentry *iommu_debugfs_log_root;

static const struct file_operations exynos_iommu_debugfs_fops = {
	.open = exynos_iommu_debugfs_log_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void __sysmmu_add_log_to_debugfs(struct dentry *debugfs_root,
			struct dentry **debugfs_eventlog_root,
			struct exynos_iommu_event_log *log, const char *name)
{
	if (!debugfs_root)
		return;

	if (!(*debugfs_eventlog_root)) {
		*debugfs_eventlog_root = debugfs_create_dir(
				SYSMMU_DENTRY_LOG_ROOT_NAME, debugfs_root);
		if (!(*debugfs_eventlog_root)) {
			pr_err("%s: Failed to create 'eventlog' entry\n",
				__func__);
			return;
		}
	}

	log->debugfs_root = debugfs_create_file(name, 0400,
						*debugfs_eventlog_root, log,
						&exynos_iommu_debugfs_fops);
	if (!log->debugfs_root)
		pr_err("%s: Failed to create '%s' entry of 'eventlog'\n",
				__func__, name);
}

void sysmmu_add_log_to_debugfs(struct dentry *debugfs_root,
			struct exynos_iommu_event_log *log, const char *name)
{
	__sysmmu_add_log_to_debugfs(debugfs_root, &sysmmu_debugfs_log_root,
				log, name);
}

void iommu_add_log_to_debugfs(struct dentry *debugfs_root,
			struct exynos_iommu_event_log *log, const char *name)
{
	__sysmmu_add_log_to_debugfs(debugfs_root, &iommu_debugfs_log_root,
				log, name);
}

#if defined(CONFIG_EXYNOS_IOVMM)
static struct dentry *iovmm_debugfs_log_root;

void iovmm_add_log_to_debugfs(struct dentry *debugfs_root,
			struct exynos_iommu_event_log *log, const char *name)
{
	__sysmmu_add_log_to_debugfs(debugfs_root, &iovmm_debugfs_log_root,
				log, name);
}
#endif
