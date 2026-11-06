// fbinfo [/dev/fb0]: what FBIOGET_FSCREENINFO / FBIOGET_VSCREENINFO report
// (smem_start is what ARM's Mali driver renders into; 0 on a stock kernel).
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	int fd = open(dev, O_RDONLY);
	if (fd < 0) { perror(dev); return 1; }
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) { perror("ioctl"); return 1; }
	printf("%s: %s smem_start 0x%lx smem_len %u line_length %u res %ux%u virtual %ux%u bpp %u\n",
	       dev, fix.id, fix.smem_start, fix.smem_len, fix.line_length,
	       var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.bits_per_pixel);
	return 0;
}
