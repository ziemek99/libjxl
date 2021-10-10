// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "tools/param_heuristics.h"

#include <lodepng_util.h>

#include <string>

#include "lib/jxl/base/file_io.h"

namespace jpegxl {
namespace tools {

int PalettizedColorsPNG(const char* file_in) {
  if (jxl::Extension(file_in) == ".png") {
    return 256;
  }

  return 0;
}

}  // namespace tools
}  // namespace jpegxl
