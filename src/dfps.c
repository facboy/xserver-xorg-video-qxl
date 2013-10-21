/*
 * Copyright (C) 2012 CodeWeavers, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Jeremy White <jwhite@codeweavers.com>
 */

/*----------------------------------------------------------------------------
  Deferred Frames Per Second (dfps) Support File

    By default, The xorg-video-qxl driver transmits all of the video
  operation across the wire.  While this has the greatest fidelity, and
  lends itself well to a variety of optimizations and behavior tuning,
  it does not always use bandwidth efficiently.

  This file implements a 'deferred frames' mode which instead renders
  everything to a frame buffer, and then periodically sends only updated
  portions of the screen.

  For some use cases, this proves to be far more efficient with network
  resources.
----------------------------------------------------------------------------*/

#include <xorg-server.h>
#include "qxl.h"
#include "dfps.h"

typedef struct _dfps_info_t
{
    RegionRec   updated_region;

    PixmapPtr   copy_src;
    Pixel       solid_pixel;
    GCPtr       pgc;
} dfps_info_t;

static inline dfps_info_t *dfps_get_info (PixmapPtr pixmap)
{
#if HAS_DEVPRIVATEKEYREC
    return dixGetPrivate(&pixmap->devPrivates, &uxa_pixmap_index);
#else
    return dixLookupPrivate(&pixmap->devPrivates, &uxa_pixmap_index);
#endif
}

static inline void dfps_set_info (PixmapPtr pixmap, dfps_info_t *info)
{
    dixSetPrivate(&pixmap->devPrivates, &uxa_pixmap_index, info);
}
typedef struct FrameTimer {
    OsTimerPtr xorg_timer;
    FrameTimerFunc func;
    void *opaque; // also stored in xorg_timer, but needed for timer_start
} Timer;

static CARD32 xorg_timer_callback(
    OsTimerPtr xorg_timer,
    CARD32 time,
    pointer arg)
{
    FrameTimer *timer = (FrameTimer*)arg;

    timer->func(timer->opaque);
    return 0; // if non zero xorg does a TimerSet, we don't want that.
}

static FrameTimer* timer_add(FrameTimerFunc func, void *opaque)
{
    FrameTimer *timer = calloc(sizeof(FrameTimer), 1);

    timer->xorg_timer = TimerSet(NULL, 0, 1e9 /* TODO: infinity? */, xorg_timer_callback, timer);
    timer->func = func;
    timer->opaque = opaque;
    return timer;
}

static void timer_start(FrameTimer *timer, uint32_t ms)
{
    TimerSet(timer->xorg_timer, 0 /* flags */, ms, xorg_timer_callback, timer);
}

void dfps_start_ticker(qxl_screen_t *qxl)
{
    qxl->frames_timer = timer_add(dfps_ticker, qxl);
    timer_start(qxl->frames_timer, 1000 / qxl->deferred_fps);
}

void dfps_ticker(void *opaque)
{
    qxl_screen_t *qxl = (qxl_screen_t *) opaque;
    dfps_info_t *info = NULL;
    PixmapPtr pixmap;

    pixmap = qxl->pScrn->pScreen->GetScreenPixmap(qxl->pScrn->pScreen);
    if (pixmap)
        info = dfps_get_info(pixmap);
    if (info)
    {
        qxl_surface_upload_primary_regions(qxl, pixmap, &info->updated_region);
        RegionUninit(&info->updated_region);
        RegionInit(&info->updated_region, NULL, 0);
    }
    timer_start(qxl->frames_timer, 1000 / qxl->deferred_fps);
}


static Bool unaccel (void)
{
    return FALSE;
}

static Bool dfps_prepare_solid (PixmapPtr pixmap, int alu, Pixel planemask, Pixel fg)
{
    dfps_info_t *info;

    if (!(info = dfps_get_info (pixmap)))
        return FALSE;

    info->solid_pixel = fg;
    info->pgc = GetScratchGC(pixmap->drawable.depth, pixmap->drawable.pScreen);
    if (! info->pgc)
        return FALSE;

    info->pgc->alu = alu;
    info->pgc->planemask = planemask;
    info->pgc->fgPixel = fg;
    info->pgc->fillStyle = FillSolid;

    fbValidateGC(info->pgc, GCForeground | GCPlaneMask, &pixmap->drawable);

    return TRUE;
}

static void dfps_solid (PixmapPtr pixmap, int x_1, int y_1, int x_2, int y_2)
{
    struct pixman_box16 box;
    RegionPtr region;
    Bool throwaway_bool;
    dfps_info_t *info;

    if (!(info = dfps_get_info (pixmap)))
        return;

    /* Draw to the frame buffer */
    fbFill(&pixmap->drawable, info->pgc, x_1, y_1, x_2 - x_1, y_2 - y_1);

    /* Track the updated region */
    box.x1 = x_1; box.x2 = x_2; box.y1 = y_1; box.y2 = y_2;
    region = RegionCreate(&box, 1);
    RegionAppend(&info->updated_region, region);
    RegionValidate(&info->updated_region, &throwaway_bool);
    RegionUninit(region);
    return;
}

static void dfps_done_solid (PixmapPtr pixmap)
{
    dfps_info_t *info;

    if ((info = dfps_get_info (pixmap)))
    {
        FreeScratchGC(info->pgc);
        info->pgc = NULL;
    }
}

static Bool dfps_prepare_copy (PixmapPtr source, PixmapPtr dest,
                  int xdir, int ydir, int alu,
                  Pixel planemask)
{
    dfps_info_t *info;

    if (!(info = dfps_get_info (dest)))
        return FALSE;

    info->copy_src = source;

    info->pgc = GetScratchGC(dest->drawable.depth, dest->drawable.pScreen);
    if (! info->pgc)
        return FALSE;

    info->pgc->alu = alu;
    info->pgc->planemask = planemask;

    fbValidateGC(info->pgc, GCPlaneMask, &dest->drawable);

    return TRUE;
}

static void dfps_copy (PixmapPtr dest,
          int src_x1, int src_y1,
          int dest_x1, int dest_y1,
          int width, int height)
{
    struct pixman_box16 box;
    RegionPtr region;
    Bool throwaway_bool;

    dfps_info_t *info;

    if (!(info = dfps_get_info (dest)))
        return;

    /* Render into to the frame buffer */
    fbCopyArea(&info->copy_src->drawable, &dest->drawable, info->pgc, src_x1, src_y1, width, height, dest_x1, dest_y1);

    /* Update the tracking region */
    box.x1 = dest_x1; box.x2 = dest_x1 + width; box.y1 = dest_y1; box.y2 = dest_y1 + height;
    region = RegionCreate(&box, 1);
    RegionAppend(&info->updated_region, region);
    RegionValidate(&info->updated_region, &throwaway_bool);
    RegionUninit(region);
}

static void dfps_done_copy (PixmapPtr dest)
{
    dfps_info_t *info;

    if ((info = dfps_get_info (dest)))
    {
        FreeScratchGC(info->pgc);
        info->pgc = NULL;
    }
}

static Bool dfps_put_image (PixmapPtr dest, int x, int y, int w, int h,
               char *src, int src_pitch)
{
    struct pixman_box16 box;
    RegionPtr region;
    Bool throwaway_bool;
    dfps_info_t *info;

    if (!(info = dfps_get_info (dest)))
        return FALSE;

    box.x1 = x; box.x2 = x + w; box.y1 = y; box.y2 = y + h;
    region = RegionCreate(&box, 1);
    RegionAppend(&info->updated_region, region);
    RegionValidate(&info->updated_region, &throwaway_bool);
    RegionUninit(region);

    /* We can avoid doing the put image ourselves, as the uxa driver
       will fall back and do it for us if we return false */
    return FALSE;
}


static Bool dfps_prepare_access (PixmapPtr pixmap, RegionPtr region, uxa_access_t requested_access)
{
    fbPrepareAccess(pixmap);
    if (requested_access == UXA_ACCESS_RW)
    {
        dfps_info_t *info;
        Bool throwaway_bool;

        if (!(info = dfps_get_info (pixmap)))
            return FALSE;
        RegionAppend(&info->updated_region, region);
        RegionValidate(&info->updated_region, &throwaway_bool);
    }
    return TRUE;
}

static void dfps_finish_access (PixmapPtr pixmap)
{
    fbFinishAccess(pixmap);
}

static Bool dfps_pixmap_is_offscreen (PixmapPtr pixmap)
{
    return !!dfps_get_info(pixmap);
}

static void dfps_set_screen_pixmap (PixmapPtr pixmap)
{
    pixmap->drawable.pScreen->devPrivate = pixmap;
}

static PixmapPtr dfps_create_pixmap (ScreenPtr screen, int w, int h, int depth, unsigned usage)
{
    PixmapPtr pixmap;
    dfps_info_t *info;

    info = calloc(1, sizeof(*info));
    if (!info)
        return FALSE;
    RegionInit(&info->updated_region, NULL, 0);

    pixmap = fbCreatePixmap (screen, w, h, depth, usage);
    if (pixmap)
        dfps_set_info(pixmap, info);
    else
        free(info);

    return pixmap;
}

static Bool dfps_destroy_pixmap (PixmapPtr pixmap)
{
    if (pixmap->refcnt == 1)
    {
        dfps_info_t *info = dfps_get_info (pixmap);
        if (info)
            free(info);
        dfps_set_info(pixmap, NULL);
    }

    return fbDestroyPixmap (pixmap);
}

void dfps_set_uxa_functions(qxl_screen_t *qxl, ScreenPtr screen)
{
    /* Solid fill */
    //qxl->uxa->check_solid = dfps_check_solid;
    qxl->uxa->prepare_solid = dfps_prepare_solid;
    qxl->uxa->solid = dfps_solid;
    qxl->uxa->done_solid = dfps_done_solid;

    /* Copy */
    //qxl->uxa->check_copy = qxl_check_copy;
    qxl->uxa->prepare_copy = dfps_prepare_copy;
    qxl->uxa->copy = dfps_copy;
    qxl->uxa->done_copy = dfps_done_copy;

    /* Composite */
    qxl->uxa->check_composite = (typeof(qxl->uxa->check_composite))unaccel;
    qxl->uxa->check_composite_target = (typeof(qxl->uxa->check_composite_target))unaccel;
    qxl->uxa->check_composite_texture = (typeof(qxl->uxa->check_composite_texture))unaccel;
    qxl->uxa->prepare_composite = (typeof(qxl->uxa->prepare_composite))unaccel;
    qxl->uxa->composite = (typeof(qxl->uxa->composite))unaccel;
    qxl->uxa->done_composite = (typeof(qxl->uxa->done_composite))unaccel;
    
    /* PutImage */
    qxl->uxa->put_image = dfps_put_image;

    /* Prepare access */
    qxl->uxa->prepare_access = dfps_prepare_access;
    qxl->uxa->finish_access = dfps_finish_access;

    /* General screen information */
    qxl->uxa->pixmap_is_offscreen = dfps_pixmap_is_offscreen;

    screen->SetScreenPixmap = dfps_set_screen_pixmap;
    screen->CreatePixmap = dfps_create_pixmap;
    screen->DestroyPixmap = dfps_destroy_pixmap;
}
