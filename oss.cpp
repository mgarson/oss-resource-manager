
// Operating Systems Project 5
// Author: Maija Garson
// Date: 04/29/2025
// Description: A program that simulates a resource management operating system with deadlock detection and recovery.
// This program runs until it has forked the total amount of processes specified in the command line, while allowing a
// specified amount of processes to run simultanously. It will allocate shared memory to represent a system clock. It will
// also keep track of a process control block table for all processes and a resource table for 5 resource types with 10 
// instances each. It will receive messages from child processes that represent a resource request or release. If it 
// receives a request, it will grant the request if possible or it will add the child to a wait queue if not possible.
// When requests/releases are received, it will update values in both tables to reflect this. It will print both tables 
// every .5 sec of system time. It will run a deadlock detection algorithm every 1 sec of system time. If a deadlock is
// found, it will run a recovery algorithm, incrementally killing processes, until the system is no longer in a deadlock.
// It will calculate and print final statistics at the end of each run.
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
#define MAX_PROC 18

using namespace std;

// Structure for command line options
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

// Structure to hold resources in the system
typedef struct
{
	int total; // Total instances of resource
	int available; // Amount currently available
	int allocation[MAX_PROC] = {0}; // How many resources held by process
	int request[MAX_PROC] = {0}; // How many requests from proces
	queue<int> waitQueue; // Holds processes waiting for resources
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
msgbuffer buf; // Message buffer to send messages
msgbuffer rcvbuf; // Message buffer to receive messages

bool logging = false; // Bool to determine if output should also print to logfile
FILE* logfile = NULL; // Pointer to logfile

// Variables to determine final statistics
int immGrant = 0; // Amount of resource requests immediately granted
int waitGrant = 0; // Amount of resource requests granted after process waited
int regTerms = 0; // Amount of processes that terminated normally on their own
int dlRuns = 0; // Amount of times deadlock detection alg was run
int dlKills = 0; // Amount of processes killed by deadlock recovery alg
int totDlProcs = 0; // Total amount of processes that became deadlocked
int dlCnt = 0; // Number of processes in each deadlock run
int lastDl[MAX_PROC]; // Holds the pids of processes in each deadlock

void print_usage(const char * app)
{
	fprintf(stdout, "usage: %s [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f]\n", app);
	fprintf(stdout, "      proc is the number of total children to launch\n");
	fprintf(stdout, "      simul indicates how many children are to be allowed to run simultaneously\n");
	fprintf(stdout, "      itnterval is the time between launching children\n");
	fprintf(stdout, "      selecting f will output to a logfile as well\n");
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

// FUnction to print formatted process table and resource table to console. Will also print to logfile if necessary.
void printInfo(int n)
{

	// Print process control block 	
	printf("OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	printf("Entry\tOccupied\tPID\tStartS\tStartNs\n");
	
	if(logging) fprintf(logfile, "OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	if (logging) fprintf(logfile,"Entry\tOccupied\tPID\tStartS\tStartNs\n");

	for (int i = 0; i < n; i++)
	{
		// Print table only if occupied by process
		if (processTable[i].occupied == 1)
		{
			printf("%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
			if (logging) fprintf(logfile, "%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
		}
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");

	// Print resource table
	printf("Current system resources\n");
	if (logging) fprintf(logfile, "Current system resources\n");

	printf("\t");
	if (logging) fprintf(logfile, "\t");
	for (int i = 0; i < MAX_RES; i++)
	{
		printf("R%d\t", i);
		if (logging) fprintf(logfile, "R%d\t", i);
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");

	for (int i = 0; i < n; i++)
	{
		// Print resources held by process only if process is occupied in PCB
		if (processTable[i].occupied)
		{
			printf("P%d\t", i);
			if (logging) fprintf(logfile, "P%d\t", i);
			for (int j = 0; j < MAX_RES; j++)
			{
				printf("%d\t", processTable[i].held[j]);
				if (logging) fprintf(logfile, "%d\t", processTable[i].held[j]);
			}
			printf("\n");
			if (logging) fprintf(logfile, "\n");
		}
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");
	
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

// Function to check if process requests (p) for each resource type amount (m) is less than available resources (work[])
bool req_lt_avail(int p, int m, int work[])
{
	for (int i = 0; i < m; i++)
	{
		// Return false if requests are greater than available
		if (resTable[i].request[p] > work[i])
			return false;
	}
	// Otherwise, return true
	return true;
}

// Function to detect if system is deadlocked
bool deadlock(int m, int n)
{
	int work[m]; // Represents currently available resources
	bool finish[n]; // Represents which processes can finish (true) and which cannot get requests met (false)

	// Initialize work to currently available resources
	for (int i = 0; i < m; i++)
	{
		work[i] = resTable[i].available;
	}

	// Initialize finsih to false initially
	for (int i = 0; i < n; i++)
		finish[i] = false;

	// Loop through to find any processes that are able to finish
	for (int i = 0; i < n; i++)
	{
		// Continue if process was determined able to finish
		if (finish[i]) continue;

		// If process is unoccupied in table, treat as finished
		if (!processTable[i].occupied)
		{
			finish[i] = true;
			continue;
		}
		// Determine if process's requests can be met
		if (req_lt_avail(i, m, work))
		{
			// If true, mark process as able to finish
			finish[i] = true;
			// Release processes allocated resources back to work within the function
			for (int j = 0; j < m; j++)
				work[j] += resTable[j].allocation[i];
			// Restart loop from 0 
			i = -1;
		}
	}

	// Represents count of deadlocked processes
	int cnt = 0;
	// Find processes that are unable to finish and increment count and add to currently deadlocked array
	for (int i = 0; i < n; i++)
	{
		if (processTable[i].occupied && !finish[i])
		{
			lastDl[cnt] = i;
			cnt++;
		}
	}

	// Set global current deadlock count to cnt
	dlCnt = cnt;

	// Return false if no deadlocked processes were counted
	if (cnt <= 0)
		return false;

	// Otherwise, return true. Meaning deadlock.
	else return true;
}

// Function to recover from deadlock state by choosing a deadlocked process and killling it
void recoverDeadlock(int m, int n)
{
	
	int work[m]; // Represents resources available during recovery
	bool finish[n]; // Represents which processes can finish (true) or stay in deadlock (false)

	// Initialize work to current available resources
	for (int i = 0; i < m; i++)
	{
		work[i] = resTable[i].available;
	}
	// Initialize finish to false initially
	for (int i = 0; i < n; i++)
	{
		finish[i] = false;
	}

	bool prog; // Represents if any process has made progress in previous loop
	
	// Determine if any processes have requests that can be met
	do
	{
		// Initially false
		prog = false;
		// Loop through and determine if any process is occupied, not marked as finished, and able to have requests met
		for (int i = 0; i < n; i++)
		{
			if (!finish[i] && processTable[i].occupied && req_lt_avail(i, m, work))
			{
				// If true, set finsih for process i to true
				finish[i] = true;
				// Set prog to true since progress was made
				prog = true;
				// Release processes allocated resources back to work within the function
				for (int j = 0; j < m; j++)
					work[j] += resTable[j].allocation[i];
			}
		}
	} while (prog);

	// Reprsents pid of process to be killed
	int victim = -1;
	// Loop through and find process that is occupied and unable to finish
	for (int i = 0; i < n; i++)
	{
		if (processTable[i].occupied && !finish[i])
		{
			// If true, set victim to i and break
			victim = i;
			break;
		}
	}

	// Return if victim is less than 0, means no victim was found
	if (victim < 0)
		return;

	// Represents how many of each resource is held by victim
	int rHeld[MAX_RES];
	for (int i = 0; i < m; i++)
	{
		rHeld[i] = processTable[victim].held[i];
	}
	// Find victim's pid in process table
	pid_t vpid = processTable[victim].pid;

	printf("   Master terminating P%d to remove deadlock\n", victim);
	if (logging) fprintf(logfile, "    Master terminating P%d to remove deadlock\n", victim);

	// Kill victim and wait for it to finish
	kill(vpid, SIGKILL);
	waitpid(vpid, NULL, 0);
	dlKills++;

	printf("   Process P%d terminated\n", victim);
	if (logging) fprintf(logfile, "   Process P%d terminated\n", victim);
	printf("   Resources released: ");
	if (logging) fprintf(logfile, "   Resources released: ");

	// Loop through and list all resources released by victim
	bool first = true; // Represents if first resource has been printed. Used to determine when to print commas.
	for (int i = 0; i < m; i++)
	{
		// Print resource if more than 0
		if (rHeld[i] > 0)
		{
			if (!first)
			{
				printf(", ");
				if (logging) fprintf(logfile, ", ");
			}
			printf("R%d:%d", i, rHeld[i]);
			if (logging) fprintf(logfile, "R%d:%d", i, rHeld[i]);
			first = false;
		}
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");

	// Put resources held by victim back into resource and clear victim's held resources
	for (int i = 0; i < m; i++)
	{
		int held = processTable[victim].held[i];
		if (held > 0)
		{
			resTable[i].available += held;
			resTable[i].allocation[victim] = 0;
			processTable[victim].held[i] = 0;
		}
		// Clear any requests from victim
		resTable[i].request[victim] = 0;

		// Loop through to find any processes waiting for resource and see if request can be mad
		while (!resTable[i].waitQueue.empty() && resTable[i].available > 0)
		{// If able to meet request
			// Take next process in queue off queue and get its index
			int indx = resTable[i].waitQueue.front();
			resTable[i].waitQueue.pop();

			// Allocate resource from available resources to resources allocated to process
			resTable[i].available--;
			resTable[i].allocation[indx]++;
			processTable[indx].held[i]++;
			// Decrement process request
			resTable[i].request[indx]--;

			// Send message to process to grant resource request
			buf.mtype = processTable[indx].pid;
			buf.granted = true;
			if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
			{
				perror("msgsnd recovery wakeup");
				exit(1);
			}
			

		}
	}

	// Mark victim as unoccupied in process table 
	processTable[victim].occupied = 0;
	// Decrement amount of currently running processes
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

	// Variables to track last deadlock check
	long long lastChkSec = shm_ptr[0];
	long long lastChkNs = shm_ptr[1];

	// Initialize process table, all values set to empty
	for (int i = 0; i < MAX_PROC; i++)
	{
		// Set occupied to 0
		processTable[i].occupied = 0;
		// Set waitingon to -1, meaning process is not waiting for any resource
		processTable[i].waitingOn = -1; 
		for (int j = 0; j < MAX_RES; j++) 
		{
			// Set held resources to 0
			processTable[i].held[j] = 0;
		}
	}

	// Calculate current system time in ns
	long long currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
	// Calculate next time to spawn a process based on command line value given for interval
	long long nSpawnT = currTimeNs + options.interval;

	// Loop that will continue until total amount of processes given are launched and all running processes are terminated
	while (total < options.proc ||  running > 0)
	{
		// Update system clock
		incrementClock();

		// Loop through and terminate any process that are finished
		pid_t pid;
		int status;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		{
			// Increment regular terminations
			regTerms++;

			// Find process's location in process table
			int indx;
			for (int i = 0; i < options.proc; i++)
			{
				if (processTable[i].occupied == 1 && processTable[i].pid == pid)
				{
					indx = i;
					break;
				}
			}

			// Use process table index to clear values for process
			for (int i = 0; i < MAX_RES; i++)
			{
				// Set request in resource table for process to 0
				resTable[i].request[indx] = 0;

				queue<int> temp; // Temporary queue to loop through wait queue
				// Loop through wait queue to determine if finished process is in queue
				while(!resTable[i].waitQueue.empty())
				{
					// Incrementally remove processes from wait queue
					int qIndx = resTable[i].waitQueue.front();
					resTable[i].waitQueue.pop();
					// If queue index does not match processs, add to temp queue
					if (qIndx != indx)
						temp.push(qIndx);
				}
				// Set waitqueue to temp queue
				resTable[i].waitQueue = temp;
			}
			// Mark finished process as unoccupied in process table
			processTable[indx].occupied = 0;
			// Decrement total processes running
			running--;
		}

		// Calculate time since last deadlock check for sec and ns
		long long chkDiffSec = shm_ptr[0] - lastChkSec;
		long long chkDiffNs = shm_ptr[1] - lastChkNs;
		// Adjust ns value for subtraction resulting in negative value
		if (chkDiffNs < 0)
		{
			chkDiffSec--;
			chkDiffNs += 1000000000;
		}
		// Calculate total time since last deadlock check in ns
		long long chkTotDiff = chkDiffSec * 1000000000 + chkDiffNs;

		if (chkTotDiff >= 1000000000) // Determine if time of last dl check surpassed 1 sec system time
		{
			printf("Master running deadlock detection at time %d:%09d: ", shm_ptr[0], shm_ptr[1]);
			if (logging) fprintf(logfile, "Master running deadlock detection at time %d:%09d: ", shm_ptr[0], shm_ptr[1]);
			if (deadlock(MAX_RES, options.simul)) // Check for deadlock
			{
				// If true, increment the amount of deadlock runs and add deadlocked processes to total amount
				dlRuns++;
				totDlProcs += dlCnt;
				// List deadlocked processes
				printf("Processes ");
				if (logging) fprintf(logfile, "Processes ");
				for (int i = 0; i < dlCnt; i++)
				{
					printf("P%d",lastDl[i]);
					if (logging) fprintf(logfile, "P%d", lastDl[i]);
					if (i < dlCnt - 1)
					{
						printf(", ");
						if(logging) fprintf(logfile, ", ");
					}
				}
				printf(" deadlocked\n");
				if (logging) fprintf(logfile, " deadlocked\n");
			}
			else // Otherwise, report no deadlock
			{ 
				printf("No deadlocks detected\n");
				if (logging) fprintf(logfile, "No deadlocks detected\n");
			}

			while (deadlock(MAX_RES, options.simul))
			{
				recoverDeadlock(MAX_RES, options.simul);
			}
			
			// Update time since last dl check to current system time for sec and ns
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
			printInfo(18);
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
				// Calculate current time and ns and determine next spawn time
				currTimeNs = (shm_ptr[0] * 1000000000) + shm_ptr[1];
				nSpawnT = currTimeNs + options.interval;

			}
		}

		// Check for message received from worker without blocking
		if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer) - sizeof(long), 1, IPC_NOWAIT) == -1)
		{
			if (errno == ENOMSG)
			{
			}
			else 
			{
				perror("msgrcv");
				exit(1);
			}
		}
		else // Message received
		{
			int indx = -1; // Represents index of process who sent message, initialized to -1
			// Loop through process table to find index of process from its pid
			for (int i = 0; i < 18; i++)
			{
				
				if (processTable[i].occupied == 1 && processTable[i].pid == rcvbuf.pid)
				{
					indx = i;
					break;
				}
			}
			
			if (indx >= 0) // Determine if process's index was found
			{
				int r = rcvbuf.resId; // Represents id of resource that worker sent to be requested or released
				if (!rcvbuf.isRelease) // Process is requesting
				{
					printf("Master has detected Process P%d requesting R%d at time %d:%09d\n", indx, r, shm_ptr[0], shm_ptr[1]);
					if (logging)
						fprintf(logfile, "Master has detected Process P%d requesting R%d at time %d:%09d\n", indx, r, shm_ptr[0], shm_ptr[1]);
					// Determine if requested resource is available
					if (resTable[r].available > 0)
					{
						// If true grant request
						printf("Master granting P%d requesting R%d at time %d:%09d \n", indx, r, shm_ptr[0], shm_ptr[1]);
						if (logging) 
							fprintf(logfile, "Master granting P%d requesting R%d at time %d:%09d \n", indx, r, shm_ptr[0], shm_ptr[1]);
						// Decrement amount available for resource in resource table
						resTable[r].available--;
						// Increment amount allocated to process for resource in resource table
						resTable[r].allocation[indx]++;
						// Increment amount of resource held by process in process table
						processTable[indx].held[r]++;

						// Prepare message to send to worker
						buf.mtype = rcvbuf.pid; // Represents worker's pid
						buf.granted = true; // Represents request being granted
						// Send message to worker to notify that request is being granted
						if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
						{
							perror("msgsnd grant");
							exit(1);
						}
						// Increment total immediate grants
						immGrant++;
					}
					
					else // Unable to grant request, not enough of requested resource
					{
						printf("Master: no instances of R%d available, P%d added to wait queue at time %d:%09d\n", r, indx, shm_ptr[0], shm_ptr[1]);
						if (logging)
							fprintf(logfile, "Master: no instances of R%d available, P%d added to wait queue at time %d:%09d\n", r, indx, shm_ptr[0], shm_ptr[1]);

						// Increment request in resource table for process
						resTable[r].request[indx]++;
						// Add process to wait queue
						resTable[r].waitQueue.push(indx);
					}
				}
				else // Process is releasing
				{
					// Decrement amount allocated to process for resource in resource table
					resTable[r].allocation[indx]--;
					// Decrement amount of resource held by process in process table
					processTable[indx].held[r]--;
					// Increment amount of resource available in resource table
					resTable[r].available++;

					printf("Master has acknowledged Process P%d releasing R%d at time %d:%09d\n", indx, r, shm_ptr[0], shm_ptr[1]);
					if (logging)
						fprintf(logfile, "Master has acknowledged Process P%d releasing R%d at time %d:%09d\n", indx, r, shm_ptr[0], shm_ptr[1]);

					// Prepare message to send to worker
					buf.mtype = rcvbuf.pid; // Represents worker's pid
					buf.granted = true; // Represents release being granted
					// Send message to notify of release
					if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
					{
						perror("msgsnd release");
						exit(1);
					}
					// List resources released
					printf("	Resources released : R%d:1\n", r);
					if (logging)
						fprintf(logfile, "        Resources released : R%d:1\n", r);

					// Determine if any processes are waiting in wait queue
					if (!resTable[r].waitQueue.empty())
					{
						// If true, get index of next waiting process and remove from queue to grant request
						int n = resTable[r].waitQueue.front();
						resTable[r].waitQueue.pop();
						// Decrement amount of resource available in rcs table
						resTable[r].available--;
						// Increment allocation of resource for process in rcs table
						resTable[r].allocation[n]++;
						// Increment amount of resource held by process in process table
						processTable[n].held[r]++;
						// Decrement requests from process in rcs table
						resTable[r].request[n]--;

						// Prepare message to send to process
						buf.mtype = processTable[n].pid; // Represents pid of worker
						buf.granted = true; // Represents request being granted
						// Send message to worker to notify that request is granted
						if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
						{
							perror("msgsnd wakeup");
							exit(1);
						}
						// Increment total wait grants
						waitGrant++;
					}
				}

			}
		}

	}

	// Calculate percentage of deadlocked processes that were killed, ensuring no division by 0
	double dlPerc;
        if (totDlProcs > 0)
		dlPerc = 100.0 * dlKills / totDlProcs;


	// Print final statistics to console
	printf("\n----Final Statistics----\n");
	printf("Immediate grants: %d\n", immGrant);
	printf("Grants after waiting: %d\n", waitGrant);
	printf("Successful terminations: %d\n", regTerms);
	printf("Deadlock detections: %d\n", dlRuns);
	printf("Processes killed by deadlock recovery: %d\n", dlKills);
	printf("Percentage of deadlocked processes that were killed: %.1f%%\n", dlPerc);

	// Print final statistics to logfile if necessary
	if (logging)
	{
		fprintf(logfile, "\n----Final Statistics----\n");
		fprintf(logfile, "Immediate grants: %d\n", immGrant);
		fprintf(logfile, "Grants after waiting: %d\n", waitGrant);
		fprintf(logfile, "Successful terminations: %d\n", regTerms);
		fprintf(logfile, "Deadlock detections: %d\n", dlRuns);
		fprintf(logfile, "Processes killed by deadlock recovery: %d\n", dlKills);
		fprintf(logfile, "Percentage of deadlocked processes that were killed: %.1f%%\n", dlPerc);
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




