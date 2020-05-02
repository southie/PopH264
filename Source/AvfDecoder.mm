#include "AvfDecoder.h"
#include "SoyPixels.h"
#include "SoyAvf.h"
#include "SoyFourcc.h"
#include "MagicEnum/include/magic_enum.hpp"
#include "json11.hpp"

#include <CoreMedia/CMBase.h>
#include <VideoToolbox/VTBase.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreMedia/CMFormatDescription.h>
#include <CoreMedia/CMTime.h>
#include <VideoToolbox/VTSession.h>
#include <VideoToolbox/VTCompressionProperties.h>
#include <VideoToolbox/VTCompressionSession.h>
#include <VideoToolbox/VTDecompressionSession.h>
#include <VideoToolbox/VTErrors.h>
#include "SoyH264.h"

#include "PopH264.h"	//	param keys



class Avf::TDecompressor
{
public:
	TDecompressor(const ArrayBridge<uint8_t>& Sps,const ArrayBridge<uint8_t>& Pps);
	~TCompressor();
	
	void								Decode(const ArrayBridge<uint8_t>&& Nalu,size_t FrameNumber);
	H264::NaluPrefixSize::Type			GetFormatNaluPrefixSize()	{	return H264::NaluPrefixSize::ThirtyTwo;	}
	
	CFPtr<VTDecompressionSessionRef>	mSession;
	CFPtr<CMFormatDescriptionRef>		mInputFormat;
};


void OnDecompress(void* DecompressionContext,void* SourceContext,OSStatus Status,VTDecodeInfoFlags Flags,CVImageBufferRef ImageBuffer,CMTime PresentationTimeStamp,CMTime PresentationDuration)
{
	if ( !DecompressionContext )
	{
		std::Debug << "OnDecompress missing context" << std::endl;
		return;
	}
	
	AvfMediaDecoder& Encoder = *reinterpret_cast<Avf::TDecompressor*>( DecompressionContext );
	SoyTime Timecode = Soy::Platform::GetTime( PresentationTimeStamp );
	
	try
	{
		Avf::IsOkay( Status, "OnDecompress" );
		Encoder.OnDecodedFrame( ImageBuffer, Timecode );
	}
	catch (std::exception& e)
	{
		Encoder.OnDecodeError( e.what(), Timecode );
	}
}

SoyPixelsMeta GetFormatDescriptionPixelMeta(CMFormatDescriptionRef Format)
{
	Boolean usePixelAspectRatio = false;
	Boolean useCleanAperture = false;
	auto Dim = CMVideoFormatDescriptionGetPresentationDimensions( Format, usePixelAspectRatio, useCleanAperture );
	Meta.mPixelMeta.DumbSetWidth( Dim.width );
	Meta.mPixelMeta.DumbSetHeight( Dim.height );

	return SoyPixelsMeta( Dim.width, Dim.height, SoyPixelsFormat::Invalid );
}


Avf::TDecompressor::TDecompressor(const ArrayBridge<uint8_t>& Sps,const ArrayBridge<uint8_t>& Pps)
{
	mInputFormat = Avf::GetFormatDescriptionH264( Sps, Pps, GetFormatNaluPrefixSize() );
		
	CFAllocatorRef Allocator = nil;
	Soy::Assert( mFormatDesc!=nullptr, "Format missing" );
	
	// Set the pixel attributes for the destination buffer
	CFMutableDictionaryRef destinationPixelBufferAttributes = CFDictionaryCreateMutable( &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
	
	auto FormatPixelMeta = GetFormatDescriptionPixelMeta( mInputFormat.mObject );
	
	SInt32 Width = size_cast<SInt32>( FormatPixelMeta.GetWidth() );
	SInt32 Height = size_cast<SInt32>( FormatPixelMeta.GetHeight() );
	
	CFDictionarySetValue(destinationPixelBufferAttributes,kCVPixelBufferWidthKey, CFNumberCreate(NULL, kCFNumberSInt32Type, &Width));
	CFDictionarySetValue(destinationPixelBufferAttributes, kCVPixelBufferHeightKey, CFNumberCreate(NULL, kCFNumberSInt32Type, &Height));
	
	bool OpenglCompatible = false;
	auto ForceNonPlanarOutput = false;
	CFDictionarySetValue(destinationPixelBufferAttributes, kCVPixelBufferOpenGLCompatibilityKey, OpenglCompatible ? kCFBooleanTrue : kCFBooleanFalse );
	
	OSType destinationPixelType = 0;
	
	if ( ForceNonPlanarOutput )
	{
		destinationPixelType = kCVPixelFormatType_32BGRA;
	}
	else
	{
#if defined(TARGET_IOS)
		//	to get ios to use an opengl texture, we need to explicitly set the format to RGBA.
		//	None (auto) creates a non-iosurface compatible texture
		if ( OpenglCompatible )
		{
			//destinationPixelType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
			//destinationPixelType = kCVPixelFormatType_24RGB;
			destinationPixelType = kCVPixelFormatType_32BGRA;
		}
		else	//	for CPU copies we prefer bi-planar as it comes out faster and we merge in shader. though costs TWO texture uploads...
#endif
		{
			//	favour bi-plane so we can merge with shader rather than have OS do it in CPU
			destinationPixelType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
			//destinationPixelType = kCVPixelFormatType_24RGB;
		}
	}
	
	if ( destinationPixelType != 0 )
	{
		CFDictionarySetValue(destinationPixelBufferAttributes,kCVPixelBufferPixelFormatTypeKey, CFNumberCreate(NULL, kCFNumberSInt32Type, &destinationPixelType));
	}
	
	// Set the Decoder Parameters
	CFMutableDictionaryRef decoderParameters = CFDictionaryCreateMutable( Allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
	
	static bool AllowDroppedFrames = false;
	CFDictionarySetValue(decoderParameters,kVTDecompressionPropertyKey_RealTime, AllowDroppedFrames ? kCFBooleanTrue : kCFBooleanFalse );
	
	const VTDecompressionOutputCallbackRecord callback = { OnDecompress, this };
	auto Result = VTDecompressionSessionCreate( Allocator, mFormatDesc->mDesc, decoderParameters, destinationPixelBufferAttributes, &callback, &mSession );
	
	CFRelease(destinationPixelBufferAttributes);
	CFRelease(decoderParameters);
	
	Avf::IsOkay( Result, "TDecompressionSessionCreate" );
	Soy::Assert( Session !=nullptr, "Failed to create decompression session");
}

Avf::TDecompressor::~TDecompressor()
{
	Flush();
	
	// End the session
	VTDecompressionSessionInvalidate( mSession );
	CFRelease( mSession );
	
	//	wait for the queue to end
}

H264::NaluPrefixSize::Type GetNaluPrefixSize()
{
	//	need length-byte-size to get proper h264 format
	int nal_size_field_bytes = 0;
	auto Result = CMVideoFormatDescriptionGetH264ParameterSetAtIndex( Desc, 0, nullptr, nullptr, nullptr, &nal_size_field_bytes );
	Avf::IsOkay( Result, "Get H264 param NAL size");
	if ( nal_size_field_bytes < 0 )
		nal_size_field_bytes = 0;
	auto Type = static_cast<H264::NaluPrefixSize::Type>(nal_size_field_bytes);
	return Type;
}

void ConvertNaluSize(ArrayBridge<uint8_t>& Nalu,H264::NaluPrefixSize::Type NaluSize)
{
	//	detect 001 or 0001 and convert
	Soy_AssertTodo();
}


CFPtr<CMSampleBufferRef> CreateSampleBuffer(const ArrayBridge<uint8_t>&& Data,size_t PresentationTime,size_t DecodeTime,size_t DurationMs)
{
	//	create buffer from packet
	CFAllocatorRef Allocator = nil;
	CFPtr<CMSampleBufferRef> SampleBuffer;
	
	uint32_t SubBlockSize = 0;
	CMBlockBufferFlags Flags = 0;
	CFPtr<CMBlockBufferRef> BlockBuffer;
	auto Result = CMBlockBufferCreateEmpty( Allocator, SubBlockSize, Flags, &BlockBuffer.mObject );
	Avf::IsOkay( Result, "CMBlockBufferCreateEmpty" );
		
	//	gr: when you pass memory to a block buffer, it only bloody frees it. make sure kCFAllocatorNull is the "allocator" for the data
	//		also means of course, for async decoding the data could go out of scope. May explain the wierd MACH__O error that came from the decoder?
	void* Data = (void*)AvccData.GetArray();
	auto DataSize = AvccData.GetDataSize();
	size_t Offset = 0;
		
	Result = CMBlockBufferAppendMemoryBlock( BlockBuffer.mObject,
											Data,
											DataSize,
											kCFAllocatorNull,
											nullptr,
											Offset,
											DataSize-Offset,
											Flags );
	Avf::IsOkay( Result, "CMBlockBufferAppendMemoryBlock" );
		
	/*
	//CMFormatDescriptionRef Format = GetFormatDescription( Packet.mMeta );
	//CMFormatDescriptionRef Format = Packet.mFormat->mDesc;
	auto Format = Packet.mFormat ? Packet.mFormat->mDesc : mFormatDesc->mDesc;
	if ( !VTDecompressionSessionCanAcceptFormatDescription( mSession->mSession, Format ) )
	{
		std::Debug << "VTDecompressionSessionCanAcceptFormatDescription failed" << std::endl;
		//	gr: maybe re-create session here with... new format? (save the packet's format to mFormatDesc?)
		//bool Dummy;
		//mOnStreamChanged.OnTriggered( Dummy );
	}
	*/
		
	int NumSamples = 1;
	BufferArray<size_t,1> SampleSizes;
	SampleSizes.PushBack( AvccData.GetDataSize() );
	BufferArray<CMSampleTimingInfo,1> SampleTimings;
	auto& FrameTiming = SampleTimings.PushBack();
	FrameTiming.duration = Soy::Platform::GetTime( DurationMs );
	FrameTiming.presentationTimeStamp = Soy::Platform::GetTime( PresentationTime );
	FrameTiming.decodeTimeStamp = Soy::Platform::GetTime( DecodeTime );
		
	Result = CMSampleBufferCreate(	Allocator,
								  BlockBuffer.mObject,
								  true,
								  nullptr,	//	callback
								  nullptr,	//	callback context
								  Format,
								  NumSamples,
								  SampleTimings.GetSize(),
								  SampleTimings.GetArray(),
								  SampleSizes.GetSize(),
								  SampleSizes.GetArray(),
								  &SampleBuffer.mObject );
	Avf::IsOkay( Result, "CMSampleBufferCreate" );
		
	//	sample buffer now has a reference to the block, so we dont want it
	//	gr: should now auto release
	//CFRelease( BlockBuffer );
	
	return SampleBuffer;
}

Avf::TDecompressor::Decode(ArrayBridge<uint8_t>&& Nalu, size_t FrameNumber)
{
	auto NaluSize = GetNaluPrefixSize();
	ConvertNaluSize( Nalu, NaluSize );
	
	auto DurationMs = 16;
	auto Sample = CreateSampleBuffer( Nalue, FrameNumber, FrameNumber, DurationMs );
	
	
	VTDecodeFrameFlags Flags = 0;
	VTDecodeInfoFlags FlagsOut = 0;
	
	//	gr: temporal means frames (may?) will be output in display order, OS will hold onto decoded frames
	bool OutputFramesInOrder = true;
	if ( OutputFramesInOrder )
		Flags |= kVTDecodeFrame_EnableTemporalProcessing;
	
	//	gr: async means frames may or may not be decoded in the background
	//	gr: also we may have issues with sample buffer lifetime in async
	bool AsyncDecompression = false;
	if ( AsyncDecompression )
		Flags |= kVTDecodeFrame_EnableAsynchronousDecompression;
	
	//	1x/low power mode means it WONT try and decode faster than 1x
	bool LowPowerDecoding = false;
	if ( LowPowerDecoding )
		Flags |= kVTDecodeFrame_1xRealTimePlayback;
	
	SoyTime DecodeDuration;
	auto OnFinished = [&DecodeDuration](SoyTime Timer)
	{
		DecodeDuration = Timer;
	};
	
	bool RecreateStream = false;
	{
		OnDecodeFrameSubmitted( Packet.mTimecode );
		
		//std::Debug << "decompressing " << Packet.mTimecode << "..." << std::endl;
		Soy::TScopeTimer Timer("VTDecompressionSessionDecodeFrame", 1, OnFinished, true );
		auto Result = VTDecompressionSessionDecodeFrame( mSession->mSession, SampleBuffer, Flags, nullptr, &FlagsOut );
		Timer.Stop();
		//std::Debug << "Decompress " << Packet.mTimecode << " took " << DecodeDuration << "; error=" << (int)Result << std::endl;
		Avf::IsOkay( Result, "VTDecompressionSessionDecodeFrame", false );
		
		static int FakeInvalidateSessionCounter = 0;
		static int FakeInvalidateSessionOnCount = -1;
		if ( ++FakeInvalidateSessionCounter == FakeInvalidateSessionOnCount )
		{
			FakeInvalidateSessionCounter = 0;
			Result = kVTInvalidSessionErr;
		}
		
		switch ( Result )
		{
				//	no error
			case 0:
				break;
				
				//	gr: if we pause for ages without eating a frame, we can get this...
				//		because for somereason the decoder thread is still trying to decode stuff??
			case MACH_RCV_TIMED_OUT:
				std::Debug << "Decompression MACH_RCV_TIMED_OUT..." << std::endl;
				break;
				
				//	gr: restoring iphone app sometimes gives us malfunction, sometimes invalid session.
				//		guessing invalid session is if it's been put to sleep properly or OS has removed some resources
			case kVTInvalidSessionErr:
			case kVTVideoDecoderMalfunctionErr:
			{
				//  gr: need to re-create session. Session dies when app sleeps and restores
				std::stringstream Error;
				Error << "Lost decompression session; " << Avf::GetString(Result);
				OnDecodeError( Error.str(), Packet.mTimecode );
				//	make errors visible for debugging
				//std::this_thread::sleep_for( std::chrono::milliseconds(1000));
				RecreateStream = true;
			}
				break;
				
				
			default:
			{
				static bool RecreateOnDecompressionError = false;
				std::Debug << "some decompression error; " << Avf::GetString(Result) << std::endl;
				if ( RecreateOnDecompressionError )
					RecreateStream = true;
			}
				break;
		}
		
		//	gr: do we NEED to make sure all referecnes are GONE here? as the data in block buffer is still in use if >1?
		//auto SampleCount = CFGetRetainCount( SampleBuffer );
		CFRelease( SampleBuffer );
		
		
		//	gr: hanging on destruction waiting for async frames, see if this makes it go away.
		if ( bool_cast(Flags & kVTDecodeFrame_EnableAsynchronousDecompression) )
			VTDecompressionSessionWaitForAsynchronousFrames( mSession->mSession );
	}
	
	if ( RecreateStream )
	{
		Soy::TScopeTimerPrint Timer("Recreating decompression session", 1);
		bool Dummy;
		mOnStreamChanged.OnTriggered(Dummy);
		
		//	gr: if packet was a keyframe, maybe return false to re-process the packet so immediate next frame is not scrambled
		if ( Packet.mIsKeyFrame )
		{
			std::Debug << "Returning keyframe back to buffer after session recreation" << std::endl;
			return false;
		}
	}
}

void Avf::TDecompressor::Flush()
{
	VTDecompressionSessionCompleteFrames( mSession, kCMTimeInvalid );
}


	
Avf::TDecoder::TDecoder()
{
}

Avf::TEncoder::~TEncoder()
{
	mDecompressor.reset();
}


void Avf::TDecoder::AllocDecoder()
{
	auto OnPacket = [this](std::shared_ptr<TPixelBuffer> pPixelBuffer,size_t FrameNumber)
	{
		std::Debug << "Decompressed pixel buffer " << FrameNumber << std::endl;
	};

	if ( mDecompressor )
		return;
	
	//	gr: does decompressor need to wait for SPS&PPS?
	mDecompressor.reset( new TDecompressor( mParams, OnPacket ) );
}

bool Avf::TDecoder::DecodeNextPacket(std::function<void(const SoyPixelsImpl&,SoyTime)> OnFrameDecoded)
{
	Array<uint8_t> Nalu;
	if ( !PopNalu( GetArrayBridge(Nalu) ) )
		return false;

	//	store latest sps & pps, need to cache these so we can create decoder
	auto H264PacketType = H264::GetPacketType(GetArrayBridge(Nalu));

	if ( H264PacketType == H264NaluContent::SequenceParameterSet )
	{
		mNaluSps = Nalu;
		return;
	}
	else if ( H264PacketType == H264NaluContent::PictureParameterSet )
	{
		mNaluPps = Nalu;
		return;
	}

	//	make sure we have a decoder
	AllocDecoder();
	
	//	no decompressor yet, drop packet
	if ( !mDecompressor )
	{
		std::Debug << "Dropping H264 frame (" << magic_enum::enum_name(H264PacketType) << ") as decompressor isn't ready (waiting for sps/pps)" << std::endl;
		return;
	}
	
	mDecompressor->Decode( GetArrayBridge(Nalu) );
}