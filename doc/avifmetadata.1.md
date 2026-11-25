% AVIFMETADATA(1) | General Commands Manual
%
% 2026-11-26

<!--
This man page is written in pandoc's Markdown.
See: https://pandoc.org/MANUAL.html#pandocs-markdown
-->

# NAME

avifmetadata - replace the Exif or XMP metadata of an AVIF file without re-encoding

# SYNOPSIS

**avifmetadata** [_options_] _input.avif_ _output.avif_

**avifmetadata** [_options_] _input.avif_ **\--output** _output.avif_

# DESCRIPTION

**avifmetadata** replaces the Exif and/or XMP payloads attached to the primary item of an
existing AVIF file.

The coded AV1 samples are not decoded, re-encoded or moved. The input file is copied
unchanged and the replacement payloads are appended in a new `mdat` box at the end of the
file; only the item location box entries that point at the metadata items are patched. The
compressed picture, the alpha item, the gain map item, grid tiles and any track data are
therefore preserved bit for bit, and the new metadata can be read back with any AVIF decoder.

If the file is an image sequence, the metadata is also present inside the color track's own
`meta` box; both copies are updated so that decoders reading either source observe the new
values.

# OPTIONS

**-h**, **\--help**
:   Show syntax help.

**-V**, **\--version**
:   Show the version number.

**-o**, **\--output** _FILENAME_
:   Output file. May be omitted if the output file is given as the last positional argument.

**\--exif** _FILENAME_
:   Raw Exif payload to store. The 4-byte `exif_tiff_header_offset` field is computed
    automatically from the payload. If omitted, the existing Exif item is left untouched.

**\--xmp** _FILENAME_
:   XMP payload to store. If omitted, the existing XMP item is left untouched.

**\--orientation** _MODE_
:   Controls the primary item's `irot`/`imir` properties when the Exif payload is replaced.
    `preserve` (the default) leaves them unchanged, so the displayed orientation is exactly
    as before. `from-exif` overwrites their existing values to match the Orientation tag of
    the new Exif payload; it fails if that would require adding or removing a property.

At least one of **\--exif** or **\--xmp** must be given. The input file must already contain
the corresponding metadata item; this tool replaces payloads, it does not add items.
