// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#ifndef _DUPLICATIONMANAGER_H_
#define _DUPLICATIONMANAGER_H_

#include <d3d11.h>
#include <dxgi1_2.h>
#include <sal.h>
#include <new>
#include <stdio.h>
#include <stdint.h>
#include "TextureConverter.h"

extern HRESULT SystemTransitionsExpectedErrors[];
extern HRESULT CreateDuplicationExpectedErrors[];
extern HRESULT FrameInfoExpectedErrors[];
extern HRESULT AcquireFrameExpectedError[];
extern HRESULT EnumOutputsExpectedErrors[];

typedef _Return_type_success_(return == DUPL_RETURN_SUCCESS) enum
{
	DUPL_RETURN_SUCCESS = 0,
	DUPL_RETURN_ERROR_EXPECTED = 1,
	DUPL_RETURN_ERROR_UNEXPECTED = 2
}DUPL_RETURN;


//
// Structure that holds D3D resources not directly tied to any one thread
//
typedef struct _DX_RESOURCES
{
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
// 	ID3D11VertexShader* VertexShader;
// 	ID3D11PixelShader* PixelShader;
// 	ID3D11InputLayout* InputLayout;
// 	ID3D11SamplerState* SamplerState;
} DX_RESOURCES;


class CMediaFoundationEncoder;

//
// Handles the task of duplicating an output.
//
class DUPLICATIONMANAGER
{
    public:
		
	//methods
        DUPLICATIONMANAGER();
        ~DUPLICATIONMANAGER();
        _Success_(*Timeout == false && return == DUPL_RETURN_SUCCESS) 
		DUPL_RETURN GetFrame(_Inout_ BYTE* ImageData);
		void Finalize();
        DUPL_RETURN InitDupl(_In_ FILE *log_file, UINT Output);

		HRESULT  InitRawCapture();
		HRESULT InitYUV420Capture();

		int GetImageHeight();
		int GetImageWidth();
		int GetImagePitch();
	//vars

    private:

    // vars
        IDXGIOutputDuplication* m_DeskDupl;
        ID3D11Texture2D* m_AcquiredDesktopImage;
		ID3D11Texture2D* m_DestImage;
        UINT m_OutputNumber;
        DXGI_OUTPUT_DESC m_OutputDesc;
		DX_RESOURCES *m_DxRes;
		FILE *m_log_file;
		int m_ImagePitch;

		LONGLONG rtStart = 0;

	//methods
		_Post_satisfies_(return != DUPL_RETURN_SUCCESS)
		DUPL_RETURN ProcessFailure(_In_opt_ ID3D11Device* Device, _In_ LPCWSTR Str, HRESULT hr, _In_opt_z_ HRESULT* ExpectedErrors = nullptr);
		void DisplayMsg(_In_ LPCWSTR Str, HRESULT hr);
		void CopyImage(BYTE* ImageData);
		void GetOutputDesc(_Out_ DXGI_OUTPUT_DESC* DescPtr);
		DUPL_RETURN DoneWithFrame();

		IMFSinkWriter *pSinkWriter = NULL;
		DWORD stream;

		uint8_t* texture_to_yuv(ID3D11Texture2D *texture[3], uint8_t *in_data, size_t in_len, size_t& out_len);

		IMFSample* text_to_yuv_to_sample(ID3D11Texture2D *texture[3], IMFMediaBuffer** pp_buf);
		
		CMediaFoundationEncoder* encoder = NULL;
		TextureConverter *m_textureConverter;
		ID3D11Texture2D              *m_texture[3];

};

#endif
