/**********************************************************************//**
***************************************************************************
*** @file    yuriscope_cam.c
***
*** @brief   image acquisition for the VC MIPI IMX226.
***
*** @author Ashish Krishnamoorthy
***************************************************************************
***************************************************************************/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#ifndef V4L2_PIX_FMT_SBGGR14
 #define V4L2_PIX_FMT_SBGGR14 v4l2_fourcc('B', 'G', '1', '4') /* 14  Color, MIPI unpacked */
#endif
#ifndef V4L2_PIX_FMT_SRGGB14
 #define V4L2_PIX_FMT_SRGGB14 v4l2_fourcc('R', 'G', '1', '4') /* 14  Color, MIPI unpacked */
#endif
#ifndef V4L2_PIX_FMT_Y14P
 #define V4L2_PIX_FMT_Y14P    v4l2_fourcc('Y', '1', '4', 'P') /* 14  Greyscale, MIPI RAW14 packed */
#endif
#ifndef V4L2_PIX_FMT_Y12P
 #define V4L2_PIX_FMT_Y12P    v4l2_fourcc('Y', '1', '2', 'P') /* 12  Greyscale, MIPI RAW12 packed */
#endif
#ifndef V4L2_PIX_FMT_Y10P
 #define V4L2_PIX_FMT_Y10P    v4l2_fourcc('Y', '1', '0', 'P') /* 10  Greyscale, MIPI RAW10 packed */
#endif

#include <linux/fb.h>
#include <syslog.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#include <libpng/png.h>

#ifdef __cplusplus
#ifdef WITH_OPENCV // defined by Makefile if choosing target 'yuriscope_cam-opencv'
	#include <opencv2/core.hpp>
	#include <opencv2/imgcodecs.hpp>
	#include <opencv2/highgui.hpp>
	#include <opencv2/imgproc.hpp>
#endif
#endif

#include "vclib-excerpt.h"
#include "vcimgnet.h"



//#define DURATION_TEST
// #define ADDED_FEAT

#define NUM_IMAGES 1// define the number of images to be captured
#define FPS 2
// #define NUM_BUFFERS 8 //uncomment after testing and optimal num buffers is found


/*--*STRUCT*----------------------------------------------------------*/
/**
*  @brief  Store timestamp and also of the number of the LED to turn on.
*
*    This structure holds the information required to test the microLED boards with the IMX226 VC MIPI camera.
*/
typedef struct
{
    char[20] timestamp;
	int counter;
} microLED_test;


#define  NULL_QBuf { NULL, NULL, 0 }




/*--*STRUCT*----------------------------------------------------------*/
/**
*  @brief  Capture Queue Slot.
*
*    This structure represents a capture queue slot
*    through which data of captured images is accessible.
*/
typedef struct
{
	char              **st;           /*!<   multiple allocated image regions due to hardware */
	struct v4l2_plane  *plane;
	size_t              planeCount;   /*!<   Number of regions          */
} QBuf;
#define  NULL_QBuf { NULL, NULL, 0 }


/*--*STRUCT*----------------------------------------------------------*/
/**
*  @brief  Sensor Access and Attributes, Image Capture Queue Slots.
*
*    This structure holds the file descriptor of the sensor for access
*    as well as sensor specific information, like width and height of
*    the pixel image. Also available are the capture queue slots to
*    be able to access the recorded images.
*/
typedef struct
{
	int      fd;  /*!<  File Descriptor of the opened Sensor Device.  */

	QBuf    *qbuf; /*!<  Queue Buffers where Images are recorded to.  */
	U32      qbufCount; /*!<  Number of Queue Buffers available.      */

	struct v4l2_format  format; /*!<  Sensor Attributes.            */

} VCMipiSenCfg;
#define NULL_VCMipiSenCfg  { -1, NULL,0, {0} }

/*--*ENUM*------------------------------------------------------------*/
/**
*  @brief  Sensor White Balance Mode
*
*    This enum encodes the Whitebalancer mode to be used.
*    See function @ref process_whitebalance() for usage instructions.
*/
typedef enum
{
	WBMODE_INACTIVE = 0,
	WBMODE_MEASURE  = 1,
	WBMODE_APPLY    = 2
} VCWhiteBalMode;
/*--*STRUCT*----------------------------------------------------------*/
/**
*  @brief  Sensor White Balance Parameters for colored images.
*
*    Run a white balance calculation for an image of type IMAGE_RGB.
*    See function @ref process_whitebalance() for usage instructions.
*/
typedef struct
{
	VCWhiteBalMode  mode;     /*!<  White balance mode  */

	int             ampRed;   /*!<  Red   channel amplifier [1 .. 255], for example 121.  */
	int             ampGreen; /*!<  Green channel amplifier [1 .. 255], for example 198.  */
	int             ampBlue;  /*!<  Blue  channel amplifier [1 .. 255], for example 125.  */
} VCWhiteBalCfg;
#define NULL_VCWhiteBalCfg  { WBMODE_INACTIVE, 1, 1, 1 }


static int  globVarQuitIff1 = 0;  // Will be set - for example - if CTRL-C is pressed,
void  sig_handler(int signo);

int  change_options_by_commandline(int argc, char *argv[], int *shutter, float *gain, int *maxCaptures, int *fileOutIff1, int *bufCount, int *fps);
int  media_set_roi(char *pcVideoDev, int optX0, int optY0, int optWidth, int optHeight);
int  sensor_open(char *dev_video_device, VCMipiSenCfg *sen, unsigned int qBufCount);
int  sensor_close(VCMipiSenCfg *sen);
int  sensor_set_shutter_gain(VCMipiSenCfg  *sen, int newGain, int newShutter);
// int  sensor_set_cropping_roi(VCMipiSenCfg  *sen, int newX0, int newY0, int newWidth, int newHeight);
int  sensor_set_fps(VCMipiSenCfg *sen, I32 fps);
int  sensor_streaming_start(VCMipiSenCfg *sen);
int  sensor_streaming_stop(VCMipiSenCfg *sen);
int  capture_buffer_enqueue(I32 bufIdx, VCMipiSenCfg *sen);
int  capture_buffer_dequeue(I32 bufIdx, VCMipiSenCfg *sen);
int  sleep_for_next_capture(VCMipiSenCfg  *sen, int timeoutUS);
// TODO: test if these two are needed and delete
// int  imgnet_connect(VCImgNetCfg *imgnetCfg, U32 pixelformat, int dx, int dy);
// int  imgnet_disconnect(VCImgNetCfg *imgnetCfg);
int  process_capture(unsigned int pixelformat, char *st, int dx, int dy, int pitch, int netSrvOutIff1, VCImgNetCfg *imgnetCfg, int fileOutIff1, int frameNr, char *pcFramebufferDev, VCWhiteBalCfg *cfgWB);
I32  process_whitebalance(image *img, VCWhiteBalCfg *cfgWB);
I32  copy_grey_to_image(image *imgOut, char *bufIn, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  convert_raw10_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  convert_raw12_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  convert_raw14_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  convert_raw_and_debayer_image(image *imgOut, char *bufIn, U32 pixelformat, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  convert_16bit_to_image(image *imgOut, char *bufIn, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 maxBits);
I32  convert_srggb_and_debayer_image(image *imgOut, char *bufIn, U32 pixelformat, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 maxBits);
I32  convert_yuyv_to_image(image *imgOut, char *bufIn, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
I32  simple_debayer_to_image(image *imgOut, char *bufIn, unsigned int pixelformat, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes);
int  copy_image(image *in, image *out);
int  copy_image_to_framebuffer(char *pcFramebufferDev, const void *pvDataGREY_OR_R, const void *pvDataGREY_OR_G, const void *pvDataGREY_OR_B, U32 dy, U32 pitch);
I32  write_image_as_pnm(char *path, image *img);
I32 write_image_as_png(char *path, image *img);
void timemeasurement_start(struct  timeval *timer);
void timemeasurement_stop(struct  timeval *timer, I64 *s, I64 *us);
#ifdef __cplusplus
#ifdef WITH_OPENCV
	I32  openCV_example(image *imgIn, image *imgOut);
#endif
#endif





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Main Function of yuriscope_cam.
*
*  This is the main function of the yuriscope_cam.
*/
/*-----------------------------------------------------------------------------*/
int  main(int argc, char *argv[])
{
	char           acVideoDev[30]     = "";
	char           acFramebufferDev[] = "/dev/fb0";
	int            timeoutUS          = 1000;

	#ifdef DURATION_TEST
		struct timeval timer;
		I64            seconds, useconds;
		int            timerCycles = 100;
		int            run=0;
	#endif

	int            ee, rc=0; unsigned int bufIdx;
	int            netSrvIff1 = 0;
	int            frameNr=0;
	int            optShutter, optMaxCaptures, optBufCount, optFileOutIff1, optVideoDevId;
	int            optWidth, optHeight, optX0, optY0, optFps;
	float          optGain;
	VCMipiSenCfg   sen       = NULL_VCMipiSenCfg;
	VCImgNetCfg    imgnetCfg = NULL_VCImgNetCfg;
	VCWhiteBalCfg  cfgWB     = NULL_VCWhiteBalCfg;

	// Handling pressing of CTRL-C to quit sane.
	{
		if(
		     (signal(SIGINT, sig_handler) == SIG_ERR)
		   ||(signal(SIGTERM,sig_handler) == SIG_ERR)
		   ||(signal(SIGQUIT,sig_handler) == SIG_ERR)
		  )
		{ee=-1;goto quit;}
	}

	// Set up configuration and apply command line parameters if set.
	{
		optFileOutIff1 = 1;//hardcode to 1 to always enale saving of images
		optShutter     = 5000;//Exposure time is in microseconds
		optGain        = 0;//Gain is set to zero by default (for our experiment)
		optBufCount    = 135;//>128 
		optVideoDevId  = 0;
		optWidth       = -1;
		optHeight      = -1;
		optX0          = -1;
		optY0          = -1;
		// optMaxCaptures = -1;
		optMaxCaptures = NUM_IMAGES; //defined by Macro
		// optFps         = -1;//change FPS value for continuous capture mode
		optFps = FPS;
		rc =  change_options_by_commandline(argc, argv, &optShutter, &optGain, &optMaxCaptures, &optFileOutIff1, &optBufCount, &optFps);
		if(rc>0){ee=0; goto quit;}
		if(rc<0){ee=-2+100*rc; goto quit;}

		snprintf(acVideoDev, 29, "/dev/video%d", optVideoDevId);
	}

	microLED_test mLED;

	// For reference only: Replaced  by  sensor_set_cropping_roi()  for newer kernels.
	// The RaspberryPi with linux kernel v.<=4.19 with commented out request for unpacked format at sensor_open() could work so.
	//rc =  media_set_roi(acVideoDev, optX0, optY0, optWidth, optHeight);
	//if(rc<0){ee=-3+100*rc; goto quit;}

	// Gets capture dimensions for imgnet_connect().
	rc =  sensor_open(acVideoDev, &sen, optBufCount);
	if(rc<0){ee=-4+100*rc; goto quit;}

	// rc =  sensor_set_cropping_roi(&sen, optX0, optY0, optWidth, optHeight);
	// if(rc<0){ee=-5+100*rc; goto quit;}

	// If vcimgnetsrv is started in background, this connects to it to transfer the captures.
	// Note: Independent of the multiplane state (pix_mp vs. pix), we access the information by (pix)
	//       since fmt is a union and pix as well as pix_mp start with those same variables (see C standard).
	// rc =  imgnet_connect(&imgnetCfg, sen.format.fmt.pix.pixelformat, sen.format.fmt.pix.width, sen.format.fmt.pix.height);
	// if(rc!=0){ netSrvIff1=0; }
	// else     { netSrvIff1=1; }

	// Apply new Shutter and Gain Settings and optionally request Frames per Second.
	{
		rc =  sensor_set_shutter_gain(&sen, optGain, optShutter);
		if(rc<0)
		{
			printf("Warning:  Could not set Gain/Shutter!\n");
		}
		if(optFps > 0)
		{
			rc =  sensor_set_fps(&sen, optFps);
			if(rc!=0)
			{
				printf("Warning:  Could not set Frames per Second! (rc: %d)\n", rc);
			}
		}
	}


	// Pre-Enqueue all capture buffers into the capture queue
	{
		for(bufIdx= 0; bufIdx< sen.qbufCount; bufIdx++)
		{
			rc =  capture_buffer_enqueue(bufIdx, &sen);
			if(rc<0){ee=-6+100*rc; goto quit;}
		}

		//cyclic processing -> bufIdx = 0
		bufIdx = 0;
	}

	rc =  sensor_streaming_start(&sen);
	if(rc<0){ee=-7+100*rc; goto quit;}

	#ifdef DURATION_TEST
		timemeasurement_start(&timer);
	#endif
	while((1!=globVarQuitIff1)&&(optMaxCaptures!=0))
	{
		// Don't poll, sleeping until wakeup by select() is better.
		rc =  sleep_for_next_capture(&sen, timeoutUS);
		if(rc<0){ee=-8+100*rc; goto quit;}
		if(rc>0)
		{
			continue; //Timeout occured and no capture is ready, sleep again.
		}

		rc =  capture_buffer_dequeue(bufIdx, &sen);
		if(rc>0){continue;} // Buffer not yet available, wait again, should not happen if using sleep_for_next_capture().
		if(rc<0){ee=-9+100*rc; goto quit;}

		switch(sen.format.type)
		{
			case V4L2_BUF_TYPE_VIDEO_CAPTURE:
					rc =  process_capture(sen.format.fmt.pix.pixelformat, sen.qbuf[bufIdx].st[0], sen.format.fmt.pix.width, sen.format.fmt.pix.height, sen.format.fmt.pix.bytesperline, netSrvIff1, &imgnetCfg, optFileOutIff1, frameNr++, &cfgWB);
					// rc =  process_capture(sen.format.fmt.pix.pixelformat, sen.qbuf[bufIdx].st[0], sen.format.fmt.pix.width, sen.format.fmt.pix.height, sen.format.fmt.pix.bytesperline, &imgnetCfg, optFileOutIff1, frameNr++, &cfgWB);
			
			break;
			case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
					rc =  process_capture(sen.format.fmt.pix_mp.pixelformat, sen.qbuf[bufIdx].st[0], sen.format.fmt.pix_mp.width, sen.format.fmt.pix_mp.height, sen.format.fmt.pix_mp.plane_fmt[0].bytesperline, netSrvIff1, &imgnetCfg, optFileOutIff1, frameNr++,&cfgWB);
					// rc =  process_capture(sen.format.fmt.pix_mp.pixelformat, sen.qbuf[bufIdx].st[0], sen.format.fmt.pix_mp.width, sen.format.fmt.pix_mp.height, sen.format.fmt.pix_mp.plane_fmt[0].bytesperline, &imgnetCfg, optFileOutIff1, frameNr++,&cfgWB);

			break;
			default: ee= -10; goto quit;
		}
		if(rc<0){ee=-11+100*rc; goto quit;}

		rc =  capture_buffer_enqueue(bufIdx, &sen);
		if(rc<0){ee=-12+100*rc; goto quit;}

		#ifdef DURATION_TEST
			// Print Out Duration.
			if(((timerCycles)-1)==(run%(timerCycles)))
			{
				timemeasurement_stop(&timer, &seconds, &useconds);
				printf("Acquisiton&Copy Duration:%11llds%11lldus  for %d Cycles ==  %ffps.\n\n", seconds, useconds, (run%timerCycles)+1, (F32)1000000 * ((run%timerCycles)+1)/(seconds * 1000000 + useconds));
				// if(1!=netSrvIff1){ printf("\033[%dA", 2); }
				timemeasurement_start(&timer);
			}
			run++;
		#endif

		//cyclic processing
		bufIdx = (bufIdx+1) % sen.qbufCount;

		if(optMaxCaptures>=0)
			if(0==optMaxCaptures--)
				break;
	}

	rc =  sensor_streaming_stop(&sen);
	if(rc<0){ee=-13+100*rc; goto quit;}


	ee=0;
quit:
	if(ee!=0){ printf("\n  '%s' quits with error code: %d\n\n", argv[0], ee); }

	sensor_close(&sen);

	if(1==netSrvIff1)
	{
		imgnet_disconnect(&imgnetCfg);
	}

	return(0);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Processes a Capture: Copy it to several Outputs.
*
*  This function processes a capture image by copying it to selected outputs.
*/
/*-----------------------------------------------------------------------------*/
int  process_capture(unsigned int pixelformat, char *st, int dx, int dy, int pitch, int netSrvOutIff1, VCImgNetCfg *imgnetCfg, int fileOutIff1, int frameNr, char *pcFramebufferDev, VCWhiteBalCfg *cfgWB)
{
	int    rc, ee;
	image  imgConverted = NULL_IMAGE;
	char   acFilename[256];

	// Allocate temporary image
	{
		switch(pixelformat)
		{
			case V4L2_PIX_FMT_SRGGB14P:
			case V4L2_PIX_FMT_SRGGB14:
			case V4L2_PIX_FMT_SBGGR14P:
			case V4L2_PIX_FMT_SBGGR14:
			case V4L2_PIX_FMT_SRGGB12P:
			case V4L2_PIX_FMT_SRGGB12:
			case V4L2_PIX_FMT_SBGGR12P:
			case V4L2_PIX_FMT_SBGGR12:
			case V4L2_PIX_FMT_SRGGB10P:
			case V4L2_PIX_FMT_SBGGR10P:
			case V4L2_PIX_FMT_SGBRG10P:
			case V4L2_PIX_FMT_SRGGB10: //or RG10
			case V4L2_PIX_FMT_SBGGR10: //or BG10
			case V4L2_PIX_FMT_SGBRG10: //or GB10
			case V4L2_PIX_FMT_SRGGB8:
			case V4L2_PIX_FMT_SBGGR8:
			case V4L2_PIX_FMT_SGBRG8:
			case V4L2_PIX_FMT_YUYV:
				imgConverted.type  = IMAGE_RGB;
				break;
			case V4L2_PIX_FMT_GREY:
			case V4L2_PIX_FMT_Y14P:
			case V4L2_PIX_FMT_Y12P:
			case V4L2_PIX_FMT_Y12:
			case V4L2_PIX_FMT_Y10:
			case V4L2_PIX_FMT_Y10P:
				imgConverted.type  = IMAGE_GREY;
				break;
			default: ee=-1; goto fail;
		}
		imgConverted.dx    = dx;
		imgConverted.dy    = dy;
		imgConverted.pitch = imgConverted.dx;
		imgConverted.st    = (U8*) malloc(3 * sizeof(U8) * imgConverted.dy * imgConverted.pitch);
		if(NULL==imgConverted.st){ee=-2; goto fail;}
		imgConverted.ccmp1 = imgConverted.st + 1 * sizeof(U8) * imgConverted.dy * imgConverted.pitch;
		imgConverted.ccmp2 = imgConverted.st + 2 * sizeof(U8) * imgConverted.dy * imgConverted.pitch;
	}

	switch(pixelformat)
	{
		case V4L2_PIX_FMT_GREY://8 bit grey image 
				rc =  copy_grey_to_image(&imgConverted,  st,  0, 0, dx, dy, dx, 0);
				if(rc<0){ee=-3+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_Y14P:
				rc =  convert_raw14_to_image(&imgConverted,  st, 0,  0, 0, dx, dy, dx, pitch - (14 * dx)/8);
				if(rc<0){ee=-4+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_Y12P:
				rc =  convert_raw12_to_image(&imgConverted,  st, 0,  0, 0, dx, dy, dx, pitch - (12 * dx)/8);
				if(rc<0){ee=-5+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_Y12:
				rc =  convert_16bit_to_image(&imgConverted,  st, dx, dy, dx, 12);
				if(rc<0){ee=-6+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_Y10:
				rc =  convert_16bit_to_image(&imgConverted,  st, dx, dy, dx, 10);
				if(rc<0){ee=-7+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_Y10P:
				rc =  convert_raw10_to_image(&imgConverted,  st, 0,  0, 0, dx, dy, dx, pitch - (10 * dx)/8);
				if(rc<0){ee=-8+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB14P:
		case V4L2_PIX_FMT_SBGGR14P:
				rc =  convert_raw_and_debayer_image(&imgConverted, st, pixelformat, 0,  0, 0, dx, dy, dx, pitch - (14 * dx)/8);
				if(rc<0){ee=-9+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB12P:
		case V4L2_PIX_FMT_SBGGR12P:
				rc =  convert_raw_and_debayer_image(&imgConverted, st, pixelformat, 0,  0, 0, dx, dy, dx, pitch - (12 * dx)/8);
				if(rc<0){ee=-10+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB10P:
		case V4L2_PIX_FMT_SBGGR10P:
		case V4L2_PIX_FMT_SGBRG10P:
				rc =  convert_raw_and_debayer_image(&imgConverted, st, pixelformat, 0,  0, 0, dx, dy, dx, pitch - (10 * dx)/8);
				if(rc<0){ee=-11+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB14:
		case V4L2_PIX_FMT_SBGGR14:
				rc =  convert_srggb_and_debayer_image(&imgConverted, st, pixelformat, dx, dy, dx, 14);
				if(rc<0){ee=-12+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB12:
		case V4L2_PIX_FMT_SBGGR12:
				rc =  convert_srggb_and_debayer_image(&imgConverted, st, pixelformat, dx, dy, dx, 12);
				if(rc<0){ee=-13+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB10: //or RG10
		case V4L2_PIX_FMT_SBGGR10: //or BG10
		case V4L2_PIX_FMT_SGBRG10: //or GB10
				rc =  convert_srggb_and_debayer_image(&imgConverted, st, pixelformat, dx, dy, dx, 10);
				if(rc<0){ee=-14+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB8:
		case V4L2_PIX_FMT_SBGGR8:
		case V4L2_PIX_FMT_SGBRG8:
				rc =  simple_debayer_to_image(&imgConverted, st, pixelformat, 0, 0, dx, dy, dx, 0);
				if(rc<0){ee=-15+100*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_YUYV:
				rc =  convert_yuyv_to_image(&imgConverted, st, 0, 0, dx, dy, dx, 0);
				if(rc<0){ee=-16+100*rc; goto fail;}
			break;
		default:
			printf("Error, Pixelformat unsupported: %c%c%c%c (0x%08x)\n",
					(char)((pixelformat >>  0)&0xFF),
					(char)((pixelformat >>  8)&0xFF),
					(char)((pixelformat >> 16)&0xFF),
					(char)((pixelformat >> 24)&0xFF),
					pixelformat);
			ee=-17; goto fail;
	}

	rc =  process_whitebalance(&imgConverted, cfgWB);
	if(rc<0){ee=-18+100*rc; goto fail;}

	#ifdef WITH_OPENCV
	{
		rc =  openCV_example(&imgConverted, &imgConverted);
		if(rc<0){ee=-19+100*rc; goto fail;}
	}
	#endif


	if(1==netSrvOutIff1)
	{
		// rc =  copy_image(&imgConverted, &(imgnetCfg->img));
		// if(rc<0){ee=-20+100*rc; goto fail;}
	}


	if(1==fileOutIff1)
	{	// Get the current time
		time_t current_time;
		struct tm *time_info;
		char timestamp[20]; // Adjust the size as needed

		time(&current_time);
		time_info = localtime(&current_time);

		// Format the timestamp
		strftime(timestamp, sizeof(timestamp), "%Y%m%d%H%M%S", time_info);
		strftime(mLED.timestamp, sizeof(mLED.timestamp), "%Y%m%d%H%M%S", time_info);//store the timestamp in this struct


		// Concatenate the timestamp with the filename
		// snprintf(acFilename, 255, "img%05d", frameNr);
		// snprintf(acFilename, sizeof(acFilename), "/home/pi/images/img%s_%05d", timestamp, frameNr);//change the path according to the board
		snprintf(acFilename, sizeof(acFilename), "/home/pi/images/img%s_%05d", mLED.timestamp, frameNr);//change the path according to the board

		// contains a filename of /home/pi/images/img20240312093045_00123

		rc =  write_image_as_pnm(acFilename, &imgConverted);
		mLED.counter++;//increase the counter of the image captured
		if(rc<0){ee=-22+100*rc; goto fail;}
	}


	ee=0;
fail:
	if(NULL!=imgConverted.st){ free(imgConverted.st);  imgConverted.st=NULL; }

	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Parses Command Line Parameters.
*
*  This function parses command line parameters.
-s: Shutter Time
Example: -s 5000

-g: Gain Value
Example: -g 10.5

-i: Number of Images
Example: -i 5

-f: Output Capture to Framebuffer
Example: -f

-a: Suppress ASCII capture at stdout
Example: -a

-o: Output Captures to file in PGM or PPM format
Example: -o

-b: Buffer Count to use
Example: -b 3

-p: Frames per seconds to request
Example: -p 30

-d: Video device ID
Example: -d 1

-r: Region of Interest
Example: -r '(0,0)/640x480'

-w: White Balance Settings as Triple (each [1..255])
Example: -w '123 145 167'

./yurisscope_cam -s 5000 -g 10.5 -i 5 -f -a -o -b 3 -p 30 -d 1 -r '(0,0)/640x480' -w '123 145 167'

sets the following values to the parameters in the program:

Shutter time: 5000
Gain value: 10.5
Number of images: 5
Output capture to framebuffer
Suppress ASCII capture at stdout
Output captures to file
Buffer count: 3
Frames per second: 30
Video device ID: 1
Region of interest: (0,0)/640x480
White balance settings: 123 (red), 145 (green), 167 (blue)
*/
/*-----------------------------------------------------------------------------*/
int  change_options_by_commandline(int argc, char *argv[], int *shutter, float *gain, int *maxCaptures, int *fileOutIff1, int *bufCount, int *fps)
{
	int  opt;

	cfgWB->mode = WBMODE_INACTIVE;
	// Hard coding parameters
	*fileOutIff1= 1; //Activating file output of captures
	// TODO:
	// *bufCount = NUM_BUFFERS//uncomment after testing and optimal num buffers is found
	// *maxCaptures= 128;//uncomment after testing
	while ((opt = getopt(argc, argv, "s:i:bp")) != -1) 
	{
		switch (opt) 
		{
			case 's':
				*shutter = atol(optarg);
				printf("Setting Shutter Value to %d.\n", *shutter);
				break;
			case 'i':
				*maxCaptures = atol(optarg);
				printf("Take %d images.\n", *maxCaptures);
				break;
			case 'b':
				*bufCount = atol(optarg);
				printf("Setting Buffer Count to %d.\n", *bufCount);
				break;
			case 'p':
				*fps = atol(optarg);
				printf("Setting FPS Request to %d.\n", *fps);
				break;
			default:
				// Default case for handling invalid options or options not listed in the switch
				printf("Usage: %s [-s shutter] [-i maxCaptures] [-b bufCount] [-p fps]\n", argv[0]);
				printf("Options:\n");
				printf("  -s <shutter>: Set shutter time(exposure time) in microseconds.\n");
				printf("  -i <maxCaptures>: Number of images to capture.\n");
				printf("  -b <bufCount>: Set buffer count.\n");
				printf("  -p <fps>: Set frames per second.\n");
				printf("Example usage:  ./yuriscope_cam -s 5000 -i 5 -b 3 -p 30\n");
				return 1; // Returning a non-zero value to indicate an error
		}
	}

	if (argc < 2) {
    printf("Hint: Activate framebuffer output by command line option (see: %s -? )\n", argv[0]);
	}
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Connects to a running vcimgnetsrv.
*
*  This function connects to a running vcimgnetsrv.
*  Normally the vcimgnetsrv application is started beforehand in background
*  using the following command:
*
*   vcimgnetsrv &
*/
/*-----------------------------------------------------------------------------*/
// int  imgnet_connect(VCImgNetCfg *imgnetCfg, U32 pixelformat, int dx, int dy)
// {
// 	int  rc, ee;

// 	//predefine dimensions for the image to be transferred
// 	switch(pixelformat)
// 	{
// 		case V4L2_PIX_FMT_SRGGB14P:
// 		case V4L2_PIX_FMT_SRGGB14:
// 		case V4L2_PIX_FMT_SBGGR14P:
// 		case V4L2_PIX_FMT_SBGGR14:
// 		case V4L2_PIX_FMT_SRGGB12P:
// 		case V4L2_PIX_FMT_SRGGB12:
// 		case V4L2_PIX_FMT_SBGGR12P:
// 		case V4L2_PIX_FMT_SBGGR12:
// 		case V4L2_PIX_FMT_SRGGB10P:
// 		case V4L2_PIX_FMT_SBGGR10P:
// 		case V4L2_PIX_FMT_SGBRG10P:
// 		case V4L2_PIX_FMT_SRGGB10: //or RG10
// 		case V4L2_PIX_FMT_SBGGR10: //or BG10
// 		case V4L2_PIX_FMT_SGBRG10: //or GB10
// 		case V4L2_PIX_FMT_SRGGB8:
// 		case V4L2_PIX_FMT_SBGGR8:
// 		case V4L2_PIX_FMT_SGBRG8:
// 		case V4L2_PIX_FMT_YUYV:
// 			imgnetCfg->img.type = IMAGE_RGB;
// 			break;
// 		case V4L2_PIX_FMT_GREY:
// 		case V4L2_PIX_FMT_Y14P:
// 		case V4L2_PIX_FMT_Y12P:
// 		case V4L2_PIX_FMT_Y12:
// 		case V4L2_PIX_FMT_Y10:
// 		case V4L2_PIX_FMT_Y10P:
// 			imgnetCfg->img.type = IMAGE_GREY;
// 			break;
// 		default: ee=-1; goto fail;
// 	}
// 	imgnetCfg->img.dx    = dx;
// 	imgnetCfg->img.dy    = dy;
// 	imgnetCfg->img.pitch = imgnetCfg->img.dx;
// 	imgnetCfg->img.st    = NULL;
// 	imgnetCfg->img.ccmp1 = NULL;
// 	imgnetCfg->img.ccmp2 = NULL;

// 	rc =  vcimgnet_attach(&(imgnetCfg->img), imgnetCfg);
// 	if(rc<0){ee=-2+10*rc; goto fail;}

// 	ee=0;
// fail:
// 	return ee;
// }





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Disconnects from a running vcimgnetsrv.
*
*  This function disconnects from a running vcimgnetsrv.
*  The vcimgnetsrv application will keep running.
*/
/*-----------------------------------------------------------------------------*/
// int  imgnet_disconnect(VCImgNetCfg *imgnetCfg)
// {
// 	return(vcimgnet_detach(imgnetCfg));
// }





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Tells the Sensor to Start Streaming Recordings.
*
*  This function tells the sensor to start streaming recordings.
*  To get access to the streamed images
*  buffers (which will be filled by the stream) must be enqueued,
*  waited until one is filled, and dequeued.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_streaming_start(VCMipiSenCfg *sen)
{
	I32                 ee, rc;


	rc =  ioctl(sen->fd, VIDIOC_STREAMON, &sen->format.type);
	if(rc<0){ee=-1; goto fail;}


	ee=0;
fail:
	switch(ee)
	{
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_STREAMON) throws Error!\n", __FUNCTION__);
			break;
	}
	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Tells the Sensor to Stop Streaming Recordings.
*
*  This function tells the sensor to stop streaming recordings.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_streaming_stop(VCMipiSenCfg *sen)
{
	I32                 ee, rc;


	rc =  ioctl(sen->fd, VIDIOC_STREAMOFF, &sen->format.type);
	if(rc<0){ee=-1; goto fail;}


	ee=0;
fail:
	switch(ee)
	{
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_STREAMOFF) throws Error!\n", __FUNCTION__);
			break;
	}
	return(ee);
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Opens the Capture Device and Retreives its Attributes.
*
*  This function opens the capture device and retreives its attributes.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_open(char *dev_video_device, VCMipiSenCfg *sen, unsigned int qbufCount)
{
	I32    ee, rc;
	U32    i, j;
	I32    ifc;


	// Reset Allocation Markers
	// prohibits closing of un-open device and prevents wrong deallocation or unmapping.
	{
		sen->fd        =   -1;
		sen->qbuf      = NULL;
		sen->qbufCount =   -1;
	}


	// Open the Device.
	{
		sen->fd =  open(dev_video_device, O_RDWR, 0);
		if(sen->fd<0){ee=-1; goto fail;}
	}


	// Check the Capabilities of the Device.
	{
		struct v4l2_capability  cap;

		rc =  ioctl(sen->fd, VIDIOC_QUERYCAP, &cap);
		if(rc<0){ee=-2; goto fail;}

		// Check if device can Capture Videos and supports mmap access.
		if(0==(cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)))
		{ee=-3; goto fail;}

		syslog(LOG_DEBUG, "%s:  Initialized Capture Device '%s' (fd:%d):\n", __FILE__, dev_video_device, sen->fd);
	}


	// Retreive Dimensions of the Camera Device.
	{
		for(ifc= 0; ifc< 2; ifc++)
		{
			switch(ifc)
			{
				case 0: //First try:  try non-multiplane interface
				{
					sen->format.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					sen->format.fmt.pix.width       = UINT_MAX;
					sen->format.fmt.pix.height      = UINT_MAX;
					// sen->format.fmt.pix.pixelformat = V4L2_PIX_FMT_Y10P;
					sen->format.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;//8-bit greyscale
				}
				break;
				case 1: //Second try:  multiplane interface
				{
					sen->format.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
					sen->format.fmt.pix_mp.width       = UINT_MAX;
					sen->format.fmt.pix_mp.height      = UINT_MAX;
					sen->format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_Y10P;
				}
				break;
				default: ee=-4; goto fail;
			}

			rc = ioctl(sen->fd, VIDIOC_G_FMT, &(sen->format));
			if(rc<0){ continue; }


			// Trying to Request an Unpacked Format Variant if Format is Packed
			{
				switch(sen->format.type)
				{
					case V4L2_BUF_TYPE_VIDEO_CAPTURE:
					{
						switch(sen->format.fmt.pix.pixelformat)
						{
							case V4L2_PIX_FMT_Y10P:      sen->format.fmt.pix.pixelformat = V4L2_PIX_FMT_Y10;      break;
							case V4L2_PIX_FMT_SRGGB10P:  sen->format.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;  break;
							case V4L2_PIX_FMT_SGBRG10P:  sen->format.fmt.pix.pixelformat = V4L2_PIX_FMT_SGBRG10;  break;
							default: break;
						}
					}
					break;
					case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
					{
						switch(sen->format.fmt.pix_mp.pixelformat)
						{
							case V4L2_PIX_FMT_Y10P:      sen->format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_Y10;      break;
							case V4L2_PIX_FMT_SRGGB10P:  sen->format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SRGGB10;  break;
							case V4L2_PIX_FMT_SGBRG10P:  sen->format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SGBRG10;  break;
							default: break;
						}
					}
					break;
					default: break;
				}

				rc = ioctl(sen->fd, VIDIOC_S_FMT, &(sen->format));
				if(rc<0){ syslog(LOG_DEBUG, "%s:    Request for not-packed Pixel Format rejected.\n", __FILE__); }
			}


			switch(sen->format.type)
			{
				case V4L2_BUF_TYPE_VIDEO_CAPTURE:
				{
					syslog(LOG_DEBUG, "%s:    Pixel Format used:       '%c%c%c%c' (0x%08x)       \n", __FILE__,
					(char)((sen->format.fmt.pix.pixelformat >>  0)&0xFF),
					(char)((sen->format.fmt.pix.pixelformat >>  8)&0xFF),
					(char)((sen->format.fmt.pix.pixelformat >> 16)&0xFF),
					(char)((sen->format.fmt.pix.pixelformat >> 24)&0xFF),
					sen->format.fmt.pix.pixelformat);
					syslog(LOG_DEBUG, "%s:    Maximum Pixel Width:      %d          \n", __FILE__, sen->format.fmt.pix.width       );
					syslog(LOG_DEBUG, "%s:    Maximum Pixel Height:     %d          \n", __FILE__, sen->format.fmt.pix.height      );
					syslog(LOG_DEBUG, "%s:    Bytes Per Line:           %d          \n", __FILE__, sen->format.fmt.pix.bytesperline);
				}
				break;
				case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
				{
					syslog(LOG_DEBUG, "%s:    Pixel Format used:       '%c%c%c%c' (0x%08x)       \n", __FILE__,
					(char)((sen->format.fmt.pix_mp.pixelformat >>  0)&0xFF),
					(char)((sen->format.fmt.pix_mp.pixelformat >>  8)&0xFF),
					(char)((sen->format.fmt.pix_mp.pixelformat >> 16)&0xFF),
					(char)((sen->format.fmt.pix_mp.pixelformat >> 24)&0xFF),
					sen->format.fmt.pix_mp.pixelformat);
					syslog(LOG_DEBUG, "%s:    Maximum Pixel Width:      %d          \n", __FILE__, sen->format.fmt.pix_mp.width       );
					syslog(LOG_DEBUG, "%s:    Maximum Pixel Height:     %d          \n", __FILE__, sen->format.fmt.pix_mp.height      );
					syslog(LOG_DEBUG, "%s:    Number of Planes:         %d          \n", __FILE__, sen->format.fmt.pix_mp.num_planes  );
					for(j= 0; j< sen->format.fmt.pix_mp.num_planes; j++)
						syslog(LOG_DEBUG, "%s:    Plane %d: Bytes Per Line: %d          \n", __FILE__, j, sen->format.fmt.pix_mp.plane_fmt[j].bytesperline);
				}
				break;
				default: ee=-5; goto fail;
			}

			break; //This break ends the for-loop, since a working solution has been found.
		}
	}


	// Allocate Capture Buffer Pointer 'Array'
	{
		QBuf  imgBufNuller = NULL_QBuf;

		sen->qbuf =  (QBuf*) malloc(sizeof(QBuf) * qbufCount);
		if(NULL==sen->qbuf){ee=-6; goto fail;}

		for(i= 0; i< qbufCount; i++)
		{
			sen->qbuf[i] = imgBufNuller; //prevents wrong deallocation
		}

		// Allocate Hardware writeable data region pointers for each Capture Buffer
		for(i= 0; i< qbufCount; i++)
		{
			switch(sen->format.type)
			{
				case V4L2_BUF_TYPE_VIDEO_CAPTURE:          sen->qbuf[i].planeCount = 1;                                   break;
				case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:   sen->qbuf[i].planeCount = sen->format.fmt.pix_mp.num_planes;   break;
				default: ee=-7; goto fail;
			}

			sen->qbuf[i].plane = (struct v4l2_plane*) malloc(sizeof(struct v4l2_plane) * sen->qbuf[i].planeCount);
			if(NULL==sen->qbuf[i].plane){ee=-8; goto fail;}

			for(j= 0; j< sen->qbuf[i].planeCount; j++)
			{
				memset(&(sen->qbuf[i].plane[j]), 0, sizeof(struct v4l2_plane));
			}

			sen->qbuf[i].st    = (char**) malloc(sizeof(char *) * sen->qbuf[i].planeCount);
			if(NULL==sen->qbuf[i].st){ee=-9; goto fail;}

			for(j= 0; j< sen->qbuf[i].planeCount; j++)
			{
				sen->qbuf[i].st[j] = NULL;
			}
		}
	}

	// Request the Count of Capture Buffers
	{
		struct v4l2_requestbuffers  req;

		memset(&req, 0, sizeof(struct v4l2_requestbuffers));

		req.memory = V4L2_MEMORY_MMAP;
		req.count  = qbufCount;
		req.type   = sen->format.type;

		rc =  ioctl(sen->fd, VIDIOC_REQBUFS, &req);
		if(rc<0){ee=-11; goto fail;}

		if(req.count != qbufCount){ee=-12; goto fail;}
	}

	// Map the capture buffer memory addresses to the Capture Buffer Pointer 'Array'
	{
		struct v4l2_buffer  buf;

		sen->qbufCount = 0;
		for(i= 0; i< qbufCount; i++)
		{
			memset(&buf, 0, sizeof(struct v4l2_buffer));
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index  = sen->qbufCount;
			buf.type   = sen->format.type;

			switch(sen->format.type)
			{
				case V4L2_BUF_TYPE_VIDEO_CAPTURE:
				{
					rc =  ioctl(sen->fd, VIDIOC_QUERYBUF, &buf);
					if(rc<0){ee=-15; goto fail;}

					sen->qbuf[i].st[0]    = (char*) mmap(NULL, buf.length,             PROT_READ | PROT_WRITE, MAP_SHARED, sen->fd, buf.m.offset                );
					if(MAP_FAILED==sen->qbuf[i].st[0]){ee=-16; goto fail;}

					sen->qbuf[i].plane[0].length = buf.length;
				}
				break;
				case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
				{
					struct v4l2_plane *planes = NULL;

					planes = (struct v4l2_plane*) malloc(sizeof(struct v4l2_plane) * sen->qbuf[i].planeCount);
					if(NULL==planes){ee=-13; goto fail;}

					buf.length   = sen->qbuf[i].planeCount;
					buf.m.planes = planes;

					rc =  ioctl(sen->fd, VIDIOC_QUERYBUF, &buf);
					if(rc<0){ee=-15; goto fail;}

					///////////////////////
					for(j= 0; j< sen->qbuf[i].planeCount; j++)
					{
						sen->qbuf[i].plane[j] = buf.m.planes[j];

						sen->qbuf[i].st[j]    = (char*) mmap(NULL, buf.m.planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, sen->fd, buf.m.planes[j].m.mem_offset);
						if(MAP_FAILED==sen->qbuf[i].st[j]){ee=-16; goto fail;}
					}
					///////////////////////

					if(NULL!=planes){ free(planes);  planes = NULL; }
				}
				break;
				default: ee=-14; goto fail;
			}

			sen->qbufCount++;
		}
	}


	ee = 0;
fail:
	if(ee<0)
	{
		sensor_close(sen);
	}
	switch(ee)
	{
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "%s():  Could not open device '%s' for Reading/Writing!\n", __FUNCTION__, dev_video_device);
			break;
		case -2:
			if(errno == EINVAL)
				syslog(LOG_ERR, "%s():  Device '%s' is no V4L2 Device!\n", __FUNCTION__, dev_video_device);
			else
				syslog(LOG_ERR, "%s():  ioctl(VIDIOC_QUERYCAP) throws Error on Device '%s'!\n", __FUNCTION__, dev_video_device);
			break;
		case -3:
			syslog(LOG_ERR, "%s():  Device '%s' is no Capture Device or doesn't support mmap as IO Method!\n", __FUNCTION__, dev_video_device);
			break;
		case -4:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_G_FMT) throws Error on Device '%s'!\n", __FUNCTION__, dev_video_device);
			break;
		case -5:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_REQBUFS) throws Error on Device '%s'!\n", __FUNCTION__, dev_video_device);
			break;
		case -6:
			syslog(LOG_ERR, "%s():  Requested buffers differ from returned buffers!\n", __FUNCTION__);
			break;
		case -7:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_QUERYBUF) throws Error on Device '%s'!\n", __FUNCTION__, dev_video_device);
			break;
		case -8:
			syslog(LOG_ERR, "%s():  mmap() failed for Buffer %d!\n", __FUNCTION__, sen->qbufCount);
			break;
		case -99:
			syslog(LOG_ERR, "%s():  Out of Memory!\n", __FUNCTION__);
			break;
		default:
			syslog(LOG_ERR, "%s():  Unknown error code: %d,  errno: %d (%s)!\n", __FUNCTION__, ee, errno, strerror(errno));
			break;
	}

	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Closes the Capture Device.
*
*  This function closes the capture device.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_close(VCMipiSenCfg *sen)
{
	I32  ee, rc;
	U32  i, j;

	// checks if device has been opened (see sensor_open() and NULL_VCMipiSenCfg)
	if(sen->fd>=0)
	{
		if(NULL!=sen->qbuf)
		{
			for(i=sen->qbufCount; i> 0; i--)
			{
				if(NULL!=sen->qbuf[i-1].plane)
				{
					for(j= 0; j< sen->qbuf[i-1].planeCount; j++)
					{
						rc =  munmap(sen->qbuf[i-1].st[j], sen->qbuf[i-1].plane[j].length);
						if(rc<0){ee=-1-10*i; goto fail;}
					}

					free(sen->qbuf[i-1].plane);
					sen->qbuf[i-1].plane = NULL;
				}
				if(NULL!=sen->qbuf[i-1].st){ free(sen->qbuf[i-1].st);  sen->qbuf[i-1].st = NULL; }

				sen->qbufCount--;
			}

			free(sen->qbuf);
			sen->qbuf = NULL;
		}

		// Close Video Device.
		{
			rc =  close(sen->fd);
			if(rc<0){return -2;}

			sen->fd = -1;
		}
	}

	ee=0;
fail:
	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Enqueues a Buffer into the Capture Queue.
*
*  This function enqueues a buffer into the capture queue.
*
*  If the sensor acquires images, e.g. by being in streaming mode,
*  buffers enqueued at the capture queue will be filled with recorded data.
*  After successful waiting for a recording,
*  the image can be accessed by dequeuing the buffer from the capture queue.
*/
/*-----------------------------------------------------------------------------*/
int  capture_buffer_enqueue(I32 bufIdx, VCMipiSenCfg *sen)
{
	I32                 ee, rc;
	struct v4l2_buffer  buf;

	memset(&buf, 0, sizeof(struct v4l2_buffer));
	buf.memory   = V4L2_MEMORY_MMAP;
	buf.type     = sen->format.type;
	buf.index    = bufIdx;
	buf.length   = sen->qbuf[bufIdx].planeCount;
	buf.m.planes = sen->qbuf[bufIdx].plane;

	rc =  ioctl(sen->fd, VIDIOC_QBUF, &buf);
	if(rc<0){ee=-9; goto fail;}

	ee = 0;
fail:
	switch(ee)
	{
		case 0:
			break;
		case -9:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_QBUF) throws Error,  errno: %d (%s)!\n", __FUNCTION__, errno, strerror(errno));
			break;
		default:
			syslog(LOG_ERR, "%s():  Unknown error code: %d,  errno: %d (%s)!\n", __FUNCTION__, ee, errno, strerror(errno));
			break;
	}

	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Dequeues a Buffer from the Capture Queue.
*
*  This function dequeues a buffer from the capture queue.
*
*  If the sensor acquires images, e.g. by being in streaming mode,
*  buffers enqueued at the capture queue will be filled with recorded data.
*  After successful waiting for a recording,
*  the image can be accessed by dequeuing the buffer from the capture queue.
*/
/*-----------------------------------------------------------------------------*/
int  capture_buffer_dequeue(I32 bufIdx, VCMipiSenCfg *sen)
{
	I32                 ee, rc;
	struct v4l2_buffer  buf;

	memset(&buf, 0, sizeof(struct v4l2_buffer));
	buf.memory   = V4L2_MEMORY_MMAP;
	buf.type     = sen->format.type;
	buf.index    = bufIdx;
	buf.length   = sen->qbuf[bufIdx].planeCount;
	buf.m.planes = sen->qbuf[bufIdx].plane;

	rc =  ioctl(sen->fd, VIDIOC_DQBUF, &buf);
	if(rc<0)
	{
		if(EAGAIN==errno){ee=+1; goto fail;} // Buffer is not available.
		else             {ee=-1; goto fail;}
	}

	if(buf.flags & V4L2_BUF_FLAG_QUEUED)
	{
		ee=+2; goto fail;
	}


	ee = 0;
fail:
	switch(ee)
	{
		case +2:
			syslog(LOG_ERR, "%s():  Buffer not ready.\n", __FUNCTION__);
			break;
		case +1:
			syslog(LOG_ERR, "%s():  Buffer not available.\n", __FUNCTION__);
			break;
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_DQBUF) throws Error!\n", __FUNCTION__);
			break;
		case -2:
			syslog(LOG_ERR, "%s():  Buffer index returned exceeds limits: %d>=%d!\n", __FUNCTION__, buf.index, sen->qbufCount);
			break;
		default:
			syslog(LOG_ERR, "%s():  Unknown error code: %d!\n", __FUNCTION__, ee);
			break;
	}

	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Waits for a Buffer at the Capture Queue to be filled with captured Data.
*
*  This function waits for a buffer at the capture queue to be filled with
*  data of a recording.
*
*  If the sensor acquires images, e.g. by being in streaming mode,
*  buffers enqueued at the capture queue will be filled with recorded data.
*  After successful waiting for a recording,
*  the image can be accessed by dequeuing the buffer from the capture queue.
*/
/*-----------------------------------------------------------------------------*/
int  sleep_for_next_capture(VCMipiSenCfg  *sen, int timeoutUS)
{
	I32             ee, rc;
	fd_set          fdSet;
	struct timeval  tv;

	while(1)
	{
		FD_ZERO(&fdSet);
		FD_SET(sen->fd, &fdSet);
		tv.tv_sec  = (timeoutUS/1000000);
		tv.tv_usec = (timeoutUS%1000000);

		// Wait for data.
		rc =  select(sen->fd + 1, &fdSet, NULL, NULL, &tv);
		if(rc<0)
		{
			// Ignore interrupt based select returns.
			if(EINTR==errno){continue;        }
			else            {ee=-1; goto fail;}
		}
		if(0==rc){ee=+1; goto fail;} //select() timeout.

		// New data is available.
		break;
	}


	ee = 0;
fail:
	switch(ee)
	{
		case +1:
			//syslog(LOG_ERR, "%s():  select() timeout.\n", __FUNCTION__);
			break;
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "%s():  select() failed!\n", __FUNCTION__);
			break;
	}

	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Requests new Exposure and Gain Settings to the Sensor Device.
*
*  This function requests new Exposure and Gain Settings to the Sensor Device.
*  When the settings become operational depends on the sensor and its configuration.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_set_shutter_gain(VCMipiSenCfg  *sen, int newGain, int newShutter)
{
	I32    ee, rc, target, getOrSet;
	U32    val;
	char   a10cTarget[11];
	struct v4l2_queryctrl  queryctl;

	struct v4l2_ext_controls ext_ctls;
	struct v4l2_ext_control  ext_ctl;


	for(target= 0; target< 2; target++)
	{
		switch(target)
		{
			case 0:  sprintf(a10cTarget,"Gain"    );  val = newGain;     break; // string for  V4L2_CID_GAIN;
			case 1:  sprintf(a10cTarget,"Exposure");  val = newShutter;  break; // string for  V4L2_CID_EXPOSURE;
		}

		// Due to proprietary NVidia IDs we have to review the ID by its name.
		{
			queryctl.id=V4L2_CTRL_FLAG_NEXT_CTRL;
			while(0==ioctl(sen->fd, VIDIOC_QUERYCTRL, &queryctl))
			{
				if(!(queryctl.flags & V4L2_CTRL_FLAG_DISABLED))
				{
					if(0==strcmp(a10cTarget, (char*)queryctl.name))
					{
						goto foundId;
					}
				}
				queryctl.id|=V4L2_CTRL_FLAG_NEXT_CTRL;
			}

			//ID text not found, so not present.
			ee=-2; goto fail;
		}
foundId:

		for(getOrSet= 0; getOrSet< 3; getOrSet++)
		{
			switch(getOrSet)
			{
				case 0:
				case 2:
				// Only needed for debugging: Get old value.
				{
					memset(&ext_ctl, 0, sizeof(ext_ctl));
					ext_ctl.id = queryctl.id;

					ext_ctls.ctrl_class = queryctl.type;
					ext_ctls.count      = 1;
					ext_ctls.controls   = &ext_ctl;

					rc =  ioctl(sen->fd, VIDIOC_G_EXT_CTRLS, &ext_ctls);
					if(rc<0)
					{
						if(EINVAL!=errno){ee=-1; goto fail;} //general error.
						else             {ee=-2; goto fail;} //unsupported.
					}

					if(V4L2_CTRL_TYPE_INTEGER64==ext_ctls.ctrl_class)
					{
						syslog(LOG_DEBUG, "%s():  %s %s Value: %lld.\n", __FUNCTION__, a10cTarget, (0==getOrSet)?("Old"):("New"), ext_ctl.value64);
					}
					else
					{
						syslog(LOG_DEBUG, "%s():  %s %s Value: %d.\n",   __FUNCTION__, a10cTarget, (0==getOrSet)?("Old"):("New"), ext_ctl.value  );
					}
				}
				break;
				case 1:
				// Set new value.
				{
					memset(&ext_ctl, 0, sizeof(ext_ctl));
					ext_ctl.id = queryctl.id;
					if(V4L2_CTRL_TYPE_INTEGER64==queryctl.type)
					{
						ext_ctl.value64 = val;
					}
					else
					{
						ext_ctl.value   = val;
					}

					ext_ctls.ctrl_class = queryctl.type;
					ext_ctls.count      = 1;
					ext_ctls.controls   = &ext_ctl;

					rc = ioctl(sen->fd, VIDIOC_S_EXT_CTRLS, &ext_ctls);
					if(rc<0)
					{
						if((EINVAL!=errno)&&(ERANGE!=errno)){ee=-3; goto fail;} //general error.
						else                                {ee=-4; goto fail;} //Value out of Range Error.
					}

					syslog(LOG_DEBUG, "%s():  Requested New %s Value: %d.\n", __FUNCTION__, a10cTarget, val);
				}
				break;
				default:ee=-5; goto fail;
			}
		}
	}


	ee = 0;
fail:
	switch(ee)
	{
		case 0:
			break;
		case -1:
		case -3:
			syslog(LOG_ERR, "%s():  ioctl(%s) throws Error (%d(%s))!\n", __FUNCTION__, (-3==ee)?("VIDIOC_S_CTRL"):("VIDIOC_G_CTRL"), errno, strerror(errno));
			break;
		case -2:
			syslog(LOG_ERR, "%s():  V4L2_CID_.. is unsupported!\n", __FUNCTION__);
			break;
		case -4:
			syslog(LOG_ERR, "%s():  %s Value is out of range (or V4L2_CID_.. is invalid)!\n", __FUNCTION__, a10cTarget);
			break;
		case -5:
			syslog(LOG_ERR, "%s():  Range Error!\n", __FUNCTION__);
			break;
	}

	return(ee);
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Requests an approximate count of frames per second from the Sensor Device.
*
*  This function requests an approximate count of frames per second from the Sensor Device
*  Due to the nature and capabilities of the sensor, the requested frames per second
*  may not be exactly the value requested. The matching accuracy can be influenced
*  by the exposure time used.
*  When the settings become operational depends on the sensor and its configuration.
*/
/*-----------------------------------------------------------------------------*/
int  sensor_set_fps(VCMipiSenCfg *sen, I32 fps)
{
	I32    ee, rc;


	// Check the Capability of Sensor to set Timeframe
	{
		struct v4l2_streamparm  par;

		memset(&par, 0, sizeof(struct v4l2_streamparm));

		par.type = sen->format.type;

		rc =  ioctl(sen->fd, VIDIOC_G_PARM, &par);
		if(rc<0){ee=-1; goto fail;}

		if(!(par.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
		{
			ee=+1; goto fail;
		}
	}

	/*
	 * Temporary deactivated ....
	 *
	// Get list of supported Frame Interval Lengths (which is 1/fps)
	{
		struct v4l2_frmivalenum  frm;

		memset(&frm, 0, sizeof(struct v4l2_frmivalenum));

		// There are two ways the driver communicates the frame intervals,
		// discrete or sequential. The index value of zero is valid for both
		// ways, and the answer returns the way how the frame interavals are presented.

		frm.width        = sen->format.fmt.pix.width;
		frm.height       = sen->format.fmt.pix.height;
		frm.pixel_format = sen->format.fmt.pix.pixelformat;

		frm.index        = 0;

		rc =  ioctl(sen->fd, VIDIOC_ENUM_FMT, &frm);
		if(rc<0){ee=-2; goto fail;}

		switch(frm.type)
		{
			case V4L2_FRMIVAL_TYPE_DISCRETE:
			{
				do
				{
					syslog(LOG_DEBUG, "%s:  Sensor supports Frame Interval Time  '%u/%us (approx. %fs)'\n", __FILE__, frm.discrete.numerator, frm.discrete.denominator, ((float)frm.discrete.numerator)/(max(1,frm.discrete.denominator)));

					frm.index++; // Request next list element

					rc =  ioctl(sen->fd, VIDIOC_ENUM_FMT, &frm);
					if(rc<0)
					{
						if(EINVAL==errno){ break; } // Indicates End of List
						else { ee=-3; goto fail;}
					}

				} while(1);
			}
			break;
			case V4L2_FRMIVAL_TYPE_STEPWISE:
			case V4L2_FRMIVAL_TYPE_CONTINUOUS: // identical with stepwise but step is 1
			{
				syslog(LOG_DEBUG, "%s:  Sensor supports Frame Interval Time:\n", __FILE__);
				syslog(LOG_DEBUG, "%s:   from %u/%us (approx. %fs)'\n", __FILE__, frm.stepwise.min.numerator, frm.stepwise.min.denominator, ((float)frm.stepwise.min.numerator)/(max(1,frm.stepwise.min.denominator)));
				syslog(LOG_DEBUG, "%s:   to   %u/%us (approx. %fs)'\n", __FILE__, frm.stepwise.max.numerator, frm.stepwise.max.denominator, ((float)frm.stepwise.max.numerator)/(max(1,frm.stepwise.max.denominator)));
				syslog(LOG_DEBUG, "%s:   step %u/%us (approx. %fs)'\n", __FILE__, frm.stepwise.step.numerator, frm.stepwise.step.denominator, ((float)frm.stepwise.step.numerator)/(max(1,frm.stepwise.step.denominator)));
			}
			break;
			default: ee=-4; goto fail;
		}
	}
	*/

	// Apply the best match of frame interval length based on the frames per seconds given
	{
		struct v4l2_fract       tpf = {0, 0};
		struct v4l2_streamparm  par;

		if(fps<=0){ee=-5; goto fail;}

		tpf.numerator   = 1;
		tpf.denominator = fps;

		memset(&par, 0, sizeof(struct v4l2_streamparm));

		par.type = sen->format.type;
		par.parm.capture.timeperframe = tpf;
		syslog(LOG_DEBUG, "%s:  Requested FPS: %d, Best Match in Time Per Frame: %d/%d  (is %f FPS).\n", __FILE__, fps, par.parm.capture.timeperframe.numerator, par.parm.capture.timeperframe.denominator, (0==par.parm.capture.timeperframe.numerator)?(0.0f):((float)par.parm.capture.timeperframe.denominator/par.parm.capture.timeperframe.numerator));

		rc =  ioctl(sen->fd, VIDIOC_S_PARM, &par);
		if(rc<0){ee=-6; goto fail;}

		rc =  ioctl(sen->fd, VIDIOC_G_PARM, &par);
		if(rc<0){ee=-7; goto fail;}

		syslog(LOG_DEBUG, "%s:  FPS reported: %d/%d  (is %f FPS).\n", __FILE__, par.parm.capture.timeperframe.numerator, par.parm.capture.timeperframe.denominator, (0==par.parm.capture.timeperframe.numerator)?(0.0f):((float)par.parm.capture.timeperframe.denominator/par.parm.capture.timeperframe.numerator));
	}

	ee=0;
fail:
	switch(ee)
	{
		case 0:
			break;
		case +1:
			syslog(LOG_WARNING, "%s():  Sensor does not support setting frames per second!\n", __FUNCTION__);
			break;
		case -1:
			syslog(LOG_ERR, "%s():  ioctl(VIDIOC_STREAMON) throws Error %d: %s!\n", __FUNCTION__, errno, strerror(errno));
			break;
	}
	return(ee);
}

/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Requests new Cropping ROI for the Sensor Device.
*
*  This function requests a new Cropping region of interest the Sensor Device.
*  When the settings become operational depends on the sensor and its configuration.
*/
/*-----------------------------------------------------------------------------*/
// int  sensor_set_cropping_roi(VCMipiSenCfg  *sen, int newX0, int newY0, int newWidth, int newHeight)
// {
// 	I32    ee, rc;
// 	struct v4l2_selection  sel_ctl;

// 	if((newX0<0)||(newY0<0)||(newWidth<1)||(newHeight<1)){ee=+1; goto fail;}

// 	// Only needed for debugging: Get old value.
// 	{
// 		memset(&sel_ctl, 0, sizeof(sel_ctl));
// 		sel_ctl.type = sen->format.type;
// 		sel_ctl.target = V4L2_SEL_TGT_CROP;

// 		rc =  ioctl(sen->fd, VIDIOC_G_SELECTION, &sel_ctl);
// 		if(rc<0)
// 		{
// 			if(EINVAL!=errno){ee=-1; goto fail;} //general error.
// 			else             {ee=-2; goto fail;} //unsupported.
// 		}

// 		syslog(LOG_DEBUG, "%s():  Cropping rectangle returned by sensor:  (x0,y0):(%d,%d) (dx,dy):(%d,%d).\n", __FUNCTION__, sel_ctl.r.left, sel_ctl.r.top, sel_ctl.r.width, sel_ctl.r.height);
// 	}
// 	// Set new value.
// 	{
// 		memset(&sel_ctl, 0, sizeof(sel_ctl));
// 		sel_ctl.type     = sen->format.type;
// 		sel_ctl.target   = V4L2_SEL_TGT_CROP;
// 		sel_ctl.r.left   = newX0;
// 		sel_ctl.r.top    = newY0;
// 		sel_ctl.r.width  = newWidth;
// 		sel_ctl.r.height = newHeight;
// 		sel_ctl.flags    = V4L2_SEL_FLAG_LE | V4L2_SEL_FLAG_GE;

// 		rc = ioctl(sen->fd, VIDIOC_S_SELECTION, &sel_ctl);
// 		if(rc<0)
// 		{
// 			if((EINVAL!=errno)&&(ERANGE!=errno)){ee=-3; goto fail;} //general error.
// 			else                                {ee=-4; goto fail;} //rectangle invalid Error.
// 		}

// 		syslog(LOG_DEBUG, "%s():  New cropping rectangle returned by sensor:  (x0,y0):(%d,%d) (dx,dy):(%d,%d).\n", __FUNCTION__, sel_ctl.r.left, sel_ctl.r.top, sel_ctl.r.width, sel_ctl.r.height);
// 	}


// 	ee = 0;
// fail:
// 	switch(ee)
// 	{
// 		case 0:
// 			break;
// 		case -1:
// 		case -3:
// 			syslog(LOG_ERR, "%s():  ioctl(%s) throws Error (%d(%s))!\n", __FUNCTION__, (-3==ee)?("VIDIOC_S_SELECTION"):("VIDIOC_G_SELECTION"), errno, strerror(errno));
// 			break;
// 		case -2:
// 			syslog(LOG_ERR, "%s():  V4L2_SEL_TGT_CROP is unsupported!\n", __FUNCTION__);
// 			break;
// 		case -4:
// 			syslog(LOG_ERR, "%s():  Rectangle is not applicable!\n", __FUNCTION__);
// 			break;
// 	}

// 	return(ee);
// }


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Applies or Determines White Balance Amplifications.
*
*  This function applies or determines white balance amplification values
*  depending on the mode requested.
*
*    Run a white balance calculation for an image of type IMAGE_RGB.
*    For mode WBMODE_MEASURE record an image showing a homogeneously
*    white surface and change the shutter value to get a maximum
*    output value of around 200.
*/
/*-----------------------------------------------------------------------------*/
I32  process_whitebalance(image *img, VCWhiteBalCfg *cfgWB)
{
	I32   x, y, r, g, b, R, G, B, max;
	U8    *pR, *pG, *pB;

	if(IMAGE_RGB != img->type)
	{
		return +1; //nothing that can be done.
	}

	switch(cfgWB->mode)
	{
		case WBMODE_INACTIVE:  //nothing to be done
		break;
		case WBMODE_MEASURE:  //get white balance values by evaluating a capture of a white surface
		{
			pR = (U8*)img->st;
			pG = (U8*)img->ccmp1;
			pB = (U8*)img->ccmp2;

			R = G = B = 0;

			for(y= 0; y< img->dy; y++)
			{
				r = g = b = 0;

				for(x= 0; x< img->dx; x++)
				{
					r += *pR++;
					g += *pG++;
					b += *pB++;
				}

				R += (r / img->dx);
				G += (g / img->dx);
				B += (b / img->dx);
			}

			cfgWB->ampRed   = R / img->dy;
			cfgWB->ampGreen = G / img->dy;
			cfgWB->ampBlue  = B / img->dy;

			printf("White Balance Values (max. 200) : ampRed=%3d ampGreen=%3d ampBlue=%3d\n", cfgWB->ampRed, cfgWB->ampGreen, cfgWB->ampBlue);
		}
		//no break here to apply the measured values directly.
		case WBMODE_APPLY:  //set white balance values
		{
			max = cfgWB->ampRed; if(max < cfgWB->ampGreen) max = cfgWB->ampGreen; if(max < cfgWB->ampBlue) max = cfgWB->ampBlue;

			R = (256 * max) / cfgWB->ampRed;
			G = (256 * max) / cfgWB->ampGreen;
			B = (256 * max) / cfgWB->ampBlue;

			#if _OPENMP
			#   pragma omp parallel for
			#endif
			for(int y= 0; y< img->dy; y++)
			{
				pR = (U8*)img->st    + y * img->pitch;
				for(int x= 0; x< img->dx; x++)
				{
					r = (*pR * R) >> 8; if(r > 255) r = 255; *pR++ = r;
				}
			}
			#if _OPENMP
			#   pragma omp parallel for
			#endif
			for(int y= 0; y< img->dy; y++)
			{
				pG = (U8*)img->ccmp1 + y * img->pitch;
				for(int x= 0; x< img->dx; x++)
				{
					g = (*pG * G) >> 8; if(g > 255) g = 255; *pG++ = g;
				}
			}

			#if _OPENMP
			#   pragma omp parallel for
			#endif
			for(int y= 0; y< img->dy; y++)
			{
				pB = (U8*)img->ccmp2 + y * img->pitch;
				for(int x= 0; x< img->dx; x++)
				{
					b = (*pB * B) >> 8; if(b > 255) b = 255; *pB++ = b;
				}
			}
		}
		break;
		default: return -1;
	}

	return 0;
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Copies an Image Buffer to another Image Buffer.
*
*  This function copies an image buffer to another image buffer.
*/
/*-----------------------------------------------------------------------------*/
int copy_image(image *in, image *out)
{
	int  ee, y;
	int  dx=min(in->dx,out->dx);
	int  dy=min(in->dy,out->dy);

	if(in->type != out->type) { ee=-1; goto fail; }

	#if _OPENMP
	#   pragma omp parallel for
	#endif
	for(y= 0; y< dy; y++)
	{
		memcpy((U8*)out->st + y * out->pitch, (U8*)in->st + y * in->pitch, dx);
	}
	if(IMAGE_RGB==out->type)
	{
		#if _OPENMP
		#   pragma omp parallel for
		#endif
		for(y= 0; y< dy; y++)
		{
			memcpy((U8*)out->ccmp1 + y * out->pitch,  (U8*)in->ccmp1 + y * in->pitch, dx);
		}

		#if _OPENMP
		#   pragma omp parallel for
		#endif
		for(y= 0; y< dy; y++)
		{
			memcpy((U8*)out->ccmp2 + y * out->pitch,  (U8*)in->ccmp2 + y * in->pitch, dx);
		}
	}

	ee=0;
fail:
	return(ee);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Outputs Image Pixels to a Framebuffer Device.
*
*  This function outputs Image Pixels to to a framebuffer device.
*
* @param  stp        Print only each  @p stp  pixel.
* @param  goUpIff1   Overwrite old ASCII image, works only on recent terminals,
*                    and needs no other data lines to be printed.
*/
/*-----------------------------------------------------------------------------*/
int  copy_image_to_framebuffer(char *pcFramebufferDev, const void *pvDataGREY_OR_R, const void *pvDataGREY_OR_G, const void *pvDataGREY_OR_B, U32 dy, U32 pitch)
{
	I32                       rc, ee;

	U8                       *pInR   = NULL, *pInG   = NULL, *pInB   = NULL;
	U8                       *pOut   = NULL;
	int                       fbufFd = 0;
	U8                       *fbufSt = NULL;
	struct fb_var_screeninfo  fbufVars;
	struct fb_fix_screeninfo  fbufConsts;
	U32                       fbufByteCount;
	U32                       x, y, scaler;


	// Open the framebuffer for reading and writing
	{
		fbufFd =  open(pcFramebufferDev, O_RDWR);
		if(fbufFd<0){ee=-1; goto fail;}
	}

	// Get framebuffer information
	{
		// Variable information
		rc =  ioctl(fbufFd, FBIOGET_VSCREENINFO, &fbufVars  );
		if(rc<0){ee=-2+10*rc; goto fail;}

		// Constant information
		rc =  ioctl(fbufFd, FBIOGET_FSCREENINFO, &fbufConsts);
		if(rc<0){ee=-3+10*rc; goto fail;}
	}

	// Map the framebuffer to memory
	{
		fbufByteCount = fbufVars.xres * fbufVars.yres * fbufVars.bits_per_pixel / 8;

		fbufSt = (U8*) mmap(NULL, fbufByteCount, PROT_READ | PROT_WRITE, MAP_SHARED, fbufFd, 0);
		if(fbufSt<0){ee=-4; goto fail;}
	}

	// Approx. scale up/down to framebuffer size.
	scaler =  max(1,  min(pitch/fbufVars.xres, dy/fbufVars.yres));

	// Write pixel per pixel (slow)
	for(y = 0; y < min(fbufVars.yres, dy); y++)
	{
		pInR = ((U8*)pvDataGREY_OR_R) + (scaler * y) * pitch;
		pInG = ((U8*)pvDataGREY_OR_G) + (scaler * y) * pitch;
		pInB = ((U8*)pvDataGREY_OR_B) + (scaler * y) * pitch;
		pOut =       fbufSt  + (y + fbufVars.yoffset) * fbufConsts.line_length
				     +      fbufVars.xoffset  * fbufVars.bits_per_pixel/8;

		for(x = 0; x < min(fbufVars.xres, pitch); x++)
		{
			*((U32*) pOut) = ((*pInR) << 16) | ((*pInG) << 8) | ((*pInB) << 0);

			pInR += scaler;
			pInG += scaler;
			pInB += scaler;
			pOut += fbufVars.bits_per_pixel/8;
		}
	}


	ee=0;
fail:
	if(NULL!=fbufSt){  munmap(fbufSt, fbufByteCount);  fbufSt = NULL; }
	close(fbufFd);

	return ee;
}







/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from 10 Bit in 16 Bit Format to 8 Bit Grey Value
*         by removing the lowermost bits.
*
*  This function converts one line of an image from 10 bit in 16 bit format
*  to 8 bit grey value by discarding the lowermost bits.
*
*  The 10 bits at 16 bit format is expected to have two bytes per pixel with
*  lower bits at the first byte, the upper byte uses only the two last bits.
*
* @param  count      Length of the conversion, should be the width of the output image.
* @param  bufIn      16 Bit encoded data: if the output image has width bytes,
*                    this buffer should have at least 2*width bytes.
* @param  bufOut     8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_10AS16BIT_U8P(U32 count, char *bufIn, U8 *bufOut)
{
	while(count >= 8)
	{
		U64 *in1, in2;

		in1 = ((U64*)bufIn);

		in2 = ((*in1 & 0x0003000300030003) << 14)|(*in1 >> 2);

		*bufOut = *(((U8*)&in2)+0);
		bufOut++;
		*bufOut = *(((U8*)&in2)+2);
		bufOut++;
		*bufOut = *(((U8*)&in2)+4);
		bufOut++;
		*bufOut = *(((U8*)&in2)+6);
		bufOut++;

		bufIn+=8;

		count -= 4;
	}
	while(count >= 4)
	{
		U32 *in1, in2;

		in1 = ((U32*)bufIn);

		in2 = ((*in1 & 0x00030003) << 14)|(*in1 >> 2);

		*bufOut = *(((U8*)&in2)+0);
		bufOut++;
		*bufOut = *(((U8*)&in2)+2);
		bufOut++;

		bufIn+=4;

		count -= 2;
	}
	while(count--)
	{
		*bufOut = (U8)( (*(bufIn+0)>>2) | (*(bufIn+1)<<6) );
		bufIn++;
		bufIn++;
		bufOut++;
	}
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from 12 Bit in 16 Bit Format to 8 Bit Grey Value
*         by removing the lowermost bits.
*
*  This function converts one line of an image from 12 bit in 16 bit format
*  to 8 bit grey value by discarding the lowermost bits.
*
*  The 12 bits at 16 bit format is expected to have two bytes per pixel with
*  lower bits at the first byte, the upper byte uses only the two last bits.
*
* @param  count      Length of the conversion, should be the width of the output image.
* @param  bufIn      16 Bit encoded data: if the output image has width bytes,
*                    this buffer should have at least 2*width bytes.
* @param  bufOut     8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_12AS16BIT_U8P(U32 count, char *bufIn, U8 *bufOut)
{
	while(count >= 8)
	{
		U64 *in1, in2;

		in1 = ((U64*)bufIn);

		in2 = (*in1 >> 4);

		*bufOut = *(((U8*)&in2)+0);
		bufOut++;
		*bufOut = *(((U8*)&in2)+2);
		bufOut++;
		*bufOut = *(((U8*)&in2)+4);
		bufOut++;
		*bufOut = *(((U8*)&in2)+6);
		bufOut++;

		bufIn+=8;

		count -= 4;
	}
	while(count >= 4)
	{
		U32 *in1, in2;

		in1 = ((U32*)bufIn);

		in2 = (*in1 >> 4);

		*bufOut = *(((U8*)&in2)+0);
		bufOut++;
		*bufOut = *(((U8*)&in2)+2);
		bufOut++;

		bufIn+=4;

		count -= 2;
	}
	while(count--)
	{
		*bufOut = (U8)( (*(bufIn+0)>>4) | (*(bufIn+1)<<4) );
		bufIn++;
		bufIn++;
		bufOut++;
	}
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts Image from 10 Bit in 16 Bit Format to 8 Bit Grey Value
*         by removing the lowermost bits.
*
*  This function converts an image of an image from 10 bit in 16 bit format
*  to 8 bit grey value by discarding the lowermost bits.
*
*  The 10 bits at 16 bit format is expected to have two bytes per pixel with
*  lower bits at the first byte, the upper byte uses only the two last bits.
*
* @param  bufIn       SRGGB10 encoded data: if the output image has width*height bytes,
*                     this buffer should have at least 2*height*width bytes.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  maxBits     Needed for getting the uppermost bits for the 8 Bit Image.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_16bit_to_image(image *imgOut, char *bufIn, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 maxBits)
{
	I32   dx = min(imgOut->dx, v4lDx);
	I32   y;

	if(IMAGE_GREY!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}


	switch(maxBits)
	{
		case 10:
		{
			#if _OPENMP
			#   pragma omp parallel for
			#endif
			for(y= 0; y< min(imgOut->dy,v4lDy); y++)
			{
				char *in  =       bufIn + y * (2*v4lPitch);
				U8   *out =  imgOut->st + y * imgOut->pitch;

				FL_CPY_10AS16BIT_U8P(dx, in, out);
			}
		}
		break;
		case 12:
		{
			#if _OPENMP
			#   pragma omp parallel for
			#endif
			for(y= 0; y< min(imgOut->dy,v4lDy); y++)
			{
				char *in  =       bufIn + y * (2*v4lPitch);
				U8   *out =  imgOut->st + y * imgOut->pitch;

				FL_CPY_12AS16BIT_U8P(dx, in, out);
			}
		}
		break;
		return(ERR_PARAM);
	}

	return(ERR_NONE);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from RAW12 to 8 Bit Grey Value (Offset-Free).
*
*  This function converts one line of an image from RAW12 to an 8 bit grey value.
*
*  The RAW12 format has two bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the two preceeding pixels packed together:
*
*    X0, X1, LowerBitsOfX0..1,  X2, X3, LowerBitsOfX2..3, etc.
*
*  The algorithm simply copies the first two Bytes and skips over the following
*  byte with the lowermost bit information.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, this function needs the byte with the
*  lower bits at third position, like being shown at the example above.
*
* @param  count      Length of the conversion, should be the width of the output image.
* @param  bufIn      RAW12 encoded data: if the output image has width bytes,
*                    this buffer should have at least (width * 12)/8 bytes.
* @param  bufOut     8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_RAW12P_U8P_NOOFFS(U32 count, char *bufIn, U8 *bufOut)
{
	while(count >= 2)
	{
		*((U16*)bufOut) = *((U16*)bufIn);
		bufIn+=3;
		bufOut+=2;

		count -= 2;
	}

	while(count--)
	{
		*bufOut = (U8)(*bufIn);
		bufIn++;
		bufOut++;
	}
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts Image from RAW12 Format to 8 Bit Grey Value.
*
*  This function converts an image from RAW12 format to 8 bit grey value.
*
*  The RAW12 format has two bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the two preceeding pixels packed together:
*
*    X0, X1, LowerBitsOfX0..1,  X2, X3, LowerBitsOfX2..3, etc.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, the trackOffset parameter encodes this:
*
*  - If trackOffset==0 the first byte is X0.
*  - If trackOffset==1 the first byte is X1.
*  - Values of trackOffset > 1 are not allowed, especially the input buffer
*    is not allowed to start at the lowermost bits byte.
*
* @param  bufIn       RAW12 encoded data: if the output image has width*height bytes,
*                     this buffer should have at least height*(width * 12)/8 bytes.
* @param  trackOffset See text.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_raw12_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   y;

	if(IMAGE_GREY!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}

	if((0==trackOffset)&&(0==v4lX0)&&(0==v4lY0))
	{
		#if _OPENMP
		#   pragma omp parallel for
		#endif
		for(y= 0; y< min(imgOut->dy,v4lDy); y++)
		{
			char *in  =       bufIn + y * ((v4lPitch*12)/8 + v4lPaddingBytes);
			U8   *out =  imgOut->st + y * imgOut->pitch;

			FL_CPY_RAW12P_U8P_NOOFFS(dx, in, out);
		}
	}
	else
	{
		printf("%s():  Error, x0/y0 other than 0 unsupported at this bit depth.\n", __FUNCTION__);
	}

	return(ERR_NONE);
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from RAW14 to 8 Bit Grey Value (Offset-Free).
*
*  This function converts one line of an image from RAW14 to an 8 bit grey value.
*
*  The RAW14 format has two bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the two preceeding pixels packed together:
*
*    X0, X1, X2, X3, LowerBitsOfX0..3(3 Bytes), etc.
*
*  The algorithm simply copies the first two Bytes and skips over the following
*  byte with the lowermost bit information.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, this function needs the byte with the
*  lower bits at third position, like being shown at the example above.
*
* @param  count      Length of the conversion, should be the width of the output image.
* @param  bufIn      RAW14 encoded data: if the output image has width bytes,
*                    this buffer should have at least (width * 14)/8 bytes.
* @param  bufOut     8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_RAW14P_U8P_NOOFFS(U32 count, char *bufIn, U8 *bufOut)
{
	while(count >= 4)
	{
		*((U32*)bufOut) = *((U32*)bufIn);
		bufIn+=7;
		bufOut+=4;

		count -= 4;
	}

	while(count--)
	{
		*bufOut = (U8)(*bufIn);
		bufIn++;
		bufOut++;
	}
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts Image from RAW14 Format to 8 Bit Grey Value.
*
*  This function converts an image from RAW14 format to 8 bit grey value.
*
*  The RAW14 format has two bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the two preceeding pixels packed together:
*
*    X0, X1, X2, X3, LowerBitsOfX0..3 (3 Bytes), etc.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, the trackOffset parameter encodes this:
*
*  - If trackOffset==0 the first byte is X0.
*  - If trackOffset==1 the first byte is X1.
*  - Values of trackOffset > 1 are not allowed, especially the input buffer
*    is not allowed to start at the lowermost bits byte.
*
* @param  bufIn       RAW14 encoded data: if the output image has width*height bytes,
*                     this buffer should have at least height*(width * 14)/8 bytes.
* @param  trackOffset See text.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_raw14_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   y;

	if(IMAGE_GREY!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}

	if((0==trackOffset)&&(0==v4lX0)&&(0==v4lY0))
	{
		#if _OPENMP
		#   pragma omp parallel for
		#endif
		for(y= 0; y< min(imgOut->dy,v4lDy); y++)
		{
			char *in  =       bufIn + y * ((v4lPitch*14)/8 + v4lPaddingBytes);
			U8   *out =  imgOut->st + y * imgOut->pitch;

			FL_CPY_RAW14P_U8P_NOOFFS(dx, in, out);
		}
	}
	else
	{
		printf("%s():  Error, x0/y0 other than 0 unsupported at this bit depth.\n", __FUNCTION__);
	}

	return(ERR_NONE);
}

/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from RAW10 to 8 Bit Grey Value (Offset-Free).
*
*  This function converts one line of an image from RAW10 to an 8 bit grey value.
*
*  The RAW10 format has four bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the four preceeding pixels packed together:
*
*    X0, X1, X2, X3, LowerBitsOfX0..3,  X4, X5, X6, X7, LowerBitsOfX4..7, etc.
*
*  The algorithm simply copies the first four Bytes and skips over the following
*  byte with the lowermost bit information.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, this function needs the byte with the
*  lower bits at fifth position, like being shown at the example above.
*
*   there is   so it is important where the buffer begins: trackOffset==0->X0,trackOffset<4->X1..3.
*
* @param  count      Length of the conversion, should be the width of the output image.
* @param  bufIn      RAW10 encoded data: if the output image has width bytes,
*                    this buffer should have at least (width * 10)/8 bytes.
* @param  bufOut     8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_RAW10P_U8P_NOOFFS(U32 count, char *bufIn, U8 *bufOut)
{
	while(count >= 4)
	{
		*((U32*)bufOut) = *((U32*)bufIn);
		bufIn+=5;
		bufOut+=4;

		count -= 4;
	}

	while(count--)
	{
		*bufOut = (U8)(*bufIn);
		bufIn++;
		bufOut++;
	}
}





/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts One Line from RAW10 to 8 Bit Grey Value.
*
*  This function converts one line of an image from RAW10 to an 8 bit grey value.
*
*  The RAW10 format has four bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the four preceeding pixels packed together:
*
*    X0, X1, X2, X3, LowerBitsOfX0..3,  X4, X5, X6, X7, LowerBitsOfX4..7, etc.
*
*  The algorithm simply copies the first four Bytes and skips over the following
*  byte with the lowermost bit information.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, the trackOffset parameter encodes this:
*
*  - If trackOffset==0 the first byte is X0.
*  - If trackOffset==1 the first byte is X1.
*  - If trackOffset==2 the first byte is X2.
*  - If trackOffset==3 the first byte is X3.
*  - Values of trackOffset > 3 are not allowed, especially the input buffer
*    is not allowed to start at the lowermost bits byte.
*
* @param  count       Length of the conversion, should be the width of the output image.
* @param  bufIn       RAW10 encoded data: if the output image has width bytes,
*                     this buffer should have at least (width * 10)/8 bytes.
* @param  trackOffset See text.
* @param  bufOut      8 Bit Grey Value Output Address.
*/
/*-----------------------------------------------------------------------------*/
inline void  FL_CPY_RAW10P_U8P(U32 count, U8 trackOffset, char *bufIn, U8 *bufOut)
{
	while((trackOffset<4)&&(count-- > 0))
	{
		*bufOut = (U8)(*bufIn);
		bufIn++;
		bufOut++;
		trackOffset+=1;
	}

	bufIn++;

	while(count >= 4)
	{
		*((U32*)bufOut) = *((U32*)bufIn);
		bufIn+=5;
		bufOut+=4;

		count -= 4;
	}

	while(count--)
	{
		*bufOut = (U8)(*bufIn);
		bufIn++;
		bufOut++;
	}
}






/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Converts Image from RAW10 Format to 8 Bit Grey Value.
*
*  This function converts an image from RAW10 format to 8 bit grey value.
*
*  The RAW10 format has four bytes with each containing the uppermost 8 bits
*  of the pixels followed by one byte with the two lowermost bits of each of
*  the four preceeding pixels packed together:
*
*    X0, X1, X2, X3, LowerBitsOfX0..3,  X4, X5, X6, X7, LowerBitsOfX4..7, etc.
*
*  Since the starting position is relevant to know where the byte with the
*  lower bits is and to jump over it, the trackOffset parameter encodes this:
*
*  - If trackOffset==0 the first byte is X0.
*  - If trackOffset==1 the first byte is X1.
*  - If trackOffset==2 the first byte is X2.
*  - If trackOffset==3 the first byte is X3.
*  - Values of trackOffset > 3 are not allowed, especially the input buffer
*    is not allowed to start at the lowermost bits byte.
*
* @param  bufIn       RAW10 encoded data: if the output image has width*height bytes,
*                     this buffer should have at least height*(width * 10)/8 bytes.
* @param  trackOffset See text.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_raw10_to_image(image *imgOut, char *bufIn, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   y;

	if(IMAGE_GREY!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}
	if(trackOffset > 3)
	{
		return(ERR_PARAM);
	}


	if((0==trackOffset)&&(0==v4lX0)&&(0==v4lY0))
	{
		#if _OPENMP
		#   pragma omp parallel for
		#endif
		for(y= 0; y< min(imgOut->dy,v4lDy); y++)
		{
			char *in  =       bufIn + y * ((v4lPitch*5)/4 + v4lPaddingBytes);
			U8   *out =  imgOut->st + y * imgOut->pitch;

			FL_CPY_RAW10P_U8P_NOOFFS(dx, in, out);
		}
	}
	else
	{
		char *in  = bufIn;
		U8   *out = imgOut->st;

		for(y= 0; y< v4lDy; y++)
		{
			in          =          in  + (         v4lX0) + ((trackOffset%4)+(         v4lX0))/4;
			trackOffset = (trackOffset + (         v4lX0))%4;

			if((y >= v4lY0)&&(y - v4lY0 < imgOut->dy))
			{
				FL_CPY_RAW10P_U8P(dx, trackOffset, in, out);

				out = out + imgOut->pitch;
			}

			in          =          in  + (v4lPitch-v4lX0) + ((trackOffset%4)+(v4lPitch-v4lX0))/4 + v4lPaddingBytes;
			trackOffset = (trackOffset + (v4lPitch-v4lX0))%4;
		}
	}

	return(ERR_NONE);
}


/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Copys Image from GREY Format to 8 Bit Grey Value.
*
*  This function copys an image from GREY format to 8 bit grey value.
*  It is just a copy routine, since the data format is already the same.
*
* @param  bufIn       GREY encoded data.
* @param  ignoredTrackOffset Ignored.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  copy_grey_to_image(image *imgOut, char *bufIn, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   y;

	if(IMAGE_GREY!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}


	#if _OPENMP
	#   pragma omp parallel for
	#endif
	for(y= 0; y< min(imgOut->dy,v4lDy); y++)
	{
		char *in  =       bufIn + v4lX0 + (y + v4lY0) * (v4lPitch + v4lPaddingBytes);
		U8   *out =  imgOut->st + y * imgOut->pitch;

		memcpy(out, in, dx);
	}


	return(ERR_NONE);
}






/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Direct conversion from raw Bayer RGB data to IMAGE_RGB.
*
*  This function does a direct conversion from raw Bayer RGB data to IMAGE_RGB.
*
* @param  bufIn       Bayer encoded data.
* @param  ignoredTrackOffset Ignored.
* @param  pixelformat Pixel format of input data.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit RGB Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_raw_and_debayer_image(image *imgOut, char *bufIn, U32 pixelformat, U8 trackOffset, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	int    rc, ee;
	image  imgU8 = NULL_IMAGE;

	// Allocate temporary image
	{
		imgU8.type = IMAGE_GREY;
		imgU8.dx   = v4lDx;
		imgU8.dy   = v4lDy;
		imgU8.pitch= imgU8.dx;
		imgU8.ccmp1= NULL;
		imgU8.ccmp2= NULL;
		imgU8.st   = (U8*) malloc(sizeof(U8) * imgU8.dy * imgU8.pitch);
		if(NULL==imgU8.st){ee=-1; goto fail;}
	}

	switch(pixelformat)
	{
		case V4L2_PIX_FMT_SRGGB14P:
		case V4L2_PIX_FMT_SBGGR14P:
			rc =  convert_raw14_to_image(&imgU8,  bufIn, trackOffset,  v4lX0, v4lY0, v4lDx, v4lDy, v4lPitch, v4lPaddingBytes);
			if(rc<0){ee=-2+10*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB12P:
		case V4L2_PIX_FMT_SBGGR12P:
			rc =  convert_raw12_to_image(&imgU8,  bufIn, trackOffset,  v4lX0, v4lY0, v4lDx, v4lDy, v4lPitch, v4lPaddingBytes);
			if(rc<0){ee=-3+10*rc; goto fail;}
			break;
		case V4L2_PIX_FMT_SRGGB10P:
		case V4L2_PIX_FMT_SBGGR10P:
		case V4L2_PIX_FMT_SGBRG10P:
			rc =  convert_raw10_to_image(&imgU8,  bufIn, trackOffset,  v4lX0, v4lY0, v4lDx, v4lDy, v4lPitch, v4lPaddingBytes);
			if(rc<0){ee=-4+10*rc; goto fail;}
			break;
		default:
			return(ERR_FORMAT);
	}

	rc =  simple_debayer_to_image(imgOut, (char*)imgU8.st, pixelformat, 0, 0, imgU8.dx, imgU8.dy, imgU8.pitch, 0);
	if(rc<0){ee=-5+10*rc; goto fail;}

	ee=0;
fail:
	if(NULL!=imgU8.st){ free(imgU8.st);  imgU8.st=NULL; }

	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Direct conversion from raw10 Bayer RGB data to IMAGE_RGB.
*
*  This function does a direct conversion from raw10 Bayer RGB data to IMAGE_RGB.
*
* @param  bufIn       Bayer encoded data.
* @param  ignoredTrackOffset Ignored.
* @param  pixelformat Pixel format of input data.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit RGB Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_srggb_and_debayer_image(image *imgOut, char *bufIn, U32 pixelformat, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 maxBits)
{
	int    rc, ee;
	image  imgU8 = NULL_IMAGE;

	// Allocate temporary image
	{
		imgU8.type = IMAGE_GREY;
		imgU8.dx   = v4lDx;
		imgU8.dy   = v4lDy;
		imgU8.pitch= imgU8.dx;
		imgU8.ccmp1= NULL;
		imgU8.ccmp2= NULL;
		imgU8.st   = (U8*) malloc(sizeof(U8) * imgU8.dy * imgU8.pitch);
		if(NULL==imgU8.st){ee=-1; goto fail;}
	}

	rc =  convert_16bit_to_image(&imgU8,  bufIn, v4lDx, v4lDy, v4lPitch, maxBits);
	if(rc<0){ee=-2+10*rc; goto fail;}

	rc =  simple_debayer_to_image(imgOut, (char*)imgU8.st, pixelformat, 0, 0, imgU8.dx, imgU8.dy, imgU8.pitch, 0);
	if(rc<0){ee=-3+10*rc; goto fail;}

	ee=0;
fail:
	if(NULL!=imgU8.st){ free(imgU8.st);  imgU8.st=NULL; }

	return(ee);
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Simple, inaccurate and SLOW YCbCr422 to RGB conversion for sensor image data.
*
*  This function does a simple inaccurate and SLOW YCbCr422 to RGB conversion for sensor image data:
*
*   R = Y                + 1.40200 * Cr
*   G = Y - 0.34414 * Cb - 0.71414 * Cr
*   B = Y + 1.77200 * Cb
*
* @param  bufIn       GREY encoded data.
* @param  ignoredTrackOffset Ignored.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  convert_yuyv_to_image(image *imgOut, char *bufIn, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   dy = min(imgOut->dy, v4lDy - v4lY0);
	I32   y;

	if(IMAGE_RGB!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}

	#if _OPENMP
	#   pragma omp parallel for
	#endif
	for(y= 0; y< dy; y++)
	{
		I32   x;
		U8   *in = NULL;
		U8   *y0 = NULL, *y1 = NULL, *cb = NULL, *cr = NULL;
		I16   cba, cra;
		U8   *oR= NULL, *oG= NULL, *oB= NULL;

		in  =  (U8*)bufIn + 2 * v4lX0 + ((y+0) + v4lY0) * (2 * v4lPitch + v4lPaddingBytes);
		oR  =  imgOut->st    + (y+0) * imgOut->pitch;
		oG  =  imgOut->ccmp1 + (y+0) * imgOut->pitch;
		oB  =  imgOut->ccmp2 + (y+0) * imgOut->pitch;

		for(x= 0; x< dx; x+=2)
		{
			// Y0|Cb|Y1|Cr

			y0 = in+0;
			cb = in+1;
			y1 = in+2;
			cr = in+3;

			//errors through rounding, this code is for speed and not accuracy.
			cba = (*cb - 128) * 0.353;
			cra = (*cr - 128) * 0.705;

			*oR++ =           + 2 * cra + *y0;
			*oR++ =           + 2 * cra + *y1;
			*oG++ = -     cba -     cra + *y0;
			*oG++ = -     cba -     cra + *y1;
			*oB++ = + 5 * cba           + *y0;
			*oB++ = + 5 * cba           + *y1;

			in+= 4;
		}
	}

	return(ERR_NONE);
}









/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Simple and SLOW debayering for sensor image data.
*
*  This function does a simple and SLOW debayering for sensor image data.
*
* @param  bufIn       GREY encoded data.
* @param  ignoredTrackOffset Ignored.
* @param  v4lX0,v4lY0 Offset of the top-left pixel relative to the current bufIn pointer.
* @param  v4lDx,v4lDy Dimensions of the input buffer.
* @param  v4lPitch    Currently the same as v4lDx.
* @param  v4lPaddingBytes  Additional bytes to v4lDx to get to a pixel one row down.
* @param  imgOut      8 Bit Grey Value Output image.
*/
/*-----------------------------------------------------------------------------*/
I32  simple_debayer_to_image(image *imgOut, char *bufIn, unsigned int pixelformat, I32 v4lX0, I32 v4lY0, I32 v4lDx, I32 v4lDy, I32 v4lPitch, I32 v4lPaddingBytes)
{
	I32   dx = min(imgOut->dx, v4lDx - v4lX0);
	I32   dy = min(imgOut->dy, v4lDy - v4lY0);
	I32   y;

	if(IMAGE_RGB!=imgOut->type)
	{
		return(ERR_TYPE);
	}
	if(v4lDx > v4lPitch)
	{
		return(ERR_PARAM);
	}
	if((v4lX0 >= v4lDx)||(v4lY0 >= v4lDy))
	{
		return(ERR_PARAM);
	}

	// parallel processing does not support 'return', so check pixelformat beforehand
	switch(pixelformat)
	{
		case V4L2_PIX_FMT_SRGGB14P:
		case V4L2_PIX_FMT_SRGGB14:
		case V4L2_PIX_FMT_SBGGR14P:
		case V4L2_PIX_FMT_SBGGR14:
		case V4L2_PIX_FMT_SRGGB12P:
		case V4L2_PIX_FMT_SRGGB12:
		case V4L2_PIX_FMT_SBGGR12P:
		case V4L2_PIX_FMT_SBGGR12:
		case V4L2_PIX_FMT_SRGGB10P:
		case V4L2_PIX_FMT_SRGGB10: //or RG10
		case V4L2_PIX_FMT_SRGGB8:
		case V4L2_PIX_FMT_SBGGR10P:
		case V4L2_PIX_FMT_SGBRG10P:
		case V4L2_PIX_FMT_SBGGR10: //or RG10
		case V4L2_PIX_FMT_SGBRG10: //or GB10
		case V4L2_PIX_FMT_SBGGR8:
		case V4L2_PIX_FMT_SGBRG8:
			break;
		default:
			return(ERR_FORMAT);
	}
	#if _OPENMP
	#   pragma omp parallel for
	#endif
	for(y= 0; y< dy; y+=2)
	{
		I32   x;
		I32   gFirstIff1=-1;
		U8   *in = NULL, *out= NULL;
		U8   *o0= NULL, *oG= NULL, *o1= NULL;

		// o0 and o1 respectively refer to all bayer positions of the same color (red or blue)
		// depending on the bayer pattern format.
		// oG is the green channel, which position is currently identical on both format types.

		switch(pixelformat)
		{
			case V4L2_PIX_FMT_SRGGB14P:
			case V4L2_PIX_FMT_SRGGB14:
			case V4L2_PIX_FMT_SRGGB12P:
			case V4L2_PIX_FMT_SRGGB12:
			case V4L2_PIX_FMT_SRGGB10P:
			case V4L2_PIX_FMT_SRGGB10: //or RG10
			case V4L2_PIX_FMT_SRGGB8:
				// o0 is red, o1 is blue
				o0  =  imgOut->st    + (y+0) * imgOut->pitch;
				oG  =  imgOut->ccmp1 + (y+0) * imgOut->pitch;
				o1  =  imgOut->ccmp2 + (y+0) * imgOut->pitch;
				gFirstIff1 = 0;
				break;
			case V4L2_PIX_FMT_SBGGR14P:
			case V4L2_PIX_FMT_SBGGR14:
			case V4L2_PIX_FMT_SBGGR12P:
			case V4L2_PIX_FMT_SBGGR12:
			case V4L2_PIX_FMT_SBGGR10P:
			case V4L2_PIX_FMT_SBGGR10: //or RG10
			case V4L2_PIX_FMT_SBGGR8:
				// o0 is blue, o1 is red
				o0  =  imgOut->ccmp2 + (y+0) * imgOut->pitch;
				oG  =  imgOut->ccmp1 + (y+0) * imgOut->pitch;
				o1  =  imgOut->st    + (y+0) * imgOut->pitch;
				gFirstIff1 = 0;
				break;
			case V4L2_PIX_FMT_SGBRG10P:
			case V4L2_PIX_FMT_SGBRG10: //or GB10
			case V4L2_PIX_FMT_SGBRG8:
				// o0 is blue, o1 is red
				o0  =  imgOut->ccmp2 + (y+0) * imgOut->pitch;
				oG  =  imgOut->ccmp1 + (y+0) * imgOut->pitch;
				o1  =  imgOut->st    + (y+0) * imgOut->pitch;
				gFirstIff1 = 1;
				break;
		}

		//first line input
		in  =  (U8*)bufIn + v4lX0 + ((y+0) + v4lY0) * (v4lPitch + v4lPaddingBytes);

		if(1==gFirstIff1)
		{
			for(x= 0; x< dx; x+=2)
			{
				*oG = *in;
				oG++;
				*oG = *in;
				oG++;

				in++;

				*o0 = *in;
				o0++;
				*o0 = *in;
				o0++;

				in++;
			}
		}
		else
		{
			for(x= 0; x< dx; x+=2)
			{
				*o0 = *in;
				o0++;
				*o0 = *in;
				o0++;

				in++;

				*oG = *in;
				oG++;
				*oG = *in;
				oG++;

				in++;
			}
		}


		//next line input
		in  =  (U8*)bufIn + v4lX0 + ((y+1) + v4lY0) * (v4lPitch + v4lPaddingBytes);
		oG  =  imgOut->ccmp1 + (y+1) * imgOut->pitch;

		if(1==gFirstIff1)
		{
			for(x= 0; x< dx; x+=2)
			{
				*o1 = *in;
				o1++;
				*o1 = *in;
				o1++;

				in++;

				*oG = *in;
				oG++;
				*oG = *in;
				oG++;

				in++;
			}
		}
		else
		{
			for(x= 0; x< dx; x+=2)
			{
				*oG = *in;
				oG++;
				*oG = *in;
				oG++;

				in++;

				*o1 = *in;
				o1++;
				*o1 = *in;
				o1++;

				in++;
			}
		}

		//copy to second line
		//Red
		in  =  imgOut->st    + (y+0) * imgOut->pitch;
		out =  imgOut->st    + (y+1) * imgOut->pitch;

		memcpy(out, in, dx);

		//Blue
		in  =  imgOut->ccmp2 + (y+0) * imgOut->pitch;
		out =  imgOut->ccmp2 + (y+1) * imgOut->pitch;

		memcpy(out, in, dx);
	}

	return(ERR_NONE);
}

void  timemeasurement_start(struct  timeval *timer)
{
	gettimeofday(timer,(struct timezone *)0);
}

void  timemeasurement_stop(struct  timeval *timer, I64 *s, I64 *us)
{
	struct  timeval  end;

	gettimeofday(&end,(struct timezone *)0);
	*s  = end.tv_sec  - timer->tv_sec;
	*us = end.tv_usec - timer->tv_usec;
	if(*us < 0){ *us += 1000000; *s = *s - 1; }
}









/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Stores an Image as Portable Graymap or Portable Pixmap (open with GIMP).
*
*  This function stores an image as Portable Graymap (PGM) or Portable Pixmap (PPM).
*  You can open this files for example with the GIMP (Gnu Image Manipulation Program).
*
* @param  path        The Filename with its path, extension will be added by type.
* @param  img         8 Bit Grey or RGB Value image to be stored.
*/
/*-----------------------------------------------------------------------------*/
I32  write_image_as_pnm(char *path, image *img)
{
	I32   ee, x, y;
	I32   fd=-1;
	I32   headerBytes, wroteBytes;
	char  acHeader[256], *pcFilename=NULL;
	char *pcLine=NULL;

	if((IMAGE_GREY!=img->type)&&(IMAGE_RGB!=img->type)){ee=-1; goto fail;}


	pcFilename = (char*) malloc(sizeof(char) * (strlen(path)+4+2));
	if(NULL==pcFilename){ee=-2; goto fail;}

	snprintf(pcFilename,strlen(path)+4+1,"%s.%s",path,(IMAGE_GREY==img->type)?("pgm"):("ppm"));


	fd =  open(pcFilename, O_WRONLY | O_CREAT, 00644);
	if(fd<0){ee=-3; goto fail;}


	headerBytes =  snprintf(acHeader,255,"P%c %d %d %d ",(IMAGE_GREY==img->type)?('5'):('6'), img->dx, img->dy, 255);
	if(headerBytes<0){ee=-4; goto fail;}

	wroteBytes =  write(fd, acHeader, headerBytes);
	if(wroteBytes!=headerBytes){ee=-5; goto fail;}


	switch(img->type)
	{
		case IMAGE_GREY:
		{
			for(y= 0; y< img->dy; y++)
			{
				wroteBytes =  write(fd, (U8*)img->st + y * img->pitch, img->dx);
				if(wroteBytes!=img->dx){ee=-6; goto fail;}
			}
		}
		break;
		case IMAGE_RGB:
		{
			pcLine = (char*) malloc(sizeof(U8) * 3 * img->dx);
			if(NULL==pcLine){ee=-7; goto fail;}

			for(y= 0; y< img->dy; y++)
			{
				for(x= 0; x< img->dx; x++)
				{
					pcLine[3*x+0] = *((U8*)img->st    + y * img->pitch + x);
					pcLine[3*x+1] = *((U8*)img->ccmp1 + y * img->pitch + x);
					pcLine[3*x+2] = *((U8*)img->ccmp2 + y * img->pitch + x);
				}

				wroteBytes =  write(fd, pcLine, 3 * img->dx);
				if(wroteBytes!=3 * img->dx){ee=-8; goto fail;}
			}
		}
		break;
		default: ee=-9; goto fail;
	}
	ee = write_image_as_png(path, img);


	ee=0;
fail:
	if(fd>=0)
	{
		close(fd);
	}
	if(NULL!=pcLine    ){ free(pcLine    ); pcLine    =NULL; }
	if(NULL!=pcFilename){ free(pcFilename); pcFilename=NULL; }

	return(ee);
}

I32 write_image_as_png(char *path, image *img) {
    I32 ee = 0;
    I32 y;

    // Append ".png" extension to the output file path
    char *pngPath = (char *)malloc(strlen(path) + 5);  // 4 for ".png" and 1 for null terminator
    if (!pngPath) {
        ee = -15;  // Set an error code for memory allocation failure
        goto fail;
    }
    sprintf(pngPath, "%s.png", path);

    FILE *fp = fopen(pngPath, "wb");
    if (!fp) {
        ee = -10;
        goto fail;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ee = -11;
        goto fail;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ee = -12;
        goto fail;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ee = -13;
        goto fail;
    }

    png_init_io(png_ptr, fp);

    png_set_IHDR(
        png_ptr, info_ptr,
        img->dx, img->dy, 8,
        (img->type == IMAGE_RGB) ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_GRAY,
        PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );

    // Declaration of row_pointers outside the switch block
    png_bytep *row_pointers = (png_bytep*)malloc(img->dy * sizeof(png_bytep));
    if (!row_pointers) {
        ee = -14;
        goto fail;
    }

    for (y = 0; y < img->dy; y++) {
        row_pointers[y] = (png_bytep)((U8 *)img->st + y * img->pitch);
    }

    png_set_rows(png_ptr, info_ptr, row_pointers);

    png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

fail:
    if (png_ptr && info_ptr) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
    }
    if (fp) {
        fclose(fp);
    }
    if (row_pointers) {
        free(row_pointers);
    }
    if (pngPath) {
        free(pngPath);
    }

    return ee;
}


// I32 write_image_as_png(char *path, image *img) {
//     I32 ee = 0;
//     I32 y;

//     FILE *fp = fopen(path, "wb");
//     if (!fp) {
//         ee = -10;
//         goto fail;
//     }

//     png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
//     if (!png_ptr) {
//         ee = -11;
//         goto fail;
//     }

//     png_infop info_ptr = png_create_info_struct(png_ptr);
//     if (!info_ptr) {
//         ee = -12;
//         goto fail;
//     }

//     if (setjmp(png_jmpbuf(png_ptr))) {
//         ee = -13;
//         goto fail;
//     }

//     png_init_io(png_ptr, fp);

//     png_set_IHDR(
//         png_ptr, info_ptr,
//         img->dx, img->dy, 8,
//         (img->type == IMAGE_RGB) ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_GRAY,
//         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
//     );

//     // Declaration of row_pointers outside the switch block
//     png_bytep *row_pointers = (png_bytep*)malloc(img->dy * sizeof(png_bytep));
//     if (!row_pointers) {
//         ee = -14;
//         goto fail;
//     }

//     for (y = 0; y < img->dy; y++) {
//         row_pointers[y] = (png_bytep)((U8 *)img->st + y * img->pitch);
//     }

//     png_set_rows(png_ptr, info_ptr, row_pointers);

//     png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

// fail:
//     if (png_ptr && info_ptr) {
//         png_destroy_write_struct(&png_ptr, &info_ptr);
//     }
//     if (fp) {
//         fclose(fp);
//     }
//     if (row_pointers) {
//         free(row_pointers);
//     }

//     return ee;
// }


/***************************************************************************/
/***********************************************************************//**
*  The whole media controller functions are solely for setting the
*  region of interest for the sensor. It is not necessary for a normal
*  image acquisition.
*
***************************************************************************/


/*--*STRUCT*----------------------------------------------------------*/
/**
*  @brief  Media Controller Access.
*
*    This structure holds the file descriptor of the media controller
*    for access as well as the read-out topology.
*/
typedef struct
{
	int                        fd;        /*!<  File Descriptor of the opened Media Controller Device.  */
	struct media_v2_topology  *topology;  /*!<  Topology of the Media Controller Device.  */

} VCMipiMediaCfg;
#define NULL_VCMipiMediaCfg { -1, NULL }



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Frees the Inner Pointers and the Media Topology Structure itself.
*
*  This function frees the inner pointers and the media topology structure itself.
*/
/*-----------------------------------------------------------------------------*/
void  media_controller_topology_free(struct media_v2_topology **topology)
{
	if(NULL!=(*topology))
	{
		if((intptr_t)NULL!=(*topology)->ptr_links     ){  free((struct media_v2_ptr_link     *)(intptr_t)(*topology)->ptr_links     );  (*topology)->ptr_links      = (intptr_t)NULL;  (*topology)->num_links      = 0; }
		if((intptr_t)NULL!=(*topology)->ptr_pads      ){  free((struct media_v2_ptr_pad      *)(intptr_t)(*topology)->ptr_pads      );  (*topology)->ptr_pads       = (intptr_t)NULL;  (*topology)->num_pads       = 0; }
		if((intptr_t)NULL!=(*topology)->ptr_interfaces){  free((struct media_v2_ptr_interface*)(intptr_t)(*topology)->ptr_interfaces);  (*topology)->ptr_interfaces = (intptr_t)NULL;  (*topology)->num_interfaces = 0; }
		if((intptr_t)NULL!=(*topology)->ptr_entities  ){  free((struct media_v2_ptr_entity   *)(intptr_t)(*topology)->ptr_entities  );  (*topology)->ptr_entities   = (intptr_t)NULL;  (*topology)->num_entities   = 0; }

		free(*topology);
		*topology=NULL;
	}
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Allocates the Inner Pointers and the Media Topology Structure itself.
*
*  This function allocates the inner pointers and the media topology structure itself.
*/
/*-----------------------------------------------------------------------------*/
int  media_controller_topology_allocate(struct media_v2_topology **topology, U32 num_entities, U32 num_interfaces, U32 num_pads, U32 num_links)
{
	I32    ee;

	(*topology) =  (struct media_v2_topology *) malloc(sizeof(struct media_v2_topology));
	if(NULL==(*topology)){ee=-1; goto fail;}

	// Reset Allocation Markers, prohibits wrong deallocation.
	memset((*topology), 0, sizeof(struct media_v2_topology));

	(*topology)->ptr_entities =  (intptr_t) malloc(sizeof(struct media_v2_entity) * num_entities);
	if((intptr_t)NULL==(*topology)->ptr_entities){ee=-2; goto fail;}

	(*topology)->num_entities = num_entities;

	(*topology)->ptr_interfaces =  (intptr_t) malloc(sizeof(struct media_v2_interface) * num_interfaces);
	if((intptr_t)NULL==(*topology)->ptr_interfaces){ee=-3; goto fail;}

	(*topology)->num_interfaces = num_interfaces;

	(*topology)->ptr_pads =  (intptr_t) malloc(sizeof(struct media_v2_pad) * num_pads);
	if((intptr_t)NULL==(*topology)->ptr_pads){ee=-4; goto fail;}

	(*topology)->num_pads = num_pads;

	(*topology)->ptr_links =  (intptr_t) malloc(sizeof(struct media_v2_link) * num_links);
	if((intptr_t)NULL==(*topology)->ptr_links){ee=-5; goto fail;}

	(*topology)->num_links = num_links;

	ee = 0;
fail:
	return(ee);
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Writes Media Controller Device Topology to Syslog.
*
*  This function writes the media controller device topology to the syslog.
*/
/*-----------------------------------------------------------------------------*/
void  media_controller_topology_log(VCMipiMediaCfg *media)
{
	I32    rc;
	U32    idx, jdx, ldx;
	char   acLogLine[1023], *pc=NULL;


	//Print Links
	for(ldx= 0; ldx< media->topology->num_links; ldx++)
	{
		struct media_v2_link *link = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[ldx];

		syslog(LOG_DEBUG, "%s:  Link    [ @%8u ]----#%8u---->[ @%8u ]  Flags: 0x%08x\n", __FUNCTION__, link->source_id, link->id, link->sink_id, link->flags);
	}

	//Print Interfaces
	for(idx= 0; idx< media->topology->num_interfaces; idx++)
	{
		struct media_v2_interface *interface = &((struct media_v2_interface *)(intptr_t)media->topology->ptr_interfaces)[idx];

		pc=acLogLine;
		rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "Interface #%8u", interface->id);
		if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

		//Scan links connected to this interface and print their connection
		{
			rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "  <-[ ");
			if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

			for(ldx= 0; ldx< media->topology->num_links; ldx++)
			{
				struct media_v2_link *link = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[ldx];

				if((interface->id==link->source_id)||(interface->id==link->sink_id))
				{
					rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "@%8u ", link->id);
					if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}
				}
			}
		}
		rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "]  Type: 0x%08x  Device Node (major, minor):(%u,%u)", interface->intf_type, interface->devnode.major, interface->devnode.minor);
		if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

		syslog(LOG_DEBUG, "%s:  %s\n", __FUNCTION__, acLogLine);
	}

	//Print Entities
	for(idx= 0; idx< media->topology->num_entities; idx++)
	{
		struct media_v2_entity *entity = &((struct media_v2_entity *)(intptr_t)media->topology->ptr_entities)[idx];

		pc=acLogLine;
		rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "Entity    #%8u", entity->id);
		if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

		//Scan links connected to this entity and print their connection
		{
			rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "  <-[ ");
			if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

			for(ldx= 0; ldx< media->topology->num_links; ldx++)
			{
				struct media_v2_link *link = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[ldx];

				if((entity->id==link->source_id)||(entity->id==link->sink_id))
				{
					rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "@%8u ", link->id);
					if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}
				}
			}
		}
		rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "] '%s'", entity->name);
		if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

		syslog(LOG_DEBUG, "%s:  %s\n", __FUNCTION__, acLogLine);


		//Print Pads: Each entity may have several pads where links can be attached
		for(jdx= 0; jdx< media->topology->num_pads; jdx++)
		{
			struct media_v2_pad *pad = &((struct media_v2_pad *)(intptr_t)media->topology->ptr_pads)[jdx];

			if(entity->id == pad->entity_id)
			{
				pc=acLogLine;
				rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), " Pad %u    #%8u", pad->index, pad->id);
				if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

				//Scan links connected to this pad and print their connection
				{
					rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "  <-[ ");
					if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

					for(ldx= 0; ldx< media->topology->num_links; ldx++)
					{
						struct media_v2_link *link = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[ldx];

						if((pad->id==link->source_id)||(pad->id==link->sink_id))
						{
							rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "@%8u ", link->id);
							if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}
						}
					}
				}
				rc =  snprintf(pc, sizeof(acLogLine)-1 - (pc-acLogLine), "]  Flags: 0x%08x", pad->flags);
				if(rc<(int)sizeof(acLogLine)-1 - (pc-acLogLine)){pc += rc;}

				syslog(LOG_DEBUG, "%s:  %s\n", __FUNCTION__, acLogLine);
			}
		}
	}
}



/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Closes the Media Controller Device.
*
*  This function closes the media controller device.
*/
/*-----------------------------------------------------------------------------*/
int  media_controller_close(VCMipiMediaCfg *media)
{
	I32  ee, rc;

	// checks if device has been opened (see media_controller_open() and NULL_VCMipiMediaCfg)
	if(media->fd>=0)
	{
		if(NULL!=media->topology)
		{
			media_controller_topology_free(&media->topology);
		}

		// Close Media Controller Device.
		{
			rc =  close(media->fd);
			if(rc<0){ee=-1; goto fail;}

			media->fd = -1;
		}
	}

	ee=0;
fail:
	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Opens the Media Controller Device usable for Preprocessing.
*
*  This function opens the media controller device which can be used for preprocessing.
*/
/*-----------------------------------------------------------------------------*/
int  media_controller_open(char *dev_media_device, VCMipiMediaCfg *media)
{
	I32    ee, rc;

	// Reset Allocation Markers
	// prohibits closing of un-open device and prevents wrong deallocation.
	{
		*media  = (VCMipiMediaCfg) NULL_VCMipiMediaCfg;
	}

	// Open the Device.
	{
		media->fd =  open(dev_media_device, O_RDWR, 0);
		if(media->fd<0){ee=-1; goto fail;}
	}

	// Show the Media Controller Info of the Device
	{
		struct media_device_info  info;

		rc =  ioctl(media->fd, MEDIA_IOC_DEVICE_INFO, &info);
		if(rc<0){ee=-2; goto fail;}

		syslog(LOG_DEBUG, "%s:  Opened Media Controller Device: '%s', Driver Name: '%s'\n", __FUNCTION__, dev_media_device, info.driver);
	}

	// Get the topology of the Media Controller
	{
		//Temporary topology for determining the allocation size
		struct media_v2_topology  topology;

		//Since the topology may be altered during allocation and application of the content
		//it has to be re-read until the topology version fits.
		while(1)
		{
			//The first request is to get the number of entries for the allocation.
			{
				memset(&topology, 0, sizeof(struct media_v2_topology));

				rc =  ioctl(media->fd, MEDIA_IOC_G_TOPOLOGY, &topology);
				if(rc<0){ee=-3; goto fail;}

				syslog(LOG_DEBUG, "%s:  The Media Controller contains:  %u entities, %u interfaces, %u pads, %u links.\n", __FUNCTION__, topology.num_entities, topology.num_interfaces, topology.num_pads, topology.num_links);
			}

			//Allocate memory
			rc =  media_controller_topology_allocate(&media->topology, topology.num_entities, topology.num_interfaces, topology.num_pads, topology.num_links);
			if(rc<0){ee=-4; goto fail;}

			//The second request is to get the current data.
			rc =  ioctl(media->fd, MEDIA_IOC_G_TOPOLOGY, media->topology);
			if(rc<0){ee=-5; goto fail;}

			if(topology.topology_version == media->topology->topology_version)
				break;

			media_controller_topology_free(&media->topology);
		}

		//Show debug info
		media_controller_topology_log(media);
	}

	ee = 0;
fail:
	if(ee<0)
	{
		media_controller_close(media);
	}
	switch(ee)
	{
		case 0:
			break;
		default:
			syslog(LOG_ERR, "%s():  Unknown error code: %d,  errno: %d (%s)!\n", __FUNCTION__, ee, errno, strerror(errno));
			break;
	}
	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Opens the Video Subdevice over the Media Controller Device.
*
*  This function opens the video subdevice over the media controller device.
*/
/*-----------------------------------------------------------------------------*/
int  media_controller_sensor_subdev_open(int *fdSubdev, struct media_v2_pad **pad, struct media_v2_entity **entity, struct media_v2_link **link, struct media_v2_interface **interface, char *dev_video_device, VCMipiMediaCfg *media)
{
	I32    ee, rc;
	U32    idx;
	U32    senMajor, senMinor;
	char   acDevicefile[101]={'\0'};


	//Get the major and minor device number of the sensor
	{
		struct  stat  statSen;

		rc =  stat(dev_video_device, &statSen);
		if(rc<0){ee=-1; goto end;}

		senMajor =  major(statSen.st_rdev);
		senMinor =  minor(statSen.st_rdev);
	}

	//Search for the interface assigned to this major/minor number
	for(idx= 0; idx< media->topology->num_interfaces; idx++)
	{
		*interface = &((struct media_v2_interface *)(intptr_t)media->topology->ptr_interfaces)[idx];

		if((senMajor==(*interface)->devnode.major)&&(senMinor==(*interface)->devnode.minor))
		{
			syslog(LOG_DEBUG, "%s:  A. '%s'-> Device Node:  (major, minor):(%u,%u) -> Sensor Interface is #%u:  Type 0x%08x   \n", __FUNCTION__, dev_video_device, (*interface)->devnode.major, (*interface)->devnode.minor, (*interface)->id, (*interface)->intf_type);

			break;
		}
	}
	if(idx==media->topology->num_interfaces)
	{
		ee=+1; goto end;  //Sensor not found.
	}


	//Search for the link source connecting from this interface to the sensor entity as sink
	for(idx= 0; idx< media->topology->num_links; idx++)
	{
		(*link) = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[idx];

		if((*interface)->id==(*link)->source_id)
		{
			syslog(LOG_DEBUG, "%s:  B. Link #%u:   Source: @%u  Sink: @%u  Flags: 0x%08x\n", __FUNCTION__, (*link)->id, (*link)->source_id, (*link)->sink_id, (*link)->flags);

			break;
		}
	}
	if(idx==media->topology->num_links)
	{
		ee=-2; goto end; //No link connected to the sensor interface
	}


	//Search for the entity of the sensor
	for(idx= 0; idx< media->topology->num_entities; idx++)
	{
		(*entity) = &((struct media_v2_entity *)(intptr_t)media->topology->ptr_entities)[idx];

		if((*entity)->id==(*link)->sink_id)
		{
			syslog(LOG_DEBUG, "%s:  C. Entity #%u: '%s'\n", __FUNCTION__, (*entity)->id, (*entity)->name);

			break;
		}
	}
	if(idx==media->topology->num_entities)
	{
		ee=-3; goto end; //No entity for the sensor
	}


	//Assuming only one source pad is attached to this sensor
	for(idx= 0; idx< media->topology->num_pads; idx++)
	{
		(*pad) = &((struct media_v2_pad *)(intptr_t)media->topology->ptr_pads)[idx];

		if((*entity)->id == (*pad)->entity_id)
		{
			syslog(LOG_DEBUG, "%s:  D. Pad #%u: Index %u, flags 0x%08x\n", __FUNCTION__, (*pad)->id, (*pad)->index, (*pad)->flags);

			break;
		}
	}
	if(idx==media->topology->num_pads)
	{
		ee=-4; goto end; //No pad at the sensor entity
	}


	//Search for the link sink connecting from this pad to the sensor entity as source
	{
		for(idx= 0; idx< media->topology->num_links; idx++)
		{
			(*link) = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[idx];

			if((*pad)->id==(*link)->sink_id)
			{
				syslog(LOG_DEBUG, "%s:  E. Link #%u:   Source: @%u  Sink: @%u  Flags: 0x%08x\n", __FUNCTION__, (*link)->id, (*link)->source_id, (*link)->sink_id, (*link)->flags);

				break;
			}
		}
	}
	if(idx==media->topology->num_links)
	{
		ee=-5; goto end; //No link connected to the sensor interface
	}


	//Follow the link to the pad of the desired entity
	for(idx= 0; idx< media->topology->num_pads; idx++)
	{
		(*pad) = &((struct media_v2_pad *)(intptr_t)media->topology->ptr_pads)[idx];

		if((*link)->source_id == (*pad)->id)
		{
			syslog(LOG_DEBUG, "%s:  F. Pad #%u: Index %u, flags 0x%08x\n", __FUNCTION__, (*pad)->id, (*pad)->index, (*pad)->flags);

			break;
		}
	}
	if(idx==media->topology->num_pads)
	{
		ee=-6; goto end; //No pad at the sensor entity
	}


	//Get the entity of the sensor subdevice
	for(idx= 0; idx< media->topology->num_entities; idx++)
	{
		(*entity) = &((struct media_v2_entity *)(intptr_t)media->topology->ptr_entities)[idx];

		if((*entity)->id==(*pad)->entity_id)
		{
			syslog(LOG_DEBUG, "%s:  G. Entity #%u: '%s'\n", __FUNCTION__, (*entity)->id, (*entity)->name);

			break;
		}
	}
	if(idx==media->topology->num_entities)
	{
		ee=-7; goto end; //No entity for the sensor
	}


	//Search for the link source connecting from this interface to the sensor entity as sink
	{
		for(idx= 0; idx< media->topology->num_links; idx++)
		{
			(*link) = &((struct media_v2_link *)(intptr_t)media->topology->ptr_links)[idx];

			if((*entity)->id==(*link)->sink_id)
			{
				syslog(LOG_DEBUG, "%s:  H. Link #%u:   Source: @%u  Sink: @%u  Flags: 0x%08x\n", __FUNCTION__, (*link)->id, (*link)->source_id, (*link)->sink_id, (*link)->flags);

				break;
			}
		}
	}
	if(idx==media->topology->num_links)
	{
		ee=-8; goto end; //No link connected to the sensor interface
	}


	//Search for the interface connected to this entity
	for(idx= 0; idx< media->topology->num_interfaces; idx++)
	{
		*interface = &((struct media_v2_interface *)(intptr_t)media->topology->ptr_interfaces)[idx];

		if((*interface)->id==(*link)->source_id)
		{
			syslog(LOG_DEBUG, "%s:  I. Sensor Interface is #%u:  Type 0x%08x   Device Node:  (major, minor):(%u,%u)\n", __FUNCTION__, (*interface)->id, (*interface)->intf_type, (*interface)->devnode.major, (*interface)->devnode.minor);

			break;
		}
	}
	if(idx==media->topology->num_interfaces)
	{
		ee=-9; goto end;  //Sensor not found.
	}


	//Maybe there is a more elegant way to open the device.
	//Looking up the /dev node by the major/minor number for getting a file descriptor by open() later
	{
		char  acFilename[101];
		int   foundDeviceIff1=0;

		memset(acFilename, '\0', sizeof(acFilename));
		snprintf(acFilename, sizeof(acFilename)-1,"/sys/dev/char/%u:%u/uevent", (*interface)->devnode.major, (*interface)->devnode.minor);

		{
			FILE *pF=NULL;
			char  acKey[101];
			char  acValue[51];
			char  acScan[101];

			sprintf(acScan, "%%%u[^=]=%%%us%%*[\r\n]",  (unsigned int)sizeof(acKey)-1, (unsigned int)sizeof(acValue)-1);

			pF =  fopen(acFilename, "r");
			if(NULL==pF){ee=-10; goto end;}

			while(1)
			{
				rc =  fscanf(pF, acScan, acKey, acValue);
				if(EOF==rc){break;}
				if(2!=rc){continue;}
				if(0==strcmp("DEVNAME",acKey))
				{
					foundDeviceIff1 = 1;
					snprintf(acDevicefile, sizeof(acDevicefile)-1, "/dev/%s", acValue);
					break;
				}
			}
			fclose(pF);

			if(1!=foundDeviceIff1){ee=-11; goto end;}

			syslog(LOG_DEBUG, "%s:  => devFile of Sensor Video Subdevice:  '%s'\n", __FUNCTION__, acDevicefile);
		}
	}

	//Finally open the file of the subdevice
	{
		(*fdSubdev) =  open(acDevicefile, O_RDWR, 0);
		if((*fdSubdev)<0){ee=-12; goto end;}
	}


	ee = 0;
end:
	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Sets the Cropping Region for the Sensor.
*
*  This function sets the cropping region for the sensor.
*  Not all values are supported by all Platforms/Sensors!
*/
/*-----------------------------------------------------------------------------*/
int  media_controller_sensor_cropping_set(struct media_v2_pad *pad, int fdSubdev, int x0, int y0, int dx, int dy)
{
	int  rc, ee;
	struct v4l2_subdev_selection  selection;
	struct v4l2_subdev_crop       crop;
	struct v4l2_rect              rect;

	if((x0<0)||(y0<0)||(dx<1)||(dy<1)){ee=+1; goto fail;}

	memset(&rect, 0, sizeof(struct v4l2_rect));
	rect.top    = y0;
	rect.left   = x0;
	rect.height = dy;
	rect.width  = dx;

	memset(&selection, 0, sizeof(struct v4l2_subdev_selection));
	selection.pad    = pad->index;
	selection.target = V4L2_SEL_TGT_CROP;
	selection.which  = V4L2_SUBDEV_FORMAT_ACTIVE;
	selection.r      = rect;

	rc =  ioctl(fdSubdev, VIDIOC_SUBDEV_S_SELECTION, &selection);
	if(rc<0){ee=-1; goto fail;}


	memset(&crop, 0, sizeof(struct v4l2_subdev_crop));
	crop.pad    = pad->index;
	crop.which  = V4L2_SUBDEV_FORMAT_ACTIVE;
	crop.rect   = rect;

	rc =  ioctl(fdSubdev, VIDIOC_SUBDEV_S_CROP, &crop);
	if(rc<0){ee=-2; goto fail;}


	ee=0;
fail:
	return(ee);
}




/*--*FUNCTION*-----------------------------------------------------------------*/
/**
* @brief  Filters for Filenames starting with 'media'.
*
*  This function filters for filenames starting with 'media'.
*/
/*-----------------------------------------------------------------------------*/
int  scandir_filter_media(const struct dirent *dirEntry)
{
	if(0==strncmp(dirEntry->d_name,"media",5)){ return(1); }else{ return(0); }
}

/*--*FUNCTION*-----------------------------------------------------------------*/
/**
*
* @note  This function (and its childs) is included for reference only:
*        The RaspberryPi with linux kernel v.<=4.19 with commented out
*        request for unpacked format at sensor_open() could work so.
*
*
* @brief  Sets the Region of Interest for the Sensor, Not all values are supported by all Platforms/Sensors!
*
*  This function sets a region of interest given.
*  To do so, the media control device with the sensor has to be opened,
*  a file descriptor of the video4linux subdevice will be received,
*  and finally with this file descriptor, the region of interest will be programmed.
*
*  The Media Device framework remembers the applied settings.
*
* @note  Not all values are applicable, the driver will not give a warning!
*/
/*-----------------------------------------------------------------------------*/
int  media_set_roi(char *pcVideoDev, int optX0, int optY0, int optWidth, int optHeight)
{
	const char      acDev[] = "/dev";

	int             ee, rc;
	int             fdSubdev   =-1;
	struct dirent **dirEntry   =NULL;
	int             dirEntries =-1;
	char            acMediaDev[511] = {'\0'};
	int             i;

	VCMipiMediaCfg             media     = NULL_VCMipiMediaCfg;
	struct media_v2_pad       *pad       = NULL;
	struct media_v2_entity    *entity    = NULL;
	struct media_v2_link      *link      = NULL;
	struct media_v2_interface *interface = NULL;


	//scandir allocates dirEntry!
	dirEntries =  scandir(acDev, &dirEntry, scandir_filter_media, alphasort);
	if(dirEntries<0){ee=-1; goto fail;}

	//Search for the video device at /dev/media* until found.
	for(i= 0; i< dirEntries; i++)
	{
		snprintf(acMediaDev, sizeof(acMediaDev)-1, "%s/%s", acDev, dirEntry[i]->d_name);

		rc =  media_controller_open(acMediaDev, &media);
		if(rc<0){ee=-2+10*rc; goto fail;}

		rc =  media_controller_sensor_subdev_open(&fdSubdev, &pad, &entity, &link, &interface, pcVideoDev, &media);
		if(0==rc)
		{
			rc =  media_controller_sensor_cropping_set(pad, fdSubdev, optX0, optY0, optWidth, optHeight);
			if(rc<0){ee=-3+10*rc; goto fail;}

			ee= 0;  goto success;
		}

		if(fdSubdev>=0){ close(fdSubdev); fdSubdev = -1; }
		media_controller_close(&media);
	}

	ee=-4;
fail:
success:
	if(fdSubdev>=0){ close(fdSubdev); fdSubdev = -1; }
	media_controller_close(&media);

	if(NULL!=dirEntry){ for(;dirEntries>0;dirEntries--){ free(dirEntry[dirEntries-1]); } free(dirEntry); dirEntry=NULL; }

	return(ee);
}

/***************************************************************************/
/***************************************************************************/



void  sig_handler(int signo)
{
	static   volatile  sig_atomic_t   processing_signal_iff1 = 0;
	static   volatile  int            firstTryIff1 = 1;

	if((1==processing_signal_iff1)||(1!=firstTryIff1))
	{
		signal(signo, SIG_DFL);
		raise(signo);
	}
	else
	{
		processing_signal_iff1 = 1;

		switch(signo)
		{
			case SIGINT:
			case SIGTERM:
			case SIGQUIT:

				globVarQuitIff1 = 1;

				break;
		}

		processing_signal_iff1 = 0;
	}

	firstTryIff1 = 0;
}


#ifdef __cplusplus
#ifdef WITH_OPENCV

	using namespace cv;

	/*--*FUNCTION*-----------------------------------------------------------------*/
	/**
	* @brief  OpenCV Processing demonstration.
	*
	*  This function demonstrates the Laplace filter applied to a recorded image.
	*/
	/*-----------------------------------------------------------------------------*/
	I32  openCV_example(image *imgIn, image *imgOut)
	{
		I32      ee, ch;
		U8      *bufIn = NULL, *bufOut = NULL;
		cv::Mat  cvImageIn, cvImageOut;

		bufIn  = (U8*) malloc(3 * sizeof(U8) * imgIn->pitch  * imgIn->dy );
		if(NULL==bufIn ){ee=-1; goto fail;}
		bufOut = (U8*) malloc(3 * sizeof(U8) * imgOut->pitch * imgOut->dy);
		if(NULL==bufOut){ee=-2; goto fail;}

		ch         =  (imgIn->type==IMAGE_RGB)?(3):(1);
		cvImageIn  =  cv::Mat(imgIn->dy,  imgIn->dx,  (3==ch)?(CV_8UC3):(CV_8UC1), bufIn );
		cvImageOut =  cv::Mat(imgOut->dy, imgOut->dx, (3==ch)?(CV_8UC3):(CV_8UC1), bufOut);

		// opencv stores an RGB image as BGR and not per plane but interleaved!
		{

			switch(ch)
			{
				case 3: //RGB
				{
					for(int y=0; y< imgIn->dy; y++)
					{
						U8 *pr =  imgIn->st     + imgIn->pitch * y;
						U8 *pg =  imgIn->ccmp1  + imgIn->pitch * y;
						U8 *pb =  imgIn->ccmp2  + imgIn->pitch * y;

						U8 *po =  bufIn         + 3 * imgIn->dx * y;

						for(int x=0; x< imgIn->dx; x++)
						{
							*po++ = *pb++;
							*po++ = *pg++;
							*po++ = *pr++;
						}
					}
				}
				break;
				case 1: //Grey
				{
					for(int y=0; y< imgIn->dy; y++)
					{
						U8 *pg =  imgIn->st + imgIn->pitch * y;
						U8 *po =  bufIn     +    imgIn->dx * y;

						memcpy(po, pg, imgIn->dx);
					}
				}
				break;
				default: ee= -3; goto fail;
			}
		}

		try
		{
			cv::dilate(cvImageIn, cvImageOut, Mat(), Point(-1,-1), 5);
			// Example for copying only:  cvImageIn.copyTo(cvImageOut);
		}
		catch(...)
		{
			ee=-4; goto fail;
		}

		//convert back
		{
			if(imgIn->type!=imgOut->type){ee=-5; goto fail;}

			switch(ch)
			{
				case 3: //RGB
				{
					for(int y=0; y< min(imgIn->dy,imgOut->dy); y++)
					{
						U8 *pr =  imgOut->st     + imgOut->pitch * y;
						U8 *pg =  imgOut->ccmp1  + imgOut->pitch * y;
						U8 *pb =  imgOut->ccmp2  + imgOut->pitch * y;

						U8 *pi =  bufOut        + 3 * imgIn->dx * y;

						for(int x= 0; x< min(imgIn->dx,imgOut->dx); x++)
						{
							*pb++ = *pi++;
							*pg++ = *pi++;
							*pr++ = *pi++;
						}
					}
				}
				break;
				case 1: //Grey
				{
					for(int y=0; y< min(imgIn->dy,imgOut->dy); y++)
					{
						U8 *pg =  bufOut     + imgIn->dx     * y;
						U8 *pi =  imgOut->st + imgOut->pitch * y;

						memcpy(pi, pg, min(imgIn->dx,imgOut->dx));
					}
				}
				break;
				default: ee= -6; goto fail;
			}
		}


		ee=0;
	fail:
		if(NULL!=bufIn ){ free(bufIn ); bufIn =NULL; }
		if(NULL!=bufOut){ free(bufOut); bufOut=NULL; }

		return(ee);
	}

#endif
#endif

