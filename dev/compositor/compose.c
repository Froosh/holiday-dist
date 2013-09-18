/*
 * The compositor opens a series of pipes 
 * These can be used by processes that wish to write to the Holiday bulbs
 * The processes can send a message that indicates how to write to the bulbs
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the MIT License.
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/types.h>
#include <time.h>
#include <string.h>

#include "spi.h"
#include "compose.h"

#define NUM_BUFS 4			// We have four buffers to mix from
#define LINE_SIZE 9			// 8 characters plus newline
#define MESSAGE_SIZE ((NUM_BULBS + 2) * LINE_SIZE)

// Bit flags for pipe_data.flags
#define NEW_DATA_FLAG   0b0000000000000001		// True if this data is freshly received from pipe
#define REPLACE_FLAG	0b0000000000000010		// if set replace buffer, otherwise mix
#define MASK_FLAG		0b0000000000000100		// if set non-zero bytes are masked in
#define HAS_DATA_FLAG	0b0000000000001000		// Set if there is data to render
#define CLEAR_FLAG		0b0000000000010000		// Set if we clear all the buffers
#define BRIGHTNESS_FLAG 0b1000000000000000		// Set the brightness in the following 7 bits
#define BRIGHTNESS_MASK 0b0111111100000000		// These are the bits used to determine brightness


uint8_t txbuf[NUM_BULBS * 3];	// Global frame buffer, allocate once
int string_size = NUM_BULBS;	

int pipe_fd;				// Pipe file descriptors

struct pipe_data {
	unsigned long flags;		// Various flags to control things and stuff
	unsigned long pid;			// Number of process or app sending this data
	time_t timestamp;	// When was the last time this buffer was written to?
	uint8_t rgbbuf[(NUM_BULBS*3)];	// Storage for the bulb RGB values
};

struct pipe_data pipebuf[NUM_BUFS];	// Storage for the pipe buffers

// Open the named pipe as given, return file descriptor to it
// If the returned value is less than zero, it failed
int open_pipe(void) {
char* name="/run/compose.fifo";
int fd, err;

	// Create the named pipe
	err = mkfifo(name, 0666);
	if (err != 0) {
		return(-100);
	}

	// Set permissions on named pipe so everyone can write to it
	err = chmod(name, 0666);
	if (err != 0) {
		return(-101);
	}

	// Open the pipe in a non-blocking way and return a descriptor thingy
	fd = open(name, O_RDONLY | O_NONBLOCK);
	return fd; 			// return the file descriptor
}

int check_pipe(void) {
int avail;
int err;

	// Does a pipe have data available?
	// Is there some way we can know this?
	// I sure hope so or this ain't gonna work
	err = ioctl(pipe_fd, FIONREAD, &avail);
	if (err == -1) {
		return -1;
	}
	//printf("Pipe %d has %d bytes to read\n", index, avail);
	return avail;
}

// Sum colour values, but peg at 0xff
uint8_t sum_colors(c0, c1, c2, c3)
{
unsigned int res;
	res = c0 + c1 + c2 + c3;
	if (res > 0xff) {
		res = 0xff;
	}
	return (uint8_t) res;
}

// The renderer is the heart of the compositor.
// It will mix the buffers together IF the flags are set correctly
// Otherwise it might do other sorts of things. Or something.
void render(void) {
int j, k;

	// The first pass on the renderer
	// All it does is sum the colour values
	// When it pegs, it pegs.  It does not wrap.  Hopefully.
	for(j = 0, k = 0; j < NUM_BULBS; j++) {
		txbuf[k] = sum_colors(pipebuf[0].rgbbuf[k], pipebuf[1].rgbbuf[k], 
			pipebuf[2].rgbbuf[k], pipebuf[3].rgbbuf[k]);
		k++;
		txbuf[k] = sum_colors(pipebuf[0].rgbbuf[k], pipebuf[1].rgbbuf[k], 
			pipebuf[2].rgbbuf[k], pipebuf[3].rgbbuf[k]);
		k++;
		txbuf[k] = sum_colors(pipebuf[0].rgbbuf[k], pipebuf[1].rgbbuf[k], 
			pipebuf[2].rgbbuf[k], pipebuf[3].rgbbuf[k]);
		k++;
	}

	// And send it on its merry way
	spi_send(txbuf, (NUM_BULBS*3));	
}


void clear_buffers(void) {
int j;

	// Clear all the buffers
	for (j=0; j < NUM_BUFS; j++) {
		memset(&pipebuf[j], 0x00, sizeof(struct pipe_data));
	}
	render();		// And start with black	
}

// Return an index to a pipebuf that has a matching PID
int match_pid(unsigned long apid) {
int j;

	for (j=0; j < NUM_BUFS; j++) {
		if (pipebuf[j].pid == apid) {
			return j;
		}
	}
	return -1;
}

// Return an index to the buffer with the oldest timestamp
int oldest_buf(void) {
int j;
time_t oldtime = pipebuf[j].timestamp;
int oldest = 0;

	for (j=1; j < NUM_BUFS; j++) {	
		if (pipebuf[j].timestamp < oldtime) {
			oldest = j;
			oldtime = pipebuf[j].timestamp;
		}
	}
	return oldest;
}

int read_pipe(void) {
int j, k;
int dest_buf;
char read_message[MESSAGE_SIZE];
ssize_t bytes_read;
unsigned long rawcolor;
uint8_t r, g, b;
int ln = 0;
struct pipe_data read_data;

	// Read an entire message in from the pipe and store it in the appropriate array thingy
	// It should be in 52 lines. We hope.
	bytes_read = read(pipe_fd, read_message, (size_t) MESSAGE_SIZE);
	if (bytes_read < MESSAGE_SIZE) {
		printf("Message too short!\n");
	}
	//printf("%sEOM\n", read_message);

	// The first line is flags, we're just kind of glossing this for now.
	//bytes_read = read(pipe_fd[index], read_string, (size_t) LINE_SIZE);
	//read_string[bytes_read] = 0x00;		// Null terminate end of string
	read_data.flags = strtoll(&read_message[(ln++ * LINE_SIZE)], NULL, 16);
	read_data.flags |= (NEW_DATA_FLAG + HAS_DATA_FLAG);			// Set the new data flag

	// The next line is the process ID of the sending process, store it too
	read_data.pid = strtoll(&read_message[(ln++ * LINE_SIZE)], NULL, 16);
	//printf("Received buffer PID %d\n", read_data.pid);

	// And the next fifty lines are bulb values.  No, really.
	for(j = 0, k = 0; j < NUM_BULBS; j++) {

		rawcolor = strtol(&read_message[(ln++ * LINE_SIZE)], NULL, 16);
		r = (rawcolor >> 16) & 0xFF;
 		g = (rawcolor >> 8) & 0xFF;
 		b = rawcolor & 0xFF;

		// Here we'll do something with the bytes we've read in.
		read_data.rgbbuf[k++] = r;
		read_data.rgbbuf[k++] = g;
		read_data.rgbbuf[k++] = b;
		//printf("Bulb %d has values %02X %02X %02X\n", j, r, g, b);
	}
	//read_data.rgbbuf[k++] = 0x00;		// And null last 3 bytes
	//read_data.rgbbuf[k++] = 0x00;
	//read_data.rgbbuf[k++] = 0x00;

	// Now affix a timestamp to the buffer
	read_data.timestamp = time(NULL);

	if (read_data.flags & CLEAR_FLAG) {
		//printf("Clearing buffers\n");
		for (j=0; j < NUM_BUFS; j++) {
			memset(&pipebuf[j], 0x00, sizeof(struct pipe_data));
		}
	}

	//printf("Everything read in\n");

	// OK, now that the data has been read in, what do we do with it?
	// If the PID matches an existing one, we copy the data to that buffer.
	// If there are no matching PIDs, we overwrite the oldest buffer.
	if ((dest_buf = match_pid(read_data.pid)) == -1) {
		printf("Buffers full, going for oldest buffer.\n");
		dest_buf = oldest_buf();
	}
	//printf("Destination buffer is %d\n", dest_buf);

	// Now copy it into the appropriate destination buffer.
	memcpy(&pipebuf[(uint8_t) dest_buf], &read_data, sizeof(struct pipe_data));
	//pipebuf[dest_buf].flags = read_data.flags;
	//pipebuf[dest_buf].pid = read_data.pid;
	//pipebuf[dest_buf].timestamp = read_data.timestamp;

	// And the next fifty lines are bulb values.  No, really.
	//for(j = 0, k = 0; j < NUM_BULBS; j++) {
		// Copy the color values.
	//	pipebuf[dest_buf].rgbbuf[k] = read_data.rgbbuf[k++];
	//	pipebuf[dest_buf].rgbbuf[k] = read_data.rgbbuf[k++];
	//	pipebuf[dest_buf].rgbbuf[k] = read_data.rgbbuf[k++];
	//}	

	// And render away!
	render();

}

void set_pixel(int pixnum, uint8_t r, uint8_t g, uint8_t b) {
	int offset;

#ifdef PREPROTO
	// Make sure everything stays within the appropriate limits
	r = r >> 1;
	g = g >> 1;
	b = b >> 1;
#endif
	
	// Need to check for out-of-bounds here, but meh.
	//printf("col %d row %d offset %d\n", col, row, (*climap + col)[row]);
	// Calculate the offset into the buffer
	offset = pixnum * 3;		// Get the correct offset from the map
#ifdef PROTO
	txbuf[offset] = g;
	txbuf[offset+1] = r;		// GGRRBB
#else
	txbuf[offset] = r;
	txbuf[offset+1] = g;
#endif
	txbuf[offset+2] = b;

}

int main(int argc, char *argv[])
{
	int j;
	int retuid;
	int doing = 1;			// While running, set true
	int avail = 0;			// Bytes available to read

	// Try to get some privleges here
	retuid = setuid(0);		// root me!
	if (retuid == -1) {
		perror("Could not setuid");
		exit(-10);
	}

	spi_open();

	// Open THE PIPE
	pipe_fd = open_pipe();
	if (pipe_fd < 0) {
		printf("Failed, exiting with %d\n", pipe_fd);
		spi_close();
		exit(pipe_fd);
	}

	clear_buffers();		// Clear the buffers

	// Any given pipe has to have at least 9 * 52 bytes to read (8 chars plus newline by 52 lines)
	// So let's just scan to see what's going on here
	while (doing) {
		avail = check_pipe();
		if (avail >= MESSAGE_SIZE) {
			//printf("Pipe %d has %d bytes to read\n", j, avail);
			read_pipe();
		}
		usleep(1000);
	}

	spi_close();

	return 0;
}
