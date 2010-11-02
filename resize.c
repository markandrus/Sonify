/*
 *		Filtered filter_Image Rescaling
 *
 *		  by Dale Schumacher
 *
 */

/*
	Additional changes by Ray Gardener, Daylon Graphics Ltd.
	December 4, 1999

	Extreme modification to this to make it usable with SDL_Surfaces -Dave Olsen 1/2006
	and compatible with c++ compilers.... namely VC++2005 Express edition.
    It's a major hack-job. If anyone cleans this up, please let me know!
    I'm sure it can be made more efficient. (It's lots faster in release than in debug)

	Summary:

		- Horizontal filter contributions are calculated on the fly,
		  as each column is mapped from src to dst image. This lets 
		  us omit having to allocate a temporary full horizontal stretch 
		  of the src image.

		- If none of the src pixels within a sampling region differ, 
		  then the output pixel is forced to equal (any of) the source pixel.
		  This ensures that filters do not corrupt areas of constant color.

		- Filter weight contribution results, after summing, are 
		  rounded to the nearest pixel color value instead of 
		  being casted to Pixel (usually an int or char). Otherwise, 
		  artifacting occurs. 

		- All memory allocations checked for failure; zoom() returns 
		  error code. filter_new_image() returns NULL if unable to allocate 
		  pixel storage, even if filter_Image struct can be allocated.
		  Some assertions added.
*/


// "Public Domain 1991 by Dale Schumacher. Mods by Ray Gardener";
// further mods by ME! (David Olsen)
// and even more by Kevin Baragona, to make it valid C
// and a few more to make it valid C89, and return NULL when needed (David Olsen)


//It would be fantastic if someone would eventually modify these routines to make use
//of native SDL image and pixel formats during the resize process... but, whatever.

#include <stdlib.h>
#include <math.h>
#include <SDL.h>

/* clamp the input to the specified range */
#define CLAMP(v,l,h)    ((v)<(l) ? (l) : (v) > (h) ? (h) : v)
#ifndef M_PI
#define M_PI    3.14159265359
#endif

typedef	Uint8 Pixel;
typedef struct
{
	int	xsize;		/* horizontal size of the image in Pixels */
	int	ysize;		/* vertical size of the image in Pixels */
	Pixel *	data;	/* pointer to first scanline of image */
	int	span;		/* byte offset between two scanlines */
} filter_Image;
typedef struct
{
	int	pixel;
	double	weight;
} CONTRIB;
typedef struct
{
	int	n;		/* number of contributors */
	CONTRIB	*p;		/* pointer to list of contributions */
} CLIST;

SDL_Surface* SDL_ResizeFactor(SDL_Surface *image, float scalefactor,    int filter);
SDL_Surface* SDL_ResizeXY(SDL_Surface *image, int new_w, int new_h, int filter);

static SDL_Surface *filter_resizexy(SDL_Surface* source,int new_w, int new_h, int filter);

static Uint32 filter_GetPixel(SDL_Surface *surface, int x, int y)
{
    int bpp = surface->format->BytesPerPixel;
    /* Here p is the address to the pixel we want to retrieve */
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

    switch(bpp)
    {
        case 1: return *p;
        case 2: return *(Uint16 *)p;
        case 3: if(SDL_BYTEORDER == SDL_BIG_ENDIAN)
                    return p[0] << 16 | p[1] << 8 | p[2];
                else
                    return p[0] | p[1] << 8 | p[2] << 16;
        case 4: return *(Uint32 *)p;
        default: return 0;       /* shouldn't happen, but avoids warnings */
    }
}

static void filter_PutPixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
{
    int bpp = surface->format->BytesPerPixel;
    /* Here p is the address to the pixel we want to set */
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

    switch(bpp)
    {
    case 1: *p = pixel; 
            break;
    case 2: *(Uint16 *)p = pixel; 
            break;
    case 3: if(SDL_BYTEORDER == SDL_BIG_ENDIAN)
            {
                p[0] = (pixel >> 16) & 0xff;
                p[1] = (pixel >> 8) & 0xff;
                p[2] = pixel & 0xff;
            }
            else 
            {
                p[0] = pixel & 0xff;
                p[1] = (pixel >> 8) & 0xff;
                p[2] = (pixel >> 16) & 0xff;
            }
            break;
    case 4: *(Uint32 *)p = pixel;
            break;
    }
}

#ifdef __cplusplus
SDL_Surface* SDL_Resize(SDL_Surface *image, float scalefactor, int filter)
{
    return SDL_ResizeFactor(image, scalefactor, filter);
}
#endif
SDL_Surface* SDL_ResizeFactor(SDL_Surface *image, float scalefactor, int filter)
{
    int neww, newh;
    SDL_Surface * r;
    if (!image) return NULL; //invalid image passed in.
    if (scalefactor > 100.0f) scalefactor = 100.0f; //let's be reasonable...
    neww = (int)((float)image->w*scalefactor);
    newh = (int)((float)image->h*scalefactor);
    if (neww<1) neww = 1;
    if (newh<1) newh = 1;
    r = SDL_ResizeXY(image, neww, newh, filter);
    return r;
}

#ifdef __cplusplus
SDL_Surface* SDL_Resize(SDL_Surface *image, int new_w, int new_h, int filter)
{
    return SDL_ResizeXY(image, new_w, new_h, filter);
}
#endif
SDL_Surface* SDL_ResizeXY(SDL_Surface *image, int new_w, int new_h, int filter)
{		
    SDL_Surface *dest = NULL;
    Uint8 alpha, r, g, b;
    char usealpha;
    int cx;
    if (!image) return NULL; //invalid image passed in

    if ((new_w != image->w) || (new_h != image->h))    
        dest = filter_resizexy(image, new_w, new_h, filter); 
	else
    {
        SDL_FreeSurface(dest);
        dest = image;
    }

    //check for alpha content of the image... like for buttons...

    if SDL_MUSTLOCK(dest) SDL_LockSurface(dest);
    alpha = 0; r = 0; g = 0; b = 0;
    usealpha = 0;
    cx = 0;
    for (; cx < dest->w; cx++)
    { //check the whole image for any occurance of alpha
        int cy = 0;
        for (; cy < dest->h; cy++)
        {	
            SDL_GetRGBA(filter_GetPixel(dest, cx, cy), dest->format, &r, &g, &b, &alpha);
            if (alpha != SDL_ALPHA_OPAQUE) {usealpha = 1; cx=dest->w; break;}
        }
    }
    if SDL_MUSTLOCK(dest) SDL_UnlockSurface(dest);	

    if (!usealpha) // no alpha component
    {	
        image = SDL_DisplayFormat(dest);
        SDL_SetAlpha(image, SDL_RLEACCEL, 0);		
    }
    else // it does have alpha
    {	
        image = SDL_DisplayFormatAlpha(dest);
        SDL_SetAlpha(image, SDL_RLEACCEL | SDL_SRCALPHA, 0);
    }
    SDL_FreeSurface(dest);	
    return image;
}

static Pixel filter_get_pixel2(SDL_Surface *image, int x, int y, int which)
{	
    static Uint8 r=0, g=0, b=0, a=0;	
    static int xx=-1, yy=-1;
    Pixel p = 0;

    if((x < 0) || (x >= image->w) || (y < 0) || (y >= image->h))
        return(0);

    if ((xx!=x) || (yy!=y))
    {
        Uint32 fullpixel;
        xx = x; yy = y; //this way it only calls the Getpixel,RGBA once per pixel...
        fullpixel = filter_GetPixel(image,x,y);
        SDL_GetRGBA(fullpixel,image->format,&r,&g,&b,&a);
    }

    switch (which)
    {
        case 0 : p = r; break;
        case 1 : p = g; break;
        case 2 : p = b; break;
        case 3 : p = a; break;
        default: p = r; break; //not really needed...
    }	
    return(p);
}

static char filter_put_pixel2(SDL_Surface *image, int x, int y, Uint8 c[4])
{	
    if((x < 0) || (x >= image->w) || (y < 0) || (y >= image->h))    
        return 0;
    filter_PutPixel(image,x,y,SDL_MapRGBA(image->format,c[0],c[1],c[2],c[3]));
    return 1;
}

static double filter_hermite_interp(double t)
{
	/* f(t) = 2|t|^3 - 3|t|^2 + 1, -1 <= t <= 1 */
	if(t < 0.0) t = -t;
	if(t < 1.0) return((2.0 * t - 3.0) * t * t + 1.0);
	return(0.0);
}

static double filter_box_interp(double t)
{
	if((t > -0.5) && (t <= 0.5)) return(1.0);
	return(0.0);
}

static double filter_triangle_interp(double t)
{
	if(t < 0.0) t = -t;
	if(t < 1.0) return(1.0 - t);
	return(0.0);
}

static double filter_bell_interp(double t)		/* box (*) box (*) box */
{
	if(t < 0) t = -t;
	if(t < .5) return(.75 - (t * t));
	if(t < 1.5) {
		t = (t - 1.5);
		return(.5 * (t * t));
	}
	return(0.0);
}

static double filter_B_spline_interp(double t)	/* box (*) box (*) box (*) box */
{
	double tt;

	if(t < 0) t = -t;
	if(t < 1) {
		tt = t * t;
		return((.5 * tt * t) - tt + (2.0 / 3.0));
	} else if(t < 2) {
		t = 2 - t;
		return((1.0 / 6.0) * (t * t * t));
	}
	return(0.0);
}

static double filter_sinc(double x)
{
	x *= M_PI;
	if(x != 0) return(sin(x) / x);
	return(1.0);
}

static double filter_Lanczos3_interp(double t)
{
	if(t < 0) t = -t;
	if(t < 3.0) return(filter_sinc(t) * filter_sinc(t/3.0));
	return(0.0);
}

static double filter_Mitchell_interp(double t)
{
	static double B = (1.0 / 3.0);
	static double C = (1.0 / 3.0);
	double tt;

	tt = t * t;
	if(t < 0) t = -t;
	if(t < 1.0) {
		t = (((12.0 - 9.0 * B - 6.0 * C) * (t * tt))
		   + ((-18.0 + 12.0 * B + 6.0 * C) * tt)
		   + (6.0 - 2 * B));
		return(t / 6.0);
	} else if(t < 2.0) {
		t = (((-1.0 * B - 6.0 * C) * (t * tt))
		   + ((6.0 * B + 30.0 * C) * tt)
		   + ((-12.0 * B - 48.0 * C) * t)
		   + (8.0 * B + 24 * C));
		return(t / 6.0);
	}
	return(0.0);
}

static int filter_roundcloser(double d)
{
	/* Untested potential one-liner, but smacks of call overhead */
	/* return fabs(ceil(d)-d) <= 0.5 ? ceil(d) : floor(d); */

	/* Untested potential optimized ceil() usage */
/*	double cd = ceil(d);
	int ncd = (int)cd;
	if(fabs(cd - d) > 0.5)
		ncd--;
	return ncd;
*/

	/* Version that uses no function calls at all. */
	int n = (int) d;
	double diff = d - (double)n;
	if(diff < 0)
		diff = -diff;
	if(diff >= 0.5)
	{
		if(d < 0)
			n--;
		else
			n++;
	}
	return n;
} /* filter_roundcloser */

static int filter_calc_x_contrib(CLIST *contribX, double xscale, double fwidth, 
					int dstwidth, int srcwidth, double (*filterf)(double), int i)
{
	double width;
	double fscale;
	double center, left, right;
	double weight;
	int j, k, n;

	if(xscale < 1.0)
	{
		/* Shrinking image */
		width = fwidth / xscale;
		fscale = 1.0 / xscale;

		contribX->n = 0;
		contribX->p = (CONTRIB *)calloc((int) (width * 2 + 1),
				sizeof(CONTRIB));
		if(contribX->p == NULL)
			return -1;

		center = (double) i / xscale;
		left = ceil(center - width);
		right = floor(center + width);
		for(j = (int)left; j <= right; ++j)
		{
			weight = center - (double) j;
			weight = ((*filterf)(weight / fscale)) / fscale;
			if(j < 0)
				n = -j;
			else if(j >= srcwidth)
				n = (srcwidth - j) + srcwidth - 1;
			else
				n = j;
			
			k = contribX->n++;
			contribX->p[k].pixel = n;
			contribX->p[k].weight = weight;
		}
	
	}
	else
	{
		/* Expanding image */
		contribX->n = 0;
		contribX->p = (CONTRIB *)calloc((int) (fwidth * 2 + 1),
				sizeof(CONTRIB));
		if(contribX->p == NULL)
			return -1;
		center = (double) i / xscale;
		left = ceil(center - fwidth);
		right = floor(center + fwidth);

		for(j = (int)left; j <= right; ++j)
		{
			weight = center - (double) j;
			weight = (*filterf)(weight);
			if(j < 0) {
				n = -j;
			} else if(j >= srcwidth) {
				n = (srcwidth - j) + srcwidth - 1;
			} else {
				n = j;
			}
			k = contribX->n++;
			contribX->p[k].pixel = n;
			contribX->p[k].weight = weight;
		}
	}
	return 0;
} /* filter_calc_x_contrib */

static int filter_zoom2(SDL_Surface *dst, SDL_Surface *src, double (*filterf)(double), double fwidth)
{
	Pixel* tmp;
	double xscale, yscale;		/* zoom scale factors */
	int xx;
	int i, j, k;			/* loop variables */
	int n;				/* pixel number */
	double center, left, right;	/* filter calculation variables */
	double width, fscale, weight;	/* filter calculation variables */
	Uint8 weightedcolor[4]; //reconstruct the pixel out of these!
	Pixel pel, pel2;
	int bPelDelta;
	CLIST	*contribY;		/* array of contribution lists */
	CLIST	contribX;
	int		nRet = -1;

	/* create intermediate column to hold horizontal dst column zoom */
	tmp = (Pixel*)malloc(src->h * sizeof(Pixel) * 4);
	if(tmp == NULL)
		return 0;

	xscale = (double) dst->w / (double) src->w;

	/* Build y weights */
	/* pre-calculate filter contributions for a column */
	contribY = (CLIST *)calloc(dst->h, sizeof(CLIST));
	if(contribY == NULL)
	{
		free(tmp);
		return -1;
	}

	yscale = (double) dst->h / (double) src->h;

	if(yscale < 1.0)
	{
		width = fwidth / yscale;
		fscale = 1.0 / yscale;
		for(i = 0; i < dst->h; ++i)
		{
			contribY[i].n = 0;
			contribY[i].p = (CONTRIB *)calloc((int) (width * 2 + 1),
					sizeof(CONTRIB));
			if(contribY[i].p == NULL)
			{
				free(tmp);
				free(contribY);
				return -1;
			}
			center = (double) i / yscale;
			left = ceil(center - width);
			right = floor(center + width);
			for(j = (int)left; j <= right; ++j) {
				weight = center - (double) j;
				weight = (*filterf)(weight / fscale) / fscale;
				if(j < 0) {
					n = -j;
				} else if(j >= src->h) {
					n = (src->h - j) + src->h - 1;
				} else {
					n = j;
				}
				k = contribY[i].n++;
				contribY[i].p[k].pixel = n;
				contribY[i].p[k].weight = weight;
			}
		}
	} else {
		for(i = 0; i < dst->h; ++i) {
			contribY[i].n = 0;
			contribY[i].p = (CONTRIB *)calloc((int) (fwidth * 2 + 1),
					sizeof(CONTRIB));
			if(contribY[i].p == NULL)
			{
				free(tmp);
				free(contribY);
				return -1;
			}
			center = (double) i / yscale;
			left = ceil(center - fwidth);
			right = floor(center + fwidth);
			for(j = (int)left; j <= right; ++j) {
				weight = center - (double) j;
				weight = (*filterf)(weight);
				if(j < 0) {
					n = -j;
				} else if(j >= src->h) {
					n = (src->h - j) + src->h - 1;
				} else {
					n = j;
				}
				k = contribY[i].n++;
				contribY[i].p[k].pixel = n;
				contribY[i].p[k].weight = weight;
			}
		}
	}
	


	for(xx = 0; xx < dst->w; xx++)
	{
		if(0 != filter_calc_x_contrib(&contribX, xscale, fwidth, 
								dst->w, src->w, filterf, xx))
		{
			goto __zoom_cleanup;
		}
		/* Apply horz filter to make dst column in tmp. */
		for(k = 0; k < src->h; ++k)
		{
		  //mine!!!!
		  int w=0;
		  for (; w<4; w++) {
			weight = 0.0;
			bPelDelta = 0;
			pel = filter_get_pixel2(src, contribX.p[0].pixel, k, w);
			for(j = 0; j < contribX.n; ++j)
			{
				pel2 = filter_get_pixel2(src, contribX.p[j].pixel, k, w);
				if(pel2 != pel)
					bPelDelta = 1;
				weight += pel2 * contribX.p[j].weight;
			}
			weight = bPelDelta ? filter_roundcloser(weight) : pel;

			tmp[k+w*src->h] = (Pixel)CLAMP(weight, 0, 255); // keep it Uint8
		  } //cycle through each color...
		} /* next row in temp column */

		free(contribX.p);


		/* The temp column has been built. Now stretch it 
		 vertically into dst column. */
		for(i = 0; i < dst->h; ++i)
		{
		  int w=0;
		  for (; w<4; w++) {
			weight = 0.0;
			bPelDelta = 0;
			pel = tmp[contribY[i].p[0].pixel+w*src->h];

			for(j = 0; j < contribY[i].n; ++j)
			{
				pel2 = tmp[contribY[i].p[j].pixel+w*src->h];
				if(pel2 != pel)
					bPelDelta = 1;
				weight += pel2 * contribY[i].p[j].weight;
			}
			weight = bPelDelta ? filter_roundcloser(weight) : pel;
			weightedcolor[w] = (Uint8)CLAMP(weight, 0, 255); //get all 4 "colors" this way
	      }
		  filter_put_pixel2(dst, xx, i, weightedcolor ); //keep it Uint8
		} /* next dst row */

	} /* next dst column */
	nRet = 0; /* success */

__zoom_cleanup:
	free(tmp);

	/* free the memory allocated for vertical filter weights */
	for(i = 0; i < dst->h; ++i)
		free(contribY[i].p);
	free(contribY);

	return nRet;
} /* zoom */

static SDL_Surface *filter_resizexy(SDL_Surface* source,int new_w, int new_h, int filter)
{	
    //f and s need to be complementary... one as filter, one as support.
	double (*f)(double) ; //function pointer
	double s; //support
    SDL_Surface *temp, *dest;

	const double box_support = 0.5,
			triangle_support = 1.0,
			bell_support     = 1.5,
			B_spline_support = 2.0,
			hermite_support  = 1.0,			
			Mitchell_support = 2.0,
            Lanczos3_support = 3.0;

	switch (filter) 
    {	
        case 1 : f=filter_box_interp;       s=box_support;      break;
        case 2 : f=filter_triangle_interp;  s=triangle_support; break;
        case 3 : f=filter_bell_interp;      s=bell_support;     break;
		case 4 : f=filter_hermite_interp;	s=hermite_support;	break;
		case 5 : f=filter_B_spline_interp;	s=B_spline_support;	break;		
		case 6 : f=filter_Mitchell_interp;	s=Mitchell_support;	break;
        case 7 : f=filter_Lanczos3_interp;	s=Lanczos3_support;	break;
		default: f=filter_Lanczos3_interp;	s=Lanczos3_support;	break;
	}

    //Make new surface and send it in to the real filter
	temp = SDL_CreateRGBSurface(SDL_SWSURFACE | SDL_SRCALPHA,
				new_w, new_h, 32,0,0,0,0) ,
	dest = SDL_DisplayFormatAlpha(temp);

	SDL_FreeSurface(temp);

	if SDL_MUSTLOCK(source) SDL_LockSurface(source);
	if SDL_MUSTLOCK(dest)   SDL_LockSurface(dest);

	filter_zoom2(dest, source, f, s ); 

	if SDL_MUSTLOCK(dest)   SDL_UnlockSurface(dest);
	if SDL_MUSTLOCK(source) SDL_UnlockSurface(source);	

	SDL_FreeSurface(source);	
	//should be all cleaned up!

	return dest;
}
