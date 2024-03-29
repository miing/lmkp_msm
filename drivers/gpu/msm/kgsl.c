/* Copyright (c) 2008-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/fb.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

#include <linux/vmalloc.h>
#include <linux/pm_runtime.h>
#include <linux/genlock.h>
#include <linux/rbtree.h>
#include <linux/ashmem.h>
#include <linux/major.h>
#include <linux/msm_ion.h>
#include <linux/io.h>
#include <mach/socinfo.h>
#include <linux/mman.h>

#include "kgsl.h"
#include "kgsl_debugfs.h"
#include "kgsl_cffdump.h"
#include "kgsl_log.h"
#include "kgsl_sharedmem.h"
#include "kgsl_device.h"
#include "kgsl_trace.h"
#include "kgsl_sync.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "kgsl."

static int kgsl_pagetable_count = KGSL_PAGETABLE_COUNT;
static char *ksgl_mmu_type;
module_param_named(ptcount, kgsl_pagetable_count, int, 0);
MODULE_PARM_DESC(kgsl_pagetable_count,
"Minimum number of pagetables for KGSL to allocate at initialization time");
module_param_named(mmutype, ksgl_mmu_type, charp, 0);
MODULE_PARM_DESC(ksgl_mmu_type,
"Type of MMU to be used for graphics. Valid values are 'iommu' or 'gpummu' or 'nommu'");

static struct ion_client *kgsl_ion_client;

int kgsl_memfree_hist_init(void)
{
	void *base;

	base = kzalloc(KGSL_MEMFREE_HIST_SIZE, GFP_KERNEL);
	kgsl_driver.memfree_hist.base_hist_rb = base;
	if (base == NULL)
		return -ENOMEM;
	kgsl_driver.memfree_hist.size = KGSL_MEMFREE_HIST_SIZE;
	kgsl_driver.memfree_hist.wptr = base;
	return 0;
}

void kgsl_memfree_hist_exit(void)
{
	kfree(kgsl_driver.memfree_hist.base_hist_rb);
	kgsl_driver.memfree_hist.base_hist_rb = NULL;
}

void kgsl_memfree_hist_set_event(unsigned int pid, unsigned int gpuaddr,
			unsigned int size, int flags)
{
	struct kgsl_memfree_hist_elem *p;

	void *base = kgsl_driver.memfree_hist.base_hist_rb;
	int rbsize = kgsl_driver.memfree_hist.size;

	if (base == NULL)
		return;

	mutex_lock(&kgsl_driver.memfree_hist_mutex);
	p = kgsl_driver.memfree_hist.wptr;
	p->pid = pid;
	p->gpuaddr = gpuaddr;
	p->size = size;
	p->flags = flags;

	kgsl_driver.memfree_hist.wptr++;
	if ((void *)kgsl_driver.memfree_hist.wptr >= base+rbsize) {
		kgsl_driver.memfree_hist.wptr =
			(struct kgsl_memfree_hist_elem *)base;
	}
	mutex_unlock(&kgsl_driver.memfree_hist_mutex);
}


/* kgsl_get_mem_entry - get the mem_entry structure for the specified object
 * @device - Pointer to the device structure
 * @ptbase - the pagetable base of the object
 * @gpuaddr - the GPU address of the object
 * @size - Size of the region to search
 */

struct kgsl_mem_entry *kgsl_get_mem_entry(struct kgsl_device *device,
	unsigned int ptbase, unsigned int gpuaddr, unsigned int size)
{
	struct kgsl_process_private *priv;
	struct kgsl_mem_entry *entry;

	mutex_lock(&kgsl_driver.process_mutex);

	list_for_each_entry(priv, &kgsl_driver.process_list, list) {
		if (!kgsl_mmu_pt_equal(&device->mmu, priv->pagetable, ptbase))
			continue;
		spin_lock(&priv->mem_lock);
		entry = kgsl_sharedmem_find_region(priv, gpuaddr, size);

		if (entry) {
			spin_unlock(&priv->mem_lock);
			mutex_unlock(&kgsl_driver.process_mutex);
			return entry;
		}
		spin_unlock(&priv->mem_lock);
	}
	mutex_unlock(&kgsl_driver.process_mutex);

	return NULL;
}
EXPORT_SYMBOL(kgsl_get_mem_entry);

static inline struct kgsl_mem_entry *
kgsl_mem_entry_create(void)
{
	struct kgsl_mem_entry *entry = kzalloc(sizeof(*entry), GFP_KERNEL);

	if (!entry)
		KGSL_CORE_ERR("kzalloc(%d) failed\n", sizeof(*entry));
	else
		kref_init(&entry->refcount);

	return entry;
}

void
kgsl_mem_entry_destroy(struct kref *kref)
{
	struct kgsl_mem_entry *entry = container_of(kref,
						    struct kgsl_mem_entry,
						    refcount);

	if (entry->memtype != KGSL_MEM_ENTRY_KERNEL)
		kgsl_driver.stats.mapped -= entry->memdesc.size;

	/*
	 * Ion takes care of freeing the sglist for us so
	 * clear the sg before freeing the sharedmem so kgsl_sharedmem_free
	 * doesn't try to free it again
	 */

	if (entry->memtype == KGSL_MEM_ENTRY_ION) {
		entry->memdesc.sg = NULL;
	}

	kgsl_sharedmem_free(&entry->memdesc);

	switch (entry->memtype) {
	case KGSL_MEM_ENTRY_PMEM:
	case KGSL_MEM_ENTRY_ASHMEM:
		if (entry->priv_data)
			fput(entry->priv_data);
		break;
	case KGSL_MEM_ENTRY_ION:
		ion_free(kgsl_ion_client, entry->priv_data);
		break;
	}

	kfree(entry);
}
EXPORT_SYMBOL(kgsl_mem_entry_destroy);

/**
 * kgsl_mem_entry_track_gpuaddr - Insert a mem_entry in the address tree
 * @process: the process that owns the memory
 * @entry: the memory entry
 *
 * Insert a kgsl_mem_entry in to the rb_tree for searching by GPU address.
 * Not all mem_entries will have gpu addresses when first created, so this
 * function may be called after creation when the GPU address is finally
 * assigned.
 */
static void
kgsl_mem_entry_track_gpuaddr(struct kgsl_process_private *process,
				struct kgsl_mem_entry *entry)
{
	struct rb_node **node;
	struct rb_node *parent = NULL;

	spin_lock(&process->mem_lock);

	node = &process->mem_rb.rb_node;

	while (*node) {
		struct kgsl_mem_entry *cur;

		parent = *node;
		cur = rb_entry(parent, struct kgsl_mem_entry, node);

		if (entry->memdesc.gpuaddr < cur->memdesc.gpuaddr)
			node = &parent->rb_left;
		else
			node = &parent->rb_right;
	}

	rb_link_node(&entry->node, parent, node);
	rb_insert_color(&entry->node, &process->mem_rb);

	spin_unlock(&process->mem_lock);
}

/**
 * kgsl_mem_entry_attach_process - Attach a mem_entry to its owner process
 * @entry: the memory entry
 * @process: the owner process
 *
 * Attach a newly created mem_entry to its owner process so that
 * it can be found later. The mem_entry will be added to mem_idr and have
 * its 'id' field assigned. If the GPU address has been set, the entry
 * will also be added to the mem_rb tree.
 *
 * @returns - 0 on success or error code on failure.
 */
static int
kgsl_mem_entry_attach_process(struct kgsl_mem_entry *entry,
				   struct kgsl_process_private *process)
{
	int ret;

	while (1) {
		if (idr_pre_get(&process->mem_idr, GFP_KERNEL) == 0) {
			ret = -ENOMEM;
			goto err;
		}

		spin_lock(&process->mem_lock);
		ret = idr_get_new_above(&process->mem_idr, entry, 1,
					&entry->id);
		spin_unlock(&process->mem_lock);

		if (ret == 0)
			break;
		else if (ret != -EAGAIN)
			goto err;
	}
	entry->priv = process;

	if (entry->memdesc.gpuaddr != 0)
		kgsl_mem_entry_track_gpuaddr(process, entry);
err:
	return ret;
}

/* Detach a memory entry from a process and unmap it from the MMU */

static void kgsl_mem_entry_detach_process(struct kgsl_mem_entry *entry)
{
	if (entry == NULL)
		return;

	spin_lock(&entry->priv->mem_lock);

	if (entry->id != 0)
		idr_remove(&entry->priv->mem_idr, entry->id);
	entry->id = 0;

	if (entry->memdesc.gpuaddr != 0)
		rb_erase(&entry->node, &entry->priv->mem_rb);

	spin_unlock(&entry->priv->mem_lock);

	entry->priv->stats[entry->memtype].cur -= entry->memdesc.size;
	entry->priv = NULL;

	kgsl_mmu_unmap(entry->memdesc.pagetable, &entry->memdesc);

	kgsl_mem_entry_put(entry);
}

/* Allocate a new context id */

static struct kgsl_context *
kgsl_create_context(struct kgsl_device_private *dev_priv)
{
	struct kgsl_context *context;
	int ret, id;

	context = kzalloc(sizeof(*context), GFP_KERNEL);

	if (context == NULL) {
		KGSL_DRV_INFO(dev_priv->device, "kzalloc(%d) failed\n",
				sizeof(*context));
		return ERR_PTR(-ENOMEM);
	}

	while (1) {
		if (idr_pre_get(&dev_priv->device->context_idr,
				GFP_KERNEL) == 0) {
			KGSL_DRV_INFO(dev_priv->device,
					"idr_pre_get: ENOMEM\n");
			ret = -ENOMEM;
			goto func_end;
		}

		ret = idr_get_new_above(&dev_priv->device->context_idr,
				  context, 1, &id);

		if (ret != -EAGAIN)
			break;
	}

	if (ret)
		goto func_end;

	/* MAX - 1, there is one memdesc in memstore for device info */
	if (id >= KGSL_MEMSTORE_MAX) {
		KGSL_DRV_INFO(dev_priv->device, "cannot have more than %d "
				"ctxts due to memstore limitation\n",
				KGSL_MEMSTORE_MAX);
		idr_remove(&dev_priv->device->context_idr, id);
		ret = -ENOSPC;
		goto func_end;
	}

	kref_init(&context->refcount);
	context->id = id;
	context->dev_priv = dev_priv;

	ret = kgsl_sync_timeline_create(context);
	if (ret) {
		idr_remove(&dev_priv->device->context_idr, id);
		goto func_end;
	}

	/* Initialize the pending event list */
	INIT_LIST_HEAD(&context->events);

	/*
	 * Initialize the node that is used to maintain the master list of
	 * contexts with pending events in the device structure. Normally we
	 * wouldn't take the time to initalize a node but at event add time we
	 * call list_empty() on the node as a quick way of determining if the
	 * context is already in the master list so it needs to always be either
	 * active or in an unused but initialized state
	 */

	INIT_LIST_HEAD(&context->events_list);

func_end:
	if (ret) {
		kfree(context);
		return ERR_PTR(ret);
	}

	return context;
}

/**
 * kgsl_context_detach - Release the "master" context reference
 * @context - The context that will be detached
 *
 * This is called when a context becomes unusable, because userspace
 * has requested for it to be destroyed. The context itself may
 * exist a bit longer until its reference count goes to zero.
 * Other code referencing the context can detect that it has been
 * detached because the context id will be set to KGSL_CONTEXT_INVALID.
 */
void
kgsl_context_detach(struct kgsl_context *context)
{
	int id;
	struct kgsl_device *device;
	if (context == NULL)
		return;
	device = context->dev_priv->device;
	trace_kgsl_context_detach(device, context);
	id = context->id;

	if (device->ftbl->drawctxt_destroy)
		device->ftbl->drawctxt_destroy(device, context);
	/*device specific drawctxt_destroy MUST clean up devctxt */
	BUG_ON(context->devctxt);
	/*
	 * Cancel events after the device-specific context is
	 * destroyed, to avoid possibly freeing memory while
	 * it is still in use by the GPU.
	 */
	kgsl_cancel_events_ctxt(device, context);
	idr_remove(&device->context_idr, id);
	context->id = KGSL_CONTEXT_INVALID;
	kgsl_context_put(context);
}

void
kgsl_context_destroy(struct kref *kref)
{
	struct kgsl_context *context = container_of(kref, struct kgsl_context,
						    refcount);
	kgsl_sync_timeline_destroy(context);
	kfree(context);
}

static void kgsl_check_idle_locked(struct kgsl_device *device)
{
	if (device->pwrctrl.nap_allowed == true &&
	    device->state == KGSL_STATE_ACTIVE &&
		device->requested_state == KGSL_STATE_NONE) {
		kgsl_pwrctrl_request_state(device, KGSL_STATE_NAP);
		kgsl_pwrscale_idle(device);
		if (kgsl_pwrctrl_sleep(device) != 0)
			mod_timer(&device->idle_timer,
				  jiffies +
				  device->pwrctrl.interval_timeout);
	}
}

static void kgsl_check_idle(struct kgsl_device *device)
{
	mutex_lock(&device->mutex);
	kgsl_check_idle_locked(device);
	mutex_unlock(&device->mutex);
}

struct kgsl_device *kgsl_get_device(int dev_idx)
{
	int i;
	struct kgsl_device *ret = NULL;

	mutex_lock(&kgsl_driver.devlock);

	for (i = 0; i < KGSL_DEVICE_MAX; i++) {
		if (kgsl_driver.devp[i] && kgsl_driver.devp[i]->id == dev_idx) {
			ret = kgsl_driver.devp[i];
			break;
		}
	}

	mutex_unlock(&kgsl_driver.devlock);
	return ret;
}
EXPORT_SYMBOL(kgsl_get_device);

static struct kgsl_device *kgsl_get_minor(int minor)
{
	struct kgsl_device *ret = NULL;

	if (minor < 0 || minor >= KGSL_DEVICE_MAX)
		return NULL;

	mutex_lock(&kgsl_driver.devlock);
	ret = kgsl_driver.devp[minor];
	mutex_unlock(&kgsl_driver.devlock);

	return ret;
}

int kgsl_check_timestamp(struct kgsl_device *device,
	struct kgsl_context *context, unsigned int timestamp)
{
	unsigned int ts_processed;

	ts_processed = kgsl_readtimestamp(device, context,
					  KGSL_TIMESTAMP_RETIRED);

	return (timestamp_cmp(ts_processed, timestamp) >= 0);
}
EXPORT_SYMBOL(kgsl_check_timestamp);

static int kgsl_suspend_device(struct kgsl_device *device, pm_message_t state)
{
	int status = -EINVAL;
	unsigned int nap_allowed_saved;
	struct kgsl_pwrscale_policy *policy_saved;

	if (!device)
		return -EINVAL;

	KGSL_PWR_WARN(device, "suspend start\n");

	mutex_lock(&device->mutex);
	nap_allowed_saved = device->pwrctrl.nap_allowed;
	device->pwrctrl.nap_allowed = false;
	policy_saved = device->pwrscale.policy;
	device->pwrscale.policy = NULL;
	kgsl_pwrctrl_request_state(device, KGSL_STATE_SUSPEND);
	/* Make sure no user process is waiting for a timestamp *
	 * before supending */
	if (device->active_cnt != 0) {
		mutex_unlock(&device->mutex);
		wait_for_completion(&device->suspend_gate);
		mutex_lock(&device->mutex);
	}
	/* Don't let the timer wake us during suspended sleep. */
	del_timer_sync(&device->idle_timer);
	switch (device->state) {
		case KGSL_STATE_INIT:
			break;
		case KGSL_STATE_ACTIVE:
			/* Wait for the device to become idle */
			device->ftbl->idle(device);
		case KGSL_STATE_NAP:
		case KGSL_STATE_SLEEP:
			/* Get the completion ready to be waited upon. */
			INIT_COMPLETION(device->hwaccess_gate);
			device->ftbl->suspend_context(device);
			device->ftbl->stop(device);
			pm_qos_update_request(&device->pwrctrl.pm_qos_req_dma,
						PM_QOS_DEFAULT_VALUE);
			kgsl_pwrctrl_set_state(device, KGSL_STATE_SUSPEND);
			break;
		case KGSL_STATE_SLUMBER:
			INIT_COMPLETION(device->hwaccess_gate);
			kgsl_pwrctrl_set_state(device, KGSL_STATE_SUSPEND);
			break;
		default:
			KGSL_PWR_ERR(device, "suspend fail, device %d\n",
					device->id);
			goto end;
	}
	kgsl_pwrctrl_request_state(device, KGSL_STATE_NONE);
	device->pwrctrl.nap_allowed = nap_allowed_saved;
	device->pwrscale.policy = policy_saved;
	status = 0;

end:
	mutex_unlock(&device->mutex);
	KGSL_PWR_WARN(device, "suspend end\n");
	return status;
}

static int kgsl_resume_device(struct kgsl_device *device)
{
	int status = -EINVAL;

	if (!device)
		return -EINVAL;

	KGSL_PWR_WARN(device, "resume start\n");
	mutex_lock(&device->mutex);
	if (device->state == KGSL_STATE_SUSPEND) {
		kgsl_pwrctrl_set_state(device, KGSL_STATE_SLUMBER);
		status = 0;
		complete_all(&device->hwaccess_gate);
	}
	kgsl_pwrctrl_request_state(device, KGSL_STATE_NONE);

	mutex_unlock(&device->mutex);
	KGSL_PWR_WARN(device, "resume end\n");
	return status;
}

static int kgsl_suspend(struct device *dev)
{

	pm_message_t arg = {0};
	struct kgsl_device *device = dev_get_drvdata(dev);
	return kgsl_suspend_device(device, arg);
}

static int kgsl_resume(struct device *dev)
{
	struct kgsl_device *device = dev_get_drvdata(dev);
	return kgsl_resume_device(device);
}

static int kgsl_runtime_suspend(struct device *dev)
{
	return 0;
}

static int kgsl_runtime_resume(struct device *dev)
{
	return 0;
}

const struct dev_pm_ops kgsl_pm_ops = {
	.suspend = kgsl_suspend,
	.resume = kgsl_resume,
	.runtime_suspend = kgsl_runtime_suspend,
	.runtime_resume = kgsl_runtime_resume,
};
EXPORT_SYMBOL(kgsl_pm_ops);

void kgsl_early_suspend_driver(struct early_suspend *h)
{
	struct kgsl_device *device = container_of(h,
					struct kgsl_device, display_off);
	KGSL_PWR_WARN(device, "early suspend start\n");
	mutex_lock(&device->mutex);
	device->pwrctrl.restore_slumber = true;
	kgsl_pwrctrl_request_state(device, KGSL_STATE_SLUMBER);
	kgsl_pwrctrl_sleep(device);
	mutex_unlock(&device->mutex);
	KGSL_PWR_WARN(device, "early suspend end\n");
}
EXPORT_SYMBOL(kgsl_early_suspend_driver);

int kgsl_suspend_driver(struct platform_device *pdev,
					pm_message_t state)
{
	struct kgsl_device *device = dev_get_drvdata(&pdev->dev);
	return kgsl_suspend_device(device, state);
}
EXPORT_SYMBOL(kgsl_suspend_driver);

int kgsl_resume_driver(struct platform_device *pdev)
{
	struct kgsl_device *device = dev_get_drvdata(&pdev->dev);
	return kgsl_resume_device(device);
}
EXPORT_SYMBOL(kgsl_resume_driver);

void kgsl_late_resume_driver(struct early_suspend *h)
{
	struct kgsl_device *device = container_of(h,
					struct kgsl_device, display_off);
	KGSL_PWR_WARN(device, "late resume start\n");
	mutex_lock(&device->mutex);
	device->pwrctrl.restore_slumber = false;
	if (device->pwrscale.policy == NULL)
		kgsl_pwrctrl_pwrlevel_change(device, KGSL_PWRLEVEL_TURBO);
	kgsl_pwrctrl_wake(device);
	mutex_unlock(&device->mutex);
	kgsl_check_idle(device);
	KGSL_PWR_WARN(device, "late resume end\n");
}
EXPORT_SYMBOL(kgsl_late_resume_driver);

/* file operations */
static struct kgsl_process_private *
kgsl_get_process_private(struct kgsl_device_private *cur_dev_priv)
{
	struct kgsl_process_private *private;

	mutex_lock(&kgsl_driver.process_mutex);
	list_for_each_entry(private, &kgsl_driver.process_list, list) {
		if (private->pid == task_tgid_nr(current)) {
			private->refcnt++;
			goto out;
		}
	}

	/* no existing process private found for this dev_priv, create one */
	private = kzalloc(sizeof(struct kgsl_process_private), GFP_KERNEL);
	if (private == NULL) {
		KGSL_DRV_ERR(cur_dev_priv->device, "kzalloc(%d) failed\n",
			sizeof(struct kgsl_process_private));
		goto out;
	}

	spin_lock_init(&private->mem_lock);
	private->refcnt = 1;
	private->pid = task_tgid_nr(current);
	private->mem_rb = RB_ROOT;

	idr_init(&private->mem_idr);

	if (kgsl_mmu_enabled())
	{
		unsigned long pt_name;
		struct kgsl_mmu *mmu = &cur_dev_priv->device->mmu;

		pt_name = task_tgid_nr(current);
		private->pagetable = kgsl_mmu_getpagetable(mmu, pt_name);
		if (private->pagetable == NULL) {
			kfree(private);
			private = NULL;
			goto out;
		}
	}

	list_add(&private->list, &kgsl_driver.process_list);

	kgsl_process_init_sysfs(private);
	kgsl_process_init_debugfs(private);

out:
	mutex_unlock(&kgsl_driver.process_mutex);
	return private;
}

static void
kgsl_put_process_private(struct kgsl_device *device,
			 struct kgsl_process_private *private)
{
	struct kgsl_mem_entry *entry = NULL;
	int next = 0;

	if (!private)
		return;

	mutex_lock(&kgsl_driver.process_mutex);

	if (--private->refcnt)
		goto unlock;

	kgsl_process_uninit_sysfs(private);
	debugfs_remove_recursive(private->debug_root);

	list_del(&private->list);

	while (1) {
		rcu_read_lock();
		entry = idr_get_next(&private->mem_idr, &next);
		rcu_read_unlock();
		if (entry == NULL)
			break;
		kgsl_mem_entry_detach_process(entry);
		/*
		 * Always start back at the beginning, to
		 * ensure all entries are removed,
		 * like list_for_each_entry_safe.
		 */
		next = 0;
	}
	kgsl_mmu_putpagetable(private->pagetable);
	idr_destroy(&private->mem_idr);
	kfree(private);
unlock:
	mutex_unlock(&kgsl_driver.process_mutex);
}

static int kgsl_release(struct inode *inodep, struct file *filep)
{
	int result = 0;
	struct kgsl_device_private *dev_priv = filep->private_data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_device *device = dev_priv->device;
	struct kgsl_context *context;
	int next = 0;

	filep->private_data = NULL;

	mutex_lock(&device->mutex);
	kgsl_check_suspended(device);

	while (1) {
		context = idr_get_next(&device->context_idr, &next);
		if (context == NULL)
			break;

		if (context->dev_priv == dev_priv)
			kgsl_context_detach(context);

		next = next + 1;
	}
	/*
	 * Clean up any to-be-freed entries that belong to this
	 * process and this device. This is done after the context
	 * are destroyed to avoid possibly freeing memory while
	 * it is still in use by the GPU.
	 */
	kgsl_cancel_events(device, dev_priv);

	device->open_count--;
	if (device->open_count == 0) {
		result = device->ftbl->stop(device);
		kgsl_pwrctrl_set_state(device, KGSL_STATE_INIT);
	}

	mutex_unlock(&device->mutex);
	kfree(dev_priv);

	kgsl_put_process_private(device, private);

	pm_runtime_put(device->parentdev);
	return result;
}

static int kgsl_open(struct inode *inodep, struct file *filep)
{
	int result;
	struct kgsl_device_private *dev_priv;
	struct kgsl_device *device;
	unsigned int minor = iminor(inodep);

	device = kgsl_get_minor(minor);
	BUG_ON(device == NULL);

	if (filep->f_flags & O_EXCL) {
		KGSL_DRV_ERR(device, "O_EXCL not allowed\n");
		return -EBUSY;
	}

	result = pm_runtime_get_sync(device->parentdev);
	if (result < 0) {
		KGSL_DRV_ERR(device,
			"Runtime PM: Unable to wake up the device, rc = %d\n",
			result);
		return result;
	}
	result = 0;

	dev_priv = kzalloc(sizeof(struct kgsl_device_private), GFP_KERNEL);
	if (dev_priv == NULL) {
		KGSL_DRV_ERR(device, "kzalloc failed(%d)\n",
			sizeof(struct kgsl_device_private));
		result = -ENOMEM;
		goto err_pmruntime;
	}

	dev_priv->device = device;
	filep->private_data = dev_priv;

	mutex_lock(&device->mutex);
	kgsl_check_suspended(device);

	if (device->open_count == 0) {
		kgsl_sharedmem_set(&device->memstore, 0, 0,
				device->memstore.size);

		result = device->ftbl->init(device);
		if (result)
			goto err_freedevpriv;

		result = device->ftbl->start(device);
		if (result)
			goto err_freedevpriv;

		kgsl_pwrctrl_set_state(device, KGSL_STATE_ACTIVE);
	}
	device->open_count++;
	mutex_unlock(&device->mutex);

	/*
	 * Get file (per process) private struct. This must be done
	 * after the first start so that the global pagetable mappings
	 * are set up before we create the per-process pagetable.
	 */
	dev_priv->process_priv = kgsl_get_process_private(dev_priv);
	if (dev_priv->process_priv ==  NULL) {
		result = -ENOMEM;
		goto err_stop;
	}

	KGSL_DRV_INFO(device, "Initialized %s: mmu=%s pagetable_count=%d\n",
		device->name, kgsl_mmu_enabled() ? "on" : "off",
		kgsl_pagetable_count);

	return result;

err_stop:
	mutex_lock(&device->mutex);
	device->open_count--;
	if (device->open_count == 0) {
		result = device->ftbl->stop(device);
		kgsl_pwrctrl_set_state(device, KGSL_STATE_INIT);
	}
err_freedevpriv:
	mutex_unlock(&device->mutex);
	filep->private_data = NULL;
	kfree(dev_priv);
err_pmruntime:
	pm_runtime_put(device->parentdev);
	return result;
}

/*call with private->mem_lock locked */
struct kgsl_mem_entry *
kgsl_sharedmem_find_region(struct kgsl_process_private *private,
	unsigned int gpuaddr, size_t size)
{
	struct rb_node *node = private->mem_rb.rb_node;

	if (!kgsl_mmu_gpuaddr_in_range(private->pagetable, gpuaddr))
		return NULL;

	while (node != NULL) {
		struct kgsl_mem_entry *entry;

		entry = rb_entry(node, struct kgsl_mem_entry, node);


		if (kgsl_gpuaddr_in_memdesc(&entry->memdesc, gpuaddr, size))
			return entry;

		if (gpuaddr < entry->memdesc.gpuaddr)
			node = node->rb_left;
		else if (gpuaddr >=
			(entry->memdesc.gpuaddr + entry->memdesc.size))
			node = node->rb_right;
		else {
			return NULL;
		}
	}

	return NULL;
}
EXPORT_SYMBOL(kgsl_sharedmem_find_region);

/*call with private->mem_lock locked */
static inline struct kgsl_mem_entry *
kgsl_sharedmem_find(struct kgsl_process_private *private, unsigned int gpuaddr)
{
	return kgsl_sharedmem_find_region(private, gpuaddr, 1);
}

/**
 * kgsl_sharedmem_region_empty - Check if an addression region is empty
 *
 * @private: private data for the process to check.
 * @gpuaddr: start address of the region
 * @size: length of the region.
 *
 * Checks that there are no existing allocations within an address
 * region. Note that unlike other kgsl_sharedmem* search functions,
 * this one manages locking on its own.
 */
int
kgsl_sharedmem_region_empty(struct kgsl_process_private *private,
	unsigned int gpuaddr, size_t size)
{
	int result = 1;
	unsigned int gpuaddr_end = gpuaddr + size;

	struct rb_node *node = private->mem_rb.rb_node;

	if (!kgsl_mmu_gpuaddr_in_range(private->pagetable, gpuaddr))
		return 0;

	/* don't overflow */
	if (gpuaddr_end < gpuaddr)
		return 0;

	spin_lock(&private->mem_lock);
	node = private->mem_rb.rb_node;
	while (node != NULL) {
		struct kgsl_mem_entry *entry;
		unsigned int memdesc_start, memdesc_end;

		entry = rb_entry(node, struct kgsl_mem_entry, node);

		memdesc_start = entry->memdesc.gpuaddr;
		memdesc_end = memdesc_start
				+ kgsl_memdesc_mmapsize(&entry->memdesc);

		if (gpuaddr_end <= memdesc_start)
			node = node->rb_left;
		else if (memdesc_end <= gpuaddr)
			node = node->rb_right;
		else {
			result = 0;
			break;
		}
	}
	spin_unlock(&private->mem_lock);
	return result;
}

/**
 * kgsl_sharedmem_find_id - find a memory entry by id
 * @process: the owning process
 * @id: id to find
 *
 * @returns - the mem_entry or NULL
 */
static inline struct kgsl_mem_entry *
kgsl_sharedmem_find_id(struct kgsl_process_private *process, unsigned int id)
{
	struct kgsl_mem_entry *entry;

	rcu_read_lock();
	entry = idr_find(&process->mem_idr, id);
	rcu_read_unlock();

	return entry;
}

/*call all ioctl sub functions with driver locked*/
static long kgsl_ioctl_device_getproperty(struct kgsl_device_private *dev_priv,
					  unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_device_getproperty *param = data;

	switch (param->type) {
	case KGSL_PROP_VERSION:
	{
		struct kgsl_version version;
		if (param->sizebytes != sizeof(version)) {
			result = -EINVAL;
			break;
		}

		version.drv_major = KGSL_VERSION_MAJOR;
		version.drv_minor = KGSL_VERSION_MINOR;
		version.dev_major = dev_priv->device->ver_major;
		version.dev_minor = dev_priv->device->ver_minor;

		if (copy_to_user(param->value, &version, sizeof(version)))
			result = -EFAULT;

		break;
	}
	case KGSL_PROP_GPU_RESET_STAT:
	{
		/* Return reset status of given context and clear it */
		uint32_t id;
		struct kgsl_context *context;

		if (param->sizebytes != sizeof(unsigned int)) {
			result = -EINVAL;
			break;
		}
		/* We expect the value passed in to contain the context id */
		if (copy_from_user(&id, param->value,
			sizeof(unsigned int))) {
			result = -EFAULT;
			break;
		}
		context = kgsl_find_context(dev_priv, id);
		if (!context) {
			result = -EINVAL;
			break;
		}
		/*
		 * Copy the reset status to value which also serves as
		 * the out parameter
		 */
		if (copy_to_user(param->value, &(context->reset_status),
			sizeof(unsigned int))) {
			result = -EFAULT;
			break;
		}
		/* Clear reset status once its been queried */
		context->reset_status = KGSL_CTX_STAT_NO_ERROR;
		break;
	}
	default:
		result = dev_priv->device->ftbl->getproperty(
					dev_priv->device, param->type,
					param->value, param->sizebytes);
	}


	return result;
}

static long kgsl_ioctl_device_setproperty(struct kgsl_device_private *dev_priv,
					  unsigned int cmd, void *data)
{
	int result = 0;
	/* The getproperty struct is reused for setproperty too */
	struct kgsl_device_getproperty *param = data;

	if (dev_priv->device->ftbl->setproperty)
		result = dev_priv->device->ftbl->setproperty(
			dev_priv->device, param->type,
			param->value, param->sizebytes);

	return result;
}

static long _device_waittimestamp(struct kgsl_device_private *dev_priv,
		struct kgsl_context *context,
		unsigned int timestamp,
		unsigned int timeout)
{
	int result = 0;
	struct kgsl_device *device = dev_priv->device;
	unsigned int context_id = context ? context->id : KGSL_MEMSTORE_GLOBAL;

	/* Set the active count so that suspend doesn't do the wrong thing */

	device->active_cnt++;

	trace_kgsl_waittimestamp_entry(device, context_id,
				       kgsl_readtimestamp(device, context,
							KGSL_TIMESTAMP_RETIRED),
				       timestamp, timeout);

	result = device->ftbl->waittimestamp(dev_priv->device,
					context, timestamp, timeout);

	trace_kgsl_waittimestamp_exit(device,
				      kgsl_readtimestamp(device, context,
							KGSL_TIMESTAMP_RETIRED),
				      result);

	/* Fire off any pending suspend operations that are in flight */
	kgsl_active_count_put(dev_priv->device);

	return result;
}

static long kgsl_ioctl_device_waittimestamp(struct kgsl_device_private
						*dev_priv, unsigned int cmd,
						void *data)
{
	struct kgsl_device_waittimestamp *param = data;

	return _device_waittimestamp(dev_priv, NULL,
			param->timestamp, param->timeout);
}

static long kgsl_ioctl_device_waittimestamp_ctxtid(struct kgsl_device_private
						*dev_priv, unsigned int cmd,
						void *data)
{
	struct kgsl_device_waittimestamp_ctxtid *param = data;
	struct kgsl_context *context;
	int result;

	context = kgsl_find_context(dev_priv, param->context_id);
	if (context == NULL)
		return -EINVAL;
	/*
	 * A reference count is needed here, because waittimestamp may
	 * block with the device mutex unlocked and userspace could
	 * request for the context to be destroyed during that time.
	 */
	kgsl_context_get(context);
	result = _device_waittimestamp(dev_priv, context,
			param->timestamp, param->timeout);
	kgsl_context_put(context);
	return result;
}

static long kgsl_ioctl_rb_issueibcmds(struct kgsl_device_private *dev_priv,
				      unsigned int cmd, void *data)
{
	int result = 0;
	int i = 0;
	struct kgsl_ringbuffer_issueibcmds *param = data;
	struct kgsl_ibdesc *ibdesc;
	struct kgsl_context *context;

	context = kgsl_find_context(dev_priv, param->drawctxt_id);
	if (context == NULL) {
		result = -EINVAL;
		goto done;
	}

	if (param->flags & KGSL_CONTEXT_SUBMIT_IB_LIST) {
		if (!param->numibs) {
			result = -EINVAL;
			goto done;
		}

		/*
		 * Put a reasonable upper limit on the number of IBs that can be
		 * submitted
		 */

		if (param->numibs > 10000) {
			result = -EINVAL;
			goto done;
		}

		ibdesc = kzalloc(sizeof(struct kgsl_ibdesc) * param->numibs,
					GFP_KERNEL);
		if (!ibdesc) {
			KGSL_MEM_ERR(dev_priv->device,
				"kzalloc(%d) failed\n",
				sizeof(struct kgsl_ibdesc) * param->numibs);
			result = -ENOMEM;
			goto done;
		}

		if (copy_from_user(ibdesc, (void *)param->ibdesc_addr,
				sizeof(struct kgsl_ibdesc) * param->numibs)) {
			result = -EFAULT;
			KGSL_DRV_ERR(dev_priv->device,
				"copy_from_user failed\n");
			goto free_ibdesc;
		}
	} else {
		KGSL_DRV_INFO(dev_priv->device,
			"Using single IB submission mode for ib submission\n");
		/* If user space driver is still using the old mode of
		 * submitting single ib then we need to support that as well */
		ibdesc = kzalloc(sizeof(struct kgsl_ibdesc), GFP_KERNEL);
		if (!ibdesc) {
			KGSL_MEM_ERR(dev_priv->device,
				"kzalloc(%d) failed\n",
				sizeof(struct kgsl_ibdesc));
			result = -ENOMEM;
			goto done;
		}
		ibdesc[0].gpuaddr = param->ibdesc_addr;
		ibdesc[0].sizedwords = param->numibs;
		param->numibs = 1;
	}

	for (i = 0; i < param->numibs; i++) {
		struct kgsl_pagetable *pt = dev_priv->process_priv->pagetable;

		if (!kgsl_mmu_gpuaddr_in_range(pt, ibdesc[i].gpuaddr)) {
			result = -ERANGE;
			KGSL_DRV_ERR(dev_priv->device,
				     "invalid ib base GPU virtual addr %x\n",
				     ibdesc[i].gpuaddr);
			goto free_ibdesc;
		}
	}

	result = dev_priv->device->ftbl->issueibcmds(dev_priv,
					     context,
					     ibdesc,
					     param->numibs,
					     &param->timestamp,
					     param->flags);

free_ibdesc:
	kfree(ibdesc);
done:

	return result;
}

static long _cmdstream_readtimestamp(struct kgsl_device_private *dev_priv,
		struct kgsl_context *context, unsigned int type,
		unsigned int *timestamp)
{
	*timestamp = kgsl_readtimestamp(dev_priv->device, context, type);

	trace_kgsl_readtimestamp(dev_priv->device,
			context ? context->id : KGSL_MEMSTORE_GLOBAL,
			type, *timestamp);

	return 0;
}

static long kgsl_ioctl_cmdstream_readtimestamp(struct kgsl_device_private
						*dev_priv, unsigned int cmd,
						void *data)
{
	struct kgsl_cmdstream_readtimestamp *param = data;

	return _cmdstream_readtimestamp(dev_priv, NULL,
			param->type, &param->timestamp);
}

static long kgsl_ioctl_cmdstream_readtimestamp_ctxtid(struct kgsl_device_private
						*dev_priv, unsigned int cmd,
						void *data)
{
	struct kgsl_cmdstream_readtimestamp_ctxtid *param = data;
	struct kgsl_context *context;

	context = kgsl_find_context(dev_priv, param->context_id);
	if (context == NULL)
		return -EINVAL;

	return _cmdstream_readtimestamp(dev_priv, context,
			param->type, &param->timestamp);
}

static void kgsl_freemem_event_cb(struct kgsl_device *device,
	void *priv, u32 id, u32 timestamp)
{
	struct kgsl_mem_entry *entry = priv;
	trace_kgsl_mem_timestamp_free(device, entry, id, timestamp, 0);
	kgsl_mem_entry_detach_process(entry);
}

static long _cmdstream_freememontimestamp(struct kgsl_device_private *dev_priv,
		unsigned int gpuaddr, struct kgsl_context *context,
		unsigned int timestamp, unsigned int type)
{
	int result = 0;
	struct kgsl_mem_entry *entry = NULL;
	struct kgsl_device *device = dev_priv->device;
	unsigned int context_id = context ? context->id : KGSL_MEMSTORE_GLOBAL;

	spin_lock(&dev_priv->process_priv->mem_lock);
	entry = kgsl_sharedmem_find(dev_priv->process_priv, gpuaddr);
	spin_unlock(&dev_priv->process_priv->mem_lock);

	if (!entry) {
		KGSL_DRV_ERR(dev_priv->device,
				"invalid gpuaddr %08x\n", gpuaddr);
		result = -EINVAL;
		goto done;
	}
	trace_kgsl_mem_timestamp_queue(device, entry, context_id,
				       kgsl_readtimestamp(device, context,
						  KGSL_TIMESTAMP_RETIRED),
				       timestamp);
	result = kgsl_add_event(dev_priv->device, context_id, timestamp,
				kgsl_freemem_event_cb, entry, dev_priv);
done:
	return result;
}

static long kgsl_ioctl_cmdstream_freememontimestamp(struct kgsl_device_private
						    *dev_priv, unsigned int cmd,
						    void *data)
{
	struct kgsl_cmdstream_freememontimestamp *param = data;

	return _cmdstream_freememontimestamp(dev_priv, param->gpuaddr,
			NULL, param->timestamp, param->type);
}

static long kgsl_ioctl_cmdstream_freememontimestamp_ctxtid(
						struct kgsl_device_private
						*dev_priv, unsigned int cmd,
						void *data)
{
	struct kgsl_cmdstream_freememontimestamp_ctxtid *param = data;
	struct kgsl_context *context;

	context = kgsl_find_context(dev_priv, param->context_id);
	if (context == NULL)
		return -EINVAL;

	return _cmdstream_freememontimestamp(dev_priv, param->gpuaddr,
			context, param->timestamp, param->type);
}

static long kgsl_ioctl_drawctxt_create(struct kgsl_device_private *dev_priv,
					unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_drawctxt_create *param = data;
	struct kgsl_context *context = NULL;

	context = kgsl_create_context(dev_priv);

	if (IS_ERR(context)) {
		result = PTR_ERR(context);
		goto done;
	}

	if (dev_priv->device->ftbl->drawctxt_create) {
		result = dev_priv->device->ftbl->drawctxt_create(
			dev_priv->device, dev_priv->process_priv->pagetable,
			context, &param->flags);
		if (result)
			goto done;
	}
	trace_kgsl_context_create(dev_priv->device, context, param->flags);
	param->drawctxt_id = context->id;
done:
	if (result && !IS_ERR(context))
		kgsl_context_detach(context);

	return result;
}

static long kgsl_ioctl_drawctxt_destroy(struct kgsl_device_private *dev_priv,
					unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_drawctxt_destroy *param = data;
	struct kgsl_context *context;

	context = kgsl_find_context(dev_priv, param->drawctxt_id);

	if (context == NULL) {
		result = -EINVAL;
		goto done;
	}

	kgsl_context_detach(context);
done:
	return result;
}

static long kgsl_ioctl_sharedmem_free(struct kgsl_device_private *dev_priv,
					unsigned int cmd, void *data)
{
	struct kgsl_sharedmem_free *param = data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;

	spin_lock(&private->mem_lock);
	entry = kgsl_sharedmem_find(private, param->gpuaddr);
	spin_unlock(&private->mem_lock);

	if (!entry) {
		KGSL_MEM_INFO(dev_priv->device, "invalid gpuaddr %08x\n",
				param->gpuaddr);
		return -EINVAL;
	}
	trace_kgsl_mem_free(entry);

	kgsl_memfree_hist_set_event(entry->priv->pid,
				    entry->memdesc.gpuaddr,
				    entry->memdesc.size,
				    entry->memdesc.flags);

	kgsl_mem_entry_detach_process(entry);
	return 0;
}

static long kgsl_ioctl_gpumem_free_id(struct kgsl_device_private *dev_priv,
					unsigned int cmd, void *data)
{
	struct kgsl_gpumem_free_id *param = data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;

	entry = kgsl_sharedmem_find_id(private, param->id);

	if (!entry) {
		KGSL_MEM_INFO(dev_priv->device, "invalid id %d\n", param->id);
		return -EINVAL;
	}
	trace_kgsl_mem_free(entry);

	kgsl_memfree_hist_set_event(entry->priv->pid,
				    entry->memdesc.gpuaddr,
				    entry->memdesc.size,
				    entry->memdesc.flags);

	kgsl_mem_entry_detach_process(entry);
	return 0;
}

static struct vm_area_struct *kgsl_get_vma_from_start_addr(unsigned int addr)
{
	struct vm_area_struct *vma;

	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, addr);
	up_read(&current->mm->mmap_sem);
	if (!vma)
		KGSL_CORE_ERR("find_vma(%x) failed\n", addr);

	return vma;
}

static inline int _check_region(unsigned long start, unsigned long size,
				uint64_t len)
{
	uint64_t end = ((uint64_t) start) + size;
	return (end > len);
}

static int kgsl_get_phys_file(int fd, unsigned long *start, unsigned long *len,
			      unsigned long *vstart, struct file **filep)
{
	struct file *fbfile;
	int ret = 0;
	dev_t rdev;
	struct fb_info *info;

	*start = 0;
	*vstart = 0;
	*len = 0;
	*filep = NULL;

	fbfile = fget(fd);
	if (fbfile == NULL) {
		KGSL_CORE_ERR("fget_light failed\n");
		return -1;
	}

	rdev = fbfile->f_dentry->d_inode->i_rdev;
	info = MAJOR(rdev) == FB_MAJOR ? registered_fb[MINOR(rdev)] : NULL;
	if (info) {
		*start = info->fix.smem_start;
		*len = info->fix.smem_len;
		*vstart = (unsigned long)__va(info->fix.smem_start);
		ret = 0;
	} else {
		KGSL_CORE_ERR("framebuffer minor %d not found\n",
			      MINOR(rdev));
		ret = -1;
	}

	fput(fbfile);

	return ret;
}

static int kgsl_setup_phys_file(struct kgsl_mem_entry *entry,
				struct kgsl_pagetable *pagetable,
				unsigned int fd, unsigned int offset,
				size_t size)
{
	int ret;
	unsigned long phys, virt, len;
	struct file *filep;

	ret = kgsl_get_phys_file(fd, &phys, &len, &virt, &filep);
	if (ret)
		return ret;

	ret = -ERANGE;

	if (phys == 0)
		goto err;

	/* Make sure the length of the region, the offset and the desired
	 * size are all page aligned or bail
	 */
	if ((len & ~PAGE_MASK) ||
		(offset & ~PAGE_MASK) ||
		(size & ~PAGE_MASK)) {
		KGSL_CORE_ERR("length offset or size is not page aligned\n");
		goto err;
	}

	/* The size or offset can never be greater than the PMEM length */
	if (offset >= len || size > len)
		goto err;

	/* If size is 0, then adjust it to default to the size of the region
	 * minus the offset.  If size isn't zero, then make sure that it will
	 * fit inside of the region.
	 */
	if (size == 0)
		size = len - offset;

	else if (_check_region(offset, size, len))
		goto err;

	entry->priv_data = filep;

	entry->memdesc.pagetable = pagetable;
	entry->memdesc.size = size;
	entry->memdesc.physaddr = phys + offset;
	entry->memdesc.hostptr = (void *) (virt + offset);
	/* USE_CPU_MAP is not impemented for PMEM. */
	entry->memdesc.flags &= ~KGSL_MEMFLAGS_USE_CPU_MAP;

	ret = memdesc_sg_phys(&entry->memdesc, phys + offset, size);
	if (ret)
		goto err;

	return 0;
err:
	return ret;
}

static int memdesc_sg_virt(struct kgsl_memdesc *memdesc,
	unsigned long paddr, int size)
{
	int i;
	int sglen = PAGE_ALIGN(size) / PAGE_SIZE;

	memdesc->sg = kgsl_sg_alloc(sglen);

	if (memdesc->sg == NULL)
		return -ENOMEM;

	memdesc->sglen = sglen;
	memdesc->sglen_alloc = sglen;

	sg_init_table(memdesc->sg, sglen);

	spin_lock(&current->mm->page_table_lock);

	for (i = 0; i < sglen; i++, paddr += PAGE_SIZE) {
		struct page *page;
		pmd_t *ppmd;
		pte_t *ppte;
		pgd_t *ppgd = pgd_offset(current->mm, paddr);

		if (pgd_none(*ppgd) || pgd_bad(*ppgd))
			goto err;

		ppmd = pmd_offset(pud_offset(ppgd, paddr), paddr);
		if (pmd_none(*ppmd) || pmd_bad(*ppmd))
			goto err;

		ppte = pte_offset_map(ppmd, paddr);
		if (ppte == NULL)
			goto err;

		page = pfn_to_page(pte_pfn(*ppte));
		if (!page)
			goto err;

		sg_set_page(&memdesc->sg[i], page, PAGE_SIZE, 0);
		pte_unmap(ppte);
	}

	spin_unlock(&current->mm->page_table_lock);

	return 0;

err:
	spin_unlock(&current->mm->page_table_lock);
	kgsl_sg_free(memdesc->sg,  sglen);
	memdesc->sg = NULL;

	return -EINVAL;
}

static int kgsl_setup_useraddr(struct kgsl_mem_entry *entry,
			      struct kgsl_pagetable *pagetable,
			      unsigned long useraddr, unsigned int offset,
			      size_t size)
{
	struct vm_area_struct *vma;
	unsigned int len;

	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, useraddr);
	up_read(&current->mm->mmap_sem);

	if (!vma) {
		KGSL_CORE_ERR("find_vma(%lx) failed\n", useraddr);
		return -EINVAL;
	}

	/* We don't necessarily start at vma->vm_start */
	len = vma->vm_end - useraddr;

	if (offset >= len)
		return -EINVAL;

	if (!KGSL_IS_PAGE_ALIGNED(useraddr) ||
	    !KGSL_IS_PAGE_ALIGNED(len)) {
		KGSL_CORE_ERR("bad alignment: start(%lx) len(%u)\n",
			      useraddr, len);
		return -EINVAL;
	}

	if (size == 0)
		size = len;

	/* Adjust the size of the region to account for the offset */
	size += offset & ~PAGE_MASK;

	size = ALIGN(size, PAGE_SIZE);

	if (_check_region(offset & PAGE_MASK, size, len)) {
		KGSL_CORE_ERR("Offset (%ld) + size (%d) is larger"
			      "than region length %d\n",
			      offset & PAGE_MASK, size, len);
		return -EINVAL;
	}

	entry->memdesc.pagetable = pagetable;
	entry->memdesc.size = size;
	entry->memdesc.useraddr = useraddr + (offset & PAGE_MASK);
	if (kgsl_memdesc_use_cpu_map(&entry->memdesc))
		entry->memdesc.gpuaddr = entry->memdesc.useraddr;

	return memdesc_sg_virt(&entry->memdesc, entry->memdesc.useraddr,
				size);
}

#ifdef CONFIG_ASHMEM
static int kgsl_setup_ashmem(struct kgsl_mem_entry *entry,
			     struct kgsl_pagetable *pagetable,
			     int fd, unsigned long useraddr, size_t size)
{
	int ret;
	struct vm_area_struct *vma;
	struct file *filep, *vmfile;
	unsigned long len;

	vma = kgsl_get_vma_from_start_addr(useraddr);
	if (vma == NULL)
		return -EINVAL;

	if (vma->vm_pgoff || vma->vm_start != useraddr) {
		KGSL_CORE_ERR("Invalid vma region\n");
		return -EINVAL;
	}

	len = vma->vm_end - vma->vm_start;

	if (size == 0)
		size = len;

	if (size != len) {
		KGSL_CORE_ERR("Invalid size %d for vma region %lx\n",
			      size, useraddr);
		return -EINVAL;
	}

	ret = get_ashmem_file(fd, &filep, &vmfile, &len);

	if (ret) {
		KGSL_CORE_ERR("get_ashmem_file failed\n");
		return ret;
	}

	if (vmfile != vma->vm_file) {
		KGSL_CORE_ERR("ashmem shmem file does not match vma\n");
		ret = -EINVAL;
		goto err;
	}

	entry->priv_data = filep;
	entry->memdesc.pagetable = pagetable;
	entry->memdesc.size = ALIGN(size, PAGE_SIZE);
	entry->memdesc.useraddr = useraddr;
	if (kgsl_memdesc_use_cpu_map(&entry->memdesc))
		entry->memdesc.gpuaddr = entry->memdesc.useraddr;

	ret = memdesc_sg_virt(&entry->memdesc, useraddr, size);
	if (ret)
		goto err;

	return 0;

err:
	put_ashmem_file(filep);
	return ret;
}
#else
static int kgsl_setup_ashmem(struct kgsl_mem_entry *entry,
			     struct kgsl_pagetable *pagetable,
			     int fd, unsigned long useraddr, size_t size)
{
	return -EINVAL;
}
#endif

static int kgsl_setup_ion(struct kgsl_mem_entry *entry,
		struct kgsl_pagetable *pagetable, void *data)
{
	struct ion_handle *handle;
	struct scatterlist *s;
	struct sg_table *sg_table;
	struct kgsl_map_user_mem *param = data;
	int fd = param->fd;

	if (!param->len)
		return -EINVAL;

	if (IS_ERR_OR_NULL(kgsl_ion_client))
		return -ENODEV;

	handle = ion_import_dma_buf(kgsl_ion_client, fd);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	else if (!handle)
		return -EINVAL;

	entry->memtype = KGSL_MEM_ENTRY_ION;
	entry->priv_data = handle;
	entry->memdesc.pagetable = pagetable;
	entry->memdesc.size = 0;
	/* USE_CPU_MAP is not impemented for ION. */
	entry->memdesc.flags &= ~KGSL_MEMFLAGS_USE_CPU_MAP;

	sg_table = ion_sg_table(kgsl_ion_client, handle);

	if (IS_ERR_OR_NULL(sg_table))
		goto err;

	entry->memdesc.sg = sg_table->sgl;

	/* Calculate the size of the memdesc from the sglist */

	entry->memdesc.sglen = 0;

	for (s = entry->memdesc.sg; s != NULL; s = sg_next(s)) {
		entry->memdesc.size += s->length;
		entry->memdesc.sglen++;
	}

	entry->memdesc.size = PAGE_ALIGN(entry->memdesc.size);

	return 0;
err:
	ion_free(kgsl_ion_client, handle);
	return -ENOMEM;
}

static long kgsl_ioctl_map_user_mem(struct kgsl_device_private *dev_priv,
				     unsigned int cmd, void *data)
{
	int result = -EINVAL;
	struct kgsl_map_user_mem *param = data;
	struct kgsl_mem_entry *entry = NULL;
	struct kgsl_process_private *private = dev_priv->process_priv;
	enum kgsl_user_mem_type memtype;

	entry = kgsl_mem_entry_create();

	if (entry == NULL)
		return -ENOMEM;

	if (_IOC_SIZE(cmd) == sizeof(struct kgsl_sharedmem_from_pmem))
		memtype = KGSL_USER_MEM_TYPE_PMEM;
	else
		memtype = param->memtype;

	/*
	 * Mask off unknown flags from userspace. This way the caller can
	 * check if a flag is supported by looking at the returned flags.
	 * Note: CACHEMODE is ignored for this call. Caching should be
	 * determined by type of allocation being mapped.
	 */
	param->flags &= KGSL_MEMFLAGS_GPUREADONLY
			| KGSL_MEMTYPE_MASK
			| KGSL_MEMALIGN_MASK
			| KGSL_MEMFLAGS_USE_CPU_MAP;

	entry->memdesc.flags = param->flags;
	if (!kgsl_mmu_use_cpu_map(private->pagetable->mmu))
		entry->memdesc.flags &= ~KGSL_MEMFLAGS_USE_CPU_MAP;

	switch (memtype) {
	case KGSL_USER_MEM_TYPE_PMEM:
		if (param->fd == 0 || param->len == 0)
			break;

		result = kgsl_setup_phys_file(entry, private->pagetable,
					      param->fd, param->offset,
					      param->len);
		entry->memtype = KGSL_MEM_ENTRY_PMEM;
		break;

	case KGSL_USER_MEM_TYPE_ADDR:
		KGSL_DEV_ERR_ONCE(dev_priv->device, "User mem type "
				"KGSL_USER_MEM_TYPE_ADDR is deprecated\n");
		if (!kgsl_mmu_enabled()) {
			KGSL_DRV_ERR(dev_priv->device,
				"Cannot map paged memory with the "
				"MMU disabled\n");
			break;
		}

		if (param->hostptr == 0)
			break;

		result = kgsl_setup_useraddr(entry, private->pagetable,
					    param->hostptr,
					    param->offset, param->len);
		entry->memtype = KGSL_MEM_ENTRY_USER;
		break;

	case KGSL_USER_MEM_TYPE_ASHMEM:
		if (!kgsl_mmu_enabled()) {
			KGSL_DRV_ERR(dev_priv->device,
				"Cannot map paged memory with the "
				"MMU disabled\n");
			break;
		}

		if (param->hostptr == 0)
			break;

		result = kgsl_setup_ashmem(entry, private->pagetable,
					   param->fd, param->hostptr,
					   param->len);

		entry->memtype = KGSL_MEM_ENTRY_ASHMEM;
		break;
	case KGSL_USER_MEM_TYPE_ION:
		result = kgsl_setup_ion(entry, private->pagetable, data);
		break;
	default:
		KGSL_CORE_ERR("Invalid memory type: %x\n", memtype);
		break;
	}

	if (result)
		goto error;

	if (entry->memdesc.size >= SZ_1M)
		kgsl_memdesc_set_align(&entry->memdesc, ilog2(SZ_1M));
	else if (entry->memdesc.size >= SZ_64K)
		kgsl_memdesc_set_align(&entry->memdesc, ilog2(SZ_64));

	result = kgsl_mmu_map(private->pagetable, &entry->memdesc);
	if (result)
		goto error_put_file_ptr;

	/* Adjust the returned value for a non 4k aligned offset */
	param->gpuaddr = entry->memdesc.gpuaddr + (param->offset & ~PAGE_MASK);
	/* echo back flags */
	param->flags = entry->memdesc.flags;

	result = kgsl_mem_entry_attach_process(entry, private);
	if (result)
		goto error_unmap;

	KGSL_STATS_ADD(param->len, kgsl_driver.stats.mapped,
		kgsl_driver.stats.mapped_max);

	kgsl_process_add_stats(private, entry->memtype, param->len);

	trace_kgsl_mem_map(entry, param->fd);

	kgsl_check_idle(dev_priv->device);
	return result;

error_unmap:
	kgsl_mmu_unmap(private->pagetable, &entry->memdesc);
error_put_file_ptr:
	switch (entry->memtype) {
	case KGSL_MEM_ENTRY_PMEM:
	case KGSL_MEM_ENTRY_ASHMEM:
		if (entry->priv_data)
			fput(entry->priv_data);
		break;
	case KGSL_MEM_ENTRY_ION:
		ion_free(kgsl_ion_client, entry->priv_data);
		break;
	default:
		break;
	}
error:
	kfree(entry);
	kgsl_check_idle(dev_priv->device);
	return result;
}

static int _kgsl_gpumem_sync_cache(struct kgsl_mem_entry *entry, int op)
{
	int ret = 0;
	int cacheop;
	int mode;

	/*
	 * Flush is defined as (clean | invalidate).  If both bits are set, then
	 * do a flush, otherwise check for the individual bits and clean or inv
	 * as requested
	 */

	if ((op & KGSL_GPUMEM_CACHE_FLUSH) == KGSL_GPUMEM_CACHE_FLUSH)
		cacheop = KGSL_CACHE_OP_FLUSH;
	else if (op & KGSL_GPUMEM_CACHE_CLEAN)
		cacheop = KGSL_CACHE_OP_CLEAN;
	else if (op & KGSL_GPUMEM_CACHE_INV)
		cacheop = KGSL_CACHE_OP_INV;
	else {
		ret = -EINVAL;
		goto done;
	}

	mode = kgsl_memdesc_get_cachemode(&entry->memdesc);
	if (mode != KGSL_CACHEMODE_UNCACHED
		&& mode != KGSL_CACHEMODE_WRITECOMBINE) {
		trace_kgsl_mem_sync_cache(entry, op);
		kgsl_cache_range_op(&entry->memdesc, cacheop);
	}

done:
	return ret;
}

/* New cache sync function - supports both directions (clean and invalidate) */

static long
kgsl_ioctl_gpumem_sync_cache(struct kgsl_device_private *dev_priv,
	unsigned int cmd, void *data)
{
	struct kgsl_gpumem_sync_cache *param = data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;

	if (param->id != 0) {
		entry = kgsl_sharedmem_find_id(private, param->id);
		if (entry == NULL) {
			KGSL_MEM_INFO(dev_priv->device, "can't find id %d\n",
					param->id);
			return -EINVAL;
		}
	} else if (param->gpuaddr != 0) {
		spin_lock(&private->mem_lock);
		entry = kgsl_sharedmem_find(private, param->gpuaddr);
		spin_unlock(&private->mem_lock);
		if (entry == NULL) {
			KGSL_MEM_INFO(dev_priv->device,
					"can't find gpuaddr %x\n",
					param->gpuaddr);
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	return _kgsl_gpumem_sync_cache(entry, param->op);
}

/* Legacy cache function, does a flush (clean  + invalidate) */

static long
kgsl_ioctl_sharedmem_flush_cache(struct kgsl_device_private *dev_priv,
				 unsigned int cmd, void *data)
{
	struct kgsl_sharedmem_free *param = data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;

	spin_lock(&private->mem_lock);
	entry = kgsl_sharedmem_find(private, param->gpuaddr);
	spin_unlock(&private->mem_lock);
	if (entry == NULL) {
		KGSL_MEM_INFO(dev_priv->device,
				"can't find gpuaddr %x\n",
				param->gpuaddr);
		return -EINVAL;
	}

	return _kgsl_gpumem_sync_cache(entry, KGSL_GPUMEM_CACHE_FLUSH);
}

/*
 * The common parts of kgsl_ioctl_gpumem_alloc and kgsl_ioctl_gpumem_alloc_id.
 */
int
_gpumem_alloc(struct kgsl_device_private *dev_priv,
		struct kgsl_mem_entry **ret_entry,
		unsigned int size, unsigned int flags)
{
	int result;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry;

	/*
	 * Mask off unknown flags from userspace. This way the caller can
	 * check if a flag is supported by looking at the returned flags.
	 */
	flags &= KGSL_MEMFLAGS_GPUREADONLY
		| KGSL_CACHEMODE_MASK
		| KGSL_MEMTYPE_MASK
		| KGSL_MEMALIGN_MASK
		| KGSL_MEMFLAGS_USE_CPU_MAP;

	entry = kgsl_mem_entry_create();
	if (entry == NULL)
		return -ENOMEM;

	result = kgsl_allocate_user(&entry->memdesc, private->pagetable, size,
				    flags);
	if (result != 0)
		goto err;

	entry->memtype = KGSL_MEM_ENTRY_KERNEL;

	kgsl_check_idle(dev_priv->device);
	*ret_entry = entry;
	return result;
err:
	kfree(entry);
	*ret_entry = NULL;
	return result;
}

static long
kgsl_ioctl_gpumem_alloc(struct kgsl_device_private *dev_priv,
			unsigned int cmd, void *data)
{
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_gpumem_alloc *param = data;
	struct kgsl_mem_entry *entry = NULL;
	int result;

	param->flags &= ~KGSL_MEMFLAGS_USE_CPU_MAP;
	result = _gpumem_alloc(dev_priv, &entry, param->size, param->flags);
	if (result)
		return result;

	result = kgsl_mmu_map(private->pagetable, &entry->memdesc);
	if (result)
		goto err;

	result = kgsl_mem_entry_attach_process(entry, private);
	if (result != 0)
		goto err;

	kgsl_process_add_stats(private, entry->memtype, param->size);
	trace_kgsl_mem_alloc(entry);

	param->gpuaddr = entry->memdesc.gpuaddr;
	param->size = entry->memdesc.size;
	param->flags = entry->memdesc.flags;
	return result;
err:
	kgsl_sharedmem_free(&entry->memdesc);
	kfree(entry);
	return result;
}

static long
kgsl_ioctl_gpumem_alloc_id(struct kgsl_device_private *dev_priv,
			unsigned int cmd, void *data)
{
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_gpumem_alloc_id *param = data;
	struct kgsl_mem_entry *entry = NULL;
	int result;

	if (!kgsl_mmu_use_cpu_map(private->pagetable->mmu))
		param->flags &= ~KGSL_MEMFLAGS_USE_CPU_MAP;

	result = _gpumem_alloc(dev_priv, &entry, param->size, param->flags);
	if (result != 0)
		goto err;

	if (!kgsl_memdesc_use_cpu_map(&entry->memdesc)) {
		result = kgsl_mmu_map(private->pagetable, &entry->memdesc);
		if (result)
			goto err;
	}

	result = kgsl_mem_entry_attach_process(entry, private);
	if (result != 0)
		goto err;

	kgsl_process_add_stats(private, entry->memtype, param->size);
	trace_kgsl_mem_alloc(entry);

	param->id = entry->id;
	param->flags = entry->memdesc.flags;
	param->size = entry->memdesc.size;
	param->mmapsize = kgsl_memdesc_mmapsize(&entry->memdesc);
	param->gpuaddr = entry->memdesc.gpuaddr;
	return result;
err:
	if (entry)
		kgsl_sharedmem_free(&entry->memdesc);
	kfree(entry);
	return result;
}

static long
kgsl_ioctl_gpumem_get_info(struct kgsl_device_private *dev_priv,
			unsigned int cmd, void *data)
{
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_gpumem_get_info *param = data;
	struct kgsl_mem_entry *entry = NULL;
	int result = 0;

	if (param->id != 0) {
		entry = kgsl_sharedmem_find_id(private, param->id);
		if (entry == NULL) {
			KGSL_MEM_INFO(dev_priv->device, "can't find id %d\n",
					param->id);
			return -EINVAL;
		}
	} else if (param->gpuaddr != 0) {
		spin_lock(&private->mem_lock);
		entry = kgsl_sharedmem_find(private, param->gpuaddr);
		spin_unlock(&private->mem_lock);
		if (entry == NULL) {
			KGSL_MEM_INFO(dev_priv->device,
					"can't find gpuaddr %lx\n",
					param->gpuaddr);
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}
	param->gpuaddr = entry->memdesc.gpuaddr;
	param->id = entry->id;
	param->flags = entry->memdesc.flags;
	param->size = entry->memdesc.size;
	param->mmapsize = kgsl_memdesc_mmapsize(&entry->memdesc);
	param->useraddr = entry->memdesc.useraddr;
	return result;
}

static long kgsl_ioctl_cff_syncmem(struct kgsl_device_private *dev_priv,
					unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_cff_syncmem *param = data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;

	spin_lock(&private->mem_lock);
	entry = kgsl_sharedmem_find_region(private, param->gpuaddr, param->len);
	if (entry)
		kgsl_cffdump_syncmem(dev_priv, &entry->memdesc, param->gpuaddr,
				     param->len, true);
	else
		result = -EINVAL;
	spin_unlock(&private->mem_lock);
	return result;
}

static long kgsl_ioctl_cff_user_event(struct kgsl_device_private *dev_priv,
		unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_cff_user_event *param = data;

	kgsl_cffdump_user_event(param->cff_opcode, param->op1, param->op2,
			param->op3, param->op4, param->op5);

	return result;
}

#ifdef CONFIG_GENLOCK
struct kgsl_genlock_event_priv {
	struct genlock_handle *handle;
	struct genlock *lock;
};

/**
 * kgsl_genlock_event_cb - Event callback for a genlock timestamp event
 * @device - The KGSL device that expired the timestamp
 * @priv - private data for the event
 * @context_id - the context id that goes with the timestamp
 * @timestamp - the timestamp that triggered the event
 *
 * Release a genlock lock following the expiration of a timestamp
 */

static void kgsl_genlock_event_cb(struct kgsl_device *device,
	void *priv, u32 context_id, u32 timestamp)
{
	struct kgsl_genlock_event_priv *ev = priv;
	int ret;

	ret = genlock_lock(ev->handle, GENLOCK_UNLOCK, 0, 0);
	if (ret)
		KGSL_CORE_ERR("Error while unlocking genlock: %d\n", ret);

	genlock_put_handle(ev->handle);

	kfree(ev);
}

/**
 * kgsl_add_genlock-event - Create a new genlock event
 * @device - KGSL device to create the event on
 * @timestamp - Timestamp to trigger the event
 * @data - User space buffer containing struct kgsl_genlock_event_priv
 * @len - length of the userspace buffer
 * @owner - driver instance that owns this event
 * @returns 0 on success or error code on error
 *
 * Attack to a genlock handle and register an event to release the
 * genlock lock when the timestamp expires
 */

static int kgsl_add_genlock_event(struct kgsl_device *device,
	u32 context_id, u32 timestamp, void __user *data, int len,
	struct kgsl_device_private *owner)
{
	struct kgsl_genlock_event_priv *event;
	struct kgsl_timestamp_event_genlock priv;
	int ret;

	if (len !=  sizeof(priv))
		return -EINVAL;

	if (copy_from_user(&priv, data, sizeof(priv)))
		return -EFAULT;

	event = kzalloc(sizeof(*event), GFP_KERNEL);

	if (event == NULL)
		return -ENOMEM;

	event->handle = genlock_get_handle_fd(priv.handle);

	if (IS_ERR(event->handle)) {
		int ret = PTR_ERR(event->handle);
		kfree(event);
		return ret;
	}

	ret = kgsl_add_event(device, context_id, timestamp,
			kgsl_genlock_event_cb, event, owner);
	if (ret)
		kfree(event);

	return ret;
}
#else
static long kgsl_add_genlock_event(struct kgsl_device *device,
	u32 context_id, u32 timestamp, void __user *data, int len,
	struct kgsl_device_private *owner)
{
	return -EINVAL;
}
#endif

/**
 * kgsl_ioctl_timestamp_event - Register a new timestamp event from userspace
 * @dev_priv - pointer to the private device structure
 * @cmd - the ioctl cmd passed from kgsl_ioctl
 * @data - the user data buffer from kgsl_ioctl
 * @returns 0 on success or error code on failure
 */

static long kgsl_ioctl_timestamp_event(struct kgsl_device_private *dev_priv,
		unsigned int cmd, void *data)
{
	struct kgsl_timestamp_event *param = data;
	int ret;

	switch (param->type) {
	case KGSL_TIMESTAMP_EVENT_GENLOCK:
		ret = kgsl_add_genlock_event(dev_priv->device,
			param->context_id, param->timestamp, param->priv,
			param->len, dev_priv);
		break;
	case KGSL_TIMESTAMP_EVENT_FENCE:
		ret = kgsl_add_fence_event(dev_priv->device,
			param->context_id, param->timestamp, param->priv,
			param->len, dev_priv);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

typedef long (*kgsl_ioctl_func_t)(struct kgsl_device_private *,
	unsigned int, void *);

#define KGSL_IOCTL_FUNC(_cmd, _func, _flags) \
	[_IOC_NR((_cmd))] = \
		{ .cmd = (_cmd), .func = (_func), .flags = (_flags) }

#define KGSL_IOCTL_LOCK		BIT(0)
#define KGSL_IOCTL_WAKE		BIT(1)

static const struct {
	unsigned int cmd;
	kgsl_ioctl_func_t func;
	unsigned int flags;
} kgsl_ioctl_funcs[] = {
	KGSL_IOCTL_FUNC(IOCTL_KGSL_DEVICE_GETPROPERTY,
			kgsl_ioctl_device_getproperty,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_DEVICE_WAITTIMESTAMP,
			kgsl_ioctl_device_waittimestamp,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID,
			kgsl_ioctl_device_waittimestamp_ctxtid,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_RINGBUFFER_ISSUEIBCMDS,
			kgsl_ioctl_rb_issueibcmds,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CMDSTREAM_READTIMESTAMP,
			kgsl_ioctl_cmdstream_readtimestamp,
			KGSL_IOCTL_LOCK),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID,
			kgsl_ioctl_cmdstream_readtimestamp_ctxtid,
			KGSL_IOCTL_LOCK),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP,
			kgsl_ioctl_cmdstream_freememontimestamp,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP_CTXTID,
			kgsl_ioctl_cmdstream_freememontimestamp_ctxtid,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_DRAWCTXT_CREATE,
			kgsl_ioctl_drawctxt_create,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_DRAWCTXT_DESTROY,
			kgsl_ioctl_drawctxt_destroy,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_MAP_USER_MEM,
			kgsl_ioctl_map_user_mem, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_SHAREDMEM_FROM_PMEM,
			kgsl_ioctl_map_user_mem, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_SHAREDMEM_FREE,
			kgsl_ioctl_sharedmem_free, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_SHAREDMEM_FLUSH_CACHE,
			kgsl_ioctl_sharedmem_flush_cache, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_GPUMEM_ALLOC,
			kgsl_ioctl_gpumem_alloc, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CFF_SYNCMEM,
			kgsl_ioctl_cff_syncmem, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_CFF_USER_EVENT,
			kgsl_ioctl_cff_user_event, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_TIMESTAMP_EVENT,
			kgsl_ioctl_timestamp_event,
			KGSL_IOCTL_LOCK),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_SETPROPERTY,
			kgsl_ioctl_device_setproperty,
			KGSL_IOCTL_LOCK | KGSL_IOCTL_WAKE),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_GPUMEM_ALLOC_ID,
			kgsl_ioctl_gpumem_alloc_id, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_GPUMEM_FREE_ID,
			kgsl_ioctl_gpumem_free_id, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_GPUMEM_GET_INFO,
			kgsl_ioctl_gpumem_get_info, 0),
	KGSL_IOCTL_FUNC(IOCTL_KGSL_GPUMEM_SYNC_CACHE,
			kgsl_ioctl_gpumem_sync_cache, 0),
};

static long kgsl_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct kgsl_device_private *dev_priv = filep->private_data;
	unsigned int nr;
	kgsl_ioctl_func_t func;
	int lock, ret, use_hw;
	char ustack[64];
	void *uptr = NULL;

	BUG_ON(dev_priv == NULL);

	/* Workaround for an previously incorrectly defined ioctl code.
	   This helps ensure binary compatability */

	if (cmd == IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP_OLD)
		cmd = IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP;
	else if (cmd == IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_OLD)
		cmd = IOCTL_KGSL_CMDSTREAM_READTIMESTAMP;
	else if (cmd == IOCTL_KGSL_TIMESTAMP_EVENT_OLD)
		cmd = IOCTL_KGSL_TIMESTAMP_EVENT;

	nr = _IOC_NR(cmd);

	if (cmd & (IOC_IN | IOC_OUT)) {
		if (_IOC_SIZE(cmd) < sizeof(ustack))
			uptr = ustack;
		else {
			uptr = kzalloc(_IOC_SIZE(cmd), GFP_KERNEL);
			if (uptr == NULL) {
				KGSL_MEM_ERR(dev_priv->device,
					"kzalloc(%d) failed\n", _IOC_SIZE(cmd));
				ret = -ENOMEM;
				goto done;
			}
		}

		if (cmd & IOC_IN) {
			if (copy_from_user(uptr, (void __user *) arg,
				_IOC_SIZE(cmd))) {
				ret = -EFAULT;
				goto done;
			}
		} else
			memset(uptr, 0, _IOC_SIZE(cmd));
	}

	if (nr < ARRAY_SIZE(kgsl_ioctl_funcs) &&
		kgsl_ioctl_funcs[nr].func != NULL) {

		/*
		 * Make sure that nobody tried to send us a malformed ioctl code
		 * with a valid NR but bogus flags
		 */

		if (kgsl_ioctl_funcs[nr].cmd != cmd) {
			KGSL_DRV_ERR(dev_priv->device,
				"Malformed ioctl code %08x\n", cmd);
			ret = -ENOIOCTLCMD;
			goto done;
		}

		func = kgsl_ioctl_funcs[nr].func;
		lock = kgsl_ioctl_funcs[nr].flags & KGSL_IOCTL_LOCK;
		use_hw = kgsl_ioctl_funcs[nr].flags & KGSL_IOCTL_WAKE;
	} else {
		func = dev_priv->device->ftbl->ioctl;
		if (!func) {
			KGSL_DRV_INFO(dev_priv->device,
				      "invalid ioctl code %08x\n", cmd);
			ret = -ENOIOCTLCMD;
			goto done;
		}
		lock = 1;
		use_hw = 1;
	}

	if (lock) {
		mutex_lock(&dev_priv->device->mutex);
		if (use_hw)
			kgsl_check_suspended(dev_priv->device);
	}

	ret = func(dev_priv, cmd, uptr);

	if (lock) {
		kgsl_check_idle_locked(dev_priv->device);
		mutex_unlock(&dev_priv->device->mutex);
	}

	/*
	 * Still copy back on failure, but assume function took
	 * all necessary precautions sanitizing the return values.
	 */
	if (cmd & IOC_OUT) {
		if (copy_to_user((void __user *) arg, uptr, _IOC_SIZE(cmd)))
			ret = -EFAULT;
	}

done:
	if (_IOC_SIZE(cmd) >= sizeof(ustack))
		kfree(uptr);

	return ret;
}

static int
kgsl_mmap_memstore(struct kgsl_device *device, struct vm_area_struct *vma)
{
	struct kgsl_memdesc *memdesc = &device->memstore;
	int result;
	unsigned int vma_size = vma->vm_end - vma->vm_start;

	/* The memstore can only be mapped as read only */

	if (vma->vm_flags & VM_WRITE)
		return -EPERM;

	if (memdesc->size  !=  vma_size) {
		KGSL_MEM_ERR(device, "memstore bad size: %d should be %d\n",
			     vma_size, memdesc->size);
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	result = remap_pfn_range(vma, vma->vm_start,
				device->memstore.physaddr >> PAGE_SHIFT,
				 vma_size, vma->vm_page_prot);
	if (result != 0)
		KGSL_MEM_ERR(device, "remap_pfn_range failed: %d\n",
			     result);

	return result;
}

/*
 * kgsl_gpumem_vm_open is called whenever a vma region is copied or split.
 * Increase the refcount to make sure that the accounting stays correct
 */

static void kgsl_gpumem_vm_open(struct vm_area_struct *vma)
{
	struct kgsl_mem_entry *entry = vma->vm_private_data;
	kgsl_mem_entry_get(entry);
}

static int
kgsl_gpumem_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct kgsl_mem_entry *entry = vma->vm_private_data;

	if (!entry->memdesc.ops || !entry->memdesc.ops->vmfault)
		return VM_FAULT_SIGBUS;

	return entry->memdesc.ops->vmfault(&entry->memdesc, vma, vmf);
}

static void
kgsl_gpumem_vm_close(struct vm_area_struct *vma)
{
	struct kgsl_mem_entry *entry  = vma->vm_private_data;

	entry->memdesc.useraddr = 0;
	kgsl_mem_entry_put(entry);
}

static struct vm_operations_struct kgsl_gpumem_vm_ops = {
	.open  = kgsl_gpumem_vm_open,
	.fault = kgsl_gpumem_vm_fault,
	.close = kgsl_gpumem_vm_close,
};

static int
get_mmap_entry(struct kgsl_process_private *private,
		struct kgsl_mem_entry **out_entry, unsigned long pgoff,
		unsigned long len)
{
	int ret = -EINVAL;
	struct kgsl_mem_entry *entry;

	entry = kgsl_sharedmem_find_id(private, pgoff);
	if (entry == NULL) {
		spin_lock(&private->mem_lock);
		entry = kgsl_sharedmem_find(private, pgoff << PAGE_SHIFT);
		spin_unlock(&private->mem_lock);
	}

	if (!entry)
		return -EINVAL;

	kgsl_mem_entry_get(entry);

	if (!entry->memdesc.ops ||
		!entry->memdesc.ops->vmflags ||
		!entry->memdesc.ops->vmfault) {
		ret = -EINVAL;
		goto err_put;
	}

	if (entry->memdesc.useraddr != 0) {
		ret = -EBUSY;
		goto err_put;
	}

	if (len != kgsl_memdesc_mmapsize(&entry->memdesc)) {
		ret = -ERANGE;
		goto err_put;
	}

	*out_entry = entry;
	return 0;
err_put:
	kgsl_mem_entry_put(entry);
	return ret;
}

static unsigned long
kgsl_get_unmapped_area(struct file *file, unsigned long addr,
			unsigned long len, unsigned long pgoff,
			unsigned long flags)
{
	unsigned long ret = 0;
	unsigned long vma_offset = pgoff << PAGE_SHIFT;
	struct kgsl_device_private *dev_priv = file->private_data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_device *device = dev_priv->device;
	struct kgsl_mem_entry *entry = NULL;
	unsigned int align;
	unsigned int retry = 0;

	if (vma_offset == device->memstore.gpuaddr)
		return get_unmapped_area(NULL, addr, len, pgoff, flags);

	ret = get_mmap_entry(private, &entry, pgoff, len);
	if (ret)
		return ret;

	if (!kgsl_memdesc_use_cpu_map(&entry->memdesc) || (flags & MAP_FIXED)) {
		/*
		 * If we're not going to use the same mapping on the gpu,
		 * any address is fine.
		 * For MAP_FIXED, hopefully the caller knows what they're doing,
		 * but we may fail in mmap() if there is already something
		 * at the virtual address chosen.
		 */
		ret = get_unmapped_area(NULL, addr, len, pgoff, flags);
		goto put;
	}
	if (entry->memdesc.gpuaddr != 0) {
		KGSL_MEM_INFO(device,
				"pgoff %lx already mapped to gpuaddr %x\n",
				pgoff, entry->memdesc.gpuaddr);
		ret = -EBUSY;
		goto put;
	}

	align = kgsl_memdesc_get_align(&entry->memdesc);
	if (align >= ilog2(SZ_1M))
		align = ilog2(SZ_1M);
	else if (align >= ilog2(SZ_64K))
		align = ilog2(SZ_64K);
	else if (align <= PAGE_SHIFT)
		align = 0;

	if (align)
		len += 1 << align;
	do {
		ret = get_unmapped_area(NULL, addr, len, pgoff, flags);
		if (IS_ERR_VALUE(ret))
			break;
		if (align)
			ret = ALIGN(ret, (1 << align));

		/*make sure there isn't a GPU only mapping at this address */
		if (kgsl_sharedmem_region_empty(private, ret, len))
			break;

		trace_kgsl_mem_unmapped_area_collision(entry, addr, len, ret);

		/*
		 * If we collided, bump the hint address so that
		 * get_umapped_area knows to look somewhere else.
		 */
		addr = (addr == 0) ? ret + len : addr + len;

		/*
		 * The addr hint can be set by userspace to be near
		 * the end of the address space. Make sure we search
		 * the whole address space at least once by wrapping
		 * back around once.
		 */
		if (!retry && (addr + len >= TASK_SIZE)) {
			addr = 0;
			retry = 1;
		} else {
			ret = -EBUSY;
		}
	} while (addr + len < TASK_SIZE);

	if (IS_ERR_VALUE(ret))
		KGSL_MEM_INFO(device,
				"pid %d pgoff %lx len %ld failed error %ld\n",
				private->pid, pgoff, len, ret);
put:
	kgsl_mem_entry_put(entry);
	return ret;
}

static int kgsl_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned int ret, cache;
	unsigned long vma_offset = vma->vm_pgoff << PAGE_SHIFT;
	struct kgsl_device_private *dev_priv = file->private_data;
	struct kgsl_process_private *private = dev_priv->process_priv;
	struct kgsl_mem_entry *entry = NULL;
	struct kgsl_device *device = dev_priv->device;

	/* Handle leagacy behavior for memstore */

	if (vma_offset == device->memstore.gpuaddr)
		return kgsl_mmap_memstore(device, vma);

	ret = get_mmap_entry(private, &entry, vma->vm_pgoff,
				vma->vm_end - vma->vm_start);
	if (ret)
		return ret;

	if (kgsl_memdesc_use_cpu_map(&entry->memdesc)) {
		entry->memdesc.gpuaddr = vma->vm_start;

		ret = kgsl_mmu_map(private->pagetable, &entry->memdesc);
		if (ret) {
			kgsl_mem_entry_put(entry);
			return ret;
		}
		kgsl_mem_entry_track_gpuaddr(private, entry);
	}

	vma->vm_flags |= entry->memdesc.ops->vmflags(&entry->memdesc);

	vma->vm_private_data = entry;

	/* Determine user-side caching policy */

	cache = kgsl_memdesc_get_cachemode(&entry->memdesc);

	switch (cache) {
	case KGSL_CACHEMODE_UNCACHED:
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		break;
	case KGSL_CACHEMODE_WRITETHROUGH:
		vma->vm_page_prot = pgprot_writethroughcache(vma->vm_page_prot);
		break;
	case KGSL_CACHEMODE_WRITEBACK:
		vma->vm_page_prot = pgprot_writebackcache(vma->vm_page_prot);
		break;
	case KGSL_CACHEMODE_WRITECOMBINE:
	default:
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		break;
	}

	vma->vm_ops = &kgsl_gpumem_vm_ops;

	if (cache == KGSL_CACHEMODE_WRITEBACK
		|| cache == KGSL_CACHEMODE_WRITETHROUGH) {
		struct scatterlist *s;
		int i;
		int sglen = entry->memdesc.sglen;
		unsigned long addr = vma->vm_start;

		/* don't map in the guard page, it should always fault */
		if (kgsl_memdesc_has_guard_page(&entry->memdesc))
			sglen--;

		for_each_sg(entry->memdesc.sg, s, sglen, i) {
			int j;
			for (j = 0; j < (sg_dma_len(s) >> PAGE_SHIFT); j++) {
				struct page *page = sg_page(s);
				page = nth_page(page, j);
				vm_insert_page(vma, addr, page);
				addr += PAGE_SIZE;
			}
		}
	}

	vma->vm_file = file;

	entry->memdesc.useraddr = vma->vm_start;

	trace_kgsl_mem_mmap(entry);

	return 0;
}

static irqreturn_t kgsl_irq_handler(int irq, void *data)
{
	struct kgsl_device *device = data;

	return device->ftbl->irq_handler(device);

}

static const struct file_operations kgsl_fops = {
	.owner = THIS_MODULE,
	.release = kgsl_release,
	.open = kgsl_open,
	.mmap = kgsl_mmap,
	.get_unmapped_area = kgsl_get_unmapped_area,
	.unlocked_ioctl = kgsl_ioctl,
};

struct kgsl_driver kgsl_driver  = {
	.process_mutex = __MUTEX_INITIALIZER(kgsl_driver.process_mutex),
	.ptlock = __SPIN_LOCK_UNLOCKED(kgsl_driver.ptlock),
	.devlock = __MUTEX_INITIALIZER(kgsl_driver.devlock),
	.memfree_hist_mutex =
		__MUTEX_INITIALIZER(kgsl_driver.memfree_hist_mutex),
};
EXPORT_SYMBOL(kgsl_driver);

static void _unregister_device(struct kgsl_device *device)
{
	int minor;

	mutex_lock(&kgsl_driver.devlock);
	for (minor = 0; minor < KGSL_DEVICE_MAX; minor++) {
		if (device == kgsl_driver.devp[minor])
			break;
	}
	if (minor != KGSL_DEVICE_MAX) {
		device_destroy(kgsl_driver.class,
				MKDEV(MAJOR(kgsl_driver.major), minor));
		kgsl_driver.devp[minor] = NULL;
	}
	mutex_unlock(&kgsl_driver.devlock);
}

static int _register_device(struct kgsl_device *device)
{
	int minor, ret;
	dev_t dev;

	/* Find a minor for the device */

	mutex_lock(&kgsl_driver.devlock);
	for (minor = 0; minor < KGSL_DEVICE_MAX; minor++) {
		if (kgsl_driver.devp[minor] == NULL) {
			kgsl_driver.devp[minor] = device;
			break;
		}
	}
	mutex_unlock(&kgsl_driver.devlock);

	if (minor == KGSL_DEVICE_MAX) {
		KGSL_CORE_ERR("minor devices exhausted\n");
		return -ENODEV;
	}

	/* Create the device */
	dev = MKDEV(MAJOR(kgsl_driver.major), minor);
	device->dev = device_create(kgsl_driver.class,
				    device->parentdev,
				    dev, device,
				    device->name);

	if (IS_ERR(device->dev)) {
		mutex_lock(&kgsl_driver.devlock);
		kgsl_driver.devp[minor] = NULL;
		mutex_unlock(&kgsl_driver.devlock);
		ret = PTR_ERR(device->dev);
		KGSL_CORE_ERR("device_create(%s): %d\n", device->name, ret);
		return ret;
	}

	dev_set_drvdata(device->parentdev, device);
	return 0;
}

int kgsl_device_platform_probe(struct kgsl_device *device)
{
	int result;
	int status = -EINVAL;
	struct resource *res;
	struct platform_device *pdev =
		container_of(device->parentdev, struct platform_device, dev);

	status = _register_device(device);
	if (status)
		return status;

	/* Initialize logging first, so that failures below actually print. */
	kgsl_device_debugfs_init(device);

	status = kgsl_pwrctrl_init(device);
	if (status)
		goto error;

	kgsl_ion_client = msm_ion_client_create(UINT_MAX, KGSL_NAME);

	/* Get starting physical address of device registers */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   device->iomemname);
	if (res == NULL) {
		KGSL_DRV_ERR(device, "platform_get_resource_byname failed\n");
		status = -EINVAL;
		goto error_pwrctrl_close;
	}
	if (res->start == 0 || resource_size(res) == 0) {
		KGSL_DRV_ERR(device, "dev %d invalid register region\n",
			device->id);
		status = -EINVAL;
		goto error_pwrctrl_close;
	}

	device->reg_phys = res->start;
	device->reg_len = resource_size(res);

	/*
	 * Check if a shadermemname is defined, and then get shader memory
	 * details including shader memory starting physical address
	 * and shader memory length
	 */
	if (device->shadermemname != NULL) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						device->shadermemname);

		if (res == NULL) {
			KGSL_DRV_ERR(device,
			"Shader memory: platform_get_resource_byname failed\n");
		}

		else {
			device->shader_mem_phys = res->start;
			device->shader_mem_len = resource_size(res);
		}

		if (!devm_request_mem_region(device->dev,
					device->shader_mem_phys,
					device->shader_mem_len,
						device->name)) {
			KGSL_DRV_ERR(device, "request_mem_region_failed\n");
		}
	}

	if (!devm_request_mem_region(device->dev, device->reg_phys,
				device->reg_len, device->name)) {
		KGSL_DRV_ERR(device, "request_mem_region failed\n");
		status = -ENODEV;
		goto error_pwrctrl_close;
	}

	device->reg_virt = devm_ioremap(device->dev, device->reg_phys,
					device->reg_len);

	if (device->reg_virt == NULL) {
		KGSL_DRV_ERR(device, "ioremap failed\n");
		status = -ENODEV;
		goto error_pwrctrl_close;
	}
	/*acquire interrupt */
	device->pwrctrl.interrupt_num =
		platform_get_irq_byname(pdev, device->pwrctrl.irq_name);

	if (device->pwrctrl.interrupt_num <= 0) {
		KGSL_DRV_ERR(device, "platform_get_irq_byname failed: %d\n",
					 device->pwrctrl.interrupt_num);
		status = -EINVAL;
		goto error_pwrctrl_close;
	}

	status = devm_request_irq(device->dev, device->pwrctrl.interrupt_num,
				  kgsl_irq_handler, IRQF_TRIGGER_HIGH,
				  device->name, device);
	if (status) {
		KGSL_DRV_ERR(device, "request_irq(%d) failed: %d\n",
			      device->pwrctrl.interrupt_num, status);
		goto error_pwrctrl_close;
	}
	disable_irq(device->pwrctrl.interrupt_num);

	KGSL_DRV_INFO(device,
		"dev_id %d regs phys 0x%08lx size 0x%08x virt %p\n",
		device->id, device->reg_phys, device->reg_len,
		device->reg_virt);

	result = kgsl_drm_init(pdev);
	if (result)
		goto error_pwrctrl_close;


	setup_timer(&device->idle_timer, kgsl_timer, (unsigned long) device);
	status = kgsl_create_device_workqueue(device);
	if (status)
		goto error_pwrctrl_close;

	status = kgsl_mmu_init(device);
	if (status != 0) {
		KGSL_DRV_ERR(device, "kgsl_mmu_init failed %d\n", status);
		goto error_dest_work_q;
	}

	status = kgsl_allocate_contiguous(&device->memstore,
		KGSL_MEMSTORE_SIZE);

	if (status != 0) {
		KGSL_DRV_ERR(device, "kgsl_allocate_contiguous failed %d\n",
				status);
		goto error_close_mmu;
	}

	pm_qos_add_request(&device->pwrctrl.pm_qos_req_dma,
				PM_QOS_CPU_DMA_LATENCY,
				PM_QOS_DEFAULT_VALUE);

	/* Initalize the snapshot engine */
	kgsl_device_snapshot_init(device);

	/* Initialize common sysfs entries */
	kgsl_pwrctrl_init_sysfs(device);

	return 0;

error_close_mmu:
	kgsl_mmu_close(device);
error_dest_work_q:
	destroy_workqueue(device->work_queue);
	device->work_queue = NULL;
error_pwrctrl_close:
	kgsl_pwrctrl_close(device);
error:
	_unregister_device(device);
	return status;
}
EXPORT_SYMBOL(kgsl_device_platform_probe);

int kgsl_postmortem_dump(struct kgsl_device *device, int manual)
{
	bool saved_nap;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	BUG_ON(device == NULL);

	kgsl_cffdump_hang(device->id);

	/* For a manual dump, make sure that the system is idle */

	if (manual) {
		if (device->active_cnt != 0) {
			mutex_unlock(&device->mutex);
			wait_for_completion(&device->suspend_gate);
			mutex_lock(&device->mutex);
		}

		if (device->state == KGSL_STATE_ACTIVE)
			kgsl_idle(device);

	}

	if (device->pm_dump_enable) {

		KGSL_LOG_DUMP(device,
			"POWER: NAP ALLOWED = %d | START_STOP_SLEEP_WAKE = %d\n"
			, pwr->nap_allowed, pwr->strtstp_sleepwake);

		KGSL_LOG_DUMP(device,
			"POWER: FLAGS = %08lX | ACTIVE POWERLEVEL = %08X",
			pwr->power_flags, pwr->active_pwrlevel);

		KGSL_LOG_DUMP(device, "POWER: INTERVAL TIMEOUT = %08X ",
			pwr->interval_timeout);

	}

	/* Disable the idle timer so we don't get interrupted */
	del_timer_sync(&device->idle_timer);
	mutex_unlock(&device->mutex);
	flush_workqueue(device->work_queue);
	mutex_lock(&device->mutex);

	/* Turn off napping to make sure we have the clocks full
	   attention through the following process */
	saved_nap = device->pwrctrl.nap_allowed;
	device->pwrctrl.nap_allowed = false;

	/* Force on the clocks */
	kgsl_pwrctrl_wake(device);

	/* Disable the irq */
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);

	/*Call the device specific postmortem dump function*/
	device->ftbl->postmortem_dump(device, manual);

	/* Restore nap mode */
	device->pwrctrl.nap_allowed = saved_nap;

	/* On a manual trigger, turn on the interrupts and put
	   the clocks to sleep.  They will recover themselves
	   on the next event.  For a hang, leave things as they
	   are until fault tolerance kicks in. */

	if (manual) {
		kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_ON);

		/* try to go into a sleep mode until the next event */
		kgsl_pwrctrl_request_state(device, KGSL_STATE_SLEEP);
		kgsl_pwrctrl_sleep(device);
	}

	return 0;
}
EXPORT_SYMBOL(kgsl_postmortem_dump);

void kgsl_device_platform_remove(struct kgsl_device *device)
{
	kgsl_device_snapshot_close(device);

	kgsl_pwrctrl_uninit_sysfs(device);

	pm_qos_remove_request(&device->pwrctrl.pm_qos_req_dma);

	idr_destroy(&device->context_idr);

	kgsl_sharedmem_free(&device->memstore);

	kgsl_mmu_close(device);

	if (device->work_queue) {
		destroy_workqueue(device->work_queue);
		device->work_queue = NULL;
	}
	kgsl_pwrctrl_close(device);

	_unregister_device(device);
}
EXPORT_SYMBOL(kgsl_device_platform_remove);

static int __devinit
kgsl_ptdata_init(void)
{
	kgsl_driver.ptpool = kgsl_mmu_ptpool_init(kgsl_pagetable_count);

	if (!kgsl_driver.ptpool)
		return -ENOMEM;
	return 0;
}

static void kgsl_core_exit(void)
{
	kgsl_mmu_ptpool_destroy(kgsl_driver.ptpool);
	kgsl_driver.ptpool = NULL;

	kgsl_drm_exit();
	kgsl_cffdump_destroy();
	kgsl_core_debugfs_close();

	/*
	 * We call kgsl_sharedmem_uninit_sysfs() and device_unregister()
	 * only if kgsl_driver.virtdev has been populated.
	 * We check at least one member of kgsl_driver.virtdev to
	 * see if it is not NULL (and thus, has been populated).
	 */
	if (kgsl_driver.virtdev.class) {
		kgsl_sharedmem_uninit_sysfs();
		device_unregister(&kgsl_driver.virtdev);
	}

	if (kgsl_driver.class) {
		class_destroy(kgsl_driver.class);
		kgsl_driver.class = NULL;
	}

	kgsl_memfree_hist_exit();
	unregister_chrdev_region(kgsl_driver.major, KGSL_DEVICE_MAX);
}

static int __init kgsl_core_init(void)
{
	int result = 0;
	/* alloc major and minor device numbers */
	result = alloc_chrdev_region(&kgsl_driver.major, 0, KGSL_DEVICE_MAX,
				  KGSL_NAME);
	if (result < 0) {
		KGSL_CORE_ERR("alloc_chrdev_region failed err = %d\n", result);
		goto err;
	}

	cdev_init(&kgsl_driver.cdev, &kgsl_fops);
	kgsl_driver.cdev.owner = THIS_MODULE;
	kgsl_driver.cdev.ops = &kgsl_fops;
	result = cdev_add(&kgsl_driver.cdev, MKDEV(MAJOR(kgsl_driver.major), 0),
		       KGSL_DEVICE_MAX);

	if (result) {
		KGSL_CORE_ERR("kgsl: cdev_add() failed, dev_num= %d,"
			     " result= %d\n", kgsl_driver.major, result);
		goto err;
	}

	kgsl_driver.class = class_create(THIS_MODULE, KGSL_NAME);

	if (IS_ERR(kgsl_driver.class)) {
		result = PTR_ERR(kgsl_driver.class);
		KGSL_CORE_ERR("failed to create class %s", KGSL_NAME);
		goto err;
	}

	/* Make a virtual device for managing core related things
	   in sysfs */
	kgsl_driver.virtdev.class = kgsl_driver.class;
	dev_set_name(&kgsl_driver.virtdev, "kgsl");
	result = device_register(&kgsl_driver.virtdev);
	if (result) {
		KGSL_CORE_ERR("driver_register failed\n");
		goto err;
	}

	/* Make kobjects in the virtual device for storing statistics */

	kgsl_driver.ptkobj =
	  kobject_create_and_add("pagetables",
				 &kgsl_driver.virtdev.kobj);

	kgsl_driver.prockobj =
		kobject_create_and_add("proc",
				       &kgsl_driver.virtdev.kobj);

	kgsl_core_debugfs_init();

	kgsl_sharedmem_init_sysfs();
	kgsl_cffdump_init();

	INIT_LIST_HEAD(&kgsl_driver.process_list);

	INIT_LIST_HEAD(&kgsl_driver.pagetable_list);

	kgsl_mmu_set_mmutype(ksgl_mmu_type);

	if (KGSL_MMU_TYPE_GPU == kgsl_mmu_get_mmutype()) {
		result = kgsl_ptdata_init();
		if (result)
			goto err;
	}

	if (kgsl_memfree_hist_init())
		KGSL_CORE_ERR("failed to init memfree_hist");

	return 0;

err:
	kgsl_core_exit();
	return result;
}

module_init(kgsl_core_init);
module_exit(kgsl_core_exit);

MODULE_AUTHOR("Qualcomm Innovation Center, Inc.");
MODULE_DESCRIPTION("MSM GPU driver");
MODULE_LICENSE("GPL");
