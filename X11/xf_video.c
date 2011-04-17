/*
   FreeRDP: A Remote Desktop Protocol client.
   UI XVideo

   Copyright (C) Vic Lee 2011

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>
#include "xf_types.h"
#include "xf_video.h"

#ifdef HAVE_XV

int
xf_video_init(xfInfo * xfi)
{
	unsigned int version;
	unsigned int release;
	unsigned int request_base;
	unsigned int event_base;
	unsigned int error_base;
	unsigned int num_adaptors;
	unsigned int i;
	int ret;
	XvAdaptorInfo * ai;
	XvAttribute * attr;

	xfi->xv_port = -1;
	xfi->xv_colorkey_atom = None;
	xfi->xv_image_size = 0;

	if (!XShmQueryExtension(xfi->display))
	{
		printf("xf_video_init: no shmem available.\n");
		return 1;
	}

	ret = XvQueryExtension(xfi->display, &version, &release, &request_base, &event_base, &error_base);
	if (ret != Success)
	{
		printf("xf_video_init: XvQueryExtension failed %d.\n", ret);
		return 1;
	}
	printf("xf_video_init:");
	printf(" version %u", version);
	printf(" release %u", release);
	printf("\n");

	ret = XvQueryAdaptors(xfi->display, DefaultRootWindow(xfi->display),
		&num_adaptors, &ai);
	if (ret != Success)
	{
		printf("xf_video_init: XvQueryAdaptors failed %d.\n", ret);
		return 1;
	}

	for (i = 0; i < num_adaptors; i++)
	{
		printf("xf_video_init: adapter port %ld (%s)\n", ai[i].base_id, ai[i].name);
		xfi->xv_port = ai[i].base_id;
	}

	if (num_adaptors > 0)
		XvFreeAdaptorInfo(ai);

	if (xfi->xv_port == -1)
	{
		printf("xf_video_init: no adapter selected, video frames will not be processed.\n");
		return 1;
	}
	printf("xf_video_init: selected %ld\n", xfi->xv_port);

	attr = XvQueryPortAttributes(xfi->display, xfi->xv_port, &ret);
	for (i = 0; i < (unsigned int)ret; i++)
	{
		if (strcmp(attr[i].name, "XV_COLORKEY") == 0)
		{
			xfi->xv_colorkey_atom = XInternAtom(xfi->display, "XV_COLORKEY", False);
			XvSetPortAttribute(xfi->display, xfi->xv_port, xfi->xv_colorkey_atom, attr[i].min_value + 1);
			break;
		}
	}
	XFree(attr);

	return 0;
}

int
xf_video_process_frame(xfInfo * xfi, RD_VIDEO_FRAME_EVENT * vevent)
{
	XShmSegmentInfo shminfo;
	XvImage * image;
	int colorkey = 0;
	int i;

	if (xfi->xv_port == -1)
		return 1;

	/* In case the player is minimized */
	if (vevent->x < -2048 || vevent->y < -2048 || vevent->num_visible_rects <= 0)
		return 0;

	if (xfi->xv_colorkey_atom != None)
	{
		XvGetPortAttribute(xfi->display, xfi->xv_port, xfi->xv_colorkey_atom, &colorkey);
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		XSetForeground(xfi->display, xfi->gc, colorkey);
		for (i = 0; i < vevent->num_visible_rects; i++)
		{
			XFillRectangle(xfi->display, xfi->wnd, xfi->gc,
				vevent->x + vevent->visible_rects[i].x,
				vevent->y + vevent->visible_rects[i].y,
				vevent->visible_rects[i].width,
				vevent->visible_rects[i].height);
		}
	}

	image = XvShmCreateImage(xfi->display, xfi->xv_port,
		vevent->frame_pixfmt, 0, vevent->frame_width, vevent->frame_height, &shminfo);
	if (xfi->xv_image_size != image->data_size)
	{
		if (xfi->xv_image_size > 0)
		{
			shmdt(xfi->xv_shmaddr);
			shmctl(xfi->xv_shmid, IPC_RMID, NULL);
		}
		xfi->xv_image_size = image->data_size;
		xfi->xv_shmid = shmget(IPC_PRIVATE, image->data_size, IPC_CREAT | 0777);
		xfi->xv_shmaddr = shmat(xfi->xv_shmid, 0, 0);
	}
	shminfo.shmid = xfi->xv_shmid;
	shminfo.shmaddr = image->data = xfi->xv_shmaddr;
	shminfo.readOnly = False;
  
	if (!XShmAttach(xfi->display, &shminfo))
	{
		XFree(image);
		printf("xf_video_process_frame: XShmAttach failed.\n");
		return 1;
	}

	/* The video driver may align each line to a different size
	   and we need to convert our original image data. */
	switch (vevent->frame_pixfmt)
	{
		case RD_PIXFMT_I420:
			/* Y */
			if (image->pitches[0] == vevent->frame_width)
			{
				memcpy(image->data + image->offsets[0],
					vevent->frame_data,
					vevent->frame_width * vevent->frame_height);
			}
			else
			{
				for (i = 0; i < vevent->frame_height; i++)
				{
					memcpy(image->data + image->offsets[0] + i * image->pitches[0],
						vevent->frame_data + i * vevent->frame_width,
						vevent->frame_width);
				}
			}
			/* UV */
			if (image->pitches[1] * 2 == vevent->frame_width)
			{
				memcpy(image->data + image->offsets[1],
					vevent->frame_data + vevent->frame_width * vevent->frame_height,
					vevent->frame_width * vevent->frame_height / 4);
				memcpy(image->data + image->offsets[2],
					vevent->frame_data + vevent->frame_width * vevent->frame_height +
					vevent->frame_width * vevent->frame_height / 4,
					vevent->frame_width * vevent->frame_height / 4);
			}
			else
			{
				for (i = 0; i < vevent->frame_height / 2; i++)
				{
					memcpy(image->data + image->offsets[1] + i * image->pitches[1],
						vevent->frame_data + vevent->frame_width * vevent->frame_height + i * vevent->frame_width / 2,
						vevent->frame_width / 2);
					memcpy(image->data + image->offsets[2] + i * image->pitches[2],
						vevent->frame_data + vevent->frame_width * vevent->frame_height +
						vevent->frame_width * vevent->frame_height / 4 + i * vevent->frame_width / 2,
						vevent->frame_width / 2);
				}
			}
			break;

		default:
			memcpy(image->data, vevent->frame_data, image->data_size <= vevent->frame_size ?
				image->data_size : vevent->frame_size);
			break;
	}

	XvShmPutImage(xfi->display, xfi->xv_port, xfi->wnd, xfi->gc_default, image,
		0, 0, image->width, image->height,
		vevent->x, vevent->y, vevent->width, vevent->height, False);
	XSync(xfi->display, False);

	XShmDetach(xfi->display, &shminfo);
	XFree(image);

	return 0;
}

void
xf_video_uninit(xfInfo * xfi)
{
	if (xfi->xv_image_size > 0)
	{
		shmdt(xfi->xv_shmaddr);
		shmctl(xfi->xv_shmid, IPC_RMID, NULL);
	}
}

#else

int
xf_video_init(xfInfo * xfi)
{
	printf("xf_video_init: XVideo extenstion is disabled.\n");
	xfi->xv_port = -1;
	return 1;
}

int
xf_video_process_frame(xfInfo * xfi, RD_VIDEO_FRAME_EVENT * vevent)
{
	return 1;
}

void
xf_video_uninit(xfInfo * xfi)
{
}

#endif

