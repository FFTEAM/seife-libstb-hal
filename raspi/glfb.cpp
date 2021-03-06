/*
	Copyright 2013 Stefan Seyfried <seife@tuxboxcvs.slipkontur.de>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.

	The GLFB namespace is just because it's already established by the
	generic-pc implementation.
*/

#include <vector>
#include <OpenThreads/Condition>

#include "glfb.h"
#include "bcm_host.h"

#include "lt_debug.h"

#define lt_debug_c(args...) _lt_debug(HAL_DEBUG_INIT, NULL, args)
#define lt_info_c(args...) _lt_info(HAL_DEBUG_INIT, NULL, args)
#define lt_debug(args...) _lt_debug(HAL_DEBUG_INIT, this, args)
#define lt_info(args...) _lt_info(HAL_DEBUG_INIT, this, args)

/* I don't want to assert right now */
#define CHECK(x) if (!(x)) { lt_info("GLFB: %s:%d warning: %s\n", __func__, __LINE__, #x); }

/* TODO: encapsulate this into pdata? */
static DISPMANX_RESOURCE_HANDLE_T res;
static uint32_t                   vc_img_ptr;
static DISPMANX_UPDATE_HANDLE_T   update;
static DISPMANX_ELEMENT_HANDLE_T  element;
static DISPMANX_DISPLAY_HANDLE_T  display;
/* this is shared with other parts of libstb-hal, thus not static */
DISPMANX_MODEINFO_T               output_info;
static VC_RECT_T                  dst_rect;
static void *image;
static int pitch;
static VC_IMAGE_TYPE_T type = VC_IMAGE_ARGB8888;
/* second element: black background which covers the framebuffer */
static DISPMANX_RESOURCE_HANDLE_T bg_res;
static uint32_t                   bg_img_ptr;
static DISPMANX_ELEMENT_HANDLE_T  bg_element;
static VC_IMAGE_TYPE_T bg_type = VC_IMAGE_RGB565;

static OpenThreads::Mutex blit_mutex;
static OpenThreads::Condition blit_cond;

static bool goodbye = false;	/* if set main loop is left */
static bool ready = false;	/* condition predicate */

static int width;		/* width and height, fixed for a framebuffer instance */
static int height;

GLFramebuffer::GLFramebuffer(int x, int y)
{
	width = x;
	height = y;

	/* linux framebuffer compat mode */
	si.bits_per_pixel = 32;
	si.xres = si.xres_virtual = width;
	si.yres = si.yres_virtual = height;
	si.blue.length = si.green.length = si.red.length = si.transp.length = 8;
	si.blue.offset = 0;
	si.green.offset = 8;
	si.red.offset = 16;
	si.transp.offset = 24;

	OpenThreads::Thread::start();
	while (!ready)
		usleep(1);
}

GLFramebuffer::~GLFramebuffer()
{
	int ret;
	goodbye = true;
	blit(); /* wake up thread */
	OpenThreads::Thread::join();
	update = vc_dispmanx_update_start(10);
	CHECK(update);
	ret = vc_dispmanx_element_remove(update, element);
	CHECK(ret == 0);
	ret = vc_dispmanx_element_remove(update, bg_element);
	CHECK(ret == 0);
	ret = vc_dispmanx_update_submit_sync(update);
	CHECK(ret == 0);
	ret = vc_dispmanx_resource_delete(res);
	CHECK(ret == 0);
	ret = vc_dispmanx_resource_delete(bg_res);
	CHECK(ret == 0);
	ret = vc_dispmanx_display_close(display);
	CHECK(ret == 0);
}

void GLFramebuffer::run()
{
	hal_set_threadname("hal:fbuff");
	setup();
	ready = true; /* signal that setup is finished */
	blit_mutex.lock();
	while (!goodbye)
	{
		blit_cond.wait(&blit_mutex);
		blit_osd();
	}
	blit_mutex.unlock();
	lt_info("GLFB: GL thread stopping\n");
}

void GLFramebuffer::blit()
{
	blit_mutex.lock();
	blit_cond.signal();
	blit_mutex.unlock();
}

void GLFramebuffer::setup()
{
	lt_info("GLFB: raspi OMX fb setup\n");
	int ret;
	VC_RECT_T src_rect, dsp_rect; /* source and display size will not change. period. */
	pitch = ALIGN_UP(width * 4, 32);
	/* broadcom example code has this ALIGN_UP in there for a reasin, I suppose */
	if (pitch != width * 4)
		lt_info("GLFB: WARNING: width not a multiple of 8? I doubt this will work...\n");

	/* global alpha nontransparent (255) */
	VC_DISPMANX_ALPHA_T alpha = { DISPMANX_FLAGS_ALPHA_FROM_SOURCE, 255, 0 };

	// bcm_host_init(); // already done in AVDec()

	display = vc_dispmanx_display_open(0);
	ret = vc_dispmanx_display_get_info(display, &output_info);
	CHECK(ret == 0);
	/* 32bit FB depth, *2 because tuxtxt uses a shadow buffer */
	osd_buf.resize(pitch * height * 2);
	lt_info("GLFB: Display is %d x %d, FB is %d x %d, memory size %d\n",
		output_info.width, output_info.height, width, height, osd_buf.size());
	/* black background */
	uint16_t bg[16]; /* 16 bpp, 16x1 bitmap. smaller is not useful, need pitch 32 anyway  */
	memset(bg, 0, sizeof(bg));
	bg_res = vc_dispmanx_resource_create(bg_type, 16, 1, &bg_img_ptr);
	CHECK(bg_res);
	vc_dispmanx_rect_set(&dst_rect, 0, 0, 16, 1);
	ret = vc_dispmanx_resource_write_data(bg_res, bg_type, 16 * sizeof(uint16_t), bg, &dst_rect);
	CHECK(ret == 0);
	/* framebuffer */
	image = &osd_buf[0];
	/* initialize to half-transparent grey */
	memset(image, 0x7f, osd_buf.size());
	res = vc_dispmanx_resource_create(type, width, height, &vc_img_ptr);
	CHECK(res);
	vc_dispmanx_rect_set(&dst_rect, 0, 0, width, height);
	ret = vc_dispmanx_resource_write_data(res, type, pitch, image, &dst_rect);
	CHECK(ret == 0);
	update = vc_dispmanx_update_start(10);
	CHECK(update);
	/* background image to cover the linux fb */
	vc_dispmanx_rect_set(&src_rect, 0, 0, 16 << 16, 1 << 16);
	vc_dispmanx_rect_set(&dsp_rect, 0, 0, output_info.width, output_info.height);
	bg_element = vc_dispmanx_element_add(update,
					  display,
					  0, /* bg layer, linux framebuffer is -127 */
					  &dsp_rect,
					  bg_res,
					  &src_rect,
					  DISPMANX_PROTECTION_NONE,
					  &alpha,
					  NULL,
					  DISPMANX_NO_ROTATE);
	/* our real framebuffer */
	vc_dispmanx_rect_set(&src_rect, 0, 0, width << 16, height << 16);
	vc_dispmanx_rect_set(&dsp_rect, 0, 0, output_info.width, output_info.height);
	element = vc_dispmanx_element_add(update,
					  display,
					  2000 /*layer*/,
					  &dsp_rect,
					  res,
					  &src_rect,
					  DISPMANX_PROTECTION_NONE,
					  &alpha,
					  NULL,
					  DISPMANX_NO_ROTATE);
	ret = vc_dispmanx_update_submit_sync(update);
	CHECK(ret == 0);
}

void GLFramebuffer::blit_osd()
{
	int ret;
	ret = vc_dispmanx_resource_write_data(res, type, pitch, image, &dst_rect);
	CHECK(ret == 0);
	update = vc_dispmanx_update_start(10);
	CHECK(update);
	ret = vc_dispmanx_update_submit_sync(update);
	CHECK(ret == 0);
}
