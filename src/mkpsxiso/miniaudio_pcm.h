#pragma once
#include "common.h"
#include "miniaudio.h"

typedef struct {
    uint8_t header[44];
    int64_t pos;   // actual file position
    int64_t vpos;  // virtual file position
    int64_t vsize; // virtual file size
    FILE *file;
} VirtualWav;

MA_API ma_result ma_decoder_init_FILE_pcm(FILE *file, ma_decoder_config* pConfig, ma_decoder* pDecoder, VirtualWav *pUserData);

#ifdef __cplusplus
#include "platform.h"

class VirtualWavEx : public VirtualWav {
    public:
    unique_file pcmFp;
};

MA_API ma_result ma_decoder_init_path_pcm(const fs::path& pFilePath, ma_decoder_config* pConfig, ma_decoder* pDecoder, VirtualWavEx *pUserData);
#endif

#if defined(MINIAUDIO_IMPLEMENTATION) || defined(MA_IMPLEMENTATION)

static ma_result virtual_wav_read(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead)
{
    VirtualWav *vw = (VirtualWav *)pDecoder->pUserData;
    size_t bytesRead = 0;
    if(vw->vpos < 44)
    {
        const size_t headerread = ma_dr_wav_min(bytesToRead, 44-vw->vpos);
        memcpy(pBufferOut, &vw->header[vw->vpos], headerread);
        vw->vpos += headerread;
        bytesRead += headerread;
        bytesToRead -= headerread;
        pBufferOut = ((uint8_t*)pBufferOut) + headerread;
    }
    if(bytesToRead > 0)
    {
        const size_t actualread = fread(pBufferOut, 1, bytesToRead, vw->file);
        bytesRead += actualread;
        vw->vpos += actualread;
        vw->pos += actualread;
    }
    *pBytesRead = bytesRead;
    return MA_SUCCESS;
}

static ma_result virtual_wav_seek(ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin)
{
    int result;
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
    vw->pos  = ma_dr_wav_max(byteOffset - 44, 0);

    if (SeekFile(vw->file, vw->pos, SEEK_SET) != 0)
    {
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

#if !defined(_WIN32) && !((defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 1) || defined(_XOPEN_SOURCE) || defined(_POSIX_SOURCE)) && !defined(MA_BSD)
int fileno(FILE *stream);
#endif

static ma_result stdio_file_size(FILE *file, uint64_t *pSizeInBytes)
{
    int fd;
    struct stat info;

    MA_ASSERT(file  != NULL);

#if defined(_WIN32)
    fd = _fileno(file);
#else
    fd =  fileno(file);
#endif

    if (fstat(fd, &info) != 0) {
        return ma_result_from_errno(errno);
    }

    *pSizeInBytes = info.st_size;

    return MA_SUCCESS;
}

inline MA_API ma_result ma_decoder_init_FILE_pcm(FILE *file, ma_decoder_config* pConfig, ma_decoder* pDecoder, VirtualWav *pUserData)
{
    uint64_t pcmSize;
    if(stdio_file_size(file, &pcmSize) != MA_SUCCESS)
    {
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
    const uint16_t blockalign = numchannels * (bitspersample/8);;
    pUserData->header[32] = blockalign;
    pUserData->header[33] = blockalign >> 8;
    pUserData->header[34] = bitspersample;
    pUserData->header[35] = bitspersample >> 8;
    memcpy(&pUserData->header[36], "data", 4);
    pUserData->header[40] = pcmSize;
    pUserData->header[41] = pcmSize >> 8;
    pUserData->header[42] = pcmSize >> 16;
    pUserData->header[43] = pcmSize >> 24;

    pUserData->file = file;

    pConfig->encodingFormat = ma_encoding_format_wav;
    if(ma_decoder_init(&virtual_wav_read, &virtual_wav_seek, pUserData, pConfig, pDecoder) != MA_SUCCESS)
    {
        return MA_ERROR;
    }

    return MA_SUCCESS;
}
#ifdef __cplusplus
// feed to pcm file to miniaudio as a wav file
inline MA_API ma_result ma_decoder_init_path_pcm(const fs::path& pFilePath, ma_decoder_config* pConfig, ma_decoder* pDecoder, VirtualWavEx *pUserData)
{
    unique_file file(OpenFile(pFilePath, "rb"));
    if(!file)
    {
        return MA_INVALID_FILE;
    }
    if(ma_decoder_init_FILE_pcm(file.get(), pConfig, pDecoder, pUserData) != MA_SUCCESS)
    {
        return MA_NO_BACKEND;
    }
    pUserData->pcmFp = std::move(file);
    return MA_SUCCESS;
}
#endif

#endif