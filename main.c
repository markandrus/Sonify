// main.c
/* Copyright (C) 2010 Mark Roberts
 * Code derived from the JACK example-client metro.c, Copyright (C) 2002 Anthony Van Groningen 
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */
// Thank you, Anthony Van Groningen and the JACK dev-team!
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
// Jack & Aubio
#include <jack/jack.h>
#include <aubio/aubio.h>
// SDL Includes
#include <SDL_image.h>
#include <SDL.h>
#include "resize.h"
// 
#include "math_util.h"
#include "color_util.h"

// Global Vars 
int image_tones_size, image_tones_index = 0, framecount;
long offset = 0;
float hopsize, max_amp = 0;
// ???: What if amp were a double?
float * image_tones, * image_tones_amp;
int X = 0, Y = 0;

const double PI = 3.14;
enum TYPE { Sine = 0, Square, Triangle, Sawtooth };

// Aubio Global Vars
aubio_pitchdetection_t * aubio;
fvec_t * aubio_fvec;

// User-Provided Vars
int pitch_scale, lower_bounds;
float ms_time;
enum TYPE waveform_type = Sine;

// SDL Surfaces
SDL_Surface * source_image, * dest_image, * dub_image;

// Jack
typedef jack_default_audio_sample_t sample_t;
jack_port_t *output_port;
jack_port_t *input_port;

// Waveform Synthesis Vars
sample_t * cycle, * temp_cycle;
jack_nframes_t sample_rate, samples_per_cycle, temp_samples_per_cycle;

// Aubio | Init pitch detection & aubio_fvec
// ???: What is `hopsize` exactly?
//      What about `aubio_pitch_fcomb`?
//      Bufsize?
void init_aubio(jack_nframes_t sr) {
	hopsize = sr * 0.001 * ms_time;
	float bufsize = sizeof(sample_t) * hopsize;
	aubio = new_aubio_pitchdetection(bufsize, hopsize, 1, sr, aubio_pitch_fcomb, aubio_pitchm_freq);
	aubio_fvec = new_fvec(hopsize, 1);
}

// Jack | Sample rate callback
int srate(jack_nframes_t nframes, void * arg) {
	printf("the sample rate is now %lu/sec\n", (long) nframes);
	sample_rate = nframes;
	return 0;
}

// SDL | Write RGB val to surface at current (X,Y)
void write_to_image(SDL_Surface *image, float R, float G, float B) {
	int w = image->w;
	int h = image->h;
	if (X == w) {
		X = 0;
		Y++;
	}
	// ???: `h * 4` instead of `h`
	if (Y == h * 4) {
		Y = 0;
	}
	// TODO: Uint32_t support?
	//       After all, our surfaces are 32-bit
	uint8_t color = SDL_MapRGB(image->format, (uint8_t) (R * 255.0), (uint8_t) (G * 255.0), (uint8_t) (B * 255.0));
	uint8_t * pixels = (uint8_t *) image->pixels;
	pixels[(Y * w) + X] = color;
	X++;
}

void swap_cycle();

// Build a waveform of type `type` with frequency `t` and amplitude `a` in `temp_cycle`, 
// Then call swap_cycle() to replace `cycle` with `temp_cycle`. 
void build_tone(float t, float a, enum TYPE type) {
	temp_samples_per_cycle = (sample_rate / t);
	sample_t scale = 2 * PI / temp_samples_per_cycle;
	temp_cycle = (sample_t *) malloc(temp_samples_per_cycle * sizeof(sample_t));
	
	if (temp_cycle == NULL) {
		fprintf(stderr,"Memory allocation failed.\n");
		exit(3);
	}
	
	int i;
	for (i = 0; i < temp_samples_per_cycle; i++) {
		switch(type) {
			case Sine:
				temp_cycle[i] = a * sin(i * scale);
				break;
			case Square:
				temp_cycle[i] = a * sgn(sin(i * scale));
				break;
			case Triangle:
				temp_cycle[i] = a * (asin(sin(i * scale)) / (PI / 2));
				break;
			case Sawtooth:
				temp_cycle[i] = a * (2 * ((((float) i) / temp_samples_per_cycle) - floor(((float) i) / temp_samples_per_cycle)) - 1);
				break;
		}
	}
	swap_cycle();
}

// Swap `cycle` for what we've built in `temp_cycle`
void swap_cycle() {
	if (cycle != NULL) {
		sample_t * trash = cycle;
		cycle = temp_cycle;
		free(trash);
	} else {
		cycle = temp_cycle;
	}
	temp_cycle = NULL;
	samples_per_cycle = temp_samples_per_cycle;
	// Reset our offset to the beginnig of our new cycle
	// TODO: What does `offset = offest % samples_per_cycle`
	//       sound like? Would this afford a smoother
	//       transition for cycles of similar frequency?
	offset = 0;
}

// Jack | Process Callback
int process(jack_nframes_t nframes, void *arg) {
	sample_t * in = (sample_t *) jack_port_get_buffer(input_port, nframes);
	sample_t * out = (sample_t *) jack_port_get_buffer(output_port, nframes);
	if (aubio_fvec == NULL || aubio_fvec->data == NULL) {
		init_aubio(sample_rate);
	}
	// 
	jack_nframes_t i;
	for (i = 0; i < nframes; i++) {
		// ???: What is the significance of `framecount == hopsize`? 
		if (framecount == hopsize) {
			image_tones_index++;
			if (image_tones_index >= image_tones_size) {
				image_tones_index = 0;
			}
			// Build waveform for the next pixel of our original image
			// TODO: Consider a "feedback" mode.
			build_tone(image_tones[image_tones_index], 1 - image_tones_amp[image_tones_index], waveform_type);
			// Write_to_image() according to analyzed samples 
			// TODO: How often are the vars below redeclared?
			float H, S, L, R, G, B;
			Sound2Hsl(&H, &S, &L, aubio_pitchdetection(aubio, aubio_fvec), max_amp, pitch_scale, lower_bounds);
			Hsl2Rgb(&R, &G, &B, H, S, 1 - L);
			write_to_image(dest_image, R, G, B);
			framecount = 0;
			max_amp = 0;
		}
		out[i] = cycle[offset];
		// !!!:
		aubio_fvec->data[0][framecount] = (smpl_t) in[i];
		if (fabs(in[i]) > max_amp) {
			max_amp = fabs(in[i]);
		}
		offset++;
		if (offset == samples_per_cycle) {
			offset = 0;
		}
		framecount++;
	}
	return 0;      
}

// Call this upon loading our image. Build arrays containing frequency and
// amplitude values calculated from the hue and luminance components of
// each pixel in our image, respectively.
// TODO: Right now it doesn't make sense to call build_tone() in process()
//       unless our sample rate changes (I'm not even sure if Jack allows
//       for this). Unless we implement a "feedback mode" where `image_tones`
//       is overwritten with incoming pixel data, we could instead generate
//       an array of cycles `image_cycles` where:
//       	image_cycles[c] = build_tone(H * pitch_scale + lower_bounds, L, waveform_type);
//	 or whatever the correct code would be...
void generate_tone_array(SDL_Surface *image) {
	int w, h, x, y, p, c = 0;
	w = image->w;
	h = image->h * 4;
	image_tones_size = w * h;
	image_tones = (float *) malloc(w * h * sizeof(float));
	image_tones_amp = (float *) malloc(w * h * sizeof(float));
	if (image_tones == NULL || image_tones_amp == NULL) {
		fprintf(stderr,"memory allocation failed\n");
		exit(3);
	}
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			color rgb = get_color(image, x, y);
			float H, S, L;
			Rgb2Hsl(&H, &S, &L, (float) (rgb.r / 255.0), (float) (rgb.g / 255.0), (float) (rgb.b / 255.0));
			image_tones[c] = H * pitch_scale + lower_bounds; // Hue = Frequency
			image_tones_amp[c] = L; // Luminosity = Amplitude
			c++;
		}
	}
}

// Handle our-user provided vars
void init_vars(char * argv[], char file_name[], int * window_scale) {
	strcpy(file_name, argv[2]);
	pitch_scale = atoi(argv[3]);
	lower_bounds = atoi(argv[4]);
	ms_time = atoi(argv[5]);
	if (strcmp(argv[6], "sin")==0) { waveform_type = 0; }
	else if (strcmp(argv[6], "sq")==0) { waveform_type = 1; }
	else if (strcmp(argv[6], "tri")==0) { waveform_type = 2; }
	else { waveform_type = 3; }
	*window_scale = atoi(argv[7]);
}

// Main
int main(int argc, char * argv[]) {
	// User supplied correct vals? Otherwise show usage...
	// TODO: Allow a minimum of <image path> to be provided and default
	// 	 the rest.
	if (argc < 8) {
		fprintf(stderr, "usage: sonify <client name> <image path> <freq scale> <lowest freq> <sin | sq | tri | saw> <window scale>\ni.e. sonify sfy img.png 10000 1000 1 sin\n");
		exit(1);
	}
	jack_client_t * client;
	const char ** ports;
	char file_name[100];
	int window_scale;
	init_vars(argv, file_name, &window_scale);

	// Init Jack Client
	if ((client = jack_client_open(argv[1], JackNullOption, NULL)) == 0) {
		fprintf (stderr, "Jack server not running?\n");
		return 1;
	}
	jack_set_process_callback(client, process, 0);
	jack_set_sample_rate_callback(client, srate, 0);
	input_port = jack_port_register(client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	output_port = jack_port_register(client, "output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	// Get Sample Rate & Init Aubio
	sample_rate = jack_get_sample_rate(client);
	init_aubio(sample_rate);
	
	// Init SDL Surfaces
	source_image = IMG_Load(file_name);
	if (source_image == NULL) {
		fprintf(stderr, "Load failes: %s\n", IMG_GetError());
		exit(1);
	}
	//   Generate Tones From Pixels
	generate_tone_array(source_image);
	dest_image = SDL_CreateRGBSurface (SDL_SWSURFACE, source_image->w, source_image->h, 32, 0, 0, 0, 0);
	if(dest_image == NULL) {
		fprintf(stderr, "CreateRGBSurface failed: %s\n", SDL_GetError());
		exit(1);
    	}
	SDL_FreeSurface(source_image);

	// Build Tone
	build_tone(image_tones[image_tones_index], image_tones_amp[image_tones_index], waveform_type);

	// Activate Jack Client
	if (jack_activate(client)) {
		fprintf(stderr, "cannot activate client\n");
		return 1;
	}

	// Init SDL Window
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "Init failed: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_Surface * display;
	display = SDL_SetVideoMode(dest_image->w * window_scale, dest_image->h * window_scale, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
	if (display == NULL) { 
		fprintf(stderr, "SetVideoMode failed: %s\n", SDL_GetError()); 
		exit(1);
	}
	SDL_WM_SetCaption("Sonify", "Sonify");
	SDL_Event event;

	// GUI Loop
	// TODO: Implement a fullscreen mode that can be toggled with a keypress.
	// TODO: The larger the window_scale, the slower SDL_ResizeFactor runs.
	//       Consider writing a function that only redraws a particular portion
	//       of dub_image instead. This will improve performance in fullscreen
	//       mode down the line. 
	// TODO: Can we introduce some simple menu items for changing the transcoding
	//       algorithm on the fly?
	while(1) {
		if (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				break;
			}
		}
		// TODO: Optimize this...
		dub_image = SDL_DisplayFormat(dest_image);
		if (SDL_BlitSurface(SDL_ResizeFactor(dub_image, window_scale, 1), NULL, display, NULL) != 0) {
			fprintf(stderr, "SDL_BlitSurface() Failed.");
			exit(1);
		}
		SDL_Flip(display);
	}

	// Cleanup
	jack_client_close(client);
	del_aubio_pitchdetection(aubio);
	SDL_FreeSurface(display);
	SDL_FreeSurface(dub_image);
	SDL_FreeSurface(dest_image);
	SDL_Quit();
	free(cycle);
	free(image_tones);
	free(image_tones_amp);
	exit(0);
}
