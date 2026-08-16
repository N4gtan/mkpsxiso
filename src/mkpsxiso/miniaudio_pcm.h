#pragma once

#include "miniaudio.h"
#include "platform.h"
#include "common.h"

typedef struct {
    uint8_t header[44];
    int64_t pos;   // actual file position
    int64_t vpos;  // virtual file position
    int64_t vsize; // virtual file size
    unique_file file;
} VirtualWav;

inline ma_result virtual_wav_read(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead)
{
    VirtualWav *vw = (VirtualWav *)pDecoder->pUserData;
    size_t bytesRead = 0;
    if(vw->vpos < 44)
    {
        const size_t headerread = std::min<size_t>(bytesToRead, 44-vw->vpos);
        memcpy(pBufferOut, &vw->header[vw->vpos], headerread);
        vw->vpos += headerread;
        bytesRead += headerread;
        bytesToRead -= headerread;
        pBufferOut = ((uint8_t*)pBufferOut) + headerread;
    }
    if(bytesToRead > 0)
    {
        const size_t actualread = fread(pBufferOut, 1, bytesToRead, vw->file.get());
        bytesRead += actualread;
        vw->vpos += actualread;
        vw->pos += actualread;
    }
    *pBytesRead = bytesRead;
    return MA_SUCCESS;
}

inline ma_result virtual_wav_seek(ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin)
{
    VirtualWav *vw = (VirtualWav *)pDecoder->pUserData;

    if (origin == ma_seek_origin_end)
    {
        byteOffset += vw->vsize;
    }
    else if (origin == ma_seek_origin_current)
    {
        byteOffset += vw->vpos;
    }

    if (byteOffset < 0 || byteOffset > vw->vsize)
    {
        return MA_ERROR;
    }

    vw->vpos = byteOffset;
    vw->pos  = std::max<int64_t>(byteOffset - 44, 0);

    if (SeekFile(vw->file.get(), vw->pos, SEEK_SET) != 0)
    {
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

// feed to miniaudio as a wav file
inline MA_API ma_result ma_decoder_init_path_pcm(const fs::path& pFilePath, ma_decoder_config* pConfig, ma_decoder* pDecoder, VirtualWav *pUserData)
{
    pUserData->file.reset(OpenFile(pFilePath, "rb"));
    if(!pUserData->file)
    {
        return MA_INVALID_FILE;
    }

    const int64_t pcmSize = GetSize(pFilePath);
    if(pcmSize < 0)
    {
        printf("    ERROR: (PCM) unable to get file size\n");
        return MA_ERROR;
    }
    else if(pcmSize == 0)
	{
		printf("    ERROR: (PCM) byte count is 0\n");
        return MA_ERROR;
	}
	// 2 channels of 16 bit samples
    else if((pcmSize % (2 * sizeof(int16_t))) != 0)
	{
		printf("    ERROR: (PCM) byte count indicates non-integer sample count\n");
        return MA_ERROR;
	}

    pUserData->pos = 0;
    pUserData->vpos = 0;
    pUserData->vsize = pcmSize+44;

    memcpy(&pUserData->header[0], "RIFF", 4);
    const unsigned chunksize = (44 - 8) + pcmSize;
    pUserData->header[4] = chunksize;
    pUserData->header[5] = chunksize >> 8;
    pUserData->header[6] = chunksize >> 16;
    pUserData->header[7] = chunksize >> 24;
    memcpy(&pUserData->header[8], "WAVE", 4);
    memcpy(&pUserData->header[12], "fmt ", 4);
    const unsigned subchunk1size = 16;
    pUserData->header[16] = subchunk1size;
    pUserData->header[17] = subchunk1size >> 8;
    pUserData->header[18] = subchunk1size >> 16;
    pUserData->header[19] = subchunk1size >> 24;
    pUserData->header[20] = 1;
    pUserData->header[21] = 0;
    const unsigned numchannels = 2;
    pUserData->header[22] = numchannels;
    pUserData->header[23] = 0;
    const unsigned samplerate = 44100;
    pUserData->header[24] = (uint8_t)samplerate;
    pUserData->header[25] = samplerate >> 8;
    pUserData->header[26] = samplerate >> 16;
    pUserData->header[27] = samplerate >> 24;
    const unsigned bitspersample = 16;
    const unsigned byteRate = (samplerate * numchannels * (bitspersample/8));
    pUserData->header[28] = (uint8_t)byteRate;
    pUserData->header[29] = (uint8_t)(byteRate >> 8);
    pUserData->header[30] = byteRate >> 16;
    pUserData->header[31] = byteRate >> 24;
    const uint16_t blockalign = numchannels * (bitspersample/8);
    pUserData->header[32] = blockalign;
    pUserData->header[33] = blockalign >> 8;
    pUserData->header[34] = bitspersample;
    pUserData->header[35] = bitspersample >> 8;
    memcpy(&pUserData->header[36], "data", 4);
    pUserData->header[40] = pcmSize;
    pUserData->header[41] = pcmSize >> 8;
    pUserData->header[42] = pcmSize >> 16;
    pUserData->header[43] = pcmSize >> 24;

    pConfig->encodingFormat = ma_encoding_format_wav;
    return ma_decoder_init(&virtual_wav_read, &virtual_wav_seek, pUserData, pConfig, pDecoder);
}
