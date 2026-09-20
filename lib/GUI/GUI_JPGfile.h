#ifndef __GUI_JPGFILE_H
#define __GUI_JPGFILE_H

#include "DEV_Config.h"

/*
 * Decode a baseline JPEG from the SD card straight into the 7-color
 * frame buffer: EXIF orientation, portrait photos turned by 90 degrees,
 * scaled and center-cropped to 800x480, Floyd-Steinberg dithered to the
 * panel palette.
 *
 * Returns 0 on success, otherwise non-zero and *err points to a short
 * human readable reason.
 */
UBYTE GUI_ReadJpg_7Color(const char *path, const char **err);

#endif
