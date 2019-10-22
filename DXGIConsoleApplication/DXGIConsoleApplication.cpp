// DXGIConsoleApplication.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"
#include "DuplicationManager.h"
#include <time.h>

clock_t start = 0, stop = 0, duration = 0;
int count = 0;
FILE *log_file;
char file_name[MAX_PATH];

void save_as_bitmap(unsigned char *bitmap_data, int rowPitch, int height, char *filename)
{
	// A file is created, this is where we will save the screen capture.

	FILE *f;

	BITMAPFILEHEADER   bmfHeader;
	BITMAPINFOHEADER   bi;

	bi.biSize = sizeof(BITMAPINFOHEADER);
	bi.biWidth = rowPitch / 4;
	//Make the size negative if the image is upside down.
	bi.biHeight = -height;
	//There is only one plane in RGB color space where as 3 planes in YUV.
	bi.biPlanes = 1;
	//In windows RGB, 8 bit - depth for each of R, G, B and alpha.
	bi.biBitCount = 32;
	//We are not compressing the image.
	bi.biCompression = BI_RGB;
	// The size, in bytes, of the image. This may be set to zero for BI_RGB bitmaps.
	bi.biSizeImage = 0;
	bi.biXPelsPerMeter = 0;
	bi.biYPelsPerMeter = 0;
	bi.biClrUsed = 0;
	bi.biClrImportant = 0;

	// rowPitch = the size of the row in bytes.
	DWORD dwSizeofImage = rowPitch * height;

	// Add the size of the headers to the size of the bitmap to get the total file size
	DWORD dwSizeofDIB = dwSizeofImage + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	//Offset to where the actual bitmap bits start.
	bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);

	//Size of the file
	bmfHeader.bfSize = dwSizeofDIB;

	//bfType must always be BM for Bitmaps
	bmfHeader.bfType = 0x4D42; //BM   

							   // TODO: Handle getting current directory
	fopen_s(&f, filename, "wb");

	DWORD dwBytesWritten = 0;
	dwBytesWritten += fwrite(&bmfHeader, sizeof(BITMAPFILEHEADER), 1, f);
	dwBytesWritten += fwrite(&bi, sizeof(BITMAPINFOHEADER), 1, f);
	dwBytesWritten += fwrite(bitmap_data, 1, dwSizeofImage, f);

	fclose(f);
}

int main(int argc, char* argv[])
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (!SUCCEEDED(hr))
	{
		return 0;
	}

	MFStartup(MF_VERSION, 0);
	errno_t er = fopen_s(&log_file, "logY.txt", "wb");
	if (er != 0) {
		printf("open log file error:%m\n");
		return -1;
	}

	DuplicationManager DupMgr;

	UINT Output = 0;

	// Make duplication manager
	hr = DupMgr.InitDupl(log_file, Output);
	if (hr != S_OK)
	{
		fprintf_s(log_file, "Duplication Manager couldn't be initialized.");
		return 0;
	}

	BYTE* pBuf = new BYTE[10 * 1024 * 1024];

	// Main duplication loop
	for (int i = 0; i < 1000; i++)
	{
		// Get new frame from desktop duplication
		hr = DupMgr.GetFrame(pBuf);
		if (hr != S_OK)
		{
			fprintf_s(log_file, "Could not get the frame.");
		}
		// 		sprintf_s(file_name, "%02d.bmp", i);
		// 		save_as_bitmap(pBuf, DuplMgr.GetImagePitch(), DuplMgr.GetImageHeight(), file_name);
	}
	delete pBuf;
	pBuf = NULL;
	DupMgr.Finalize();

	fclose(log_file);
	log_file = NULL;
	while (1)
	{
		printf("are you quit (Y/n)\n");
		printf("are you quit (Y/n)\n");
		printf("are you quit (Y/n)\n");
		char k = getchar();
		if (k == 'y' || k == 'Y') {
			break;
		}
	}

	return 0;
}
