/* tinyplay.c
**
** Copyright 2011, The Android Open Source Project
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of The Android Open Source Project nor the names of
**       its contributors may be used to endorse or promote products derived
**       from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY The Android Open Source Project ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL The Android Open Source Project BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
** DAMAGE.
*/

#include <tinyalsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <semaphore.h>

static int close = 0;
struct j6 {
    struct pcm_config speakerConfig;
    struct pcm_config fmConfig;
    struct pcm *speaker ;	
    struct pcm *fm ;
    int *buffer[2];
    int *buffer1[2];
    int size;
	int proc_int;
	int play_int;
    pthread_t proc_thread;
    pthread_t play_thread;
	sem_t proc_sem;
	sem_t play_sem;
	int proc_id;
	int play_id;
};

void fm_sample(struct j6 *dev,  unsigned int period_size,unsigned int period_count);

static void *proc_func(void *arg)
{
    struct j6 *dev = (struct j6 *)arg;
    int ret = 0; 
	int i=0;
	int proc_id=0;
	int play_id=0;
	int len=0;
	int buffer1_len=0;
	int j = 0;
    int *buffer;
    int *buffer1;
	sem_t * sem=NULL;
	len = (dev->size)/4; 
	buffer1_len = (dev->size)*4;
	sem=&(dev->proc_sem);
	while(!close){
		sem_wait(sem);
		proc_id=dev->play_id;
	 	play_id=dev->play_id+1;
		play_id=play_id%2;
		for(i=0;i<len;i++)	{
			for(j=0;j<4;j++){
				dev->buffer1[play_id][i*4+j]=dev->buffer[proc_id][i];
			}
			dev->play_id=play_id;
			sem_post(&(dev->play_sem));
		}	
	}
    return (void*)ret;
}

static void *play_func(void *arg)
{
    struct j6 *dev = (struct j6 *)arg;
	int len=0;
	int buffer1_len=0;
    int ret = 0; 
    struct pcm *speaker ;	
    struct pcm *fm ;  
	int *buffer;
    int *buffer1;
	int play_id = 0;
	sem_t * sem=NULL;
	len = (dev->size)/4; 
	buffer1_len = (dev->size)*4;
	speaker= dev->speaker;
	sem=&(dev->play_sem);
	while(!close){
		sem_wait(sem);
		play_id=dev->play_id;
		
		fprintf(stderr,"play_func play_id:%d\n",play_id);
		pcm_write(speaker, dev->buffer1[play_id],buffer1_len);
	}
    return (void*)ret;
}

void stream_close(int sig)
{
    /* allow the stream to be closed gracefully */
    signal(sig, SIG_IGN);
    close = 1;
}

int main(int argc, char **argv)
{
    unsigned int period_size = 1024;
    unsigned int period_count = 4;
	struct j6 dev;
	int ret=0;
	sem_init(&(dev.play_sem), 0,0);
	sem_init(&(dev.proc_sem), 0,0);
    fm_sample(&dev, period_size, period_count);
    return 0;
}

int check_param(struct pcm_params *params, unsigned int param, unsigned int value,
                 char *param_name, char *param_unit)
{
    unsigned int min;
    unsigned int max;
    int is_within_bounds = 1;

    min = pcm_params_get_min(params, param);
    if (value < min) {
        fprintf(stderr, "%s is %u%s, device only supports >= %u%s\n", param_name, value,
                param_unit, min, param_unit);
        is_within_bounds = 0;
    }

    max = pcm_params_get_max(params, param);
    if (value > max) {
        fprintf(stderr, "%s is %u%s, device only supports <= %u%s\n", param_name, value,
                param_unit, max, param_unit);
        is_within_bounds = 0;
    }

    return is_within_bounds;
}

int sample_is_playable(unsigned int card, unsigned int device, unsigned int channels,
                        unsigned int rate, unsigned int bits, unsigned int period_size,
                        unsigned int period_count)
{
    struct pcm_params *params;
    int can_play;

    params = pcm_params_get(card, device, PCM_OUT);
    if (params == NULL) {
        fprintf(stderr, "Unable to open PCM device %u.\n", device);
        return 0;
    }

    can_play = check_param(params, PCM_PARAM_RATE, rate, "Sample rate", "Hz");
    can_play &= check_param(params, PCM_PARAM_CHANNELS, channels, "Sample", " channels");
    can_play &= check_param(params, PCM_PARAM_SAMPLE_BITS, bits, "Bitrate", " bits");
    can_play &= check_param(params, PCM_PARAM_PERIOD_SIZE, period_size, "Period size", "Hz");
    can_play &= check_param(params, PCM_PARAM_PERIODS, period_count, "Period count", "Hz");

    pcm_params_free(params);

    return can_play;
}

void fm_sample(struct j6 *dev, unsigned int period_size,
                 unsigned int period_count)
{
    int ret;
	int i;
	int j;
	int len;
	int buffer1_len;
	int k=0;
	int proc_id=0;
	sem_t * sem;
    memset(&dev->speakerConfig, 0, sizeof(dev->speakerConfig));
    memset(&dev->fmConfig, 0, sizeof(dev->fmConfig));
    dev->speakerConfig.channels = 8;
    dev->speakerConfig.rate = 44100;
    dev->speakerConfig.period_size = period_size;
    dev->speakerConfig.period_count = period_count;
   	dev->speakerConfig.format = PCM_FORMAT_S16_LE;
    dev->speakerConfig.start_threshold = period_size*period_count/2;
    dev->speakerConfig.stop_threshold = period_size*period_count;
    dev->speakerConfig.silence_threshold = 0;
	
    dev->fmConfig.channels = 2;
    dev->fmConfig.rate = 44100;
    dev->fmConfig.period_size = period_size;
    dev->fmConfig.period_count = period_count;
   	dev->fmConfig.format = PCM_FORMAT_S16_LE;
    dev->fmConfig.start_threshold = 1;
    dev->fmConfig.stop_threshold = period_size*period_count;
    dev->fmConfig.silence_threshold = 0;


    if (!sample_is_playable(0, 0, 8, 44100, 16, period_size, period_count)) {
        return;
    }
    dev->speaker = pcm_open(0, 0, PCM_OUT, &dev->speakerConfig);
    if (!dev->speaker || !pcm_is_ready(dev->speaker)) {
        fprintf(stderr, "Unable to open speaker device 0 (%s)\n",
                pcm_get_error(dev->speaker));
        return;
    }
    dev->fm = pcm_open(2, 0, PCM_IN, &dev->fmConfig);
    if (!dev->fm || !pcm_is_ready(dev->fm)) {
        fprintf(stderr, "Unable to open fm device (%s)\n",
                pcm_get_error(dev->fm));
        return ;
    }

    dev->size = pcm_frames_to_bytes(dev->speaker, pcm_get_buffer_size(dev->speaker));
	for(i=0;i<2;i++){
    	dev->buffer[i] = malloc(dev->size);
    	if (!dev->buffer[i]) {
       	 	fprintf(stderr, "Unable to allocate %d bytes\n", dev->size);
        	pcm_close(dev->speaker);
        	return;
    	}
	}
	
	for(i=0;i<2;i++){
    	dev->buffer1[i] = malloc(dev->size*4);
    	if (!dev->buffer1[i]) {
        	fprintf(stderr, "Unable to allocate %d bytes for buffer1\n", dev->size*4);
        	pcm_close(dev->speaker);
        	return;
    	}
	}
    printf("Playing sample: 8 ch, 44100 hz, 16 bit,dev->size:%d\n",dev->size);
	ret = pthread_create(&(dev->proc_thread), NULL, proc_func, dev);
	ret = pthread_create(&(dev->play_thread), NULL, play_func, dev);

    /* catch ctrl-c to shutdown cleanly */
    signal(SIGINT, stream_close);
	len = dev->size/4; 
	buffer1_len = dev->size*4;
	sem=&(dev->proc_sem);
    do {
		proc_id=dev->proc_id+1;
		proc_id=proc_id%2;
		fprintf(stderr,"read proc_id:%d\n",proc_id);
        pcm_read(dev->fm, dev->buffer[proc_id], dev->size);
		dev->proc_id=proc_id;
		sem_post(sem);
	
    } while (!close);
    free(dev->buffer1);
    free(dev->buffer);
    pcm_close(dev->fm);
	pcm_close(dev->speaker);
}

