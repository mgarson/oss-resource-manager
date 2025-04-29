// Operating Systems Project 4
// Author: Maija Garson
// Date: 04/18/2025
// Description: A program that simulates an operating system scheduler using a multi-level feedback queue.
// This program will run in a loop until it forks up to 100 child processes, with 18 allowed simultaneously.
// This program creates a system clock in shared memory and uses it to determine time within the program. 
// Information about the child processes are stored in a Process Control Block table. This program schedules 
// children by sending messages to the child processes using a message queue, these messages sent the child
// the total allowed time quantum it can run before stopping. The quantum is based on which queue the child
// is in at that time. It will always schedule the children in the highest priority queue. It will then wait
// for a response from the child, which informs oss how much of its time it used and also its status (terminated,
// blocked, or used full quantum). This program then puts the processes that did not terminate back into a
// queue based on its status and the queue it was previously in. This program will also print the PCB table
// and queue states every half second of system time. At the end, it will calculate and print statistics based on the run.
// The program will send a kill signal to all processes and terminate if 3 real-life seconds are reached.

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string>
#include <queue>

#define PERMS 0644
#define MAX_RES 5
#define INST_PER_RES 10

using namespace std;

typedef struct
{
	int proc;
	int simul;
	long long interval;
	string logfile;
} options_t;

// Structure for Process Control Block
typedef struct 
{
	int occupied; // Either true or false
	pid_t pid; // Process ID of this child
	int startSeconds; // Time when it was forked
	int startNano; // Time when it was forked
	int held[5] = {0};
	int waitingOn = -1;
} PCB;

typedef struct
{
	int total;
	int available;
	int allocation[18] = {0};
	int request[18] = {0};
	queue<int> waitQueue;
} Resource;

// Message buffer for communication between OSS and child processes
typedef struct msgbuffer 
{
	long mtype; // Message type used for message queue
	pid_t pid;
	int resId; // Which resource
	bool isRelease; // False means requested, true means release
	bool granted; // Grant resources to worker
} msgbuffer;

// Global variables
PCB* processTable; // Process control block table to track child processes
Resource* resTable;

int running; // Amount of running processes in system

int *shm_ptr; // Shared memory pointer to store system clock
int shm_id; // Shared memory ID

int msqid; // Queue ID for communication
msgbuffer buf;
msgbuffer rcvbuf;

bool logging = false; // Bool to determine if output should also print to logfile
FILE* logfile = NULL; // Pointer to logfile

int immGrant = 0;
int waitGrant = 0;
int regTerms = 0;
int dlRuns = 0;
int dlKills = 0;
int totDlProcs = 0;
int dlCnt = 0;

void print_usage(const char * app)
{
	fprintf(stdout, "usage: %s [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInMsToLaunchChildren\n", app);
	fprintf(stdout, "      proc is the number of total children to launch\n");
	fprintf(stdout, "      simul indicates how many children are to be allowed to run simultaneously\n");
	fprintf(stdout, "      iter is the number to pass to the user process\n");
}

// Function to increment system clock in seconds and nanoseconds
void incrementClock()
{
	// Update nanoseconds and check for overflow
	shm_ptr[1] += 10000000;
	if (shm_ptr[1] >= 1000000000) // Determines if nanosec is gt 1 billion, meaning it should convert to 1 second
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}

}

// Function to add a small overhead of 1000 ns to the clock (less amount than incrmenting clock)
void addOverhead()
{
	// Increment ns in shared memor
	shm_ptr[1] += 1000;
	// Check for overflow
	if (shm_ptr[1] >= 1000000000)
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}
}

// Function to access and add to shared memory
void shareMem()
{
	// Generate key
	const int sh_key = ftok("main.c", 0);
	// Create shared memory
	shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	if (shm_id <= 0) // Check if shared memory get failed
	{
		// If true, print error message and exit
		fprintf(stderr, "Shared memory get failed\n");
		exit(1);
	}
	
	// Attach shared memory
	shm_ptr = (int*)shmat(shm_id, 0, 0);
	if (shm_ptr <= 0)
	{
		fprintf(stderr, "Shared memory attach failed\n");
		exit(1);
	}
	// Initialize shared memory pointers to represent clock
	// Index 0 represents seconds, index 1 represents nanoseconds
	shm_ptr[0] = 0;
	shm_ptr[1] = 0;
}

// FUnction to print formatted process table and contents of the three different priority queues
void printInfo(int n)
{
	
	// Print to console
	printf("OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	printf("Entry\tOccupied\tPID\tStartS\tStartNs\n");
	
	// Print to log file as well
	fprintf(logfile, "OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	fprintf(logfile,"Entry\tOccupied\tPID\tStartS\tStartNs\n");

	for (int i = 0; i < n; i++)
	{
		// Print table only if occupied by process
		if (processTable[i].occupied == 1)
		{
			printf("%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
			fprintf(logfile, "%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
		}
	}
	printf("\n");
	fprintf(logfile, "\n");
	
}

// Signal handler to terminate all processes after 3 seconds in real time
void signal_handler(int sig)
{
	printf("3 seconds have passed, process(es) will now terminate.\n");
	pid_t pid;

	// Loop through process table to find all processes still running and terminate
	for (int i = 0; i < 18; i++)
	{
		if(processTable[i].occupied)
		{
			pid = processTable[i].pid;
			if (pid > 0)
				kill(pid, SIGKILL);
		}
	}
	 // Detach from shared memory and remove it
        if(shmdt(shm_ptr) == -1)
        {
                perror("shmdt failed");
                exit(1);
        }
        if (shmctl(shm_id, IPC_RMID, NULL) == -1)
        {
                perror("shmctl failed");
                exit(1);
        }

        // Remove the message queue
        if (msgctl(msqid, IPC_RMID, NULL) == -1)
        {
                perror("msgctl failed");
                exit(1);
        }


	exit(1);
}

bool req_lt_avail(int p, int m, int work[])
{
	for (int i = 0; i < m; i++)
	{
		if (resTable[i].request[p] > work[i])
			return false;
	}
	return true;
}

bool deadlock(int m, int n)
{
	int work[m];
	bool finish[n];

	for (int i = 0; i < m; i++)
	{
		work[i] = resTable[i].available;
	}

	for (int i = 0; i < n; i++)
		finish[i] = false;

	for (int i = 0; i < n; i++)
	{
		if (finish[i]) continue;

		if (!processTable[i].occupied)
		{
			finish[i] = true;
			continue;
		}
		if (req_lt_avail(i, m, work))
		{
			finish[i] = true;
			for (int j = 0; j < m; j++)
				work[j] += resTable[j].allocation[i];
			i = -1;
		}
	}

	int cnt = 0;
	for (int i = 0; i < n; i++)
	{
		if (processTable[i].occupied && !finish[i])
			cnt++;
	}

	dlCnt = cnt;

	if (cnt <= 0)
		return false;

	else return true;
}

void recoverDeadlock(int m, int n)
{
	
	int work[m];
	bool finish[n];
	for (int i = 0; i < m; i++)
	{
		work[i] = resTable[i].available;
	}
	for (int i = 0; i < n; i++)
	{
		finish[i] = false;
	}

	bool prog;
	do
	{
		prog = false;
		for (int i = 0; i < n; i++)
		{
			if (!finish[i] && processTable[i].occupied && req_lt_avail(i, m, work))
			{
				finish[i] = true;
				prog = true;
				for (int j = 0; j < m; j++)
					work[j] += resTable[j].allocation[i];
			}
		}
	} while (prog);

	int victim = -1;
	for (int i = 0; i < n; i++)
	{
		if (processTable[i].occupied && !finish[i])
		{
			victim = i;
			break;
		}
	}

	if (victim < 0)
		return;

	pid_t vpid = processTable[victim].pid;
	printf("Master: Running deadlock recovery at time %d:%09d. Deadlocked process: P%d\n", shm_ptr[0], shm_ptr[1], victim);
	printf("Master: Terminating P%d (pid %d) to recover\n", victim, vpid);
	dlKills++;
	kill(vpid, SIGKILL);
	waitpid(vpid, NULL, 0);

	for (int i = 0; i < m; i++)
	{
		int held = processTable[victim].held[i];
		if (held > 0)
		{
			resTable[i].available += held;
			resTable[i].allocation[victim] = 0;
			processTable[victim].held[i] = 0;
		}
		resTable[i].request[victim] = 0;

		while (!resTable[i].waitQueue.empty() && resTable[i].available > 0)
		{
			int indx = resTable[i].waitQueue.front();
			resTable[i].waitQueue.pop();

			resTable[i].available--;
			resTable[i].allocation[indx]++;
			processTable[indx].held[i]++;
			resTable[i].request[indx]--;

			buf.mtype = processTable[indx].pid;
			buf.granted = true;
			if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
			{
				perror("msgsnd recovery wakeup");
				exit(1);
			}
			

		}
	}

	processTable[victim].occupied = 0;
	running--;
	

}

int main(int argc, char* argv[])
{
	// Signal that will terminate program after 3 sec (real time)
	signal(SIGALRM, signal_handler);
	alarm(3);

	key_t key; // Key to access queue

	// Create file to track message queue
	system("touch msgq.txt");

	// Get key for message queue
	if ((key = ftok("msgq.txt", 1)) == -1)
	{
		perror("ftok");
		exit(1);
	}

	// Create message queue
	if ((msqid = msgget(key, PERMS | IPC_CREAT)) == -1)
	{
		perror("msgget in parent\n");
		exit(1);
	}	

	printf("Message queue set up\n");

	// **GET RID OF THIS**
	logfile = fopen("ossLog.txt", "w");
	if(logfile == NULL)
	{
		fprintf(stderr, "Failed to open log file.\n");
		return EXIT_FAILURE;
	}

	// Structure to hold values for options in command line argument
	options_t options;

	// Set default values
	options.proc = 1;
	options.simul = 1;
	options.interval = 0;


	// Values to keep track of child iterations
	int total = 0; // Total amount of processes
	running = 0;
	//int lastForkSec = 0; // Time in sec since last fork
	//int lastForkNs = 0; // Time in ns since last fork
	int msgsnt = 0;

	const char optstr[] = "hn:s:t:i:f"; // Options h, n, s, t, i, f
	char opt;
	
	// Parse command line arguments with getopt
	while ( (opt = getopt(argc, argv, optstr)) != -1)
	{
		switch(opt)
		{
			case 'h': // Help
				// Prints usage
				print_usage(argv[0]);
				return EXIT_SUCCESS;
			case 'n': // Total amount of processes
				// Check if n's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Check if next character starts with other option, meaning no argument given for n and another option given
					if (optarg[1] == 's' || optarg[1] == 'i' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print an error statement, print usage, and exit program
						fprintf(stderr, "Error! Option n requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is still invalid input
					{
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in n's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						// If non digit is found, print error statement, print usage, and exit program
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set proc to optarg and break
				options.proc = atoi(optarg);
				break;
			
			case 's': // Total amount of processes that can run simultaneously
				// Checks if s's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Check if next character starts with other option, meaning no argument given for n and another option given
					if (optarg[1] == 'n' || optarg[1] == 'i' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print an error statement, print usage, and exit program
						fprintf(stderr, "Error! Option s requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is still invalid input
					{
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in s's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set simul to optarg and break
				options.simul = atoi(optarg);
				if (options.simul > 18)
				{
					fprintf(stderr, "Error! Value entered for options s cannot exceed 18. %d > 18.\n", options.simul);
					print_usage(argv[0]);
					return EXIT_FAILURE;
				}
				break;

			case 'i':
				// Checks if i's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Checks if next character is character of other option, meaning no argument given for i and another option given
					if (optarg[1] == 'n' || optarg[1] == 's' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print error statement, print usage, and exit program
						fprintf(stderr, "Error! Option i requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is invalid input
					{
						// Print error statement, print usage, and exit program
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in i's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set interval to optarg, converting to ns,  and break
				options.interval = atoll(optarg) * 1000000;
				break;

			case 'f': // Print output also to logfile if option is passed
				logging = true;
				// Open logfile
				logfile = fopen("ossLog.txt", "w");
				if (logfile == NULL)
				{
					fprintf(stderr, "Error! Failed to open logfile.\n");
					return EXIT_FAILURE;
				}
				break;

			default:
				// Prints message that option given is invalid, prints usage, and exits program
				fprintf(stderr, "Error! Invalid option %c.\n", optopt);
				print_usage(argv[0]);
				return EXIT_FAILURE;
		}
	}
			

	// Set up shared memory for clock
	shareMem();

	// Allocate memory for process table based on total processes
	processTable = new PCB[20];

	resTable = new Resource[5];
	for (int i = 0; i < 5; i++)
	{
		resTable[i].total = 10;
		resTable[i].available = 10;
		for (int j = 0; j < 18; j++)
		{
			resTable[i].allocation[j] = 0;
			resTable[i].request[j] = 0;
		}
	}
	// Variables to track last printed time
	long long int lastPrintSec = shm_ptr[0];
	long long int lastPrintNs = shm_ptr[1];

	long long lastChkSec = shm_ptr[0];
	long long lastChkNs = shm_ptr[1];

	// Initialize process table, all values set to 0
	for (int i = 0; i < 18; i++)
	{
		processTable[i].occupied = 0;
		processTable[i].waitingOn = -1; // Double check this, rn -1 means not blocked on any resource
		for (int j = 0; j < 5; j++) // NEED TO ADD DEFINE FOR MAX RES
		{
			processTable[i].held[j] = 0;
		}
	}

	long long currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
	long long nSpawnT = currTimeNs + options.interval;
	// Loop that will continue until amount of 100 total child processes is reached or until running processes is 0
	// Ensures only 100 total  processes are able to run, and that no processses are still running when the loop ends
	while (total < options.proc ||  running > 0)
	{
		// Update system clock
		incrementClock();

		pid_t pid;
		int status;
		// Loop to terminate any finished children
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		{
			regTerms++;

			int indx;
			for (int i = 0; i < options.proc; i++)
			{
				if (processTable[i].occupied == 1 && processTable[i].pid == pid)
				{
					indx = i;
					break;
				}
			}

			// if (idx < 0) continue; (w/ indx initially set to -1)

			for (int i = 0; i < 5; i++)
			{
				int hCount = processTable[indx].held[i];
				if (hCount > 0)
				{
					//resTable[i].available += hCount;
					//resTable[i].allocation[indx] = 0;
					//processTable[indx].held[i] = 0;
				}
				resTable[i].request[indx] = 0;

				queue<int> temp;
				while(!resTable[i].waitQueue.empty())
				{
					int qIndx = resTable[i].waitQueue.front();
					resTable[i].waitQueue.pop();
					if (qIndx != indx)
						temp.push(qIndx);
				}
				resTable[i].waitQueue = temp;
			}
			processTable[indx].occupied = 0;
			running--;
		}

		long long chkDiffSec = shm_ptr[0] - lastChkSec;
		long long chkDiffNs = shm_ptr[1] - lastChkNs;
		if (chkDiffNs < 0)
		{
			chkDiffSec--;
			chkDiffNs += 1000000000;
		}
		long long chkTotDiff = chkDiffSec * 1000000000 + chkDiffNs;

		if (chkTotDiff >= 1000000000)
		{
			if (deadlock(MAX_RES, options.simul))
			{
				dlRuns++;
				totDlProcs += dlCnt;
			}
			while (deadlock(MAX_RES, options.simul))
			{
				dlKills++;
				printf("\nMaster: Deadlock detected at time %d:%09d\n\n", shm_ptr[0], shm_ptr[1]);
				recoverDeadlock(MAX_RES, options.simul);
			}
			
			lastChkSec = shm_ptr[0];
			lastChkNs = shm_ptr[1];
		}

		// Calculate time since last print for sec and ns
		long long int printDiffSec = shm_ptr[0] - lastPrintSec;
		long long int printDiffNs = shm_ptr[1] - lastPrintNs;
		// Adjust ns value for subtraction resulting in negative value
		if (printDiffNs < 0)
		{
			printDiffSec--;
			printDiffNs += 1000000000;
		}
		// Calculate total time sincd last print in ns
		long long int printTotDiff = printDiffSec * 1000000000 + printDiffNs;

		if (printTotDiff >= 500000000) // Determine if time of last print surpasssed .5 sec system time
		{
			// If true, print table and update time since last print in sec and ns
			//printInfo(18);
			lastPrintSec = shm_ptr[0];
			lastPrintNs = shm_ptr[1];
		}

		currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
		// Determine if a new child process can be spawned
		// Must be greater than next spawn time, less than total process allowed (100), and less than simultanous processes allowed (18)
		if (currTimeNs >= nSpawnT && total < options.proc  && running < options.simul)
		{
			//Fork new child
			pid_t childPid = fork();
			if (childPid == 0) // Child process
			{
				// Create array of arguments to pass to exec. "./worker" is the program to execute, arg is the command line argument
				// to be passed to "./worker", and NULL shows it is the end of the argument list
				char* args[] = {"./worker",  NULL};
				// Replace current process with "./worker" process and pass iteration amount as parameter
				execvp(args[0], args);
				// If this prints, means exec failed
				// Prints error message and exits
				fprintf(stderr, "Exec failed, terminating!\n");
				exit(1);
			}
			else // Parent process
			{
				// Increment total created processes and running processes
				total++;
				running++;
					
				// Increment clock
				incrementClock();

				// Update table with new child info
				for (int i = 0; i < 18; i++)
				{
					if (processTable[i].occupied == 0)
					{
						processTable[i].occupied = 1;
						processTable[i].pid = childPid;
						processTable[i].startSeconds = shm_ptr[0];
						processTable[i].startNano = shm_ptr[1];
						break;
					}
				}
				currTimeNs = (shm_ptr[0] * 1000000000) + shm_ptr[1];
				nSpawnT = currTimeNs + options.interval;

			}
		}

		if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer) - sizeof(long), 1, IPC_NOWAIT) == -1)
		{
			if (errno == ENOMSG)
			{
				//printf("Master: no message at %d:%09d\n", shm_ptr[0], shm_ptr[1]);
			}
			else 
			{
				perror("msgrcv");
				exit(1);
			}
		}
		else
		{
			int indx = -1;
			for (int i = 0; i < 18; i++)
			{
				if (processTable[i].occupied == 1 && processTable[i].pid == rcvbuf.pid)
				{
					indx = i;
					break;
				}
			}
			
			if (indx >= 0)
			{
				int r = rcvbuf.resId;
				if (!rcvbuf.isRelease)
				{
					printf("Master: Process %d REQUESTS R%d at time %d:%09d\n", rcvbuf.pid, r, shm_ptr[0], shm_ptr[1]);
					if (resTable[r].available > 0)
					{
						printf("Master: GRANTING R%d to PRocess %d (avail:%d->%d)\n", r, rcvbuf.pid, resTable[r].available, resTable[r].available-1);
						resTable[r].available--;
						resTable[r].allocation[indx]++;
						processTable[indx].held[r]++;

						buf.mtype = rcvbuf.pid;
						buf.granted = true;
						if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
						{
							perror("msgsnd grant");
							exit(1);
						}
						immGrant++;
					}
					
					else 
					{
						printf("Master: no instances of R%d for Process %d; enqueueing\n", r, rcvbuf.pid);
						resTable[r].request[indx]++;
						resTable[r].waitQueue.push(indx);
					}
				}
				else
				{
					printf("Master: Process %d RELEASES R%d at time %d:%09d\n", rcvbuf.pid, r, shm_ptr[0], shm_ptr[1]);
					resTable[r].allocation[indx]--;
					processTable[indx].held[r]--;
					resTable[r].available++;

					printf("Master: ACK release of R%d from Process %d (avail:%d)\n", r, rcvbuf.pid, resTable[r].available);
					buf.mtype = rcvbuf.pid;
					buf.granted = true;
					if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
					{
						perror("msgsnd release");
						exit(1);
					}

					if (!resTable[r].waitQueue.empty())
					{
						int n = resTable[r].waitQueue.front();
						resTable[r].waitQueue.pop();
						resTable[r].available--;
						resTable[r].allocation[n]++;
						processTable[n].held[r]++;
						resTable[r].request[n]--;

						buf.mtype = processTable[n].pid;
						buf.granted = true;
						if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
						{
							perror("msgsnd wakeup");
							exit(1);
						}
						waitGrant++;
					}
				}

			}
		}

	}

	printf("Total dl procs: %d\n", totDlProcs);
	double dlPerc;
        if (totDlProcs > 0)
		dlPerc = 100.0 * dlKills / totDlProcs;


	printf("\n----Final Statistics----\n");
	printf("Immediate grants: %d\n", immGrant);
	printf("Grants after waiting: %d\n", waitGrant);
	printf("Successful terminations: %d\n", regTerms);
	printf("Deadlock detections: %d\n", dlRuns);
	printf("Processes killed by deadlock recovery: %d\n", dlKills);
	printf("Percentage of deadlocked processes that were killed: %d%%\n", (int)dlPerc);

	if (logging)
	{
		fprintf(logfile, "\n----Final Statistics----\n");
		fprintf(logfile, "Immediate grants: %d\n", immGrant);
		fprintf(logfile, "Grants after waiting: %d\n", waitGrant);
		fprintf(logfile, "Successful terminations: %d\n", regTerms);
		fprintf(logfile, "Deadlock detections: %d\n", dlRuns);
		fprintf(logfile, "Processes killed by deadlock recovery: %d\n", dlKills);
		fprintf(logfile, "Percentage of deadlocked processes that were killed: %d%%\n", (int)dlPerc);
	}

	// Detach from shared memory and remove it
	if(shmdt(shm_ptr) == -1)
	{
		perror("shmdt failed");
		exit(1);
	}
	if (shmctl(shm_id, IPC_RMID, NULL) == -1)
	{
		perror("shmctl failed");
		exit(1);
	}

	// Remove the message queue
	if (msgctl(msqid, IPC_RMID, NULL) == -1)
	{
		perror("msgctl failed");
		exit(1);
	}

	return 0;

}




