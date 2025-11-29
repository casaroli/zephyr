/*
 * Copyright (c) 2025 Atym, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "fcntl.h"
#include "syscalls/kernel.h"
#include <sys/timerfd.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define BUFFER_SIZE (1000)

#define fatal(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

K_THREAD_STACK_DEFINE(thread_stack, 8192);
static struct k_thread thread;

static void thread_pipe_reader(void *arg1, void *arg2, void *arg3)
{

    // int fd = ((int *)arg1)[1];
    int fd = *((int *)arg1);
    // int pipefd = (*(int **[2])arg1)[0];
    char buffer[BUFFER_SIZE];

    while(1) {
        ssize_t rc = read(fd, buffer, BUFFER_SIZE);
        if (rc == -1) {
            printf("read returned %d, errno %d\n", rc, errno);
            sleep(1);
        }
        buffer[rc] = '\0';
        printf("Read %zd bytes: %s\n", rc, buffer);
    }
}

int main(void)
{
    int rc;
    int pipefd[2];

    rc = pipe2(pipefd, 0);
    if (rc == -1) {
        fatal("pipe");
    }

    printf("READ FD %d\n", pipefd[0]);


   	k_thread_create(&thread, thread_stack, K_THREAD_STACK_SIZEOF(thread_stack),
			thread_pipe_reader, &pipefd[0], NULL, NULL, 0, 0, K_NO_WAIT);


    for (int i = 0; i < 10; i++) {
        char buffer[100];
        sprintf(buffer, "Hello %d", i);
        printf("will write %s\n", buffer);
        rc = write(pipefd[1], buffer, strlen(buffer));
        printf("wrote %d bytes\n", rc);
        if (rc != strlen(buffer)) {
            fatal("write");
        }
    }

    // close(pipefd[1]);

	k_thread_join(&thread, K_FOREVER);

	close(pipefd[0]);
	// close(pipefd[1]);

	printf("Pipe closed\n");


	printf("Thread joined\n");

	return 0;
}


int main2(void)
{
    int rc;
    int pipefd[2];

    rc = pipe(pipefd);
    if (rc == -1) {
        fatal("pipe");
    }

    rc = write(pipefd[1], "Hello", 5);
    if (rc != 5) {
        fatal("write");
    }

    char buffer[11];

    rc = read(pipefd[0], buffer, 10);
    printf("Read returned %d\n", rc);
    if (rc < 0) {
        fatal("read");
    }

    buffer[rc] = '\0';
    printf("Received message: %s\n", buffer);

	close(pipefd[0]);
	close(pipefd[1]);

	printf("Pipe closed\n");

	return 0;
}
