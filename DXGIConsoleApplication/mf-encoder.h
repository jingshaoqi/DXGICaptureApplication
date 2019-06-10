#pragma once

#include <d3d11.h>

#include <malloc.h>

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <Mfreadwrite.h>
#include <mferror.h>
#include <Wmcodecdsp.h>
#include <Codecapi.h>

#include <stdio.h>
#include <initguid.h>

#undef SafeRelease
#define SafeRelease(ppT) \
{ \
    if (*ppT) \
    { \
        (*ppT)->Release(); \
        *ppT = NULL; \
    } \
} 

enum EncodeMode
{
	EncodeMode_CBR,
	EncodeMode_VBR_Quality,
	EncodeMode_VBR_Peak,
	EncodeMode_VBR_Unconstrained,
};

struct LeakyBucket
{
	DWORD dwBitrate;
	DWORD msBufferSize;
	DWORD msInitialBufferFullness;
};

#undef CHECK_HR
#define CHECK_HR(x) if (FAILED(x)) { fprintf(stdout, "Operation Failed %s:%d\n",__FILE__, __LINE__); goto bail; }

HRESULT CreateMediaSample(DWORD cbData, IMFSample **ppSample);
class CMediaFoundationEncoder
{
public:

	CMediaFoundationEncoder(){	}

	~CMediaFoundationEncoder()
	{
		SafeRelease(&m_pMFT);
		SafeRelease(&m_pOutputType);
	}

	HRESULT Initialize();
	HRESULT SetEncodingType(EncodeMode mode);
	HRESULT SetInputAndOutputType();
	HRESULT GetOutputType(IMFMediaType** ppMediaType);
	HRESULT GetLeakyBucket1(LeakyBucket *pBucket);
	HRESULT ProcessInput(IMFSample* pSample);
	HRESULT ProcessOutput(IMFSample **ppSample);
	HRESULT Drain();
	void EncodeToH264(ID3D11Texture2D *texture);
	void EncodeToH264BySample(IMFSample* pSample);

	LONGLONG rtStart = 0;
	UINT64 rtDuration = 0;

	IMFSample *pSampleIn = NULL;

	DWORD nLength = 0;
	FILE *pf = NULL;

protected:
	DWORD           m_dwInputID = 0;     // Input stream ID.
	DWORD           m_dwOutputID = 0;    // Output stream ID.

	IMFTransform    *m_pMFT = NULL;         // Pointer to the encoder MFT.
	IMFMediaType    *m_pOutputType = NULL;  // Output media type of the encoder.

};