/* libjpeg-turbo's TurboJPEG API for the media tool's JPEGs. tj3Init is a macro from 3.2 on, which Swift cannot call. */
#ifndef SWITCH_TURBOJPEG_H
#define SWITCH_TURBOJPEG_H

#include <turbojpeg.h>

static inline tjhandle switch_tj3_compressor(void)
{
    return tj3Init(TJINIT_COMPRESS);
}

#endif
