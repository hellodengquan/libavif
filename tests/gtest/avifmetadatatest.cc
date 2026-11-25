// Copyright 2022 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>

#include "avif/avif.h"
#include "avif/avif_cxx.h"
#include "avif/internal.h"
#include "avifexif.h"
#include "avifjpeg.h"
#include "avifpng.h"
#include "aviftest_helpers.h"
#include "gtest/gtest.h"

using ::testing::Bool;
using ::testing::Combine;
using ::testing::Values;

namespace avif {
namespace {

//------------------------------------------------------------------------------

// Used to pass the data folder path to the GoogleTest suites.
const char* data_path = nullptr;

//------------------------------------------------------------------------------
// AVIF encode/decode metadata tests

class AvifMetadataTest
    : public testing::TestWithParam<
          std::tuple</*use_icc=*/bool, /*use_exif=*/bool, /*use_xmp=*/bool>> {};

// Encodes, decodes then verifies that the output metadata matches the input
// metadata defined by the parameters.
TEST_P(AvifMetadataTest, EncodeDecode) {
  const bool use_icc = std::get<0>(GetParam());
  const bool use_exif = std::get<1>(GetParam());
  const bool use_xmp = std::get<2>(GetParam());

  ImagePtr image =
      testutil::CreateImage(/*width=*/12, /*height=*/34, /*depth=*/10,
                            AVIF_PIXEL_FORMAT_YUV444, AVIF_PLANES_ALL);
  ASSERT_NE(image, nullptr);
  testutil::FillImageGradient(image.get());  // The pixels do not matter.
  if (use_icc) {
    ASSERT_EQ(avifImageSetProfileICC(image.get(), testutil::kSampleIcc.data(),
                                     testutil::kSampleIcc.size()),
              AVIF_RESULT_OK);
  }
  if (use_exif) {
    const avifTransformFlags old_transform_flags = image->transformFlags;
    const uint8_t old_irot_angle = image->irot.angle;
    const uint8_t old_imir_axis = image->imir.axis;
    ASSERT_EQ(
        avifImageSetMetadataExif(image.get(), testutil::kSampleExif.data(),
                                 testutil::kSampleExif.size()),
        AVIF_RESULT_OK);
    // testutil::kSampleExif is not a valid Exif payload, just some part of it.
    // These fields should not be modified.
    EXPECT_EQ(image->transformFlags, old_transform_flags);
    EXPECT_EQ(image->irot.angle, old_irot_angle);
    EXPECT_EQ(image->imir.axis, old_imir_axis);
  }
  if (use_xmp) {
    ASSERT_EQ(avifImageSetMetadataXMP(image.get(), testutil::kSampleXmp.data(),
                                      testutil::kSampleXmp.size()),
              AVIF_RESULT_OK);
  }

  // Encode.
  EncoderPtr encoder(avifEncoderCreate());
  ASSERT_NE(encoder, nullptr);
  encoder->speed = AVIF_SPEED_FASTEST;
  testutil::AvifRwData encoded_avif;
  ASSERT_EQ(avifEncoderWrite(encoder.get(), image.get(), &encoded_avif),
            AVIF_RESULT_OK);

  // Decode.
  ImagePtr decoded(avifImageCreateEmpty());
  ASSERT_NE(decoded, nullptr);
  DecoderPtr decoder(avifDecoderCreate());
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(avifDecoderReadMemory(decoder.get(), decoded.get(),
                                  encoded_avif.data, encoded_avif.size),
            AVIF_RESULT_OK);

  // Compare input and output metadata.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->icc.data, decoded->icc.size, testutil::kSampleIcc.data(),
      use_icc ? testutil::kSampleIcc.size() : 0u));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, testutil::kSampleExif.data(),
      use_exif ? testutil::kSampleExif.size() : 0u));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->xmp.data, decoded->xmp.size, testutil::kSampleXmp.data(),
      use_xmp ? testutil::kSampleXmp.size() : 0u));
}

INSTANTIATE_TEST_SUITE_P(All, AvifMetadataTest,
                         Combine(/*use_icc=*/Bool(), /*use_exif=*/Bool(),
                                 /*use_xmp=*/Bool()));

//------------------------------------------------------------------------------
// Jpeg and PNG metadata tests

ImagePtr WriteAndReadImage(const avifImage& image,
                           const std::string& file_name) {
  const std::string file_path = testing::TempDir() + file_name;
  if (file_name.substr(file_name.size() - 4) == ".png") {
    if (!avifPNGWrite(file_path.c_str(), &image, /*requestedDepth=*/0,
                      AVIF_CHROMA_UPSAMPLING_AUTOMATIC,
                      /*compressionLevel=*/0)) {
      return nullptr;
    }
  } else {
    if (!avifJPEGWrite(file_path.c_str(), &image, /*jpegQuality=*/100,
                       AVIF_CHROMA_UPSAMPLING_AUTOMATIC)) {
      return nullptr;
    }
  }
  return testutil::ReadImage(testing::TempDir().c_str(), file_name.c_str());
}

class MetadataTest : public testing::TestWithParam<
                         std::tuple</*file_name=*/const char*, /*use_icc=*/bool,
                                    /*use_exif=*/bool, /*use_xmp=*/bool>> {};

TEST_P(MetadataTest, ReadWriteReadCompare) {
  const char* file_name = std::get<0>(GetParam());
  const bool use_icc = std::get<1>(GetParam());
  const bool use_exif = std::get<2>(GetParam());
  const bool use_xmp = std::get<3>(GetParam());

  const ImagePtr image = testutil::ReadImage(
      data_path, file_name, AVIF_PIXEL_FORMAT_NONE, 0,
      AVIF_CHROMA_DOWNSAMPLING_AUTOMATIC, !use_icc, !use_exif, !use_xmp);
  ASSERT_NE(image, nullptr);
  EXPECT_NE(image->width * image->height, 0u);

  if (use_icc) {
    EXPECT_NE(image->icc.size, 0u);
    EXPECT_NE(image->icc.data, nullptr);
  } else {
    EXPECT_EQ(image->icc.size, 0u);
    EXPECT_EQ(image->icc.data, nullptr);
  }
  if (use_exif) {
    EXPECT_NE(image->exif.size, 0u);
    EXPECT_NE(image->exif.data, nullptr);
  } else {
    EXPECT_EQ(image->exif.size, 0u);
    EXPECT_EQ(image->exif.data, nullptr);
  }
  if (use_xmp) {
    EXPECT_NE(image->xmp.size, 0u);
    EXPECT_NE(image->xmp.data, nullptr);
  } else {
    EXPECT_EQ(image->xmp.size, 0u);
    EXPECT_EQ(image->xmp.data, nullptr);
  }

  // Writing and reading that same metadata should give the same bytes.
  for (const std::string extension : {".png", ".jpg"}) {
    const ImagePtr temp_image =
        WriteAndReadImage(*image, file_name + extension);
    ASSERT_NE(temp_image, nullptr);
    ASSERT_TRUE(testutil::AreByteSequencesEqual(image->icc, temp_image->icc));
    ASSERT_TRUE(testutil::AreByteSequencesEqual(image->exif, temp_image->exif));
    ASSERT_TRUE(testutil::AreByteSequencesEqual(image->xmp, temp_image->xmp));
  }
}

INSTANTIATE_TEST_SUITE_P(
    PngJpeg, MetadataTest,
    Combine(Values("paris_icc_exif_xmp.png",         // iCCP zTXt zTXt IDAT
                   "paris_icc_exif_xmp_at_end.png",  // iCCP IDAT eXIf tEXt
                   "paris_exif_xmp_icc.jpg"),  // APP1-Exif, APP1-XMP, APP2-ICC
            /*use_icc=*/Bool(), /*use_exif=*/Bool(), /*use_xmp=*/Bool()));

// Verify all parsers lead exactly to the same metadata bytes.
TEST(MetadataTest, Compare) {
  const ImagePtr ref = testutil::ReadImage(data_path, "paris_icc_exif_xmp.png");
  ASSERT_NE(ref, nullptr);
  EXPECT_GT(ref->exif.size, 0u);
  EXPECT_GT(ref->xmp.size, 0u);
  EXPECT_GT(ref->icc.size, 0u);

  for (const char* file_name :
       {"paris_exif_xmp_icc.jpg", "paris_icc_exif_xmp_at_end.png"}) {
    const ImagePtr image = testutil::ReadImage(data_path, file_name);
    ASSERT_NE(image, nullptr);
    EXPECT_TRUE(testutil::AreByteSequencesEqual(image->exif, ref->exif));
    EXPECT_TRUE(testutil::AreByteSequencesEqual(image->xmp, ref->xmp));
    EXPECT_TRUE(testutil::AreByteSequencesEqual(image->icc, ref->icc));
  }
}

// A test for https://github.com/AOMediaCodec/libavif/issues/1086 to prevent
// regression.
TEST(MetadataTest, DecoderParseICC) {
  std::string file_path = std::string(data_path) + "paris_icc_exif_xmp.avif";
  avifDecoder* decoder = avifDecoderCreate();
  ASSERT_NE(decoder, nullptr);
  EXPECT_EQ(avifDecoderSetIOFile(decoder, file_path.c_str()), AVIF_RESULT_OK);
  EXPECT_EQ(avifDecoderParse(decoder), AVIF_RESULT_OK);
  // Check the first four bytes of the ICC profile.
  ASSERT_GE(decoder->image->icc.size, 4u);
  EXPECT_EQ(decoder->image->icc.data[0], 0);
  EXPECT_EQ(decoder->image->icc.data[1], 0);
  EXPECT_EQ(decoder->image->icc.data[2], 2);
  EXPECT_EQ(decoder->image->icc.data[3], 84);
  avifDecoderDestroy(decoder);

  decoder = avifDecoderCreate();
  ASSERT_NE(decoder, nullptr);
  decoder->ignoreICC = true;
  EXPECT_EQ(avifDecoderSetIOFile(decoder, file_path.c_str()), AVIF_RESULT_OK);
  EXPECT_EQ(avifDecoderParse(decoder), AVIF_RESULT_OK);
  ASSERT_EQ(decoder->image->icc.size, 0u);
  avifDecoderDestroy(decoder);
}

//------------------------------------------------------------------------------

TEST(MetadataTest, ExifButDefaultIrotImir) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "paris_exif_xmp_icc.jpg");
  ASSERT_NE(image, nullptr);
  // The Exif metadata contains orientation information: 1.
  // It is converted to no irot/imir.
  EXPECT_GT(image->exif.size, 0u);
  EXPECT_EQ(image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            avifTransformFlags{AVIF_TRANSFORM_NONE});

  const testutil::AvifRwData encoded =
      testutil::Encode(image.get(), AVIF_SPEED_FASTEST);
  const ImagePtr decoded = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(decoded, nullptr);

  // No irot/imir after decoding because 1 maps to default no irot/imir.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(image->exif, decoded->exif));
  EXPECT_EQ(
      decoded->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
      avifTransformFlags{AVIF_TRANSFORM_NONE});
}

TEST(MetadataTest, ExifOrientation) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "paris_exif_orientation_5.jpg");
  ASSERT_NE(image, nullptr);
  // The Exif metadata contains orientation information: 5.
  // When reading the JPEG file, the Exif orientation is set to 1.
  EXPECT_GT(image->exif.size, 0u);
  EXPECT_EQ(image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            avifTransformFlags{AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR});
  EXPECT_EQ(image->irot.angle, 1u);
  EXPECT_EQ(image->imir.axis, 0u);
  testutil::AvifRwData originalReadExif;
  ASSERT_EQ(
      avifRWDataSet(&originalReadExif, image->exif.data, image->exif.size),
      AVIF_RESULT_OK);
  // For testing purposes, set back the orientation.
  ASSERT_EQ(avifSetExifOrientation(&image->exif, 5), AVIF_RESULT_OK);
  // The two Exifs are different, showing that the Orientation was indeed
  // modified when reading.
  EXPECT_FALSE(testutil::AreByteSequencesEqual(originalReadExif, image->exif));

  const testutil::AvifRwData encoded =
      testutil::Encode(image.get(), AVIF_SPEED_FASTEST);
  const ImagePtr decoded = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(decoded, nullptr);

  // irot/imir are expected.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(image->exif, decoded->exif));
  EXPECT_EQ(
      decoded->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
      avifTransformFlags{AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR});
  EXPECT_EQ(decoded->irot.angle, 1u);
  EXPECT_EQ(decoded->imir.axis, 0u);

  // JPEG: Exif orientation should be applied when saving and removed from Exif.
  ImagePtr temp_image =
      WriteAndReadImage(*image, "paris_exif_orientation_5.jpg");
  ASSERT_NE(temp_image, nullptr);
  EXPECT_FALSE(testutil::AreByteSequencesEqual(image->exif, temp_image->exif));
  EXPECT_EQ(temp_image->transformFlags, AVIF_TRANSFORM_NONE);
  // Samples have been rotated.
  EXPECT_EQ(image->width, temp_image->height);
  EXPECT_EQ(image->height, temp_image->width);

  // PNG: Exif orientation should be applied when saving and removed from Exif.
  temp_image = WriteAndReadImage(*image, "paris_exif_orientation_5.png");
  ASSERT_NE(temp_image, nullptr);
  EXPECT_FALSE(testutil::AreByteSequencesEqual(image->exif, temp_image->exif));
  EXPECT_EQ(
      temp_image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
      avifTransformFlags{0});
  // Samples have been rotated.
  EXPECT_EQ(image->width, temp_image->height);
  EXPECT_EQ(image->height, temp_image->width);
}

TEST(MetadataTest, AllExifOrientations) {
  for (uint8_t orientation = 1; orientation <= 8; ++orientation) {
    const ImagePtr image =
        testutil::ReadImage(data_path, "paris_exif_orientation_5.jpg");
    ASSERT_NE(image, nullptr);
    image->transformFlags = AVIF_TRANSFORM_NONE;
    // Check roundtrip.
    ASSERT_EQ(avifSetExifOrientation(&image->exif, orientation),
              AVIF_RESULT_OK);
    ASSERT_EQ(avifImageExtractExifOrientationToIrotImir(image.get()),
              AVIF_RESULT_OK);
    ASSERT_EQ(avifImageGetExifOrientationFromIrotImir(image.get()),
              orientation);
  }
}

TEST(MetadataTest, ExifOrientationAndForcedImir) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "paris_exif_orientation_5.jpg");
  ASSERT_NE(image, nullptr);
  // The Exif metadata contains orientation information: 5.
  // Force irot/imir to values that have a different meaning than 5.
  // This is not recommended but for testing only.
  EXPECT_GT(image->exif.size, 0u);
  image->transformFlags = AVIF_TRANSFORM_IMIR;
  image->imir.axis = 1;
  testutil::AvifRwData originalReadExif;
  ASSERT_EQ(
      avifRWDataSet(&originalReadExif, image->exif.data, image->exif.size),
      AVIF_RESULT_OK);
  // For testing purposes, set back the orientation.
  ASSERT_EQ(avifSetExifOrientation(&image->exif, 5), AVIF_RESULT_OK);
  // The two Exifs are different, showing that the Orientation was indeed
  // modified when reading.
  EXPECT_FALSE(testutil::AreByteSequencesEqual(originalReadExif, image->exif));

  const testutil::AvifRwData encoded =
      testutil::Encode(image.get(), AVIF_SPEED_FASTEST);
  const ImagePtr decoded = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(decoded, nullptr);

  // Exif orientation is still there but irot/imir do not match it.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(image->exif, decoded->exif));
  EXPECT_EQ(decoded->transformFlags, avifTransformFlags{AVIF_TRANSFORM_IMIR});
  EXPECT_EQ(decoded->irot.angle, 0u);
  EXPECT_EQ(decoded->imir.axis, image->imir.axis);

  // Exif orientation is discarded (set to 1) and samples are rotated/mirrored
  // according to irot/imir in JPEG export.
  const ImagePtr temp_image =
      WriteAndReadImage(*image, "paris_exif_orientation_2.jpg");
  ASSERT_NE(temp_image, nullptr);
  EXPECT_FALSE(testutil::AreByteSequencesEqual(image->exif, temp_image->exif));
  EXPECT_EQ(temp_image->transformFlags, AVIF_TRANSFORM_NONE);
  // Samples have been mirrored but you can't really tell from the dimensions.
  EXPECT_EQ(image->width, temp_image->width);
  EXPECT_EQ(image->height, temp_image->height);
}

TEST(MetadataTest, RotatedJpegBecauseOfIrotImir) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "paris_exif_orientation_5.jpg");
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(avifImageSetMetadataExif(image.get(), nullptr, 0),
            AVIF_RESULT_OK);  // Clear Exif.
  // Orientation is kept in irot/imir.
  EXPECT_EQ(image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            avifTransformFlags{AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR});
  EXPECT_EQ(image->irot.angle, 1u);
  EXPECT_EQ(image->imir.axis, 0u);

  // No Exif metadata to store the orientation: the samples should be rotated.
  const ImagePtr temp_image =
      WriteAndReadImage(*image, "paris_exif_orientation_5.jpg");
  ASSERT_NE(temp_image, nullptr);
  EXPECT_EQ(temp_image->exif.size, 0u);
  EXPECT_EQ(
      temp_image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
      avifTransformFlags{0});
  // Samples have been rotated.
  EXPECT_EQ(image->width, temp_image->height);
  EXPECT_EQ(image->height, temp_image->width);
}

TEST(MetadataTest, ExifIfdOffsetLoopingTo8) {
  const ImagePtr image(avifImageCreateEmpty());
  ASSERT_NE(image, nullptr);
  const uint8_t kBadExifPayload[128] = {
      'M', 'M', 0, 42,                          // TIFF header
      0,   0,   0, 8,                           // Offset to 0th IFD
      0,   1,                                   // fieldCount
      0,   0,   0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  // tag, type, count, valueOffset
      0,   0,   0, 8  // Invalid IFD offset, infinitely looping back to 0th IFD.
  };
  // avifImageSetMetadataExif() calls
  // avifImageExtractExifOrientationToIrotImir() internally.
  // The avifImageExtractExifOrientationToIrotImir() call should not enter an
  // infinite loop.
  ASSERT_EQ(avifImageSetMetadataExif(
                image.get(), kBadExifPayload,
                sizeof(kBadExifPayload) / sizeof(kBadExifPayload[0])),
            AVIF_RESULT_OK);
}

// The TIFF header may be located in the last four bytes of the payload.
// avifGetExifTiffHeaderOffset() must still find it there.
TEST(MetadataTest, ExifTiffHeaderOffsetAtEnd) {
  const uint8_t kLittleEndian[] = {0x00, 0x00, 'I', 'I', 42, 0};
  size_t offset;
  ASSERT_EQ(avifGetExifTiffHeaderOffset(kLittleEndian, sizeof(kLittleEndian),
                                        &offset),
            AVIF_RESULT_OK);
  EXPECT_EQ(offset, 2u);

  const uint8_t kBigEndian[] = {0x00, 'M', 'M', 0, 42};
  ASSERT_EQ(
      avifGetExifTiffHeaderOffset(kBigEndian, sizeof(kBigEndian), &offset),
      AVIF_RESULT_OK);
  EXPECT_EQ(offset, 1u);
}

//------------------------------------------------------------------------------
// Metadata rewriting (avifMetadataRewrite): replacing the Exif and XMP payloads of
// the primary item without re-encoding the coded AV1 samples.

namespace {

// Returns a read-only view of the given bytes.
avifROData RoData(const testutil::AvifRwData& data) {
  return avifROData{data.data, data.size};
}
avifROData RoData(const std::vector<uint8_t>& data) {
  return avifROData{data.data(), data.size()};
}
avifROData EmptyRoData() { return avifROData{nullptr, 0}; }

// Encodes a small image with the given Exif/XMP payloads.
testutil::AvifRwData EncodeWithMetadata(const avifROData& exif,
                                        const avifROData& xmp) {
  ImagePtr image = testutil::CreateImage(/*width=*/16, /*height=*/12,
                                         /*depth=*/8, AVIF_PIXEL_FORMAT_YUV444,
                                         AVIF_PLANES_YUV);
  if (image == nullptr) return {};
  testutil::FillImageGradient(image.get());
  if (exif.size != 0) {
    EXPECT_EQ(avifImageSetMetadataExif(image.get(), exif.data, exif.size),
              AVIF_RESULT_OK);
  }
  if (xmp.size != 0) {
    EXPECT_EQ(avifImageSetMetadataXMP(image.get(), xmp.data, xmp.size),
              AVIF_RESULT_OK);
  }
  return testutil::Encode(image.get(), AVIF_SPEED_FASTEST,
                          AVIF_QUALITY_LOSSLESS);
}

// Builds a minimal but valid big-endian Exif payload with the given orientation.
// The payload starts with three bytes of junk so that the TIFF header is *not* at
// offset 0, exercising the exif_tiff_header_offset handling.
std::vector<uint8_t> MakeExifPayload(uint8_t orientation,
                                     uint8_t filler = 0xEE) {
  std::vector<uint8_t> exif;
  // Two filler bytes before the TIFF header.
  exif.push_back(filler);
  exif.push_back(filler);
  // TIFF header (big-endian).
  exif.insert(exif.end(), {'M', 'M', 0, 42});
  // Offset to the 0th IFD.
  const uint32_t ifd_offset = 8;
  exif.push_back((ifd_offset >> 24) & 0xFF);
  exif.push_back((ifd_offset >> 16) & 0xFF);
  exif.push_back((ifd_offset >> 8) & 0xFF);
  exif.push_back(ifd_offset & 0xFF);
  // IFD: one entry (the orientation tag).
  exif.push_back(0);  // field_count high
  exif.push_back(1);  // field_count low
  // Tag 0x0112 (Orientation), type 3 (SHORT), count 1.
  exif.insert(exif.end(), {0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01});
  // Value: big-endian SHORT plus two padding bytes.
  exif.push_back(0x00);
  exif.push_back(orientation);
  exif.push_back(0x00);
  exif.push_back(0x00);
  // Offset to the next IFD.
  exif.insert(exif.end(), {0x00, 0x00, 0x00, 0x00});
  return exif;
}

std::vector<uint8_t> MakeXmpPayload(const std::string& description) {
  const std::string xmp =
      "<?xml version=\"1.0\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:description>" +
      description + "</dc:description></rdf:Description></rdf:RDF></x:xmpmeta>";
  return std::vector<uint8_t>(xmp.begin(), xmp.end());
}

// Compares only the coded pixel planes (ignoring all metadata), to prove the
// rewrite did not recompress anything.
bool PixelsEqual(const avifImage& a, const avifImage& b) {
  if (a.width != b.width || a.height != b.height || a.depth != b.depth ||
      a.yuvFormat != b.yuvFormat || a.yuvRange != b.yuvRange) {
    return false;
  }
  for (avifChannelIndex c :
       {AVIF_CHAN_Y, AVIF_CHAN_U, AVIF_CHAN_V, AVIF_CHAN_A}) {
    const uint8_t* row1 = avifImagePlane(&a, c);
    const uint8_t* row2 = avifImagePlane(&b, c);
    if ((row1 == nullptr) != (row2 == nullptr)) return false;
    if (row1 == nullptr) continue;
    if (avifImagePlaneRowBytes(&a, c) != avifImagePlaneRowBytes(&b, c)) {
      return false;
    }
    const size_t bytes = static_cast<size_t>(avifImagePlaneRowBytes(&a, c)) *
                         avifImagePlaneHeight(&a, c);
    if (std::memcmp(row1, row2, bytes) != 0) return false;
  }
  return true;
}

// Returns true if `needle` appears in `haystack`. Used to verify the XMP text is
// readable back byte for byte.
bool Contains(const avifRWData& haystack, const std::vector<uint8_t>& needle) {
  if (needle.size() > haystack.size) return false;
  return std::search(haystack.data, haystack.data + haystack.size,
                     needle.begin(), needle.end()) !=
         haystack.data + haystack.size;
}

}  // namespace

// The core requirement: the AV1 sample bytes must be identical before and after the
// metadata rewrite, and the new Exif/XMP must be readable back.
TEST(MetadataRewriteTest, CodedBytesUnchanged) {
  const std::vector<uint8_t> old_exif = MakeExifPayload(1);
  const std::vector<uint8_t> old_xmp = MakeXmpPayload("old description");
  const testutil::AvifRwData encoded =
      EncodeWithMetadata({old_exif.data(), old_exif.size()},
                         {old_xmp.data(), old_xmp.size()});
  ASSERT_NE(encoded.data, nullptr);
  const avifROData input = RoData(encoded);
  const ImagePtr original = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(original, nullptr);

  const std::vector<uint8_t> new_exif =
      MakeExifPayload(6);  // Different orientation, same TIFF layout.
  const std::vector<uint8_t> new_xmp = MakeXmpPayload(
      "new description that is noticeably longer than the previous one");
  const avifROData new_exif_ro = RoData(new_exif);
  const avifROData new_xmp_ro = RoData(new_xmp);
  testutil::AvifRwData rewritten;
  avifMetadataRewriteResult rewrite_result;
  ASSERT_EQ(avifMetadataRewrite(
                &input,
                &new_exif_ro,
                &new_xmp_ro,
                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE, &rewrite_result,
                &rewritten),
            AVIF_RESULT_OK);

  // The pixels decode identically: no recompression happened.
  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));

  // The new metadata is readable back.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, new_exif.data(), new_exif.size()));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->xmp.data, decoded->xmp.size, new_xmp.data(), new_xmp.size()));
  const std::vector<uint8_t> description_text = MakeXmpPayload(
      "new description that is noticeably longer than the previous one");
  EXPECT_TRUE(Contains(decoded->xmp, description_text));

  // With PRESERVE, the byte payload changed but the orientation interpretation
  // (irot/imir) did not move.
  EXPECT_EQ(rewrite_result.previousExifOrientation, 1u);
  EXPECT_EQ(rewrite_result.newExifOrientation, 6u);
  EXPECT_FALSE(rewrite_result.orientationPropertiesModified);
  EXPECT_EQ(decoded->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            original->transformFlags &
                (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR));
}

// The new Exif payload has a TIFF header offset that differs from the old one. The
// 4-byte exif_tiff_header_offset prefix stored in the file must be recomputed, not
// copied from the previous payload.
TEST(MetadataRewriteTest, ExifTiffHeaderOffsetRecomputed) {
  const std::vector<uint8_t> old_exif = MakeExifPayload(1, /*filler=*/0x11);
  const testutil::AvifRwData encoded =
      EncodeWithMetadata({old_exif.data(), old_exif.size()},
                         /*xmp=*/{nullptr, 0});
  ASSERT_NE(encoded.data, nullptr);
  const avifROData input = RoData(encoded);

  // Same TIFF content but with four filler bytes instead of two: the TIFF header
  // is now at offset 4 rather than offset 2.
  std::vector<uint8_t> new_exif = MakeExifPayload(1, /*filler=*/0x22);
  new_exif.insert(new_exif.begin(), {0xCC, 0xCC});  // Two extra leading bytes.
  const avifROData new_exif_ro = RoData(new_exif);

  testutil::AvifRwData rewritten;
  ASSERT_EQ(avifMetadataRewrite(
                &input,
                &new_exif_ro, /*replacementXMP=*/nullptr,
                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE, /*outResult=*/nullptr,
                &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, new_exif.data(), new_exif.size()));
  EXPECT_EQ(decoded->exif.data[0], 0xCC);
  EXPECT_EQ(decoded->exif.data[1], 0xCC);
  EXPECT_EQ(decoded->exif.data[2], 0x22);

  // The Exif orientation tag is still found by libavif.
  size_t orientation_offset = 0;
  ASSERT_EQ(avifGetExifOrientationOffset(decoded->exif.data, decoded->exif.size,
                                         &orientation_offset),
            AVIF_RESULT_OK);
  ASSERT_LT(orientation_offset, decoded->exif.size);
  EXPECT_EQ(decoded->exif.data[orientation_offset], 1u);
}

// Exif and XMP moving in opposite directions (one shrinks, the other grows) must
// not disturb the other items of the file.
TEST(MetadataRewriteTest, OppositeSizeChangesKeepOtherItems) {
  const std::vector<uint8_t> big_exif = MakeExifPayload(1);
  std::vector<uint8_t> padded_exif = big_exif;
  padded_exif.resize(4096, 0x5A);  // Inflate the old Exif with trailing bytes.
  const std::vector<uint8_t> small_xmp = MakeXmpPayload("x");
  const testutil::AvifRwData encoded =
      EncodeWithMetadata({padded_exif.data(), padded_exif.size()},
                         {small_xmp.data(), small_xmp.size()});
  ASSERT_NE(encoded.data, nullptr);
  const avifROData input = RoData(encoded);

  const std::vector<uint8_t> small_exif = MakeExifPayload(1);
  const std::vector<uint8_t> big_xmp = MakeXmpPayload(std::string(3000, 'd'));
  const avifROData small_exif_ro = RoData(small_exif);
  const avifROData big_xmp_ro = RoData(big_xmp);
  testutil::AvifRwData rewritten;
  ASSERT_EQ(avifMetadataRewrite(
                &input,
                &small_exif_ro,
                &big_xmp_ro,
                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE, /*outResult=*/nullptr,
                &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr original = testutil::Decode(encoded.data, encoded.size);
  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(original, nullptr);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, small_exif.data(),
      small_exif.size()));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(decoded->xmp.data, decoded->xmp.size,
                                              big_xmp.data(), big_xmp.size()));
}

// Following the new Exif orientation updates irot/imir without touching the pixels.
// Orientation 5 and 7 both map to {irot, imir}, so the property set is unchanged and
// only the existing irot angle byte moves (mirroring stays around the same axis).
TEST(MetadataRewriteTest, OrientationFromExif) {
  const std::vector<uint8_t> old_exif = MakeExifPayload(5);
  const testutil::AvifRwData encoded =
      EncodeWithMetadata({old_exif.data(), old_exif.size()},
                         /*xmp=*/{nullptr, 0});
  ASSERT_NE(encoded.data, nullptr);
  const avifROData input = RoData(encoded);

  const ImagePtr original = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(original, nullptr);
  ASSERT_EQ(original->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            avifTransformFlags{AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR});
  ASSERT_EQ(original->irot.angle, 1u);

  const std::vector<uint8_t> new_exif = MakeExifPayload(7);  // irot.angle = 3.
  const avifROData new_exif_ro = RoData(new_exif);
  testutil::AvifRwData rewritten;
  avifMetadataRewriteResult rewrite_result;
  ASSERT_EQ(avifMetadataRewrite(&input,
                                &new_exif_ro,
                                /*replacementXMP=*/nullptr,
                                AVIF_METADATA_REWRITE_ORIENTATION_FROM_EXIF,
                                &rewrite_result, &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  // AV1 samples were not re-encoded, but the orientation property now says "7".
  EXPECT_EQ(decoded->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR),
            avifTransformFlags{AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR});
  EXPECT_EQ(decoded->irot.angle, 3u);
  EXPECT_EQ(decoded->imir.axis, 0u);
  EXPECT_EQ(rewrite_result.previousExifOrientation, 5u);
  EXPECT_EQ(rewrite_result.newExifOrientation, 7u);
  EXPECT_TRUE(rewrite_result.orientationPropertiesModified);

  // Decoding the rewritten file must still yield the same pixel bytes: the rotation
  // is metadata, not a resample.
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
}

// Rewriting through an auxiliary program name attached to the primary item: an
// alpha auxiliary item must still be decodable after the rewrite.
TEST(MetadataRewriteTest, AlphaItemUnaffected) {
  ImagePtr image = testutil::CreateImage(/*width=*/16, /*height=*/12,
                                         /*depth=*/8, AVIF_PIXEL_FORMAT_YUV444,
                                         AVIF_PLANES_ALL);
  ASSERT_NE(image, nullptr);
  testutil::FillImageGradient(image.get());
  const std::vector<uint8_t> old_exif = MakeExifPayload(1);
  ASSERT_EQ(avifImageSetMetadataExif(image.get(), old_exif.data(),
                                     old_exif.size()),
            AVIF_RESULT_OK);
  const testutil::AvifRwData encoded =
      testutil::Encode(image.get(), AVIF_SPEED_FASTEST);
  ASSERT_NE(encoded.data, nullptr);
  const avifROData input = RoData(encoded);

  const std::vector<uint8_t> new_exif = MakeExifPayload(1, /*filler=*/0x33);
  const avifROData new_exif_ro = RoData(new_exif);
  testutil::AvifRwData rewritten;
  ASSERT_EQ(avifMetadataRewrite(
                &input,
                &new_exif_ro,
                /*replacementXMP=*/nullptr,
                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE, nullptr, &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  ASSERT_NE(decoded->alphaPlane, nullptr);
  const ImagePtr original = testutil::Decode(encoded.data, encoded.size);
  ASSERT_NE(original, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
}

// In an image sequence, the Exif/XMP payloads are stored twice: once in the root meta box
// and once in the color track's own meta box. The decoder reads the track copy, so both
// copies must be rewritten for the new values to be observable after decoding.
TEST(MetadataRewriteTest, AnimatedTrackMetadataUpdated) {
  const std::string path =
      std::string(data_path) + "colors-animated-8bpc-alpha-exif-xmp.avif";
  const testutil::AvifRwData file = testutil::ReadFile(path);
  ASSERT_NE(file.data, nullptr);
  const ImagePtr original = testutil::DecodeFile(path);
  ASSERT_NE(original, nullptr);

  const std::vector<uint8_t> new_exif = MakeExifPayload(6, /*filler=*/0x77);
  const std::vector<uint8_t> new_xmp = MakeXmpPayload("archive sequence description");
  const avifROData input = RoData(file);
  const avifROData new_exif_ro = RoData(new_exif);
  const avifROData new_xmp_ro = RoData(new_xmp);
  testutil::AvifRwData rewritten;
  ASSERT_EQ(avifMetadataRewrite(&input, &new_exif_ro, &new_xmp_ro,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                /*outResult=*/nullptr, &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
  // The values read back by the decoder are the new ones, for the track copy too.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, new_exif.data(), new_exif.size()));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->xmp.data, decoded->xmp.size, new_xmp.data(), new_xmp.size()));
}

// The rewrite must work on files whose primary image is stored as an item with a real
// pre-existing Exif and XMP payload (not the synthetic ones from this test).
TEST(MetadataRewriteTest, RealStillFileRoundTrip) {
  const std::string path = std::string(data_path) + "paris_icc_exif_xmp.avif";
  const testutil::AvifRwData file = testutil::ReadFile(path);
  ASSERT_NE(file.data, nullptr);
  const ImagePtr original = testutil::DecodeFile(path);
  ASSERT_NE(original, nullptr);

  const std::vector<uint8_t> new_exif = MakeExifPayload(3, /*filler=*/0x55);
  const std::vector<uint8_t> new_xmp = MakeXmpPayload("archive still description");
  const avifROData input = RoData(file);
  const avifROData new_xmp_ro = RoData(new_xmp);
  const avifROData new_exif_ro = RoData(new_exif);
  testutil::AvifRwData rewritten;
  avifMetadataRewriteResult rewrite_result;
  ASSERT_EQ(avifMetadataRewrite(&input, &new_exif_ro, &new_xmp_ro,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                &rewrite_result, &rewritten),
            AVIF_RESULT_OK);
  EXPECT_EQ(rewrite_result.previousExifOrientation, 1u);
  EXPECT_EQ(rewrite_result.newExifOrientation, 3u);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, new_exif.data(), new_exif.size()));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->xmp.data, decoded->xmp.size, new_xmp.data(), new_xmp.size()));
  // The ICC profile is untouched.
  EXPECT_TRUE(testutil::AreByteSequencesEqual(original->icc, decoded->icc));
}

// A file whose last top-level box uses size 0 (extends to end of file) must still be
// rewritable: the size field is patched so the new mdat becomes a proper sibling.
TEST(MetadataRewriteTest, TrailingSizeZeroBox) {
  const std::vector<uint8_t> old_exif = MakeExifPayload(1);
  const testutil::AvifRwData encoded =
      EncodeWithMetadata({old_exif.data(), old_exif.size()}, {nullptr, 0});
  ASSERT_NE(encoded.data, nullptr);

  // The last top-level box of the encoded file is its mdat. Zero its size field.
  size_t mdat_pos = SIZE_MAX;
  size_t pos = 0;
  while (pos + 8 <= encoded.size) {
    uint32_t size = (static_cast<uint32_t>(encoded.data[pos]) << 24) |
                    (static_cast<uint32_t>(encoded.data[pos + 1]) << 16) |
                    (static_cast<uint32_t>(encoded.data[pos + 2]) << 8) |
                    encoded.data[pos + 3];
    if (std::memcmp(encoded.data + pos + 4, "mdat", 4) == 0) {
      mdat_pos = pos;
    }
    if (size < 8) break;
    pos += size;
  }
  ASSERT_NE(mdat_pos, SIZE_MAX);
  std::vector<uint8_t> zero_sized(encoded.data, encoded.data + encoded.size);
  zero_sized[mdat_pos] = zero_sized[mdat_pos + 1] = 0;
  zero_sized[mdat_pos + 2] = zero_sized[mdat_pos + 3] = 0;
  const avifROData input = {zero_sized.data(), zero_sized.size()};
  const ImagePtr original = testutil::Decode(zero_sized.data(), zero_sized.size());
  ASSERT_NE(original, nullptr);

  const std::vector<uint8_t> new_exif = MakeExifPayload(2, /*filler=*/0x44);
  const avifROData new_exif_ro = RoData(new_exif);
  testutil::AvifRwData rewritten;
  ASSERT_EQ(avifMetadataRewrite(&input, &new_exif_ro, nullptr,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                nullptr, &rewritten),
            AVIF_RESULT_OK);

  const ImagePtr decoded = testutil::Decode(rewritten.data, rewritten.size);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(PixelsEqual(*original, *decoded));
  EXPECT_TRUE(testutil::AreByteSequencesEqual(
      decoded->exif.data, decoded->exif.size, new_exif.data(), new_exif.size()));
}

// Invalid inputs are rejected without producing output.
TEST(MetadataRewriteTest, RejectsInvalidArguments) {
  testutil::AvifRwData encoded{};
  {
    const std::vector<uint8_t> exif = MakeExifPayload(1);
    const testutil::AvifRwData tmp =
        EncodeWithMetadata({exif.data(), exif.size()}, {nullptr, 0});
    ASSERT_EQ(avifRWDataSet(&encoded, tmp.data, tmp.size), AVIF_RESULT_OK);
  }
  testutil::AvifRwData output;
  const std::vector<uint8_t> exif = MakeExifPayload(1);
  const avifROData input = RoData(encoded);
  const avifROData exif_ro = RoData(exif);
  const avifROData empty_ro = EmptyRoData();

  // No replacement at all.
  EXPECT_EQ(avifMetadataRewrite(&input, nullptr,
                                nullptr,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                nullptr, &output),
            AVIF_RESULT_INVALID_ARGUMENT);
  // Empty input.
  EXPECT_EQ(avifMetadataRewrite(&empty_ro,
                                &exif_ro, nullptr,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                nullptr, &output),
            AVIF_RESULT_INVALID_ARGUMENT);
  // Invalid Exif payload (no TIFF header).
  const uint8_t kNotExif[] = {1, 2, 3, 4, 5, 6};
  const avifROData not_exif_ro = avifROData{kNotExif, sizeof(kNotExif)};
  EXPECT_EQ(avifMetadataRewrite(&input,
                                &not_exif_ro, nullptr,
                                AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE,
                                nullptr, &output),
            AVIF_RESULT_INVALID_EXIF_PAYLOAD);
  EXPECT_EQ(output.data, nullptr);
  EXPECT_EQ(output.size, 0u);
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------

TEST(MetadataTest, ExtendedXMP) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "dog_exif_extended_xmp_icc.jpg");
  ASSERT_NE(image, nullptr);
  ASSERT_NE(image->xmp.size, 0u);
  ASSERT_LT(image->xmp.size,
            size_t{65503});  // Fits in a single JPEG APP1 marker.

  for (const char* temp_file_name : {"dog.png", "dog.jpg"}) {
    const ImagePtr temp_image = WriteAndReadImage(*image, temp_file_name);
    ASSERT_NE(temp_image, nullptr);
    EXPECT_TRUE(testutil::AreByteSequencesEqual(image->xmp, temp_image->xmp));
  }
}

TEST(MetadataTest, MultipleExtendedXMPAndAlternativeGUIDTag) {
  const ImagePtr image =
      testutil::ReadImage(data_path, "paris_extended_xmp.jpg");
  ASSERT_NE(image, nullptr);
  ASSERT_GT(image->xmp.size, size_t{65536 * 2});

  ImagePtr temp_image = WriteAndReadImage(*image, "paris_extended_xmp.png");
  ASSERT_NE(temp_image, nullptr);
  EXPECT_TRUE(testutil::AreByteSequencesEqual(image->xmp, temp_image->xmp));

  // Writing more than 65502 bytes of XMP in a JPEG is not supported.
  temp_image = WriteAndReadImage(*image, "paris_extended_xmp.jpg");
  ASSERT_NE(temp_image, nullptr);
  ASSERT_EQ(temp_image->xmp.size, 0u);  // XMP was dropped.
}

//------------------------------------------------------------------------------

// Regression test for https://github.com/AOMediaCodec/libavif/issues/1333.
// Coverage for avifImageFixXMP().
TEST(MetadataTest, XMPWithTrailingNullCharacter) {
  ImagePtr jpg = testutil::ReadImage(data_path, "paris_xmp_trailing_null.jpg");
  ASSERT_NE(jpg, nullptr);
  ASSERT_NE(jpg->xmp.size, 0u);
  // avifJPEGRead() should strip the trailing null character.
  ASSERT_EQ(std::memchr(jpg->xmp.data, '\0', jpg->xmp.size), nullptr);

  // Append a zero byte to see what happens when encoded with libpng.
  ASSERT_EQ(avifRWDataRealloc(&jpg->xmp, jpg->xmp.size + 1), AVIF_RESULT_OK);
  jpg->xmp.data[jpg->xmp.size - 1] = '\0';
  testutil::WriteImage(jpg.get(),
                       (testing::TempDir() + "xmp_trailing_null.png").c_str());
  const ImagePtr png =
      testutil::ReadImage(testing::TempDir().c_str(), "xmp_trailing_null.png");
  ASSERT_NE(png, nullptr);
  ASSERT_NE(png->xmp.size, 0u);
  // avifPNGRead() should strip the trailing null character, but the libpng
  // export during testutil::WriteImage() probably took care of that anyway.
  ASSERT_EQ(std::memchr(png->xmp.data, '\0', png->xmp.size), nullptr);
}

//------------------------------------------------------------------------------
// Multiple targets in a single item type reference box

// Section 8.11.12.1 of ISO/IEC 14496-12 represents the targets of a
// SingleItemTypeReferenceBox as an array of to_item_IDs. libavif used to keep
// one target per item, so the target parsed last silently won.
// In circle_cdsc_two_targets.avif the Exif item describes items [1, 2]. The
// Exif metadata used to disappear without any error, exit code or diagnostic;
// swapping the two IDs made it reappear. It must survive either way.
TEST(MetadataTest, CdscWithMultipleTargets) {
  ImagePtr decoded(avifImageCreateEmpty());
  ASSERT_NE(decoded, nullptr);
  DecoderPtr decoder(avifDecoderCreate());
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(
      avifDecoderReadFile(
          decoder.get(), decoded.get(),
          (std::string(data_path) + "circle_cdsc_two_targets.avif").c_str()),
      AVIF_RESULT_OK);
  EXPECT_NE(decoded->exif.size, 0u);
  EXPECT_NE(decoded->exif.data, nullptr);
}

// Same for the auxiliary item relation. The consequence is heavier than for
// cdsc: in circle_auxl_two_targets.avif the alpha item is declared auxiliary
// for items [1, 3], so the alpha plane used to be dropped and a transparent
// image decoded as opaque, with no error, no diagnostic and an exit code of
// zero.
TEST(MetadataTest, AuxlWithMultipleTargets) {
  ImagePtr decoded(avifImageCreateEmpty());
  ASSERT_NE(decoded, nullptr);
  DecoderPtr decoder(avifDecoderCreate());
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(
      avifDecoderReadFile(
          decoder.get(), decoded.get(),
          (std::string(data_path) + "circle_auxl_two_targets.avif").c_str()),
      AVIF_RESULT_OK);
  ASSERT_NE(decoded->alphaPlane, nullptr);
  EXPECT_NE(decoded->alphaRowBytes, 0u);
}

//------------------------------------------------------------------------------

}  // namespace
}  // namespace avif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    std::cerr << "There must be exactly one argument containing the path to "
                 "the test data folder"
              << std::endl;
    return 1;
  }
  avif::data_path = argv[1];
  return RUN_ALL_TESTS();
}
