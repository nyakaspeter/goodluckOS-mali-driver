// glos-gpu: run an app on ARM's closed Mali driver; Mesa lima stays the default.
//   glos-gpu switch mali|lima       switch the GPU driver
//   glos-gpu run mali <cmd> [args]  switch to ARM's driver, run <cmd> with ARM's
//                                   libraries, switch back to lima when it
//                                   exits (<cmd> isn't started if the switch
//                                   fails)
//   glos-gpu run lima <cmd> [args]  switch to lima, then run <cmd>
// Everything it needs sits next to it (a game folder's glos/ directory):
// glos-gpu-switch, mali.ko, glos_fbfix.ko and lib/ (ARM's libraries, outside
// the normal library path: their libEGL/libGLESv2 would otherwise replace
// Mesa's for every app). Nothing is installed in the system.
// The switching needs root. The folder lives on a FAT partition, so the
// switch can't be setuid: it is run through busybox su with goodluckOS's root
// password (GLOS_ROOT_PASSWORD, default "root"), fed on a pseudo-terminal.
// This program is deliberately not setuid (and not a shell script: goodluckOS's
// busybox is setuid), so the environment, LD_LIBRARY_PATH included, reaches
// the command untouched.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char dir[PATH_MAX];
static pid_t child;

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
	return 0;
}

// The switch, as root. Already root: run it directly. Otherwise through su,
// answering its password prompt on a pseudo-terminal; its output goes on to
// stderr.
static int
switchTo(const char *args)
{
	char cmd[PATH_MAX + 64];
	snprintf(cmd, sizeof(cmd), "%s/glos-gpu-switch %s", dir, args);
	int master = -1;
	pid_t pid;
	if(geteuid() == 0){
		pid = fork();
		if(pid == 0){
			execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
			_exit(127);
		}
	}else{
		pid = forkpty(&master, NULL, NULL, NULL);
		if(pid == 0){
			execlp("su", "su", "-c", cmd, (char*)NULL);
			_exit(127);
		}
	}
	if(pid < 0){
		perror("glos-gpu: fork");
		return 1;
	}
	if(master >= 0){
		const char *password = getenv("GLOS_ROOT_PASSWORD");
		if(password == NULL)
			password = "root";
		int answered = 0;
		char buf[512], line[512];
		size_t len = 0;
		for(;;){
			struct pollfd p = {master, POLLIN, 0};
			if(poll(&p, 1, 10000) <= 0)
				break;
			ssize_t n = read(master, buf, sizeof(buf) - 1);
			if(n <= 0)
				break;
			buf[n] = 0;
			if(!answered){
				if(len + n < sizeof(line) - 1){
					memcpy(line + len, buf, n);
					len += n;
					line[len] = 0;
				}
				if(strstr(line, "assword") != NULL){
					char reply[256];
					snprintf(reply, sizeof(reply), "%s\n", password);
					if(write(master, reply, strlen(reply)) < 0)
						break;
					answered = 1;
				}
			}else{
				// the echoed newline, then the switch's own messages
				fputs(buf, stderr);
			}
		}
		close(master);
	}
	int status;
	if(waitpid(pid, &status, 0) < 0)
		return 1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// Pass a quit request on to the app; switch back once it has exited.
static void
forward(int sig)
{
	if(child > 0)
		kill(child, sig);
}

static int
usage(void)
{
	fprintf(stderr, "usage: glos-gpu switch mali|lima\n"
	                "       glos-gpu run mali|lima <command> [args]\n");
	return 2;
}

int
main(int argc, char **argv)
{
	if(argc < 3 || (strcmp(argv[2], "mali") != 0 && strcmp(argv[2], "lima") != 0))
		return usage();
	int run = strcmp(argv[1], "run") == 0;
	if(!(run && argc >= 4) && !(strcmp(argv[1], "switch") == 0 && argc == 3))
		return usage();
	if(ownDir() < 0){
		perror("glos-gpu: /proc/self/exe");
		return 1;
	}
	const char *driver = argv[2];
	char args[64];
	// run mali: the switch leaves a watcher behind that switches back to lima
	// if this process dies without doing so (the app killed with its group).
	if(run && strcmp(driver, "mali") == 0)
		snprintf(args, sizeof(args), "mali --watch %d", (int)getpid());
	else
		snprintf(args, sizeof(args), "%s", driver);
	if(switchTo(args) != 0){
		fprintf(stderr, "glos-gpu: couldn't switch to %s\n", driver);
		return 1;
	}
	if(!run)
		return 0;
	char **cmd = argv + 3;
	if(strcmp(driver, "lima") == 0){
		execvp(cmd[0], cmd);
		perror(cmd[0]);
		return 127;
	}
	const char *old = getenv("LD_LIBRARY_PATH");
	char *path = malloc(strlen(dir) + 5 + (old ? strlen(old) + 1 : 0));
	strcpy(path, dir);
	strcat(path, "/lib");
	if(old && *old){
		strcat(path, ":");
		strcat(path, old);
	}
	setenv("LD_LIBRARY_PATH", path, 1);
	signal(SIGTERM, forward);
	signal(SIGINT, forward);
	signal(SIGHUP, forward);
	child = fork();
	if(child == 0){
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		execvp(cmd[0], cmd);
		perror(cmd[0]);
		_exit(127);
	}
	int status = 0;
	if(child > 0)
		while(waitpid(child, &status, 0) < 0 && errno == EINTR)
			;
	child = 0;
	switchTo("lima");
	if(WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
