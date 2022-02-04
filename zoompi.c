/*
 * Ultimate 4 channel audio recorder
 *
 * Copyright (C) 2018-2019 Adam Williams <broadcast at earthling dot net>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 */



// this runs on the pi for the zoom replacement, 
// recording SPI input & serving web pages

// scp it to the pi
// scp html/index.html.zoom 10.0.2.101:index.html
// scp zoompi.c zoom.h 10.0.2.101:
// gcc -O2 -o zoompi zoompi.c -lwiringPi -lpthread -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

// STM32 program: zoom.c

// html files are in html/
// uses index.html.zoom & all the images


// Connect a jumper from UART TX to the WIFI GPIO to lower the transmit
// power.  Necessary to reduce interference during recording.




#include <arpa/inet.h>
#include <errno.h>
#include <wiringPiSPI.h>
#include <wiringPi.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <ctype.h>
#include <sched.h>

#include "zoom.h"

// channel is the wiringPi name for the chip select (or chip enable) pin.
// Set this to 0 or 1, depending on how it's connected.

#define WIFI_GPIO 18
// txpower settings
#define HIGH_WIFI 31
#define LOW_WIFI 0

#define CHANNEL 0
// requires adding spidev.bufsiz=32768 to /boot/cmdline.txt
#define SPI_BUFSIZE 32768
#define SPI_HEADER 8
#define SYNC_CODE 0xe5
#define TEXTLEN 1024
#define SAMPLERATE 48000
#define SAMPLESIZE 3
// channels coming from the ARM
// aux is channels 0,1  mane is channels 2,3
#define CHANNELS 4
// channels in each I2S stream
#define I2S_CHANNELS 2
#define SPI_SAMPLES ((SPI_BUFSIZE - SPI_HEADER) / SAMPLESIZE / CHANNELS)
// the possible filenames
#define MAXFILES 100
// we record 2 files with 2 channels in each
// aux I2S is file 0, mane I2S is file 1
#define FILES 2
#define AUX_FILE 0
#define MANE_FILE 1
// bytes per file buffer
#define FILE_BUFSIZE (16 * SAMPLERATE * SAMPLESIZE * I2S_CHANNELS)
// maximum size the web server can serve
#define MAX_FILESIZE 0x100000

int monitor_volume0 = 0x0;
int monitor_volume1 = 0x0;
int monitor_mode = 0x0;
int wifi_power = HIGH_WIFI;

typedef struct
{
	uint8_t buffer[FILE_BUFSIZE];
// bytes in the buffer
	int size;
// position of the file thread
	int read_offset;
// position of the SPI thread
	int write_offset;
// total bytes written for this file
	int64_t total_written;
// writer thread waits for this
	sem_t write_lock;
// lock access to the offsets
	pthread_mutex_t mutex;


	FILE *fd;
	char filename[TEXTLEN];
	pthread_t tid;
} file_state_t;

file_state_t files[FILES];
char *filename_formats[] = 
{
	"aux%02d.wav",
	"mane%02d.wav"
};

// statistics for the web page
// bytes left on the disk
int64_t total_remane = 0;
int next_file = 0;
int need_recording = 0;
int need_stop = 0;
int recording = 0;
// mode set before the recording started
int recording_mode = 0;
// temporary for mixing down the recording samples
uint8_t mixdown_temp[SPI_BUFSIZE];

pthread_mutex_t www_mutex;
// write 1 buffer at a time
pthread_mutex_t file_mutex;
// maximum sample level
int max[CHANNELS];
int need_max = 1;

#define BYTERATE (SAMPLERATE * SAMPLESIZE)
unsigned char header[] = 
{
	'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0x7f,
	'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
	16, 0, 0, 0, 1, 0,
	// channels.  Offset 22
	0,
	0,
	// samplerate.  offset 24
	(SAMPLERATE & 0xff),
	((SAMPLERATE >> 8) & 0xff),
	((SAMPLERATE >> 16) & 0xff),
	((SAMPLERATE >> 24) & 0xff),
	// bytes per second.  offset 28
	(BYTERATE & 0xff),
	((BYTERATE >> 8) & 0xff),
	((BYTERATE >> 16) & 0xff),
	((BYTERATE >> 24) & 0xff),
	// blockalign.  offset 32
	SAMPLESIZE, 0,
	// bits per sample
	SAMPLESIZE * 8,
	0, 
	'd', 'a', 't', 'a', 0xff, 0xff, 0xff, 0x7f
};
#define HEADER_SIZE (sizeof(header))



#define PORT 80
#define TOTAL_CONNECTIONS 32
#define SOCKET_BUFSIZE 1024
#define TEXTLEN 1024
typedef struct 
{
	int is_busy;
	int fd;
	sem_t lock;
} webserver_connection_t;
webserver_connection_t* connections[TOTAL_CONNECTIONS];

int get_frame_size(int mode, int file_number);

void send_header(webserver_connection_t *connection, char *content_type)
{
	char header[TEXTLEN];
	sprintf(header, "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Server: Ultimate Vlogging Mic\r\n\r\n",
			content_type);
	write(connection->fd, header, strlen(header));
}

void send_string(webserver_connection_t *connection, char *text)
{
	write(connection->fd, text, strlen(text));
}

void send_file(webserver_connection_t *connection, char *getpath, char *mime)
{
	char string[TEXTLEN];

// the home page
	if(!strcmp(getpath, "/"))
	{
		sprintf(string, "index.html");
	}
	else
// strip the .
	{
		sprintf(string, "%s", getpath + 1);
	}

	FILE *fd = fopen(string, "r");
	if(fd)
	{
		fseek(fd, 0, SEEK_END);
		int size = ftell(fd);
		fseek(fd, 0, SEEK_SET);

		if(size > MAX_FILESIZE)
		{
			size = MAX_FILESIZE;
		}

// need to pad the buffer or fclose locks up
		unsigned char *buffer2 = (unsigned char*)malloc(size + 16);
		int result = fread(buffer2, size, 1, fd);
		fclose(fd);

		send_header(connection, mime);
		write(connection->fd, buffer2, size);
		free(buffer2);
	}

}

// return 1 if the key matched
int get_number(char **ptr, char *key, int *result)
{	
	int got_it = 0;
	if(strncasecmp((*ptr), key, strlen(key)) == 0)
	{
		got_it = 1;
		(*ptr) += strlen(key);
		if(**ptr == '=')
		{
			(*ptr)++;
			*result = atoi(*ptr);

printf("get_number %d: %s=%d\n", __LINE__, key, *result);
			while(**ptr != 0 && isdigit(**ptr))
			{
				(*ptr)++;
			}
		}
	}
	return got_it;
}

// return 1 if the key matched
int get_command(char **ptr, char *key)
{	
	int got_it = 0;
	if(strncasecmp((*ptr), key, strlen(key)) == 0)
	{
		got_it = 1;
		(*ptr) += strlen(key);
	}
	return got_it;
}

void web_server_connection(void *ptr)
{
	webserver_connection_t *connection = (webserver_connection_t*)ptr;
	unsigned char buffer[SOCKET_BUFSIZE];
	int i;
	
	while(1)
	{
		sem_wait(&connection->lock);
		
		int done = 0;
		while(!done)
		{
			buffer[0] = 0;
			int bytes_read = read(connection->fd, buffer, SOCKET_BUFSIZE);
			if(bytes_read <= 0)
			{
				break;
			}
			
//printf("web_server_connection %d:\n", __LINE__);
//printf("%s", buffer);

			char *ptr = buffer;
			char getpath[TEXTLEN];
			if(ptr[0] == 'G' &&
				ptr[1] == 'E' &&
				ptr[2] == 'T' &&
				ptr[3] == ' ')
			{
				ptr += 4;
				while(*ptr != 0 && *ptr == ' ')
				{
					ptr++;
				}
				
				if(*ptr != 0)
				{
					char *ptr2 = getpath;
					while(*ptr != 0 && *ptr != ' ')
					{
						*ptr2++ = *ptr++;
					}
					*ptr2 = 0;
				}
				
//				printf("web_server_connection %d: requested %s\n", __LINE__, getpath);

				if(!strncasecmp(getpath, "/meter", 6))
				{
//printf("web_server_connection %d: requested %s\n", __LINE__, getpath);
					char string[TEXTLEN];
					
					char *ptr = getpath + 6;
					while(*ptr != 0)
					{
						if(*ptr == '?')
						{
							ptr++;
							if(get_command(&ptr, "record"))
							{
								need_recording = 1;
							}
							else
							if(get_command(&ptr, "stop"))
							{
								need_stop = 1;
							}
							else
							if(get_number(&ptr, "volume0", &monitor_volume0))
							{
							}
							else
							if(get_number(&ptr, "volume1", &monitor_volume1))
							{
							}
							else
							if(get_number(&ptr, "mode", &monitor_mode))
							{
							}
							else
// unrecognized option
							{
								ptr++;
							}
						}
						else
// no ? character
						{
							ptr++;
						}
					}
					
					
					
// estimate remaneing samples based on the future mode
					int mode = recording_mode;
					if(!recording)
					{
						mode = monitor_mode;
					}

					pthread_mutex_lock(&www_mutex);
					int64_t bytes_written = 0;
					int total_frame_size = 0;
					for(i = 0; i < FILES; i++)
					{
						total_frame_size += get_frame_size(mode, i);
						bytes_written += files[i].total_written;
					}

					int64_t remaneing_samples = (total_remane - bytes_written) /
						total_frame_size;
					sprintf(string, "%d %d %d %d %d %lld %lld %s %s %d", 
						max[0], 
						max[1], 
						max[2], 
						max[3], 
						recording, 
						bytes_written / total_frame_size, 
						remaneing_samples, 
						files[0].filename,
						files[1].filename,
                        wifi_power);
					need_max = 1;
					pthread_mutex_unlock(&www_mutex);
//printf("web_server_connection %d: filename=%p\n", __LINE__, filename);
// printf("web_server_connection %d: max=%d %d %d %d\n", __LINE__, max[0], 
// max[1], 
// max[2], 
// max[3]);
					
					
					
					
					send_header(connection, "text/html");
					write(connection->fd, (unsigned char*)string, strlen(string));
					done = 1;
				}
				else
				if(!strcasecmp(getpath, "/favicon.ico"))
				{
					send_file(connection, getpath, "image/x-icon");
//					send_header(connection, "image/x-icon");
//					write(connection->fd, favicon, sizeof(favicon));
					done = 1;
				}
				else
				if(!strcasecmp(getpath, "/") ||
					!strcasecmp(getpath, "/index.html"))
				{
					send_file(connection, getpath, "text/html");
					done = 1;
				}
				else
				if(!strcasecmp(getpath, "/record.png") ||
					!strcasecmp(getpath, "/stop.png") ||
					!strcasecmp(getpath, "/single.gif"))
				{
					send_file(connection, getpath, "image/png");
					done = 1;
				}
				else
				{
					send_header(connection, "text/html");
					send_string(connection, "SHIT\n");
					done = 1;
				}
			}
		}
		
		if(!done)
		{
//			printf("web_server_connection %d: client closed\n", __LINE__);
		}
		else
		{
//			printf("web_server_connection %d: server closed\n", __LINE__);
		}
		close(connection->fd);
		connection->is_busy = 0;
		
	}
}

webserver_connection_t* new_connection()
{
	webserver_connection_t *result = calloc(1, sizeof(webserver_connection_t));
	sem_init(&result->lock, 0, 0);
	pthread_attr_t  attr;
	pthread_attr_init(&attr);
	pthread_t tid;
	pthread_create(&tid, 
		&attr, 
		(void*)web_server_connection, 
		result);
	return result;
}

void start_connection(webserver_connection_t *connection, int fd)
{
	connection->is_busy = 1;
	connection->fd = fd;
	sem_post(&connection->lock);
}


void web_server(void *ptr)
{
	int i;
	for(i = 0; i < TOTAL_CONNECTIONS; i++)
	{
		connections[i] = new_connection();
	}
	
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	
	int reuseon = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseon, sizeof(reuseon));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    int result = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	if(result)
	{
		printf("web_server %d: bind failed\n", __LINE__);
		return;
	}
	
	printf("web_server %d: started\n", __LINE__);
	while(1)
	{
		listen(fd, 256);
		struct sockaddr_in clientname;
		socklen_t size = sizeof(clientname);
		int connection_fd = accept(fd,
                			(struct sockaddr*)&clientname, 
							&size);
		int got_it = 0;
		for(i = 0; i < TOTAL_CONNECTIONS; i++)
		{
			if(!connections[i]->is_busy)
			{
				start_connection(connections[i], connection_fd);
				got_it = 1;
				break;
			}
		}
		
		if(!got_it)
		{
			printf("web_server %d: out of connections\n", __LINE__);
		}
	}
}


void flush_writer(file_state_t *file)
{
	pthread_mutex_lock(&file->mutex);
	int size = file->size;
	pthread_mutex_unlock(&file->mutex);


/*
 * printf("flush_writer %d filename=%s bytes=%d\n", 
 * __LINE__, 
 * file->filename, 
 * size);
 */

	int orig_size = size;
	while(size > 0)
	{
		int fragment = size;
		if(file->read_offset + fragment > FILE_BUFSIZE)
		{
			fragment = FILE_BUFSIZE - file->read_offset;
		}

		fwrite(file->buffer + file->read_offset, 
			fragment, 
			1, 
			file->fd);
		file->read_offset += fragment;
		size -= fragment;
		if(file->read_offset >= FILE_BUFSIZE)
		{
			file->read_offset = 0;
		}
	}

	pthread_mutex_lock(&file->mutex);
	file->size -= orig_size;
	pthread_mutex_unlock(&file->mutex);

	pthread_mutex_lock(&www_mutex);
	file->total_written += orig_size;
	pthread_mutex_unlock(&www_mutex);
}

void file_writer(void *ptr)
{
	file_state_t *file = (file_state_t*)ptr;
	while(1)
	{
// wait for more data
		sem_wait(&file->write_lock);

		flush_writer(file);


		if(!recording)
		{
			break;
		}
	}
}

// calculate remaneing space in samples
int64_t calculate_remane()
{
	FILE *fd = popen("df /", "r");
	char buffer[TEXTLEN];
	fgets(buffer, TEXTLEN, fd);
	fgets(buffer, TEXTLEN, fd);
	char *ptr = buffer;
	int i;
	for(i = 0; i < 3; i++)
	{
		while(*ptr != 0 && *ptr != ' ')
		{
			ptr++;
		}

		while(*ptr != 0 && *ptr == ' ')
		{
			ptr++;
		}
	}
	
	int64_t remane = atol(ptr);
	return remane * 1024;
}

// the frame size in each file
int get_file_channels(int mode, int file_number);
int get_frame_size(int mode, int file_number)
{
	return get_file_channels(mode, file_number) * SAMPLESIZE;
}

// the number of channels in each file
int get_file_channels(int mode, int file_number)
{
	switch(mode)
	{
// 2 files with 1 channel.  Each I2S must write its own file
		case MONITOR_2CH_DIFF:
			return 1;
			break;

// 2 files with 1 channel.  Each I2S must write its own file
		case MONITOR_2CH_AVG:
			return 1;
			break;
		
// 2 files with 2 channels
		case MONITOR_4CH:
			return 2;
			break;


		case MONITOR_3CH:
			if(file_number == MANE_FILE)
			{
// Mane I2S/I2S3
				return 1;
			}
			else
			{
// AUX I2S/I2S2
				return 2;
			}
			break;
		
		case MONITOR_1CH_DIFF:
			if(file_number == MANE_FILE)
			{
// Mane I2S/I2S3
				return 1;
			}
			else
			{
// AUX I2S/I2S2
				return 0;
			}
			break;
	}
}

// doesn't increment ptr
#define READ_INT24_OFFSET(ptr, offset) \
({ \
	int result = (ptr[offset + 0] | \
	(((uint32_t)ptr[offset + 1]) << 8) | \
	(((uint32_t)ptr[offset + 2]) << 16)); \
/* extend the sign bit */ \
	if(result & 0x800000) \
	{ \
		result |= 0xff000000; \
	} \
	result; \
})

// increments ptr
#define READ_INT24(ptr) \
({ \
	int result = (ptr[0] | \
	(((uint32_t)ptr[1]) << 8) | \
	(((uint32_t)ptr[2]) << 16)); \
/* extend the sign bit */ \
	if(result & 0x800000) \
	{ \
		result |= 0xff000000; \
	} \
	ptr += 3; \
	result; \
})

// increments ptr
#define WRITE_INT24(ptr, value) \
	*ptr++ = value & 0xff; \
	*ptr++ = (value >> 8) & 0xff; \
	*ptr++ = (value >> 16) & 0xff;



void set_wifi(int power)
{
	char string[TEXTLEN];
	sprintf(string, "iwconfig wlan0 txpower %d", power);
	system(string);
}

// change the txpower based on the gpio jumper
void wifi_poller(void *ptr)
{
	int prev_gpio = digitalRead(WIFI_GPIO);
	if(prev_gpio)
	{
		set_wifi(LOW_WIFI);
		wifi_power = LOW_WIFI;
	}
	else
	{
		set_wifi(HIGH_WIFI);
		wifi_power = HIGH_WIFI;
	}

	while(1)
	{
		sleep(1);
		int wifi_gpio = digitalRead(WIFI_GPIO);
		
		if(wifi_gpio && !prev_gpio)
		{
			set_wifi(LOW_WIFI);
			wifi_power = LOW_WIFI;
		}
		else
		if(!wifi_gpio && prev_gpio)
		{
			set_wifi(HIGH_WIFI);
			wifi_power = HIGH_WIFI;
		}
		
//		printf("wifi_poller %d: %d\n", __LINE__, digitalRead(WIFI_GPIO));
	
		prev_gpio = wifi_gpio;
	}
}



int main()
{
   	int fd, result, i, j, k;
	int channel0, channel1, output;
	int file_num;
   	unsigned char buffer[SPI_BUFSIZE];

	wiringPiSetupGpio();
	pinMode(WIFI_GPIO, INPUT);
	pullUpDnControl(WIFI_GPIO, PUD_DOWN);

	pthread_mutexattr_t attr2;
	pthread_mutexattr_init(&attr2);
	pthread_mutex_init(&www_mutex, &attr2);
	pthread_mutex_init(&file_mutex, &attr2);
	for(i = 0; i < CHANNELS; i++)
	{
		max[i] = 0;
	}
	need_max = 1;

	for(i = 0; i < FILES; i++)
	{
		bzero(&files[i], sizeof(file_state_t));
		pthread_mutex_init(&files[i].mutex, &attr2);
	}


// probe the last file written
	int64_t total_written = 0;
	for(next_file = 0; next_file < MAXFILES; next_file++)
	{
		int file_num;
		int got_one = 0;
		
		char next_filename[FILES][TEXTLEN];
		int64_t next_size[FILES];
		
		for(file_num = 0; file_num < FILES; file_num++)
		{
			sprintf(next_filename[file_num], filename_formats[file_num], next_file);
//			printf("main %d: testing %s\n", __LINE__, next_filename[file_num]);
			FILE *fd = fopen(next_filename[file_num], "r");

			if(fd)
			{
				got_one = 1;
				struct stat ostat;
// get the info for the browser
				fclose(fd);

//printf("main %d: got %s\n", __LINE__, next_filename[file_num]);
				stat(next_filename[file_num], &ostat);
				next_size[file_num] = ostat.st_size;
			}
			else
			{
				next_filename[file_num][0] = 0;
				next_size[file_num] = 0;
			}
		}

// the next filename has been found
		if(!got_one)
		{
			break;
		}
		else
		{
// could be the last files found
			for(file_num = 0; file_num < FILES; file_num++)
			{
				strcpy(files[file_num].filename, next_filename[file_num]);
				files[file_num].total_written = next_size[file_num];
				total_written += next_size[file_num];
			}
//printf("main %d: got %s %s\n", __LINE__, files[0].filename, files[1].filename);
		}
	}

printf("main %d: next file is #%d\n", __LINE__, next_file);

// add back the amount written in the last file, since it will be subtracted
// in the browser
	total_remane = calculate_remane() + total_written;
	

	
	pthread_attr_t  attr;
	pthread_attr_init(&attr);
	pthread_t tid;

	pthread_create(&tid, 
		&attr, 
		(void*)web_server, 
		0);


	pthread_create(&tid, 
		&attr, 
		(void*)wifi_poller, 
		0);

   	fd = wiringPiSPISetup(CHANNEL, 16000000);


// set the SPI thread to realtime
	struct sched_param params;
	params.sched_priority = 1;
	if(sched_setscheduler(0, SCHED_RR, &params))
	{
		perror("sched_setscheduler");
	}
	
	while(1)
	{
	  	memset(buffer, 0x00, SPI_BUFSIZE);
		buffer[0] = SYNC_CODE;
		buffer[1] = monitor_volume0;
		buffer[2] = monitor_volume1;
		buffer[3] = monitor_mode;
	
   		if(wiringPiSPIDataRW(CHANNEL, buffer, SPI_BUFSIZE) < 0)
		{
			printf("main %d: SPI failed.  Did you add spidev.bufsiz=32768 to /boot/cmdline.txt?\n",
				__LINE__);
		}

// printf("main %d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", 
// __LINE__, 
// result,
// buffer[0], 
// buffer[1], 
// buffer[2], 
// buffer[3], 
// buffer[4], 
// buffer[5], 
// buffer[6], 
// buffer[7], 
// buffer[8], 
// buffer[9], 
// buffer[10], 
// buffer[11], 
// buffer[12], 
// buffer[13], 
// buffer[14], 
// buffer[15]);

// samples to write to each file
		int samples[FILES];
		samples[AUX_FILE] = *(uint16_t*)(buffer + 4);
		samples[MANE_FILE] = *(uint16_t*)(buffer + 6);
		if(samples[AUX_FILE] >= SPI_SAMPLES || samples[MANE_FILE] >= SPI_SAMPLES)
		{
			printf("main %d: buffer overrun samples=%d,%d\n", 
				__LINE__, 
				samples[AUX_FILE],
				samples[MANE_FILE]);
			continue;
		}
		
// get the maximum level
		int max2[CHANNELS];
		for(i = 0; i < CHANNELS; i++)
		{
			max2[i] = 0;
		}
		for(j = 0; j < CHANNELS; j++)
		{
			int file_samples = samples[j / 2];
			unsigned char *ptr = buffer + SPI_HEADER + j * SAMPLESIZE;
			for(i = 0; i < file_samples; i++)
			{
				int value = READ_INT24_OFFSET(ptr, 0);

				int value2 = value;
				if(value < 0)
				{
					value2 = -value;
				}
				if(value2 > max2[j])
				{
					max2[j] = value2;
				}
				
				ptr += SAMPLESIZE * CHANNELS;
			}
		}
		
		pthread_mutex_lock(&www_mutex);
// reset the peaks
		if(need_max)
		{
			need_max = 0;
			for(i = 0; i < CHANNELS; i++)
			{
				max[i] = 0;
			}
		}

		int meter_num = 0;
		for(file_num = 0; file_num < FILES; file_num++)
		{
			int frame_size = get_frame_size(monitor_mode, file_num);
			if(frame_size)
			{
// samples which came off the I2S
				int file_samples = samples[file_num];
				uint8_t *src = buffer + 
					SPI_HEADER + 
					file_num * SAMPLESIZE * I2S_CHANNELS;


// the 2 common peak detectors
#define SINGLE_ENDED_PEAKS \
	for(i = 0; i < I2S_CHANNELS; i++) \
	{ \
		if(max2[meter_num + i] > max[meter_num + i]) \
		{ \
			max[meter_num + i] = max2[meter_num + i]; \
		} \
	} \
	meter_num += I2S_CHANNELS;

#define DIFFERENTIAL_PEAKS \
	for(i = 0; i < file_samples; i++) \
	{ \
		channel0 = READ_INT24(src); \
		channel1 = READ_INT24(src); \
		output = (channel0 - channel1) / 2; \
		if(output < 0) \
		{ \
			output = -output; \
		} \
		if(output > max[meter_num]) \
		{ \
			max[meter_num] = output; \
		} \
/* next I2S frame */ \
		src += SAMPLESIZE * I2S_CHANNELS; \
	} \
	meter_num++;


				switch(monitor_mode)
				{
					case MONITOR_4CH:
// all 4 channels use single ended peaks from the ARM
						SINGLE_ENDED_PEAKS
						break;
;

					case MONITOR_3CH:
// aux channels are single ended
						if(file_num == AUX_FILE)
						{
							SINGLE_ENDED_PEAKS
						}
						else
						{
// mane channels are differential
							DIFFERENTIAL_PEAKS
						}
						break;

// channels are averaged
					case MONITOR_2CH_AVG:
						for(i = 0; i < file_samples; i++)
						{
							channel0 = READ_INT24(src);
							channel1 = READ_INT24(src);
							output = (channel0 + channel1) / 2;
							if(output < 0)
							{
								output = -output;
							}
							if(output > max[meter_num])
							{
								max[meter_num] = output;
							}
// next I2S frame
							src += SAMPLESIZE * I2S_CHANNELS;
						}
						meter_num++;
						break;
					
// each I2S outputs a diff
						case MONITOR_2CH_DIFF:
						case MONITOR_1CH_DIFF:
							DIFFERENTIAL_PEAKS
							break;
					
				}

			}
		}


		pthread_mutex_unlock(&www_mutex);


// start recording
		if(need_recording)
		{
			need_recording = 0;
			if(!recording)
			{
				pthread_mutex_lock(&www_mutex);
				recording = 1;
				recording_mode = monitor_mode;

				total_remane = calculate_remane();
				for(i = 0; i < FILES; i++)
				{
					int frame_size = get_frame_size(recording_mode, i);
					file_state_t *file = &files[i];
					if(frame_size > 0)
					{
						sprintf(file->filename, filename_formats[i], next_file);
						file->fd = fopen(file->filename, "w");

						if(!file->fd)
						{
							printf("main %d: couldn't open file %s\n", __LINE__, file->filename);
						}
						else
						{
							printf("main %d: writing %s\n", __LINE__, file->filename);

// write the header
							header[22] = get_file_channels(recording_mode, i);
							int byterate = frame_size * SAMPLERATE;
							header[28] = byterate & 0xff;
							header[29] = (byterate >> 8) & 0xff;
							header[30] = (byterate >> 16) & 0xff;
							header[31] = (byterate >> 24) & 0xff;
							header[32] = frame_size;
							fwrite(header, sizeof(header), 1, file->fd);
						}

				
						file->size = 0;
						file->read_offset = 0;
						file->write_offset = 0;
						file->total_written = 0;
						sem_init(&file->write_lock, 0, 0);

						pthread_create(&file->tid, 
							&attr, 
							(void*)file_writer, 
							file);
					} // frame_size > 0
					else
// file not used
					{
						file->filename[0] = 0;
						file->total_written = 0;
					}
				}
				next_file++;
				
				pthread_mutex_unlock(&www_mutex);
			}
		}



		if(recording)
		{
// write the samples
			for(file_num = 0; file_num < FILES; file_num++)
			{
				int frame_size = get_frame_size(recording_mode, file_num);

// only write if the file is used
				if(frame_size)
				{
					file_state_t *file = &files[file_num];
// samples which came off the I2S
					int file_samples = samples[file_num];
// samples left in the file buffer
					pthread_mutex_lock(&file->mutex);
					int samples_remane = (FILE_BUFSIZE - file->size) / frame_size;
					pthread_mutex_unlock(&file->mutex);

					if(samples_remane < file_samples)
					{
						printf("main %d: file writer overrun\n", __LINE__);
						file_samples = samples_remane;
					}

// mix down
					uint8_t *dst = mixdown_temp;
// src is aligned on I2S frames
					uint8_t *src = buffer + 
						SPI_HEADER + 
						file_num * SAMPLESIZE * I2S_CHANNELS;

					for(i = 0; i < file_samples; i++)
					{
						switch(recording_mode)
						{
// each I2S passes through
							case MONITOR_4CH:
								*dst++ = *src++;
								*dst++ = *src++;
								*dst++ = *src++;
								*dst++ = *src++;
								*dst++ = *src++;
								*dst++ = *src++;
								break;

							case MONITOR_3CH:
// aux file/I2S2 passes through
								if(file_num == AUX_FILE)
								{
									*dst++ = *src++;
									*dst++ = *src++;
									*dst++ = *src++;
									*dst++ = *src++;
									*dst++ = *src++;
									*dst++ = *src++;
								}
								else
// mane file/I2S3 is a diff
								{
									channel0 = READ_INT24(src);
									channel1 = READ_INT24(src);
// difference
									output = (channel0 - channel1) / 2;
									WRITE_INT24(dst, output)
								}
								break;

// each I2S outputs an average
							case MONITOR_2CH_AVG:
								channel0 = READ_INT24(src);
								channel1 = READ_INT24(src);
// difference
								output = (channel0 + channel1) / 2;
								WRITE_INT24(dst, output)
								break;
						
// each I2S outputs a diff
							case MONITOR_2CH_DIFF:
							case MONITOR_1CH_DIFF:
								channel0 = READ_INT24(src);
								channel1 = READ_INT24(src);
// difference
								output = (channel0 - channel1) / 2;
// static int debug_counter = 0;
// if(debug_counter++ < 100)
// {
// printf("main %d: %x %x\n", __LINE__, channel0, channel1);
// }
								WRITE_INT24(dst, output)
								break;
						}

// next I2S frame
						src += SAMPLESIZE * I2S_CHANNELS;

					}

// write in fragments
					int bytes = file_samples * frame_size;
					for(i = 0; i < bytes; )
					{
						
						int fragment = bytes - i;
						
						
						if(fragment + file->write_offset > FILE_BUFSIZE)
						{
							fragment = FILE_BUFSIZE - file->write_offset;
						}


						memcpy(file->buffer + file->write_offset,
							mixdown_temp + i,
							fragment);


						i += fragment;
						file->write_offset += fragment;
						if(file->write_offset >= FILE_BUFSIZE)
						{
							file->write_offset = 0;
						}
					}
					
// release to the file writer
					pthread_mutex_lock(&file->mutex);
					file->size += bytes;
					pthread_mutex_unlock(&file->mutex);
					sem_post(&file->write_lock);
				}
			}
		}


// stop recording
		if(need_stop)
		{
			need_stop = 0;
			if(recording)
			{
				recording = 0;

				for(j = 0; j < FILES; j++)
				{
					int frame_size = get_frame_size(recording_mode, j);
					if(frame_size > 0)
					{
						file_state_t *file = &files[j];
						sem_post(&file->write_lock);
					
					
						pthread_join(file->tid, 0);

// flush anything after recording = 0
						flush_writer(file);

						fclose(file->fd);
						sem_destroy(&file->write_lock);

						printf("main %d: stopped %s\n", __LINE__, file->filename);
					}
				}
			}
		}

		
		
	}

}






