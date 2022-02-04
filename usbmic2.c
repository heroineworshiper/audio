/*
 * Ultimate vlogging mic.  2nd version
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



// This runs on the pi, recording from 2 generalplus chips & serving web pages
// This version doesn't monitor, but records 2 channels using 2 chips.

// Using snd_pcm_readi

// Connect a jumper from UART TX to the WIFI GPIO to lower the transmit
// power.  Necessary to reduce interference during recording.

// UART RX must be floating or it won't boot

// scp it to the pi
// scp usbmic2.c 10.0.2.100:
// scp html/usbmic2.html 10.0.2.100:
// gcc -O2 -o usbmic2 usbmic2.c -lwiringPi -lasound -lpthread -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

// html/usbmic2.html must be copied to /root/
// asoundrc.usbmic2 must be copied to /root/.asoundrc
// audio.usbmic2 must be copied to /root/audio.usbmic2


// the home router puts it on 10.0.2.100
// the pi access point puts it on 10.0.3.1

#include <alsa/asoundlib.h>
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
// channels per sound device
#define RECORD_CHANNELS 2
// channels per file
#define FILE_CHANNELS 2
#define MAXFILES 99
#define MAX_FILESIZE 0x100000
// 15 minute buffer
#define FILE_BUFSIZE (15 * 60 * 48000 * SAMPLESIZE * RECORD_CHANNELS)
#define FILE_SAMPLES (FILE_BUFSIZE / SAMPLESIZE)
#define WIFI_GPIO 18

// these values came from https://wiki.linuxaudio.org/wiki/raspberrypi
// hardware buffer samples to request
#define FRAGMENT 1024
// number of hardware buffers
#define TOTAL_FRAGMENTS 4

// txpower settings
#define HIGH_WIFI 31
#define LOW_WIFI 0

int wifi_power = HIGH_WIFI;

// values filled in by the web page
int monitor_volume = 0;
int mic_gain = 0x0;
// maximum volume amixer accepts.  The rest is added in software
#define MAX_VOLUME 30


// the file writer
FILE *fd;
// current file being recorded
char filename[TEXTLEN];

snd_pcm_t *dsp_in = 0;
snd_pcm_uframes_t alsa_buffer_size;
snd_pcm_uframes_t alsa_period_size;
unsigned char *record_buffer;



int16_t file_buffer[FILE_SAMPLES * FILE_CHANNELS]; 
// frames used
int file_buffer_used;  
// position of the alsa thread in samples
int write_offset;
// position of the file writer in samples
int read_offset;
// writer thread waits for this
sem_t write_sem;
// lock access to the offsets
pthread_mutex_t write_mutex;

// statistics for the web page
// frames
int64_t total_written = 0;
int64_t total_remane = 0;
// keepalive number
int64_t total_read = 0;
int need_recording = 0;
int need_stop = 0;
int recording = 0;
pthread_t record_tid;
int next_file = 0;




pthread_mutex_t www_mutex;
// maximum sample levels
int lmax;
int rmax;
int need_max;

#define BYTERATE (SAMPLERATE * SAMPLESIZE)
const unsigned char header[] = 
{
	'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0x7f,
	'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
	16, 0, 0, 0, 1, 0,
	// channels
	FILE_CHANNELS,
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

void write_output(unsigned char *data);

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
		sprintf(string, "usubmic2.html");
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
					
					
					
					pthread_mutex_lock(&www_mutex);
					sprintf(string, "%d %d %d %lld %lld %d %s", 
						lmax, 
						rmax, 
						recording, 
						total_written, 
						total_remane - total_written, 
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

						sprintf(string, 
							"amixer -c 2 set 'Mic' playback mute 0 capture %d cap",
							mic_gain);
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
                    !strcasecmp(getpath, "/index.html") ||
					!strcasecmp(getpath, "/usbmic2.html"))
				{
					send_file(connection, "/usbmic2.html", "text/html");
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





// calculate remaneing space in frames
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
	return remane * 1024 / SAMPLESIZE / FILE_CHANNELS;
}





void flush_writer()
{
	pthread_mutex_lock(&write_mutex);
// number of frames
	int buffer_used = file_buffer_used;
	pthread_mutex_unlock(&write_mutex);
	
	int i = 0;
	while(i < buffer_used * FILE_CHANNELS)
	{
		int fragment = buffer_used * FILE_CHANNELS - i;
		if(read_offset + fragment > FILE_SAMPLES * FILE_CHANNELS)
		{
			fragment = FILE_SAMPLES * FILE_CHANNELS - read_offset;
		}
		
		fwrite(file_buffer + read_offset, 
            fragment * SAMPLESIZE, 
            1, 
            fd);
		
		read_offset += fragment;
		if(read_offset >= FILE_SAMPLES * FILE_CHANNELS)
		{
			read_offset = 0;
		}
		i += fragment;
	}

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
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *swparams;
	int err;

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);
	err = snd_pcm_hw_params_any(dsp, params);

	if (err < 0) 
	{
		printf("set_params: no PCM configurations available\n");
		return;
	}

	snd_pcm_hw_params_set_access(dsp, 
		params,
		SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(dsp, 
		params, 
		bits_to_format(bits));
	snd_pcm_hw_params_set_channels(dsp, 
		params, 
		channels);
	snd_pcm_hw_params_set_rate_near(dsp, 
		params, 
		(unsigned int*)&samplerate, 
		(int*)0);

// get times in microseconds
	int buffer_time;
	int period_time;
// the total time of all periods
	buffer_time = 1000000 * (int64_t)period * nperiods / samplerate;
	snd_pcm_hw_params_set_buffer_time_near(dsp, 
		params,
		(unsigned int*)&buffer_time, 
		(int*)0);
// the time of a single period
	period_time = 1000000 * (int64_t)period / samplerate;
	snd_pcm_hw_params_set_period_time_near(dsp, 
		params,
		(unsigned int*)&period_time, 
		(int*)0);

// check the values
	snd_pcm_hw_params_get_buffer_size(params, &alsa_buffer_size );
	snd_pcm_hw_params_get_period_size(params, &alsa_period_size, 0);

	printf("set_params %d: got buffer=%d period=%d\n", 
        __LINE__, 
        alsa_buffer_size, 
        alsa_period_size);

/* write the parameters to device */
	err = snd_pcm_hw_params(dsp, params);
	if(err < 0)
	{
		printf("set_params: hw_params failed\n");
		return;
	}


/* get the current swparams */
	snd_pcm_sw_params_current(dsp, swparams);
/* start the transfer when the buffer is full */
	snd_pcm_sw_params_set_start_threshold(dsp, swparams, period);
	snd_pcm_sw_params_set_stop_threshold(dsp, swparams, -1 );
/* allow the transfer when at least a period can be processed */
	snd_pcm_sw_params_set_avail_min(dsp, swparams, period );
/* align all transfers to 1 sample */
	snd_pcm_sw_params_set_xfer_align(dsp, swparams, 1);
/* write the parameters to the playback device */
	snd_pcm_sw_params(dsp, swparams);
}

int open_input()
{
	int result = snd_pcm_open(&dsp_in, 
		"merge", 
		SND_PCM_STREAM_CAPTURE, 
		0);

	if(result < 0)
	{
		printf("open_input %d: %s\n", __LINE__, snd_strerror(result));
		return 1;
	}

	set_params(dsp_in, 
		RECORD_CHANNELS,  // channels
		16, // bits
		SAMPLERATE,
		FRAGMENT, // samples per fragment
		TOTAL_FRAGMENTS);
    
	record_buffer = calloc(1, alsa_buffer_size * SAMPLESIZE * RECORD_CHANNELS);

	snd_pcm_start(dsp_in);
    
    return result;
}


void close_input()
{
	if(dsp_in)
	{
		snd_pcm_drop(dsp_in);
		snd_pcm_drain(dsp_in);
		snd_pcm_close(dsp_in);
        free(record_buffer);
		dsp_in = 0;
        record_buffer = 0;
	}
}


void alsa_reader(void *ptr)
{
	struct sched_param params;
	params.sched_priority = 1;
	if(sched_setscheduler(0, SCHED_RR, &params))
		perror("sched_setscheduler");

    while(1)
    {
        int result = 0;


// wait for capture to free up a fragment
		if((result = snd_pcm_readi(dsp_in, 
			record_buffer, 
			FRAGMENT)) < 0)
		{
			printf("read_input %d: %s\n", __LINE__, snd_strerror(result));
			close_input();
			open_input();
            continue;
		}



        int16_t *in_ptr = (int16_t*)record_buffer;
        int lmax2 = 0;
        int rmax2 = 0;

// get the maximum levels
        int i;
        for(i = 0; i < FRAGMENT; i++)
        {
            int value = *in_ptr++;
            if(value < 0)
            {
                value = -value;
            }
            if(value > lmax2)
            {
                lmax2 = value;
            }

            value = *in_ptr++;
            if(value < 0)
            {
                value = -value;
            }
            if(value > rmax2)
            {
                rmax2 = value;
            }
        }
//printf("alsa_reader %d lmax2=%d rmax2=%d\n", __LINE__, lmax2, rmax2);


    // put in recording buffer
	    if(recording)
	    {
		    if(file_buffer_used + FRAGMENT < FILE_SAMPLES)
		    {
                in_ptr = (int16_t*)record_buffer;

			    for(i = 0; i < FRAGMENT; i++)
			    {
				    file_buffer[write_offset++] = *in_ptr++;
				    file_buffer[write_offset++] = *in_ptr++;
				    if(write_offset >= FILE_SAMPLES * FILE_CHANNELS)
				    {
					    write_offset = 0;
				    }
			    }

    // update the counter
			    pthread_mutex_lock(&www_mutex);
			    file_buffer_used += FRAGMENT;
			    pthread_mutex_unlock(&www_mutex);


    // release the file writer
			    sem_post(&write_sem);
		    }
		    else
		    {
			    printf("alsa_reader %d: recording overrun\n", __LINE__);
		    }
	    }


	    pthread_mutex_lock(&www_mutex);
	    if(need_max)
	    {
		    need_max = 0;
		    lmax = 0;
		    rmax = 0;
	    }

	    if(lmax2 > lmax)
	    {
		    lmax = lmax2;
	    }
	    if(rmax2 > rmax)
	    {
		    rmax = rmax2;
	    }
	    pthread_mutex_unlock(&www_mutex);
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



int main()
{
	char string[TEXTLEN];
   	int result, i;

    printf("Welcome to the ultimate vlogging mic\n");

	wiringPiSetupGpio();
	pinMode(WIFI_GPIO, INPUT);
	pullUpDnControl(WIFI_GPIO, PUD_OFF);


	pthread_mutexattr_t attr2;
	pthread_mutexattr_init(&attr2);
	pthread_mutex_init(&www_mutex, &attr2);
	pthread_mutex_init(&write_mutex, &attr2);
	sem_init(&write_sem, 0, 0);
    need_max = 1;
	lmax = 0;
	rmax = 0;



// probe the last filename
	strcpy(filename, "");
	for(next_file = 0; next_file < MAXFILES; next_file++)
	{
		char next_filename[TEXTLEN];
		sprintf(next_filename, "sound%02d.wav", next_file);


		FILE *fd = fopen(next_filename, "r");
		if(fd)
		{
// get the length of the last file
			struct stat ostat;
			stat(next_filename, &ostat);
			total_written = (ostat.st_size - HEADER_SIZE) / 
                SAMPLESIZE / 
                FILE_CHANNELS;
			strcpy(filename, next_filename);
			fclose(fd);
		}
        else
        {
            break;
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


	open_input();

	pthread_create(&tid, 
		&attr, 
		(void*)alsa_reader, 
		0);




   	while (1)
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
				fclose(fd);
				fd = 0;

				recording = 0;
			}
		}

// start a new recording
		if(need_recording)
		{
			need_recording = 0;

			pthread_mutex_lock(&www_mutex);
// create new files
			sprintf(filename, "sound%02d.wav", next_file);
			next_file++;
			pthread_mutex_unlock(&www_mutex);

	        fd = fopen(filename, "w");

	        if(!fd)
	        {
		        printf("main %d: couldn't open output file %s\n",
                    __LINE__, 
                    filename);
	        }
	        else
	        {
		        printf("main %d: writing %s\n", 
                    __LINE__, 
                    filename);

        // write the header
		        fwrite(header, sizeof(header), 1, fd);

				pthread_mutex_lock(&www_mutex);
				total_remane = calculate_remane();
				recording = 1;
				total_written = 0;
				pthread_mutex_unlock(&www_mutex);

			}

		}
    }


}






