// Copyright 2026 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "avif/avif.h"
#include "aviftest_helpers.h"
#include "gtest/gtest.h"

namespace avif {
namespace {

// High bit depth AVIFs are coded as Sample Transform derived image items
// ('sato') combining a base image item and a hidden bit depth extension image
// item. Both the single image and the grid layouts are exercised here because
// the grid case goes through avifParseImageGridBox() and through the whole
// canvas dimensions rather than through one cell.
//
// The tests only check decoder behavior: the encoder is used as a fixture
// generator. The caller-configured avifDecoder::imageSizeLimit and
// avifDecoder::imageDimensionLimit must be enforced on the reconstructed canvas
// before any pixel buffer is allocated, and the decoded planes must follow the
// bit depth and chroma subsampling of the image, both in their geometry and in
// the last row (which is the easiest row to get wrong when plane dimensions are
// mixed up with row byte counts).

constexpr avifSampleTransformRecipe kRecipe =
    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B;
constexpr int kDepth = 16;
// A multi-cell grid requires cells of at least 64x64 pixels. The last cell may
// be smaller, so the canvas dimensions are not a multiple of the cell size.
constexpr uint32_t kCellWidth = 64;
constexpr uint32_t kCellHeight = 64;
// Small odd dimensions, to exercise the chroma plane rounding and the last row
// of each plane for the single image case.
constexpr uint32_t kSingleWidth = 65;
constexpr uint32_t kSingleHeight = 33;

bool AomEncoderAndDecoderAvailable() {
  return avifCodecName(AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE) !=
             nullptr &&
         testutil::Av1DecoderAvailable();
}

// Deterministic sample value of the input image. Any value is fine as long as
// the encoding is lossless, but making it depend on both coordinates and on the
// channel exercises the last row of each plane independently.
uint16_t ExpectedSample(uint32_t x, uint32_t y, int channel) {
  return static_cast<uint16_t>((x * 37 + y * 101 + channel * 10007 + 1) &
                               0xFFFF);
}

void FillPlanePattern(avifImage* image) {
  ASSERT_TRUE(avifImageUsesU16(image));
  for (int channel : {AVIF_CHAN_Y, AVIF_CHAN_U, AVIF_CHAN_V, AVIF_CHAN_A}) {
    uint8_t* plane = avifImagePlane(image, channel);
    if (plane == nullptr) {
      continue;
    }
    const uint32_t plane_width = avifImagePlaneWidth(image, channel);
    const uint32_t plane_height = avifImagePlaneHeight(image, channel);
    const uint32_t row_bytes = avifImagePlaneRowBytes(image, channel);
    for (uint32_t y = 0; y < plane_height; ++y) {
      uint16_t* const row = reinterpret_cast<uint16_t*>(
          plane + static_cast<size_t>(y) * row_bytes);
      for (uint32_t x = 0; x < plane_width; ++x) {
        row[x] = ExpectedSample(x, y, channel);
      }
    }
  }
}

// Encodes image losslessly with the bit depth extension recipe. If the grid is
// larger than 1x1, splits image into views with a smaller last cell.
avifResult Encode16Bit(const avifImage* image, avifSampleTransformRecipe recipe,
                       uint32_t grid_cols, uint32_t grid_rows,
                       testutil::AvifRwData* encoded) {
  EncoderPtr encoder(avifEncoderCreate());
  if (!encoder) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
  encoder->speed = AVIF_SPEED_FASTEST;
  encoder->quality = AVIF_QUALITY_LOSSLESS;
  encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
  encoder->sampleTransformRecipe = recipe;

  if (grid_cols == 1 && grid_rows == 1) {
    return avifEncoderWrite(encoder.get(), image, encoded);
  }

  // Two cells: a full one and a smaller last one. Only one dimension is
  // smaller so that the canvas stays valid for chroma subsampling.
  const avifCropRect rect0 = {0, 0, kCellWidth, kCellHeight};
  const avifCropRect rect1 = {
      (grid_cols > 1) ? kCellWidth : 0, (grid_rows > 1) ? kCellHeight : 0,
      (grid_cols > 1) ? image->width - kCellWidth : kCellWidth,
      (grid_rows > 1) ? image->height - kCellHeight : kCellHeight};
  ImagePtr cell0(avifImageCreateEmpty());
  ImagePtr cell1(avifImageCreateEmpty());
  if (!cell0 || !cell1) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  avifResult result = avifImageSetViewRect(cell0.get(), image, &rect0);
  if (result != AVIF_RESULT_OK) {
    return result;
  }
  result = avifImageSetViewRect(cell1.get(), image, &rect1);
  if (result != AVIF_RESULT_OK) {
    return result;
  }
  const avifImage* cells[] = {cell0.get(), cell1.get()};
  result = avifEncoderAddImageGrid(encoder.get(), grid_cols, grid_rows, cells,
                                   AVIF_ADD_IMAGE_FLAG_SINGLE);
  if (result != AVIF_RESULT_OK) {
    return result;
  }
  return avifEncoderFinish(encoder.get(), encoded);
}

avifResult Decode16Bit(const avifRWData& encoded, avifImage* decoded,
                       avifDiagnostics* diag) {
  DecoderPtr decoder(avifDecoderCreate());
  if (!decoder) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  decoder->imageContentToDecode |= AVIF_IMAGE_CONTENT_SAMPLE_TRANSFORMS;
  const avifResult result =
      avifDecoderReadMemory(decoder.get(), decoded, encoded.data, encoded.size);
  if (diag != nullptr) {
    *diag = decoder->diag;
  }
  return result;
}

void ExpectNoPlanesAllocated(const avifImage& image) {
  for (int channel : {AVIF_CHAN_Y, AVIF_CHAN_U, AVIF_CHAN_V}) {
    EXPECT_EQ(image.yuvPlanes[channel], nullptr) << "channel " << channel;
  }
  EXPECT_EQ(image.alphaPlane, nullptr);
}

// Expected geometry of one plane, computed independently from the decoder's
// avifImagePlaneWidth()/avifImagePlaneHeight() helpers.
struct PlaneLayout {
  uint32_t width;
  uint32_t height;
};

PlaneLayout ExpectedPlaneLayout(const avifImage& image, int channel) {
  avifPixelFormatInfo info;
  avifGetPixelFormatInfo(image.yuvFormat, &info);
  if (channel == AVIF_CHAN_Y) {
    return {image.width, image.height};
  }
  if (channel == AVIF_CHAN_U || channel == AVIF_CHAN_V) {
    if (info.monochrome) {
      return {0, 0};
    }
    const uint32_t chroma_width =
        (image.width + ((1u << info.chromaShiftX) - 1u)) >> info.chromaShiftX;
    const uint32_t chroma_height =
        (image.height + ((1u << info.chromaShiftY) - 1u)) >> info.chromaShiftY;
    return {chroma_width, chroma_height};
  }
  // Alpha is never subsampled.
  return image.alphaPlane != nullptr ? PlaneLayout{image.width, image.height}
                                     : PlaneLayout{0, 0};
}

// Compares the last row of each plane of the decoded image with the source
// samples, using the plane geometry (pixels) multiplied by the sample size, and
// never the row byte count as a pixel count or the pixel count as a byte count.
void ExpectPlanesMatchSourceLastRow(const avifImage& source,
                                    const avifImage& decoded) {
  ASSERT_EQ(avifImageUsesU16(&source), avifImageUsesU16(&decoded));
  const uint32_t bytes_per_sample = avifImageUsesU16(&decoded) ? 2u : 1u;
  for (int channel : {AVIF_CHAN_Y, AVIF_CHAN_U, AVIF_CHAN_V, AVIF_CHAN_A}) {
    const PlaneLayout expected = ExpectedPlaneLayout(source, channel);
    EXPECT_EQ(avifImagePlaneWidth(&decoded, channel), expected.width)
        << "channel " << channel;
    EXPECT_EQ(avifImagePlaneHeight(&decoded, channel), expected.height)
        << "channel " << channel;
    if (expected.width == 0 || expected.height == 0) {
      EXPECT_EQ(avifImagePlane(&decoded, channel), nullptr)
          << "channel " << channel;
      continue;
    }

    const uint8_t* const decoded_plane = avifImagePlane(&decoded, channel);
    const uint8_t* const source_plane = avifImagePlane(&source, channel);
    ASSERT_NE(decoded_plane, nullptr) << "channel " << channel;
    ASSERT_NE(source_plane, nullptr) << "channel " << channel;
    const uint32_t decoded_row_bytes =
        avifImagePlaneRowBytes(&decoded, channel);
    const uint32_t source_row_bytes = avifImagePlaneRowBytes(&source, channel);
    // A 16-bit sample takes two bytes, whatever the number of pixels or planes.
    // A codec may return a stride with padding, but never less than the samples
    // of the last row need.
    EXPECT_GE(decoded_row_bytes, expected.width * bytes_per_sample)
        << "channel " << channel;
    EXPECT_EQ(source_row_bytes, expected.width * bytes_per_sample)
        << "channel " << channel;

    const size_t decoded_last_row_offset =
        static_cast<size_t>(expected.height - 1) * decoded_row_bytes;
    const size_t source_last_row_offset =
        static_cast<size_t>(expected.height - 1) * source_row_bytes;
    const uint8_t* const decoded_last_row =
        decoded_plane + decoded_last_row_offset;
    const uint8_t* const source_last_row =
        source_plane + source_last_row_offset;
    EXPECT_EQ(std::memcmp(decoded_last_row, source_last_row,
                          expected.width * bytes_per_sample),
              0)
        << "channel " << channel;

    // Also check the corner samples of that row against the independent
    // pattern, to catch a test that would compare the wrong row with the wrong
    // stride.
    ASSERT_EQ(bytes_per_sample, 2u);
    const uint16_t* const decoded_samples =
        reinterpret_cast<const uint16_t*>(decoded_last_row);
    EXPECT_EQ(decoded_samples[0],
              ExpectedSample(0, expected.height - 1, channel))
        << "channel " << channel;
    EXPECT_EQ(decoded_samples[expected.width - 1],
              ExpectedSample(expected.width - 1, expected.height - 1, channel))
        << "channel " << channel;
  }
}

// ---------------------------------------------------------------------------
// Caller-configured limits on the reconstructed canvas.

struct LimitInput {
  const char* name;
  avifPixelFormat format;
  uint32_t grid_cols;
  uint32_t grid_rows;
};

struct LimitCase {
  const char* name;
  uint32_t size_limit;
  uint32_t dimension_limit;
  bool accepted;
};

class DecoderLimitsHighBitDepthTest
    : public testing::TestWithParam<LimitInput> {};

TEST_P(DecoderLimitsHighBitDepthTest, ImageSizeAndDimensionLimits) {
  if (!AomEncoderAndDecoderAvailable()) {
    GTEST_SKIP() << "AOM encoder or AV1 decoder unavailable, skip test.";
  }
  const LimitInput input = GetParam();
  const bool grid = input.grid_cols * input.grid_rows > 1;
  const uint32_t width =
      grid ? (input.grid_cols > 1 ? kCellWidth * input.grid_cols - 4
                                  : kCellWidth)
           : kSingleWidth;
  const uint32_t height =
      grid ? (input.grid_rows > 1 ? kCellHeight * input.grid_rows - 4
                                  : kCellHeight)
           : kSingleHeight;
  ImagePtr source =
      testutil::CreateImage(static_cast<int>(width), static_cast<int>(height),
                            kDepth, input.format, AVIF_PLANES_YUV);
  ASSERT_NE(source, nullptr);
  FillPlanePattern(source.get());

  testutil::AvifRwData encoded;
  ASSERT_EQ(Encode16Bit(source.get(), kRecipe, input.grid_cols, input.grid_rows,
                        &encoded),
            AVIF_RESULT_OK);

  const uint32_t pixel_count = width * height;
  const uint32_t max_dimension = std::max(width, height);
  // The limits are expressed in pixels: a limit equal to width*height must be
  // accepted even though the 16-bit pixel buffers of several planes are much
  // larger in bytes than the pixel count.
  std::vector<LimitCase> cases = {
      {"PixelLimitInclusive", pixel_count, 0, true},
      {"DimensionLimitInclusive", 0, max_dimension, true},
      {"BothLimitsInclusive", pixel_count, max_dimension, true},
      {"PixelLimitExceeded", pixel_count - 1, max_dimension, false},
      {"DimensionLimitExceeded", pixel_count, max_dimension - 1, false},
      {"SizeDisabledDimensionEnforced", 0, max_dimension - 1, false},
      {"DimensionDisabledSizeEnforced", pixel_count - 1, 0, false},
      {"BothDisabled", 0, 0, true},
  };
  if (grid) {
    // The limit applies to the reconstructed canvas, not to a single cell: each
    // cell is much smaller than the canvas, so a limit equal to one cell's
    // pixel count or to one cell's largest dimension must be rejected.
    const uint32_t cell_count = kCellWidth * kCellHeight;
    cases.push_back({"CellPixelLimitIsNotEnough", cell_count, 0, false});
    cases.push_back({"CellDimensionLimitIsNotEnough", 0, kCellWidth, false});
  }
  for (const LimitCase& limit : cases) {
    SCOPED_TRACE(limit.name);
    DecoderPtr decoder(avifDecoderCreate());
    ASSERT_NE(decoder, nullptr);
    decoder->imageContentToDecode |= AVIF_IMAGE_CONTENT_SAMPLE_TRANSFORMS;
    decoder->imageSizeLimit = limit.size_limit;
    decoder->imageDimensionLimit = limit.dimension_limit;
    ASSERT_EQ(avifDecoderSetIOMemory(decoder.get(), encoded.data, encoded.size),
              AVIF_RESULT_OK);
    const avifResult parse_result = avifDecoderParse(decoder.get());
    if (!limit.accepted) {
      // Illegal dimensions are rejected while parsing the container, before any
      // pixel buffer exists.
      EXPECT_NE(parse_result, AVIF_RESULT_OK);
      EXPECT_NE(std::string(decoder->diag.error).find("too large"),
                std::string::npos)
          << "unexpected diagnostic: " << decoder->diag.error;
      // Either the output image was never created, or it has no pixel buffers.
      if (decoder->image != nullptr) {
        ExpectNoPlanesAllocated(*decoder->image);
      }
      continue;
    }
    ASSERT_EQ(parse_result, AVIF_RESULT_OK) << decoder->diag.error;
    ASSERT_NE(decoder->image, nullptr);
    EXPECT_EQ(decoder->image->width, width);
    EXPECT_EQ(decoder->image->height, height);
    // Parsing must not allocate decoded pixels even for accepted inputs. Only
    // the AV1 decoder may allocate them in avifDecoderNextImage().
    ExpectNoPlanesAllocated(*decoder->image);
    ASSERT_EQ(avifDecoderNextImage(decoder.get()), AVIF_RESULT_OK)
        << decoder->diag.error;
    EXPECT_EQ(decoder->image->width, width);
    EXPECT_EQ(decoder->image->height, height);
    EXPECT_EQ(decoder->image->depth, kDepth);
    EXPECT_NE(decoder->image->yuvPlanes[AVIF_CHAN_Y], nullptr);
  }
}

INSTANTIATE_TEST_SUITE_P(
    FormatsAndLayouts, DecoderLimitsHighBitDepthTest,
    testing::Values(LimitInput{"Single444", AVIF_PIXEL_FORMAT_YUV444, 1, 1},
                    LimitInput{"Single420", AVIF_PIXEL_FORMAT_YUV420, 1, 1},
                    LimitInput{"Single400", AVIF_PIXEL_FORMAT_YUV400, 1, 1},
                    LimitInput{"Grid444", AVIF_PIXEL_FORMAT_YUV444, 2, 1},
                    LimitInput{"Grid420", AVIF_PIXEL_FORMAT_YUV420, 2, 1},
                    LimitInput{"Grid400", AVIF_PIXEL_FORMAT_YUV400, 2, 1}),
    [](const testing::TestParamInfo<LimitInput>& info) {
      return info.param.name;
    });

// The default limits apply to a high bit depth image whose declared 'ispe' was
// tampered with, without any huge pixel buffer being allocated first.
TEST(DecoderLimitsHighBitDepthTest, OversizedIspeRejectedBeforeAllocation) {
  if (!AomEncoderAndDecoderAvailable()) {
    GTEST_SKIP() << "AOM encoder or AV1 decoder unavailable, skip test.";
  }
  ImagePtr source = testutil::CreateImage(
      static_cast<int>(kSingleWidth), static_cast<int>(kSingleHeight), kDepth,
      AVIF_PIXEL_FORMAT_YUV420, AVIF_PLANES_YUV);
  ASSERT_NE(source, nullptr);
  FillPlanePattern(source.get());
  testutil::AvifRwData encoded;
  ASSERT_EQ(Encode16Bit(source.get(), kRecipe, 1, 1, &encoded), AVIF_RESULT_OK);

  // Rewrites the first 'ispe' payload (version/flags followed by image_width
  // and image_height, both 32-bit big-endian). All image items of a single
  // image share the same deduplicated property.
  auto patch_ispe = [](avifRWData* data, uint32_t width, uint32_t height) {
    const uint8_t kIspe[4] = {'i', 's', 'p', 'e'};
    for (size_t i = 0; i + 4 + 4 + 8 <= data->size; ++i) {
      if (std::memcmp(data->data + i, kIspe, 4) != 0) {
        continue;
      }
      uint8_t* const payload = data->data + i + 4 + 4;
      const uint32_t values[2] = {width, height};
      for (int j = 0; j < 2; ++j) {
        for (int b = 0; b < 4; ++b) {
          payload[j * 4 + b] =
              static_cast<uint8_t>((values[j] >> (8 * (3 - b))) & 0xFF);
        }
      }
      return true;
    }
    return false;
  };

  struct TamperedCase {
    const char* name;
    uint32_t width;
    uint32_t height;
    const char* expected_diag_substring;
  };
  const TamperedCase cases[] = {
      // 20000*20000 = 400,000,000 pixels is more than
      // AVIF_DEFAULT_IMAGE_SIZE_LIMIT (16384*16384), while 20000 is less than
      // AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT (32768): only the pixel count limit
      // can reject this one.
      {"PixelLimit", 20000, 20000, "too large"},
      // 40000 exceeds AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT while its pixel count
      // is far below the pixel count limit: only the dimension limit can reject
      // this one.
      {"DimensionLimit", 40000, 1, "too large"},
  };
  for (const TamperedCase& tampered : cases) {
    SCOPED_TRACE(tampered.name);
    testutil::AvifRwData tampered_data;
    ASSERT_EQ(avifRWDataRealloc(&tampered_data, encoded.size), AVIF_RESULT_OK);
    std::memcpy(tampered_data.data, encoded.data, encoded.size);
    ASSERT_TRUE(patch_ispe(&tampered_data, tampered.width, tampered.height));

    DecoderPtr decoder(avifDecoderCreate());
    ASSERT_NE(decoder, nullptr);
    decoder->imageContentToDecode |= AVIF_IMAGE_CONTENT_SAMPLE_TRANSFORMS;
    ASSERT_EQ(avifDecoderSetIOMemory(decoder.get(), tampered_data.data,
                                     tampered_data.size),
              AVIF_RESULT_OK);
    EXPECT_EQ(avifDecoderParse(decoder.get()), AVIF_RESULT_BMFF_PARSE_FAILED)
        << decoder->diag.error;
    EXPECT_NE(
        std::string(decoder->diag.error).find(tampered.expected_diag_substring),
        std::string::npos)
        << "unexpected diagnostic: " << decoder->diag.error;
    if (decoder->image != nullptr) {
      ExpectNoPlanesAllocated(*decoder->image);
    }
  }
}

// The grid box declares the reconstructed canvas dimensions, separately from
// the 'ispe' property of each cell. A canvas that is too large must be rejected
// even though every cell on its own is small enough.
TEST(DecoderLimitsHighBitDepthTest, OversizedGridCanvasRejected) {
  if (!AomEncoderAndDecoderAvailable()) {
    GTEST_SKIP() << "AOM encoder or AV1 decoder unavailable, skip test.";
  }
  // 2x1 grid of 64x64 cells with a smaller last cell: canvas 124x64.
  const uint32_t width = kCellWidth * 2 - 4;
  const uint32_t height = kCellHeight;
  ImagePtr source =
      testutil::CreateImage(static_cast<int>(width), static_cast<int>(height),
                            kDepth, AVIF_PIXEL_FORMAT_YUV420, AVIF_PLANES_YUV);
  ASSERT_NE(source, nullptr);
  FillPlanePattern(source.get());
  testutil::AvifRwData encoded;
  ASSERT_EQ(Encode16Bit(source.get(), kRecipe, 2, 1, &encoded), AVIF_RESULT_OK);

  // The ImageGrid payload written by the encoder above starts with
  // version=0, flags=0, rows_minus_one=0, columns_minus_one=1 and then the
  // 16-bit output_width and output_height, so the canvas dims are unique in
  // this small file. Patch them to a canvas above the default limits while
  // keeping the cells (and thus their 'ispe') untouched.
  const uint8_t kGridPayloadPrefix[4] = {0, 0, 0, 1};
  std::vector<uint8_t> canvas_pattern(kGridPayloadPrefix,
                                      kGridPayloadPrefix + 4);
  const uint16_t original_dims[2] = {static_cast<uint16_t>(width),
                                     static_cast<uint16_t>(height)};
  for (uint16_t dim : original_dims) {
    canvas_pattern.push_back(static_cast<uint8_t>(dim >> 8));
    canvas_pattern.push_back(static_cast<uint8_t>(dim & 0xFF));
  }
  // Require the canvas dimensions to be uniquely identifiable in this small
  // file. Otherwise a match inside the AV1 payload could be patched instead of
  // the ImageGrid payload.
  const uint8_t* const encoded_end = encoded.data + encoded.size;
  const uint8_t* canvas_match = encoded_end;
  size_t match_count = 0;
  for (const uint8_t* search_start = encoded.data; search_start < encoded_end;
       ++search_start) {
    search_start = std::search(search_start, encoded_end,
                               canvas_pattern.begin(), canvas_pattern.end());
    if (search_start == encoded_end) {
      break;
    }
    canvas_match = search_start;
    ++match_count;
  }
  ASSERT_EQ(match_count, 1u)
      << "Could not uniquely locate the grid payload in the encoded file";
  const size_t canvas_offset =
      static_cast<size_t>(canvas_match - encoded.data) +
      sizeof(kGridPayloadPrefix);

  // 40000x40000 pixels is both above the default pixel count limit and above
  // the default dimension limit. The cell 'ispe' stays 64x64 or smaller. Values
  // above 65535 would use the 32-bit grid field, but the default limits reject
  // anything above 32768 anyway, so the 16-bit field is enough here.
  const uint16_t kOversizedCanvas = 40000;
  testutil::AvifRwData tampered_data;
  ASSERT_EQ(avifRWDataRealloc(&tampered_data, encoded.size), AVIF_RESULT_OK);
  std::memcpy(tampered_data.data, encoded.data, encoded.size);
  for (int i = 0; i < 2; ++i) {
    tampered_data.data[canvas_offset + i * 2] =
        static_cast<uint8_t>(kOversizedCanvas >> 8);
    tampered_data.data[canvas_offset + i * 2 + 1] =
        static_cast<uint8_t>(kOversizedCanvas & 0xFF);
  }

  DecoderPtr decoder(avifDecoderCreate());
  ASSERT_NE(decoder, nullptr);
  decoder->imageContentToDecode |= AVIF_IMAGE_CONTENT_SAMPLE_TRANSFORMS;
  ASSERT_EQ(avifDecoderSetIOMemory(decoder.get(), tampered_data.data,
                                   tampered_data.size),
            AVIF_RESULT_OK);
  EXPECT_NE(avifDecoderParse(decoder.get()), AVIF_RESULT_OK)
      << decoder->diag.error;
  if (decoder->image != nullptr) {
    ExpectNoPlanesAllocated(*decoder->image);
  }
}

// ---------------------------------------------------------------------------
// Plane geometry and last row of decoded small high bit depth images.

struct LayoutInput {
  const char* name;
  avifSampleTransformRecipe recipe;
  avifPixelFormat format;
  bool alpha;
  uint32_t grid_cols;
  uint32_t grid_rows;
};

class DecodeSizeHighBitDepthLayoutTest
    : public testing::TestWithParam<LayoutInput> {};

TEST_P(DecodeSizeHighBitDepthLayoutTest, PlaneGeometryAndLastRow) {
  if (!AomEncoderAndDecoderAvailable()) {
    GTEST_SKIP() << "AOM encoder or AV1 decoder unavailable, skip test.";
  }
  const LayoutInput input = GetParam();
  const bool grid = input.grid_cols * input.grid_rows > 1;
  const uint32_t width =
      grid ? (input.grid_cols > 1 ? kCellWidth * input.grid_cols - 4
                                  : kCellWidth)
           : kSingleWidth;
  const uint32_t height =
      grid ? (input.grid_rows > 1 ? kCellHeight * input.grid_rows - 4
                                  : kCellHeight)
           : kSingleHeight;
  const avifPlanesFlags planes =
      input.alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV;
  ImagePtr source =
      testutil::CreateImage(static_cast<int>(width), static_cast<int>(height),
                            kDepth, input.format, planes);
  ASSERT_NE(source, nullptr);
  source->yuvRange = AVIF_RANGE_FULL;
  FillPlanePattern(source.get());

  testutil::AvifRwData encoded;
  ASSERT_EQ(Encode16Bit(source.get(), input.recipe, input.grid_cols,
                        input.grid_rows, &encoded),
            AVIF_RESULT_OK);

  ImagePtr decoded(avifImageCreateEmpty());
  ASSERT_NE(decoded, nullptr);
  avifDiagnostics diag;
  ASSERT_EQ(Decode16Bit(encoded, decoded.get(), &diag), AVIF_RESULT_OK)
      << diag.error;

  EXPECT_EQ(decoded->depth, kDepth);
  EXPECT_EQ(decoded->width, width);
  EXPECT_EQ(decoded->height, height);
  EXPECT_EQ(decoded->yuvFormat, input.format);
  EXPECT_TRUE(avifImageUsesU16(decoded.get()));
  ExpectPlanesMatchSourceLastRow(*source, *decoded);
}

INSTANTIATE_TEST_SUITE_P(
    FormatsAndLayouts, DecodeSizeHighBitDepthLayoutTest,
    testing::Values(
        LayoutInput{"Single444",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV444, false, 1, 1},
        LayoutInput{"Single422",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV422, false, 1, 1},
        LayoutInput{"Single420",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV420, false, 1, 1},
        LayoutInput{"Single400",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV400, false, 1, 1},
        LayoutInput{"Single444Alpha",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV444, true, 1, 1},
        LayoutInput{"Single420Alpha",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV420, true, 1, 1},
        LayoutInput{"Single400Alpha",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV400, true, 1, 1},
        LayoutInput{"Grid444", AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV444, false, 2, 1},
        LayoutInput{"Grid422", AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV422, false, 2, 1},
        LayoutInput{"Grid420", AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV420, false, 2, 1},
        LayoutInput{"Grid400", AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV400, false, 2, 1},
        LayoutInput{"VerticalGrid444",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV444, false, 1, 2},
        LayoutInput{"VerticalGrid420",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV420, false, 1, 2},
        LayoutInput{"VerticalGrid420Alpha",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_8B_8B,
                    AVIF_PIXEL_FORMAT_YUV420, true, 1, 2},
        // The other bit depth extension recipes pack the 16 bits differently.
        LayoutInput{"Recipe12B4BSingle420",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_12B_4B,
                    AVIF_PIXEL_FORMAT_YUV420, false, 1, 1},
        LayoutInput{"Recipe12B4BGrid420",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_12B_4B,
                    AVIF_PIXEL_FORMAT_YUV420, false, 2, 1},
        LayoutInput{"Recipe12B8BOverlap4BSingle444",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_12B_8B_OVERLAP_4B,
                    AVIF_PIXEL_FORMAT_YUV444, false, 1, 1},
        LayoutInput{"Recipe12B8BOverlap4BGrid444",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_12B_8B_OVERLAP_4B,
                    AVIF_PIXEL_FORMAT_YUV444, false, 2, 1},
        LayoutInput{"Recipe12B8BOverlap4BSingle420Alpha",
                    AVIF_SAMPLE_TRANSFORM_BIT_DEPTH_EXTENSION_12B_8B_OVERLAP_4B,
                    AVIF_PIXEL_FORMAT_YUV420, true, 1, 1}),
    [](const testing::TestParamInfo<LayoutInput>& info) {
      return info.param.name;
    });

//------------------------------------------------------------------------------

}  // namespace
}  // namespace avif
