#include "Bitmap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// Note: For cover images, dithering is done in JpegToBmpConverter.cpp
// This file handles BMP reading - use simple quantization to avoid double-dithering
constexpr bool USE_ATKINSON = true;  // Use Atkinson dithering instead of Floyd-Steinberg
// ============================================================================

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;

  delete atkinsonDitherer;
  delete fsDitherer;
}

uint16_t Bitmap::readLE16(FsFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t Bitmap::readLE32(FsFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const int c2 = f.read();
  const int c3 = f.read();

  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);

  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

const char* Bitmap::errorToString(BmpReaderError err) {
  switch (err) {
    case BmpReaderError::Ok:
      return "Ok";
    case BmpReaderError::FileInvalid:
      return "FileInvalid";
    case BmpReaderError::SeekStartFailed:
      return "SeekStartFailed";
    case BmpReaderError::NotBMP:
      return "NotBMP (missing 'BM')";
    case BmpReaderError::DIBTooSmall:
      return "DIBTooSmall (<40 bytes)";
    case BmpReaderError::BadPlanes:
      return "BadPlanes (!= 1)";
    case BmpReaderError::UnsupportedBpp:
      return "UnsupportedBpp (expected 1, 2, 8, 24, or 32)";
    case BmpReaderError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB or BI_BITFIELDS for 32bpp)";
    case BmpReaderError::BadDimensions:
      return "BadDimensions";
    case BmpReaderError::ImageTooLarge:
      return "ImageTooLarge (max 2048x3072)";
    case BmpReaderError::PaletteTooLarge:
      return "PaletteTooLarge";

    case BmpReaderError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpReaderError::BufferTooSmall:
      return "BufferTooSmall";

    case BmpReaderError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpReaderError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpReaderError Bitmap::parseHeaders() {
  if (!file) return BmpReaderError::FileInvalid;
  if (!file.seek(0)) return BmpReaderError::SeekStartFailed;

  // --- BMP FILE HEADER ---
  const uint16_t bfType = readLE16(file);
  if (bfType != 0x4D42) return BmpReaderError::NotBMP;

  file.seekCur(8);
  bfOffBits = readLE32(file);

  // --- DIB HEADER ---
  const uint32_t biSize = readLE32(file);
  if (biSize < 40) return BmpReaderError::DIBTooSmall;

  width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  bpp = readLE16(file);
  const uint32_t comp = readLE32(file);
  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 8 || bpp == 24 || bpp == 32;

  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!validBpp) return BmpReaderError::UnsupportedBpp;
  // Allow BI_RGB (0) for all, and BI_BITFIELDS (3) for 32bpp which is common for BGRA masks.
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return BmpReaderError::UnsupportedCompression;

  file.seekCur(12);  // biSizeImage, biXPelsPerMeter, biYPelsPerMeter
  const uint32_t colorsUsed = readLE32(file);
  if (colorsUsed > 256u) return BmpReaderError::PaletteTooLarge;
  file.seekCur(4);  // biClrImportant

  if (width <= 0 || height <= 0) return BmpReaderError::BadDimensions;

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    return BmpReaderError::ImageTooLarge;
  }

  // Pre-calculate Row Bytes to avoid doing this every row
  rowBytes = (width * bpp + 31) / 32 * 4;

  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    for (uint32_t i = 0; i < colorsUsed; i++) {
      uint8_t rgb[4];
      file.read(rgb, 4);  // Read B, G, R, Reserved in one go
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  outputWidth = width;
  outputHeight = height;
  sourceRowsRead = 0;
  outputRowsRead = 0;

  // Error diffusion only for high-color sources. 1-bit and 2-bit files are
  // already panel levels; dithering them again would soften a crisp image.
  if (bpp > 2 && dithering) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(width, imageLevels);
    } else {
      fsDitherer = new FloydSteinbergDitherer(width);
    }
  }

  return BmpReaderError::Ok;
}

bool Bitmap::setDitheredOutputSize(const int targetWidth, const int targetHeight) {
  if (!dithering || !atkinsonDitherer || targetWidth <= 0 || targetHeight <= 0 || targetWidth > width ||
      targetHeight > height || (targetWidth == width && targetHeight == height)) {
    return false;
  }

  auto* resized = new (std::nothrow) AtkinsonDitherer(targetWidth, imageLevels);
  if (!resized) return false;
  delete atkinsonDitherer;
  atkinsonDitherer = resized;
  outputWidth = targetWidth;
  outputHeight = targetHeight;
  return true;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readNextRow(uint8_t* data, uint8_t* rowBuffer) const {
  if (outputRowsRead >= outputHeight) return BmpReaderError::ShortReadRow;

  // Pick the source row before dithering, so a wallpaper fitted to the panel
  // is diffused on the screen grid instead of being nearest-neighbour scaled
  // after the pattern has already been decided.
  const int sourceY = std::min(height - 1, (outputRowsRead * height + height / 2) / outputHeight);
  while (sourceRowsRead <= sourceY) {
    if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;
    sourceRowsRead++;
  }

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  const int outputY = outputRowsRead;

  auto packPixel = [&](const uint8_t lum, const int outputX) {
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(adjustPixel(lum), outputX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(adjustPixel(lum), outputX);
    } else if (bpp > 2) {
      color = quantize(adjustPixel(lum), outputX, outputY);
    } else {
      color = static_cast<uint8_t>(lum >> 6);
    }
    currentOutByte |= (color << bitShift);
    if (bitShift == 0) {
      *outPtr++ = currentOutByte;
      currentOutByte = 0;
      bitShift = 6;
    } else {
      bitShift -= 2;
    }
  };

  for (int outputX = 0; outputX < outputWidth; outputX++) {
    const int sourceX = std::min(width - 1, (outputX * width + width / 2) / outputWidth);
    uint8_t lum;
    switch (bpp) {
      case 32: {
        const uint8_t* p = rowBuffer + sourceX * 4;
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        break;
      }
      case 24: {
        const uint8_t* p = rowBuffer + sourceX * 3;
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        break;
      }
      case 8:
        lum = paletteLum[rowBuffer[sourceX]];
        break;
      case 2:
        lum = paletteLum[(rowBuffer[sourceX >> 2] >> (6 - ((sourceX & 3) * 2))) & 0x03];
        break;
      case 1: {
        const uint8_t palIndex = (rowBuffer[sourceX >> 3] & (0x80 >> (sourceX & 7))) ? 1 : 0;
        lum = paletteLum[palIndex];
        break;
      }
      default:
        return BmpReaderError::UnsupportedBpp;
    }
    packPixel(lum, outputX);
  }

  if (atkinsonDitherer)
    atkinsonDitherer->nextRow();
  else if (fsDitherer)
    fsDitherer->nextRow();

  if (bitShift != 6) *outPtr = currentOutByte;
  outputRowsRead++;
  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  if (fsDitherer) fsDitherer->reset();
  if (atkinsonDitherer) atkinsonDitherer->reset();
  sourceRowsRead = 0;
  outputRowsRead = 0;
  return BmpReaderError::Ok;
}
