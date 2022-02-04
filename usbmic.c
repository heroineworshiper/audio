/*
 * Ultimate vlogging mic.  1st version.
 *
 * Copyright (C) 2018-2022 Adam Williams <broadcast at earthling dot net>
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



// this runs on the pi, recording from a generalplus chip & serving web pages
// this version records & plays in software because
// the phones output can't monitor the mic input
// uses ALSA DMA calls to get low latency monitoring

// Connect a jumper from UART TX to the WIFI GPIO to lower the transmit
// power.  Necessary to reduce interference during recording.

// UART RX must be floating or it won't boot

// 5 pin connector:
// GND, SPEAKER, SPEAKER, MIC, MIC

// scp it to the pi
// gcc -O2 -o usbmic usbmic.c -lwiringPi -lasound -lpthread -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

// html files are in html/
// uses index.html & all the images


// the home router puts it on 10.0.2.100
// the pi access point puts it on 10.0.3.1

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <ctype.h>
#include <wiringPi.h>




#define TEXTLEN 1024
#define SAMPLERATE 48000
#define SAMPLESIZE 2
#define RECORD_CHANNELS 1
#define MAXFILES 99
#define MAX_FILESIZE 0x100000
// 30 minute buffer
#define FILE_BUFSIZE (30 * 60 * 48000 * SAMPLESIZE)
#define WIFI_GPIO 18
// txpower settings
#define HIGH_WIFI 31
#define LOW_WIFI 0
#define PAGE_SIZE 4096




#include <alsa/asoundlib.h>

// samples
#define FRAGMENT 256
// number of hardware buffers
#define TOTAL_FRAGMENTS 3

snd_pcm_t *dsp_out = 0;
snd_pcm_t *dsp_in = 0;

// Generalplus only takes stereo playback
#define PLAYBACK_CHANNELS 2
uint8_t *playback_buffer0;
uint8_t *playback_buffer1;
int current_playback_buffer1 = 0;
int current_playback_buffer2 = 0;
int playback_samples0 = 0;
int playback_samples1 = 0;
sem_t playback_lock1;
sem_t playback_lock2;
pthread_t playback_tid;






int wifi_power = HIGH_WIFI;

// values filled in by the web page
int monitor_volume = 0;
int mic_gain = 0x0;
// maximum volume amixer accepts.  The rest is added in software
#define MAX_VOLUME 30

uint8_t file_buffer[FILE_BUFSIZE];
// offsets in bytes
int file_buffer_used = 0;
int write_offset = 0;
int read_offset = 0;
sem_t write_sem;
pthread_mutex_t write_mutex;


int write_fd = -1;
// statistics for the web page in bytes
int64_t total_written = 0;
int64_t total_remane = 0;
// keepalive number
int64_t total_read = 0;
int need_recording = 0;
int need_stop = 0;
int recording = 0;
char filename[TEXTLEN];
pthread_t record_tid;
int next_file = 0;
uint8_t page_buffer_[PAGE_SIZE * 2];
uint8_t *page_buffer = 0;
int page_used = 0;





// maximum sample level
pthread_mutex_t www_mutex;
float max;
int need_max = 1;

#define BYTERATE (SAMPLERATE * SAMPLESIZE)
const unsigned char header[] = 
{
	'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0x7f,
	'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
	16, 0, 0, 0, 1, 0,
	// channels
	1,
	0,
	(SAMPLERATE & 0xff),
	((SAMPLERATE >> 8) & 0xff),
	((SAMPLERATE >> 16) & 0xff),
	((SAMPLERATE >> 24) & 0xff),
	// bytes per second
	(BYTERATE & 0xff),
	((BYTERATE >> 8) & 0xff),
	((BYTERATE >> 16) & 0xff),
	((BYTERATE >> 24) & 0xff),
	// blockalign
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
				
//printf("web_server_connection %d: requested %s\n", __LINE__, getpath);

// parse commands & get status
				if(!strncasecmp(getpath, "/meter", 6))
				{
//printf("web_server_connection %d: requested %s\n", __LINE__, getpath);
					char string[TEXTLEN];
					
					int need_mixer = 0;
					int need_monitor = 0;
					char *ptr = getpath + 6;
					while(*ptr != 0)
					{
						if(*ptr == '?')
						{
							ptr++;
							
							if(ptr[0] == 'r' &&
								ptr[1] == 'e' &&
								ptr[2] == 'c' &&
								ptr[3] == 'o' &&
								ptr[4] == 'r' &&
								ptr[5] == 'd')
							{
								need_recording = 1;
								sem_post(&write_sem);
								ptr += 6;
							}
							else
							if(ptr[0] == 's' &&
								ptr[1] == 't' &&
								ptr[2] == 'o' &&
								ptr[3] == 'p')
							{
								need_stop = 1;
								sem_post(&write_sem);
								ptr += 4;
							}
							else
							if(ptr[0] == 'v' &&
								ptr[1] == 'o' &&
								ptr[2] == 'l' &&
								ptr[3] == 'u' &&
								ptr[4] == 'm' &&
								ptr[5] == 'e')
							{
								ptr += 6;
								if(*ptr == '=')
								{
									ptr++;
									monitor_volume = atoi(ptr);
									need_monitor = 1;
									
printf("web_server_connection %d: monitor_volume=%d\n", __LINE__, monitor_volume);
									while(*ptr != 0 && isdigit(*ptr))
									{
										ptr++;
									}
								}
							}
							else
							if(ptr[0] == 'g' &&
								ptr[1] == 'a' &&
								ptr[2] == 'i' &&
								ptr[3] == 'n')
							{
								ptr += 4;
								if(*ptr == '=')
								{
									ptr++;
									mic_gain = atoi(ptr);
									need_mixer = 1;
									
printf("web_server_connection %d: mic_gain=%d\n", __LINE__, mic_gain);
									while(*ptr != 0 && isdigit(*ptr))
									{
										ptr++;
									}
								}
							}
							else
							{
								ptr++;
							}
						}
						else
						{
							ptr++;
						}
					}
					

// printf("web_server_connection %d: total_remane=%lld total_written=%lld\n", 
// __LINE__, 
// total_remane,
// total_written);

// must write seconds instead of samples because javascript can't handle 64 bits
					pthread_mutex_lock(&www_mutex);
					sprintf(string, "%d %d %lld %lld %d %s", 
						(int)(max * 32767), 
						recording, 
						total_written / SAMPLERATE / SAMPLESIZE, 
						(total_remane - total_written) / SAMPLERATE / SAMPLESIZE, 
						wifi_power,
						filename);
					need_max = 1;
					pthread_mutex_unlock(&www_mutex);
//printf("web_server_connection %d: filename=%p\n", __LINE__, filename);
					
					
					
					
					send_header(connection, "text/html");
					write(connection->fd, (unsigned char*)string, strlen(string));
					
					if(need_mixer)
					{
						need_mixer = 0;

// 'Mic' playback doesn't work for monitoring, so mute it
						sprintf(string, 
							"amixer -c 1 set 'Mic' playback mute 0 capture %d cap",
							mic_gain);
						system(string);
					}
					
					
					if(need_monitor)
					{
						need_monitor = 0;
// Using 'Speaker' for monitoring
						int hw_volume = monitor_volume;
						if(hw_volume > MAX_VOLUME)
						{
							hw_volume = MAX_VOLUME;
						}
						
						sprintf(string, 
							"amixer -c 1 set 'Speaker' unmute %d",
							monitor_volume);
						system(string);
					}
					
					done = 1;
				}
				else
				if(!strcasecmp(getpath, "/favicon.ico"))
				{
					send_file(connection, getpath, "image/x-icon");
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
	
	while(1)
	{
		listen(fd, 256);
		struct sockaddr_in clientname;
		socklen_t size = sizeof(clientname);
		int connection_fd = accept(fd,
                			(struct sockaddr*)&clientname, 
							&size);

//printf("web_server %d: accept\n", __LINE__);

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
//printf("calculate_remane %d remane=%lld\n", __LINE__, remane);
	return remane * 1024LL;
}


int total_flushed = 0;
void flush_writer()
{
	pthread_mutex_lock(&write_mutex);
	int buffer_used = file_buffer_used;
	pthread_mutex_unlock(&write_mutex);

	int i = 0;
	while(i < buffer_used)
	{
		int fragment = buffer_used - i;
		if(read_offset + fragment > FILE_BUFSIZE)
		{
			fragment = FILE_BUFSIZE - read_offset;
		}

        if(fragment > PAGE_SIZE - page_used)
        {
            fragment = PAGE_SIZE - page_used;
        }


        memcpy(page_buffer + page_used,
            file_buffer + read_offset,
            fragment);

		read_offset += fragment;
		if(read_offset >= FILE_BUFSIZE)
		{
			read_offset = 0;
		}
        page_used += fragment;
        if(page_used >= PAGE_SIZE)
        {

            write(write_fd,
                page_buffer,
                PAGE_SIZE);
            page_used = 0;
        }

		i += fragment;
	}

    total_flushed += i;
//     if(buffer_used > 0)
//     {
//         printf("flush_writer %d read_offset=%d total_flushed=%d write_offset=%d\n", 
//             __LINE__, 
//             read_offset,
//             total_flushed,
//             write_offset);
//     }

	pthread_mutex_lock(&write_mutex);
	file_buffer_used -= buffer_used;
	pthread_mutex_unlock(&write_mutex);

	pthread_mutex_lock(&www_mutex);
	total_written += buffer_used;
	pthread_mutex_unlock(&www_mutex);
}



snd_pcm_format_t bits_to_format(int bits)
{
	switch(bits)
	{
		case 8:
			return SND_PCM_FORMAT_S8;
			break;
		case 16:
			return SND_PCM_FORMAT_S16_LE;
			break;
		case 24:
			return SND_PCM_FORMAT_S24_LE;
			break;
		case 32:
			return SND_PCM_FORMAT_S32_LE;
			break;
	}
}

void set_params(snd_pcm_t *dsp, 
	int channels, 
	int bits,
	int samplerate,
// ALSA uses the term "period" as the buffer size.
	int period, 
// The number of buffers
	int nperiods)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	int err;

	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);
	err = snd_pcm_hw_params_any(dsp, hwparams);

	if (err < 0) 
	{
		printf("set_params: no PCM configurations available\n");
		return;
	}

	snd_pcm_hw_params_set_access(dsp, 
		hwparams,
		SND_PCM_ACCESS_MMAP_INTERLEAVED);

	snd_pcm_hw_params_set_format(dsp, 
		hwparams, 
		bits_to_format(bits));
	snd_pcm_hw_params_set_channels(dsp, 
		hwparams, 
		channels);
	snd_pcm_hw_params_set_rate_near(dsp, 
		hwparams, 
		(unsigned int*)&samplerate, 
		(int*)0);




    snd_pcm_hw_params_set_period_size(dsp, 
        hwparams,
        period,
        0);
    unsigned int nperiodsp = nperiods;
    snd_pcm_hw_params_set_periods_min(dsp, hwparams, &nperiodsp, NULL);
    snd_pcm_hw_params_set_periods_near(dsp, hwparams, &nperiodsp, NULL);
    snd_pcm_hw_params_set_buffer_size(dsp, 
        hwparams,
        nperiodsp * period);

// check the values
	snd_pcm_uframes_t real_buffer_size;
	snd_pcm_uframes_t real_period_size;
	snd_pcm_hw_params_get_buffer_size(hwparams, &real_buffer_size );
	snd_pcm_hw_params_get_period_size(hwparams, &real_period_size, 0);

	printf("set_params %d: got buffer=%d period=%d\n", __LINE__, real_buffer_size, real_period_size);

/* write the parameters to device */
	err = snd_pcm_hw_params(dsp, hwparams);
	if(err < 0)
	{
		printf("set_params: hw_params failed\n");
		return;
	}


/* get the current swparams */
	snd_pcm_sw_params_current(dsp, swparams);
/* start the transfer when the buffer is full */
	snd_pcm_sw_params_set_start_threshold(dsp, swparams, 0);
	snd_pcm_sw_params_set_stop_threshold(dsp, swparams, period * nperiods);
/* allow the transfer when at least a period can be processed */
	snd_pcm_sw_params_set_avail_min(dsp, swparams, period );
/* align all transfers to 1 sample */
//	snd_pcm_sw_params_set_xfer_align(dsp, swparams, 1);
/* write the parameters to the playback device */
	snd_pcm_sw_params(dsp, swparams);
}

int open_alsa()
{
// recording
	int result = snd_pcm_open(&dsp_in, 
		"hw:1", 
		SND_PCM_STREAM_CAPTURE, 
		SND_PCM_NONBLOCK);

	if(result < 0)
	{
        dsp_in = 0;
		printf("open_alsa %d: %s\n", __LINE__, snd_strerror(result));
		return 1;
	}

	set_params(dsp_in, 
		RECORD_CHANNELS,  // channels
		16, // bits
		SAMPLERATE,
		FRAGMENT, // samples per fragment
		TOTAL_FRAGMENTS);


// playback

	result = snd_pcm_open(&dsp_out, 
		"hw:1", 
		SND_PCM_STREAM_PLAYBACK, 
		SND_PCM_NONBLOCK);
	if(result < 0)
	{
		dsp_out = 0;
		printf("open_alsa %d: %s\n", __LINE__, snd_strerror(result));
		return 1;
	}

	set_params(dsp_out, 
		PLAYBACK_CHANNELS, 
		16, // bits
		SAMPLERATE,
		FRAGMENT, // samples per fragment
		TOTAL_FRAGMENTS);

// fill with 0
	const snd_pcm_channel_area_t *areas;
	snd_pcm_uframes_t offset, frames;
	snd_pcm_avail_update(dsp_out);
	snd_pcm_mmap_begin(dsp_out,
		&areas, 
		&offset, 
		&frames);
    if(!areas[0].addr)
    {
        printf("open_alsa %d failed to mmap offset=%d frames=%d\n", 
            __LINE__, 
            offset, 
            frames);
        return 1;
    }

// assume this points to the entire buffer & write the entire buffer
	bzero(areas[0].addr, FRAGMENT * TOTAL_FRAGMENTS * PLAYBACK_CHANNELS * 2);

	snd_pcm_mmap_commit(dsp_out, offset, FRAGMENT * TOTAL_FRAGMENTS);



	snd_pcm_start(dsp_in);
	snd_pcm_start(dsp_out);
	
    return result;
}


void close_alsa()
{
	if(dsp_in)
	{
		snd_pcm_drop(dsp_in);
		snd_pcm_drain(dsp_in);
		snd_pcm_close(dsp_in);
		dsp_in = 0;
	}

	if(dsp_out)
	{
		snd_pcm_close(dsp_out);
		dsp_out = 0;
	}
}






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

void file_writer(void *ptr)
{
	while(1)
	{
        sem_wait(&write_sem);

		if(recording)
		{
			flush_writer();
		}

// stop a recording
		if(need_stop)
		{
			need_stop = 0;
			
			if(recording)
			{
				close(write_fd);
				write_fd = -1;
				recording = 0;
			}
		}

// start a new recording
		if(need_recording)
		{
			need_recording = 0;

			pthread_mutex_lock(&www_mutex);
// create a new file
			sprintf(filename, "sound%02d.wav", next_file);
			next_file++;
			pthread_mutex_unlock(&www_mutex);

			
			write_fd = open64(filename, 
                O_WRONLY | O_CREAT | O_DIRECT,
                0777);

			if(write_fd < 0)
			{
				printf("file_writer %d: couldn't open file %s\n", __LINE__, filename);
			}
			else
			{
				printf("file_writer %d: writing %s\n", __LINE__, filename);

// write the header
				memcpy(page_buffer, header, sizeof(header));
                page_used = sizeof(header);
                flush_writer();

				pthread_mutex_lock(&www_mutex);
				total_remane = calculate_remane();
				recording = 1;
				total_written = 0;
				pthread_mutex_unlock(&www_mutex);

			}

		}
    }
}




int main()
{
	char string[TEXTLEN];
   	int result, i;

	wiringPiSetupGpio();
	pinMode(WIFI_GPIO, INPUT);
	pullUpDnControl(WIFI_GPIO, PUD_DOWN);


	pthread_mutexattr_t attr2;
	pthread_mutexattr_init(&attr2);
	pthread_mutex_init(&www_mutex, &attr2);
	pthread_mutex_init(&write_mutex, &attr2);
	sem_init(&write_sem, 0, 0);
	max = 0;
	need_max = 1;

    page_buffer = page_buffer_ + PAGE_SIZE - ((int)page_buffer_ % PAGE_SIZE);


// probe the last filename
	strcpy(filename, "");
	for(next_file = 0; next_file < MAXFILES; next_file++)
	{
		char next_filename[TEXTLEN];
		sprintf(next_filename, "sound%02d.wav", next_file);
		FILE *fd = fopen(next_filename, "r");
		if(!fd)
		{
			break;
		}
		else
		{
			struct stat ostat;
			stat(next_filename, &ostat);
			total_written = (ostat.st_size - HEADER_SIZE);
			strcpy(filename, next_filename);
			fclose(fd);
		}
	}


	total_remane = calculate_remane();
	

	
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



	pthread_create(&tid, 
		&attr, 
		(void*)file_writer, 
		0);



	struct sched_param params;
	params.sched_priority = 1;
	if(sched_setscheduler(0, SCHED_RR, &params))
		perror("sched_setscheduler");

    open_alsa();
    

    while(1)
	{
        if(!dsp_in || !dsp_out)
        {
            printf("main %d: reopening\n", __LINE__);
            sleep(1);
            close_alsa();
            open_alsa();
            continue;
        }

// wait for both playback & capture to free up a fragment
		int total_fds_in = snd_pcm_poll_descriptors_count(dsp_in);
		struct pollfd fds_in[total_fds_in];
		snd_pcm_poll_descriptors(dsp_in,
			fds_in,
			total_fds_in);
		poll(fds_in, total_fds_in, -1);


// copy the input to the output
		const snd_pcm_channel_area_t *areas_in;
		snd_pcm_uframes_t offset_in, frames_in;
		int avail_in = snd_pcm_avail_update(dsp_in);
		
		snd_pcm_mmap_begin(dsp_in,
			&areas_in, 
			&offset_in, 
			&frames_in);
// printf("main %d: avail_in=%d addr=%p first=%d step=%d offset_in=%d frames_in=%d\n", 
// __LINE__, 
// avail_in, 
// areas_in[0].addr, 
// areas_in[0].first, 
// areas_in[0].step, 
// offset_in, 
// frames_in);

		const snd_pcm_channel_area_t *areas_out;
		snd_pcm_uframes_t offset_out, frames_out;
		int avail_out = snd_pcm_avail_update(dsp_out);
		snd_pcm_mmap_begin(dsp_out,
			&areas_out, 
			&offset_out, 
			&frames_out);
// printf("main %d: avail_out=%d addr=%p first=%d step=%d offset_out=%d frames_out=%d\n", 
// __LINE__, 
// avail_out, 
// areas_out[0].addr, 
// areas_out[0].first, 
// areas_out[0].step, 
// offset_out, 
// frames_out);



        if(avail_in < 0 || avail_out < 0)
        {
            printf("main %d: glitch\n", __LINE__);
            close_alsa();
            open_alsa();
            continue;
        }

		int16_t *in_ptr = (int16_t*)((uint8_t*)areas_in[0].addr + 
            offset_in * RECORD_CHANNELS * SAMPLESIZE);
		int16_t *out_ptr = (int16_t*)((uint8_t*)areas_out[0].addr + 
            offset_out * PLAYBACK_CHANNELS * SAMPLESIZE);
// get level for VU meter
	    int max2 = 0;
// apply software gain to the monitor volume
	    int volume;
	    if(monitor_volume > MAX_VOLUME)
	    {
		    volume = monitor_volume;
	    }
        else
        {
            volume = MAX_VOLUME;
        }

		for(i = 0; i < avail_in; i++)
		{
// mono to stereo
            int value = (int)*in_ptr * volume / MAX_VOLUME;
            if(value > 32767)
            {
                value = 32767;
            }
            else
            if(value < -32768)
            {
                value = -32768;
            }

			*out_ptr++ = value;
			*out_ptr++ = value;

            int abs_value = *in_ptr;
            if(abs_value < 0)
            {
                abs_value = -abs_value;
            }
            if(abs_value > max2)
            {
                max2 = abs_value;
            }



			in_ptr++;
		}

// put in recording buffer
	    if(recording)
	    {
		    if(file_buffer_used + avail_in * RECORD_CHANNELS * SAMPLESIZE < 
                FILE_BUFSIZE)
		    {
		        uint8_t *in_ptr = (uint8_t*)areas_in[0].addr + 
                    offset_in * RECORD_CHANNELS * SAMPLESIZE;
			    for(i = 0; i < avail_in * RECORD_CHANNELS * SAMPLESIZE; i++)
			    {
				    file_buffer[write_offset++] = *in_ptr++;
				    if(write_offset >= FILE_BUFSIZE)
				    {
					    write_offset = 0;
				    }
			    }

// update the counter
			    pthread_mutex_lock(&write_mutex);
			    file_buffer_used += avail_in * RECORD_CHANNELS * SAMPLESIZE;
			    pthread_mutex_unlock(&write_mutex);
// release the thread
			    sem_post(&write_sem);
		    }
		    else
		    {
			    printf("main %d: recording overrun\n", __LINE__);
		    }
	    }

		snd_pcm_mmap_commit(dsp_in, offset_in, avail_in);
		snd_pcm_mmap_commit(dsp_out, offset_out, avail_out);



	    pthread_mutex_lock(&www_mutex);
	    if(need_max)
	    {
		    need_max = 0;
		    max = 0;
	    }

	    if(max2 > max)
	    {
		    max = (float)max2 / 32768;
	    }
	    pthread_mutex_unlock(&www_mutex);
    }

    



}






