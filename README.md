# Multi-Container Runtime with Kernel Memory Monitor

This repository implements a lightweight Linux container runtime in C with:

- a long-running parent supervisor
- concurrent multi-container management
- a UNIX-domain-socket CLI control plane
- a bounded-buffer logging pipeline
- a kernel-space memory monitor with soft and hard limits
- workload programs for memory and scheduler experiments

Source files live under `boilerplate/`. The repository root includes a wrapper `Makefile`, so `make` delegates to `boilerplate/`.

## 1. Team Information

| Member | Name | SRN |
| --- | --- | --- |
| 1 | Ibrahim M. | `PLACEHOLDER_SRN_1` |
| 2 | `PLACEHOLDER_TEAMMATE_NAME` | `PLACEHOLDER_SRN_2` |

Update the placeholders above before final academic submission.

## 2. Repository Layout

| Path | Purpose |
| --- | --- |
| `boilerplate/engine.c` | user-space supervisor runtime and CLI client |
| `boilerplate/monitor.c` | kernel module for soft/hard memory enforcement |
| `boilerplate/monitor_ioctl.h` | shared ioctl contract |
| `boilerplate/cpu_hog.c` | CPU-bound scheduler workload |
| `boilerplate/io_pulse.c` | I/O-oriented scheduler workload |
| `boilerplate/memory_hog.c` | memory pressure workload |
| `boilerplate/Makefile` | primary Linux build logic |
| `Makefile` | root wrapper for `make`, `make ci`, and `make clean` |

## 3. Build, Load, and Run Instructions

### 3.1 Recommended Grading Environment: Ubuntu 22.04 / 24.04 VM

The project guide still expects an Ubuntu VM with Secure Boot disabled. Use this path for grading.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget tar
```

### 3.2 Arch Linux Setup

This project is still kernel-module-based Linux code, so Arch works as a local development/demo environment if your running kernel headers match your kernel.

For the stock Arch kernel:

```bash
sudo pacman -Syu
sudo pacman -S --needed base-devel linux-headers wget tar alpine
```

If you use a different kernel package, install the matching headers instead:

- `linux-lts` -> `linux-lts-headers`
- `linux-zen` -> `linux-zen-headers`

### 3.3 Build the Project

From the repository root:

```bash
make
```

Or directly inside the implementation directory:

```bash
make -C boilerplate
```

CI-safe compile-only path:

```bash
make -C boilerplate ci
```

### 3.4 Environment Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
cd ..
```

### 3.5 Prepare the Base Root Filesystem

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create one writable copy per live container:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp -a ./rootfs-base ./rootfs-gamma
```

### 3.6 Copy Helper Workloads into Container Root Filesystems

Build first, then copy the host-built workload binaries into each writable rootfs you plan to use:

```bash
cp boilerplate/cpu_hog ./rootfs-alpha/
cp boilerplate/io_pulse ./rootfs-beta/
cp boilerplate/memory_hog ./rootfs-gamma/
```

If your Alpine rootfs needs statically linked binaries, set:

```bash
make -C boilerplate clean
make -C boilerplate WORKLOAD_LDFLAGS=-static
```

### 3.7 Load the Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
cd ..
```

### 3.8 Start the Supervisor

Run the supervisor once and keep it alive in its own terminal:

```bash
cd boilerplate
sudo ./engine supervisor ../rootfs-base
```

### 3.9 CLI Commands

In another terminal:

```bash
cd boilerplate
sudo ./engine start alpha ../rootfs-alpha "/bin/sh" --soft-mib 48 --hard-mib 80 --nice 5
sudo ./engine start beta ../rootfs-beta "/bin/sh" --soft-mib 64 --hard-mib 96 --nice 0
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

Foreground run mode:

```bash
cd boilerplate
sudo ./engine run gamma ../rootfs-gamma "/memory_hog 8 500" --soft-mib 32 --hard-mib 48 --nice 0
echo $?
```

If the `run` client receives `Ctrl+C`, it forwards a stop request to the supervisor and keeps waiting for final status.

### 3.10 Suggested Demo Commands

Two background containers under one supervisor:

```bash
cd boilerplate
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 20" --soft-mib 48 --hard-mib 80 --nice 10
sudo ./engine start beta ../rootfs-beta "/io_pulse 20 150" --soft-mib 64 --hard-mib 96 --nice 0
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine logs beta
```

Memory soft/hard limit demo:

```bash
cd boilerplate
sudo ./engine run gamma ../rootfs-gamma "/memory_hog 8 500" --soft-mib 24 --hard-mib 40 --nice 0
dmesg | tail -n 30
```

CPU scheduling comparison:

```bash
cd boilerplate
cp cpu_hog ../rootfs-alpha/
cp cpu_hog ../rootfs-beta/
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 25" --nice 15
sudo ./engine start beta ../rootfs-beta "/cpu_hog 25" --nice -5
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine logs beta
```

CPU vs I/O responsiveness comparison:

```bash
cd boilerplate
cp cpu_hog ../rootfs-alpha/
cp io_pulse ../rootfs-beta/
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start beta ../rootfs-beta "/io_pulse 25 120" --nice 0
sudo ./engine logs alpha
sudo ./engine logs beta
```

### 3.11 Clean Shutdown and Unload

```bash
cd boilerplate
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop gamma
```

Then terminate the supervisor terminal with `Ctrl+C` and inspect cleanup:

```bash
ps -ef | grep engine
ps -ef | grep defunct
dmesg | tail -n 50
```

Unload the module:

```bash
cd boilerplate
sudo rmmod monitor
```

Optional cleanup:

```bash
make clean
rm -rf rootfs-base rootfs-alpha rootfs-beta rootfs-gamma
rm -f alpine-minirootfs-3.20.3-x86_64.tar.gz
```

## 4. Screenshot Capture Commands

Use whichever desktop stack matches your Arch Linux environment.

### Wayland

Install once:

```bash
sudo pacman -S --needed grim slurp wl-clipboard
mkdir -p screenshots
```

Full screen:

```bash
grim screenshots/01-full.png
```

Interactive region:

```bash
grim -g "$(slurp)" screenshots/02-region.png
```

### X11

Install once:

```bash
sudo pacman -S --needed scrot
mkdir -p screenshots
```

Full screen:

```bash
scrot screenshots/01-full.png
```

Select a region after a short delay:

```bash
scrot -s -d 2 screenshots/02-region.png
```

### Recommended Screenshot Workflow

1. Open the exact terminal state you want to document.
2. Run one of the commands above.
3. Rename each image to the matching placeholder file in the next section.
4. Replace the placeholder captions with your final observation.

## 5. Demo with Screenshot Placeholders

Create a `screenshots/` directory and save final images with the filenames below.

### 5.1 Multi-container supervision

Caption placeholder: two or more containers running under one supervisor.

![Multi-container supervision placeholder](screenshots/01-multi-container-supervision.png)

Suggested terminal state:

```bash
cd boilerplate
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 20"
sudo ./engine start beta ../rootfs-beta "/io_pulse 20 150"
sudo ./engine ps
```

### 5.2 Metadata tracking

Caption placeholder: `ps` output showing tracked metadata such as PID, state, limits, reason, and log path.

![Metadata tracking placeholder](screenshots/02-metadata-tracking.png)

### 5.3 Bounded-buffer logging

Caption placeholder: per-container log contents written through the producer/consumer pipeline.

![Bounded-buffer logging placeholder](screenshots/03-bounded-buffer-logging.png)

Suggested commands:

```bash
cd boilerplate
sudo ./engine logs alpha
sudo ./engine logs beta
```

### 5.4 CLI and IPC

Caption placeholder: a CLI request reaching the supervisor over the UNIX socket control channel.

![CLI and IPC placeholder](screenshots/04-cli-ipc.png)

### 5.5 Soft-limit warning

Caption placeholder: kernel warning after the process first crosses the soft limit.

![Soft-limit warning placeholder](screenshots/05-soft-limit-warning.png)

Suggested commands:

```bash
cd boilerplate
sudo ./engine run gamma ../rootfs-gamma "/memory_hog 8 500" --soft-mib 24 --hard-mib 64
dmesg | tail -n 30
```

### 5.6 Hard-limit enforcement

Caption placeholder: kernel kill event plus supervisor metadata showing `hard_limit_killed`.

![Hard-limit enforcement placeholder](screenshots/06-hard-limit-enforcement.png)

Suggested commands:

```bash
cd boilerplate
sudo ./engine run gamma ../rootfs-gamma "/memory_hog 8 500" --soft-mib 24 --hard-mib 40
dmesg | tail -n 30
sudo ./engine ps
```

### 5.7 Scheduling experiment

Caption placeholder: terminal output comparing two different scheduling configurations.

![Scheduling experiment placeholder](screenshots/07-scheduling-experiment.png)

### 5.8 Clean teardown

Caption placeholder: evidence that children were reaped, no zombies remain, and the supervisor shut down cleanly.

![Clean teardown placeholder](screenshots/08-clean-teardown.png)

Suggested commands:

```bash
ps -ef | grep engine
ps -ef | grep defunct
dmesg | tail -n 20
```

## 6. Engineering Analysis

### 6.1 Isolation Mechanisms

The runtime isolates containers by creating each child with separate PID, UTS, and mount namespaces through `clone()`. PID namespaces give each container its own process-numbering view, so processes inside the container do not see host PIDs directly. UTS namespaces isolate the hostname, which is useful for demonstrating that each container has its own machine identity from user space. Mount namespaces isolate mount-table changes, allowing `/proc` to be mounted inside the container without polluting the host mount tree.

Filesystem isolation is completed with `chroot()` into a per-container writable copy of the Alpine minirootfs. That gives each container its own `/`, while still sharing the same underlying host kernel. The kernel is therefore not virtualized here: all containers still share the host scheduler, host memory manager, host cgroup environment if any, the same system-call implementation, and the same loaded kernel modules. Containers are isolated process environments, not separate kernels.

### 6.2 Supervisor and Process Lifecycle

A long-running parent supervisor is valuable because container execution is not just a single `clone()` call. The supervisor owns container metadata, the control socket, the logging pipeline, and the cleanup logic. It becomes the stable parent that can accept multiple CLI requests over time, track each child by ID, and classify how it ended.

This design also matters for reaping. When a child exits, the kernel sends `SIGCHLD` to the supervisor. The supervisor then calls `waitpid(..., WNOHANG)` and records the final exit information, preventing zombies. Because metadata is centralized in the parent, the CLI can ask for `ps`, `logs`, and `stop` at any time without needing direct handles to child processes.

### 6.3 IPC, Threads, and Synchronization

The project uses two separate IPC paths. The control plane uses a UNIX domain socket between the short-lived CLI client and the long-running supervisor. The logging plane uses per-container pipes from child stdout/stderr into the supervisor. These should stay separate because control messages are structured request/response traffic, while logs are streaming output with backpressure concerns.

The bounded log buffer sits between producer threads and the logger consumer thread. Producers block on `not_full` when the buffer is full, and the consumer blocks on `not_empty` when the buffer is empty. A mutex protects the ring-buffer indices and count so two producers cannot overwrite the same slot or race with the consumer. Without that synchronization, log chunks could be lost, duplicated, or written to the wrong offset. Shutdown also needs coordination: the shutdown flag and condition broadcasts let the consumer drain remaining entries and exit without deadlocking on an empty queue.

Container metadata is synchronized separately with a dedicated mutex. That prevents races between lifecycle updates, `ps`, `stop`, log inspection, and run waiters. Without that lock, one thread could read a half-updated record while another thread reclassifies the exit reason or updates the PID.

### 6.4 Memory Management and Enforcement

RSS, or resident set size, measures how many pages of a process are currently resident in physical memory. It does not measure all virtual memory mappings equally, and it does not mean those bytes are uniquely owned by the process. Shared mappings and file-backed pages complicate interpretation, which is why RSS is useful for enforcement heuristics but not a perfect measure of “total memory asked for.”

Soft and hard limits represent different policies. A soft limit is informational: it gives visibility into pressure without immediately destroying the workload. That is useful for warning, tuning, or classroom observation. A hard limit is enforcement: once the process crosses it, the kernel monitor kills the task to protect the host and preserve the policy boundary.

Kernel-space enforcement matters because user-space polling alone is weaker. A user-space runtime can observe memory use, but it cannot reliably out-race every offending allocation or guarantee authoritative process termination semantics across all timing windows. The kernel module can inspect task memory state directly and send the terminating signal from privileged code close to the scheduler and memory-management machinery.

### 6.5 Scheduling Behavior

The scheduler experiments in this project demonstrate that Linux is balancing fairness, responsiveness, and throughput across runnable tasks on the same CPU resources. For two CPU-bound containers with different `nice` values, the higher-priority task should receive more CPU share and typically complete sooner. For a CPU-bound workload competing with an I/O-heavy workload, the I/O-oriented task often appears more responsive because it sleeps frequently, wakes up, performs short bursts of work, and benefits from interactive scheduling heuristics.

The runtime makes this observable because both workloads run concurrently under one supervisor while their output is timestamped and retained in separate logs. That means differences in progress cadence and completion time can be compared directly, rather than guessed from a single foreground terminal.

## 7. Design Decisions and Tradeoffs

### 7.1 Namespace Isolation

Choice: `clone()` with PID, UTS, and mount namespaces plus `chroot()`.

Tradeoff: `chroot()` is simpler than `pivot_root()`, but it is less strict as a filesystem-isolation primitive in advanced threat models.

Why this was the right call: it keeps the implementation focused on the course goals while still demonstrating per-container rootfs isolation and an independent `/proc` mount.

### 7.2 Supervisor Architecture

Choice: one persistent supervisor daemon plus short-lived CLI clients over a UNIX socket.

Tradeoff: the daemon is more complex than a one-shot launcher because it must manage signals, metadata, and cleanup across many requests.

Why this was the right call: it directly supports multi-container management, `ps`, `stop`, `logs`, and blocking `run` semantics from the same runtime binary.

### 7.3 Logging Pipeline

Choice: per-container pipe readers feed a shared bounded ring buffer consumed by one logger thread.

Tradeoff: a bounded queue introduces blocking and synchronization complexity that direct file writes would avoid.

Why this was the right call: the queue makes the producer/consumer problem explicit, shows how backpressure is handled, and cleanly separates container execution from log persistence.

### 7.4 Kernel Monitor

Choice: an LKM with a linked list of monitored PIDs, `ioctl` registration, periodic RSS polling, and separate soft/hard policies.

Tradeoff: kernel code is harder to debug and requires exact matching headers and elevated privileges.

Why this was the right call: hard-limit enforcement belongs at the kernel boundary where memory state and signal authority are both available.

### 7.5 Scheduling Experiments

Choice: use simple focused workloads (`cpu_hog`, `io_pulse`, `memory_hog`) instead of more complex benchmark suites.

Tradeoff: these programs are synthetic and do not model every real application behavior.

Why this was the right call: their behavior is easy to reason about, easy to reproduce inside minimal root filesystems, and well suited to explaining scheduler policies in a report.

## 8. Scheduler Experiment Results

This section is intentionally left as a fill-in template because the Linux-only experiments were not executed in this repository preparation step.

### 8.1 Experiment A: CPU vs CPU with Different Nice Values

Commands:

```bash
cd boilerplate
cp cpu_hog ../rootfs-alpha/
cp cpu_hog ../rootfs-beta/
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 25" --nice 15
sudo ./engine start beta ../rootfs-beta "/cpu_hog 25" --nice -5
sudo ./engine logs alpha
sudo ./engine logs beta
```

Fill in after running:

| Container | Nice | Start Time | Finish Time | Observed Progress Rate | Notes |
| --- | --- | --- | --- | --- | --- |
| alpha | 15 | `FILL_ME` | `FILL_ME` | `FILL_ME` | `FILL_ME` |
| beta | -5 | `FILL_ME` | `FILL_ME` | `FILL_ME` | `FILL_ME` |

Expected interpretation placeholder: the lower nice value should generally receive more CPU time and finish earlier.

### 8.2 Experiment B: CPU-bound vs I/O-bound

Commands:

```bash
cd boilerplate
cp cpu_hog ../rootfs-alpha/
cp io_pulse ../rootfs-beta/
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start beta ../rootfs-beta "/io_pulse 25 120" --nice 0
sudo ./engine logs alpha
sudo ./engine logs beta
```

Fill in after running:

| Container | Workload | Completion Time | Responsiveness Observation | Notes |
| --- | --- | --- | --- | --- |
| alpha | CPU-bound | `FILL_ME` | `FILL_ME` | `FILL_ME` |
| beta | I/O-bound | `FILL_ME` | `FILL_ME` | `FILL_ME` |

Expected interpretation placeholder: the I/O-oriented task should keep producing short bursts of visible progress while the CPU-bound task consumes longer uninterrupted CPU slices.

## 9. Notes

- Do not commit `rootfs-base/` or any `rootfs-*` writable copies.
- Run the Ubuntu VM path for grading consistency even though Arch Linux steps are included here.
- Replace screenshot placeholders and fill in the experiment tables after completing the live Linux demo.
