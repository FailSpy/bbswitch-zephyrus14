/**
 * Disable discrete graphics (currently nvidia only)
 *
 * Usage:
 * Disable discrete card
 * # echo OFF > /proc/acpi/bbswitch
 * Enable discrete card
 * # echo ON > /proc/acpi/bbswitch
 * Get status
 * # cat /proc/acpi/bbswitch
 */
/*
 *  Copyright (C) 2011-2013 Bumblebee Project
 *  Author: Peter Wu <lekensteyn@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/proc_fs.h>
#include <linux/version.h>


#define BBSWITCH_VERSION "0.8"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Toggle the discrete graphics card");
MODULE_AUTHOR("Peter Wu <lekensteyn@gmail.com>");
MODULE_VERSION(BBSWITCH_VERSION);

enum {
    CARD_UNCHANGED = -1,
    CARD_OFF = 0,
    CARD_ON = 1,
};

static int load_state = CARD_UNCHANGED;
MODULE_PARM_DESC(load_state, "Initial card state (0 = off, 1 = on, -1 = unchanged)");
module_param(load_state, int, 0400);
static int unload_state = CARD_UNCHANGED;
MODULE_PARM_DESC(unload_state, "Card state on unload (0 = off, 1 = on, -1 = unchanged)");
module_param(unload_state, int, 0600);
static bool skip_optimus_dsm = false;
MODULE_PARM_DESC(skip_optimus_dsm, "Skip probe of Optimus discrete DSM (default = false)");
module_param(skip_optimus_dsm, bool, 0400);

extern struct proc_dir_entry *acpi_root_dir;

static const char acpi_optimus_dsm_muid[16] = {
    0xF8, 0xD8, 0x86, 0xA4, 0xDA, 0x0B, 0x1B, 0x47,
    0xA7, 0x2B, 0x60, 0x42, 0xA6, 0xB5, 0xBE, 0xE0,
};

static const char acpi_nvidia_dsm_muid[16] = {
    0xA0, 0xA0, 0x95, 0x9D, 0x60, 0x00, 0x48, 0x4D,
    0xB3, 0x4D, 0x7E, 0x5F, 0xEA, 0x12, 0x9F, 0xD4
};

/*
The next UUID has been found as well in
https://bugs.launchpad.net/lpbugreporter/+bug/752542:

0xD3, 0x73, 0xD8, 0x7E, 0xD0, 0xC2, 0x4F, 0x4E,
0xA8, 0x54, 0x0F, 0x13, 0x17, 0xB0, 0x1C, 0x2C 
It looks like something for Intel GPU:
http://lxr.linux.no/#linux+v3.1.5/drivers/gpu/drm/i915/intel_acpi.c
 */

#define DSM_TYPE_UNSUPPORTED    0
#define DSM_TYPE_OPTIMUS        1
#define DSM_TYPE_NVIDIA         2
static int dsm_type = DSM_TYPE_UNSUPPORTED;

static struct pci_dev *dis_dev;
static acpi_handle dis_handle;

static char dis_dev_name[16];
unsigned int vendor;
unsigned int device;

static struct dev_pm_domain pm_domain;

/* whether the card was off before suspend or not; on: 0, off: 1 */
static int dis_before_suspend_disabled;

static char *buffer_to_string(const char *buffer, size_t n, char *target) {
    int i;
    for (i=0; i<n; i++) {
        snprintf(target + i * 5, 5 * (n - i), "0x%02X,", buffer ? buffer[i] & 0xFF : 0);
    }
    return target;
}

// Returns 0 if the call succeeded and non-zero otherwise. If the call
// succeeded, the result is stored in "result" providing that the result is an
// integer or a buffer containing 4 values
static int acpi_call_dsm(acpi_handle handle, const char muid[16], int revid,
    int func, char args[4], uint32_t *result) {
    struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
    struct acpi_object_list input;
    union acpi_object params[4];
    union acpi_object *obj;
    int err;

    input.count = 4;
    input.pointer = params;
    params[0].type = ACPI_TYPE_BUFFER;
    params[0].buffer.length = 16;
    params[0].buffer.pointer = (char *)muid;
    params[1].type = ACPI_TYPE_INTEGER;
    params[1].integer.value = revid;
    params[2].type = ACPI_TYPE_INTEGER;
    params[2].integer.value = func;
    /* Although the ACPI spec defines Arg3 as a Package, in practise
     * implementations expect a Buffer (CreateWordField and Index functions are
     * applied to it). */
    params[3].type = ACPI_TYPE_BUFFER;
    params[3].buffer.length = 4;
    if (args) {
        params[3].buffer.pointer = args;
    } else {
        // Some implementations (Asus U36SD) seem to check the args before the
        // function ID and crash if it is not a buffer.
        params[3].buffer.pointer = (char[4]){0, 0, 0, 0};
    }

    err = acpi_evaluate_object(handle, "_DSM", &input, &output);
    if (err) {
        struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
        char muid_str[5 * 16];
        char args_str[5 * 4];

        acpi_get_name(handle, ACPI_FULL_PATHNAME, &buf);

        pr_warn("failed to evaluate %s._DSM {%s} 0x%X 0x%X {%s}: %s\n",
            (char *)buf.pointer,
            buffer_to_string(muid, 16, muid_str), revid, func,
            buffer_to_string(args,  4, args_str), acpi_format_exception(err));
        return err;
    }

    obj = (union acpi_object *)output.pointer;

    if (obj->type == ACPI_TYPE_INTEGER && result) {
        *result = obj->integer.value;
    } else if (obj->type == ACPI_TYPE_BUFFER) {
        if (obj->buffer.length == 4 && result) {
            *result = 0;
            *result |= obj->buffer.pointer[0];
            *result |= (obj->buffer.pointer[1] << 8);
            *result |= (obj->buffer.pointer[2] << 16);
            *result |= (obj->buffer.pointer[3] << 24);
        }
    } else {
        pr_warn("_DSM call yields an unsupported result type: %#x\n",
            obj->type);
    }

    kfree(output.pointer);
    return 0;
}

// Returns 1 if a _DSM function and its function index exists and 0 otherwise
static int has_dsm_func(const char muid[16], int revid, int sfnc) {
    u32 result = 0;

    // fail if the _DSM call failed
    if (acpi_call_dsm(dis_handle, muid, revid, 0, 0, &result))
        return 0;

    // ACPI Spec v4 9.14.1: if bit 0 is zero, no function is supported. If
    // the n-th bit is enabled, function n is supported
    return result & 1 && result & (1 << sfnc);
}

static int handle_has_dsm_func(acpi_handle handle, const char muid[16], int revid, int sfnc) {
    u32 result = 0;

    // fail if the _DSM call failed
    if (acpi_call_dsm(handle, muid, revid, 0, 0, &result))
        return 0;

    // ACPI Spec v4 9.14.1: if bit 0 is zero, no function is supported. If
    // the n-th bit is enabled, function n is supported
    return result & 1 && result & (1 << sfnc);
}

static int bbswitch_optimus_dsm(void) {
    if (dsm_type == DSM_TYPE_OPTIMUS) {
        char args[] = {1, 0, 0, 3};
        u32 result = 0;

        if (acpi_call_dsm(dis_handle, acpi_optimus_dsm_muid, 0x100, 0x1A, args,
            &result)) {
            // failure
            return 1;
        }
        pr_debug("Result of Optimus _DSM call: %08X\n", result);
    }
    return 0;
}

static void get_dis_dev(void){
    struct pci_dev *pdev = NULL;

    if ((pdev = pci_get_device(vendor, device, pdev)) != NULL) {
        dis_dev = pdev;
    }
}

static int bbswitch_acpi_off(void) {
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

    acpi_status err = (acpi_status) 0x0;
    acpi_handle hnd;
    err = acpi_get_handle(NULL, (acpi_string) "\\_SB.PCI0.GPP0.PG00", &hnd);
    err = acpi_evaluate_object(hnd,"_OFF", NULL, &buffer);
    kfree(buffer.pointer);
    return 0;
}

static int bbswitch_acpi_on(void) {
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

    acpi_status err = (acpi_status) 0x0000;
    acpi_handle hnd;
    err = acpi_get_handle(NULL, (acpi_string) "\\_SB.PCI0.GPP0.PG00", &hnd);
    err = acpi_evaluate_object(hnd,"_ON", NULL, &buffer);
    kfree(buffer.pointer);

    return 0;
}

// Returns 1 if the card is disabled, 0 if enabled

// NOTE: With a fully disabling PCI device(disappears from 'lspci'), 
// you must check that this is '0' anytime you're wanting to interact with dis_dev.
// Otherwise, you will segfault.
static int is_card_disabled(void) {
    struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

    acpi_status err = (acpi_status) 0x0000;
    acpi_handle hnd;

    acpi_get_handle(NULL, (acpi_string) "\\_SB.PCI0.GPP0.PEGP", &hnd);
    err = acpi_evaluate_object(hnd,"SGST", NULL, &buffer);
    int gpustatus = ((union acpi_object *)buffer.pointer)->integer.value > 0 ? 0 : 1;
    if(gpustatus == 0){
        get_dis_dev();
        if(dis_dev == NULL){
            // Card is still powering on.
            gpustatus = -1;
        }
    }

    kfree(buffer.pointer);

    return gpustatus;
}

static void bbswitch_off(void) {
    if (is_card_disabled() == 1){
        pr_info("discrete graphics already disabled");
        return;
    }
    
    if (dis_dev->driver) {
        pr_warn("device %s is in use by driver '%s', refusing OFF\n",
            dis_dev_name, dis_dev->driver->name);
        return;
    }
    
    pr_info("disabling discrete graphics\n");

    if (bbswitch_acpi_off())
        pr_warn("The discrete card could not be disabled by an _OFF call\n");
    dis_dev = NULL;
}

static void bbswitch_on(void) {
    if (is_card_disabled() < 1)
        return;

    pr_info("enabling discrete graphics\n");

    if (bbswitch_acpi_on())
        pr_warn("The discrete card could not be enabled by an _ON call\n");
    
    int i = 0;
    while(dis_dev == NULL){
        msleep(500);
        i++;
        get_dis_dev();
        if(i > 4){
            break;
        }
    }
}

/* power bus so we can read PCI configuration space */
static void dis_dev_get(void) {
    if(is_card_disabled() < 1){
        int i = 0;
        while(dis_dev == NULL){
            msleep(500);
            i++;
            get_dis_dev();
            if(i > 4){
                if(dis_dev != NULL){
                    break;
                }
                else{
                    return;
                }
            }
        }
        if (dis_dev->bus && dis_dev->bus->self)
            pm_runtime_get_sync(&dis_dev->bus->self->dev);
    }
}

static void dis_dev_put(void) {
    if(is_card_disabled() == 0){
        if (dis_dev->bus && dis_dev->bus->self)
            pm_runtime_put_sync(&dis_dev->bus->self->dev);
    }
}

static ssize_t bbswitch_proc_write(struct file *fp, const char __user *buff,
    size_t len, loff_t *off) {
    char cmd[8];

    if (len >= sizeof(cmd))
        len = sizeof(cmd) - 1;

    if (copy_from_user(cmd, buff, len))
        return -EFAULT;

    dis_dev_get();

    if (strncmp(cmd, "OFF", 3) == 0)
        bbswitch_off();

    if (strncmp(cmd, "ON", 2) == 0)
        bbswitch_on();

    dis_dev_put();

    return len;
}

static int bbswitch_proc_show(struct seq_file *seqfp, void *p) {
    // show the card state. Example output: 0000:01:00:00 ON
    dis_dev_get();
    seq_printf(seqfp, "%s %s\n", dis_dev_name,
             is_card_disabled() > 0 ? "OFF" : "ON");
    dis_dev_put();
    return 0;
}
static int bbswitch_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, bbswitch_proc_show, NULL);
}

static int bbswitch_pm_handler(struct notifier_block *nbp,
    unsigned long event_type, void *p) {
    switch (event_type) {
    case PM_HIBERNATION_PREPARE:
    case PM_SUSPEND_PREPARE:
        pr_debug("Detected suspend");
        dis_dev_get();
        dis_before_suspend_disabled = is_card_disabled();
        // enable the device before suspend to avoid the PCI config space from
        // being saved incorrectly
        if (dis_before_suspend_disabled)
            pr_info("Enabling GPU for suspend");
            bbswitch_on();
        dis_dev_put();
        break;
    case PM_POST_HIBERNATION:
    case PM_POST_SUSPEND:
    case PM_POST_RESTORE:
        pr_debug("Detected restore");
        // after suspend, the card is on, but if it was off before suspend,
        // disable it again
        if (dis_before_suspend_disabled) {
            pr_info("Restoring GPU to off");
            dis_dev_get();
            bbswitch_off();
            dis_dev_put();
        }
        break;
    case PM_RESTORE_PREPARE:
        // deliberately don't do anything as it does not occur before suspend
        // nor hibernate, but before restoring a saved image. In that case,
        // either PM_POST_HIBERNATION or PM_POST_RESTORE will be called
        break;
    }
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static struct proc_ops bbswitch_fops = {
    .proc_open   = bbswitch_proc_open,
    .proc_read   = seq_read,
    .proc_write  = bbswitch_proc_write,
    .proc_lseek  = seq_lseek,
    .proc_release= single_release
};
#else
static struct file_operations bbswitch_fops = {
    .open   = bbswitch_proc_open,
    .read   = seq_read,
    .write  = bbswitch_proc_write,
    .llseek = seq_lseek,
    .release= single_release
};
#endif

static struct notifier_block nb = {
    .notifier_call = &bbswitch_pm_handler
};

static int __init bbswitch_init(void) {
    struct proc_dir_entry *acpi_entry;
    struct pci_dev *pdev = NULL;
    acpi_handle igd_handle = NULL;

    pr_info("version %s\n", BBSWITCH_VERSION);

    while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
        struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
        acpi_handle handle;
        int pci_class = pdev->class >> 8;

        if (pci_class != PCI_CLASS_DISPLAY_VGA &&
            pci_class != PCI_CLASS_DISPLAY_3D)
            continue;

        handle = ACPI_HANDLE(&pdev->dev);

        if (!handle) {
            pr_warn("cannot find ACPI handle for VGA device %s\n",
                dev_name(&pdev->dev));
            continue;
        }

        acpi_get_name(handle, ACPI_FULL_PATHNAME, &buf);

        if (pdev->vendor == PCI_VENDOR_ID_INTEL) {
            igd_handle = handle;
            pr_info("Found integrated VGA device %s: %s\n",
                dev_name(&pdev->dev), (char *)buf.pointer);
        } else {
            if(handle && handle_has_dsm_func(handle,acpi_optimus_dsm_muid, 0x100, 0x1A)){
                dis_dev = pdev;
                strlcpy(dis_dev_name, dev_name(&pdev->dev), sizeof(dis_dev_name));
                dis_handle = handle;
                vendor = pdev->vendor;
                device = pdev->device;
                pr_info("Found discrete VGA device %s: %s\n",
                    dis_dev_name, (char *)buf.pointer);
            }else{
                igd_handle = handle;
                pr_info("Found non-intel integrated VGA device %s: %s\n",
                    dev_name(&pdev->dev), (char *)buf.pointer);
            }
        }
        kfree(buf.pointer);
    }

    if (dis_dev == NULL) {
        pr_err("No discrete VGA device found\n");
        return -ENODEV;
    }

    if (!skip_optimus_dsm &&
            has_dsm_func(acpi_optimus_dsm_muid, 0x100, 0x1A)) {
        dsm_type = DSM_TYPE_OPTIMUS;
        pr_info("detected an Optimus _DSM function\n");
    } else if (has_dsm_func(acpi_nvidia_dsm_muid, 0x102, 0x3)) {
        dsm_type = DSM_TYPE_NVIDIA;
        pr_info("detected a nVidia _DSM function\n");
    } else {
       /* At least two Acer machines are known to use the intel ACPI handle
        * with the legacy nvidia DSM */
        dis_handle = igd_handle;
        if (dis_handle && has_dsm_func(acpi_nvidia_dsm_muid, 0x102, 0x3)) {
            dsm_type = DSM_TYPE_NVIDIA;
            pr_info("detected a nVidia _DSM function on the"
                " integrated video card\n");

        } else {
            pr_err("No suitable _DSM call found.\n");
            return -ENODEV;
        }
    }

    acpi_entry = proc_create("bbswitch", 0664, acpi_root_dir, &bbswitch_fops);
    if (acpi_entry == NULL) {
        pr_err("Couldn't create proc entry\n");
        return -ENOMEM;
    }

    dis_dev_get();

    if (is_card_disabled() < 1) {
        /* We think the card is enabled, so ensure the kernel does as well */
        if (pci_enable_device(dis_dev))
            pr_warn("failed to enable %s\n", dis_dev_name);
    }

    if (load_state == CARD_ON){
        bbswitch_on();
    } else if (load_state == CARD_OFF){
        bbswitch_off();
    }

    pr_info("Succesfully loaded. Discrete card %s is %s\n",
        dis_dev_name, is_card_disabled() > 0 ? "off" : "on");

    dis_dev_put();

    register_pm_notifier(&nb);

    return 0;
}

static void __exit bbswitch_exit(void) {
    remove_proc_entry("bbswitch", acpi_root_dir);

    dis_dev_get();

    if (unload_state == CARD_ON)
        bbswitch_on();
    else if (unload_state == CARD_OFF)
        bbswitch_off();

    pr_info("Unloaded. Discrete card %s is %s\n",
        dis_dev_name, is_card_disabled() > 0 ? "off" : "on");

    dis_dev_put();

    if (nb.notifier_call)
        unregister_pm_notifier(&nb);
}

module_init(bbswitch_init);
module_exit(bbswitch_exit);

/* vim: set sw=4 ts=4 et: */
