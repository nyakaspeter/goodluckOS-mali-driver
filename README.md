# goodluckOS-mali-driver — ARM's Mali driver for goodluckOS, as a portable driver set

The GA36-MB's Mali-400 MP2 runs 2–3× faster on ARM's closed r6p2 driver than
on Mesa's lima (re3 driving: 18–20 fps against 7–9). This repo ports that
driver to goodluckOS's Linux 7.2 kernel and delivers it as **`glos/`**, a
1.7 MB directory a game folder carries and uses for its own run only. The
console itself stays stock: no boot image, no files outside the game folder,
and the kernel is back to its boot state when the game exits.

## `glos/` — the driver set (what each port packages)

| File | What |
|---|---|
| `glos-gpu` | What a port's `start.sh` calls: `exec ./glos/glos-gpu run mali <game> [args]`. Finds everything next to itself, switches to ARM's driver, runs the game with `glos/lib` first in `LD_LIBRARY_PATH`, switches back when it exits. Not setuid and not a shell script, so the caller's environment reaches the game. `glos-gpu switch mali\|lima` switches without running anything. |
| `glos-gpu-switch` | The root part. The game folder is on a FAT partition, so nothing in it can be setuid: `glos-gpu` runs this through busybox `su -c` with goodluckOS's root password (`root`; `GLOS_ROOT_PASSWORD` overrides), answered on a pseudo-terminal it opens for `su`. |
| `mali.ko` | ARM's kernel driver (sunxi-mali r6p2) built for goodluckOS's kernel. |
| `glos_fbfix.ko` | Gives the kernel's framebuffer the shape ARM's driver needs, for the game's run (below). |
| `lib/libMali.so` | ARM's user-space driver (r6p2, fbdev, GLES 2.0 / EGL), with the executable-stack flag cleared: glibc ≥ 2.41 refuses to `dlopen` a library that asks for one. `stubs/libMali.so.orig` is the untouched blob. Not in the repo (ARM's, proprietary): `./get-libmali.sh path/to/libMali.so` takes the r6p2-01rel0 fbdev armhf build (Bootlin publishes ARM's r6p2 builds with ARM's licence at github.com/bootlin/mali-blobs), checks its md5 and writes both. |
| `lib/libEGL.so.1`, `lib/libGLESv2.so.2` | Stubs that forward to libMali, under the names the games open. Outside the normal library path on purpose, so they never replace Mesa's for other apps. |
| `testing/fbinfo` | Probe: what `/dev/fb0` reports (sizes, physical address). Not shipped. |

`glos-gpu-switch mali` (at a game start, about 0.5 s in all):
1. Waits up to 5 s for lima's users to exit (unbinding lima under an exiting
   Puppy left it holding the display until a reboot).
2. Framebuffer: loads `glos_fbfix.ko`, sets `drm_fbdev_overalloc=200`, has
   the display driver's fbdev recreated (double-height now) and makes it
   report its physical address. ~40 ms.
3. Unbinds lima from the GPU, loads `mali.ko`, `chmod 666 /dev/mali`, blanks
   the framebuffer (ARM's driver draws into it, and the screen falls back to
   it whenever Puppy exits, so an app's last frame would show at the next
   launch). ARM's driver takes ~0.3 s to initialise.
4. With `--watch <pid>` (what `run mali` passes) leaves a root watcher behind
   in the root cgroup. `appctl` kills a game together with its whole cgroup
   (Mode+Start+Select), `glos-gpu` included; the watcher sees the pid go
   with the driver still loaded, stops the launcher supervisor (`appctl stop`,
   because it would start Puppy at once and the framebuffer reset would pull
   the screen from under it), does the `lima` steps, starts it again. About
   1.4 s; the launcher was back every time.

`glos-gpu-switch lima` (at exit, about 0.3 s): unloads `mali.ko` (waits for
the game's handles to close), blanks, rebinds lima, sets `drm_fbdev_overalloc`
back to 100, recreates the stock single-height framebuffer (its address hidden
again, as a fresh one is), unloads `glos_fbfix.ko`. Verified after a normal
exit and after a kill: `virtual_size` 640,480, address 0, no modules, lima
bound, display memory freed. Nothing of this may stay: the double-height
framebuffer alone holds 2.4 MB of the 16 MB display pool instead of 1.2, which
is enough to stop a lima app from allocating its buffers (RetroArch did).

When `mali` fails (no double-height framebuffer, lima still busy, `mali.ko`
refused) it puts the stock framebuffer back before returning, and the `lima`
restore recreates the framebuffer also when there is none at all. On
2026-10-31, after a day of game launches, the display pool was too fragmented
for the double-height framebuffer (`Failed to setup emulation (ret=-12)`, the
stock kernel's lima leaks a little display memory per launch); the failed
recreate left no `/dev/fb0`, and the launcher came back to a dead screen until
the framebuffer was recreated by hand. A reboot frees the pool.

## Why the framebuffer needs fixing, and how `glos_fbfix.ko` does it

ARM's fbdev driver renders into the physical address `FBIOGET_FSCREENINFO`
reports and flips between two halves of a double-height framebuffer. The
stock kernel hides the address (it reports 0, and the GPU then writes the
frame to physical 0: the whole system freezes on the first job) and
allocates a single-height one; both are boot-time settings
(`CONFIG_DRM_FBDEV_LEAK_PHYS_SMEM`, `drm_fbdev_overalloc`) that a stock image
cannot be given. The module does both at runtime, without any address from a
particular kernel build (the flashed kernel and a rebuild of it differ by a
4-byte shift from the build host's name in the banner, so fixed addresses
were ruled out):

- `overalloc`: finds `drm_fbdev_overalloc` through the kernel's own
  `/sys/module/drm_kms_helper` parameter table and writes it.
- `recreate`: tears the display driver's fbdev client down and sets it up
  again (`drm_client_dev_unregister` + `drm_client_setup` on the `sun4i-drm`
  device, found through `/dev/dri/card*`), which reallocates the framebuffer
  with the new height while the panel and the display pipeline stay up.
  ~40 ms. (A rebind of the whole display driver does the same in ~1 s with the
  panel's power cycle and init sequence; the helper falls back to it if
  `recreate` is refused.)
- `smem`: makes fb0 report its physical address, computed as the kernel's
  own option does (`page_to_phys(virt_to_page(screen_buffer))`).

Facts about the stock kernel (7.2.9, Debian bookworm GCC 12): modules load,
no module signing, `vermagic` `7.2.9-dirty SMP preempt mod_unload ARMv7 p2v8`;
the stock 16 MB CMA pool is enough (8.7 MB free with a game running).

## Repo layout

| Path | What |
|---|---|
| `glos/` | The deliverable above. Committed as built, so a port can package without building anything. |
| `assemble-glos.sh` | Rebuilds the helpers and the fix module and refreshes `glos/` (needs the `glos-kernel:ready` image below). |
| `make-kernel-image.sh` | Creates the `glos-kernel:ready` image (below). |
| `helper/` | `glos-gpu.c`, `glos-gpu-switch.c`, `build.sh`. |
| `fbfix/` | `glos_fbfix.c`, `fbinfo.c`, `build.sh`. |
| `driver/` | sunxi-mali r6p2 kernel driver source, ported to Linux 7.2: `vm_flags_set`, the new shrinker API, `timer_delete`, void platform `remove`, `vma_flags_t` for shmem, `zap_special_vma_range`, `MODULE_IMPORT_NS("DMA_BUF")`, and a driver override so lima doesn't probe the device. `build-module.sh` builds it (`out/mali.ko`). |
| `stubs/` | `empty.c` (the stub libraries' source), `libMali.so` (flag cleared), `libMali.so.orig`, the stubs. |
| `kernel/` | goodluckOS's kernel build: `Dockerfile.modules` (the stock tree the module builds use, with `stock/`: the stock configuration and gamepad driver), and `Dockerfile` + `build.sh` for kernel experiments (boot images into `kernel/out/`; its configuration and gamepad driver are no longer stock), its display and gamepad drivers, patches, device trees. |
| `sdl/` | Dockerfile for an SDL2 with the "mali" video backend, for SDL apps that should draw through ARM's driver (the three ports don't: they use the stock SDL for input and audio only). |

Docker image: `glos-kernel:ready` is the stock goodluckOS kernel tree with
`make modules` done (`7.2.9-dirty`, the stock configuration), about 3 GB.
Everything here builds against it, so `vermagic` and the module layout match
the stock kernel. `./make-kernel-image.sh` creates it
(`kernel/Dockerfile.modules`, a full kernel build); delete it when not working
on the driver (`docker image rm glos-kernel:ready`). `mali.ko` built against
the custom kernel's configuration had the same code (`.text`) as against the
stock one; only build IDs differed.

## Using ARM's driver for another app

Copy `glos/` next to the app and start it as `./glos/glos-gpu run mali <app>`.
The app draws to `/dev/fb0` through EGL (ARM's driver's only output) while
`/dev/mali` exists. Mali-400 limits that shaped the ports: GLES 2.0 only;
vertex shaders of at most 512 instruction words and no real loops; a small
uniform budget; unsized texture formats, no `GL_TEXTURE_MAX_LEVEL`, an
incomplete mip chain samples black; the driver copies every client-side index
list and uniform upload with an ioctl (~3 µs each) and turns every
`glBufferData(NULL)` orphan into a GPU allocation; it blocks inside draw calls
when the GPU is behind, so swap time doesn't show GPU load.

## Fixed: kernel BUG when `mali.ko` unloads right after the game

ARM's `mali_remove` freed the driver's locks (`mali_terminate_subsystems`)
while runtime PM was still enabled; the sunxi glue disables it only at device
unregister, after that. The driver suspends the GPU 1 s after its last job
(autosuspend), and `glos-gpu-switch` unloads the module as soon as the game is
gone, so a suspend still pending ran `mali_pm_runtime_suspend` on the freed lock:
`kernel BUG at mali_osk_locks.h:299`, `rmmod` stuck in D state, lima never back,
black screen until a forced power-off (seen 2026-10-31 after killing Jedi
Outcast with Fn+Start+Select). `mali_remove` now calls `pm_runtime_disable()`
first (cancels a pending suspend, waits for a running one). Checked: clean stop
and forced kill of Jedi Outcast, a kill of GTA III and a stop of VCS, no BUG.

## Fixed: a GPU job timeout hung the GPU for good

`mali_group_timeout` (the watchdog for a GP or PP job that runs too long) got
its group with `container_of` on a stack variable holding the timer pointer,
not on the group, so it worked on garbage: the GPU was never reset, the game
hung and the kernel warned in `__queue_work`. GTA SA's first load hits one
such timeout (2026-11-01). The timer now keeps the data given to
`_mali_osk_timer_setcallback` and the callback takes the group from there;
SA's first load now recovers and goes on.

## Known cost: the display memory pool shrinks a little per game launch

The display pool (CMA, 16 MB on the stock kernel) is shared by the kernel's
framebuffer, the launcher's and the emulators' scanout buffers. Each game
launch loses about 50–70 kB of it for the rest of the boot. The cause is in
Mesa's lima driver in the stock kernel, not in anything here: unbinding and
rebinding lima and then running any GL session leaks one GPU page-table set
(17 pages) — measured with a plain sysfs unbind/bind and the launcher alone,
no ARM driver and no framebuffer change involved, and unchanged with lima
forced on around the rebind or during the session. ARM's driver can only take
the GPU when lima is unbound, so every game launch pays it once. The
launcher's own sessions, the framebuffer fix cycle and the driver switch
without a session lose nothing.

Scanout buffers need 2 MB-aligned slots in that pool, so the leaked scraps
also fragment it. After roughly 60–80 launches in one boot the emulators and
Chocolate Doom can no longer allocate their buffers
(`DRM_IOCTL_MODE_CREATE_DUMB failed: Cannot allocate memory`, a black screen
or a crash) although the games still run (they draw into the kernel's
framebuffer). A reboot clears it. Measure with the launcher stopped:
`sync; echo 3 > /proc/sys/vm/drop_caches; grep CmaFree /proc/meminfo`
(8.7 MB free on a fresh boot with the launcher running, 9.2–9.4 MB without
it); `/proc/pagetypeinfo`'s CMA line shows the free blocks by size.

## Tried and dropped

- A rebuilt boot image with the settings baked in: works, but it is a system
  change and a flash; replaced by the module. `kernel/` can rebuild it.
- Declaring the sticks' real range in the input device at launch
  (`EVIOCSABS`, saved and restored around the game): works, but it is a
  system-level change; the games scale their sticks themselves instead.
- lima for the ports: 7–9 fps in GTA III.
- Around the lima leak above: resuming lima before the unbind, keeping it on
  during the session, a pause before the session, a trivial open/close of
  the GPU node after the rebind: the loss stays. Not rebinding lima is not
  possible on the stock kernel: lima keeps handlers on the GPU's seven
  interrupt lines while bound (requested `IRQF_SHARED`, also while
  suspended), and they read and clear the GPU's status on every interrupt,
  so ARM's driver sharing the lines would lose its job completions. The
  ways out are a lima with the bug fixed (a rebuilt kernel, or lima built
  as a module and loaded from the game folder in place of the built-in one
  for the rest of the boot) after the bug is found, or a reboot when the
  emulators start failing. Left as is (2026-10-31).

## Third-party components

ARM's kernel driver in `driver/` is GPL-2.0 (`COPYING`). ARM's user-space
driver (`libMali.so`) is ARM's proprietary binary and is not in the repo
(`get-libmali.sh`).
