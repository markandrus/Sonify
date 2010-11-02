// sonify_cleanup.c
#include <math.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <jack/jack.h>
#include <gd.h>
#include <aubio/aubio.h>

#include "math_util.h"
#include "color_util.h"

#include <SDL_image.h>
#include <SDL.h>

//#include <gnome.h>

//#include "interface.h"
//#include "support.h"
typedef struct
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
} color;

color get_Color(SDL_Surface *img, int x, int y)
{
	color rgb;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t *pixels = (uint8_t*) img->pixels;
	uint8_t pixel = pixels[(y*img->w) + x];
	SDL_GetRGB(pixel, img->format, &r, &g, &b);
	rgb.r = r;
	rgb.g = g;
	rgb.b = b;
	return rgb;
}
//pthread_t thread;
const double PI = 3.14;

// Jack
typedef jack_default_audio_sample_t sample_t;
jack_port_t *output_port;
jack_port_t *input_port;

char *global_file_name;
int global_image_format;
FILE *global_image_file;

// Waveform Synthesis
jack_nframes_t sample_rate;

sample_t *cycle;
sample_t *temp_cycle;

jack_nframes_t samples_per_cycle;
jack_nframes_t temp_samples_per_cycle;

enum TYPE { Sine = 0, Square, Triangle, Sawtooth };
enum TYPE waveform_type = Sine; // *

float tone = 101;
double amplitude = 1;

float *image_tones;
float *image_tones_amp;

int image_tones_size;
int image_tones_index = 0;
long offset = 0;
float ms_time = 0; // *
int pitch_scale = 5000; // *
int lower_bounds = 100; // *
int framecount = 0;
float hopsize = 0;
float bufsize = 0;
float max_amp = 0;
int X = 0;
int Y = 0;

//gdImagePtr source_image, dest_image;
SDL_Surface *source_image, *dest_image;

// aubio pitch detection stuff
aubio_pitchdetection_t *aubio = NULL;
smpl_t aubio_pitch = 0;
fvec_t *aubio_fvec = NULL;

void *thread_save();

void init_aubio() {
	hopsize = sample_rate * 0.001 * ms_time;
	bufsize = sizeof(sample_t) * hopsize;
	aubio = new_aubio_pitchdetection(bufsize, hopsize, 1, sample_rate, aubio_pitch_fcomb, aubio_pitchm_freq);
	aubio_fvec = new_fvec(hopsize, 1);
}

//void generate_tone_array(gdImagePtr image) {
void generate_tone_array(SDL_Surface *image) {
	int width, height, x, y, p, c = 0;
	//width = gdImageSX(image);
	//height = gdImageSY(image);
	width = image->w;
	height = image->h*4;
	image_tones_size = width * height;
	image_tones = (float *) malloc(width * height * sizeof(float));
	image_tones_amp = (float *) malloc(width * height * sizeof(float));
	if (image_tones == NULL || image_tones_amp == NULL) {
		fprintf(stderr,"memory allocation failed\n");
		exit(3);
	}
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			//p = gdImageGetPixel(image, x, y);
			color rgb;
			rgb = get_Color(image, x, y);
			float H, S, L;
			Rgb2Hsl(&H, &S, &L, (float) (rgb.r / 255.0), (float) (rgb.g / 255.0), (float) (rgb.b / 255.0));
			image_tones[c] = H * pitch_scale + lower_bounds; // Hue = Frequency
			image_tones_amp[c] = L; // Luminosity = Amplitude
			c++;
		}
	}
}

void write_to_image(SDL_Surface *image, float R, float G, float B) {
	int width, height;
	//width = gdImageSX(image);
	//height = gdImageSY(image);
	width = image->w;
	height = image->h;
	if (X == width) {
		X = 0;
		Y++;
	}
	if (Y == height*4) {
		Y = 0;
	}
	//int color = gdImageColorAllocate(image, R, G, B);
	uint8_t color = SDL_MapRGB(image->format, (uint8_t) (R * 255.0), (uint8_t) (G * 255.0), (uint8_t) (B * 255.0));
	//gdImageSetPixel(image, X, Y, color);
	uint8_t *pixels = (uint8_t*)image->pixels;
	pixels[(Y*image->w) + X] = color;
	X++;
}

void build_tone(float t, float a, enum TYPE type) {
	temp_samples_per_cycle = (sample_rate / t);
	sample_t scale = 2 * PI / temp_samples_per_cycle;
	temp_cycle = (sample_t *) malloc(temp_samples_per_cycle * sizeof(sample_t));
	
	if (temp_cycle == NULL) {
		fprintf(stderr,"memory allocation failed\n");
		exit(3);
	}
	
	int i = 0;
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

void swap_cycle() {
	if (cycle != NULL) {
		sample_t *trash = cycle;
		cycle = temp_cycle;
		free(trash);
	} else {
		cycle = temp_cycle;
	}
	temp_cycle = NULL;
	samples_per_cycle = temp_samples_per_cycle;
	offset = 0;
}

int process(jack_nframes_t nframes, void *arg) {
	sample_t *in = (sample_t *) jack_port_get_buffer(input_port, nframes);
	sample_t *out = (sample_t *) jack_port_get_buffer(output_port, nframes);
	if (aubio_fvec == NULL || aubio_fvec->data == NULL) {
		init_aubio();
	}
	jack_nframes_t i;
	for (i = 0; i < nframes; i++) {
		if (framecount == hopsize) {
			image_tones_index++;
			if (image_tones_index >= image_tones_size) {
				image_tones_index = 0;
			}
			build_tone(image_tones[image_tones_index], 1-image_tones_amp[image_tones_index], waveform_type);
			float H, S, L, R, G, B;
			Sound_to_HSL(aubio_pitchdetection(aubio, aubio_fvec), max_amp, pitch_scale, lower_bounds, &H, &S, &L);
			Hsl2Rgb(&R, &G, &B, H, S, 1-L);
			write_to_image(dest_image, R, G, B);
			framecount = 0;
			max_amp = 0;
		}
		out[i] = cycle[offset];
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

int srate(jack_nframes_t nframes, void *arg) {
	printf("the sample rate is now %lu/sec\n", nframes);
	sample_rate = nframes;
	return 0;
}

void error(const char *desc) {
	fprintf(stderr, "JACK error: %s\n", desc);
}

void jack_shutdown(void *arg) {
	exit(1);
}

void save_image_file(FILE *image_file, char *file_name, int image_format) {
	//image_file = fopen(file_name, "wb");
	//if (image_format == 1) {
		//gdImageJpeg(dest_image, image_file, -1);
	//} else {
		//gdImagePng(dest_image, image_file);
	//}
	//fclose(image_file);
}

void *thread_interface() {
	while(1) {
		int choice = 0;
		printf("0-Quit, 1-Save, 2-Edit, 3-Position\n");
		printf(" > ");
		scanf("%i", &choice);
		if (choice == 0) {
			break;
		} else if (choice == 1) {
			//save_image_file(global_image_file, global_file_name, global_image_format);
		} else if (choice == 2) {
			printf("0-Return, 1-Pitch Scale, 2-Lower Bounds, 3-Time-per-pixel (ms), 4-Waveform Type\n");
			printf(" > ");
			scanf("%i", &choice);
			if (choice == 1) {
				printf("Pitch Scale: ");
				scanf("%d", &pitch_scale);
			} else if (choice == 2) {
				printf("Lower Bounds: ");
				scanf("%d", &lower_bounds);
			} else if (choice == 3) {
				printf("Time-per-pixel (ms): ");
				scanf("%f", &ms_time);
				init_aubio();
			} else if (choice == 4) {
				printf("Waveform Type: ");
				scanf("%d", &waveform_type);
			}
		} else {
			printf("Currently at (%i, %i)...\n", X, Y);
		}
	}
}

int main(int argc, char *argv[]) {
	pthread_t thread_interface;//, thread_gtk;

	//GtkWidget *sonify_app;
	//gnome_program_init(PACKAGE, VERSION, LIBGNOMEUI_MODULE, argc, argv, GNOME_PARAM_APP_DATADIR, PACKAGE_DATA_DIR, NULL);
	//sonify_app = create_sonify_app();
	//gtk_widget_show(sonify_app);
	//pthread_create(&thread_gtk, NULL, gtk_main, NULL);
	//gtk_main();

	int image_format;
	int image_format2;
	jack_client_t *client;
	const char **ports;
	char file_name[100], file_name2[100];
	//char * file_name = "un.png";
	//char * file_name2 = "nu.png";
  
	if (argc < 5) {
		fprintf(stderr, "usage: sonify <client name> <image path> <freq scale> <lowest freq> <sine | square | saw | tri>\n");
		fprintf(stderr, "i.e. sonify sfy img_in.png 10000 1000 1 sine\n");
		return 1;
	}
	
	jack_set_error_function(error);
  
	if ((client = jack_client_new(argv[1])) == 0) {
		fprintf (stderr, "Jack server not running?\n");
		return 1;
	}

	strcpy(file_name, argv[2]);
	pitch_scale = atoi(argv[3]);
	lower_bounds = atoi(argv[4]);
	ms_time = atoi(argv[5]);
	if (strcmp(argv[6], "tri")==0) { waveform_type = 2; }
	else if (strcmp(argv[6], "square")==0) { waveform_type = 1; }
	else if (strcmp(argv[6], "saw")==0) { waveform_type = 3; } else { waveform_type = 0; }
  
	jack_set_process_callback(client, process, 0);
	jack_set_sample_rate_callback(client, srate, 0);
	jack_on_shutdown(client, jack_shutdown, 0);
	
	sample_rate = jack_get_sample_rate(client);
	
	input_port = jack_port_register(client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	output_port = jack_port_register(client, "output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
/*
	printf("Source Image: ");
	scanf("%s", &file_name);
	printf("0-Png, 1-Jpg: ");
	scanf("%d", &image_format);
	printf("Destination Image: ");
	scanf("%s", &file_name2);	
	printf("0-Png, 1-Jpg: ");
	scanf("%d", &image_format2);	
	printf("Pitch Scale: ");
	scanf("%d", &pitch_scale);
	printf("Lower Bounds: ");
	scanf("%d", &lower_bounds);
	printf("Time-per-pixel (ms): ");
	scanf("%f", &ms_time);
	init_aubio();
	printf("Waveform Type: ");
	scanf("%d", &waveform_type);
*/
	/*FILE *image_file;
	image_file = fopen(file_name, "rb");

	if (image_format == 1) {
		source_image = gdImageCreateFromJpeg(image_file);
	} else {
		source_image = gdImageCreateFromPng(image_file);
	}*/
	source_image = IMG_Load(file_name);

	generate_tone_array(source_image);
	//gdImageDestroy(source_image);
	SDL_FreeSurface(source_image);

	//dest_image = gdImageCreateTrueColor(gdImageSX(source_image), gdImageSY(source_image));
	dest_image = SDL_CreateRGBSurface (SDL_SWSURFACE, source_image->w, source_image->h, 32, 0, 0, 0, 0);

	if(dest_image == NULL) {
		fprintf(stderr, "CreateRGBSurface failed: %s\n", SDL_GetError());
		exit(1);
    	}

	build_tone(image_tones[image_tones_index], image_tones_amp[image_tones_index], waveform_type);
	
	if (jack_activate(client)) {
		fprintf(stderr, "cannot activate client\n");
		return 1;
	}

	/*if ((ports = jack_get_ports(client, NULL, NULL, JackPortIsOutput)) == NULL) {
		fprintf(stderr, "no physical capture ports\n");
		exit(1);
	}

	int i = 0;
	while (ports[i] != NULL) {
		if (jack_connect(client, ports[i], jack_port_name(input_port))) {
			fprintf(stderr, "cannot connect input ports\n");
		}
		i++;
	}

	if ((ports = jack_get_ports(client, NULL, NULL, JackPortIsPhysical|JackPortIsInput)) == NULL) {
		fprintf(stderr, "cannot find any physical playback ports\n");
		exit(1);
	}
  
	i = 0;
	while (ports[i] != NULL && i < 2) {
		if (jack_connect(client, jack_port_name(output_port), ports[i])) {
			fprintf(stderr, "cannot connect output ports\n");
		}
		i++;
	}
  
	free(ports);*/

	//pthread_create(&thread_interface, NULL, thread_interface, NULL);
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init() Failed.");
		exit(1);
	}
	SDL_Surface * display;
	display = SDL_SetVideoMode((*source_image).w, (*source_image).h, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
	if (display == NULL) { fprintf(stderr, "SDL_SetVideoMode() Failed."); exit(1); }

	SDL_WM_SetCaption("Sonify", "Sonify");
	SDL_Event event;
	while(1) {
		//save_image_file(global_image_file, global_file_name, global_image_format);
		if (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				break;
			}
		}
		if (SDL_BlitSurface(dest_image, NULL, display, NULL) != 0) {
			fprintf(stderr, "SDL_BlitSurface() Failed.");
			exit(1);
		}
		SDL_Flip(display);
	}

	jack_client_close(client);
	del_aubio_pitchdetection(aubio);
	//gdImageDestroy(dest_image);
	SDL_Quit();
	SDL_FreeSurface(dest_image);
	free(cycle);
	free(image_tones);
	free(image_tones_amp);
	exit(0);
}
