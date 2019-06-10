#include "stdafx.h"
#include "Com.h"
#include "mf-encoder.h"

HRESULT CreateMediaSample(DWORD cbData, IMFSample **ppSample)
{
	HRESULT hr = S_OK;

	IMFSample *pSample = NULL;
	IMFMediaBuffer *pBuffer = NULL;

	hr = MFCreateSample(&pSample);

	if (SUCCEEDED(hr))
	{
		hr = MFCreateMemoryBuffer(cbData, &pBuffer);
	}

	if (SUCCEEDED(hr))
	{
		hr = pSample->AddBuffer(pBuffer);
	}

	if (SUCCEEDED(hr))
	{
		*ppSample = pSample;
		(*ppSample)->AddRef();
	}

	SafeRelease(&pSample);
	SafeRelease(&pBuffer);
	return hr;
}


HRESULT CMediaFoundationEncoder::Initialize()
{
	CLSID *pCLSIDs = NULL;   // Pointer to an array of CLISDs. 
	UINT32 count = 0;      // Size of the array.
	VARIANT var = { 0 };

	IMFMediaType* pOutMediaType = NULL;
	ICodecAPI* pCodecAPI = NULL;

	// Look for a encoder.
	MFT_REGISTER_TYPE_INFO toutinfo;
	toutinfo.guidMajorType = MFMediaType_Video;
	toutinfo.guidSubtype = VIDEO_ENCODING_FORMAT;

	HRESULT hr = S_OK;

	UINT32 unFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
	//UINT32 unFlags = MFT_ENUM_FLAG_HARDWARE ;

	CHECK_HR(hr = MFTEnum(
		MFT_CATEGORY_VIDEO_ENCODER,
		unFlags,                  // Reserved
		NULL,               // Input type to match. 
		&toutinfo,          // Output type to match.
		NULL,               // Attributes to match. (None.)
		&pCLSIDs,           // Receives a pointer to an array of CLSIDs.
		&count              // Receives the size of the array.
		));

	if (count == 0)
	{
		printf("no transform found\n");
		return MF_E_TOPO_CODEC_NOT_FOUND;
	}

	//hr = CoCreateInstance(CLSID_MF_H264EncFilter, NULL, 
	//        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT));


	//Create the MFT decoder
	CHECK_HR(hr = CoCreateInstance(pCLSIDs[0], NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT)));
	CHECK_HR(hr = m_pMFT->QueryInterface(IID_PPV_ARGS(&pCodecAPI)));


	// FIXME: encoder only
	var.vt = VT_UI4;
	var.ulVal = 0;
	CHECK_HR(hr = pCodecAPI->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var));

	var.vt = VT_BOOL;
	var.boolVal = VARIANT_TRUE;
	hr = pCodecAPI->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);
	hr = pCodecAPI->SetValue(&CODECAPI_AVEncCommonRealTime, &var);

	var.vt = VT_UI4;
	var.ulVal = eAVEncCommonRateControlMode_CBR;
	hr = pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);


#if defined(CODECAPI_AVLowLatencyMode) // Win8 only
	var.vt = VT_BOOL;
	var.boolVal = VARIANT_TRUE;
	hr = pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &var);
#endif
#if defined(CODECAPI_AVEncCommonMeanBitRate) // Win8 only
	var.vt = VT_UI4;
	var.ullVal = 4000000;
	hr = pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

#endif

	SafeRelease(&pCodecAPI);

	CHECK_HR(hr = SetInputAndOutputType());
	CHECK_HR(hr = CreateMediaSample(VIDEO_BUFFER_SIZE, &pSampleIn));
	CHECK_HR(hr = pSampleIn->SetSampleDuration(VIDEO_FRAME_DURATION));

	errno_t t = fopen_s(&pf, "test-desktop.h264", "wb");
	if (t != 0) 
	{
		printf("fopen file fail:%m\n");
		return S_FALSE;
	}
	return hr;
bail:
	return hr;
}

HRESULT CMediaFoundationEncoder::SetEncodingType(EncodeMode mode)
{
	if (!m_pMFT)
	{
		return MF_E_NOT_INITIALIZED;
	}

	IPropertyStore* pProp = NULL;

	PROPVARIANT var;

	//Query the encoder for its property store

	HRESULT hr = m_pMFT->QueryInterface(IID_PPV_ARGS(&pProp));
	if (FAILED(hr))
	{
		goto done;
	}

	switch (mode)
	{
	case EncodeMode_CBR:
		//Set the VBR property to FALSE, which indicates CBR encoding
		//By default, encoding mode is set to CBR
		var.vt = VT_BOOL;
		var.boolVal = FALSE;
		hr = pProp->SetValue(MFPKEY_VBRENABLED, var);
		break;


	default:
		hr = E_NOTIMPL;
	}

done:
	SafeRelease(&pProp);
	return hr;
}

HRESULT CMediaFoundationEncoder::SetInputAndOutputType()
{
	HRESULT hr = S_OK;
	m_dwInputID = 0;
	m_dwOutputID = 0;

	hr = m_pMFT->GetStreamIDs(1, &m_dwInputID, 1, &m_dwOutputID);
	if (hr != S_OK&& hr != E_NOTIMPL) {
		goto bail;
	}

	SafeRelease(&m_pOutputType);
	CHECK_HR(hr = MFCreateMediaType(&m_pOutputType));
	CHECK_HR(hr = m_pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	CHECK_HR(hr = m_pOutputType->SetGUID(MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT));
	CHECK_HR(hr = m_pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, 1 ? eAVEncH264VProfile_Base : eAVEncH264VProfile_Main)); // FIXME
	CHECK_HR(hr = m_pOutputType->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE));
	CHECK_HR(hr = m_pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	CHECK_HR(hr = MFSetAttributeSize(m_pOutputType, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT));
	CHECK_HR(hr = MFSetAttributeRatio(m_pOutputType, MF_MT_FRAME_RATE, VIDEO_FPS, 1));
	CHECK_HR(hr = MFSetAttributeRatio(m_pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	CHECK_HR(hr = m_pMFT->SetOutputType(m_dwOutputID, m_pOutputType, 0));


	//input type
	IMFMediaType    *pMediaTypeIn = NULL;
	CHECK_HR(hr = MFFrameRateToAverageTimePerFrame(VIDEO_FPS, 1, &rtDuration));

	CHECK_HR(hr = MFCreateMediaType(&pMediaTypeIn));
	CHECK_HR(hr = pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	CHECK_HR(hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, VIDEO_INPUT_FORMAT));
	CHECK_HR(hr = pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	CHECK_HR(hr = MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT));
	CHECK_HR(hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, VIDEO_FPS, 1));
	CHECK_HR(hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	// Set the input type to the one passed by the application
	CHECK_HR(hr = m_pMFT->SetInputType(m_dwInputID, pMediaTypeIn, 0));

	return S_OK;
bail:
	return hr;
}

HRESULT CMediaFoundationEncoder::GetOutputType(IMFMediaType** ppMediaType)
{
	if (m_pOutputType == NULL)
	{
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	}

	*ppMediaType = m_pOutputType;
	(*ppMediaType)->AddRef();

	return S_OK;
};


HRESULT CMediaFoundationEncoder::GetLeakyBucket1(LeakyBucket *pBucket)
{
	if (m_pMFT == NULL || m_pOutputType == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	ZeroMemory(pBucket, sizeof(*pBucket));

	// Get the bit rate.

	pBucket->dwBitrate = 8 * MFGetAttributeUINT32(
		m_pOutputType, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);


	// Get the buffer window.

	IWMCodecLeakyBucket *pLeakyBuckets = NULL;

	HRESULT hr = m_pMFT->QueryInterface(IID_PPV_ARGS(&pLeakyBuckets));
	if (SUCCEEDED(hr))
	{
		ULONG ulBuffer = 0;

		hr = pLeakyBuckets->GetBufferSizeBits(&ulBuffer);

		if (SUCCEEDED(hr))
		{
			pBucket->msBufferSize = ulBuffer / (pBucket->dwBitrate / 1000);
		}

		pLeakyBuckets->Release();
	}

	return S_OK;
}


HRESULT CMediaFoundationEncoder::ProcessInput(IMFSample* pSample)
{
	if (m_pMFT == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	return m_pMFT->ProcessInput(m_dwInputID, pSample, 0);
}

HRESULT CMediaFoundationEncoder::ProcessOutput(IMFSample **ppSample)
{
	if (m_pMFT == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	*ppSample = NULL;

	IMFMediaBuffer* pBufferOut = NULL;
	IMFSample* pSampleOut = NULL;

	DWORD dwStatus;

	MFT_OUTPUT_STREAM_INFO mftStreamInfo = { 0 };
	MFT_OUTPUT_DATA_BUFFER mftOutputData = { 0 };

	HRESULT hr = m_pMFT->GetOutputStreamInfo(m_dwOutputID, &mftStreamInfo);
	if (FAILED(hr))
	{
		goto done;
	}

	//create a buffer for the output sample
	hr = MFCreateMemoryBuffer(mftStreamInfo.cbSize, &pBufferOut);
	if (FAILED(hr))
	{
		goto done;
	}

	//Create the output sample
	hr = MFCreateSample(&pSampleOut);
	if (FAILED(hr))
	{
		goto done;
	}

	//Add the output buffer 
	hr = pSampleOut->AddBuffer(pBufferOut);
	if (FAILED(hr))
	{
		goto done;
	}

	//Set the output sample
	mftOutputData.pSample = pSampleOut;

	//Set the output id
	mftOutputData.dwStreamID = m_dwOutputID;

	//Generate the output sample
	hr = m_pMFT->ProcessOutput(0, 1, &mftOutputData, &dwStatus);
	if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
	{
		hr = S_OK;
		goto done;
	}

	// TODO: Handle MF_E_TRANSFORM_STREAM_CHANGE

	if (FAILED(hr))
	{
		goto done;
	}

	*ppSample = pSampleOut;
	(*ppSample)->AddRef();

done:
	SafeRelease(&pBufferOut);
	SafeRelease(&pSampleOut);
	return hr;
};

void CMediaFoundationEncoder::EncodeToH264(ID3D11Texture2D *texture)
{
	HRESULT status;
	IMFMediaBufferPtr buffer;
	status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
	if (FAILED(status))
	{
		return;
	}

	IMF2DBufferPtr imfBuffer(buffer);
	DWORD length = 0;
	imfBuffer->GetContiguousLength(&length);
	buffer->SetCurrentLength(length);

	IMFSample *sample;
	MFCreateSample(&sample);
	sample->AddBuffer(buffer);
	HRESULT hr = S_OK;

#if 0
	sample->SetSampleTime(rtStart);
	sample->SetSampleDuration(rtDuration);
	CHECK_HR(hr = this->ProcessInput(sample));
#else
	pSampleIn->SetSampleTime(rtStart);
	pSampleIn->SetSampleDuration(rtDuration);
	CHECK_HR(hr = this->ProcessInput(pSampleIn));
#endif	

	rtStart += rtDuration;

	IMFSample *pSampleOut = NULL;
	CHECK_HR(hr = this->ProcessOutput(&pSampleOut));
	if (pSampleOut)
	{
		IMFMediaBuffer *pMediaBuffer = NULL;
		SafeRelease(&pMediaBuffer);
		CHECK_HR(hr = pSampleOut->GetBufferByIndex(0, &pMediaBuffer));
		CHECK_HR(hr = pMediaBuffer->GetCurrentLength(&nLength));
		if (nLength > 0)
		{
			BYTE* pBuffer = NULL;
			CHECK_HR(hr = pMediaBuffer->Lock(&pBuffer, NULL, NULL));

			//write to file
			fwrite(pBuffer, 1, nLength, pf);
			fflush(pf);
		}
		SafeRelease(&pSampleOut);
		SafeRelease(&pMediaBuffer);
	}

bail:
	return;
}

void CMediaFoundationEncoder::EncodeToH264BySample(IMFSample* pSample)
{
	HRESULT hr = S_OK;
	CHECK_HR(hr = this->ProcessInput(pSample));

	IMFSample *pSampleOut = NULL;
	CHECK_HR(hr = this->ProcessOutput(&pSampleOut));
	if (pSampleOut == NULL)
	{
		return;
	}

	IMFMediaBuffer *pMediaBuffer = NULL;
	SafeRelease(&pMediaBuffer);
	CHECK_HR(hr = pSampleOut->GetBufferByIndex(0, &pMediaBuffer));
	CHECK_HR(hr = pMediaBuffer->GetCurrentLength(&nLength));
	if (nLength > 0)
	{
		BYTE* pBuffer = NULL;
		CHECK_HR(hr = pMediaBuffer->Lock(&pBuffer, NULL, NULL));

		//write to file
		fwrite(pBuffer, 1, nLength, pf);
		fflush(pf);

		pMediaBuffer->Unlock();
	}
	SafeRelease(&pSampleOut);
	SafeRelease(&pMediaBuffer);


	return;
bail:
	return;
}

HRESULT CMediaFoundationEncoder::Drain()
{
	if (m_pMFT == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	return m_pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m_dwInputID);
}