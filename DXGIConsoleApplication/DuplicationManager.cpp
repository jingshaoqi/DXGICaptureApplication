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

// Below are lists of errors expect from Dxgi API calls when a transition event like mode change, PnpStop, PnpStart
// desktop switch, TDR or session disconnect/reconnect. In all these cases we want the application to clean up the threads that process
// the desktop updates and attempt to recreate them.
// If we get an error that is not on the appropriate list then we exit the application

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

const UINT32 VIDEO_WIDTH = 1920;
const UINT32 VIDEO_HEIGHT = 1080;
const UINT32 VIDEO_FPS = 25;
const UINT32 VIDEO_BIT_RATE = 3000000;
const UINT32 VIDEO_BUFFER_SIZE = (VIDEO_WIDTH * VIDEO_HEIGHT * 3) >> 1;

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

HRESULT CreateMediaSample(DWORD cbData, IMFSample **ppSample);
class CWmaEncoder
{
public:

	CWmaEncoder()
		: m_pMFT(NULL), m_dwInputID(0), m_dwOutputID(0), m_pOutputType(NULL)
	{
	}

	~CWmaEncoder()
	{
		SafeRelease(&m_pMFT);
		SafeRelease(&m_pOutputType);
	}

	HRESULT Initialize();
	HRESULT SetEncodingType(EncodeMode mode);
	HRESULT SetInputType(IMFMediaType* pMediaType);
	HRESULT GetOutputType(IMFMediaType** ppMediaType);
	HRESULT GetLeakyBucket1(LeakyBucket *pBucket);
	HRESULT ProcessInput(IMFSample* pSample);
	HRESULT ProcessOutput(IMFSample **ppSample);
	HRESULT Drain();
	void EncodeToH264(ID3D11Texture2D *texture);

	LONGLONG rtStart;
	UINT64 rtDuration;

	IMFSample *pSampleIn;

	DWORD nLength;
	FILE *pf;

protected:
	DWORD           m_dwInputID;     // Input stream ID.
	DWORD           m_dwOutputID;    // Output stream ID.

	IMFTransform    *m_pMFT;         // Pointer to the encoder MFT.
	IMFMediaType    *m_pOutputType;  // Output media type of the encoder.

};

class CWmaEncoder;
CWmaEncoder* encoder = NULL;

#undef CHECK_HR
#define CHECK_HR(x) if (FAILED(x)) { fprintf(stdout, "Operation Failed line:%d\n", __LINE__); goto bail; }


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
	DUPL_RETURN Ret = InitializeDx(); 
	if (Ret != DUPL_RETURN_SUCCESS)
	{
		fprintf_s(log_file, "DX_RESOURCES couldn't be initialized.");
		return Ret;
	}
    m_OutputNumber = Output;

    // Get DXGI device
    IDXGIDevice* DxgiDevice = nullptr;
    HRESULT hr = m_DxRes->Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
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
		ProcessFailure(nullptr, L"Creating cpu accessable texture failed.", hr);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}

	if (m_DestImage == nullptr)
	{
		ProcessFailure(nullptr, L"Creating cpu accessable texture failed.", hr);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}

	hr = init_encoder();

    return DUPL_RETURN_SUCCESS;
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

	{
		m_DxRes->Context->CopyResource(m_DestImage, m_AcquiredDesktopImage);
		D3D11_MAPPED_SUBRESOURCE resource;
		UINT subresource = D3D11CalcSubresource(0, 0, 0);
		m_DxRes->Context->Map(m_DestImage, subresource, D3D11_MAP_READ, 0, &resource);
		encoder->EncodeToH264(m_DestImage);
		m_DxRes->Context->Unmap(m_DestImage, subresource);
		DoneWithFrame();
		return DUPL_RETURN_SUCCESS;
	}
	

	CopyImage(ImageData);
	
    return DUPL_RETURN_SUCCESS;
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

//
// Get DX_RESOURCES
//
DUPL_RETURN DUPLICATIONMANAGER::InitializeDx()
{

	HRESULT hr = S_OK;

	// Driver types supported
	D3D_DRIVER_TYPE DriverTypes[] =
	{
		D3D_DRIVER_TYPE_HARDWARE,
		D3D_DRIVER_TYPE_WARP,
		D3D_DRIVER_TYPE_REFERENCE,
	};
	UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

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

	// Create device
	for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
	{
		hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, 0, FeatureLevels, NumFeatureLevels,
			D3D11_SDK_VERSION, &m_DxRes->Device, &FeatureLevel, &m_DxRes->Context);
		if (SUCCEEDED(hr))
		{
			// Device creation success, no need to loop anymore
			break;
		}
	}
	if (FAILED(hr))
	{

		return ProcessFailure(nullptr, L"Failed to create device in InitializeDx", hr);
	}

	return DUPL_RETURN_SUCCESS;
}


HRESULT CWmaEncoder::Initialize()
{
	CLSID *pCLSIDs = NULL;   // Pointer to an array of CLISDs. 
	UINT32 count = 0;      // Size of the array.

	IMFMediaType* pOutMediaType = NULL;
	ICodecAPI* pCodecAPI = NULL;

	// Look for a encoder.
	MFT_REGISTER_TYPE_INFO toutinfo;
	toutinfo.guidMajorType = MFMediaType_Video;
	toutinfo.guidSubtype = MFVideoFormat_H264;

	HRESULT hr = S_OK;

	UINT32 unFlags = MFT_ENUM_FLAG_HARDWARE |
		MFT_ENUM_FLAG_SYNCMFT |
		MFT_ENUM_FLAG_LOCALMFT |
		MFT_ENUM_FLAG_SORTANDFILTER;

	hr = MFTEnum(
		MFT_CATEGORY_VIDEO_ENCODER,
		unFlags,                  // Reserved
		NULL,               // Input type to match. 
		&toutinfo,          // Output type to match.
		NULL,               // Attributes to match. (None.)
		&pCLSIDs,           // Receives a pointer to an array of CLSIDs.
		&count              // Receives the size of the array.
		);

	if (SUCCEEDED(hr))
	{
		if (count == 0)
		{
			hr = MF_E_TOPO_CODEC_NOT_FOUND;
		}
	}

	//hr = CoCreateInstance(CLSID_MF_H264EncFilter, NULL, 
	//        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT));


	//Create the MFT decoder
	if (SUCCEEDED(hr))
	{
		hr = CoCreateInstance(pCLSIDs[0], NULL,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT));
	}

	hr = m_pMFT->QueryInterface(IID_PPV_ARGS(&pCodecAPI));
	if (SUCCEEDED(hr))
	{
		VARIANT var = { 0 };

		// FIXME: encoder only
		var.vt = VT_UI4;
		var.ulVal = 0;
		hr = pCodecAPI->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

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
		var.ullVal = 1000000;
		hr = pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

#endif

		hr = S_OK;
	}


	SafeRelease(&pCodecAPI);

	hr = MFFrameRateToAverageTimePerFrame(25, 1, &rtDuration);
	
	CHECK_HR(hr = CreateMediaSample(VIDEO_BUFFER_SIZE, &pSampleIn));
	CHECK_HR(hr = pSampleIn->SetSampleDuration(400000));

	fopen_s(&pf, "test.wmv", "wb");

	return hr;

bail:
	return S_FALSE;
}

HRESULT CWmaEncoder::SetEncodingType(EncodeMode mode)
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

HRESULT CWmaEncoder::SetInputType(IMFMediaType* pMediaType)
{
	if (!m_pMFT)
	{
		return MF_E_NOT_INITIALIZED;
	}

	SafeRelease(&m_pOutputType);

	IMFMediaType *pOutputType = NULL;

	HRESULT hr = m_pMFT->GetStreamIDs(1, &m_dwInputID, 1, &m_dwOutputID);

	if (hr == E_NOTIMPL)
	{
		// The stream identifiers are zero-based.
		m_dwInputID = 0;
		m_dwOutputID = 0;
		hr = S_OK;
	}
	else if (FAILED(hr))
	{
		goto done;
	}

	{ // FIXME
		SafeRelease(&m_pOutputType);
		hr = MFCreateMediaType(&m_pOutputType);
		hr = m_pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		hr = m_pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		hr = m_pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, 1 ? eAVEncH264VProfile_Base : eAVEncH264VProfile_Main); // FIXME
		hr = m_pOutputType->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE);
		hr = m_pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		hr = MFSetAttributeSize(m_pOutputType, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT);
		hr = MFSetAttributeRatio(m_pOutputType, MF_MT_FRAME_RATE, VIDEO_FPS, 1);
		hr = MFSetAttributeRatio(m_pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		hr = m_pMFT->SetOutputType(m_dwOutputID, m_pOutputType, 0);
	}

	// Set the input type to the one passed by the application
	hr = m_pMFT->SetInputType(m_dwInputID, pMediaType, 0);
	if (FAILED(hr))
	{
		goto done;
	}

	// Loop through the available output types
	/*for (DWORD iType = 0; ; iType++)
	{
	hr = m_pMFT->GetOutputAvailableType(m_dwOutputID, iType, &pOutputType);
	if (FAILED(hr))
	{
	goto done;
	}

	hr = m_pMFT->SetOutputType(m_dwOutputID, pOutputType, 0);

	if (SUCCEEDED(hr))
	{
	m_pOutputType = pOutputType;
	m_pOutputType->AddRef();
	break;
	}

	SafeRelease(&pOutputType);
	}*/

done:
	SafeRelease(&pOutputType);
	return hr;
}

HRESULT CWmaEncoder::GetOutputType(IMFMediaType** ppMediaType)
{
	if (m_pOutputType == NULL)
	{
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	}

	*ppMediaType = m_pOutputType;
	(*ppMediaType)->AddRef();

	return S_OK;
};


HRESULT CWmaEncoder::GetLeakyBucket1(LeakyBucket *pBucket)
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


HRESULT CWmaEncoder::ProcessInput(IMFSample* pSample)
{
	if (m_pMFT == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	return m_pMFT->ProcessInput(m_dwInputID, pSample, 0);
}

HRESULT CWmaEncoder::ProcessOutput(IMFSample **ppSample)
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

void CWmaEncoder::EncodeToH264(ID3D11Texture2D *texture)
{
	HRESULT status;
	IMFMediaBufferPtr buffer;
	status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
	if (FAILED(status))
	{		
		return ;
	}

	IMF2DBufferPtr imfBuffer(buffer);
	DWORD length;
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

HRESULT CWmaEncoder::Drain()
{
	if (m_pMFT == NULL)
	{
		return MF_E_NOT_INITIALIZED;
	}

	return m_pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m_dwInputID);
}

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

HRESULT DUPLICATIONMANAGER::init_encoder()
{
	HRESULT hr = S_OK;
	IMFMediaType    *pMediaTypeIn = NULL;
	IMFMediaType    *pMediaTypeOut = NULL;
	IMFSample *pSampleOut = NULL;
	
	IMFMediaBuffer *pMediaBuffer = NULL;
	rtStart = 0;

	//CHECK_HR(hr = MFStartup(MF_VERSION, 0));
	encoder = new CWmaEncoder();
	CHECK_HR(hr = encoder->Initialize());

	CHECK_HR(hr = MFFrameRateToAverageTimePerFrame(VIDEO_FPS, 1, &rtDuration));

	CHECK_HR(hr = MFCreateMediaType(&pMediaTypeIn));
	CHECK_HR(hr = pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	CHECK_HR(hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
	CHECK_HR(hr = pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	CHECK_HR(hr = MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT));
	CHECK_HR(hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, VIDEO_FPS, 1));
	CHECK_HR(hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	CHECK_HR(hr = encoder->SetInputType(pMediaTypeIn));


	return S_OK;	
bail:
	SafeRelease(&pMediaBuffer);
	SafeRelease(&pMediaTypeIn);
	SafeRelease(&pMediaTypeOut);
	SafeRelease(&pSampleOut);
	return S_FALSE;
}

