// glos-gpu-switch: switch the Mali-400 between Mesa's lima driver and ARM's
// closed driver (mali.ko). Run as root by glos-gpu (through su); it only ever
// does these fixed operations:
//   glos-gpu-switch mali [--watch PID]
//       prepare the framebuffer (below), unbind lima, load mali.ko (next to
//       this program), open /dev/mali. With --watch, leave a watcher behind
//       (outside the app's cgroup, so a kill of the app's group doesn't take
//       it) that switches back to lima once PID is gone, if that process did
//       not do it itself.
//   glos-gpu-switch lima
//       unload mali.ko, rebind lima, and put the framebuffer back to stock
//       (single-height, address hidden, glos_fbfix.ko unloaded): a game's
//       exit leaves the kernel exactly as it booted. The double-height
//       framebuffer alone cost lima apps 1.2 MB of the 16 MB display pool
//       (RetroArch failed to allocate its buffers with it).
// On a stock goodluckOS kernel the fbdev is single-height and hides its
// physical address, and ARM's driver needs both (it renders into that address;
// 0 freezes the system). 'mali' then loads glos_fbfix.ko (next to this
// program), sets drm_fbdev_overalloc=200, has the display driver's fbdev
// recreated double-height (the module does it; a rebind of the display driver
// is the fallback), and has fb0 report its address: the
// same as a rebuilt boot image would, for the game's run. Nothing on disk changes.
// Both also blank the framebuffer: ARM's driver draws into it, and the screen
// falls back to it whenever Puppy exits, so an app's last frame would
// otherwise flash up at the next launch.
// Each side waits (up to 5 s) for the other driver's users to go away first:
// unbinding lima under a process that is still exiting leaves it stuck holding
// the display (DRM master), and Puppy can't start again until a reboot.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define GPU "1c40000.gpu"
#define LIMA "/sys/bus/platform/drivers/lima/"
#define FBFIX_PARAM "/sys/module/glos_fbfix/parameters/"
#define DISPLAY "/sys/bus/platform/drivers/sun4i-drm/"
#define DISPLAY_DEV "display-engine"
#define ROOT_CGROUP "/sys/fs/cgroup/cgroup.procs"
#define APPCTL "/usr/local/bin/appctl"
#define WAIT_TRIES 100   /* x 50 ms */

static char dir[PATH_MAX];      /* where this program, mali.ko and glos_fbfix.ko are */
static char modulePath[PATH_MAX], fbfixPath[PATH_MAX];

static int
writeFile(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if(fd < 0)
		return -1;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
	close(fd);
	return r;
}

static void
blankFramebuffer(void)
{
	static const char zero[4096];
	int fd = open("/dev/fb0", O_WRONLY | O_CLOEXEC);
	if(fd < 0)
		return;
	while(write(fd, zero, sizeof(zero)) > 0)
		;
	close(fd);
}

static int
loadModule(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return -1;
	int r = syscall(SYS_finit_module, fd, "", 0);
	if(r < 0 && errno == EEXIST)
		r = 0;
	close(fd);
	return r;
}

// fb0's height, virtual height and physical address as apps see them.
static int
fbState(int *yres, int *virtualHeight, unsigned long *address)
{
	int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return -1;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int r = ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 ? 0 : -1;
	close(fd);
	if(r == 0){
		*yres = var.yres;
		*virtualHeight = var.yres_virtual;
		*address = fix.smem_start;
	}
	return r;
}

// Recreate the fbdev with the current drm_fbdev_overalloc: through the
// module's 'recreate' (the display driver's fbdev client set up again, ~50
// ms), falling back to a rebind of the whole display driver (~1 s, the
// panel's power cycle included) if that is refused.
static int
recreateFramebuffer(int *yres, int *virtualHeight, unsigned long *address)
{
	if(writeFile(FBFIX_PARAM "recreate", "1") < 0){
		fprintf(stderr, "glos-gpu: fbdev recreate refused (%s), rebinding the display driver\n", strerror(errno));
		if(writeFile(DISPLAY "unbind", DISPLAY_DEV) < 0 || writeFile(DISPLAY "bind", DISPLAY_DEV) < 0){
			perror("glos-gpu: rebind " DISPLAY_DEV);
			return -1;
		}
	}
	for(int i = 0; i < WAIT_TRIES; i++){
		if(fbState(yres, virtualHeight, address) == 0)
			return 0;
		usleep(20000);
	}
	return -1;
}

// Double-height fbdev with a visible physical address, as ARM's driver needs.
// Nothing to do on the rebuilt boot image or after the first call of a boot.
static int
prepareFramebuffer(void)
{
	int yres, virtualHeight;
	unsigned long address;
	if(fbState(&yres, &virtualHeight, &address) < 0){
		perror("glos-gpu: /dev/fb0");
		return -1;
	}
	if(virtualHeight >= 2 * yres && address != 0)
		return 0;
	if(loadModule(fbfixPath) < 0){
		perror(fbfixPath);
		return -1;
	}
	if(virtualHeight < 2 * yres){
		if(writeFile(FBFIX_PARAM "overalloc", "200") < 0){
			perror("glos-gpu: set drm_fbdev_overalloc");
			return -1;
		}
		if(recreateFramebuffer(&yres, &virtualHeight, &address) < 0 || virtualHeight < 2 * yres){
			fprintf(stderr, "glos-gpu: fb0 is not double-height after the rebind\n");
			return -1;
		}
	}
	if(address == 0){
		if(writeFile(FBFIX_PARAM "smem", "1") < 0 || fbState(&yres, &virtualHeight, &address) < 0 || address == 0){
			fprintf(stderr, "glos-gpu: fb0 still hides its physical address\n");
			return -1;
		}
	}
	fprintf(stderr, "glos-gpu: framebuffer prepared (%d lines virtual, address %#lx)\n", virtualHeight, address);
	return 0;
}

// Undo prepareFramebuffer: the stock single-height fbdev with its address
// hidden (a freshly created one hides it on the stock kernel), glos_fbfix.ko
// unloaded. Nothing to do when the module was never loaded (rebuilt kernel).
static int
restoreFramebuffer(void)
{
	if(access("/sys/module/glos_fbfix", F_OK) != 0)
		return 0;
	int yres = 0, virtualHeight = 0;
	unsigned long address = 0;
	// Also when there is no fb0 at all: a double-height recreate that failed
	// (the display memory pool too fragmented, ret -12) leaves none, and the
	// launcher came back to a dead screen.
	if(fbState(&yres, &virtualHeight, &address) < 0 || virtualHeight > yres || address != 0){
		if(writeFile(FBFIX_PARAM "overalloc", "100") < 0)
			perror("glos-gpu: reset drm_fbdev_overalloc");
		recreateFramebuffer(&yres, &virtualHeight, &address);
	}
	int r, tries = 0;
	while((r = syscall(SYS_delete_module, "glos_fbfix", O_NONBLOCK)) < 0 &&
	      (errno == EWOULDBLOCK || errno == EBUSY) && ++tries < WAIT_TRIES)
		usleep(50000);
	if(r < 0 && errno != ENOENT)
		perror("glos-gpu: unload glos_fbfix.ko");
	if(access("/dev/fb0", F_OK) != 0 || virtualHeight > yres || address != 0 || access("/sys/module/glos_fbfix", F_OK) == 0){
		fprintf(stderr, "glos-gpu: framebuffer NOT back to stock (%d lines virtual, address %#lx)\n", virtualHeight, address);
		return -1;
	}
	fprintf(stderr, "glos-gpu: framebuffer back to stock (%d lines virtual, address hidden, glos_fbfix.ko unloaded)\n", virtualHeight);
	return 0;
}

static int
limaBound(void)
{
	return access(LIMA GPU, F_OK) == 0;
}

// Does any process have one of lima's device nodes (card*, renderD*) open?
static int
limaInUse(void)
{
	char nodes[4][32];
	int n = 0;
	DIR *d = opendir(LIMA GPU "/drm");
	if(d == NULL)
		return 0;
	for(struct dirent *e; (e = readdir(d)) != NULL && n < 4;)
		if(e->d_name[0] != '.')
			snprintf(nodes[n++], sizeof(nodes[0]), "/dev/dri/%s", e->d_name);
	closedir(d);

	int busy = 0;
	DIR *proc = opendir("/proc");
	for(struct dirent *pe; proc && !busy && (pe = readdir(proc)) != NULL;){
		if(pe->d_name[0] < '1' || pe->d_name[0] > '9')
			continue;
		char path[300], link[64];
		snprintf(path, sizeof(path), "/proc/%s/fd", pe->d_name);
		DIR *fds = opendir(path);
		for(struct dirent *fe; fds && !busy && (fe = readdir(fds)) != NULL;){
			snprintf(path, sizeof(path), "/proc/%s/fd/%s", pe->d_name, fe->d_name);
			ssize_t len = readlink(path, link, sizeof(link) - 1);
			if(len <= 0)
				continue;
			link[len] = 0;
			for(int i = 0; i < n; i++)
				if(strcmp(link, nodes[i]) == 0)
					busy = 1;
		}
		if(fds)
			closedir(fds);
	}
	if(proc)
		closedir(proc);
	return busy;
}

static int
toMali(void)
{
	for(int i = 0; i < WAIT_TRIES && limaBound() && limaInUse(); i++)
		usleep(50000);
	if(limaBound() && limaInUse()){
		fprintf(stderr, "glos-gpu: lima is still in use, staying on lima\n");
		return 1;
	}
	if(prepareFramebuffer() < 0){
		restoreFramebuffer();
		return 1;
	}
	if(limaBound() && writeFile(LIMA "unbind", GPU) < 0){
		perror("glos-gpu: unbind lima");
		return 1;
	}
	int fd = open(modulePath, O_RDONLY | O_CLOEXEC);
	if(fd < 0){
		perror(modulePath);
		goto fail;
	}
	if(syscall(SYS_finit_module, fd, "", 0) < 0 && errno != EEXIST){
		perror("glos-gpu: load mali.ko");
		close(fd);
		goto fail;
	}
	close(fd);
	for(int i = 0; i < 50 && access("/dev/mali", F_OK) != 0; i++)
		usleep(20000);
	if(chmod("/dev/mali", 0666) < 0){
		perror("glos-gpu: /dev/mali");
		goto fail;
	}
	blankFramebuffer();
	return 0;
fail:
	writeFile(LIMA "bind", GPU);
	restoreFramebuffer();
	return 1;
}

static int
toLima(void)
{
	// mali.ko can't unload while the app is still exiting; wait for it.
	int r, tries = 0;
	while((r = syscall(SYS_delete_module, "mali", O_NONBLOCK)) < 0 &&
	      (errno == EWOULDBLOCK || errno == EBUSY) && ++tries < WAIT_TRIES)
		usleep(50000);
	if(r < 0 && errno != ENOENT)
		perror("glos-gpu: unload mali.ko");
	blankFramebuffer();
	if(!limaBound() && writeFile(LIMA "bind", GPU) < 0){
		perror("glos-gpu: bind lima");
		return 1;
	}
	return restoreFramebuffer() < 0 ? 1 : 0;
}

// A detached root process outside the app's cgroup: when PID (glos-gpu) is
// gone and ARM's driver or the fix module is still loaded (the app was killed
// with its group), put everything back. Normally glos-gpu does that itself
// before exiting and the watcher just leaves.
static void
watch(pid_t pid)
{
	pid_t p = fork();
	if(p < 0)
		return;
	if(p > 0){
		waitpid(p, NULL, 0);
		return;
	}
	setsid();
	if(fork() != 0)
		_exit(0);
	char buf[32];
	snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	if(writeFile(ROOT_CGROUP, buf) < 0)
		fprintf(stderr, "glos-gpu: watcher stays in the app's cgroup: %s\n", strerror(errno));
	int null = open("/dev/null", O_RDWR);
	dup2(null, 0);
	dup2(null, 1);
	while(kill(pid, 0) == 0 || errno != ESRCH)
		usleep(200000);
	if(access("/sys/module/mali", F_OK) == 0 || access("/dev/mali", F_OK) == 0 || access("/sys/module/glos_fbfix", F_OK) == 0){
		// appd starts the launcher as soon as the app's cgroup is empty, and
		// the display rebind would pull the screen from under it: stop the
		// launcher (and appd) first, reset, start them again.
		fprintf(stderr, "glos-gpu: app gone with ARM's driver loaded, resetting to stock\n");
		if(system(APPCTL " stop >/dev/null 2>&1") != 0)
			fprintf(stderr, "glos-gpu: " APPCTL " stop failed\n");
		usleep(500000);
		toLima();
		if(system(APPCTL " start >/dev/null 2>&1") != 0)
			fprintf(stderr, "glos-gpu: " APPCTL " start failed\n");
	}
	_exit(0);
}

static int
ownDir(void)
{
	ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
	if(n <= 0)
		return -1;
	dir[n] = 0;
	char *slash = strrchr(dir, '/');
	if(slash == NULL)
		return -1;
	*slash = 0;
	snprintf(modulePath, sizeof(modulePath), "%s/mali.ko", dir);
	snprintf(fbfixPath, sizeof(fbfixPath), "%s/glos_fbfix.ko", dir);
	return 0;
}

int
main(int argc, char **argv)
{
	if(ownDir() < 0){
		perror("glos-gpu: /proc/self/exe");
		return 1;
	}
	if(argc >= 2 && strcmp(argv[1], "mali") == 0 && (argc == 2 || (argc == 4 && strcmp(argv[2], "--watch") == 0))){
		int r = toMali();
		if(r == 0 && argc == 4)
			watch((pid_t)atoi(argv[3]));
		return r;
	}
	if(argc == 2 && strcmp(argv[1], "lima") == 0)
		return toLima();
	fprintf(stderr, "usage: glos-gpu-switch mali [--watch PID] | lima\n");
	return 2;
}
