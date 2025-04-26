// Operating Systems Project 4
// Author: Maija Garson
// Date: 04/18/2025
// Description: Worker process launched by oss. Uses the clock in shared memory and loops until receiving a message from the parent containing its time
// quantum. Once it receives the quantum, it generates a random number between 0-99 that will determine if it will either terminate (0-19), block
// from I/O (20-49) or run its full time quantum (50-99). It will then compute the simulated time that it used and send a message back to the
// parent containing the amount of time it used along with a status flag: 0 means terminate, -1 means it is blocked, and 1 means it ran its full time 
// quantum given. Once it decides to terminate, it will leave the loop and terminate.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <cstdio>
#include <cstdlib>

#define PERMS 0644
#define MAX_RES 5
#define INST_PER_RES 10
#define BOUND_NS 100000000
#define TERM_CHECK_NS 250000000
#define LIFE_NS 1000000000

typedef struct msgbuffer
{
	long mtype;
	int resId;
	bool isRelease;
	bool granted;
} msgbuffer;

int *shm_ptr;
int shm_id;

// Function to attach to shared memory
void shareMem()
{
	// Generate key
	const int sh_key = ftok("main.c", 0);
	// Access shared memory
	shm_id = shmget(sh_key, sizeof(int) * 2, 0666);

	// Determine if shared memory access not successful
	if (shm_id == -1)
	{
		// If true, print error message and exit 
		fprintf(stderr, "Child: Shared memory get failed.\n");
		exit(1);
	}

	// Attach shared memory
	shm_ptr = (int *)shmat(shm_id, 0, 0);
	//Determine if insuccessful
	if (shm_ptr == (int *)-1)
	{
		// If true, print error message and exit
		fprintf(stderr, "Child: Shared memory attach failed.\n");
		exit(1);
	}
}

void addTime()
{
	shm_ptr[1] += 1000;
	if (shm_ptr[1] > 1000000000)
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}
}

int main(int argc, char* argv[])
{
	shareMem();
	
	// Info needed for message sending/receiving
	msgbuffer buf;
	msgbuffer rcvbuf;
	buf.mtype = 1;
	int msqid = 0;
	key_t key;

	// Get key for message queue
	if ((key = ftok("msgq.txt", 1)) == -1)
	{
		perror("ftok");
		exit(1);
	}

	// Create message queue
	if ((msqid = msgget(key, PERMS)) == -1)
	{
		perror("msgget in child\n");
		exit(1);
	}

	int held[MAX_RES] = {0};

	long long startTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
	long long lastTermChk = startTimeNs;

	srand(getpid());
	long long nAct = startTimeNs + (rand() % BOUND_NS);

	// Loop that loop suntil determined end time is reached
	while(true)
	{
		long long currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];

		if (currTimeNs - lastTermChk >= TERM_CHECK_NS)
		{
			lastTermChk = currTimeNs;
			if (currTimeNs - startTimeNs >= LIFE_NS)
			{
				for (int i = 0; i < MAX_RES; i++)
				{
					while (held[i]-- > 0)
					{
						buf.mtype = 1;
						buf.resId = i;
						buf.isRelease = true;

						if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
						{
							perror("msgsnd release");
							exit(1);
						}
						//addTime()
						
						if (msgrcv(msqid, &rcvbuf, sizeof(rcvbuf) - sizeof(long), getpid(), 0) == -1)
						{
							perror("msgrcv release ack");
							exit(1);
						}
						//addTime
						
					}
				}
				break;
			}
		}

		if (currTimeNs >= nAct)
		{
			int outcome = rand() % 100;
			bool release;
			if (outcome > 30)
				release = true;
			else
				release = false;

			int r;
			if (release)
			{
				int tries = 0;
				while (tries < MAX_RES)
				{
					r = rand() % MAX_RES;
					if (held[r] > 0)
						break;
					tries++;
				}
				if (tries == MAX_RES)
				{
					nAct = currTimeNs + (rand() % BOUND_NS);
					continue;
				}
			}
			else // Request
			{
				int tries = 0;
				while (tries < MAX_RES)
				{
					r = rand() % MAX_RES;
					if (held[r] > 0)
						break;
					tries++;
				}
				if (tries == MAX_RES)
				{
					nAct = currTimeNs + (rand() % BOUND_NS);
					continue;
				}
			}
		

			buf.mtype = 1;
			buf.resId = r;
			buf.isRelease = release;
			buf.granted = false;
			if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
			{
				perror("msgsnd request");
				exit(1);
			}

			//addTime()
		
			// Wait until Oss sends a message back
			if (msgrcv(msqid, &rcvbuf, sizeof(rcvbuf) - sizeof(long), getpid(), 0) == -1)
			{
				perror("msgrcv grant");
				exit(1);
			}

			//addTime()

			if (rcvbuf.granted && !release) // If new resource was received
			{
				// Increment held at resoure's location 
				held[r]++;
			}

			nAct = currTimeNs + (rand() % BOUND_NS);
		}
	}



	
	// Detach from memory
	if (shmdt(shm_ptr) == -1)
	{
		perror("memory detach failed in worker\n");
		exit(1);
	}

	return 0;
}
