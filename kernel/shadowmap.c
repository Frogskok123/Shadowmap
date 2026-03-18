#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/version.h>
#include <linux/miscdevice.h>

#include "shared_buffer.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("shadow");
MODULE_DESCRIPTION("Shadowmap user memory mirror (VA=0 window)");
MODULE_VERSION("1.1");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <linux/mmap_lock.h>
#define MM_TRYLOCK(mm) mmap_read_trylock(mm)
#define MM_UNLOCK(mm)  mmap_read_unlock(mm)
#else
#define MM_TRYLOCK(mm) down_read_trylock(&(mm)->mmap_sem)
#define MM_UNLOCK(mm)  up_read(&(mm)->mmap_sem)
#endif

static struct shadowmap_region current_req;

/* С„РёРєСЃРёСЂСѓРµРј РѕРєРЅРѕ: СЃ VA 0, СЂР°Р·РјРµСЂ 512MB (РјРѕР¶РµС€СЊ СѓРІРµР»РёС‡РёС‚СЊ) */
#define SHADOWMAP_BASE   0ULL
#define SHADOWMAP_SIZE   (512ULL * 1024 * 1024)

static inline void shadow_vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    vm_flags_set(vma, flags);
#else
    *(unsigned long *)&vma->vm_flags |= flags;
#endif
}

static vm_fault_t shadow_vma_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    unsigned long fault_addr   = vmf->address;
    unsigned long offset       = fault_addr - vma->vm_start;
    struct task_struct *target_task;
    struct mm_struct   *target_mm;
    struct page        *pinned_page = NULL;
    unsigned long pfn;
    long pin_ret;
    u64 target_va;

    if (!current_req.target_pid || offset >= SHADOWMAP_SIZE)
        return VM_FAULT_SIGBUS;

    /* Р±Р°Р·Сѓ СЃС‡РёС‚Р°РµРј 0, С‡РёС‚Р°РµРј РЅР°РїСЂСЏРјСѓСЋ VA = offset */
    target_va = (SHADOWMAP_BASE + offset) & PAGE_MASK;

    target_task = get_pid_task(find_vpid(current_req.target_pid), PIDTYPE_PID);
    if (!target_task)
        return VM_FAULT_SIGBUS;

    target_mm = get_task_mm(target_task);
    if (!target_mm) {
        put_task_struct(target_task);
        return VM_FAULT_SIGBUS;
    }

    if (!MM_TRYLOCK(target_mm)) {
        mmput(target_mm);
        put_task_struct(target_task);
        return VM_FAULT_RETRY;
    }

#ifndef FOLL_GET
#define FOLL_GET 0x10
#endif
#ifndef FOLL_PIN
#define FOLL_PIN 0x800
#endif
#ifndef FOLL_LONGTERM
#define FOLL_LONGTERM 0x10000
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    pin_ret = pin_user_pages_remote(target_mm, target_va, 1,
                                    FOLL_PIN | FOLL_LONGTERM,
                                    &pinned_page, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    pin_ret = pin_user_pages_remote(target_mm, target_va, 1,
                                    FOLL_PIN | FOLL_LONGTERM,
                                    &pinned_page, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
    pin_ret = get_user_pages_remote(target_mm, target_va, 1,
                                    FOLL_GET, &pinned_page, NULL, NULL);
#else
    pin_ret = get_user_pages_remote(target_task, target_mm, target_va, 1,
                                    FOLL_GET, &pinned_page, NULL, NULL);
#endif

    MM_UNLOCK(target_mm);

    if (pin_ret <= 0 || !pinned_page) {
        pr_err("shadowmap: pin_user_pages_remote failed: %ld", pin_ret);
        mmput(target_mm);
        put_task_struct(target_task);
        return VM_FAULT_SIGBUS;
    }

    pfn = page_to_pfn(pinned_page);
    vmf_insert_pfn(vma, fault_addr, pfn);

    mmput(target_mm);
    put_task_struct(target_task);
    return VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct shadow_vm_ops = {
    .fault = shadow_vma_fault,
};

static int shadowmap_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > SHADOWMAP_SIZE)
        return -EINVAL;

    vma->vm_ops = &shadow_vm_ops;
    shadow_vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
    return 0;
}

static long shadowmap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == IOCTL_SHADOWMAP_REGISTER) {
        if (copy_from_user(&current_req,
                           (void __user *)arg,
                           sizeof(current_req)))
            return -EFAULT;
        /* target_va_start/size РёРіРЅРѕСЂРёСЂСѓРµРј, РЅР°Рј РЅСѓР¶РµРЅ С‚РѕР»СЊРєРѕ pid */
        return 0;
    }
    return -ENOTTY;
}

static const struct file_operations shadowmap_fops = {
    .owner          = THIS_MODULE,
    .mmap           = shadowmap_mmap,
    .unlocked_ioctl = shadowmap_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = shadowmap_ioctl,
#endif
};

static struct miscdevice shadowmap_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = SHADOWMAP_DEVICE_NAME,
    .fops  = &shadowmap_fops,
};

static int __init shadowmap_init(void)
{
    pr_info("shadowmap: init /dev/%s, window size=%llu MB",
           SHADOWMAP_DEVICE_NAME,
            (unsigned long long)(SHADOWMAP_SIZE / (1024 * 1024)));
    return misc_register(&shadowmap_dev);
}

static void __exit shadowmap_exit(void)
{
    misc_deregister(&shadowmap_dev);
    pr_info("shadowmap: exit
");
}

module_init(shadowmap_init);
module_exit(shadowmap_exit);
