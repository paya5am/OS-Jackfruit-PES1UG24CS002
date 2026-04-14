# **Lightweight Container Runtime with Kernel Memory Monitor**

---

## **1. Team Information**

* Aadhavan Muthusamy - SRN: PES1UG24CS002<br>
( **dev** in past commits -> **updated** to -> **paya5am** ( latest four commits ) )
 
* Aakash Desai - SRN: PES1UG24CS006<br>
( **Aakash-Desai-0103** )
---

## **2. Build, Load, and Run Instructions**

### 🔧 Build

```bash
make
```

---

### 🔌 Load Kernel Module

```bash
sudo insmod monitor.ko
```

---

### ✅ Verify Device

```bash
ls -l /dev/container_monitor
```

---

### 🚀 Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### 📁 Create Writable Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### ▶️ Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

---

### 📊 List Containers

```bash
sudo ./engine ps
```

---

### 📜 View Logs

```bash
sudo ./engine logs alpha
```

---

### 🧪 Run Workloads

```bash
cp cpu_hog ./rootfs-alpha/
cp io_pulse ./rootfs-beta/
cp memory_hog ./rootfs-alpha/

sudo ./engine start cpu ./rootfs-alpha ./cpu_hog
sudo ./engine start io ./rootfs-beta ./io_pulse
```

---

### 🛑 Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

### 📟 Inspect Kernel Logs

```bash
dmesg | tail
```

---

### ❌ Unload Module

```bash
sudo rmmod monitor
```

---

## **3. Demo with Screenshots**

### 1. Multi-container supervision

![SS1](Screen_Shots/ss1_supervisor.png)
*Multiple containers running under a single supervisor process.*

---

### 2. Metadata tracking

![SS2](Screen_Shots/ss2_ps.png)
*Output of `engine ps` showing container metadata.*

---

### 3. Bounded-buffer logging

![SS3](Screen_Shots/ss3_logging.png)
*Logs captured from multiple containers via pipes and written to log files.*

---

### 4. CLI and IPC

![SS4](Screen_Shots/ss4_cli.png)
*CLI command issued and response received from supervisor demonstrating IPC.*

---

### 5. Soft-limit warning

![SS5](Screen_Shots/ss5_soft_limit.png)
*Kernel log showing soft memory limit warning.(Followed by Hard-Limit Escalation)*

---

### 6. Hard-limit enforcement

![SS6](Screen_Shots/ss6_hard_limit.png)
*Kernel log and metadata showing container killed after exceeding hard limit.*

---

### 7. Scheduling experiment

![SS7](Screen_Shots/ss7_scheduling.png)

*CPU-bound vs I/O-bound workloads showing different CPU usage behavior.*

---

### 8. Clean teardown

![SS8](Screen_Shots/ss8_cleanup.png)

*No zombie processes after shutdown.*

---

## **4. Engineering Analysis**

### **1. Isolation Mechanisms**

Our runtime achieves isolation using a combination of Linux namespaces and filesystem isolation techniques.

Each container is created using the clone() system call with the CLONE_NEWPID, CLONE_NEWUTS, and CLONE_NEWNS flags. The PID namespace ensures that processes inside the container have their own independent process tree, starting from PID 1, which isolates them from host processes. The UTS namespace allows each container to have its own hostname, providing logical separation. The mount namespace ensures that filesystem mount operations inside a container do not affect the host or other containers.

For filesystem isolation, we use chroot() to restrict the container’s root directory to its assigned rootfs. This ensures that the container cannot access files outside its designated filesystem tree. Additionally, /proc is mounted inside the container to provide process visibility within the namespace.

However, all containers still share the same underlying Linux kernel. This means that kernel resources such as CPU scheduling, memory management, and device drivers are shared across containers, which is a fundamental property of containerization compared to full virtualization.

---

### **2. Supervisor and Process Lifecycle**

A long-running supervisor process is central to our design. Instead of launching containers as independent processes, the supervisor maintains control over all container lifecycles.

When a container is started, the supervisor uses clone() to create a child process with isolated namespaces. The supervisor stores metadata such as container ID, PID, state, start time, and resource limits. This allows it to track and manage multiple containers concurrently.

The supervisor handles process termination using waitpid() in a non-blocking loop to reap exited child processes and prevent zombie processes. When a container exits, its state is updated based on the reason for termination (normal exit, manual stop, or hard-limit kill).

Signal handling plays a critical role. When a user issues a stop command, the supervisor sets a stop_requested flag and sends a termination signal to the container. This allows the system to distinguish between intentional termination and forced termination due to resource limits.

This architecture ensures centralized lifecycle management, avoids orphaned processes, and provides a consistent interface for container control.

---

### **3. IPC, Threads, and Synchronization**

The system uses two distinct IPC mechanisms:

* Path A (Logging): Pipes are used to capture stdout and stderr from container processes and send them to the supervisor.
* Path B (Control): A UNIX domain socket is used for communication between CLI clients and the supervisor.

For logging, we implemented a bounded-buffer producer-consumer model. Producer threads read container output from pipes and insert log entries into a shared buffer. A consumer thread removes entries from the buffer and writes them to per-container log files.

We use a mutex and condition variables (pthread_mutex_t, pthread_cond_t) to synchronize access to the buffer. Without synchronization, race conditions could occur where multiple producers overwrite buffer entries or the consumer reads inconsistent data.

The bounded buffer prevents uncontrolled memory growth and ensures backpressure when producers generate data faster than it can be consumed. The use of condition variables ensures that producers wait when the buffer is full and consumers wait when it is empty, avoiding busy waiting and deadlocks.

---

### **4. Memory Management and Enforcement**

The kernel module monitors memory usage using RSS (Resident Set Size), which represents the portion of a process’s memory that is currently resident in physical RAM

RSS does not include swapped-out memory or untouched virtual address space. It only reflects physically resident pages.

User-space cannot reliably enforce memory limits because processes can allocate memory faster than monitoring intervals. Kernel-space enforcement ensures immediate and authoritative control over process memory usage.

We implement two types of limits:

* Soft limit: Logs a warning
* Hard limit: Terminates process using SIGKILL

Memory enforcement is implemented in kernel space because only the kernel provides accurate and immediate access to memory usage, ensuring reliable enforcement.

---

### **5. Scheduling Behavior**

We conducted experiments using CPU-bound and I/O-bound workloads.

CPU-bound processes consume CPU continuously, while I/O-bound processes frequently yield CPU. Observations show that I/O-bound processes remain responsive, and CPU-bound processes dominate CPU usage.

This behavior aligns with the Completely Fair Scheduler (CFS), which prioritizes interactive (I/O-bound) processes by scheduling them quickly after wake-up while distributing CPU time proportionally among CPU-bound tasks.

This demonstrates:

* Fairness
* Responsiveness
* Efficient CPU utilization


---

## **5. Design Decisions and Tradeoffs**

### **1. Namespace Isolation**

* Choice: `clone()` + `chroot()`
* Tradeoff: `chroot` does not fully isolate filesystem like `pivot_root` (possible escape via descriptors or mount propagation)
* Justification: significantly simpler to implement while still providing sufficient isolation for project scope

---

### **2. Supervisor Architecture**

* Choice: centralized supervisor process
* Tradeoff: introduces a single point of failure
* Justification: simplifies lifecycle management, metadata tracking, and IPC coordination

---

### **3. IPC and Logging**

* Choice: pipes (logging) + UNIX sockets (control)
* Tradeoff: increased system complexity due to managing two IPC mechanisms
* Justification: clean separation of data plane and control plane improves modularity and avoids interference

---

### **4. Kernel Monitor**

* Choice: kernel-space monitoring
* Tradeoff: higher implementation complexity and risk compared to user-space
* Justification: only kernel has accurate, real-time access to process memory (RSS), ensuring reliable enforcement

---

### **5. Scheduling Experiments**

* Choice: synthetic workloads (`cpu_hog`, `io_pulse`)
* Tradeoff: may not reflect real-world workloads
* Justification: provides controlled, predictable behavior for clear demonstration of scheduling principles

---

## 6. Scheduler Experiment Results

### Experiment Setup

Two containers were executed concurrently:
- `cpu_hog` (CPU-bound workload)
- `io_pulse` (I/O-bound workload)

System behavior was observed using the `top` command.

---

### Observed Output

![Scheduling Experiment](Screen_Shots/ss7_scheduling.png)

The following behavior was observed during execution:

- `cpu_hog` consistently utilized nearly **100% CPU**
- `io_pulse` showed **very low CPU usage (~0–1%)**
- Both processes ran concurrently without starvation

---

### Comparison

| Process  | Type       | CPU Usage |
|----------|-----------|----------|
| cpu_hog  | CPU-bound | ~100%     |
| io_pulse | I/O-bound | ~0–1%     |

---

### Observation

The CPU-bound process consumed nearly all available CPU resources, while the I/O-bound process remained responsive and continued execution without delay.

---

### Conclusion

This demonstrates key properties of the Linux scheduler:

- **Fairness:** CPU time is distributed among processes based on behavior  
- **Responsiveness:** I/O-bound processes are favored due to frequent blocking  
- **Efficiency:** CPU-bound processes utilize available CPU cycles effectively  

The scheduler prioritizes tasks that yield the CPU frequently, ensuring interactive responsiveness while maintaining overall system throughput.


