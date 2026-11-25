// Copyright 2026 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

// avifmetadata: replace the Exif and/or XMP payloads of the primary item of an existing
// AVIF file without decoding or re-encoding the coded AV1 samples. This is the archive
// workflow: correct the shooting information or the description text while leaving the
// compressed picture bytes exactly as they were.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avif/avif.h"
#include "avifutil.h"

static void syntax(void)
{
    printf("Syntax: avifmetadata [options] input.avif -o output.avif\n");
    printf("        avifmetadata [options] input.avif output.avif\n");
    printf("Replaces the Exif and/or XMP metadata of the primary image item of an AVIF file\n");
    printf("without re-encoding the coded picture. The output file is byte-for-byte identical\n");
    printf("to the input except for the replaced metadata items.\n");
    printf("\n");
    printf("Options:\n");
    printf("    -h,--help                 : Show syntax help (this page)\n");
    printf("    -V,--version              : Show the version number\n");
    printf("    -o,--output FILENAME      : Output file (alternative to the last positional argument)\n");
    printf("    --exif FILENAME           : New raw Exif payload to store. Omit to leave Exif unchanged.\n");
    printf("    --xmp FILENAME            : New XMP payload to store. Omit to leave XMP unchanged.\n");
    printf("    --orientation MODE        : 'preserve' (default) keeps the primary item's irot/imir\n");
    printf("                                untouched; 'from-exif' overwrites the existing irot/imir\n");
    printf("                                values to match the new Exif Orientation tag.\n");
    printf("\n");
    printf("At least one of --exif or --xmp must be given. The input must already contain the\n");
    printf("corresponding metadata item; this tool replaces payloads, it does not add items.\n");
}

int main(int argc, const char * argv[])
{
    const char * outputFilename = NULL;
    const char * inputFilename = NULL;
    const char * positionals[2] = { NULL, NULL };
    int positionalCount = 0;
    const char * exifFilename = NULL;
    const char * xmpFilename = NULL;
    avifMetadataRewriteOrientation orientationMode = AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE;

    for (int i = 1; i < argc; ++i) {
        const char * arg = argv[i];
        if (!strcmp(arg, "--")) {
            for (++i; i < argc; ++i) {
                if (positionalCount == 2) {
                    fprintf(stderr, "ERROR: too many positional arguments\n");
                    return 1;
                }
                positionals[positionalCount++] = argv[i];
            }
            break;
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            syntax();
            return 0;
        } else if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            printf("avifmetadata %s\n", avifVersion());
            return 0;
        } else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
            if (++i == argc) {
                fprintf(stderr, "ERROR: missing value for %s\n", arg);
                return 1;
            }
            outputFilename = argv[i];
        } else if (!strcmp(arg, "--exif")) {
            if (++i == argc) {
                fprintf(stderr, "ERROR: missing value for %s\n", arg);
                return 1;
            }
            exifFilename = argv[i];
        } else if (!strcmp(arg, "--xmp")) {
            if (++i == argc) {
                fprintf(stderr, "ERROR: missing value for %s\n", arg);
                return 1;
            }
            xmpFilename = argv[i];
        } else if (!strcmp(arg, "--orientation")) {
            if (++i == argc) {
                fprintf(stderr, "ERROR: missing value for %s\n", arg);
                return 1;
            }
            if (!strcmp(argv[i], "preserve")) {
                orientationMode = AVIF_METADATA_REWRITE_ORIENTATION_PRESERVE;
            } else if (!strcmp(argv[i], "from-exif")) {
                orientationMode = AVIF_METADATA_REWRITE_ORIENTATION_FROM_EXIF;
            } else {
                fprintf(stderr, "ERROR: unknown --orientation value '%s' (expected 'preserve' or 'from-exif')\n", argv[i]);
                return 1;
            }
        } else if (arg[0] == '-') {
            fprintf(stderr, "ERROR: unknown option '%s'\n", arg);
            return 1;
        } else {
            if (positionalCount == 2) {
                fprintf(stderr, "ERROR: too many positional arguments\n");
                return 1;
            }
            positionals[positionalCount++] = arg;
        }
    }

    // Resolve positional arguments: the input is first, the output is second unless -o was used.
    if (positionalCount >= 1) {
        inputFilename = positionals[0];
    }
    if (positionalCount == 2) {
        if (outputFilename != NULL) {
            fprintf(stderr, "ERROR: both --output and a positional output file were given\n");
            return 1;
        }
        outputFilename = positionals[1];
    }
    if (inputFilename == NULL || outputFilename == NULL) {
        syntax();
        return 1;
    }
    if (exifFilename == NULL && xmpFilename == NULL) {
        fprintf(stderr, "ERROR: at least one of --exif or --xmp must be given\n");
        return 1;
    }

    avifRWData input = AVIF_DATA_EMPTY;
    avifRWData exif = AVIF_DATA_EMPTY;
    avifRWData xmp = AVIF_DATA_EMPTY;
    avifRWData output = AVIF_DATA_EMPTY;
    int returnCode = 1;

    if (!avifReadEntireFile(inputFilename, &input)) {
        fprintf(stderr, "ERROR: failed to read %s\n", inputFilename);
        goto cleanup;
    }
    if (exifFilename != NULL && !avifReadEntireFile(exifFilename, &exif)) {
        fprintf(stderr, "ERROR: failed to read %s\n", exifFilename);
        goto cleanup;
    }
    if (xmpFilename != NULL && !avifReadEntireFile(xmpFilename, &xmp)) {
        fprintf(stderr, "ERROR: failed to read %s\n", xmpFilename);
        goto cleanup;
    }

    const avifROData inputRO = { input.data, input.size };
    const avifROData exifRO = { exif.data, exif.size };
    const avifROData xmpRO = { xmp.data, xmp.size };
    avifMetadataRewriteResult result;
    const avifResult rewriteResult = avifMetadataRewrite(&inputRO,
                                                         (exifFilename != NULL) ? &exifRO : NULL,
                                                         (xmpFilename != NULL) ? &xmpRO : NULL,
                                                         orientationMode,
                                                         &result,
                                                         &output);
    if (rewriteResult != AVIF_RESULT_OK) {
        fprintf(stderr, "ERROR: avifMetadataRewrite() failed: %s\n", avifResultToString(rewriteResult));
        goto cleanup;
    }

    FILE * f = fopen(outputFilename, "wb");
    if (f == NULL) {
        fprintf(stderr, "ERROR: failed to open %s for writing\n", outputFilename);
        goto cleanup;
    }
    if (fwrite(output.data, 1, output.size, f) != output.size) {
        fprintf(stderr, "ERROR: failed to write %s\n", outputFilename);
        fclose(f);
        goto cleanup;
    }
    fclose(f);

    printf("Wrote AVIF: %s\n", outputFilename);
    printf(" * Primary item Exif orientation: %u -> %u\n", result.previousExifOrientation, result.newExifOrientation);
    printf(" * irot/imir properties %s\n", result.orientationPropertiesModified ? "updated" : "left unchanged");
    returnCode = 0;

cleanup:
    avifRWDataFree(&output);
    avifRWDataFree(&xmp);
    avifRWDataFree(&exif);
    avifRWDataFree(&input);
    return returnCode;
}
