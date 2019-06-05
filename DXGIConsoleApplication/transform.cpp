
#include "stdafx.h"
#include "MFUtility.h"
#include "transform.h"

using namespace System;

#define CHECK_HR(hr, msg) if (hr != S_OK) { printf(msg); printf("Error: %.2X.\n", hr); goto done; }

#define CHECKHR_GOTO(x, y) if(FAILED(x)) goto y

#define INTERNAL_GUID_TO_STRING( _Attribute, _skip ) \
if (Attr == _Attribute) \
{ \
	pAttrStr = #_Attribute; \
	C_ASSERT((sizeof(#_Attribute) / sizeof(#_Attribute[0])) > _skip); \
	pAttrStr += _skip; \
	goto done; \
} \

// template <class T> void SafeRelease(T **ppT)
// {
// 	if (*ppT)
// 	{
// 		(*ppT)->Release();
// 		*ppT = NULL;
// 	}
// }
// 
// template <class T> inline void SafeRelease(T*& pT)
// {
// 	if (pT != NULL)
// 	{
// 		pT->Release();
// 		pT = NULL;
// 	}
// }

IMFTransform *pTransform = NULL; //< this is H264 Encoder MFT
IMFSinkWriter *pWriter= NULL;
DWORD writerVideoStreamIndex = 0;

// Format constants
const UINT32 VIDEO_WIDTH = 1920;
const UINT32 VIDEO_HEIGHT = 1080;
const UINT32 VIDEO_FPS = 25;
const UINT64 VIDEO_FRAME_DURATION = 10 * 1000 * 1000 / VIDEO_FPS;
const UINT32 VIDEO_BIT_RATE = 800000;

const GUID   VIDEO_ENCODING_FORMAT = MFVideoFormat_H264;
const GUID   VIDEO_INPUT_FORMAT = MFVideoFormat_RGB32;
const WCHAR *CAPTURE_FILENAME = L"sample.mp4";


HRESULT init_transform(void)
{
	const int WEBCAM_DEVICE_INDEX = 1;	// <--- Set to 0 to use default system webcam.
	
	const int SAMPLE_COUNT = 100;

	IMFMediaSource *videoSource = NULL;
	UINT32 videoDeviceCount = 0;
	IMFAttributes *videoConfig = NULL;
	IMFActivate **videoDevices = NULL;
	IMFSourceReader *videoReader = NULL;
	WCHAR *webcamFriendlyName;
	IMFMediaType *videoSourceOutputType = NULL, *pSrcOutMediaType = NULL;
	IUnknown *spTransformUnk = NULL;
	
	IWMResamplerProps *spResamplerProps = NULL;
	IMFMediaType *pMFTInputMediaType = NULL, *pMFTOutputMediaType = NULL;
	
	IMFMediaType *pVideoOutType = NULL;
	
	DWORD totalSampleBufferSize = 0;
	DWORD mftStatus = 0;
	UINT8 blob[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1e, 0x96, 0x54, 0x05, 0x01,
		0xe9, 0x80, 0x80, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80 };

// 	CHECK_HR(videoReader->GetCurrentMediaType(
// 		(DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
// 		&videoSourceOutputType), "Error retrieving current media type from first video stream.\n");


	// Create H.264 encoder.
	CHECK_HR(CoCreateInstance(CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER,
		IID_IUnknown, (void**)&spTransformUnk), "Failed to create H264 encoder MFT.\n");

	CHECK_HR(spTransformUnk->QueryInterface(IID_PPV_ARGS(&pTransform)), "Failed to get IMFTransform interface from H264 encoder MFT object.\n");

	MFCreateMediaType(&pMFTOutputMediaType);
	pMFTOutputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pMFTOutputMediaType->SetGUID(MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT);
	pMFTOutputMediaType->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE);
	CHECK_HR(MFSetAttributeSize(pMFTOutputMediaType, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT), "Failed to set frame size on H264 MFT out type.\n");
	CHECK_HR(MFSetAttributeRatio(pMFTOutputMediaType, MF_MT_FRAME_RATE, VIDEO_FPS, 1), "Failed to set frame rate on H264 MFT out type.\n");
	CHECK_HR(MFSetAttributeRatio(pMFTOutputMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Failed to set aspect ratio on H264 MFT out type.\n");
	pMFTOutputMediaType->SetUINT32(MF_MT_INTERLACE_MODE, 2);
	pMFTOutputMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

	CHECK_HR(pTransform->SetOutputType(0, pMFTOutputMediaType, 0), "Failed to set output media type on H.264 encoder MFT.\n");

	MFCreateMediaType(&pMFTInputMediaType);
	pMFTInputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pMFTInputMediaType->SetGUID(MF_MT_SUBTYPE, VIDEO_INPUT_FORMAT);
	CHECK_HR(MFSetAttributeSize(pMFTInputMediaType, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT), "Failed to set frame size on H264 MFT out type.\n");
	CHECK_HR(MFSetAttributeRatio(pMFTInputMediaType, MF_MT_FRAME_RATE, VIDEO_FPS, 1), "Failed to set frame rate on H264 MFT out type.\n");
	CHECK_HR(MFSetAttributeRatio(pMFTInputMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Failed to set aspect ratio on H264 MFT out type.\n");
	pMFTInputMediaType->SetUINT32(MF_MT_INTERLACE_MODE, 2);

	CHECK_HR(pTransform->SetInputType(0, pMFTInputMediaType, 0), "Failed to set input media type on H.264 encoder MFT.\n");

	CHECK_HR(pTransform->GetInputStatus(0, &mftStatus), "Failed to get input status from H.264 MFT.\n");
	if (MFT_INPUT_STATUS_ACCEPT_DATA != mftStatus) {
		printf("E: ApplyTransform() pTransform->GetInputStatus() not accept data.\n");
		goto done;
	}

	CHECK_HR(pTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL), "Failed to process FLUSH command on H.264 MFT.\n");
	CHECK_HR(pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL), "Failed to process BEGIN_STREAMING command on H.264 MFT.\n");
	CHECK_HR(pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL), "Failed to process START_OF_STREAM command on H.264 MFT.\n");

	// Create the MP4 sink writer.	         
	CHECK_HR(MFCreateSinkWriterFromURL(
		CAPTURE_FILENAME,
		NULL,
		NULL,
		&pWriter), "Error creating mp4 sink writer.");

	CHECK_HR(MFTRegisterLocalByCLSID(
		__uuidof(CColorConvertDMO),
		MFT_CATEGORY_VIDEO_PROCESSOR,
		L"",
		MFT_ENUM_FLAG_SYNCMFT,
		0,
		NULL,
		0,
		NULL
		), "Error registering colour converter DSP.\n");

	// Configure the output video type on the sink writer.
/*	CHECK_HR(MFCreateMediaType(&pVideoOutType), "Configure encoder failed to create media type for video output sink.");
	CHECK_HR(pVideoOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Failed to set video writer attribute, media type.");
	CHECK_HR(pVideoOutType->SetGUID(MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT), "Failed to set video writer attribute, video format (H.264).");
	CHECK_HR(pVideoOutType->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE), "Failed to set video writer attribute, bit rate.");
	CHECK_HR(CopyAttribute(videoSourceOutputType, pVideoOutType, MF_MT_FRAME_SIZE), "Failed to set video writer attribute, frame size.");
	CHECK_HR(CopyAttribute(videoSourceOutputType, pVideoOutType, MF_MT_FRAME_RATE), "Failed to set video writer attribute, frame rate.");
	CHECK_HR(CopyAttribute(videoSourceOutputType, pVideoOutType, MF_MT_PIXEL_ASPECT_RATIO), "Failed to set video writer attribute, aspect ratio.");
	CHECK_HR(CopyAttribute(videoSourceOutputType, pVideoOutType, MF_MT_INTERLACE_MODE), "Failed to set video writer attribute, interlace mode.");

	// See http://stackoverflow.com/questions/24411737/media-foundation-imfsinkwriterfinalize-method-fails-under-windows-7-when-mux
	CHECK_HR(pVideoOutType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob, sizeof(blob)/sizeof(blob[0])), "Failed to set MF_MT_MPEG_SEQUENCE_HEADER.\n");

	CHECK_HR(pWriter->AddStream(pVideoOutType, &writerVideoStreamIndex), "Failed to add the video stream to the sink writer.");

	pVideoOutType->Release();

	//CHECK_HR(pWriter->SetInputMediaType(writerVideoStreamIndex, videoSourceOutputType, NULL), "Error setting the sink writer video input type.\n");

	// Ready to go.

	CHECK_HR(pWriter->BeginWriting(), "Sink writer begin writing call failed.\n");
	*/
	return S_OK;
done:
	return S_FALSE;
}

void texture_to_sample(ID3D11Texture2D *texture, IMFSample **pp_sample)
{
	HRESULT status;
	IMFMediaBufferPtr buffer;
	status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);

	if (FAILED(status))
	{
		printf("MFCreateDXGISurfaceBuffer fail %d\n", __LINE__);
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

HRESULT encode_to_h264(ID3D11Texture2D* texture)
{
	MFT_OUTPUT_DATA_BUFFER outputDataBuffer;
	DWORD processOutputStatus = 0;
	DWORD streamIndex, flags;
	static LONGLONG llVideoTimeStamp, llSampleDuration;
	HRESULT mftProcessInput = S_OK;
	HRESULT mftProcessOutput = S_OK;
	MFT_OUTPUT_STREAM_INFO StreamInfo;
	IMFSample *mftOutSample = NULL;
	IMFMediaBuffer *pBuffer = NULL;
	//DWORD cbOutBytes = 0;
	int sampleCount = 0;
	DWORD mftOutFlags;

	IMFSample *videoSample = NULL;
	texture_to_sample(texture, &videoSample);

	CHECK_HR(videoSample->SetSampleTime(llVideoTimeStamp), "Error setting the video sample time.\n");
	CHECK_HR(videoSample->GetSampleDuration(&llSampleDuration), "Error getting video sample duration.\n");

	// Pass the video sample to the H.264 transform.

	CHECK_HR(pTransform->ProcessInput(0, videoSample, 0), "The resampler H264 ProcessInput call failed.\n");

	CHECK_HR(pTransform->GetOutputStatus(&mftOutFlags), "H264 MFT GetOutputStatus failed.\n");

	if (mftOutFlags == MFT_OUTPUT_STATUS_SAMPLE_READY)
	{
		CHECK_HR(pTransform->GetOutputStreamInfo(0, &StreamInfo), "Failed to get output stream info from H264 MFT.\n");

		while (true)
		{
			CHECK_HR(MFCreateSample(&mftOutSample), "Failed to create MF sample.\n");
			CHECK_HR(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer), "Failed to create memory buffer.\n");
			CHECK_HR(mftOutSample->AddBuffer(pBuffer), "Failed to add sample to buffer.\n");
			outputDataBuffer.dwStreamID = 0;
			outputDataBuffer.dwStatus = 0;
			outputDataBuffer.pEvents = NULL;
			outputDataBuffer.pSample = mftOutSample;

			mftProcessOutput = pTransform->ProcessOutput(0, 1, &outputDataBuffer, &processOutputStatus);

			if (mftProcessOutput != MF_E_TRANSFORM_NEED_MORE_INPUT)
			{
				CHECK_HR(outputDataBuffer.pSample->SetSampleTime(llVideoTimeStamp), "Error setting MFT sample time.\n");
				CHECK_HR(outputDataBuffer.pSample->SetSampleDuration(llSampleDuration), "Error setting MFT sample duration.\n");

				IMFMediaBuffer *buf = NULL;
				DWORD bufLength;
				CHECK_HR(mftOutSample->ConvertToContiguousBuffer(&buf), "ConvertToContiguousBuffer failed.\n");
				CHECK_HR(buf->GetCurrentLength(&bufLength), "Get buffer length failed.\n");

				//totalSampleBufferSize += bufLength;

				printf("Writing sample %i, sample time %I64d, sample duration %I64d, sample size %i.\n", sampleCount, llVideoTimeStamp, llSampleDuration, bufLength);
		//		CHECK_HR(pWriter->WriteSample(writerVideoStreamIndex, outputDataBuffer.pSample), "The stream sink writer was not happy with the sample.\n");
			}
			else {
				break;
			}

			pBuffer->Release();
			mftOutSample->Release();
		}
	}

	SafeRelease(&videoSample);
	
	return S_OK;

done:
	return S_FALSE;
}