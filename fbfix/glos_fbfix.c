// glos_fbfix: give ARM's Mali driver, on a stock goodluckOS kernel, what the
// rebuilt boot image gives it: a double-height fbdev (drm_fbdev_overalloc=200,
// read when the display driver creates its framebuffer) and the framebuffer's
// physical address in FBIOGET_FSCREENINFO (the stock kernel hides it; the GPU
// renders into that address, and 0 freezes the system).
//
//   insmod glos_fbfix.ko
//   echo 200 > /sys/module/glos_fbfix/parameters/overalloc   # read when the
//      fbdev is (re)created
//   echo 1 > /sys/module/glos_fbfix/parameters/recreate      # the display
//      driver's fbdev client torn down and set up again (same as a rebind of
//      the display driver, without the panel's power cycle: ~50 ms, not ~1 s)
//   echo 1 > /sys/module/glos_fbfix/parameters/smem          # on the new fbdev
//
// Nothing here depends on a kernel build's addresses: the overalloc variable
// is found through the kernel's own /sys/module parameter table, and the
// framebuffer through /dev/fb0.
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kobject.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/string.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/clients/drm_client_setup.h>

void drm_client_dev_unregister(struct drm_device *dev);
#ifndef DRM_MAJOR
#define DRM_MAJOR 226
#endif

// Private to kernel/params.c, same in 7.2.
struct param_attribute {
	struct module_attribute mattr;
	const struct kernel_param *param;
};
struct module_param_attrs {
	unsigned int num;
	struct attribute_group grp;
	struct param_attribute attrs[];
};

static const char *param_basename(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : name;
}

// &drm_fbdev_overalloc (static in drm_fb_helper.c), through the parameter
// attributes of the built-in module's kobject (/sys/module/drm_kms_helper).
static int *find_overalloc(void)
{
	struct kset *kset = THIS_MODULE->mkobj.kobj.kset;
	struct kobject *kobj;
	struct module_kobject *mk;
	int *p = NULL;
	unsigned int i;

	if (!kset)
		return NULL;
	kobj = kset_find_obj(kset, "drm_kms_helper");
	if (!kobj)
		return NULL;
	mk = container_of(kobj, struct module_kobject, kobj);
	if (mk->mp) {
		for (i = 0; i < mk->mp->num; i++) {
			const struct kernel_param *kp = mk->mp->attrs[i].param;
			if (kp && kp->name && !strcmp(param_basename(kp->name), "drm_fbdev_overalloc")) {
				p = kp->arg;
				break;
			}
		}
	}
	kobject_put(kobj);
	return p;
}

static int overalloc_set(const char *val, const struct kernel_param *kp)
{
	int want, *p;
	int err = kstrtoint(val, 0, &want);

	if (err)
		return err;
	if (want < 100 || want > 400)
		return -EINVAL;
	p = find_overalloc();
	if (!p)
		return -ENOENT;
	if (*p < 100 || *p > 400) {
		pr_err("glos_fbfix: drm_fbdev_overalloc at %px reads %d, not touching it\n", p, *p);
		return -EIO;
	}
	pr_info("glos_fbfix: drm_fbdev_overalloc %d -> %d (at %px); recreate the fbdev to apply\n", *p, want, p);
	*p = want;
	return 0;
}

static int overalloc_get(char *buf, const struct kernel_param *kp)
{
	int *p = find_overalloc();

	if (!p)
		return -ENOENT;
	return scnprintf(buf, PAGE_SIZE, "%d\n", *p);
}

static const struct kernel_param_ops overalloc_ops = {
	.set = overalloc_set,
	.get = overalloc_get,
};
module_param_cb(overalloc, &overalloc_ops, NULL, 0600);
MODULE_PARM_DESC(overalloc, "drm_kms_helper.drm_fbdev_overalloc, read at the next display driver bind");

// /dev/fb0's fb_info, as fb_open leaves it in the file.
static struct file *open_fb0(struct fb_info **infop)
{
	struct file *f = filp_open("/dev/fb0", O_RDWR, 0);
	struct inode *inode;

	if (IS_ERR(f))
		return f;
	inode = file_inode(f);
	if (!S_ISCHR(inode->i_mode) || imajor(inode) != FB_MAJOR || !f->private_data) {
		filp_close(f, NULL);
		return ERR_PTR(-ENODEV);
	}
	*infop = f->private_data;
	return f;
}

static int smem_set(const char *val, const struct kernel_param *kp)
{
	bool show;
	struct fb_info *info;
	struct file *f;
	int err = kstrtobool(val, &show);

	if (err)
		return err;
	f = open_fb0(&info);
	if (IS_ERR(f))
		return PTR_ERR(f);
	lock_fb_info(info);
	mutex_lock(&info->mm_lock);
	if (show) {
		if (!info->screen_buffer || is_vmalloc_addr(info->screen_buffer)) {
			err = -ENXIO;
		} else {
			// What drm_fbdev_dma does when drm_leak_fbdev_smem is set.
			info->fix.smem_start = page_to_phys(virt_to_page(info->screen_buffer));
			info->flags &= ~FBINFO_HIDE_SMEM_START;
		}
	} else {
		info->flags |= FBINFO_HIDE_SMEM_START;
	}
	pr_info("glos_fbfix: fb0 %s smem_start %#lx len %u (%s)\n",
		(info->flags & FBINFO_HIDE_SMEM_START) ? "hides" : "reports",
		info->fix.smem_start, info->fix.smem_len, err ? "failed" : "ok");
	mutex_unlock(&info->mm_lock);
	unlock_fb_info(info);
	filp_close(f, NULL);
	return err;
}

static int smem_get(char *buf, const struct kernel_param *kp)
{
	struct fb_info *info;
	struct file *f = open_fb0(&info);
	int n;

	if (IS_ERR(f))
		return PTR_ERR(f);
	lock_fb_info(info);
	n = scnprintf(buf, PAGE_SIZE, "%d\n", !(info->flags & FBINFO_HIDE_SMEM_START));
	unlock_fb_info(info);
	filp_close(f, NULL);
	return n;
}

static const struct kernel_param_ops smem_ops = {
	.set = smem_set,
	.get = smem_get,
};
module_param_cb(smem, &smem_ops, NULL, 0600);
MODULE_PARM_DESC(smem, "1: fb0 reports its physical address (FBIOGET_FSCREENINFO), 0: hides it");

// The display's DRM device, through its /dev/dri/card* node (the file holds
// a drm_file, whose minor points at the device). The file stays open while
// the caller works and is closed by the caller.
static struct drm_device *find_display(struct file **fp)
{
	char path[24];
	int n;

	for (n = 0; n < 4; n++) {
		struct file *f;
		struct inode *inode;
		struct drm_file *priv;
		struct drm_device *dev;

		snprintf(path, sizeof(path), "/dev/dri/card%d", n);
		f = filp_open(path, O_RDWR, 0);
		if (IS_ERR(f))
			continue;
		inode = file_inode(f);
		priv = f->private_data;
		dev = (S_ISCHR(inode->i_mode) && imajor(inode) == DRM_MAJOR && priv && priv->minor) ? priv->minor->dev : NULL;
		if (dev && dev->driver && dev->driver->name && !strcmp(dev->driver->name, "sun4i-drm")) {
			*fp = f;
			return dev;
		}
		filp_close(f, NULL);
	}
	return NULL;
}

// Tear the display driver's fbdev client down and set it up again: the
// framebuffer is reallocated with the current drm_fbdev_overalloc, exactly
// as a rebind of the display driver would do it, but the panel and the
// display pipeline stay up.
static int recreate_set(const char *val, const struct kernel_param *kp)
{
	struct file *f = NULL;
	struct drm_device *dev = find_display(&f);

	if (!dev)
		return -ENODEV;
	pr_info("glos_fbfix: recreating %s's fbdev\n", dev->driver->name);
	drm_client_dev_unregister(dev);
	drm_client_setup(dev, NULL);
	filp_close(f, NULL);
	return 0;
}

static const struct kernel_param_ops recreate_ops = {
	.set = recreate_set,
	.get = NULL,
};
module_param_cb(recreate, &recreate_ops, NULL, 0200);
MODULE_PARM_DESC(recreate, "write 1: the display driver's fbdev is torn down and created again");

static int __init glos_fbfix_init(void)
{
	int *p = find_overalloc();

	if (p)
		pr_info("glos_fbfix: drm_fbdev_overalloc is %d (at %px)\n", *p, p);
	else
		pr_warn("glos_fbfix: drm_fbdev_overalloc not found under /sys/module/drm_kms_helper\n");
	return 0;
}

static void __exit glos_fbfix_exit(void)
{
}

module_init(glos_fbfix_init);
module_exit(glos_fbfix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("double-height fbdev and visible framebuffer address for ARM's Mali driver on a stock goodluckOS kernel");
