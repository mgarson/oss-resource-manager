Operating Systems Project 5
Author: Maija Garson
Date: 04/29/2025

Description
This project will compile two programs into two executables using the makefile provided. One of the executables, oss, is generated from oss.cpp. The other executable,
worker, is generated from worker.cpp. The oss program will allocate a shared memory clock, keep track of a process control block table and resource table, 
and will launch the worker program as its child up to a specified amount of times. The worker child will randomly generate a probability to request or release resources.
Th worker will send messages to oss representing a request or release of resources. Oss will attempt to grant requests if possible, or will add the child to a wait queue.
 Oss will also update all values in the tables to reflect requests/releases from child processes. Every 1 sec of system time, oss will run a deadlock detection algorithm.
 If a deadlock is detected, it will run a recovery algorithm to incrementally kill child
processes until there is no longer a deadlock. Every .5 sec, oss will print both the resource table and process table. Oss will print final statistics at the end of each run.

Compilation
These programs will compile using the included makefile. In the command line it will compile if given:
make

Running the Program:
Once compiled, the oss program can be run with 5 options that are optional:
oss [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f]
Where
        -h: Display help message
        -n proc: Proc represents the amount of total child processes to launch
        -s simul: Simul represents the amount of child processes that can run simultaneously
        -i intervalInMsToLaunchChildren: Represents the interval in ms to launch the next child process
	-f logfile: Will print output from oss to logfile, while still printing to console
Default values for options n, s, and t will be 1 and for i will be 0 if not specified in the command line

Problems Encountered:
I struggled with properly updating everything in the tables to reflect requests/releases initially, which caused some bugs in the program. Once I had everything properly updated, the
bugs went away.
I also had some trouble properly implementing the deadlock recovery algorithm in the program. It would still end up becoming stuck in a deadlock until I properly killed the victim program
and updated the values in the table to reflect this.

Known Bugs:
I currently do not know of any bugs in this project.



