//
// ACF Extractor
//
// This code is a complete C++ rewrite not reusing any of the original Adeline code.
// 
// The program is based on original documentation and tools, but it was easier to start from
// a clean code base using only standard C++, instead of trying to port the original mix of
// Watcom C and x86 assembler.
//
// The intent is to present and document how the old Adeline technology work, for the sake of
// documentation and conservation. Reuse of any of this code and information for commercial purpose
// would require an authorization from the right holder, which at this point in time (July 2021) is
// Didier Chanfray, and gave me (Mickaël Pointier) the authorization to share this information.
//
// This code has been written for readability more than for performance and does not pretend to be
// perfect or not buggy in any way. And yes, if you run an analyzer it will tell you it's full of
// overflows and ugly casts, but it's how we did this type of code back in the days, and doing it
// "the right way" would look probably even worse (I know, I tried, and it was not worth the hassle)
//
// This version seem to be able to properly export most of the ACF files from the GOG version of Time
// Commando, with the exclusion of the first run of Rome, Japan and Middle Age levels which get corrupted.
//
// Change history:
// - 2021-10-20 [Mike] "ACF Extractor 1.0" aka 'blog post' article
//   Able to properly decompress almost all the videos, but still some issues on a couple of them
//

#include <cassert>
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <cstddef>
#include <filesystem> 
#include <iostream>
#include <format>


uint32_t g_DiagonalOffsets_1[64] =
{ 0, 1, 320, 640, 321, 2, 3, 322, 641, 960, 1280, 961, 642, 323, 4, 5, 324, 643, 962, 1281, 1600, 1920, 1601, 1282, 963, 644, 325, 6, 7,
  326, 645,  964, 1283, 1602, 1921, 2240, 2241, 1922, 1603, 1284, 965, 646, 327, 647, 966, 1285, 1604, 1923, 2242, 2243, 1924, 1605, 1286,
  967, 1287, 1606, 1925, 2244, 2245, 1926, 1607, 1927, 2246, 2247 };

uint32_t g_DiagonalOffsets_2[64] =
{ 7, 6, 327, 647, 326, 5, 4, 325, 646, 967, 1287, 966, 645, 324, 3, 2, 323, 644, 965, 1286, 1607, 1927, 1606, 1285, 964, 643, 322, 1, 0,
  321, 642, 963, 1284, 1605, 1926, 2247, 2246, 1925, 1604, 1283, 962, 641, 320, 640, 961, 1282, 1603, 1924, 2245, 2244, 1923, 1602, 1281,
  960, 1280, 1601, 1922, 2243, 2242, 1921, 1600, 1920, 2241, 2240 };

uint32_t g_SplitTileOffsets[4] = { 0, 4, 320*4, 320*4 + 4 };


uint16_t ReadU16(const uint8_t*& ptr)                 { uint16_t value = (*(uint16_t*)(ptr)); ptr += 2; return value; }
int16_t ReadS16(const uint8_t*& ptr)                  { int16_t value = (*(int16_t*)(ptr)); ptr += 2; return value; }

uint32_t ReadU32(const uint8_t*& ptr, int skip = 4)   { uint32_t value = (*(uint32_t*)(ptr)); ptr += skip; return value; }

int16_t ReadXYOffset(const uint8_t*& ptr, int stride) { int16_t value = (*(int8_t*)(ptr)) + (*(int8_t*)(ptr + 1)) * stride / 2; ptr += 2; return value; }


class Format
{
public:
  uint32_t     struct_size;
  uint32_t     width;
  uint32_t     height;
  uint32_t     frame_size;
  uint32_t     key_size;
  uint32_t     key_rate;
  uint32_t     play_rate;
  uint32_t     sampling_rate;
  uint32_t     sample_type;
  uint32_t     sample_flags;
  uint32_t     compressor;         ///< 0==ACF / 1==XCF
};




class FrameLen
{
public:
  const uint8_t* GetFrameSizeArray() const { return &frame_size_in_sectors;  }

public:
  uint32_t     biggest_frame_size;
  uint8_t      frame_size_in_sectors;  // Actual first entry, it's an array, but the size depends of the actual chunk size
};



class FrameData
{
public:
  const uint8_t* GetOpcodesArray() const                    { return opcodes; }
  const uint8_t* GetAlignedData(uint32_t height) const      { return ((uint8_t*)opcodes) + (height / 8) * 30; }
  const uint8_t* GetUnalignedData() const                   { return ((uint8_t*)this) + color_offset; }

public:
  uint32_t     color_offset;
  uint8_t      opcodes[30];       // Actually (height/8)*30 bytes, opcodes are stored as 6 bits per 8x8 bloc in the picture
};


class PaletteEntry
{
public:
  uint8_t   m_Red;
  uint8_t   m_Green;
  uint8_t   m_Blue;
};




class Palette
{
public:
  const uint8_t* GetBuffer() const { return &m_PaletteEntries[0].m_Red; }

public:
  PaletteEntry  m_PaletteEntries[256];
};


class Camera
{
public:
  std::string GetCameraString(int frameId) const
  {
    double computedAngle = (1200.0 * 3.14159265359) / atan((320.0 / 2) / (focal - 0.5)) / 180.0;
    return std::format("frame {} \r\ncamera {} {} {} {} {} {} {} {}\r\n", frameId, cam_x, cam_y, cam_z, target_x, target_y, target_z, gamma, computedAngle);   // Almost OK but wobbly!
  }

public:
  int32_t     cam_x;
  int32_t     cam_z;
  int32_t     cam_y;
  int32_t     target_x;
  int32_t     target_z;
  int32_t     target_y;
  int32_t     gamma;       ///< Aka "roll" but apparently ignored in the game?
  int32_t     focal;
};


enum class ChunkType
{
  e_Unknown = 0,  // We don't know that one
  e_End,
  e_FrameLen,     ///< Used to know the size of each frame
  e_Format,
  e_Palette,
  e_NulChunk,     ///< Used to pad data on aligned sectors to improve the loading performance 
  e_KeyFrame,
  e_DltFrame,
  e_Recouvre,
  e_Camera,
  e_SoundBuf,
  e_SoundFrm,
  e_SoundEnd,
  e_SAL_STRT,
  e_SAL_PART,
  e_SAL_END,
  e_SAL_COMP,
};

class Chunk
{
public:
  ChunkType GetChunkType() const
  {
    if (memcmp("NulChunk", m_Name, 8) == 0)		return ChunkType::e_NulChunk;
    if (memcmp("KeyFrame", m_Name, 8) == 0)		return ChunkType::e_KeyFrame;
    if (memcmp("DltFrame", m_Name, 8) == 0)		return ChunkType::e_DltFrame;
    if (memcmp("FrameLen", m_Name, 8) == 0)		return ChunkType::e_FrameLen;
    if (memcmp("Format  ", m_Name, 8) == 0)		return ChunkType::e_Format;
    if (memcmp("Palette ", m_Name, 8) == 0)		return ChunkType::e_Palette;
    if (memcmp("SoundBuf", m_Name, 8) == 0)		return ChunkType::e_SoundBuf;
    if (memcmp("SoundFrm", m_Name, 8) == 0)		return ChunkType::e_SoundFrm;
    if (memcmp("SoundEnd", m_Name, 8) == 0)		return ChunkType::e_SoundEnd;
    if (memcmp("SAL_STRT", m_Name, 8) == 0)		return ChunkType::e_SAL_STRT;
    if (memcmp("SAL_PART", m_Name, 8) == 0)		return ChunkType::e_SAL_PART;
    if (memcmp("SAL_END ", m_Name, 8) == 0)		return ChunkType::e_SAL_END;
    if (memcmp("SAL_COMP", m_Name, 8) == 0)		return ChunkType::e_SAL_COMP;
    if (memcmp("Recouvre", m_Name, 8) == 0)		return ChunkType::e_Recouvre;
    if (memcmp("Camera  ", m_Name, 8) == 0)		return ChunkType::e_Camera;
    if (memcmp("End     ", m_Name, 8) == 0)		return ChunkType::e_End;

    return ChunkType::e_Unknown;
  }

  std::string GetChunkName() const
  {
    return std::string(m_Name, 8);
  }

  uint32_t GetChunkSize() const
  {
    return m_Size;
  }

  const Chunk* GetNextChunk() const
  {
    assert(sizeof(Chunk) == 12);
    return GetChunkAtOffset(sizeof(Chunk) + m_Size);
  }

  const Chunk* GetChunkAtOffset(size_t offset) const
  {
    const char* pointer((const char*)this);
    pointer += offset;
    return (const Chunk*)pointer;
  }

  template<typename T>
  const T* GetData() const
  {
    const char* pointer((const char*)(this + 1));
    return (const T*)pointer;
  }

private:
  char	    m_Name[8];
  uint32_t  m_Size;
};





struct PCXHeader
{
  char password = 10;
  char version = 5;
  char encoding = 1;
  char bits_per_pixel = 8;                  // 256 colors
  short int xmin = 0, ymin = 0, xmax =0, ymax =0;
  short int xres, yres;
  unsigned char palette[48] = {0};
  char reserved = 0;
  char no_of_planes = 1;
  short int bytes_per_line = 0;
  short int palette_type = 0;
  char filler[58] = { 0 };
};


class ImageBuffer
{
public:
  ImageBuffer(uint32_t width, uint32_t height)
    : m_Width(width)
    , m_Height(height)
  {
    m_Buffer.resize((size_t)m_Width * (size_t)m_Height);
  }

  uint8_t* GetBuffer() { return m_Buffer.data(); }

  void SaveToPcx(const char* filename, const uint8_t* ptrpalette)
  {
    short int index = 0, i, k, num_out;
    unsigned char ch, file_buf[320 * 2];

    PCXHeader pcx_header;
    pcx_header.xmax = m_Width - 1;
    pcx_header.ymax = m_Height - 1;
    pcx_header.xres = m_Width;
    pcx_header.yres = m_Height;
    pcx_header.bytes_per_line = m_Width;

    std::ofstream os(filename, std::ios::binary);

    os.write((char*)&pcx_header, 128);

    const uint8_t* screen = GetBuffer();

    for (k = pcx_header.ymin; k <= pcx_header.ymax; k++)
    {
      short int number = 1;
      unsigned char old_ch = *(screen + m_Width * k);

      for (i = 1; i <= (int32_t)m_Width; i++)
      {
        if (i == m_Width)   ch = old_ch - 1;
        else		    ch = *(screen + m_Width * k + i);

        if ((ch == old_ch) && number < 63)
        {
          number++;
        }
        else
        {
          num_out = ((unsigned char)number | 0xC0);
          if ((number != 1) || ((old_ch & 0xC0) == 0xC0))
          {
            file_buf[index++] = (unsigned char)num_out;
          }
          file_buf[index++] = old_ch;
          old_ch = ch;
          number = 1;
        }
      }
      os.write((char*)file_buf, index);

      index = 0;
    }

    uint8_t c = 0x0C;
    os.write((char*)&c, 1);
    os.write((char*)ptrpalette, 768);
    os.close();
  }

  void SaveToRaw(const char* filename, const uint8_t* ptrpalette)
  {
    std::ofstream os(filename, std::ios::binary);
    os.write((char*)m_Buffer.data(), m_Buffer.size());
    os.close();
  }

public:
  uint32_t                m_Width;
  uint32_t                m_Height;
  std::vector<uint8_t>    m_Buffer;
};






class ACFDecoder
{
public:

  void SetPixel(int x, int y, uint8_t color)
  {
    m_CurrentTile[x + (y * m_Width)] = color;
  }


  // 3 bytes (6 bitsx4) for the position, 4 bytes for colors
  void Update4()
  {
    uint32_t value = *(uint32_t*)m_UnAlignedStream;	// Que 3 octets int‚ressants (Little Endian)
    m_UnAlignedStream += 3;

    for (int32_t i = 0; i < 4; i++)
    {
      SetPixel(value & 7 , ((value >> 3) & 7), *m_AlignedStream++);
      value >>= 6;
    }
  }


  void Update8()
  {
    Update4();
    Update4();
  }



  void Update16()
  {
    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t mask = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (mask & 1)
        {
          SetPixel(x,y, *m_UnAlignedStream++);
        }
        mask >>= 1;
      }
    }
  }



  void BlockCopy8x8(uint8_t* dest, uint8_t* source)
  {
    for (int32_t y = 0; y < 8; y++)
    {
      memcpy(dest + y * m_Width, source + y * m_Width, 8);
    }
  }

  void BlockCopy4x4(uint8_t* dest, uint8_t* source)
  {
    for (int32_t y = 0; y < 4; y++)
    {
      memcpy(dest + y * m_Width, source + y * m_Width, 4);
    }
  }


  

  void ZeroMotionDecode()
  {
    BlockCopy8x8(m_CurrentTile, m_PreviousTile);
  }


  void ShortMotion8Decode()
  {
    int32_t value = *m_UnAlignedStream++;
    int32_t dx = (((value & 15) << 28) >> 28);
    int32_t dy = ((value << 24) >> 28);
    BlockCopy8x8(m_CurrentTile, m_PreviousTile + (4 + m_Width * 4) + dx + dy * m_Width);
  }

  void ShortMotion4Decode()
  {
    int32_t value = *m_AlignedStream++;
    int32_t dx = (((value & 15) << 28) >> 28);
    int32_t dy = ((value << 24) >> 28);
    BlockCopy4x4(m_CurrentTile, m_PreviousTile + 2 + m_Width * 2 + dx + dy * m_Width);
    value = *m_AlignedStream++;
    dx = (((value & 15) << 28) >> 28);
    dy = ((value << 24) >> 28);
    BlockCopy4x4(m_CurrentTile + 4, m_PreviousTile + 2 + m_Width * 2 + dx + dy * m_Width + 4);
    value = *m_AlignedStream++;
    dx = (((value & 15) << 28) >> 28);
    dy = ((value << 24) >> 28);
    BlockCopy4x4(m_CurrentTile + m_Width * 4, m_PreviousTile + 2 + m_Width * 2 + dx + dy * m_Width + m_Width * 4);
    value = *m_AlignedStream++;
    dx = (((value & 15) << 28) >> 28);
    dy = ((value << 24) >> 28);
    BlockCopy4x4(m_CurrentTile + m_Width * 4 + 4, m_PreviousTile + 2 + m_Width * 2 + dx + dy * m_Width + m_Width * 4 + 4);
  }


  void Motion8Decode()
  {
    BlockCopy8x8(m_CurrentTile, m_PreviousFrameBuffer + (*(uint16_t*)m_UnAlignedStream));
    m_UnAlignedStream += 2;
  }

  void Motion4Decode()
  {
    BlockCopy4x4(m_CurrentTile, m_PreviousFrameBuffer + ReadU16(m_AlignedStream));
    BlockCopy4x4(m_CurrentTile + 4, m_PreviousFrameBuffer + ReadU16(m_AlignedStream));
    BlockCopy4x4(m_CurrentTile + m_Width * 4, m_PreviousFrameBuffer + ReadU16(m_AlignedStream));
    BlockCopy4x4(m_CurrentTile + m_Width * 4 + 4, m_PreviousFrameBuffer + ReadU16(m_AlignedStream));
  }


  void ROMotion8Decode()
  {
    BlockCopy8x8(m_CurrentTile, m_PreviousTile + ReadS16(m_UnAlignedStream) + 4 + m_Width * 4);
  }
  void ROMotion4Decode()
  {
    BlockCopy4x4(m_CurrentTile, m_PreviousTile + ReadS16(m_AlignedStream) + 2 + m_Width * 2);
    BlockCopy4x4(m_CurrentTile + 4, m_PreviousTile + 4 + ReadS16(m_AlignedStream) + 2 + m_Width * 2);
    BlockCopy4x4(m_CurrentTile + m_Width * 4, m_PreviousTile + m_Width * 4 + ReadS16(m_AlignedStream) + 2 + m_Width * 2);
    BlockCopy4x4(m_CurrentTile + m_Width * 4 + 4, m_PreviousTile + m_Width * 4 + 4 + ReadS16(m_AlignedStream) + 2 + m_Width * 2);
  }





  void RCMotion8Decode()
  {
    BlockCopy8x8(m_CurrentTile, m_PreviousTile + ReadXYOffset(m_UnAlignedStream,m_Width) + 4 + m_Width * 4);
  }
  void RCMotion4Decode()
  {
    BlockCopy4x4(m_CurrentTile, m_PreviousTile + ReadXYOffset(m_AlignedStream, m_Width) + 2 + m_Width * 2);
    BlockCopy4x4(m_CurrentTile + 4, m_PreviousTile + ReadXYOffset(m_AlignedStream, m_Width) + 2 + m_Width * 2 + 4);
    BlockCopy4x4(m_CurrentTile + m_Width * 4, m_PreviousTile + ReadXYOffset(m_AlignedStream, m_Width) + 2 + m_Width * 2 + m_Width * 4);
    BlockCopy4x4(m_CurrentTile + m_Width * 4 + 4, m_PreviousTile + ReadXYOffset(m_AlignedStream, m_Width) + 2 + m_Width * 2 + m_Width * 4 + 4);
  }




  // Load one byte, apply it to the entire tile
  void SingleColorFillDecode()
  {
    uint8_t colorTile = *m_UnAlignedStream++;

    for (int32_t y = 0; y < 8; y++)
    {
      memset(m_CurrentTile + y * m_Width, colorTile, 8);
    }
  }

  // Load four bytes, one for each quadrant of the tile
  void FourColorFillDecode()
  {
    uint8_t colorTopLeft = *m_AlignedStream++;
    uint8_t colorTopRight = *m_AlignedStream++;
    uint8_t colorBottomLeft = *m_AlignedStream++;
    uint8_t colorBottomRight = *m_AlignedStream++;

    for (int32_t y = 0; y < 4; y++)
    {
      memset(m_CurrentTile + y * m_Width, colorTopLeft, 4);
      memset(m_CurrentTile + y * m_Width + 4, colorTopRight, 4);
      memset(m_CurrentTile + (y + 4) * m_Width, colorBottomLeft, 4);
      memset(m_CurrentTile + (y + 4) * m_Width + 4, colorBottomRight, 4);
    }
  }





  //
  //
  // 10 octets:
  // - 8 octets formant 8x8x1 bits d‚signant chacun la couleur … utiliser
  // - 2 octets de couleur (indice dans la palette)
  //
  //
  void OneBitTileDecode()
  {
    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        SetPixel(x, y, m_UnAlignedStream[a & 1]);
        a >>= 1;
      }
    }
    m_UnAlignedStream += 2;
  }


  //
  // 20 octets:
  // - 4 octets de couleur (indice dans la palette)
  // - 16 octets formant 8x8x2 bits d‚signant chacun la couleur … utiliser
  //
  void TwoBitTileDecode()
  {
    const uint8_t* colors = m_AlignedStream;
    m_AlignedStream += 4;
    for (int32_t y = 0; y < 8; y++)
    {
      int32_t a = *(uint16_t*)m_AlignedStream;
      m_AlignedStream += 2;
      for (int32_t x = 0; x < 8; x++)
      {
        SetPixel(x, y, colors[a & 3]);
        a >>= 2;
      }
    }
  }





  //
  // 32 octets:
  // - 24 octets formant 8x8x3 bits d‚signant chacun la couleur … utiliser
  // - 8 octets de couleur (indice dans la palette)
  //
  void ThreeBitTileDecode()
  {
    for (int32_t y = 0; y < 8; y++)
    {
      uint32_t a = ReadU32(m_AlignedStream, 3);
      for (int32_t x = 0; x < 8; x++)
      {
        SetPixel(x, y, m_UnAlignedStream[a & 7]);
        a >>= 3;
      }
    }
    m_UnAlignedStream += 8;
  }




  //
  // 48 octets:
  // - 32 octets formant 8x8x4 bits d‚signant chacun la couleur … utiliser
  // - 16 octets de couleur (indice dans la palette)
  //
  void FourBitTileDecode()
  {
    for (int32_t y = 0; y < 8; y++)
    {
      uint32_t a = ReadU32(m_AlignedStream);
      for (int32_t x = 0; x < 8; x++)
      {
        SetPixel(x, y, m_UnAlignedStream[a & 15]);
        a >>= 4;
      }
    }
    m_UnAlignedStream += 16;
  }



  void OneBitSplitTileDecode()
  {
    for (uint32_t offset : g_SplitTileOffsets)
    {
      uint16_t a = ReadU16(m_AlignedStream);
      for (int32_t y = 0; y < 4; y++)
      {
        for (int32_t x = 0; x < 4; x++)
        {
          m_CurrentTile[x + y * m_Width + offset] = m_AlignedStream[a & 1];
          a >>= 1;
        }
      }
      m_AlignedStream += 2;
    }
  }
  

  void TwoBitSplitTileDecode()
  {
    for (uint32_t offset : g_SplitTileOffsets)
    {
      uint32_t a = ReadU32(m_AlignedStream);
      for (int32_t y = 0; y < 4; y++)
      {
        for (int32_t x = 0; x < 4; x++)
        {
          m_CurrentTile[x + y * m_Width + offset] = m_AlignedStream[a & 3];
          a >>= 2;
        }
      }
      m_AlignedStream += 4;
    }
  }

 
  void ThreeBitSplitTileDecode()
  {
    for (uint32_t offset : g_SplitTileOffsets)
    {
      uint32_t a;
      for (int32_t y = 0; y < 4; y++)
      {
        if (!(y & 1))
        {
          a = ReadU32(m_AlignedStream, 3);
        }
        for (int32_t x = 0; x < 4; x++)
        {
          m_CurrentTile[x + y * m_Width + offset] = m_UnAlignedStream[a & 7];
          a >>= 3;
        }
      }
      m_UnAlignedStream += 8;
    }
  }


  //
  // 20 bytes:
  // - 4 octets (Couleurs de base)
  // - 16 octets (4x4) pour indiquer les correspondances.
  //
  void CrossDecode()
  {
    uint32_t value = ReadU32(m_AlignedStream);
    for (uint32_t offset : g_SplitTileOffsets)
    {
      uint8_t* dest = m_CurrentTile + offset;
      if (value & 1)  dest[0] = m_AlignedStream[1];
      else	      dest[0] = m_AlignedStream[0];

      dest[0] = m_AlignedStream[(value & 1)];	    // 0 ou 1
      dest[1] = m_AlignedStream[0];		    // 0
      dest[2] = m_AlignedStream[0];		    // 0
      dest[3] = m_AlignedStream[((value & 2) >> 1) * 3];    // 0 ou 3

      dest[320] = m_AlignedStream[1];		    // 1
      dest[321] = m_AlignedStream[(value & 4) >> 2];	    // 0 ou 1
      dest[322] = m_AlignedStream[((value & 8) >> 3) * 3];    // 0 ou 3
      dest[323] = m_AlignedStream[3];		    // 3

      dest[640] = m_AlignedStream[1];		    // 1
      dest[641] = m_AlignedStream[1 + ((value & 16) >> 4)];   // 1 ou 2
      dest[642] = m_AlignedStream[2 + ((value & 32) >> 5)];   // 2 ou 3
      dest[643] = m_AlignedStream[3];		    // 3

      dest[960] = m_AlignedStream[1 + ((value & 64) >> 6)];   // 1 ou 2
      dest[961] = m_AlignedStream[2];		    // 2
      dest[962] = m_AlignedStream[2];		    // 2
      dest[963] = m_AlignedStream[2 + ((value & 128) >> 7)];  // 2 ou 3

      m_AlignedStream += 4;
      value >>= 8;
    }
  }



  void PrimeDecode()
  {
    int32_t prime_color = *m_UnAlignedStream++;
    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)  SetPixel(x, y, *m_UnAlignedStream++);
        else	    SetPixel(x, y, prime_color);
        a >>= 1;
      }
    }
  }




  // All 64 colors to directly copy to the buffer. No trick of any kind
  void RawTileDecode()
  {
    for (int32_t y = 0; y < 8; y++)
    {
      memcpy(m_CurrentTile + y * m_Width, m_AlignedStream, 8);
      m_AlignedStream += 8;
    }
  }



  
  // Similar to RawTileDecode, but all the colors are in the same bank, thus using only 4 bits per pixel
  void OneBankTileDecode()
  {
    uint8_t bank = *m_UnAlignedStream++;

    for (int32_t y = 0; y < 8; y++)
    {
      for (int32_t x = 0; x < 8; x++)
      {
        if (x & 1)    SetPixel(x,y, bank + ((*m_AlignedStream++) >> 4));
        else	      SetPixel(x,y, bank + ((*m_AlignedStream) & 15));
      }
    }
  }

  // Similar to OneBankTileDecode, but with colors in two different banks, and thus using 5 bits per pixel
  // 41 octets:
  // - 40 octets formant 8x8x5 bits qui d‚finissent la couleur (dans l'intervalle [0,15]), et le num‚ro de la banque.
  // - 1 octet formant 2x4 bits qui donnent les num‚ros des 2 banques de couleur … utiliser. (Il faut multiplier par 16)
  //
  void TwoBanksTileDecode()
  {
    uint8_t bank[2];

    bank[0] = ((*m_UnAlignedStream) & 0x0f) << 4;
    bank[1] = ((*m_UnAlignedStream) & 0xf0);
    m_UnAlignedStream++;

    for (uint32_t y = 0; y < 8; y++)
    {
      uint32_t part1 = *(int32_t*)m_AlignedStream;		// On r‚cupŠre ainsi 5 octets...
      uint32_t part2 = *(int32_t*)(m_AlignedStream + 4);
      m_AlignedStream += 5;
      for (uint32_t x = 0; x < 8; x++)
      {
        SetPixel(x, y, bank[(part1 & 16) >> 4] + (part1 & 15));   // Bit 4: Banque … utiliser / Bits 0-3:Couleur
        part1 >>= 5;
        part1 |= (part2 << 27);
        part2 >>= 5;
      }
    }
  }



  void BlockDecodeHorizontal()
  {
    uint8_t last_color = 0;

    for (uint32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (uint32_t x = 0; x < 8; x++)
      {
        if (a & 1)	last_color = *m_UnAlignedStream++;
        a >>= 1;
        SetPixel(x, y, last_color);
      }
    }
  }
  void BlockDecodeVertical()
  {
    uint8_t last_color = 0;

    for (int32_t x = 0; x < 8; x++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t y = 0; y < 8; y++)
      {
        if (a & 1)	last_color = *m_UnAlignedStream++;
        a >>= 1;
        SetPixel(x, y, last_color);
      }
    }
  }



  void BlockDecode2()
  {
    uint8_t last_color = 0;

    uint32_t* offsets = g_DiagonalOffsets_1;

    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)	last_color = *m_UnAlignedStream++;
        a >>= 1;
        m_CurrentTile[*offsets++] = last_color;
      }
    }
  }

  void BlockDecode3()
  {
    uint8_t last_color = 0;

    uint32_t* offsets = g_DiagonalOffsets_2;

    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)	last_color = *m_UnAlignedStream++;
        a >>= 1;
        m_CurrentTile[*offsets++] = last_color;
      }
    }
  }





  void BlockBank1DecodeHorizontal()
  {
    uint8_t last_color = 0;
    uint8_t bank = (*m_UnAlignedStream) << 4;	// R‚cupŠre la banque
    uint8_t flag = 1;

    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)
        {
          if (flag)
          {
            last_color = (*m_UnAlignedStream) >> 4;
            flag = 0;
            m_UnAlignedStream++;
          }
          else
          {
            last_color = (*m_UnAlignedStream) & 15;
            flag++;
          }
        }
        a >>= 1;
        SetPixel(x, y, bank + last_color);
      }
    }
    if (flag)	m_UnAlignedStream++;
  }


  void BlockBank1DecodeVertical()
  {
    uint8_t last_color = 0;
    uint8_t bank = (*m_UnAlignedStream) << 4;	// R‚cupŠre la banque
    uint8_t flag = 1;

    for (int32_t x = 0; x < 8; x++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t y = 0; y < 8; y++)
      {
        if (a & 1)
        {
          if (flag)
          {
            last_color = (*m_UnAlignedStream) >> 4;
            flag = 0;
            m_UnAlignedStream++;
          }
          else
          {
            last_color = (*m_UnAlignedStream) & 15;
            flag++;
          }
        }
        a >>= 1;
        SetPixel(x, y, bank + last_color);
      }
    }
    if (flag)	m_UnAlignedStream++;
  }


  void BlockBank1Decode2()
  {
    uint8_t last_color = 0;
    uint8_t bank = (*m_UnAlignedStream) << 4;	// R‚cupŠre la banque
    uint8_t flag = 1;

    uint32_t* offsets = g_DiagonalOffsets_1;

    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)
        {
          if (flag)
          {
            last_color = (*m_UnAlignedStream) >> 4;
            flag = 0;
            m_UnAlignedStream++;
          }
          else
          {
            last_color = (*m_UnAlignedStream) & 15;
            flag++;
          }
        }
        a >>= 1;
        m_CurrentTile[*offsets++] = bank + last_color;
      }
    }
    if (flag)	m_UnAlignedStream++;
  }

  void BlockBank1Decode3()
  {
    uint8_t last_color = 0;
    uint8_t bank = (*m_UnAlignedStream) << 4;	// Get the bank number
    uint8_t flag = 1;

    uint32_t* offsets = g_DiagonalOffsets_2;

    for (int32_t y = 0; y < 8; y++)
    {
      uint8_t a = *m_AlignedStream++;
      for (int32_t x = 0; x < 8; x++)
      {
        if (a & 1)
        {
          if (flag)
          {
            last_color = (*m_UnAlignedStream) >> 4;
            flag = 0;
            m_UnAlignedStream++;
          }
          else
          {
            last_color = (*m_UnAlignedStream) & 15;
            flag++;
          }
        }
        a >>= 1;
        m_CurrentTile[*offsets++] = bank + last_color;
      }
    }
    if (flag)	m_UnAlignedStream++;
  }





  void DecompressFrame()
  {
    m_PreviousTile = m_PreviousFrameBuffer = m_PreviousBuffer->GetBuffer();
    m_CurrentTile = m_CurrentBuffer->GetBuffer();

    const FrameData* frameData = m_CurrentChunk->GetData<FrameData>();
    m_UnAlignedStream = frameData->GetUnalignedData();                      // Pointer on things that can be out of alignment 
    m_AlignedStream   = frameData->GetAlignedData(m_Height);                // Pointer on data guaranteed to be aligned on a 32 bit multiple

    const uint8_t* ptr_opcode = frameData->GetOpcodesArray();               // Pointer on the list of decoding methods

    int32_t codes = -1;                                                     // "-1" means "need to read the 3 next bytes from the stream"
    for (int32_t y = 0; y < (m_Height/8); y++)
    {
      for (int32_t x = 0; x < (m_Width/8); x++)
      {
        if (codes == -1)
        {
          codes = ((*(int32_t*)ptr_opcode) | 0xff000000);
          ptr_opcode += 3;
        }

        switch (codes & 63)
        {
        case 0: RawTileDecode(); break;

        case 1: ZeroMotionDecode(); break;
        case 2: ZeroMotionDecode(); Update4(); break;
        case 3: ZeroMotionDecode(); Update8(); break;
        case 4: ZeroMotionDecode(); Update16(); break;

        case 5: ShortMotion8Decode(); break;
        case 6: ShortMotion8Decode(); Update4(); break;
        case 7: ShortMotion8Decode(); Update8(); break;
        case 8: ShortMotion8Decode(); Update16(); break;

        case 9: Motion8Decode(); break;
        case 10: Motion8Decode(); Update4(); break;
        case 11: Motion8Decode(); Update8(); break;
        case 12: Motion8Decode(); Update16(); break;

        case 13: ShortMotion4Decode(); break;
        case 14: ShortMotion4Decode(); Update4(); break;
        case 15: ShortMotion4Decode(); Update8(); break;
        case 16: ShortMotion4Decode(); Update16(); break;

        case 17: Motion4Decode(); break;
        case 18: Motion4Decode(); Update4(); break;
        case 19: Motion4Decode(); Update8(); break;
        case 20: Motion4Decode(); Update16(); break;

        case 21: SingleColorFillDecode(); break;
        case 22: SingleColorFillDecode(); Update4(); break;
        case 23: SingleColorFillDecode(); Update8(); break;
        case 24: SingleColorFillDecode(); Update16(); break;

        case 25: FourColorFillDecode(); break;
        case 26: FourColorFillDecode(); Update4(); break;
        case 27: FourColorFillDecode(); Update8(); break;
        case 28: FourColorFillDecode(); Update16(); break;

        case 29: OneBitTileDecode(); break;
        case 30: TwoBitTileDecode(); break;
        case 31: ThreeBitTileDecode(); break;
        case 32: FourBitTileDecode(); break;

        case 33: OneBitSplitTileDecode(); break;
        case 34: TwoBitSplitTileDecode(); break;
        case 35: ThreeBitSplitTileDecode(); break;

        case 36: CrossDecode(); break;
        case 37: PrimeDecode(); break;

        case 38: OneBankTileDecode(); break;
        case 39: TwoBanksTileDecode(); break;

        case 40: BlockDecodeHorizontal(); break;
        case 41: BlockDecodeVertical(); break;
        case 42: BlockDecode2(); break;
        case 43: BlockDecode3(); break;

        case 44: BlockBank1DecodeHorizontal(); break;
        case 45: BlockBank1DecodeVertical(); break;
        case 46: BlockBank1Decode2(); break;
        case 47: BlockBank1Decode3(); break;

        case 48: ROMotion8Decode(); break;
        case 49: ROMotion8Decode(); Update4(); break;
        case 50: ROMotion8Decode(); Update8(); break;
        case 51: ROMotion8Decode(); Update16(); break;

        case 52: RCMotion8Decode(); break;
        case 53: RCMotion8Decode(); Update4(); break;
        case 54: RCMotion8Decode(); Update8(); break;
        case 55: RCMotion8Decode(); Update16(); break;

        case 56: ROMotion4Decode(); break;
        case 57: ROMotion4Decode(); Update4(); break;
        case 58: ROMotion4Decode(); Update8(); break;
        case 59: ROMotion4Decode(); Update16(); break;

        case 60: RCMotion4Decode(); break;
        case 61: RCMotion4Decode(); Update4(); break;
        case 62: RCMotion4Decode(); Update8(); break;
        case 63: RCMotion4Decode(); Update16(); break;
        }

        codes >>= 6;		        // Get the next opcode by shifting. We will reload the next 3 bytes when the variable reaches the value -1

        m_PreviousTile += 8;	        // Next 8x8 block
        m_CurrentTile += 8;	        // Next 8x8 block
      }
      m_PreviousTile += m_Width * 7;	// Next 8x8 Line
      m_CurrentTile  += m_Width * 7;	// Next 8x8 Line
    }

    // Save the decoded picture to PCX format
    std::string pcxPath = m_OutputFolder + "PCX_" + std::to_string(m_FrameNumber++) + ".pcx";
    m_CurrentBuffer->SaveToPcx(pcxPath.c_str(), m_Palette->GetBuffer());

    // Swap the buffers
    std::swap(m_CurrentBuffer, m_PreviousBuffer);
  }


  void CreateBuffers()
  {
    delete m_CurrentBuffer;
    delete m_PreviousBuffer;
    m_CurrentBuffer  = new ImageBuffer(m_Width, m_Height);
    m_PreviousBuffer = new ImageBuffer(m_Width, m_Height);
  }



  bool ParseACF(const std::vector<std::byte>& acfFile)
  {
    m_CurrentChunk = (const Chunk*)acfFile.data();
    const Chunk* lastChunk(m_CurrentChunk->GetChunkAtOffset(acfFile.size()));

    CreateBuffers();

    m_FrameNumber = 0;
    std::string cameraFrames;

    while (m_CurrentChunk < lastChunk)
    {
      // Show the name of the current chunk
      std::cout << "Chunk: '" << m_CurrentChunk->GetChunkName() << "' (" << m_CurrentChunk->GetChunkSize() << " bytes long)" << std::endl;

      // Process the current chunk
      switch (m_CurrentChunk->GetChunkType())
      {
      case ChunkType::e_End:
        std::cout << "Reached the end" << std::endl;
        return true;

      case ChunkType::e_Unknown:
        std::cout << "Unknown chunk detected." << std::endl;
        break;

      case ChunkType::e_NulChunk:  // Nothing to do, nul chunks are just for padding/alignment to get better CD streaming performance
        break;

      case ChunkType::e_Format:
        m_Format = m_CurrentChunk->GetData<Format>();
        m_Width = m_Format->width;
        m_Height = m_Format->height;
        CreateBuffers();
        break;

      case ChunkType::e_FrameLen:
        m_FrameLen = m_CurrentChunk->GetData<FrameLen>();
        break;

      case ChunkType::e_Palette:
        m_Palette = m_CurrentChunk->GetData<Palette>();
        break;

      case ChunkType::e_Camera:
        m_Camera = m_CurrentChunk->GetData<Camera>();
        cameraFrames += m_Camera->GetCameraString(m_FrameNumber);
        break;

      case ChunkType::e_KeyFrame:
        DecompressFrame();
        break;

      case ChunkType::e_DltFrame:
        DecompressFrame();
        break;

      default:
        break;
      }

      // Jump to next one
      m_CurrentChunk = m_CurrentChunk->GetNextChunk();
    };

    // Save the VUE file with all the camera data
    // D:\PROJET\TIME\SCENE\STAGE00\RUN0\SCENE.VUE
    //std::string cameraPath = m_OutputFolder + "SCENE.VUE";
    std::string cameraPath = "D:\\TimeCo\\Mount_D\\Projet\\Time\\Scene\\STAGE00\\RUN0\\SCENE.VUE";
    std::ofstream os(cameraPath, std::ios::binary);
    os.write(cameraFrames.data(), cameraFrames.length());
    os.close();

    return true;  // Sometimes there's no End chunk
  }



  bool ExportACF(const std::filesystem::path& sourcePath,
    //const std::string& sourcePath,
    const std::string& outputFolder)
  {
    m_SourcePath = sourcePath;
    //m_SourceFile   = sourceFile;
    m_OutputFolder = outputFolder;

    // Let's load the file
    std::vector<std::byte> fileContent;
    if (std::filesystem::exists(sourcePath))
    {
      // We have a valid file, let's get the size and try to load it
      std::error_code errorCode;
      const auto fileSize = std::filesystem::file_size(sourcePath, errorCode);  // errorCode is available since C++17, without it, error handling is done by throwing an exception
      if (!errorCode)
      {
        std::cout << sourcePath << " size= " << fileSize << std::endl;
        fileContent.resize(fileSize);
        std::ifstream is(sourcePath, std::ios::binary);
        is.read(reinterpret_cast<char*>(fileContent.data()), fileSize);

        if (is.gcount() == fileSize)
        {
          // It's in the box
          if (ParseACF(fileContent))
          {
            // Yeah \o/
            return true;
          }
          else
          {
            // Got an error when trying to parse the ACF file
            std::cout << sourcePath << " : could not parse ACF format" << std::endl;
          }
        }
        else
        {
          // Got an error when trying to get the size
          std::cout << sourcePath << " file size " << fileSize << " does not match loaded size " << is.gcount() << std::endl;
        }
      }
      else
      {
        // Got an error when trying to get the size
        std::cout << sourcePath << " : " << errorCode.message() << std::endl;
      }
    }
    else
    {
      // Not found
      std::cout << sourcePath << " was not found" << std::endl;
    }
    return false;
  }

public:
  int32_t         m_Width  = 320;
  int32_t         m_Height = 240;
  int32_t         m_FrameNumber = 0;

  const Chunk*    m_CurrentChunk = nullptr;

  const Format*   m_Format   = nullptr;
  const Palette*  m_Palette  = nullptr;
  const FrameLen* m_FrameLen = nullptr;
  const Camera*   m_Camera   = nullptr;

  ImageBuffer*    m_PreviousBuffer = nullptr;
  uint8_t*        m_PreviousFrameBuffer = nullptr;
  uint8_t*        m_PreviousTile = nullptr;

  ImageBuffer*    m_CurrentBuffer = nullptr;
  uint8_t*        m_CurrentTile = nullptr;

  const uint8_t* m_AlignedStream = nullptr;
  const uint8_t* m_UnAlignedStream = nullptr;

  std::filesystem::path   m_SourcePath;
  std::string             m_OutputFolder;
};




static_assert(sizeof(PaletteEntry) == 3 , "Palette entries are supposed to be 8 bit RGB triplets (3 bytes)");
static_assert(sizeof(Palette) == 256 * 3, "A Palette should contain 256 8 bit RGB triples (768 bytes)");
static_assert(_HAS_CXX17 == 1           , "C++17 or higher required");

int main()
{
  std::cout << "ACF Extractor 1.0" << std::endl;
  //std::cout << _HAS_CXX17 << ":" << std::endl;


  try
  {
#if 0  // Batch mode
    //
    // Corrupted:
    // - SCN-01-0 (Rome streets)
    // - SCN-02-0 (Japan garden)
    // - SCN-03-0 (Medieval Castle)
    //
    std::string sourceFolder = "D:\\TimeCo\\FullGogGame\\ISO\\";
    std::string baseExportFolder = "C:\\Projects\\TimeCommando\\Exported\\ACF2PCX\\";
    for (auto& directoryEntry : std::filesystem::directory_iterator(sourceFolder))
    {
      const std::filesystem::path& path(directoryEntry.path());
      if (path.extension() == ".ACF")  // Should be a case insensitive comparison really, but the files come from a 8.3 MS-DOS content
      {
        std::string exportFolder = baseExportFolder + path.stem().string() + "\\";
        if (!std::filesystem::exists(exportFolder))
        {
          // Create folder if does not exist yet
          std::error_code errorCode;
          bool result=std::filesystem::create_directories(exportFolder, errorCode);
        }

        ACFDecoder acfDecoder;
        acfDecoder.ExportACF(path, exportFolder);
      }
    }
#else  // One one file decoder
    ACFDecoder acfDecoder;
    //acfDecoder.ExportACF("D:\\TimeCo\\FullGogGame\\ISO\\SCN-01-0.ACF", "C:\\Projects\\TimeCommando\\Exported\\ACF2PCX\\SCN-01-0\\");
    acfDecoder.ExportACF("D:\\TimeCo\\FullGogGame\\ISO\\SCN-00-0.ACF", "C:\\Projects\\TimeCommando\\Exported\\ACF2PCX\\SCN-00-0\\");
#endif
  }


  catch (std::exception e)
  {
    std::cout << e.what() << std::endl;
  }

  catch (...)
  {
    std::cout << "oops?" << std::endl;
  }

  return 0;
}




