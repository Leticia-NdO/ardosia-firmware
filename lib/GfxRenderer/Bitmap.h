#pragma once

#include <SdFat.h>

#include <cstdint>

#include "BitmapHelpers.h"

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,

  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  // imageLevels: quantize to even 0/85/170/255 (4-level panel) instead of the
  // X4-tuned thresholds. Sleep photos use this together with the grayscale LUT.
  explicit Bitmap(FsFile& file, bool dithering = false, bool imageLevels = false)
      : file(file), dithering(dithering), imageLevels(imageLevels) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readNextRow(uint8_t* data, uint8_t* rowBuffer) const;
  BmpReaderError rewindToData() const;
  // Downsample before error diffusion. The renderer must not rescale an
  // already-dithered image or the pattern aliases into seams.
  bool setDitheredOutputSize(int targetWidth, int targetHeight);
  int getWidth() const { return outputWidth; }
  int getHeight() const { return outputHeight; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  bool is1Bit() const { return bpp == 1; }
  uint16_t getBpp() const { return bpp; }

 private:
  static uint16_t readLE16(FsFile& f);
  static uint32_t readLE32(FsFile& f);

  FsFile& file;
  bool dithering = false;
  bool imageLevels = false;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  int rowBytes = 0;
  int outputWidth = 0;
  int outputHeight = 0;
  uint8_t paletteLum[256] = {};

  mutable int16_t* errorCurRow = nullptr;
  mutable int16_t* errorNextRow = nullptr;
  mutable int sourceRowsRead = 0;
  mutable int outputRowsRead = 0;

  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;
};
