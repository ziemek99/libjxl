// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <jxl/cms.h>
#include <stdio.h>
#include <stdlib.h>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "tools/hdr/local_tone_map.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/extras/codec.h"
#include "lib/extras/packed_image_convert.h"
#include "lib/extras/tone_mapping.h"
#include "lib/jxl/base/fast_math-inl.h"
#include "lib/jxl/convolve.h"
#include "lib/jxl/enc_gamma_correct.h"
#include "lib/jxl/image_bundle.h"
#include "tools/args.h"
#include "tools/cmdline.h"
#include "tools/thread_pool_internal.h"

HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {
namespace {

using ::hwy::HWY_NAMESPACE::Add;
using ::hwy::HWY_NAMESPACE::Div;
using ::hwy::HWY_NAMESPACE::Lt;
using ::hwy::HWY_NAMESPACE::Max;
using ::hwy::HWY_NAMESPACE::Min;
using ::hwy::HWY_NAMESPACE::Mul;
using ::hwy::HWY_NAMESPACE::MulAdd;
using ::hwy::HWY_NAMESPACE::Sub;

constexpr size_t kDownsampling = 128;

// Color components must be in linear Rec. 2020.
template <typename V>
V ComputeLuminance(const float intensity_target, const V r, const V g,
                   const V b) {
  hwy::HWY_NAMESPACE::DFromV<V> df;
  const auto luminance =
      Mul(Set(df, intensity_target),
          MulAdd(Set(df, 0.2627f), r,
                 MulAdd(Set(df, 0.6780f), g, Mul(Set(df, 0.0593f), b))));
  return Max(Set(df, 1e-12f), luminance);
}

ImageF DownsampledLuminances(const Image3F& image,
                             const float intensity_target) {
  HWY_CAPPED(float, kDownsampling) d;
  ImageF result(DivCeil(image.xsize(), kDownsampling),
                DivCeil(image.ysize(), kDownsampling));
  FillImage(kDefaultIntensityTarget, &result);
  for (size_t y = 0; y < image.ysize(); ++y) {
    const float* const JXL_RESTRICT rows[3] = {image.ConstPlaneRow(0, y),
                                               image.ConstPlaneRow(1, y),
                                               image.ConstPlaneRow(2, y)};
    float* const JXL_RESTRICT result_row = result.Row(y / kDownsampling);

    for (size_t x = 0; x < image.xsize(); x += kDownsampling) {
      auto max = Set(d, result_row[x / kDownsampling]);
      for (size_t kx = 0; kx < kDownsampling && x + kx < image.xsize();
           kx += Lanes(d)) {
        max =
            Max(max, ComputeLuminance(
                         intensity_target, Load(d, rows[0] + x + kx),
                         Load(d, rows[1] + x + kx), Load(d, rows[2] + x + kx)));
      }
      result_row[x / kDownsampling] = GetLane(MaxOfLanes(d, max));
    }
  }
  HWY_FULL(float) df;
  for (size_t y = 0; y < result.ysize(); ++y) {
    float* const JXL_RESTRICT row = result.Row(y);
    for (size_t x = 0; x < result.xsize(); x += Lanes(df)) {
      Store(FastLog2f(df, Load(df, row + x)), df, row + x);
    }
  }
  return result;
}

ImageF Upsample(const ImageF& image, ThreadPool* pool) {
  ImageF upsampled_horizontally(2 * image.xsize(), image.ysize());
  const auto BoundX = [&image](ssize_t x) {
    return Clamp1<ssize_t>(x, 0, image.xsize() - 1);
  };
  JXL_CHECK(RunOnPool(
      pool, 0, image.ysize(), &ThreadPool::NoInit,
      [&](const int32_t y, const int32_t /*thread_id*/) {
        const float* const JXL_RESTRICT in_row = image.ConstRow(y);
        float* const JXL_RESTRICT out_row = upsampled_horizontally.Row(y);

        for (ssize_t x = 0; x < static_cast<ssize_t>(image.xsize()); ++x) {
          out_row[2 * x] = in_row[x];
          out_row[2 * x + 1] =
              0.5625f * (in_row[x] + in_row[BoundX(x + 1)]) -
              0.0625f * (in_row[BoundX(x - 1)] + in_row[BoundX(x + 2)]);
        }
      },
      "UpsampleHorizontally"));

  HWY_FULL(float) df;
  ImageF upsampled(2 * image.xsize(), 2 * image.ysize());
  const auto BoundY = [&image](ssize_t y) {
    return Clamp1<ssize_t>(y, 0, image.ysize() - 1);
  };
  JXL_CHECK(RunOnPool(
      pool, 0, image.ysize(), &ThreadPool::NoInit,
      [&](const int32_t y, const int32_t /*thread_id*/) {
        const float* const JXL_RESTRICT in_rows[4] = {
            upsampled_horizontally.ConstRow(BoundY(y - 1)),
            upsampled_horizontally.ConstRow(y),
            upsampled_horizontally.ConstRow(BoundY(y + 1)),
            upsampled_horizontally.ConstRow(BoundY(y + 2)),
        };
        float* const JXL_RESTRICT out_rows[2] = {
            upsampled.Row(2 * y),
            upsampled.Row(2 * y + 1),
        };

        for (ssize_t x = 0;
             x < static_cast<ssize_t>(upsampled_horizontally.xsize());
             x += Lanes(df)) {
          Store(Load(df, in_rows[1] + x), df, out_rows[0] + x);
          Store(MulAdd(Set(df, 0.5625f),
                       Add(Load(df, in_rows[1] + x), Load(df, in_rows[2] + x)),
                       Mul(Set(df, -0.0625f), Add(Load(df, in_rows[0] + x),
                                                  Load(df, in_rows[3] + x)))),
                df, out_rows[1] + x);
        }
      },
      "UpsampleVertically"));
  return upsampled;
}

float ComputeOffset(const ImageF& original_luminances,
                    const ImageF& upsampled_blurred_luminances) {
  HWY_CAPPED(float, kDownsampling) df;
  float max_difference = 0.f;
  for (size_t y = 0; y < original_luminances.ysize(); ++y) {
    const float* const JXL_RESTRICT original_row =
        original_luminances.ConstRow(y);
    for (size_t x = 0; x < original_luminances.xsize(); ++x) {
      auto block_min = Set(df, std::numeric_limits<float>::infinity());
      for (size_t ky = 0; ky < kDownsampling; ++ky) {
        const float* const JXL_RESTRICT blurred_row =
            upsampled_blurred_luminances.ConstRow(kDownsampling * y + ky);
        for (size_t kx = 0; kx < kDownsampling; kx += Lanes(df)) {
          block_min =
              Min(block_min, Load(df, blurred_row + kDownsampling * x + kx));
        }
      }

      const float difference =
          original_row[x] - GetLane(MinOfLanes(df, block_min));
      if (difference > max_difference) max_difference = difference;
    }
  }
  return max_difference;
}

Status ApplyLocalToneMapping(const ImageF& blurred_luminances,
                             const float intensity_target,
                             const float max_difference, Image3F* color,
                             ThreadPool* pool) {
  HWY_FULL(float) df;

  const auto log_default_intensity_target =
      Set(df, FastLog2f(kDefaultIntensityTarget));
  const auto log_10000 = Set(df, FastLog2f(10000.f));
  JXL_RETURN_IF_ERROR(RunOnPool(
      pool, 0, color->ysize(), &ThreadPool::NoInit,
      [&](const int32_t y, const int32_t /*thread_id*/) {
        float* const JXL_RESTRICT rows[3] = {color->PlaneRow(0, y),
                                             color->PlaneRow(1, y),
                                             color->PlaneRow(2, y)};
        const float* const JXL_RESTRICT blurred_lum_row =
            blurred_luminances.ConstRow(y);

        for (size_t x = 0; x < color->xsize(); x += Lanes(df)) {
          const auto log_local_max =
              Add(Load(df, blurred_lum_row + x), Set(df, max_difference));
          const auto luminance =
              ComputeLuminance(intensity_target, Load(df, rows[0] + x),
                               Load(df, rows[1] + x), Load(df, rows[2] + x));
          const auto log_luminance = FastLog2f(df, luminance);
          const auto log_knee =
              Mul(log_default_intensity_target,
                  MulAdd(Set(df, -0.85f),
                         Div(Sub(log_local_max, log_default_intensity_target),
                             Sub(log_10000, log_default_intensity_target)),
                         Set(df, 1.f)));
          const auto second_segment_position =
              Div(Sub(log_luminance, log_knee), Sub(log_local_max, log_knee));
          const auto log_new_luminance = IfThenElse(
              Lt(log_luminance, log_knee), log_luminance,
              MulAdd(
                  second_segment_position,
                  MulAdd(Sub(log_default_intensity_target, log_knee),
                         second_segment_position, Sub(log_knee, log_luminance)),
                  log_luminance));
          const auto new_luminance = FastPow2f(df, log_new_luminance);
          const auto ratio =
              Div(Mul(Set(df, intensity_target), new_luminance),
                  Mul(luminance, Set(df, kDefaultIntensityTarget)));
          for (int c = 0; c < 3; ++c) {
            Store(Mul(ratio, Load(df, rows[c] + x)), df, rows[c] + x);
          }
        }
      },
      "ApplyLocalToneMapping"));

  return true;
}

}  // namespace
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace jxl {
namespace {

HWY_EXPORT(DownsampledLuminances);
HWY_EXPORT(Upsample);
HWY_EXPORT(ComputeOffset);
HWY_EXPORT(ApplyLocalToneMapping);

void Blur(ImageF* image) {
  static constexpr WeightsSeparable5 kBlurFilter = {
      {HWY_REP4(.375f), HWY_REP4(.25f), HWY_REP4(.0625f)},
      {HWY_REP4(.375f), HWY_REP4(.25f), HWY_REP4(.0625f)}};
  ImageF blurred_once(image->xsize(), image->ysize());
  Separable5(*image, Rect(*image), kBlurFilter, nullptr, &blurred_once);
  Separable5(blurred_once, Rect(blurred_once), kBlurFilter, nullptr, image);
}

void ProcessFrame(CodecInOut* image, float preserve_saturation,
                  ThreadPool* pool) {
  ColorEncoding linear_rec2020;
  JXL_CHECK(linear_rec2020.SetWhitePointType(WhitePoint::kD65));
  JXL_CHECK(linear_rec2020.SetPrimariesType(Primaries::k2100));
  linear_rec2020.Tf().SetTransferFunction(TransferFunction::kLinear);
  JXL_CHECK(linear_rec2020.CreateICC());
  JXL_CHECK(
      image->Main().TransformTo(linear_rec2020, *JxlGetDefaultCms(), pool));

  const float intensity_target = image->metadata.m.IntensityTarget();

  Image3F color = std::move(*image->Main().color());
  ImageF subsampled_image =
      HWY_DYNAMIC_DISPATCH(DownsampledLuminances)(color, intensity_target);
  ImageF original_luminances(subsampled_image.xsize(),
                             subsampled_image.ysize());
  CopyImageTo(subsampled_image, &original_luminances);

  Blur(&subsampled_image);
  const auto& Upsample = HWY_DYNAMIC_DISPATCH(Upsample);
  ImageF blurred_luminances = std::move(subsampled_image);
  for (int downsampling = HWY_NAMESPACE::kDownsampling; downsampling > 1;
       downsampling >>= 1) {
    blurred_luminances =
        Upsample(blurred_luminances, downsampling > 4 ? nullptr : pool);
  }

  const float max_difference = HWY_DYNAMIC_DISPATCH(ComputeOffset)(
      original_luminances, blurred_luminances);

  JXL_CHECK(HWY_DYNAMIC_DISPATCH(ApplyLocalToneMapping)(
      blurred_luminances, intensity_target, max_difference, &color, pool));

  image->SetFromImage(std::move(color), linear_rec2020);
  image->metadata.m.color_encoding = linear_rec2020;
  image->metadata.m.SetIntensityTarget(kDefaultIntensityTarget);

  JXL_CHECK(GamutMap(image, preserve_saturation, pool));

  ColorEncoding rec2020_srgb = linear_rec2020;
  rec2020_srgb.Tf().SetTransferFunction(TransferFunction::kSRGB);
  JXL_CHECK(rec2020_srgb.CreateICC());
  JXL_CHECK(image->Main().TransformTo(rec2020_srgb, *JxlGetDefaultCms(), pool));
  image->metadata.m.color_encoding = rec2020_srgb;
}

}  // namespace
}  // namespace jxl

int main(int argc, const char** argv) {
  jpegxl::tools::ThreadPoolInternal pool(8);

  jpegxl::tools::CommandLineParser parser;
  float preserve_saturation = .4f;
  parser.AddOptionValue(
      's', "preserve_saturation", "0..1",
      "to what extent to try and preserve saturation over luminance",
      &preserve_saturation, &jpegxl::tools::ParseFloat, 0);
  const char* input_filename = nullptr;
  auto input_filename_option = parser.AddPositionalOption(
      "input", true, "input image", &input_filename, 0);
  const char* output_filename = nullptr;
  auto output_filename_option = parser.AddPositionalOption(
      "output", true, "output image", &output_filename, 0);

  if (!parser.Parse(argc, argv)) {
    fprintf(stderr, "See -h for help.\n");
    return EXIT_FAILURE;
  }

  if (parser.HelpFlagPassed()) {
    parser.PrintHelp();
    return EXIT_SUCCESS;
  }

  if (!parser.GetOption(input_filename_option)->matched()) {
    fprintf(stderr, "Missing input filename.\nSee -h for help.\n");
    return EXIT_FAILURE;
  }
  if (!parser.GetOption(output_filename_option)->matched()) {
    fprintf(stderr, "Missing output filename.\nSee -h for help.\n");
    return EXIT_FAILURE;
  }

  jxl::CodecInOut image;
  jxl::extras::ColorHints color_hints;
  color_hints.Add("color_space", "RGB_D65_202_Rel_PeQ");
  std::vector<uint8_t> encoded;
  JXL_CHECK(jpegxl::tools::ReadFile(input_filename, &encoded));
  JXL_CHECK(jxl::SetFromBytes(jxl::Bytes(encoded), color_hints, &image, &pool));

  jxl::ProcessFrame(&image, preserve_saturation, &pool);

  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  jxl::extras::PackedPixelFile ppf =
      jxl::extras::ConvertImage3FToPackedPixelFile(
          *image.Main().color(), image.metadata.m.color_encoding, format,
          &pool);
  JXL_CHECK(jxl::Encode(ppf, output_filename, &encoded, &pool));
  JXL_CHECK(jpegxl::tools::WriteFile(output_filename, encoded));
}

#endif
