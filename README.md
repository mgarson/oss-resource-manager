# OSS Resource Manager
> C++ resource-manager simulator with deadlock detection

[![Language: C++](https://img.shields.io/badge/language-C%2B%2B-blue.svg)](https://isocpp.org/)  

---

## Overview
This project consists of two programs--'oss' (the master scheduler) and 'worker' (the child process)--that work together to simulate resource allocation, process blocking, and deadlock recovery.

---

## Features

- **Process forking**
  'oss' forks each 'worker' as a child process.
- **Shared-clock**
  Uses shared memory for a simulated system clock
- **Resource & PCB tables**
  Maintains up-to-date process table and resource table reflecting each worker's state and resource allocation
- **Interprocess Communication**
  Uses message queues and shared memory to facilitate communication
- **Request handling**
  -Grants resource requests when available
  -Otherwise enqueues the worker in a wait queue
- **Deadlock detection**
  Runs detection algorithm every **1 second** of system time
- **Deadlock recovery**
  Incrementally terminates victim workers until the deadlock is resolved
- **Runtime reporting**
  -Prints PCB and Resource tables every **0.5 seconds** of simulated time
  -Outputs final statistics at program termination

---

## Build & Run

```bash
# 1. Clone the repository
 git clone https://github.com/mgarson/oss-resource-manager.git
 cd oss-resource-manager

# 2. Build both programs
 make

# 3. Run the scheduler
 ./oss [-h] [-n proc] [-s simul] [-i interval_ms] [-f logfile]

# Options:
  -h                     Show help message  
  -n proc                Total worker processes to launch (default: 1)  
  -s simul               Max simultaneous workers (default: 1)  
  -i interval_ms         Delay between spawns in milliseconds (default: 0)  
  -f logfile             Write console output to <logfile> as well 
 ``` 
  ---

## Technical Highlights

- Centralized all PCB/resource table updates to eliminate synchronization errors
- Designed a victim-selection loop that re-evaluates resource ownership after each termination

  

