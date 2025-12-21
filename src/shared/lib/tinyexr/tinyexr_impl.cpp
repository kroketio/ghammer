// The tinyexr implementation (tinyexr.h, BSD-3, as vendored by Godot's
// thirdparty/tinyexr 3.2.0): system zlib instead of miniz, threaded decoding.
#include <zlib.h>  // before tinyexr

#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 0
#define TINYEXR_USE_THREAD 1
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
