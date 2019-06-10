// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved
#include "stdafx.h"
#include "Com.h"
#include "mfobjects.h"
#include "DuplicationManager.h"
#include "mf-encoder.h"
#include "common/debug.h"
#include "common/memcpySSE.h"
#include "TextureConverter.h"

// Below are lists of errors expect from Dxgi API calls when a transition event like mode change, PnpStop, PnpStart
// desktop switch, TDR or session disconnect/reconnect. In all these cases we want the application to clean up the threads that process
// the desktop updates and attempt to recreate them.
// If we get an error that is not on the appropriate list then we exit the application



// Format constants

// These are the errors we expect from general Dxgi API due to a transition
HRESULT SystemTransitionsExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	DXGI_ERROR_ACCESS_LOST,
	static_cast<HRESULT>(WAIT_ABANDONED),
	S_OK                                    // Terminate list with zero valued HRESULT
};


// These are the errors we expect from IDXGIOutput1::DuplicateOutput due to a transition
HRESULT CreateDuplicationExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	static_cast<HRESULT>(E_ACCESSDENIED),
	DXGI_ERROR_UNSUPPORTED,
	DXGI_ERROR_SESSION_DISCONNECTED,
	S_OK                                    // Terminate list with zero valued HRESULT
};

// These are the errors we expect from IDXGIOutputDuplication methods due to a transition
HRESULT FrameInfoExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	DXGI_ERROR_ACCESS_LOST,
	S_OK                                    // Terminate list with zero valued HRESULT
};

// These are the errors we expect from IDXGIAdapter::EnumOutputs methods due to outputs becoming stale during a transition
HRESULT EnumOutputsExpectedErrors[] = {
	DXGI_ERROR_NOT_FOUND,
	S_OK                                    // Terminate list with zero valued HRESULT
};

HRESULT InitializeSinkWriter(IMFSinkWriter **ppWriter, DWORD *pStreamIndex);

HRESULT InitializeSinkWriter(IMFSinkWriter **ppWriter, DWORD *pStreamIndex)
{
	*ppWriter = NULL;
	*pStreamIndex = NULL;

	IMFSinkWriter   *pSinkWriter = NULL;
	IMFMediaType    *pMediaTypeOut = NULL;
	IMFMediaType    *pMediaTypeIn = NULL;
	DWORD           streamIndex;

	HRESULT hr = MFCreateSinkWriterFromURL(L"output.mp4", NULL, NULL, &pSinkWriter);

	// Set the output media type.
	if (SUCCEEDED(hr))
	{
		hr = MFCreateMediaType(&pMediaTypeOut);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeSize(pMediaTypeOut, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeRatio(pMediaTypeOut, MF_MT_FRAME_RATE, VIDEO_FPS, 1);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeRatio(pMediaTypeOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSinkWriter->AddStream(pMediaTypeOut, &streamIndex);
	}

	// Set the input media type.
	if (SUCCEEDED(hr))
	{
		hr = MFCreateMediaType(&pMediaTypeIn);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, VIDEO_INPUT_FORMAT);
		//hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, VIDEO_FPS, 1);
	}
	if (SUCCEEDED(hr))
	{
		hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, NULL);
	}

	// Tell the sink writer to start accepting data.
	if (SUCCEEDED(hr))
	{
		hr = pSinkWriter->BeginWriting();
	}

	// Return the pointer to the caller.
	if (SUCCEEDED(hr))
	{
		*ppWriter = pSinkWriter;
		(*ppWriter)->AddRef();
		*pStreamIndex = streamIndex;
	}

	SafeRelease(&pSinkWriter);
	SafeRelease(&pMediaTypeOut);
	SafeRelease(&pMediaTypeIn);
	return hr;
}

//
// Constructor sets up references / variables
//
DUPLICATIONMANAGER::DUPLICATIONMANAGER() : m_DeskDupl(nullptr),
                                           m_AcquiredDesktopImage(nullptr),
										   m_DestImage(nullptr),
                                           m_OutputNumber(0),
										   m_ImagePitch(0),
										   m_DxRes(nullptr)
{
    RtlZeroMemory(&m_OutputDesc, sizeof(m_OutputDesc));
}

//
// Destructor simply calls CleanRefs to destroy everything
//
DUPLICATIONMANAGER::~DUPLICATIONMANAGER()
{
    if (m_DeskDupl)
    {
        m_DeskDupl->Release();
        m_DeskDupl = nullptr;
    }
    if (m_AcquiredDesktopImage)
    {
        m_AcquiredDesktopImage->Release();
        m_AcquiredDesktopImage = nullptr;
    }
	if (m_DestImage)
	{
		m_DestImage->Release();
		m_DestImage = nullptr;
	}
    if (m_DxRes->Device)
    {
		m_DxRes->Device->Release();
		m_DxRes->Device = nullptr;
    }
	if (m_DxRes->Device)
	{
		m_DxRes->Device->Release();
		m_DxRes->Device = nullptr;
	}
	if (m_DxRes->Context)
	{
		m_DxRes->Context->Release();
		m_DxRes->Context = nullptr;
	}
}

//
// Initialize duplication interfaces
//
DUPL_RETURN DUPLICATIONMANAGER::InitDupl(_In_ FILE *log_file, UINT Output)
{
	m_log_file = log_file;
	m_DxRes = new (std::nothrow) DX_RESOURCES;
	RtlZeroMemory(m_DxRes, sizeof(DX_RESOURCES));

	HRESULT hr = S_OK;

	// Feature levels supported
	D3D_FEATURE_LEVEL FeatureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_1
	};
	UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

	D3D_FEATURE_LEVEL FeatureLevel;

	// Create  hardware device
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, FeatureLevels, NumFeatureLevels,
		D3D11_SDK_VERSION, &m_DxRes->Device, &FeatureLevel, &m_DxRes->Context);
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed to create device in InitializeDx", hr);
	}

    m_OutputNumber = Output;

    // Get DXGI device
    IDXGIDevice* DxgiDevice = nullptr;
    hr = m_DxRes->Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI for DXGI Device", hr);
    }

    // Get DXGI adapter
    IDXGIAdapter* DxgiAdapter = nullptr;
    hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
    DxgiDevice->Release();
    DxgiDevice = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_DxRes->Device, L"Failed to get parent DXGI Adapter", hr, SystemTransitionsExpectedErrors);
    }

    // Get output
    IDXGIOutput* DxgiOutput = nullptr;
    hr = DxgiAdapter->EnumOutputs(Output, &DxgiOutput);
    DxgiAdapter->Release();
    DxgiAdapter = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_DxRes->Device, L"Failed to get specified output in DUPLICATIONMANAGER", hr, EnumOutputsExpectedErrors);
    }

    DxgiOutput->GetDesc(&m_OutputDesc);

    // QI for Output 1
    IDXGIOutput1* DxgiOutput1 = nullptr;
    hr = DxgiOutput->QueryInterface(__uuidof(DxgiOutput1), reinterpret_cast<void**>(&DxgiOutput1));
    DxgiOutput->Release();
    DxgiOutput = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI for DxgiOutput1 in DUPLICATIONMANAGER", hr);
    }

    // Create desktop duplication
    hr = DxgiOutput1->DuplicateOutput(m_DxRes->Device, &m_DeskDupl);
    DxgiOutput1->Release();
    DxgiOutput1 = nullptr;
    if (FAILED(hr))
    {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        {
            MessageBoxW(nullptr, L"There is already the maximum number of applications using the Desktop Duplication API running, please close one of those applications and then try again.", L"Error", MB_OK);
            return DUPL_RETURN_ERROR_UNEXPECTED;
        }
        return ProcessFailure(m_DxRes->Device, L"Failed to get duplicate output in DUPLICATIONMANAGER", hr, CreateDuplicationExpectedErrors);
    }

	//hr  = InitRawCapture();
	hr = InitYUV420Capture();
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed CreateTextureForDevice in DUPLICATIONMANAGER", hr);
	}

	encoder = new CMediaFoundationEncoder();
	hr = encoder->Initialize();
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed to init_encoder in DUPLICATIONMANAGER", hr);
	}
// 	hr = InitializeSinkWriter(&pSinkWriter, &stream);
// 	if (FAILED(hr))
// 	{
// 		return ProcessFailure(nullptr, L"Failed to InitializeSinkWriter in DUPLICATIONMANAGER", hr);
// 	}
    return DUPL_RETURN_SUCCESS;
}

HRESULT DUPLICATIONMANAGER::InitRawCapture()
{
	HRESULT hr = S_OK;
	D3D11_TEXTURE2D_DESC desc;
	DXGI_OUTDUPL_DESC lOutputDuplDesc;
	m_DeskDupl->GetDesc(&lOutputDuplDesc);
	desc.Width = lOutputDuplDesc.ModeDesc.Width;
	desc.Height = lOutputDuplDesc.ModeDesc.Height;
	desc.Format = lOutputDuplDesc.ModeDesc.Format;
	desc.ArraySize = 1;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.MipLevels = 1;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.Usage = D3D11_USAGE_STAGING;

	hr = m_DxRes->Device->CreateTexture2D(&desc, NULL, &m_DestImage);

	if (FAILED(hr))
	{
		ProcessFailure(nullptr, L"Creating cpu accessible texture failed.", hr);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}

	if (m_DestImage == nullptr)
	{
		ProcessFailure(nullptr, L"Creating cpu accessible texture failed.", hr);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}
	return hr;
}

HRESULT DUPLICATIONMANAGER::InitYUV420Capture()
{
	HRESULT status;
	D3D11_TEXTURE2D_DESC texDesc;

	ZeroMemory(&texDesc, sizeof(texDesc));
	texDesc.Width = VIDEO_WIDTH;
	texDesc.Height = VIDEO_HEIGHT;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_STAGING;
	texDesc.Format = DXGI_FORMAT_R8_UNORM;
	texDesc.BindFlags = 0;
	texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	texDesc.MiscFlags = 0;

	status = m_DxRes->Device->CreateTexture2D(&texDesc, NULL, &m_texture[0]);
	if (FAILED(status))
	{
		DEBUG_WINERROR("Failed to create texture", status);
		return false;
	}

	texDesc.Width /= 2;
	texDesc.Height /= 2;

	status = m_DxRes->Device->CreateTexture2D(&texDesc, NULL, &m_texture[1]);
	if (FAILED(status))
	{
		DEBUG_WINERROR("Failed to create texture", status);
		return false;
	}

	status = m_DxRes->Device->CreateTexture2D(&texDesc, NULL, &m_texture[2]);
	if (FAILED(status))
	{
		DEBUG_WINERROR("Failed to create texture", status);
		return false;
	}

	m_textureConverter = new TextureConverter();
	if (!m_textureConverter->Initialize(m_DxRes->Context, m_DxRes->Device, VIDEO_WIDTH, VIDEO_HEIGHT, FRAME_TYPE_YUV420))
		return false;

	return true;
}

void texture_to_sample(ID3D11Texture2D *texture, IMFSample **pp_sample)
{
	HRESULT status;
	IMFMediaBufferPtr buffer;
	status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
	if (FAILED(status))
	{
		return;
	}

	IMF2DBufferPtr imfBuffer(buffer);
	DWORD length;
	imfBuffer->GetContiguousLength(&length);
	buffer->SetCurrentLength(length);

	IMFSample *sample;
	MFCreateSample(&sample);
	sample->AddBuffer(buffer);
	*pp_sample = sample;
}

uint8_t* DUPLICATIONMANAGER::texture_to_yuv(ID3D11Texture2D *texture[3], uint8_t *in_data, size_t in_len, size_t& out_len)
{
	HRESULT result;
	bool timeout;

	size_t    remain = in_len;
	uint8_t * data = in_data;
	for (int i = 0; i < 3; ++i)
	{
		HRESULT                  status;
		D3D11_MAPPED_SUBRESOURCE mapping;
		D3D11_TEXTURE2D_DESC     desc;

		m_texture[i]->GetDesc(&desc);
		status = m_DxRes->Context->Map(m_texture[i], 0, D3D11_MAP_READ, 0, &mapping);
		if (FAILED(status))
		{
			DEBUG_WINERROR("Failed to map the texture", status);
			//DeInitialize();
			return NULL;
		}

		const unsigned int size = desc.Height * desc.Width;
		if (size > remain)
		{
			m_DxRes->Context->Unmap(m_texture[i], 0);
			DEBUG_ERROR("Too much data to fit in buffer");

			return NULL;
		}

		const uint8_t * src = (uint8_t *)mapping.pData;
		for (unsigned int y = 0; y < desc.Height; ++y)
		{
			memcpy(data, src, desc.Width);
			data += desc.Width;
			src += mapping.RowPitch;
		}
		m_DxRes->Context->Unmap(m_texture[i], 0);
		remain -= size;
	}
	size_t buf_size = in_len - remain;
	assert(buf_size == (VIDEO_WIDTH * VIDEO_HEIGHT * 3 >> 1));
	out_len = buf_size;
	return in_data;
}

IMFSample* DUPLICATIONMANAGER::text_to_yuv_to_sample(ID3D11Texture2D *texture[3], IMFMediaBuffer** pp_buf)
{
	IMFMediaBuffer *buf = NULL;
	size_t len = (VIDEO_WIDTH * VIDEO_HEIGHT * 3) >> 1;
	HRESULT hr = MFCreateMemoryBuffer(len, &buf);
	if (FAILED(hr))
	{
		DEBUG_WINERROR("MFCreateMemoryBuffer fail", hr);
		return NULL;
	}

	BYTE* t = NULL;
	size_t out_len = 0;
	hr = buf->Lock(&t, NULL, NULL);	
	uint8_t *p = texture_to_yuv(texture, t, len, out_len);
	buf->Unlock();

	buf->SetCurrentLength(len);

	IMFSample *sample;
	MFCreateSample(&sample);
	sample->AddBuffer(buf);

	hr = sample->SetSampleTime(rtStart);
	rtStart += VIDEO_FRAME_DURATION;
	hr = sample->SetSampleDuration(VIDEO_FRAME_DURATION);

	*pp_buf = buf;
	return sample;
}

//
// Get next frame and write it into Data
//
_Success_(*Timeout == false && return == DUPL_RETURN_SUCCESS)
DUPL_RETURN DUPLICATIONMANAGER::GetFrame(_Inout_ BYTE* ImageData)
{
    IDXGIResource* DesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO FrameInfo;

    // Get new frame
    HRESULT hr = m_DeskDupl->AcquireNextFrame(500, &FrameInfo, &DesktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return DUPL_RETURN_SUCCESS;
    }

    if (FAILED(hr))
    {
        return ProcessFailure(m_DxRes->Device, L"Failed to acquire next frame in DUPLICATIONMANAGER", hr, FrameInfoExpectedErrors);
    }

    // If still holding old frame, destroy it
    if (m_AcquiredDesktopImage)
    {
        m_AcquiredDesktopImage->Release();
        m_AcquiredDesktopImage = nullptr;
    }

    // QI for IDXGIResource
    hr = DesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&m_AcquiredDesktopImage));
    DesktopResource->Release();
    DesktopResource = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI for ID3D11Texture2D from acquired IDXGIResource in DUPLICATIONMANAGER", hr);
    }

#if 0
	{
		m_DxRes->Context->CopyResource(m_DestImage, m_AcquiredDesktopImage);
		D3D11_MAPPED_SUBRESOURCE resource;
		UINT subresource = D3D11CalcSubresource(0, 0, 0);
		hr = m_DxRes->Context->Map(m_DestImage, subresource, D3D11_MAP_READ, 0, &resource);
		IMFSample *sample = NULL;
		texture_to_sample(m_DestImage, &sample);
		m_DxRes->Context->Unmap(m_DestImage, subresource);

		hr = sample->SetSampleTime(rtStart);
		rtStart += VIDEO_FRAME_DURATION;
		hr = sample->SetSampleDuration(VIDEO_FRAME_DURATION);
		hr = pSinkWriter->WriteSample(stream, sample);
		//SafeRelease(&sample);

		
		DoneWithFrame();
		return DUPL_RETURN_SUCCESS;
	}
#else
	{
		TextureList planes;
		if (!m_textureConverter->Convert(m_AcquiredDesktopImage, planes))
			return DUPL_RETURN_ERROR_EXPECTED;

		for (int i = 0; i < 3; ++i)
		{
			ID3D11Texture2DPtr t = planes.at(i);
			m_DxRes->Context->CopyResource(m_texture[i], t);
		}
		
		IMFMediaBuffer *buf = NULL;
		IMFSample *sample = text_to_yuv_to_sample(m_texture, &buf);
		
		//hr = pSinkWriter->WriteSample(stream, sample);
		encoder->EncodeToH264BySample(sample);

		SafeRelease(&sample);
		SafeRelease(&buf);

		DoneWithFrame();
		return DUPL_RETURN_SUCCESS;
}
#endif

	

	CopyImage(ImageData);
	
    return DUPL_RETURN_SUCCESS;
}

void DUPLICATIONMANAGER::Finalize()
{
	if (pSinkWriter) 
	{
		pSinkWriter->Finalize();
	}
	
}

void  DUPLICATIONMANAGER::CopyImage(BYTE* ImageData)
{
	m_DxRes->Context->CopyResource(m_DestImage, m_AcquiredDesktopImage);
	D3D11_MAPPED_SUBRESOURCE resource;
	UINT subresource = D3D11CalcSubresource(0, 0, 0);
	m_DxRes->Context->Map(m_DestImage, subresource, D3D11_MAP_READ, 0, &resource);

	BYTE* sptr = reinterpret_cast<BYTE*>(resource.pData);

	//Store Image Pitch
	m_ImagePitch = resource.RowPitch;

	int height = GetImageHeight();
	memcpy_s(ImageData, resource.RowPitch*height, sptr, resource.RowPitch*height);

	m_DxRes->Context->Unmap(m_DestImage, subresource);
	DoneWithFrame();
}

int DUPLICATIONMANAGER::GetImageHeight()
{
	DXGI_OUTDUPL_DESC lOutputDuplDesc;
	m_DeskDupl->GetDesc(&lOutputDuplDesc);
	return m_OutputDesc.DesktopCoordinates.bottom - m_OutputDesc.DesktopCoordinates.top;
}


int DUPLICATIONMANAGER::GetImageWidth()
{
	DXGI_OUTDUPL_DESC lOutputDuplDesc;
	m_DeskDupl->GetDesc(&lOutputDuplDesc);
	return m_OutputDesc.DesktopCoordinates.right - m_OutputDesc.DesktopCoordinates.left;
}


int DUPLICATIONMANAGER::GetImagePitch()
{
	return m_ImagePitch;
}

//
// Release frame
//
DUPL_RETURN DUPLICATIONMANAGER::DoneWithFrame()
{
    HRESULT hr = m_DeskDupl->ReleaseFrame();
    if (FAILED(hr))
    {
        return ProcessFailure(m_DxRes->Device, L"Failed to release frame in DUPLICATIONMANAGER", hr, FrameInfoExpectedErrors);
    }

    if (m_AcquiredDesktopImage)
    {
        m_AcquiredDesktopImage->Release();
        m_AcquiredDesktopImage = nullptr;
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Gets output desc into DescPtr
//
void DUPLICATIONMANAGER::GetOutputDesc(_Out_ DXGI_OUTPUT_DESC* DescPtr)
{
    *DescPtr = m_OutputDesc;
}

_Post_satisfies_(return != DUPL_RETURN_SUCCESS)
DUPL_RETURN DUPLICATIONMANAGER::ProcessFailure(_In_opt_ ID3D11Device* Device, _In_ LPCWSTR Str, HRESULT hr, _In_opt_z_ HRESULT* ExpectedErrors)
{
	HRESULT TranslatedHr;

	// On an error check if the DX device is lost
	if (Device)
	{
		HRESULT DeviceRemovedReason = Device->GetDeviceRemovedReason();

		switch (DeviceRemovedReason)
		{
		case DXGI_ERROR_DEVICE_REMOVED:
		case DXGI_ERROR_DEVICE_RESET:
		case static_cast<HRESULT>(E_OUTOFMEMORY) :
		{
			// Our device has been stopped due to an external event on the GPU so map them all to
			// device removed and continue processing the condition
			TranslatedHr = DXGI_ERROR_DEVICE_REMOVED;
			break;
		}

		case S_OK:
		{
			// Device is not removed so use original error
			TranslatedHr = hr;
			break;
		}

		default:
		{
			// Device is removed but not a error we want to remap
			TranslatedHr = DeviceRemovedReason;
		}
		}
	}
	else
	{
		TranslatedHr = hr;
	}

	// Check if this error was expected or not
	if (ExpectedErrors)
	{
		HRESULT* CurrentResult = ExpectedErrors;

		while (*CurrentResult != S_OK)
		{
			if (*(CurrentResult++) == TranslatedHr)
			{
				return DUPL_RETURN_ERROR_EXPECTED;
			}
		}
	}

	// Error was not expected so display the message box
	DisplayMsg(Str, TranslatedHr);

	return DUPL_RETURN_ERROR_UNEXPECTED;
}

//
// Displays a message
//
void DUPLICATIONMANAGER::DisplayMsg(_In_ LPCWSTR Str, HRESULT hr)
{
	if (SUCCEEDED(hr))
	{
		fprintf_s(m_log_file, "%ls\n", Str);
		return;
	}

	const UINT StringLen = (UINT)(wcslen(Str) + sizeof(" with HRESULT 0x########."));
	wchar_t* OutStr = new wchar_t[StringLen];
	if (!OutStr)
	{
		return;
	}

	INT LenWritten = swprintf_s(OutStr, StringLen, L"%s with 0x%X.", Str, hr);
	if (LenWritten != -1)
	{
		fprintf_s(m_log_file, "%ls\n", OutStr);
	}

	delete[] OutStr;
}

