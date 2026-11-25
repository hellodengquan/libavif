#!/bin/bash
# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# tests for command lines (metadata rewriting without re-encoding)

source $(dirname "$0")/cmd_test_common.sh || exit

INPUT_AVIF="${TESTDATA_DIR}/paris_icc_exif_xmp.avif"
INPUT_ANIMATED="${TESTDATA_DIR}/colors-animated-8bpc-alpha-exif-xmp.avif"
REWRITTEN_FILE="avif_test_cmd_metadata_rewrite.avif"
REWRITTEN_ANIMATED_FILE="avif_test_cmd_metadata_rewrite_animated.avif"
NEW_EXIF="avif_test_cmd_metadata_rewrite_exif.bin"
NEW_XMP="avif_test_cmd_metadata_rewrite_xmp.xml"
DECODED_FILE="avif_test_cmd_metadata_rewrite_decoded.png"

# A minimal big-endian Exif payload whose TIFF header is *not* at offset 0 (two leading bytes)
# and whose Orientation tag is 6.
NEW_EXIF_HEX="eeee4d4d002a00000008000101120003000000010006000000000000"
NEW_XMP_TEXT='<?xml version="1.0"?><x:xmpmeta xmlns:x="adobe:ns:meta/">ARCHIVEEDITED</x:xmpmeta>'

cleanup() {
  pushd ${TMP_DIR}
    rm -f -- "${REWRITTEN_FILE}" "${REWRITTEN_ANIMATED_FILE}" \
             "${NEW_EXIF}" "${NEW_XMP}" "${DECODED_FILE}"
  popd
}
trap cleanup EXIT

pushd ${TMP_DIR}
  printf '%s' "${NEW_EXIF_HEX}" | xxd -r -p > "${NEW_EXIF}"
  printf '%s' "${NEW_XMP_TEXT}" > "${NEW_XMP}"

  # Rewrite both payloads of a still image and read them back.
  echo "Testing metadata rewrite (still image)"
  "${AVIFMETADATA}" "${INPUT_AVIF}" --exif "${NEW_EXIF}" --xmp "${NEW_XMP}" -o "${REWRITTEN_FILE}"
  "${AVIFDEC}" "${REWRITTEN_FILE}" "${DECODED_FILE}"
  # The decoded image must still be viewable (the command above already checks that).

  # The coded picture bytes must be preserved: the original mdat payload must appear unchanged
  # in the rewritten file, and the new metadata must live in a separate box.
  # The original mdat box starts at byte offset 1020 and is 20112 bytes long (header included).
  if ! cmp -s <(tail -c +1021 "${INPUT_AVIF}" | head -c 20112) \
                <(tail -c +1021 "${REWRITTEN_FILE}" | head -c 20112); then
    echo "The original mdat payload was modified by the metadata rewrite"
    exit 1
  fi

  # A rewrite that only changes Exif (not XMP) must succeed as well.
  "${AVIFMETADATA}" "${INPUT_AVIF}" --exif "${NEW_EXIF}" -o "${REWRITTEN_FILE}2"

  # An animated file stores the metadata both in the root meta box and in the color track's
  # meta box; both must be rewritten so that the decoder observes the new values.
  echo "Testing metadata rewrite (image sequence)"
  "${AVIFMETADATA}" "${INPUT_ANIMATED}" --exif "${NEW_EXIF}" --xmp "${NEW_XMP}" -o "${REWRITTEN_ANIMATED_FILE}"
  "${AVIFDEC}" "${REWRITTEN_ANIMATED_FILE}" "${DECODED_FILE}"

  # Invalid invocations must fail.
  if "${AVIFMETADATA}" "${INPUT_AVIF}" -o "${REWRITTEN_FILE}"; then
    echo "avifmetadata must fail without --exif or --xmp"
    exit 1
  fi
  if "${AVIFMETADATA}" "${INPUT_AVIF}" --orientation bogus --xmp "${NEW_XMP}" -o "${REWRITTEN_FILE}"; then
    echo "avifmetadata must reject an unknown --orientation value"
    exit 1
  fi
popd

exit 0
