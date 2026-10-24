/* NVRAM for the wl core and for the stock Broadcom userland (nas, eapd, nvram CLI).
 *
 * One page-aligned NVRAM_SPACE buffer holds "name=value\0name=value\0\0". It is filled
 * from the kernel's bcm47xx_nvram copy of the flash partition, then overridden by
 * brcm/wl-nvram.txt (request_firmware, the radio calibration stock injected at boot),
 * and finally by whatever userland writes through /dev/nvram. Nothing is ever written
 * back to flash.
 *
 * /dev/nvram speaks the SDK protocol (arch/arm/plat-brcm/nvram_linux.c in 2.6.36):
 *   read(buf=name)  -> buf receives the byte offset of the value inside the mmap()ed buffer
 *   read(buf="")    -> buf receives all variables
 *   write("n=v")    -> set, write("n") -> unset
 *   ioctl(NVRAM_MAGIC) -> commit (no-op here)
 *   mmap            -> the buffer itself, read-only
 */
#include <typedefs.h>
#include <linuxver.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <bcmnvram.h>
#include <linux/bcm47xx_nvram.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>

#define WL_NVRAM_FILE "brcm/wl-nvram.txt"

static char *nvram_buf;		/* NVRAM_SPACE bytes, physically contiguous (mmap) */
static size_t nvram_len;	/* bytes in use, not counting the final list terminator */
static DEFINE_SPINLOCK(nvram_lock);
static int file_tried;
static int nvram_debug;
module_param(nvram_debug, int, 0644);
MODULE_PARM_DESC(nvram_debug, "log /dev/nvram lookups of missing variables");

char *nvram_get(const char *name);

/* ---- list primitives, caller holds nvram_lock ---- */
static char *nv_find(const char *name, size_t nlen)
{
	char *p;

	for (p = nvram_buf; p < nvram_buf + nvram_len && *p; p += strlen(p) + 1)
		if (!strncmp(p, name, nlen) && p[nlen] == '=')
			return p;
	return NULL;
}

static void nv_delete(const char *name, size_t nlen)
{
	char *p;

	while ((p = nv_find(name, nlen))) {
		size_t l = strlen(p) + 1;
		memmove(p, p + l, nvram_len - (p - nvram_buf) - l);
		nvram_len -= l;
		nvram_buf[nvram_len] = 0;
	}
}

static int nv_append(const char *name, size_t nlen, const char *value)
{
	size_t vlen = strlen(value), l = nlen + 1 + vlen + 1;

	if (nvram_len + l + 1 > NVRAM_SPACE)
		return -ENOSPC;
	memcpy(nvram_buf + nvram_len, name, nlen);
	nvram_buf[nvram_len + nlen] = '=';
	memcpy(nvram_buf + nvram_len + nlen + 1, value, vlen);
	nvram_len += l;
	nvram_buf[nvram_len - 1] = 0;
	nvram_buf[nvram_len] = 0;
	return 0;
}

/* ---- API used by the core (bcmutils getvar() falls back to nvram_get) ---- */
char *nvram_get(const char *name)
{
	unsigned long flags;
	char *p;

	if (!nvram_buf || !name)
		return NULL;
	spin_lock_irqsave(&nvram_lock, flags);
	p = nv_find(name, strlen(name));
	spin_unlock_irqrestore(&nvram_lock, flags);
	return p ? p + strlen(name) + 1 : NULL;
}

int nvram_set(const char *name, const char *value)
{
	unsigned long flags;
	size_t nlen;
	int ret;

	if (!nvram_buf || !name || !*name || strchr(name, '='))
		return -EINVAL;
	nlen = strlen(name);
	spin_lock_irqsave(&nvram_lock, flags);
	nv_delete(name, nlen);
	ret = value ? nv_append(name, nlen, value) : 0;
	spin_unlock_irqrestore(&nvram_lock, flags);
	return ret;
}

int nvram_unset(const char *name)
{
	return nvram_set(name, NULL);
}

int nvram_getall(char *buf, int count)
{
	unsigned long flags;

	if (!nvram_buf)
		return -ENOENT;
	spin_lock_irqsave(&nvram_lock, flags);
	if (count < (int)nvram_len + 2) {
		spin_unlock_irqrestore(&nvram_lock, flags);
		return -ENOMEM;
	}
	memcpy(buf, nvram_buf, nvram_len);
	buf[nvram_len] = 0;
	buf[nvram_len + 1] = 0;
	spin_unlock_irqrestore(&nvram_lock, flags);
	return 0;
}

int nvram_commit(void)
{
	return 0;	/* RAM only */
}

/* the SDK's nvram_init(si) hook: nothing to do */
int nvram_init(void *sih)
{
	return 0;
}

/* ---- fill from flash, then from the calibration file ---- */
static int nvram_load_flash(void)
{
	char *buf;
	size_t len;
	const char *p;
	int n = 0;

	buf = bcm47xx_nvram_get_contents(&len);
	if (IS_ERR_OR_NULL(buf)) {
		printk(KERN_WARNING "wl: no flash NVRAM available (%ld)\n", buf ? PTR_ERR(buf) : 0L);
		return 0;
	}
	for (p = buf; p < buf + len && *p; p += strlen(p) + 1) {
		const char *eq = strchr(p, '=');
		if (!eq)
			continue;
		if (nvram_len + strlen(p) + 2 > NVRAM_SPACE)
			break;
		memcpy(nvram_buf + nvram_len, p, strlen(p) + 1);
		nvram_len += strlen(p) + 1;
		n++;
	}
	nvram_buf[nvram_len] = 0;
	bcm47xx_nvram_release_contents(buf);
	printk(KERN_INFO "wl: %d variables from flash NVRAM\n", n);
	return 0;
}

int wlblob_nvram_load_file(struct device *dev)
{
	const struct firmware *fw;
	const char *p, *end, *nl;
	char *line;
	int ret, count = 0;

	if (file_tried || !nvram_buf)
		return 0;
	file_tried = 1;
	ret = request_firmware(&fw, WL_NVRAM_FILE, dev);
	if (ret) {
		printk(KERN_INFO "wl: no %s (%d), using flash NVRAM only\n", WL_NVRAM_FILE, ret);
		return 0;
	}
	line = kmalloc(1024, GFP_KERNEL);
	if (!line) {
		release_firmware(fw);
		return -ENOMEM;
	}
	p = fw->data;
	end = p + fw->size;
	while (p < end) {
		size_t l;
		char *eq;

		nl = memchr(p, '\n', end - p);
		l = nl ? (size_t)(nl - p) : (size_t)(end - p);
		while (l && (p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t'))
			l--;
		while (l && (*p == ' ' || *p == '\t')) {
			p++;
			l--;
		}
		if (l && l < 1024 && *p != '#') {
			memcpy(line, p, l);
			line[l] = 0;
			eq = strchr(line, '=');
			if (eq) {
				*eq = 0;
				if (!nvram_set(line, eq + 1))
					count++;
			}
		}
		p = nl ? nl + 1 : end;
	}
	kfree(line);
	release_firmware(fw);
	printk(KERN_INFO "wl: %d variables from %s\n", count, WL_NVRAM_FILE);
	return 0;
}

/* ---- /dev/nvram ---- */
static ssize_t dev_nvram_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	char tmp[100], *name = tmp, *value;
	unsigned long off;
	ssize_t ret;

	if (count + 1 > sizeof(tmp)) {
		name = kmalloc(count + 1, GFP_KERNEL);
		if (!name)
			return -ENOMEM;
	}
	if (copy_from_user(name, buf, count)) {
		ret = -EFAULT;
		goto done;
	}
	name[count] = 0;
	if (*name == 0) {
		ret = nvram_getall(name, count);
		if (ret == 0) {
			if (copy_to_user(buf, name, count))
				ret = -EFAULT;
			else
				ret = count;
		}
	} else {
		value = nvram_get(name);
		if (!value) {
			if (nvram_debug)
				printk(KERN_INFO "wl: nvram miss \"%s\" (%s)\n", name, current->comm);
			ret = 0;
			goto done;
		}
		off = (unsigned long)value - (unsigned long)nvram_buf;
		ret = sizeof(off);
		if (copy_to_user(buf, &off, sizeof(off)))
			ret = -EFAULT;
	}
done:
	if (name != tmp)
		kfree(name);
	return ret;
}

static ssize_t dev_nvram_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char tmp[512], *name = tmp, *value;
	ssize_t ret;

	if (count + 1 > sizeof(tmp)) {
		name = kmalloc(count + 1, GFP_KERNEL);
		if (!name)
			return -ENOMEM;
	}
	if (copy_from_user(name, buf, count)) {
		ret = -EFAULT;
		goto done;
	}
	name[count] = 0;
	value = name;
	name = strsep(&value, "=");
	ret = value ? nvram_set(name, value) : nvram_unset(name);
	if (ret == 0)
		ret = count;
done:
	if (name != tmp && !(name >= tmp && name < tmp + sizeof(tmp)))
		kfree(name);
	return ret;
}

static long dev_nvram_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (cmd != NVRAM_MAGIC)
		return -EINVAL;
	return nvram_commit();
}

static int dev_nvram_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > NVRAM_SPACE || vma->vm_pgoff)
		return -EINVAL;
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags & ~VM_WRITE);
	if (remap_pfn_range(vma, vma->vm_start, virt_to_phys(nvram_buf) >> PAGE_SHIFT,
			    size, vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}

static const struct file_operations dev_nvram_fops = {
	.owner = THIS_MODULE,
	.read = dev_nvram_read,
	.write = dev_nvram_write,
	.unlocked_ioctl = dev_nvram_ioctl,
	.compat_ioctl = dev_nvram_ioctl,
	.mmap = dev_nvram_mmap,
};

static struct miscdevice dev_nvram = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "nvram",
	.fops = &dev_nvram_fops,
	.mode = 0644,
};
static int dev_registered;

int wlblob_nvram_dev_init(void)
{
	int ret;

	nvram_buf = (char *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(NVRAM_SPACE));
	if (!nvram_buf)
		return -ENOMEM;
	nvram_len = 0;
	nvram_load_flash();
	ret = misc_register(&dev_nvram);
	if (ret)
		printk(KERN_WARNING "wl: /dev/nvram not available (%d)\n", ret);
	else
		dev_registered = 1;
	return 0;
}

void wlblob_nvram_exit(void)
{
	if (dev_registered)
		misc_deregister(&dev_nvram);
	dev_registered = 0;
	if (nvram_buf)
		free_pages((unsigned long)nvram_buf, get_order(NVRAM_SPACE));
	nvram_buf = NULL;
	nvram_len = 0;
	file_tried = 0;
}
