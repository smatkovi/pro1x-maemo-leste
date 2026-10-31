/*
 * PVRTC1 decoder (2 and 4 bits per pixel, opaque and with alpha) to RGBA8.
 *
 * Written for this repository from the public description of the format:
 * S. Fenney, "Texture Compression using Low-Frequency Signal Modulation"
 * (Graphics Hardware 2003) and Imagination's "PVRTC Specification and User
 * Guide". No code from Imagination's SDK or other decoders.
 *
 * The format in short: the texture is cut into blocks of 4x4 (4bpp) or 8x4
 * (2bpp) pixels, each stored as 64 bits in Morton ("twiddled") order of the
 * block coordinates. A block holds two colours, A and B, and 32 bits of
 * modulation. Colours A and B of all blocks form two low resolution images
 * that are upscaled bilinearly, with the block colours at the block centres
 * and wrapping around the texture's edges; the modulation then blends
 * between the two upscaled images pixel by pixel.
 *
 *   colour word (second 32 bits of a block)
 *     bit 0       modulation mode
 *     bits 1-15   colour A: bit 15 set: RGB 5:5:4, else ARGB 3:4:4:3
 *     bits 16-31  colour B: bit 31 set: RGB 5:5:5, else ARGB 3:4:4:4
 *   modulation word (first 32 bits)
 *     4bpp: 2 bits per pixel, row by row. Mode 0: weights 0, 3/8, 5/8, 1.
 *           Mode 1: 0, 1/2, 1/2 with alpha 0 ("punch-through"), 1.
 *     2bpp, mode 0: 1 bit per pixel, weight 0 or 1.
 *     2bpp, mode 1: 2-bit weights (0, 3/8, 5/8, 1) for the pixels where x+y
 *           is even; the others are averaged from their neighbours: all
 *           four, or (when bit 0 is set) only the horizontal (bit 20 clear)
 *           or vertical (bit 20 set) ones. Bits 0 and 20 are then replaced
 *           by bits 1 and 21 in their stored values.
 *
 * Textures smaller than 2x2 blocks (mipmap tails) are stored padded to
 * 8x8 (4bpp) or 16x8 (2bpp) pixels.
 */
#ifndef PVRTC_DECODE_H
#define PVRTC_DECODE_H

#include <stdint.h>
#include <stddef.h>

/* Bytes of a w x h level, as glCompressedTexImage2D expects them. */
size_t pvrtc_size(int w, int h, int bpp);

/* Decode a w x h level (bpp 2 or 4) to w*h RGBA8 pixels, row by row.
 * opaque: the RGB formats, alpha forced to 255. Returns 0, or -1 if the
 * sizes are not powers of two. */
int pvrtc_decode(const uint8_t *src, int w, int h, int bpp, int opaque, uint8_t *dst);

#endif
