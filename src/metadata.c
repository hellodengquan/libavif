// Copyright 2026 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

// Metadata-only rewrite of the Exif and XMP payloads attached to the primary item of an
// AVIF file.
//
// The encoded AV1 sample bytes are never decoded, re-encoded or even moved. The input file
// is copied unchanged and the replacement metadata payloads are appended in a new
// MediaDataBox at the very end of the file. Only the 'iloc' extent_offset/extent_length
// fields of the Exif and XMP items are patched so that they point at the new payloads.
//
// Because the size of the 'meta' box does not change and no existing box moves, every other
// item keeps its original absolute byte offsets: the coded color image, the alpha item, the
// gain map item, grid tiles, track sample tables and any auxiliary item still read exactly
// the same bytes as before. This is what makes the update truly lossless with respect to the
// compressed picture.
//
// The 'irot'/'imir' transformative properties are preserved byte-for-byte by default, so the
// way the primary image is oriented always stays the same. When requested, they can instead
// be updated in place (without resizing the meta box) to follow the orientation tag of the
// new Exif payload.

#include "avif/internal.h"

#include <stdint.h>
#include <string.h>

#define AVIF_METADATA_CONTENT_TYPE_SIZE 64
#define AVIF_METADATA_MAX_EXTENTS 1024

// ---------------------------------------------------------------------------
// Bounds-checked big-endian cursor over the whole input file.

typedef struct avifMetadataCursor
{
    const uint8_t * data;
    size_t size;
    size_t pos;
} avifMetadataCursor;

static avifBool avifMetadataCursorHas(const avifMetadataCursor * c, size_t byteCount)
{
    return (c->pos <= c->size) && (byteCount <= c->size - c->pos);
}

static avifBool avifMetadataCursorRead(avifMetadataCursor * c, size_t byteCount, const uint8_t ** out)
{
    if (!avifMetadataCursorHas(c, byteCount)) {
        return AVIF_FALSE;
    }
    if (out != NULL) {
        *out = c->data + c->pos;
    }
    c->pos += byteCount;
    return AVIF_TRUE;
}

static avifBool avifMetadataCursorSkip(avifMetadataCursor * c, size_t byteCount)
{
    return avifMetadataCursorRead(c, byteCount, NULL);
}

static avifBool avifMetadataReadU16(avifMetadataCursor * c, uint16_t * v)
{
    const uint8_t * p;
    if (!avifMetadataCursorRead(c, 2, &p)) {
        return AVIF_FALSE;
    }
    *v = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return AVIF_TRUE;
}

static avifBool avifMetadataReadU32(avifMetadataCursor * c, uint32_t * v)
{
    const uint8_t * p;
    if (!avifMetadataCursorRead(c, 4, &p)) {
        return AVIF_FALSE;
    }
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return AVIF_TRUE;
}

// Reads a big-endian unsigned integer of byteCount bytes (0 to 8).
static avifBool avifMetadataReadUX(avifMetadataCursor * c, uint8_t byteCount, uint64_t * v)
{
    if (byteCount > 8) {
        return AVIF_FALSE;
    }
    const uint8_t * p;
    if (!avifMetadataCursorRead(c, byteCount, &p)) {
        return AVIF_FALSE;
    }
    uint64_t result = 0;
    for (uint8_t i = 0; i < byteCount; ++i) {
        result = (result << 8) | p[i];
    }
    *v = result;
    return AVIF_TRUE;
}

// Reads a NUL-terminated string and returns its length (without the NUL), or AVIF_FALSE.
static avifBool avifMetadataReadString(avifMetadataCursor * c, size_t * length)
{
    const uint8_t * start = c->data + c->pos;
    const size_t remaining = c->size - c->pos;
    for (size_t i = 0; i < remaining; ++i) {
        if (start[i] == 0) {
            if (length != NULL) {
                *length = i;
            }
            c->pos += i + 1;
            return AVIF_TRUE;
        }
    }
    return AVIF_FALSE;
}

// ---------------------------------------------------------------------------
// A parsed box header.

typedef struct avifMetadataBox
{
    size_t start;       // Offset of the box size field in the file.
    size_t headerSize;  // 8, 16 (largesize) or 24 (uuid) bytes.
    size_t payloadSize; // Size of the box content, excluding the header.
    size_t end;         // start + headerSize + payloadSize.
    avifBool isSizeZero;
    uint8_t type[4];
} avifMetadataBox;

static avifBool avifMetadataReadBox(avifMetadataCursor * c, avifMetadataBox * box)
{
    const size_t start = c->pos;
    uint32_t smallSize;
    if (!avifMetadataReadU32(c, &smallSize)) {
        return AVIF_FALSE;
    }
    const uint8_t * type;
    if (!avifMetadataCursorRead(c, 4, &type)) {
        return AVIF_FALSE;
    }
    memcpy(box->type, type, 4);
    uint64_t boxSize = smallSize;
    size_t headerSize = 8;
    box->isSizeZero = AVIF_FALSE;
    if (smallSize == 1) {
        // unsigned int(64) largesize;
        const uint8_t * p;
        if (!avifMetadataCursorRead(c, 8, &p)) {
            return AVIF_FALSE;
        }
        boxSize = 0;
        for (int i = 0; i < 8; ++i) {
            boxSize = (boxSize << 8) | p[i];
        }
        headerSize = 16;
    } else if (smallSize == 0) {
        // A box with size 0 extends to the end of the enclosing container.
        box->isSizeZero = AVIF_TRUE;
        boxSize = c->size - start;
    }
    if (!memcmp(box->type, "uuid", 4)) {
        if (!avifMetadataCursorSkip(c, 16)) {
            return AVIF_FALSE;
        }
        headerSize += 16;
    }
    if (boxSize < headerSize || boxSize > c->size - start) {
        return AVIF_FALSE;
    }
    box->start = start;
    box->headerSize = headerSize;
    box->payloadSize = (size_t)(boxSize - headerSize);
    box->end = start + (size_t)boxSize;
    c->pos = box->end;
    return AVIF_TRUE;
}

// ---------------------------------------------------------------------------
// One item's worth of information parsed from a meta box.

typedef struct avifMetadataExtent
{
    size_t offsetFieldPos; // Absolute file offset of the extent_offset field.
    size_t lengthFieldPos; // Absolute file offset of the extent_length field.
    uint8_t offsetSize;
    uint8_t lengthSize;
    uint64_t absoluteOffset;
    uint64_t length;
} avifMetadataExtent;
AVIF_ARRAY_DECLARE(avifMetadataExtentArray, avifMetadataExtent, extent);

typedef struct avifMetadataItem
{
    uint32_t id;
    uint8_t type[4];
    char contentType[AVIF_METADATA_CONTENT_TYPE_SIZE]; // Only meaningful for 'mime' items.
    avifBool hasContentType;

    avifBool inIloc;
    avifBool idatStored; // construction_method == 1
    uint64_t baseOffset; // base_offset field of the iloc entry.
    avifMetadataExtentArray extents;

    avifBool describesImage; // True if referenced by a 'cdsc' iref whose target is the image item of its meta box.
} avifMetadataItem;
AVIF_ARRAY_DECLARE(avifMetadataItemArray, avifMetadataItem, item);

// One 'meta' box (root level or inside a track) and its items.
typedef struct avifMetadataMetaBox
{
    size_t start;
    size_t end;
    avifBool isRoot;
    uint32_t pitm; // Primary item ID declared by this meta box's 'pitm' box, or 0.
    avifMetadataItemArray items;
} avifMetadataMetaBox;
AVIF_ARRAY_DECLARE(avifMetadataMetaBoxArray, avifMetadataMetaBox, meta);

// Transform properties of an image item, located so their value bytes can be patched.
typedef struct avifMetadataOrientation
{
    avifBool hasIrot;
    size_t irotAngleBytePos; // Absolute file offset of the byte holding the 2-bit angle.

    avifBool hasImir;
    size_t imirAxisBytePos; // Absolute file offset of the byte holding the 1-bit axis.
} avifMetadataOrientation;

typedef struct avifMetadataFile
{
    size_t fileEndSize; // Total size of the input file.

    // Offset of the last top-level box's size field if that box uses size 0 (extends to the
    // end of the file), or 0 if the last top-level box has an explicit size. When set, the
    // size field is rewritten to the box's new size before the payload is appended.
    size_t lastTopLevelSizeZeroFieldPos;

    // Root-level meta box, if any.
    avifMetadataMetaBox * rootMeta;

    // Every meta box in the file, in file order.
    avifMetadataMetaBoxArray metaBoxes;

    // irot/imir of the image item in the root meta box.
    avifMetadataOrientation orientation;
} avifMetadataFile;

static void avifMetadataMetaBoxDestroy(avifMetadataMetaBox * meta)
{
    for (uint32_t i = 0; i < meta->items.count; ++i) {
        avifArrayDestroy(&meta->items.item[i].extents);
    }
    avifArrayDestroy(&meta->items);
}

static void avifMetadataFileDestroy(avifMetadataFile * file)
{
    for (uint32_t i = 0; i < file->metaBoxes.count; ++i) {
        avifMetadataMetaBoxDestroy(&file->metaBoxes.meta[i]);
    }
    avifArrayDestroy(&file->metaBoxes);
}

static avifMetadataItem * avifMetadataFindItem(avifMetadataMetaBox * meta, uint32_t itemID, avifBool create)
{
    for (uint32_t i = 0; i < meta->items.count; ++i) {
        if (meta->items.item[i].id == itemID) {
            return &meta->items.item[i];
        }
    }
    if (!create) {
        return NULL;
    }
    avifMetadataItem * item = (avifMetadataItem *)avifArrayPush(&meta->items);
    if (item == NULL) {
        return NULL;
    }
    memset(item, 0, sizeof(*item));
    item->id = itemID;
    if (!avifArrayCreate(&item->extents, sizeof(avifMetadataExtent), 1)) {
        avifArrayPop(&meta->items);
        return NULL;
    }
    return item;
}

// ---------------------------------------------------------------------------
// Per-box parsing helpers. Each returns AVIF_RESULT_OK on success.

static avifResult avifMetadataParsePitm(avifMetadataMetaBox * meta, const avifMetadataBox * box, const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    const uint8_t * versionAndFlags;
    if (!avifMetadataCursorRead(&c, 4, &versionAndFlags)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t version = versionAndFlags[0];
    uint32_t itemID;
    if (version == 0) {
        uint16_t tmp;
        if (!avifMetadataReadU16(&c, &tmp)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        itemID = tmp;
    } else {
        if (!avifMetadataReadU32(&c, &itemID)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
    }
    meta->pitm = itemID;
    return AVIF_RESULT_OK;
}

static avifResult avifMetadataParseIloc(avifMetadataMetaBox * meta, const avifMetadataBox * box, const uint8_t * fileData)
{
    // Section 8.11.3.2 of ISO/IEC 14496-12.
    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    const uint8_t * versionAndFlags;
    if (!avifMetadataCursorRead(&c, 4, &versionAndFlags)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t version = versionAndFlags[0];
    if (version > 2) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t * sizes;
    if (!avifMetadataCursorRead(&c, 2, &sizes)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t offsetSize = (uint8_t)(sizes[0] >> 4);
    const uint8_t lengthSize = (uint8_t)(sizes[0] & 0x0F);
    const uint8_t baseOffsetSize = (uint8_t)(sizes[1] >> 4);
    const uint8_t indexSize = (version == 1 || version == 2) ? (uint8_t)(sizes[1] & 0x0F) : 0;
    if ((offsetSize != 0 && offsetSize != 4 && offsetSize != 8) || (lengthSize != 0 && lengthSize != 4 && lengthSize != 8) ||
        (baseOffsetSize != 0 && baseOffsetSize != 4 && baseOffsetSize != 8) || (indexSize != 0 && indexSize != 4 && indexSize != 8)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }

    uint32_t itemCount;
    if (version < 2) {
        uint16_t tmp;
        if (!avifMetadataReadU16(&c, &tmp)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        itemCount = tmp;
    } else {
        if (!avifMetadataReadU32(&c, &itemCount)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
    }

    for (uint32_t i = 0; i < itemCount; ++i) {
        uint32_t itemID;
        if (version < 2) {
            uint16_t tmp;
            if (!avifMetadataReadU16(&c, &tmp)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            itemID = tmp;
        } else {
            if (!avifMetadataReadU32(&c, &itemID)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
        }
        if (itemID == 0) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }

        avifMetadataItem * item = avifMetadataFindItem(meta, itemID, AVIF_TRUE);
        if (item == NULL) {
            return AVIF_RESULT_OUT_OF_MEMORY;
        }
        if (item->inIloc) {
            return AVIF_RESULT_BMFF_PARSE_FAILED; // Duplicate iloc entries for one item.
        }
        item->inIloc = AVIF_TRUE;

        if (version == 1 || version == 2) {
            uint16_t tmp;
            if (!avifMetadataReadU16(&c, &tmp)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            const uint8_t constructionMethod = (uint8_t)(tmp & 0x0F);
            if (constructionMethod != 0 && constructionMethod != 1) {
                return AVIF_RESULT_NOT_IMPLEMENTED; // construction method 2 is unsupported.
            }
            item->idatStored = (constructionMethod == 1);
        }

        uint16_t dataReferenceIndex;
        if (!avifMetadataReadU16(&c, &dataReferenceIndex)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        uint64_t baseOffset;
        if (!avifMetadataReadUX(&c, baseOffsetSize, &baseOffset)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        item->baseOffset = baseOffset;
        uint16_t extentCount;
        if (!avifMetadataReadU16(&c, &extentCount)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (extentCount > AVIF_METADATA_MAX_EXTENTS) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        for (uint16_t extentIndex = 0; extentIndex < extentCount; ++extentIndex) {
            if ((version == 1 || version == 2) && indexSize > 0) {
                uint64_t itemReferenceIndex;
                if (!avifMetadataReadUX(&c, indexSize, &itemReferenceIndex)) {
                    return AVIF_RESULT_BMFF_PARSE_FAILED;
                }
            }
            avifMetadataExtent * extent = (avifMetadataExtent *)avifArrayPush(&item->extents);
            if (extent == NULL) {
                return AVIF_RESULT_OUT_OF_MEMORY;
            }
            memset(extent, 0, sizeof(*extent));
            extent->offsetSize = offsetSize;
            extent->lengthSize = lengthSize;
            extent->offsetFieldPos = c.pos;
            uint64_t extentOffset;
            if (!avifMetadataReadUX(&c, offsetSize, &extentOffset)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            extent->lengthFieldPos = c.pos;
            uint64_t extentLength;
            if (!avifMetadataReadUX(&c, lengthSize, &extentLength)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            if (extentOffset > UINT64_MAX - baseOffset) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            extent->absoluteOffset = baseOffset + extentOffset;
            extent->length = extentLength;
        }
    }
    return AVIF_RESULT_OK;
}

static avifResult avifMetadataParseInfe(avifMetadataMetaBox * meta, const avifMetadataBox * box, const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    const uint8_t * versionAndFlags;
    if (!avifMetadataCursorRead(&c, 4, &versionAndFlags)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t version = versionAndFlags[0];
    if (version != 2 && version != 3) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    uint32_t itemID;
    if (version == 2) {
        uint16_t tmp;
        if (!avifMetadataReadU16(&c, &tmp)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        itemID = tmp;
    } else {
        if (!avifMetadataReadU32(&c, &itemID)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
    }
    uint16_t itemProtectionIndex;
    if (!avifMetadataReadU16(&c, &itemProtectionIndex)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t * itemType;
    if (!avifMetadataCursorRead(&c, 4, &itemType)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    if (!avifMetadataReadString(&c, NULL)) { // item_name
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }

    avifMetadataItem * item = avifMetadataFindItem(meta, itemID, AVIF_TRUE);
    if (item == NULL) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    memcpy(item->type, itemType, 4);

    if (!memcmp(itemType, "mime", 4)) {
        const uint8_t * start = fileData + c.pos;
        size_t length;
        if (!avifMetadataReadString(&c, &length)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (length >= AVIF_METADATA_CONTENT_TYPE_SIZE) {
            length = AVIF_METADATA_CONTENT_TYPE_SIZE - 1;
        }
        memcpy(item->contentType, start, length);
        item->contentType[length] = '\0';
        item->hasContentType = AVIF_TRUE;
        // content_encoding is optional and ignored.
    }
    return AVIF_RESULT_OK;
}

static avifResult avifMetadataParseIinf(avifMetadataMetaBox * meta, const avifMetadataBox * box, const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    const uint8_t * versionAndFlags;
    if (!avifMetadataCursorRead(&c, 4, &versionAndFlags)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t version = versionAndFlags[0];
    uint32_t entryCount;
    if (version == 0) {
        uint16_t tmp;
        if (!avifMetadataReadU16(&c, &tmp)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        entryCount = tmp;
    } else if (version == 1) {
        if (!avifMetadataReadU32(&c, &entryCount)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
    } else {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }

    for (uint32_t i = 0; i < entryCount; ++i) {
        avifMetadataCursor entryCursor = c;
        avifMetadataBox child;
        if (!avifMetadataReadBox(&entryCursor, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "infe", 4)) {
            AVIF_CHECKRES(avifMetadataParseInfe(meta, &child, fileData));
        }
        c.pos = child.end;
    }
    return AVIF_RESULT_OK;
}

// Marks the items referenced by a 'cdsc' iref as describing the image item of this meta box.
// Inside a track meta box, metadata items are implicitly associated with the track (there is
// no 'cdsc' reference to the track itself), so every metadata item is a target.
static avifResult avifMetadataMarkCdsc(avifMetadataMetaBox * meta, const avifMetadataBox * box, const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    const uint8_t * versionAndFlags;
    if (!avifMetadataCursorRead(&c, 4, &versionAndFlags)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    const uint8_t version = versionAndFlags[0];
    if (version > 1) {
        return AVIF_RESULT_OK;
    }

    while (avifMetadataCursorHas(&c, 8)) {
        const size_t refStart = c.pos;
        uint32_t refSize;
        if (!avifMetadataReadU32(&c, &refSize)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        const uint8_t * refType;
        if (!avifMetadataCursorRead(&c, 4, &refType)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (refSize < 8 || refSize > meta->end - refStart) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        const size_t refEnd = refStart + refSize;

        avifMetadataCursor refCursor = { fileData, refEnd, c.pos };
        uint32_t fromID;
        if (version == 0) {
            uint16_t tmp;
            if (!avifMetadataReadU16(&refCursor, &tmp)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            fromID = tmp;
        } else {
            if (!avifMetadataReadU32(&refCursor, &fromID)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
        }
        uint16_t referenceCount;
        if (!avifMetadataReadU16(&refCursor, &referenceCount)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        const avifBool isCdsc = !memcmp(refType, "cdsc", 4);
        for (uint16_t refIndex = 0; refIndex < referenceCount; ++refIndex) {
            uint32_t toID;
            if (version == 0) {
                uint16_t tmp;
                if (!avifMetadataReadU16(&refCursor, &tmp)) {
                    return AVIF_RESULT_BMFF_PARSE_FAILED;
                }
                toID = tmp;
            } else {
                if (!avifMetadataReadU32(&refCursor, &toID)) {
                    return AVIF_RESULT_BMFF_PARSE_FAILED;
                }
            }
            if (isCdsc && meta->pitm != 0 && toID == meta->pitm) {
                avifMetadataItem * item = avifMetadataFindItem(meta, fromID, AVIF_TRUE);
                if (item == NULL) {
                    return AVIF_RESULT_OUT_OF_MEMORY;
                }
                item->describesImage = AVIF_TRUE;
            }
        }
        c.pos = refEnd;
    }
    return AVIF_RESULT_OK;
}

// Locates the irot/imir transformative properties associated with the image item.
static avifResult avifMetadataParseIprp(avifMetadataMetaBox * meta,
                                        const avifMetadataBox * box,
                                        const uint8_t * fileData,
                                        avifMetadataOrientation * orientation)
{
    // Record the file offset of each property payload in the ipco box.
    typedef struct
    {
        uint8_t type[4];
        size_t payloadPos;
        size_t payloadSize;
    } avifMetadataRawProp;
    avifMetadataRawProp rawProps[256];
    uint32_t rawPropCount = 0;

    avifMetadataCursor c = { fileData, box->end, box->start + box->headerSize };
    avifBool ipcoSeen = AVIF_FALSE;
    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox child;
        if (!avifMetadataReadBox(&c, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "ipco", 4)) {
            ipcoSeen = AVIF_TRUE;
            avifMetadataCursor ipcoCursor = { fileData, child.end, child.start + child.headerSize };
            while (avifMetadataCursorHas(&ipcoCursor, 8)) {
                avifMetadataBox prop;
                if (!avifMetadataReadBox(&ipcoCursor, &prop)) {
                    return AVIF_RESULT_BMFF_PARSE_FAILED;
                }
                if (rawPropCount < 256) {
                    memcpy(rawProps[rawPropCount].type, prop.type, 4);
                    rawProps[rawPropCount].payloadPos = prop.start + prop.headerSize;
                    rawProps[rawPropCount].payloadSize = prop.payloadSize;
                    ++rawPropCount;
                }
            }
        }
        c.pos = child.end;
    }
    if (!ipcoSeen || meta->pitm == 0) {
        return AVIF_RESULT_OK;
    }

    // Scan ipma boxes for the associations of the image item.
    c.pos = box->start + box->headerSize;
    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox child;
        if (!avifMetadataReadBox(&c, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "ipma", 4)) {
            avifMetadataCursor ipmaCursor = { fileData, child.end, child.start + child.headerSize };
            const uint8_t * versionAndFlags;
            if (!avifMetadataCursorRead(&ipmaCursor, 4, &versionAndFlags)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            const uint8_t version = versionAndFlags[0];
            const avifBool propertyIndexIsU15 = (versionAndFlags[3] & 0x1) != 0;
            uint32_t entryCount;
            if (!avifMetadataReadU32(&ipmaCursor, &entryCount)) {
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            for (uint32_t e = 0; e < entryCount; ++e) {
                uint32_t itemID;
                if (version < 1) {
                    uint16_t tmp;
                    if (!avifMetadataReadU16(&ipmaCursor, &tmp)) {
                        return AVIF_RESULT_BMFF_PARSE_FAILED;
                    }
                    itemID = tmp;
                } else {
                    if (!avifMetadataReadU32(&ipmaCursor, &itemID)) {
                        return AVIF_RESULT_BMFF_PARSE_FAILED;
                    }
                }
                const uint8_t * associationCountByte;
                if (!avifMetadataCursorRead(&ipmaCursor, 1, &associationCountByte)) {
                    return AVIF_RESULT_BMFF_PARSE_FAILED;
                }
                const uint8_t associationCount = associationCountByte[0];
                for (uint8_t a = 0; a < associationCount; ++a) {
                    const size_t assocPos = ipmaCursor.pos;
                    if (propertyIndexIsU15) {
                        uint16_t tmp;
                        if (!avifMetadataReadU16(&ipmaCursor, &tmp)) {
                            return AVIF_RESULT_BMFF_PARSE_FAILED;
                        }
                    } else {
                        if (!avifMetadataCursorRead(&ipmaCursor, 1, NULL)) {
                            return AVIF_RESULT_BMFF_PARSE_FAILED;
                        }
                    }
                    if (itemID != meta->pitm) {
                        continue;
                    }
                    const uint8_t firstByte = fileData[assocPos];
                    uint32_t propertyIndex;
                    if (propertyIndexIsU15) {
                        propertyIndex = (((uint32_t)firstByte & 0x7F) << 8) | fileData[assocPos + 1];
                    } else {
                        propertyIndex = firstByte & 0x7F;
                    }
                    if (propertyIndex == 0 || propertyIndex > rawPropCount) {
                        continue;
                    }
                    const avifMetadataRawProp * prop = &rawProps[propertyIndex - 1];
                    if (!memcmp(prop->type, "irot", 4) && prop->payloadSize == 1) {
                        orientation->hasIrot = AVIF_TRUE;
                        orientation->irotAngleBytePos = prop->payloadPos;
                    } else if (!memcmp(prop->type, "imir", 4) && prop->payloadSize == 1) {
                        orientation->hasImir = AVIF_TRUE;
                        orientation->imirAxisBytePos = prop->payloadPos;
                    }
                }
            }
        }
        c.pos = child.end;
    }
    return AVIF_RESULT_OK;
}

// Parses one meta box. meta must already be added to file->metaBoxes.
static avifResult avifMetadataParseMetaBox(avifMetadataFile * file,
                                           avifMetadataMetaBox * meta,
                                           const avifMetadataBox * metaBox,
                                           const uint8_t * fileData)
{
    meta->start = metaBox->start;
    meta->end = metaBox->end;

    avifMetadataCursor c = { fileData, metaBox->end, metaBox->start + metaBox->headerSize };
    // Every meta box is a FullBox: 4 bytes of version/flags.
    if (!avifMetadataCursorSkip(&c, 4)) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }

    // iref and iprp need the primary item ID, which may be declared after them. Defer them.
    avifBool needIref = AVIF_FALSE;
    avifBool needIprp = AVIF_FALSE;
    avifMetadataBox irefBox;
    avifMetadataBox iprpBox;
    memset(&irefBox, 0, sizeof(irefBox));
    memset(&iprpBox, 0, sizeof(iprpBox));

    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox child;
        if (!avifMetadataReadBox(&c, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "pitm", 4)) {
            AVIF_CHECKRES(avifMetadataParsePitm(meta, &child, fileData));
        } else if (!memcmp(child.type, "iloc", 4)) {
            AVIF_CHECKRES(avifMetadataParseIloc(meta, &child, fileData));
        } else if (!memcmp(child.type, "iinf", 4)) {
            AVIF_CHECKRES(avifMetadataParseIinf(meta, &child, fileData));
        } else if (!memcmp(child.type, "iref", 4)) {
            irefBox = child;
            needIref = AVIF_TRUE;
        } else if (!memcmp(child.type, "iprp", 4)) {
            iprpBox = child;
            needIprp = AVIF_TRUE;
        }
    }
    if (needIref) {
        AVIF_CHECKRES(avifMetadataMarkCdsc(meta, &irefBox, fileData));
    }
    if (needIprp) {
        AVIF_CHECKRES(avifMetadataParseIprp(meta, &iprpBox, fileData, &file->orientation));
    }
    return AVIF_RESULT_OK;
}

// Parses a 'trak' box, looking for a nested 'meta' box.
static avifResult avifMetadataParseTrak(avifMetadataFile * file, const avifMetadataBox * trakBox, const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, trakBox->end, trakBox->start + trakBox->headerSize };
    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox child;
        if (!avifMetadataReadBox(&c, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "meta", 4)) {
            avifMetadataMetaBox * meta = (avifMetadataMetaBox *)avifArrayPush(&file->metaBoxes);
            AVIF_CHECKERR(meta != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memset(meta, 0, sizeof(*meta));
            if (!avifArrayCreate(&meta->items, sizeof(avifMetadataItem), 4)) {
                avifArrayPop(&file->metaBoxes);
                return AVIF_RESULT_OUT_OF_MEMORY;
            }
            meta->isRoot = AVIF_FALSE;
            AVIF_CHECKRES(avifMetadataParseMetaBox(file, meta, &child, fileData));
            // Track metadata items are implicitly associated with the track.
            for (uint32_t i = 0; i < meta->items.count; ++i) {
                avifMetadataItem * item = &meta->items.item[i];
                if (!memcmp(item->type, "Exif", 4) || !memcmp(item->type, "mime", 4)) {
                    item->describesImage = AVIF_TRUE;
                }
            }
        }
        c.pos = child.end;
    }
    return AVIF_RESULT_OK;
}

// Recursively descends through container boxes to find meta and trak boxes.
static avifResult avifMetadataParseContainers(avifMetadataFile * file,
                                              const avifMetadataBox * container,
                                              size_t contentStart,
                                              const uint8_t * fileData)
{
    avifMetadataCursor c = { fileData, container->end, contentStart };
    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox child;
        if (!avifMetadataReadBox(&c, &child)) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(child.type, "meta", 4)) {
            avifMetadataMetaBox * meta = (avifMetadataMetaBox *)avifArrayPush(&file->metaBoxes);
            AVIF_CHECKERR(meta != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memset(meta, 0, sizeof(*meta));
            if (!avifArrayCreate(&meta->items, sizeof(avifMetadataItem), 8)) {
                avifArrayPop(&file->metaBoxes);
                return AVIF_RESULT_OUT_OF_MEMORY;
            }
            meta->isRoot = AVIF_FALSE;
            AVIF_CHECKRES(avifMetadataParseMetaBox(file, meta, &child, fileData));
        } else if (!memcmp(child.type, "trak", 4)) {
            AVIF_CHECKRES(avifMetadataParseTrak(file, &child, fileData));
        } else if (!memcmp(child.type, "moov", 4) || !memcmp(child.type, "mdia", 4) ||
                   !memcmp(child.type, "minf", 4) || !memcmp(child.type, "udta", 4)) {
            AVIF_CHECKRES(avifMetadataParseContainers(file, &child, child.start + child.headerSize, fileData));
        }
        c.pos = child.end;
    }
    return AVIF_RESULT_OK;
}

static avifResult avifMetadataParseFile(avifMetadataFile * file, const uint8_t * data, size_t size)
{
    memset(file, 0, sizeof(*file));
    file->fileEndSize = size;
    if (!avifArrayCreate(&file->metaBoxes, sizeof(avifMetadataMetaBox), 2)) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }

    avifMetadataCursor c = { data, size, 0 };
    avifBool ftypSeen = AVIF_FALSE;
    avifBool rootMetaSeen = AVIF_FALSE;
    while (avifMetadataCursorHas(&c, 8)) {
        avifMetadataBox box;
        if (!avifMetadataReadBox(&c, &box)) {
            avifMetadataFileDestroy(file);
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (!memcmp(box.type, "ftyp", 4)) {
            ftypSeen = AVIF_TRUE;
        } else if (!memcmp(box.type, "meta", 4)) {
            if (rootMetaSeen) {
                avifMetadataFileDestroy(file);
                return AVIF_RESULT_BMFF_PARSE_FAILED;
            }
            avifMetadataMetaBox * meta = (avifMetadataMetaBox *)avifArrayPush(&file->metaBoxes);
            if (meta == NULL) {
                avifMetadataFileDestroy(file);
                return AVIF_RESULT_OUT_OF_MEMORY;
            }
            memset(meta, 0, sizeof(*meta));
            if (!avifArrayCreate(&meta->items, sizeof(avifMetadataItem), 8)) {
                avifArrayPop(&file->metaBoxes);
                avifMetadataFileDestroy(file);
                return AVIF_RESULT_OUT_OF_MEMORY;
            }
            meta->isRoot = AVIF_TRUE;
            file->rootMeta = meta;
            const avifResult result = avifMetadataParseMetaBox(file, meta, &box, data);
            if (result != AVIF_RESULT_OK) {
                avifMetadataFileDestroy(file);
                return result;
            }
            rootMetaSeen = AVIF_TRUE;
        } else if (!memcmp(box.type, "moov", 4)) {
            const avifResult result = avifMetadataParseContainers(file, &box, box.start + box.headerSize, data);
            if (result != AVIF_RESULT_OK) {
                avifMetadataFileDestroy(file);
                return result;
            }
        }
        file->lastTopLevelSizeZeroFieldPos = box.isSizeZero ? box.start : 0;
    }
    if (!ftypSeen || file->rootMeta == NULL || file->rootMeta->pitm == 0) {
        avifMetadataFileDestroy(file);
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Target item lookup.

static avifBool avifMetadataIsXmpItem(const avifMetadataItem * item)
{
    return !memcmp(item->type, "mime", 4) && item->hasContentType && !strcmp(item->contentType, AVIF_CONTENT_TYPE_XMP);
}

// Collects every Exif (or XMP) item that describes the image, across all meta boxes. A file
// may carry the same payload in the root meta box and again inside the color track's meta
// box; both copies must be updated so that both decoder sources agree.
typedef struct avifMetadataTargetList
{
    avifMetadataItem * items[16];
    uint32_t count;
} avifMetadataTargetList;

static avifResult avifMetadataFindTargets(avifMetadataFile * file, avifBool wantExif, avifMetadataTargetList * targets)
{
    targets->count = 0;
    for (uint32_t m = 0; m < file->metaBoxes.count; ++m) {
        avifMetadataMetaBox * meta = &file->metaBoxes.meta[m];
        for (uint32_t i = 0; i < meta->items.count; ++i) {
            avifMetadataItem * item = &meta->items.item[i];
            const avifBool matches = wantExif ? !memcmp(item->type, "Exif", 4) : avifMetadataIsXmpItem(item);
            if (!matches || !item->describesImage) {
                continue;
            }
            if (targets->count == sizeof(targets->items) / sizeof(targets->items[0])) {
                return AVIF_RESULT_NOT_IMPLEMENTED;
            }
            targets->items[targets->count++] = item;
        }
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Payload helpers.

static avifResult avifMetadataReadPayload(const avifMetadataFile * file,
                                          const uint8_t * data,
                                          const avifMetadataItem * item,
                                          avifRWData * out)
{
    size_t total = 0;
    for (uint32_t i = 0; i < item->extents.count; ++i) {
        const avifMetadataExtent * extent = &item->extents.extent[i];
        if (extent->length > SIZE_MAX - total) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        total += (size_t)extent->length;
    }
    if (total == 0) {
        return AVIF_RESULT_BMFF_PARSE_FAILED;
    }
    AVIF_CHECKRES(avifRWDataRealloc(out, total));
    size_t writeOffset = 0;
    for (uint32_t i = 0; i < item->extents.count; ++i) {
        const avifMetadataExtent * extent = &item->extents.extent[i];
        if (extent->absoluteOffset > file->fileEndSize || extent->length > file->fileEndSize - extent->absoluteOffset) {
            return AVIF_RESULT_BMFF_PARSE_FAILED;
        }
        if (extent->length > 0) {
            memcpy(out->data + writeOffset, data + (size_t)extent->absoluteOffset, (size_t)extent->length);
        }
        writeOffset += (size_t)extent->length;
    }
    return AVIF_RESULT_OK;
}

// Reads the Exif Orientation tag (1-8) from an Exif item payload (which begins with the 4-byte
// exif_tiff_header_offset field), or 0 if there is none.
static uint8_t avifMetadataExifPayloadOrientation(const avifRWData * payload)
{
    if (payload == NULL || payload->size <= 4) {
        return 0;
    }
    const avifROData exif = { payload->data + 4, payload->size - 4 };
    size_t offset = 0;
    if (avifGetExifOrientationOffset(exif.data, exif.size, &offset) != AVIF_RESULT_OK || offset >= exif.size) {
        return 0;
    }
    const uint8_t orientation = exif.data[offset];
    return (orientation >= 1 && orientation <= 8) ? orientation : 0;
}

// Builds the on-disk Exif item payload: the 4-byte exif_tiff_header_offset field followed by
// the raw Exif bytes. The TIFF header offset is recomputed for the new payload because the
// TIFF header is not necessarily at the start of the Exif bytes.
static avifResult avifMetadataBuildExifItemPayload(const avifROData * exif, avifRWData * out)
{
    size_t tiffHeaderOffset = 0;
    AVIF_CHECKRES(avifGetExifTiffHeaderOffset(exif->data, exif->size, &tiffHeaderOffset));
    if (tiffHeaderOffset > UINT32_MAX) {
        return AVIF_RESULT_INVALID_EXIF_PAYLOAD;
    }
    if (exif->size > SIZE_MAX - 4) {
        return AVIF_RESULT_OUT_OF_MEMORY;
    }
    AVIF_CHECKRES(avifRWDataRealloc(out, 4 + exif->size));
    const uint32_t offset32bit = avifHTONL((uint32_t)tiffHeaderOffset);
    memcpy(out->data, &offset32bit, 4);
    if (exif->size > 0) {
        memcpy(out->data + 4, exif->data, exif->size);
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// In-place patching.

static avifResult avifMetadataWriteUX(uint8_t * data, size_t pos, uint8_t byteCount, uint64_t v)
{
    if (byteCount > 8) {
        return AVIF_RESULT_INTERNAL_ERROR;
    }
    // Refuse to truncate a value that does not fit in the on-disk field width.
    if (byteCount < 8 && (v >> (8 * byteCount)) != 0) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    for (uint8_t i = 0; i < byteCount; ++i) {
        data[pos + i] = (uint8_t)((v >> (8 * (byteCount - 1 - i))) & 0xFF);
    }
    return AVIF_RESULT_OK;
}

// Rewrites the single iloc extent of item so that it points at absoluteOffset/length.
static avifResult avifMetadataPatchExtent(uint8_t * outData, const avifMetadataItem * item, uint64_t absoluteOffset, uint64_t length)
{
    if (item->idatStored || item->extents.count != 1) {
        // idat-stored metadata is addressed relative to the meta box, and multiple extents
        // would need a different layout. Refuse rather than corrupt the file.
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    const avifMetadataExtent * extent = &item->extents.extent[0];
    if (extent->offsetSize == 0 || extent->lengthSize == 0) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    if (absoluteOffset < item->baseOffset) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    AVIF_CHECKRES(avifMetadataWriteUX(outData, extent->offsetFieldPos, extent->offsetSize, absoluteOffset - item->baseOffset));
    AVIF_CHECKRES(avifMetadataWriteUX(outData, extent->lengthFieldPos, extent->lengthSize, length));
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Orientation.

typedef struct avifMetadataOrientationTarget
{
    avifBool needsIrot;
    uint8_t angle;
    avifBool needsImir;
    uint8_t axis;
} avifMetadataOrientationTarget;

// Same mapping as avifImageExtractExifOrientationToIrotImir() in src/exif.c.
static avifMetadataOrientationTarget avifMetadataOrientationToTransform(uint8_t orientation)
{
    avifMetadataOrientationTarget t;
    memset(&t, 0, sizeof(t));
    switch (orientation) {
        case 2:
            t.needsImir = AVIF_TRUE;
            t.axis = 1;
            break;
        case 3:
            t.needsIrot = AVIF_TRUE;
            t.angle = 2;
            break;
        case 4:
            t.needsImir = AVIF_TRUE;
            t.axis = 0;
            break;
        case 5:
            t.needsIrot = AVIF_TRUE;
            t.angle = 1;
            t.needsImir = AVIF_TRUE;
            t.axis = 0;
            break;
        case 6:
            t.needsIrot = AVIF_TRUE;
            t.angle = 3;
            break;
        case 7:
            t.needsIrot = AVIF_TRUE;
            t.angle = 3;
            t.needsImir = AVIF_TRUE;
            t.axis = 0;
            break;
        case 8:
            t.needsIrot = AVIF_TRUE;
            t.angle = 1;
            break;
        default:
            // Orientation 1 (or unknown) needs no transform property.
            break;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Public API.

avifResult avifMetadataRewrite(const avifROData * input,
                               const avifROData * replacementExif,
                               const avifROData * replacementXMP,
                               avifMetadataRewriteOrientation orientationMode,
                               avifMetadataRewriteResult * outResult,
                               avifRWData * output)
{
    if (input == NULL || input->data == NULL || input->size == 0 || output == NULL) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (output->data != NULL) {
        // The caller must pass a zero-initialized avifRWData.
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (replacementExif == NULL && replacementXMP == NULL) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (replacementExif != NULL && (replacementExif->data == NULL || replacementExif->size == 0)) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (replacementXMP != NULL && (replacementXMP->data == NULL || replacementXMP->size == 0)) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    if (orientationMode != AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE &&
        orientationMode != AVIF_METADATA_REWRITE_ORIENTATION_FROM_EXIF) {
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    if (outResult != NULL) {
        memset(outResult, 0, sizeof(*outResult));
    }

    avifMetadataFile file;
    AVIF_CHECKRES(avifMetadataParseFile(&file, input->data, input->size));

    avifResult result = AVIF_RESULT_OK;
    avifMetadataTargetList exifTargets;
    avifMetadataTargetList xmpTargets;
    exifTargets.count = 0;
    xmpTargets.count = 0;
    avifRWData exifPayload = AVIF_DATA_EMPTY;
    avifRWData previousExifPayload = AVIF_DATA_EMPTY;
    uint8_t previousOrientation = 0;
    uint8_t newOrientation = 0;

    if (replacementExif != NULL) {
        result = avifMetadataFindTargets(&file, AVIF_TRUE, &exifTargets);
        if (result != AVIF_RESULT_OK) {
            goto cleanup;
        }
        if (exifTargets.count == 0) {
            // This implementation replaces existing Exif items; adding one would require growing
            // the meta box and rebasing every offset in the file.
            result = AVIF_RESULT_INVALID_ARGUMENT;
            goto cleanup;
        }
        result = avifMetadataReadPayload(&file, input->data, exifTargets.items[0], &previousExifPayload);
        if (result != AVIF_RESULT_OK) {
            goto cleanup;
        }
        previousOrientation = avifMetadataExifPayloadOrientation(&previousExifPayload);
        result = avifMetadataBuildExifItemPayload(replacementExif, &exifPayload);
        if (result != AVIF_RESULT_OK) {
            goto cleanup;
        }
        newOrientation = avifMetadataExifPayloadOrientation(&exifPayload);
    }

    if (replacementXMP != NULL) {
        result = avifMetadataFindTargets(&file, AVIF_FALSE, &xmpTargets);
        if (result != AVIF_RESULT_OK) {
            goto cleanup;
        }
        if (xmpTargets.count == 0) {
            result = AVIF_RESULT_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    // Validate every target's iloc entry before allocating anything.
    for (uint32_t i = 0; i < exifTargets.count; ++i) {
        const avifMetadataItem * item = exifTargets.items[i];
        if (item->idatStored || item->extents.count != 1 || item->extents.extent[0].offsetSize == 0 ||
            item->extents.extent[0].lengthSize == 0) {
            result = AVIF_RESULT_NOT_IMPLEMENTED;
            goto cleanup;
        }
    }
    for (uint32_t i = 0; i < xmpTargets.count; ++i) {
        const avifMetadataItem * item = xmpTargets.items[i];
        if (item->idatStored || item->extents.count != 1 || item->extents.extent[0].offsetSize == 0 ||
            item->extents.extent[0].lengthSize == 0) {
            result = AVIF_RESULT_NOT_IMPLEMENTED;
            goto cleanup;
        }
    }

    // The metadata payload may be shared with another item by pointing its iloc extent at the
    // same bytes (libavif's encoder deduplicates identical chunks). Repointing one item's extent
    // would then corrupt the other item, so refuse in that case.
    {
        const avifMetadataItem * targets[32];
        uint32_t targetCount = 0;
        for (uint32_t i = 0; i < exifTargets.count; ++i) {
            targets[targetCount++] = exifTargets.items[i];
        }
        for (uint32_t i = 0; i < xmpTargets.count; ++i) {
            targets[targetCount++] = xmpTargets.items[i];
        }
        for (uint32_t m = 0; m < file.metaBoxes.count; ++m) {
            const avifMetadataMetaBox * meta = &file.metaBoxes.meta[m];
            for (uint32_t j = 0; j < meta->items.count; ++j) {
                const avifMetadataItem * item = &meta->items.item[j];
                avifBool isTarget = AVIF_FALSE;
                for (uint32_t t = 0; t < targetCount; ++t) {
                    if (targets[t] == item) {
                        isTarget = AVIF_TRUE;
                        break;
                    }
                }
                if (isTarget) {
                    continue;
                }
                for (uint32_t e = 0; e < item->extents.count; ++e) {
                    const avifMetadataExtent * other = &item->extents.extent[e];
                    if (other->length == 0) {
                        continue;
                    }
                    for (uint32_t t = 0; t < targetCount; ++t) {
                        for (uint32_t te = 0; te < targets[t]->extents.count; ++te) {
                            const avifMetadataExtent * a = &targets[t]->extents.extent[te];
                            if (a->length == 0) {
                                continue;
                            }
                            const uint64_t aStart = a->absoluteOffset;
                            const uint64_t aEnd = aStart + a->length;
                            const uint64_t bStart = other->absoluteOffset;
                            const uint64_t bEnd = bStart + other->length;
                            if (bStart < aEnd && aStart < bEnd) {
                                result = AVIF_RESULT_NOT_IMPLEMENTED;
                                goto cleanup;
                            }
                        }
                    }
                }
            }
        }
    }

    {
        const uint64_t exifSize = (replacementExif != NULL) ? (uint64_t)exifPayload.size : 0;
        const uint64_t xmpSize = (replacementXMP != NULL) ? (uint64_t)replacementXMP->size : 0;
        const uint64_t payloadTotal = exifSize + xmpSize;
        if (payloadTotal > SIZE_MAX) {
            result = AVIF_RESULT_OUT_OF_MEMORY;
            goto cleanup;
        }
        const uint64_t max32 = (uint64_t)UINT32_MAX;
        const uint64_t headerSize = (8 + payloadTotal > max32) ? 16 : 8;
        if (payloadTotal > SIZE_MAX - headerSize || input->size > SIZE_MAX - (size_t)(headerSize + payloadTotal)) {
            result = AVIF_RESULT_OUT_OF_MEMORY;
            goto cleanup;
        }
        const size_t mdatStart = input->size;
        const size_t payloadStart = mdatStart + (size_t)headerSize;
        const size_t totalSize = mdatStart + (size_t)(headerSize + payloadTotal);

        result = avifRWDataRealloc(output, totalSize);
        if (result != AVIF_RESULT_OK) {
            goto cleanup;
        }
        memcpy(output->data, input->data, input->size);

        // A final size-0 box extends to the end of the file. Appending bytes after it would
        // silently extend that box; rewrite its size field so the new mdat becomes a sibling.
        if (file.lastTopLevelSizeZeroFieldPos != 0) {
            const uint64_t grownSize = input->size - file.lastTopLevelSizeZeroFieldPos;
            if (grownSize > UINT32_MAX) {
                // Size 0 cannot express a size larger than 32 bits via the size field alone.
                result = AVIF_RESULT_NOT_IMPLEMENTED;
                goto cleanup;
            }
            const uint32_t grownSizeBE = avifHTONL((uint32_t)grownSize);
            memcpy(output->data + file.lastTopLevelSizeZeroFieldPos, &grownSizeBE, 4);
        }

        // Write the mdat header.
        if (headerSize == 8) {
            const uint32_t boxSize = avifHTONL((uint32_t)(8 + payloadTotal));
            memcpy(output->data + mdatStart, &boxSize, 4);
            memcpy(output->data + mdatStart + 4, "mdat", 4);
        } else {
            const uint32_t one = avifHTONL(1);
            memcpy(output->data + mdatStart, &one, 4);
            memcpy(output->data + mdatStart + 4, "mdat", 4);
            const uint64_t boxSize = avifHTON64(headerSize + payloadTotal);
            memcpy(output->data + mdatStart + 8, &boxSize, 8);
        }

        size_t writePos = payloadStart;
        if (replacementExif != NULL) {
            memcpy(output->data + writePos, exifPayload.data, exifPayload.size);
            for (uint32_t i = 0; i < exifTargets.count; ++i) {
                result = avifMetadataPatchExtent(output->data, exifTargets.items[i], (uint64_t)writePos, exifPayload.size);
                if (result != AVIF_RESULT_OK) {
                    goto cleanup;
                }
            }
            writePos += exifPayload.size;
        }
        if (replacementXMP != NULL) {
            memcpy(output->data + writePos, replacementXMP->data, replacementXMP->size);
            for (uint32_t i = 0; i < xmpTargets.count; ++i) {
                result = avifMetadataPatchExtent(output->data, xmpTargets.items[i], (uint64_t)writePos, replacementXMP->size);
                if (result != AVIF_RESULT_OK) {
                    goto cleanup;
                }
            }
            writePos += replacementXMP->size;
        }
        if (writePos != totalSize) {
            result = AVIF_RESULT_INTERNAL_ERROR;
            goto cleanup;
        }
    }

    // Optionally make the primary image item's irot/imir follow the new Exif orientation.
    avifBool orientationModified = AVIF_FALSE;
    if (orientationMode == AVIF_METADATA_REWRITE_ORIENTATION_FROM_EXIF && newOrientation != 0) {
        const avifMetadataOrientationTarget target = avifMetadataOrientationToTransform(newOrientation);
        const avifMetadataOrientation * orientation = &file.orientation;
        if (target.needsIrot != orientation->hasIrot || target.needsImir != orientation->hasImir) {
            // Adding or removing a property would resize ipco/ipma. Report rather than guess.
            result = AVIF_RESULT_NOT_IMPLEMENTED;
            goto cleanup;
        }
        if (target.needsIrot) {
            const uint8_t oldValue = (uint8_t)(output->data[orientation->irotAngleBytePos] & 0x03);
            const uint8_t newValue = (uint8_t)(target.angle & 0x03);
            if (oldValue != newValue) {
                orientationModified = AVIF_TRUE;
            }
            output->data[orientation->irotAngleBytePos] =
                (uint8_t)((output->data[orientation->irotAngleBytePos] & 0xFC) | newValue);
        }
        if (target.needsImir) {
            const uint8_t oldValue = (uint8_t)(output->data[orientation->imirAxisBytePos] & 0x01);
            const uint8_t newValue = (uint8_t)(target.axis & 0x01);
            if (oldValue != newValue) {
                orientationModified = AVIF_TRUE;
            }
            output->data[orientation->imirAxisBytePos] =
                (uint8_t)((output->data[orientation->imirAxisBytePos] & 0xFE) | newValue);
        }
    }

    if (outResult != NULL) {
        outResult->previousExifOrientation = previousOrientation;
        outResult->newExifOrientation = newOrientation;
        outResult->orientationPropertiesModified = orientationModified;
    }

cleanup:
    avifRWDataFree(&exifPayload);
    avifRWDataFree(&previousExifPayload);
    avifMetadataFileDestroy(&file);
    if (result != AVIF_RESULT_OK && output != NULL) {
        avifRWDataFree(output);
    }
    return result;
}
