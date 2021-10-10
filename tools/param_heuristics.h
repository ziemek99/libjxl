// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef TOOLS_PARAM_HEURISTICS_H_
#define TOOLS_PARAM_HEURISTICS_H_

namespace jpegxl {
namespace tools {

// Returns number of colors from PNG file's PLTE chunk, or 0 if PNG file doesn't
// have it or if it's not a PNG file to begin with. Used in command-line tools.
int PalettizedColorsPNG(const char* file_in);

}  // namespace tools
}  // namespace jpegxl

#endif  // TOOLS_PARAM_HEURISTICS_H_
