# Multi-Container Runtime


**Team Members:**

* Srivani Karanth - SRN: PES1UG24CS471

* Shriya K - SRN: PES1UG24CS448


---


## Build, Load, and Run Instructions


Follow these steps to build the runtime, load the kernel monitor, and start managing containers from a fresh Ubuntu VM environment.


### 1. Clean and Build the Project

```bash

export PS1="CS471" 

cd ~/OS-Jackfruit/boilerplate

sudo make clean

make

```


### 2. Load the Kernel Module

```bash

sudo insmod monitor.ko

```


### 3. Prepare the Root Filesystems

```bash

# Verify base-rootfs is extracted

mkdir -p rootfs-base

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base


# Create writable copies

rm -rf rootfs-alpha rootfs-beta

cp -a ./rootfs-base ./rootfs-alpha

cp -a ./rootfs-base ./rootfs-beta

```


### 4. Start the Supervisor

Start the long-running supervisor daemon in Terminal 1:

```bash

sudo ./engine supervisor ./rootfs-base

```


---


## Demo Screenshots


### Milestone 1: Metadata Tracking & Supervisor Initialization

![Metadata Tracking](boilerplate/screenshots/cp1_ipc_supervisor_ps.png)

*Initial supervisor startup and the empty metadata table, verifying the control-plane socket and prompt branding.*


### Milestone 2: CLI and IPC Communication

![CLI IPC](boilerplate/screenshots/cp2.png)

*CLI sending a 'start' request and the supervisor responding via UNIX Domain Socket (IPC Path B).*


### Milestone 3: Multi-Container Supervision

![Multi-Container PS](boilerplate/screenshots/ss1_multicontainer-ps.png)

*Two containers (alpha and beta) running concurrently, each tracked with its own host PID and isolated rootfs.*


### Milestone 4: Bounded-Buffer Logging

![Bounded Buffer Logs](boilerplate/screenshots/ss2_bounded-buffer-logs.png)

*Log contents captured through the synchronized producer-consumer pipeline (IPC Path A).*


### Milestone 5: Soft-Limit Warning

![Soft Limit](boilerplate/screenshots/softhardlim_warning.png)

*Kernel dmesg output showing the monitor LKM detecting a soft-limit breach for container 'charlie'.*


### Milestone 6: Hard-Limit Enforcement

![Hard Limit](boilerplate/screenshots/softhardlim_kill.png)

*Kernel logs showing SIGKILL enforcement and Supervisor metadata reflecting the 'killed' state (sig=9).*


### Milestone 7: Scheduling Experiment

![Scheduler Race](boilerplate/screenshots/scheduler_exp.png)

*Scheduling experiment: High-priority container (nice 0) showing significantly higher CPU accumulator values than low-priority (nice 19).*


### Milestone 8: Clean Teardown

![Teardown](boilerplate/screenshots/ss3_teardown.png)

*Verification that all containers are reaped, metadata is updated to 'exited', and the supervisor daemon has exited cleanly.*


---


## Engineering Analysis


**1. Isolation Mechanisms**

The runtime achieves isolation using Linux namespaces and `chroot`. `CLONE_NEWPID` provides a private process tree, while `CLONE_NEWUTS` isolates the hostname. `chroot` locks the process into the Alpine root filesystem, ensuring it cannot access host files.


**2. Supervisor Lifecycle**

The supervisor daemon manages the entire lifecycle of the containers. It uses `waitpid` with the `WNOHANG` flag to asynchronously reap exited child processes, maintaining system stability and preventing resource leaks.


**3. IPC and Synchronization**

- **Control Plane:** Uses UNIX Domain Sockets for low-latency communication between the CLI and the daemon.

- **Data Plane:** Container logs are piped into a bounded buffer. Thread safety is ensured using `pthread_mutex_t` and condition variables (`not_full`/`not_empty`) to manage the producer-consumer relationship.


**4. Memory Monitoring**

Implemented as a Linux Kernel Module (LKM) for accuracy. It uses `get_mm_rss` to track the Resident Set Size (RSS). Hard limits are strictly enforced via `SIGKILL` sent directly from the kernel to the offending process.


**5. Scheduling Behavior**

The experiment confirms that the Linux Completely Fair Scheduler (CFS) respects `nice` values. Processes with higher priority (lower nice) receive a larger proportion of CPU time-slices, as seen in the higher accumulator values for the `alpha2` container.


---


## Design Decisions


* **Spinlock over Mutex:** Used in the kernel monitor because the list is traversed in a timer interrupt context where the kernel cannot sleep.

* **Bounded Buffer:** Used to decouple container execution from disk I/O latency, preventing a slow disk write from blocking the containerized application.

* **Static Linking:** Workload binaries (`cpu_hog`, `memory_hog`) are statically linked to ensure they run correctly within the isolated root filesystem without external dependencies.


---


## Key Learnings & Challenges


* **Concurrency is Hard:** One of the biggest challenges was handling the producer-consumer relationship for logging. We initially faced deadlocks when the buffer was full, which was resolved by correctly implementing `pthread_cond_broadcast`.

* **The Kernel doesn't Sleep:** While implementing the memory monitor, we learned that inside a timer interrupt, one must use a `spinlock` rather than a mutex to avoid sleeping.

* **Namespace Nuances:** We discovered that while namespaces provide process and hostname isolation, they don't automatically isolate the filesystem;`chroot` is required to finalize the "container" feel.
