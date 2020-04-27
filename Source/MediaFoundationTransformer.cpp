#include "MediaFoundationDecoder.h"
#include <sstream>
#include "SoyLib/src/SoyDebug.h"
#include "SoyLib/src/SoyPixels.h"
#include "SoyLib/src/SoyH264.h"
#include "MagicEnum/include/magic_enum.hpp"
#include "SoyFourcc.h"

#include <mfapi.h>
#include <mftransform.h>
#include <codecapi.h>
#include <Mferror.h>

#pragma comment(lib,"dxva2.lib")	
#pragma comment(lib,"evr.lib")	
#pragma comment(lib,"mf.lib")	
#pragma comment(lib,"mfplat.lib")	
#pragma comment(lib,"mfplay.lib")	
#pragma comment(lib,"mfreadwrite.lib")	
#pragma comment(lib,"mfuuid.lib")	

#include <SoyAutoReleasePtr.h>

//	https://github.com/sipsorcery/mediafoundationsamples/blob/master/MFH264RoundTrip/MFH264RoundTrip.cpp

namespace MediaFoundation
{
	class TActivateList;
	class TContext;
	class TActivateMeta;

	void	IsOkay(HRESULT Result, const char* Context);
	void	IsOkay(HRESULT Result,const std::string& Context);

	TActivateList	EnumTransforms(const GUID& Category);
	TActivateMeta	GetBestTransform(const GUID& Category,const ArrayBridge<Soy::TFourcc>& InputFilter,const ArrayBridge<Soy::TFourcc>& OutputFilter);
}


void MediaFoundation::IsOkay(HRESULT Result, const char* Context)
{
	Platform::IsOkay(Result, Context);
}

void MediaFoundation::IsOkay(HRESULT Result,const std::string& Context)
{
	Platform::IsOkay(Result, Context.c_str());
}

class MediaFoundation::TContext
{
public:
	TContext();
	~TContext();
};

class MediaFoundation::TActivateMeta
{
public:
	TActivateMeta() {}
	TActivateMeta(IMFActivate& Activate);

public:
	std::string						mName;
	bool							mHardwareAccelerated = false;
	BufferArray<Soy::TFourcc, 20>	mInputs;
	BufferArray<Soy::TFourcc, 20>	mOutputs;
	Soy::AutoReleasePtr<IMFActivate>	mActivate;
};

class MediaFoundation::TActivateList
{
public:
	TActivateList() {}
	TActivateList(TActivateList&& Move)
	{
		this->mActivates = Move.mActivates;
		this->mCount = Move.mCount;
		Move.mActivates = nullptr;
	}
	~TActivateList();

	void			EnumActivates(std::function<void(TActivateMeta&)> EnumMeta);

public:
	IMFActivate**	mActivates = nullptr;
	uint32_t		mCount = 0;
};

MediaFoundation::TActivateList::~TActivateList()
{
	if (mActivates)
	{
		for (auto i = 0; i < mCount; i++)
		{
			auto* Activate = mActivates[i];
			Activate->Release();
		}
	}
	if (mActivates)
	{
		CoTaskMemFree(mActivates);
		mActivates = nullptr;
	}
}

void MediaFoundation::TActivateList::EnumActivates(std::function<void(TActivateMeta&)> EnumMeta)
{
	for (auto a = 0; a < mCount; a++)
	{
		auto* Activate = mActivates[a];
		TActivateMeta Meta(*Activate);
		EnumMeta(Meta);
	}
}



template<typename T>
void MemZero(T& Object)
{
	memset(&Object, 0, sizeof(T));
}


MediaFoundation::TContext::TContext()
{
	CoInitializeEx(nullptr,COINIT_MULTITHREADED);

	auto Result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
	IsOkay(Result, "MFStartup");
}

MediaFoundation::TContext::~TContext()
{
	auto Result = MFShutdown();
	try
	{
		IsOkay(Result, "MFShutdown");
	}
	catch (std::exception& e)
	{
		std::Debug << e.what() << std::endl;
	}
}

template<typename TYPE>
void GetBlobs(IMFActivate& Activate, const GUID& Key, ArrayBridge<TYPE>&& Array)
{
	//	get blob size
	uint8_t* BlobData8 = nullptr;
	uint32_t BlobDataSize = 0;
	auto Result = Activate.GetAllocatedBlob(Key, &BlobData8, &BlobDataSize);

	auto Cleanup = [&]()
	{
		if (!BlobData8)
			return;
		CoTaskMemFree(BlobData8);
		BlobData8 = nullptr;
	};
	try
	{
		MediaFoundation::IsOkay(Result, "GetAllocatedBlob");
		auto* BlobData = reinterpret_cast<TYPE*>(BlobData8);
		auto BlobDataCount = BlobDataSize / sizeof(TYPE);

		auto BlobArray = GetRemoteArray(BlobData, BlobDataCount);
		Array.Copy(BlobArray);
		Cleanup();
	}
	catch (...)
	{
		Cleanup();
		throw;
	}
}

std::string GetString(IMFActivate& Activate, const GUID& Key)
{
	//	get length not including terminator
	uint32_t Length = 0;
	wchar_t StringWBuffer[1024] = { 0 };
	auto Result = Activate.GetString(Key, StringWBuffer, std::size(StringWBuffer), &Length);
	MediaFoundation::IsOkay(Result, "GetString");
	std::wstring StringW(StringWBuffer, Length);

	auto String = Soy::WStringToString(StringW);
	return String;
}

std::string GetStringSafe(IMFActivate& Activate, const GUID& Key)
{
	try
	{
		return GetString(Activate, Key);
	}
	catch (std::exception& e)
	{
		//std::Debug << e.what() << std::endl;
		return std::string();
	}
}

Soy::TFourcc GetFourCC(const GUID& Guid)
{
	//	https://docs.microsoft.com/en-us/windows/win32/medfound/video-subtype-guids#creating-subtype-guids-from-fourccs-and-d3dformat-values
	//	XXXXXXXX - 0000 - 0010 - 8000 - 00AA00389B71
	Soy::TFourcc Fourcc(Guid.Data1);
	return Fourcc;
}


GUID GetGuid(const Soy::TFourcc& Fourcc)
{
	//	https://docs.microsoft.com/en-us/windows/win32/medfound/video-subtype-guids#creating-subtype-guids-from-fourccs-and-d3dformat-values
	//	XXXXXXXX - 0000 - 0010 - 8000 - 00 AA 00 38 9B 71
	//	see DEFINE_MEDIATYPE_GUID
	//auto Fourcc32 = _byteswap_ulong(Fourcc.mFourcc32);
	auto Fourcc32 = (Fourcc.mFourcc32);
	GUID Guid = { Fourcc32, 0x000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
	return Guid;
}

MediaFoundation::TActivateMeta::TActivateMeta(IMFActivate& Activate) :
	mActivate	( &Activate, true )
{
	mName = GetStringSafe(Activate, MFT_FRIENDLY_NAME_Attribute);
	auto HardwareUrl = GetStringSafe(Activate, MFT_ENUM_HARDWARE_URL_Attribute);
	mHardwareAccelerated = !HardwareUrl.empty();

	Array<MFT_REGISTER_TYPE_INFO> InputTypes;
	Array<MFT_REGISTER_TYPE_INFO> OutputTypes;
	GetBlobs(Activate, MFT_INPUT_TYPES_Attributes, GetArrayBridge(InputTypes));
	GetBlobs(Activate, MFT_OUTPUT_TYPES_Attributes, GetArrayBridge(OutputTypes));
	
	for (auto i = 0; i < InputTypes.GetSize(); i++)
	{
		auto Fourcc = GetFourCC(InputTypes[i].guidSubtype);
		mInputs.PushBack(Fourcc);
	}

	for (auto i = 0; i < OutputTypes.GetSize(); i++)
	{
		auto Fourcc = GetFourCC(OutputTypes[i].guidSubtype);
		mOutputs.PushBack(Fourcc);
	}
}

//	MFT_CATEGORY_VIDEO_ENCODER	MFT_CATEGORY_VIDEO_DECODER
MediaFoundation::TActivateList MediaFoundation::EnumTransforms(const GUID& Category)
{
	//	auto init context once
	static TContext Context;

	//	gr: these filters always seem to return zero results, so get all and filter after
	//MFT_REGISTER_TYPE_INFO InputFilter = { 0 };
	//InputFilter.guidMajorType = MFMediaType_Video;
	//InputFilter.guidSubtype = MFVideoFormat_H264;
	//InputFilter.guidSubtype = MFVideoFormat_H264_ES;
	MFT_REGISTER_TYPE_INFO* InputFilter = nullptr;
	MFT_REGISTER_TYPE_INFO* OutputFilter = nullptr;

	uint32_t Flags = MFT_ENUM_FLAG_ALL;
	
	TActivateList Activates;
	IMFAttributes* Attributes = nullptr;
	auto Result = MFTEnum2(Category, Flags, InputFilter, OutputFilter, Attributes, &Activates.mActivates, &Activates.mCount);
	IsOkay(Result, "MFTEnum2");

	return Activates;
}

std::string GetName(const GUID Guid)
{
	if (Guid == MFVideoFormat_NV12)	return "MFVideoFormat_NV12";
	if (Guid == MFVideoFormat_YUY2)	return "MFVideoFormat_YUY2";
	if (Guid == MFVideoFormat_YV12)	return "MFVideoFormat_YV12";
	if (Guid == MFVideoFormat_IYUV)	return "MFVideoFormat_IYUV";
	if (Guid == MFVideoFormat_I420)	return "MFVideoFormat_I420";

	return "<guid>";
}

//	get the best activate which matches the input list, and output list
//	sorted results by hardware, then highest input, then highest output
MediaFoundation::TActivateMeta MediaFoundation::GetBestTransform(const GUID& Category,const ArrayBridge<Soy::TFourcc>& InputFilter,const ArrayBridge<Soy::TFourcc>& OutputFilter)
{
	auto Transformers = EnumTransforms(MFT_CATEGORY_VIDEO_DECODER);
	TActivateMeta MatchingTransform;
	size_t MatchingTransformScore = 0;
	const auto HardwareScore = 1000;
	const auto InputScore = 10;
	const auto OutputScore = 1;

	auto AddMatch = [&](TActivateMeta Meta, Soy::TFourcc Input, Soy::TFourcc Output, int Score)
	{
		//	is this better than the current match?
		if (Score <= MatchingTransformScore)
			return;

		//	set the meta to only have one input/output format which is our preffered
		Meta.mInputs.Clear();
		Meta.mInputs.PushBack(Input);
		Meta.mOutputs.Clear();
		Meta.mOutputs.PushBack(Output);
		MatchingTransform = Meta;
		MatchingTransformScore = Score;
	};

	auto GetMatchingIndexes = [](BufferArray<Soy::TFourcc, 20>& List,const ArrayBridge<Soy::TFourcc>& MatchList)
	{
		BufferArray<int, 20> MatchingInputIndexes;
		for (auto i = 0; i < List.GetSize(); i++)
		{
			auto Index = List.FindIndex(MatchList[i]);
			if (Index == -1)
				continue;
			MatchingInputIndexes.PushBack(Index);
		}
		return MatchingInputIndexes;
	};

	auto GetLowestMatchingIndex = [&](BufferArray<Soy::TFourcc, 20>& List,const ArrayBridge<Soy::TFourcc>& MatchList)
	{
		auto MatchingIndexes = GetMatchingIndexes( List, MatchList );
		if (MatchingIndexes.IsEmpty())
			return -1;
		auto Lowest = MatchingIndexes[0];
		for (auto i = 1; i < MatchingIndexes.GetSize(); i++)
			Lowest = std::min(Lowest, MatchingIndexes[i]);
		return Lowest;
	};


	auto FindTransform = [&](TActivateMeta& Meta)
	{
		auto BestInputIndex = GetLowestMatchingIndex(Meta.mInputs, InputFilter);
		auto BestOutputIndex = GetLowestMatchingIndex(Meta.mOutputs, OutputFilter);
		//	not a match
		if (BestInputIndex==-1 || BestOutputIndex==-1)
			return;

		//	calc a score
		auto Score = 0;
		if (Meta.mHardwareAccelerated)
			Score += HardwareScore;
		Score += (InputFilter.GetSize() - BestInputIndex) * InputScore;
		Score += (OutputFilter.GetSize() - BestOutputIndex) * OutputScore;
		AddMatch(Meta, InputFilter[BestInputIndex], OutputFilter[BestOutputIndex], Score);
	};

	Transformers.EnumActivates(FindTransform);

	if (MatchingTransformScore == 0)
	{
		throw Soy::AssertException("No transformers matching the input/output filters");
	}

	return MatchingTransform;
}

MediaFoundation::TTransformer::TTransformer(const ArrayBridge<Soy::TFourcc>&& InputFormats, const ArrayBridge<Soy::TFourcc>&& OutputFormats)
{
	//	todo: support user-selected names
	auto Transform = GetBestTransform(MFT_CATEGORY_VIDEO_DECODER, InputFormats, OutputFormats );
	std::Debug << "Picked Transform " << Transform.mName << std::endl;

	//	activate a transformer
	{
		auto* Activate = Transform.mActivate.mObject;
		auto Result = Activate->ActivateObject(IID_PPV_ARGS(&mTransformer));
		IsOkay(Result, "Activate transform");
	}

	auto& Transformer = *mTransformer;

	//	grab attribs
	{
		IMFAttributes* Attributes = nullptr;
		auto Result = Transformer.GetAttributes(&Attributes);
		if (Attributes)
		{
			uint32_t HasAcceleration = 0;
			auto Result = Attributes->GetUINT32(CODECAPI_AVDecVideoAcceleration_H264, &HasAcceleration);
			if (Result == MF_E_ATTRIBUTENOTFOUND)
				std::Debug << "no CODECAPI_AVDecVideoAcceleration_H264 key" << std::endl;
			else
				std::Debug << "HasAcceleration = " << HasAcceleration << std::endl;
			Attributes->Release();
		}
	}

	/*
	
	In Windows 8, the H.264 decoder also supports the following attributes.

TABLE 4
Attribute	Description
CODECAPI_AVLowLatencyMode	Enables or disables low-latency decoding mode.
CODECAPI_AVDecNumWorkerThreads	Sets the number of worker threads used by the decoder.
CODECAPI_AVDecVideoMaxCodedWidth	Sets the maximum picture width that the decoder will accept as an input type.
CODECAPI_AVDecVideoMaxCodedHeight	Sets the maximum picture height that the decoder will accept as an input type.
MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT	Specifies the maximum number of output samples.
MFT_DECODER_EXPOSE_OUTPUT_TYPES_IN_NATIVE_ORDER	Specifies whether a decoder exposes IYUV/I420 output types (suitable for transcoding) before other formats.


	//	stream is always 0?
	auto StreamIndex = 0;

	IMFMediaType OutputFormatTypes[] =
	{
		MFVideoFormat_NV12,
		MFVideoFormat_YUY2,
		MFVideoFormat_YV12,
		MFVideoFormat_IYUV,
		MFVideoFormat_I420
	};

	//	try and set output type
	for (auto ot = 0; ot < std::size(OutputFormatTypes); ot++)
	{
		try
		{
			auto OutputFormat = OutputFormatTypes[ot];
			auto OutputFormatName = GetName(OutputFormat);
			auto Flags = 0;
			auto Result = Decoder->SetOutputType(StreamIndex, &OutputFormat, Flags);
			IsOkay(Result, std::string("SetOutputType ") + OutputFormatName);
			break;
		}
		catch (std::exception& e)
		{
			std::Debug << "Error setting output format " << e.what() << std::endl;
			continue;
		}
	}
	*/

	//	setup formats
	{
		IMFMediaType* InputMediaType = nullptr;
		auto Result = MFCreateMediaType(&InputMediaType);
		IsOkay(Result, "MFCreateMediaType");
		Result = InputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)");
		auto InputFormatGuid = GetGuid(Transform.mInputs[0]);
		Result = InputMediaType->SetGUID(MF_MT_SUBTYPE, InputFormatGuid);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_SUBTYPE)");
		Result = Transformer.SetInputType(0, InputMediaType, 0);
		IsOkay(Result, "SetInputType");
	}
	//	gr: this errors atm, but input status is still accepting
	/*
	{
		IMFMediaType* MediaType = nullptr;
		auto Result = MFCreateMediaType(&MediaType);
		IsOkay(Result, "MFCreateMediaType");
		Result = MediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Image)");
		//auto FormatGuid = GetGuid(Transform.mOutputs[0]);
		auto FormatGuid = MFVideoFormat_NV12;
		Result = MediaType->SetGUID(MF_MT_SUBTYPE, FormatGuid);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_SUBTYPE)");
		Result = Decoder.SetOutputType(0, MediaType, 0);
		IsOkay(Result, "SetOutputType");
	}
	*/
	{
		DWORD StatusFlags = 0;
		auto Result = Transformer.GetInputStatus(mInputStreamId, &StatusFlags);
		IsOkay(Result, "GetInputStatus");
		std::Debug << "Input status is " << StatusFlags << std::endl;

		auto CanAcceptData = (StatusFlags & MFT_INPUT_STATUS_ACCEPT_DATA) != 0;
		if (!CanAcceptData)
		{
			std::stringstream Error;
			Error << Transform.mName << " not ready for input data";
			throw Soy::AssertException(Error);
		}
	}

	/* gr: not implemented on all transforms
	{
		DWORD InputStreamIds[10];
		DWORD OutputStreamIds[10];
		auto Result = Decoder.GetStreamIDs(std::size(InputStreamIds), InputStreamIds, std::size(OutputStreamIds), OutputStreamIds);
		IsOkay(Result, "GetStreamIDs");
	}
	*/

	auto ProcessCommand = [&](MFT_MESSAGE_TYPE Message)
	{
		ULONG_PTR Param = 0;// nullptr;
		auto Result = Transformer.ProcessMessage(Message, Param);
		IsOkay(Result, std::string("ProcessMessage ") + std::string(magic_enum::enum_name(Message)));
	};

	//	gr: are these needed?
	ProcessCommand(MFT_MESSAGE_COMMAND_FLUSH);
	ProcessCommand(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING);
	ProcessCommand(MFT_MESSAGE_NOTIFY_START_OF_STREAM);
	/*	returns not implemented
	{
		auto Result = Decoder.AddInputStreams(1, &mStreamId);
		IsOkay(Result, "AddInputStream");
	}
	*/
}

MediaFoundation::TTransformer::~TTransformer()
{
	
}

void MediaFoundation::TTransformer::PushFrame(const ArrayBridge<uint8_t>&& Data)
{
	if (!mTransformer)
		throw Soy::AssertException("Decoder is null");

	auto& Transformer = *mTransformer;

	auto DebugInputStatus = [&]()
	{
		DWORD StatusFlags = 0;
		auto Result = Transformer.GetInputStatus(mInputStreamId, &StatusFlags);
		try
		{
			IsOkay(Result, "GetInputStatus");
			std::Debug << "Input status is " << StatusFlags << std::endl;
		}
		catch (std::exception& e)
		{
			std::Debug << e.what() << std::endl;
		}
	};

	auto DebugOutputStatus = [&]()
	{
		DWORD StatusFlags = 0;
		auto Result = Transformer.GetOutputStatus(&StatusFlags);
		try
		{
			IsOkay(Result, "GetOutputStatus");
			std::Debug << "Output status is " << StatusFlags << std::endl;
		}
		catch (std::exception& e)
		{
			std::Debug << e.what() << std::endl;
		}
	};

	Soy::AutoReleasePtr<IMFSample> pSample;
	{
		auto Result = MFCreateSample(&pSample.mObject);
		IsOkay(Result, "MFCreateSample");
		throw Soy::AssertException("Put data here");
	}

	DebugInputStatus();

	DWORD Flags = 0;
	auto Result = Transformer.ProcessInput(mInputStreamId, pSample.mObject, Flags);
	IsOkay(Result, "Decoder.ProcessInput");

	DebugInputStatus();
}

void MediaFoundation::TTransformer::SetOutputFormat()
{
	auto& Transformer = *mTransformer;

	//	gr: a manual one never seems to work
	if ( false )
	{
		IMFMediaType* MediaType = nullptr;
		auto Result = MFCreateMediaType(&MediaType);
		IsOkay(Result, "MFCreateMediaType");
		Result = MediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Image)");
		//auto FormatGuid = GetGuid(Transform.mOutputs[0]);
		auto FormatGuid = MFVideoFormat_NV12;
		Result = MediaType->SetGUID(MF_MT_SUBTYPE, FormatGuid);
		IsOkay(Result, "InputMediaType->SetGUID(MF_MT_SUBTYPE)");
		/*
		Result = MFSetAttributeRatio( MediaType, MF_MT_FRAME_RATE, 30, 1);
		IsOkay(Result, "MFSetAttributeRatio");
		Result = MFSetAttributeSize(MediaType,MF_MT_FRAME_SIZE, 640, 480);
		IsOkay(Result, "MFSetAttributeSize");
		*/
		//Result = Decoder.SetOutputType(0, MediaType, 0);
		//IsOkay(Result, "SetOutputType");
		Result = Transformer.SetOutputType(0, nullptr, 0);
		IsOkay(Result, "SetOutputType");
	}

	//	enum availiable formats and try each
	for ( auto i=0;	i<1000;	i++ )
	{
		Soy::AutoReleasePtr<IMFMediaType> pMediaType;
		auto Result = Transformer.GetOutputAvailableType(mOutputStreamId, i, &pMediaType.mObject);
		
		if (Result == MF_E_NO_MORE_TYPES)
			break;

		//	input hasn't been set
		if (Result == MF_E_TRANSFORM_TYPE_NOT_SET)
			IsOkay(Result, "GetOutputAvailableType");
		IsOkay(Result, "GetOutputAvailableType");
		
		GUID SubType;
		auto& MediaType = *pMediaType;
		Result = MediaType.GetGUID(MF_MT_SUBTYPE, &SubType);
		IsOkay(Result, "OutputFormat GetGuid Subtype");
		//	todo: is it a format we support?
		auto Fourcc = GetFourCC(SubType);

		//	set format
		DWORD Flags = 0;
		Result = Transformer.SetOutputType(mOutputStreamId, &MediaType, Flags );
		IsOkay(Result, "SetOutputType");
		return;
	}

	throw Soy::AssertException("Ran out of availible output formats");
}


void MediaFoundation::TTransformer::PopFrame(const ArrayBridge<uint8_t>&& Data)
{
	if (!mTransformer)
		throw Soy::AssertException("Transformer is null");

	auto& Transformer = *mTransformer;
	DWORD StatusFlags = 0;
	{
		auto Result = Transformer.GetOutputStatus(&StatusFlags);
		if (Result == MF_E_TRANSFORM_TYPE_NOT_SET)
		{
			SetOutputFormat();
			Result = Transformer.GetOutputStatus(&StatusFlags);
		}
		IsOkay(Result, "GetOutputStatus");
	}

	auto FrameReady = (StatusFlags & MFT_OUTPUT_STATUS_SAMPLE_READY) != 0;
	if (!FrameReady)
	{
		//return;
	}

	{
		MFT_OUTPUT_STREAM_INFO OutputInfo;
		auto Result = Transformer.GetOutputStreamInfo(mOutputStreamId, &OutputInfo);
		IsOkay(Result, "GetOutputStreamInfo");

		bool PreAllocatedSamples = (OutputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

		/*
		hr = mf->fptr_MFCreateSample(&output_sample);
		if (FAILED(hr))
		goto error;

		IMFMediaBuffer *output_media_buffer = NULL;
		DWORD allocation_size = output_info.cbSize;
		DWORD alignment = output_info.cbAlignment;
		if (alignment > 0)
		hr = mf->fptr_MFCreateAlignedMemoryBuffer(allocation_size, alignment - 1, &output_media_buffer);
		else
		hr = mf->fptr_MFCreateMemoryBuffer(allocation_size, &output_media_buffer);
		if (FAILED(hr))
		goto error;
*/
		IMFSample* OutputSample = nullptr;
		MFT_OUTPUT_DATA_BUFFER output_buffer = { mOutputStreamId, OutputSample, 0, nullptr };
		DWORD Flags = 0;
		DWORD Status = 0;
		Result = Transformer.ProcessOutput( Flags, 1, &output_buffer, &Status );

		//	not ready
		if (Result == MF_E_TRANSFORM_NEED_MORE_INPUT)
			return;
		IsOkay(Result, "ProcessOutput");
	}
	
	//	read a frame!
	std::Debug << "Output frame ready!" << std::endl;
}
