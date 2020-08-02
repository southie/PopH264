#include "SoyPixels.h"


//	fake some linux stuff so we can compile on other platforms
#if !defined(TARGET_LINUX)
#include <stdint.h>
typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;
#define __user
typedef uint32_t __le32;

#include <linux/videodev2.h>
#include "Linux/include/linux/videodev2.h"
#include <linux/v4l2-controls.h>
#endif

//#include "nvidia/samples/01_video_encode/video_encode.h"
#include "NvidiaEncoder.h"
#include "nvidia/include/NvVideoEncoder.h"

#include "PopH264.h"
#include "json11.hpp"

//	gr: cleanup this
#define MICROSECOND_UNIT 1000000


namespace Nvidia
{
	void	IsOkay(int Result, const char* Context);
	void	Log(void *data, int i_level, const char *psz, va_list args);

	int		GetColourSpace(SoyPixelsFormat::Type Format);

	class TPacket;
}

void Nvidia::IsOkay(int Result, const char* Context)
{
	if(Result == 0)
		return;

	std::stringstream Error;
	Error << "nvidia error " << Result << " (" << Context << ")";
	throw Soy::AssertException(Error);
}


namespace V4lPixelFormat
{
	enum Type : uint32_t
	{
		//	gr: from nvidia error output;
		//		Only YUV420M, YUV444M and P010M are supported
		YUV420M = V4L2_PIX_FMT_YUV420M,
		NV12M = V4L2_PIX_FMT_NV12M,
		NV21M = V4L2_PIX_FMT_NV21M,
	};
}


//	to make compiling easier, we have a more native class
//	which references any linux/nvidia types
class Nvidia::TNative
{
public:
	void	OnFrameEncoded(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer,NvBuffer * shared_buffer);
	
public:
	v4l2_memory	mYuvMemoryMode = V4L2_MEMORY_MMAP;
	v4l2_memory	mH264MemoryMode = V4L2_MEMORY_MMAP;	//	demo only uses mmap
};


Nvidia::TEncoderParams::TEncoderParams(json11::Json& Options)
{
	auto SetInt = [&](const char* Name,size_t& ValueUnsigned)
	{
		auto& Handle = Options[Name];
		if ( !Handle.is_number() )
			return false;
		auto Value = Handle.int_value();
		if ( Value < 0 )
		{
			std::stringstream Error;
			Error << "Value for " << Name << " is " << Value << ", not expecting negative";
			throw Soy::AssertException(Error);
		}
		ValueUnsigned = Value;
		return true;
	};
	auto SetBool = [&](const char* Name, bool& Value)
	{
		auto& Handle = Options[Name];
		if (!Handle.is_bool())
			return false;
		Value = Handle.bool_value();
		return true;
	};

	
	SetInt(POPH264_ENCODER_KEY_PROFILELEVEL, mProfileLevel);
	SetBool(POPH264_ENCODER_KEY_VERBOSEDEBUG, mVerboseDebug);
	SetInt(POPH264_ENCODER_KEY_AVERAGEKBPS, mAverageKBitRate);
	SetInt(POPH264_ENCODER_KEY_MAXKBPS, mPeakKBitRate);
	SetBool(POPH264_ENCODER_KEY_REALTIME, mMaxPerformance);
	SetBool(POPH264_ENCODER_KEY_CONSTANTBITRATE, mConstantBitRate);
	//SetBool(POPH264_ENCODER_KEY_AVERAGEKBPS, mInsertSpsBeforeKeyframe);
	SetBool(POPH264_ENCODER_KEY_SLICELEVELENCODING, mSliceLevelEncoding);
}


//	gr: PopH264 multiple profile support is still todo
v4l2_mpeg_video_h264_profile GetProfile()
{
	return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
}

v4l2_mpeg_video_h264_level GetProfileLevel(size_t ProfileLevel)
{
	//	convert our levels (30=3.0 51=5.1 etc)
	std::map<size_t,v4l2_mpeg_video_h264_level> ProfileLevelValue =
	{
		{	10,	V4L2_MPEG_VIDEO_H264_LEVEL_1_0	},	//	V4L2_MPEG_VIDEO_H264_LEVEL_1B
		{	11,	V4L2_MPEG_VIDEO_H264_LEVEL_1_1	},
		{	12,	V4L2_MPEG_VIDEO_H264_LEVEL_1_2	},
		{	13,	V4L2_MPEG_VIDEO_H264_LEVEL_1_3	},
		{	20,	V4L2_MPEG_VIDEO_H264_LEVEL_2_0	},
		{	21,	V4L2_MPEG_VIDEO_H264_LEVEL_2_1	},
		{	22,	V4L2_MPEG_VIDEO_H264_LEVEL_2_2	},
		{	30,	V4L2_MPEG_VIDEO_H264_LEVEL_3_0	},
		{	31,	V4L2_MPEG_VIDEO_H264_LEVEL_3_1	},
		{	32,	V4L2_MPEG_VIDEO_H264_LEVEL_3_2	},
		{	40,	V4L2_MPEG_VIDEO_H264_LEVEL_4_0	},
		{	41,	V4L2_MPEG_VIDEO_H264_LEVEL_4_1	},
		{	42,	V4L2_MPEG_VIDEO_H264_LEVEL_4_2	},
		{	50,	V4L2_MPEG_VIDEO_H264_LEVEL_5_0	},
		{	51,	V4L2_MPEG_VIDEO_H264_LEVEL_5_1	},
	};
	return ProfileLevelValue[ProfileLevel];
}


V4lPixelFormat::Type GetPixelFormat(SoyPixelsFormat::Type Format)
{
	switch(Format)
	{
		case SoyPixelsFormat::Yuv_844:
			return V4lPixelFormat::YUV420M;

		case SoyPixelsFormat::Yuv_8_8_8:
			return V4lPixelFormat::YUV420M;

		case SoyPixelsFormat::Nv12:
			return V4lPixelFormat::NV12M;

		case SoyPixelsFormat::Nv21:
			return V4lPixelFormat::NV21M;

		default:break;
	}
	
	std::stringstream Error;
	Error << "No conversion from " << Format << " to v4l";
	throw Soy::AssertException(Error);
}



Nvidia::TEncoder::TEncoder(TEncoderParams& Params,std::function<void(PopH264::TPacket&)> OnOutPacket) :
	PopH264::TEncoder	( OnOutPacket ),
	mParams				( Params )
{
	log_level = LOG_LEVEL_DEBUG;

	mNative.reset(new TNative);
	
	//	the nvvideoencoder is a wrapper for a V4L2 device
	//	which is a standard linux file i/o stream
	//	alloc an encoder in blocking mode
	//	so the non-blocking mode is an IO mode
	//	gr: what is enc0 ? device name?
	//ctx.enc = NvVideoEncoder::createVideoEncoder("enc0", O_NONBLOCK);
	std::Debug << "Creating video encoder" << std::endl;
	mEncoder = NvVideoEncoder::createVideoEncoder("enc0");
	if ( !mEncoder )
		throw Soy::AssertException("Failed to allocate nvidia encoder");
	
	/*
	auto& Encoder = *mEncoder;
	auto Result = Encoder.subscribeEvent(V4L2_EVENT_EOS,0,0);
	IsOkay(Result,"Failed to subscribe to EOS event");
	*/
}

Nvidia::TEncoder::~TEncoder()
{
	Shutdown();
}

void Nvidia::TEncoder::InitEncoder(SoyPixelsMeta PixelMeta)
{
	if ( mInitialised )
		return;
	
	//	"capture" plane needs to be set before "output"
	InitH264Format(PixelMeta);
	InitYuvFormat(PixelMeta);

	//	need to set formats before memory
	InitH264MemoryMode();
	InitYuvMemoryMode();

	{
		auto& YuvPlane = GetYuvPlane();
		YuvPlane.setStreamStatus(true);
		auto& H264Plane = GetH264Plane();
		H264Plane.setStreamStatus(true);
	}
	
	InitH264Callback();
	InitYuvCallback();
	
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	mInitialised = true;
}

void Nvidia::TEncoder::InitYuvMemoryMode()
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& mMemoryMode = mNative->mYuvMemoryMode;

	auto& YuvPlane = GetYuvPlane();

	auto BufferCount = 6;

	//	input can be any mode
	switch(mMemoryMode)
    {
        case V4L2_MEMORY_MMAP:
		{
			auto Map = true;
			auto Allocate = false;
            auto Result = YuvPlane.setupPlane(V4L2_MEMORY_MMAP, BufferCount, Map, Allocate);
         	IsOkay(Result,"Setup YUV memory V4L2_MEMORY_MMAP");
		}
		break;

        case V4L2_MEMORY_USERPTR:
		{
            auto Map = false;
			auto Allocate = true;
            auto Result = YuvPlane.setupPlane(V4L2_MEMORY_USERPTR, BufferCount, Map, Allocate);
			IsOkay(Result,"Setup YUV memory V4L2_MEMORY_USERPTR");
		}
		break;

        case V4L2_MEMORY_DMABUF:
			InitDmaBuffers(BufferCount);
            break;

        default:
			throw Soy::AssertException("Invalid yuv memory mode");
    }

	//	buffers are now ready looking at the output
	//	so put them in the "ready to be used" queue
	//	gr: should I be calling "dqBuffer" here to request them all?
	//	gr: what if the buffer indexes aren't setup? 
	for ( auto i=0;	i<YuvPlane.getNumBuffers();	i++ )
	{
		PushYuvUnusedBufferIndex(i);
	}
}


void Nvidia::TEncoder::InitH264MemoryMode()
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& mMemoryMode = mNative->mH264MemoryMode;

	auto& H264Plane = GetH264Plane();

	//	demo is fewer than yuv
	auto BufferCount = 6;

	switch(mMemoryMode)
    {
        case V4L2_MEMORY_MMAP:
		{
			auto Map = true;
			auto Allocate = false;
            auto Result = H264Plane.setupPlane(V4L2_MEMORY_MMAP, BufferCount, Map, Allocate);
         	IsOkay(Result,"Setup H264 memory V4L2_MEMORY_MMAP");
		}
		break;

        case V4L2_MEMORY_USERPTR:
		{
            auto Map = false;
			auto Allocate = true;
            auto Result = H264Plane.setupPlane(V4L2_MEMORY_USERPTR, BufferCount, Map, Allocate);
			IsOkay(Result,"Setup H264 memory V4L2_MEMORY_USERPTR");
		}
		break;

        case V4L2_MEMORY_DMABUF:
			InitDmaBuffers(BufferCount);
            break;

        default:
			throw Soy::AssertException("Invalid yuv memory mode");
    }
}


void Nvidia::TEncoder::InitDmaBuffers(size_t BufferCount)
{
	Soy_AssertTodo();
	/*
	int ret=0;
	NvBufferCreateParams cParams;
	int fd;
	ret = ctx->enc->output_plane.reqbufs(V4L2_MEMORY_DMABUF,num_buffers);
	if(ret)
	{
		cerr << "reqbufs failed for output plane V4L2_MEMORY_DMABUF" << endl;
		return ret;
	}
	for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
	{
		cParams.width = ctx->width;
		cParams.height = ctx->height;
		cParams.layout = NvBufferLayout_Pitch;
		if (ctx->enableLossless && ctx->encoder_pixfmt == V4L2_PIX_FMT_H264)
		{
			cParams.colorFormat = NvBufferColorFormat_YUV444;
		}
		else if (ctx->profile == V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10)
		{
			cParams.colorFormat = NvBufferColorFormat_NV12_10LE;
		}
		else
		{
			cParams.colorFormat = ctx->enable_extended_colorformat ?
			NvBufferColorFormat_YUV420_ER : NvBufferColorFormat_YUV420;
		}
		cParams.nvbuf_tag = NvBufferTag_VIDEO_ENC;
		cParams.payloadType = NvBufferPayload_SurfArray;
		// Create output plane fd for DMABUF io-mode
		ret = NvBufferCreateEx(&fd, &cParams);
		if(ret < 0)
		{
			cerr << "Failed to create NvBuffer" << endl;
			return ret;
		}
		ctx->output_plane_fd[i]=fd;
	}
	return ret;
	 */
}

void Nvidia::TEncoder::InitYuvFormat(SoyPixelsMeta PixelMeta)
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	//	output plane = yuv input plane
	auto& YuvPlane = GetYuvPlane();
	auto& Encoder = *mEncoder;

	//	the input is a "capture" plane
	//SoyPixelsMeta PixelMeta(100,100,SoyPixelsFormat::Yuv_844);
	//	gr: only a limited set of formats, so force this for now
	//auto Format = GetPixelFormat( PixelMeta.GetFormat() );
	auto Format = V4L2_PIX_FMT_YUV420M;
	auto Width = PixelMeta.GetWidth();
	auto Height = PixelMeta.GetHeight();
	//auto MaxSize = InputFormat.GetDataSize();
	auto Result = Encoder.setOutputPlaneFormat( Format, Width, Height );
	IsOkay(Result,"InitInputFormat failed setOutputPlaneFormat");

	//	encoding params have to be done AFTER setting up input
	InitEncodingParams(mParams);
}


NvV4l2ElementPlane& Nvidia::TEncoder::GetYuvPlane()
{
	auto& Encoder = *mEncoder;
	auto& YuvPlane = Encoder.output_plane;
	return YuvPlane;
}

NvV4l2ElementPlane& Nvidia::TEncoder::GetH264Plane()
{
	auto& Encoder = *mEncoder;
	auto& H264Plane = Encoder.capture_plane;
	return H264Plane;
}


void Nvidia::TEncoder::InitEncodingParams(const TEncoderParams& Params)
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& Encoder = *mEncoder;

	{
		auto Profile = GetProfile();
		std::Debug << "SetProfile(" << magic_enum::enum_name(Profile) << ")" << std::endl;
		auto Result = Encoder.setProfile(Profile);
		IsOkay(Result,"setProfile");
	}
	
	{
		auto Level = GetProfileLevel(Params.mProfileLevel);
		std::Debug << "setLevel(" << Level << ")" << std::endl;
		auto Result = Encoder.setLevel(Level);
		IsOkay(Result,"setLevel");
	}
	
	auto AverageBitRate = Params.mAverageKBitRate * 1024;
	if ( AverageBitRate != 0 )
	{
		std::Debug << "setBitrate(" << AverageBitRate << ")" << std::endl;
		auto Result = Encoder.setBitrate(AverageBitRate);
		IsOkay(Result,"setBitrate");
	}
	
	auto MaxBitRate = Params.mPeakKBitRate * 1024;
	if ( MaxBitRate != 0 )
	{
		std::Debug << "setPeakBitrate(" << MaxBitRate << ")" << std::endl;
		auto Result = Encoder.setPeakBitrate(MaxBitRate);
		IsOkay(Result,"setBitrate");
	}
	
	//	ret = ctx.enc->setFrameRate(ctx.fps_n, ctx.fps_d);
	{
		std::Debug << "setSliceLevelEncode(" << Params.mSliceLevelEncoding << ")" << std::endl;
		auto Result = Encoder.setSliceLevelEncode(Params.mSliceLevelEncoding);
		IsOkay(Result,"setSliceLevelEncode");
	}
	
	{
		std::Debug << "setMaxPerfMode(" << Params.mMaxPerformance << ")" << std::endl;
		auto Result = Encoder.setMaxPerfMode(Params.mMaxPerformance);
		IsOkay(Result,"setMaxPerfMode");
	}
	
	{
		auto BitRateMode = Params.mConstantBitRate ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
		std::Debug << "setRateControlMode(" << magic_enum::enum_name(BitRateMode) << ")" << std::endl;
		auto Result = Encoder.setRateControlMode(BitRateMode);
		IsOkay(Result,"setRateControlMode");
	}

	{
		std::Debug << "setInsertSpsPpsAtIdrEnabled(" << Params.mInsertSpsBeforeKeyframe << ")" << std::endl;
		auto Result = Encoder.setInsertSpsPpsAtIdrEnabled(Params.mInsertSpsBeforeKeyframe);
		IsOkay(Result,"setInsertSpsPpsAtIdrEnabled");
	}
}


//	nvidia needs to know w/h
void Nvidia::TEncoder::InitH264Format(SoyPixelsMeta InputMeta)
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;

	//	capture plane = h264 output plane
	auto& H264Plane = GetH264Plane();
	auto& Encoder = *mEncoder;
	
	//	needs to be specified
	auto Width = InputMeta.GetWidth();
	auto Height = InputMeta.GetHeight();
	auto BufferSize = 1024 * 1024 * 2;
	auto Format = V4L2_PIX_FMT_H264;

	auto Result = Encoder.setCapturePlaneFormat( Format, Width, Height, BufferSize );
	IsOkay(Result,"InitOutputFormat failed setCapturePlaneFormat");
}

namespace V4L2BufferFlags
{
	enum Type : uint32_t
	{
		Invalid = 0xffffffff,	//	for soyenum
		MAPPED = V4L2_BUF_FLAG_MAPPED,
		QUEUED = V4L2_BUF_FLAG_QUEUED,
		DONE = V4L2_BUF_FLAG_DONE,
		KEYFRAME = V4L2_BUF_FLAG_KEYFRAME,
		PFRAME = V4L2_BUF_FLAG_PFRAME,
		BFRAME = V4L2_BUF_FLAG_BFRAME,
		ERROR = V4L2_BUF_FLAG_ERROR,
		//IN_REQUEST = V4L2_BUF_FLAG_IN_REQUEST,
		TIMECODE = V4L2_BUF_FLAG_TIMECODE,
		//M2M_HOLD_CAPTURE_BUF = V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF,
		PREPARED = V4L2_BUF_FLAG_PREPARED,
		NO_CACHE_INVALIDATE = V4L2_BUF_FLAG_NO_CACHE_INVALIDATE,
		NO_CACHE_CLEAN = V4L2_BUF_FLAG_NO_CACHE_CLEAN,
		TIMESTAMP_MONOTONIC = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
		TIMESTAMP_COPY = V4L2_BUF_FLAG_TIMESTAMP_COPY,
		TIMESTAMP_UNDOCUMENTED = 0x00008000,	//	timestamp mask is 0xe so there's another bit here
		TSTAMP_SRC_SOE = V4L2_BUF_FLAG_TSTAMP_SRC_SOE,
		//	V4L2_BUF_FLAG_TSTAMP_SRC_MASK		0x00070000
		//MAPPED = V4L2_BUF_FLAG_TSTAMP_SRC_EOF,	//	0
		LAST = V4L2_BUF_FLAG_LAST,
		//REQUEST_FD = V4L2_BUF_FLAG_REQUEST_FD,
	};
	DECLARE_SOYENUM(V4L2BufferFlags);

	static const auto AllEnums = {	MAPPED,QUEUED,DONE,KEYFRAME,PFRAME,BFRAME,ERROR,TIMECODE,PREPARED,NO_CACHE_INVALIDATE,NO_CACHE_CLEAN,TIMESTAMP_MONOTONIC,TIMESTAMP_COPY,TIMESTAMP_UNDOCUMENTED,TSTAMP_SRC_SOE,LAST };
}

std::string GetFlagsDebug(uint32_t Flags)
{
	std::stringstream Debug;
	Debug << "(0x" << std::hex << Flags << std::dec << ") ";
	//	gr: this isn't geting all enums :/
	auto AllEnums = V4L2BufferFlags::AllEnums;

	for ( auto e : AllEnums )
	{
		auto evalue = static_cast<uint32_t>(e);
		bool Present = (Flags & evalue)!=0;
		if ( !Present )
			continue;
		Debug << magic_enum::enum_name(e) << ",";
	}
	return Debug.str();
}

void Nvidia::TEncoder::OnFrameEncoded(ArrayBridge<uint8_t>&& FrameData,uint32_t Flags,std::chrono::milliseconds Timestamp)
{
	PopH264::TPacket OutputPacket;
	OutputPacket.mData.reset(new Array<uint8_t>());
	OutputPacket.mData->PushBackArray(FrameData);

	//	json meta
	OutputPacket.mInputMeta = std::string();
	OnOutputPacket(OutputPacket);
}

bool Nvidia::TEncoder::OnEncodedBuffer(v4l2_buffer& v4l2_buf, NvBuffer * buffer,NvBuffer * shared_buffer)
{
	//	gr: don't know when shared_buffer comes up. 
	if ( !buffer && shared_buffer )
	{
		std::Debug << "Warning: H264 dequeue callback, no buffer, using shared_buffer..." << std::endl;
		buffer = shared_buffer;
	}

	//	pull out the data in the planes
	//	we're only expecting one, but for the sake of completeness, iterate over them all
	auto PlaneCount = buffer->n_planes;
	auto Flags = v4l2_buf.flags;
	auto FlagsDebug = GetFlagsDebug(Flags);
	auto BufferIndex = buffer->index;
	auto TimestampMicro64 = (v4l2_buf.timestamp.tv_sec * MICROSECOND_UNIT) + v4l2_buf.timestamp.tv_usec ;
	auto TimestampMs = std::chrono::milliseconds( TimestampMicro64 / 1000 );
	std::Debug << "H264 plane dequeue (frame encoded). BufferIndex=" << BufferIndex << " PlaneCount=" << PlaneCount << " SharedBuffer=" << (shared_buffer ? "non-null":"null") << " flags=" << FlagsDebug << " Timestamp=" << TimestampMs.count() << std::endl;

	for ( auto p=0;	p<buffer->n_planes;	p++ )
	{
		auto& Plane = buffer->planes[p];
		auto Format = Plane.fmt;
		auto DataSize = Plane.bytesused;
		auto DataOffset = Plane.mem_offset;
		auto* Data = Plane.data + DataOffset;
		auto DataArray = GetRemoteArray(Data,DataSize);
		std::Debug << "Encoded plane; x" << DataSize << " bytes, flags=" << FlagsDebug << std::endl;
		OnFrameEncoded( GetArrayBridge(DataArray), Flags, TimestampMs );
	}

	auto EndOfStreamNoBytes = PlaneCount ? (buffer->planes[0].bytesused == 0) : false;
	auto EndOfStreamFlag = (Flags & V4L2BufferFlags::LAST)!=0;
	if ( EndOfStreamNoBytes || EndOfStreamFlag )
   	{
		std::Debug << "End of stream (0bytes=" << EndOfStreamNoBytes << " Flag="<< EndOfStreamFlag <<")" << std::endl;
		return false;
   	}

	//	gr: requeue this buffer index?
	auto& H264Plane = GetH264Plane();
	auto Result = H264Plane.qBuffer(v4l2_buf, nullptr);
	IsOkay(Result,"Re-qbuffer encoded h264 buffer");
		
	return true;
}

void Nvidia::TEncoder::InitH264Callback()
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;

	//	CFunc callback
	//	encoder_capture_plane_dq_callback
	auto EncoderCallback = [](struct v4l2_buffer *v4l2_buf, NvBuffer * buffer,
							  NvBuffer * shared_buffer, void * This)
	{
		if ( !This )
		{
			std::Debug << "Warning: H264 dequeue callback, missing This" << std::endl; 
			return false;
		}
		auto& ThisEncoder = *reinterpret_cast<Nvidia::TEncoder*>(This);
		try
		{
			if ( !v4l2_buf )
				throw Soy::AssertException("H264 dequeue callback; Missing v4l2_buf");
			auto Continue = ThisEncoder.OnEncodedBuffer( *v4l2_buf, buffer, shared_buffer );
			return Continue;
		}
		catch(std::exception& e)
		{
			std::Debug << "Exception handling encoded buffer: " << e.what() << std::endl;
		}
		return false;
	};
	
	auto& H264Plane = GetH264Plane();
	
	std::Debug << "starting H264 dequeue thread" << std::endl;
	auto* This = this;
	H264Plane.setDQThreadCallback(EncoderCallback);
	H264Plane.startDQThread(This);

	//	queue empty buffers for encoder to use
	std::Debug << "Queuing empty H264 buffers x" << H264Plane.getNumBuffers() << std::endl;
	for ( auto i=0;	i<H264Plane.getNumBuffers();	i++ )
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
		
		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		
		auto Result = H264Plane.qBuffer(v4l2_buf, nullptr);
		IsOkay(Result,"Failed to queue H264/capture_plane buffer");
	}
}



void Nvidia::TEncoder::InitYuvCallback()
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& YuvPlane = GetYuvPlane();
	
	//	CFunc callback
	//	encoder_capture_plane_dq_callback
	auto Callback = [](struct v4l2_buffer *v4l2_buf, NvBuffer * buffer,
							  NvBuffer * shared_buffer, void * This)
	{
		std::Debug << "YUV plane dequeue (index = " << v4l2_buf->index << ")" << std::endl;
		auto* ThisEncoder = reinterpret_cast<Nvidia::TEncoder*>(This);
		ThisEncoder->PushYuvUnusedBufferIndex(v4l2_buf->index);
		return true;
	};

	//	setup callback
	auto* This = this;
	YuvPlane.setDQThreadCallback(Callback);
	YuvPlane.startDQThread(This);

	
	/*
	//	Enqueue all the empty capture plane buffers.
	//	gr: which are the H264 output buffers
	for (uint32_t i = 0; i <Encoder.capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
		
		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		
		auto Result = Encoder.capture_plane.qBuffer(v4l2_buf, nullptr);
		IsOkay(Result,"Failed to queue H264/capture_plane buffer");
	}
	*/
}

/*
void Nvidia::TEncoder::OnFrameEncoded(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer,NvBuffer * shared_buffer)
{
	auto& Encoder = *mEncoder;

	//	gr: number of frames ready?
	uint32_t frame_num = Encoder.capture_plane.getTotalDequeuedBuffers() - 1;
	uint32_t ReconRef_Y_CRC = 0;
	uint32_t ReconRef_U_CRC = 0;
	uint32_t ReconRef_V_CRC = 0;
	static uint32_t num_encoded_frames = 1;
	struct v4l2_event ev;
	int ret = 0;
	
	if (!v4l2_buf)
	{
		std::Debug << __PRETTY_FUNCTION__ << " error while dequeing output buffer (v4l2_buf=null)" << std::endl;
		return;
	}
	
	if ( mUsingCommands )
	{
		if(v4l2_buf->flags & V4L2_BUF_FLAG_LAST)
		{
			std::Debug << __PRETTY_FUNCTION__ << "V4L2_BUF_FLAG_LAST" << std::endl;
			/*
			memset(&ev,0,sizeof(struct v4l2_event));
			ret = ctx->enc->dqEvent(ev,1000);
			if (ret < 0)
				cout << "Error in dqEvent" << endl;
			if(ev.type == V4L2_EVENT_EOS)
				return false;
			*//*
		}
	}
	
	//	end of string
	if (buffer->planes[0].bytesused == 0)
	{
		std::Debug << "Got 0 size buffer in capture" << std::endl;
		return;
	}
	
	//	Computing CRC with each frame
	//if(ctx->pBitStreamCrc)
	//	CalculateCrc (ctx->pBitStreamCrc, buffer->planes[0].data, buffer->planes[0].bytesused);
	

	//	push/read the encoded buffer
	OnFrame(buffer);

	//	get frame meta
	/*
	{
		v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
		auto Result = Encoder.getMetadata(v4l2_buf->index, enc_metadata);
		if ( Result == 0 )
		{
				cout << "Frame " << frame_num <<
				": isKeyFrame=" << (int) enc_metadata.KeyFrame <<
				" AvgQP=" << enc_metadata.AvgQP <<
				" MinQP=" << enc_metadata.FrameMinQP <<
				" MaxQP=" << enc_metadata.FrameMaxQP <<
				" EncodedBits=" << enc_metadata.EncodedFrameBits <<
				endl;
			}
		}
	}
	*/

	//	Get motion vector parameters of the frames from encoder
	/*
	{
		v4l2_ctrl_videoenc_outputbuf_metadata_MV enc_mv_metadata;
		if (ctx->enc->getMotionVectors(v4l2_buf->index, enc_mv_metadata) == 0)
		{
			uint32_t numMVs = enc_mv_metadata.bufSize / sizeof(MVInfo);
			MVInfo *pInfo = enc_mv_metadata.pMVInfo;
			
			cout << "Frame " << frame_num << ": Num MVs=" << numMVs << endl;
			
			for (uint32_t i = 0; i < numMVs; i++, pInfo++)
			{
				cout << i << ": mv_x=" << pInfo->mv_x <<
				" mv_y=" << pInfo->mv_y <<
				" weight=" << pInfo->weight <<
				endl;
			}
		}
	}
	*//*
	
	//	put the output/h264 buffer back so it can be used
	auto Result = Encoder.capture_plane.qBuffer(*v4l2_buf, nullptr);
	IsOkay(Result,"Post-new-buffer re-enqueuing buffer for encoder");
}
*/



void Nvidia::TEncoder::Start()
{
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& Encoder = *mEncoder;
	auto UseCommand = true;
	
	if ( UseCommand )
	{
		//	Send v4l2 command for encoder start
		auto Result = Encoder.setEncoderCommand(V4L2_ENC_CMD_START, 0);
		IsOkay(Result,"Failed to send V4L2_ENC_CMD_START");
	}
	else
	{
		//	start streaming
		auto Result = Encoder.output_plane.setStreamStatus(true);
		IsOkay(Result,"output_plane enable streaming");
		
		Result = Encoder.capture_plane.setStreamStatus(true);
		IsOkay(Result,"capture_plane enable streaming");
	}

}


void Nvidia::TEncoder::Sync()
{
	/*
	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	auto& mMemoryMode = mNative->mMemoryMode;
	/*
	if(mMemoryMode == V4L2_MEMORY_DMABUF || mMemoryMode == V4L2_MEMORY_MMAP)
	{
		for (uint32_t j = 0 ; j < buffer->n_planes; j++)
		{
			ret = NvBufferMemSyncForDevice (buffer->planes[j].fd, j, (void **)&buffer->planes[j].data);
			if (ret < 0)
			{
				cerr << "Error while NvBufferMemSyncForDevice at output plane for V4L2_MEMORY_DMABUF" << endl;
				abort(&ctx);
				goto cleanup;
			}
		}
	}
	
	if(mMemoryMode == V4L2_MEMORY_DMABUF)
	{
		for (uint32_t j = 0 ; j < buffer->n_planes ; j++)
		{
			v4l2_buf.m.planes[j].bytesused = buffer->planes[j].bytesused;
		}
	}
	 */
}

void Nvidia::TEncoder::ReadNextFrame()
{
	auto& Encoder = *mEncoder;
	
	//	dequeue all the planes from the capture
	/*
	 //	dequeue a buffer to write to
	 struct v4l2_buffer v4l2_buf;
	 struct v4l2_plane planes[MAX_PLANES];
	 NvBuffer *buffer = nullptr;
	 memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	 memset(planes, 0, sizeof(planes));
	 v4l2_buf.m.planes = planes;
	 auto Result = YuvPlane.dqBuffer(v4l2_buf, &buffer, NULL, 10);
	 IsOkay(Result,"Failed to dequeue YUV buffer");
	 */
}


void Nvidia::TEncoder::WaitForEnd()
{
	auto& Encoder = *mEncoder;
	/*
	//	if blocking mode
	//	Wait till capture plane DQ Thread finishes
	//	i.e. all the capture plane buffers are dequeued.
	bool EndOfStream = false;
	auto Result = encoder_proc_blocking(ctx, EndOfStream);
	IsOkay(Result,"encoder_proc_blocking");
	Encoder.capture_plane.waitForDQThread(-1);
	 */
	//	wait for threads to end
	auto& YuvPlane = GetYuvPlane();
	auto& H264Plane = GetH264Plane();

	std::Debug << "Stopping DQ threads..." << std::endl;

	//	gr: code says dont stop the thread, but I don't see how else to
	//		(broken pipe is running forever and not blocking)
	YuvPlane.stopDQThread();
	H264Plane.stopDQThread();

	//auto Timeout = -1;
	auto Timeout = 100;
	YuvPlane.waitForDQThread(Timeout);
	H264Plane.waitForDQThread(Timeout);

	std::Debug << "DQ thread ended" << std::endl;
}

/*
int
read_video_frame(std::ifstream * stream, NvBuffer & buffer)
{
	uint32_t i, j;
	char *data;
	
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		std::streamsize bytes_to_read =
		plane.fmt.bytesperpixel * plane.fmt.width;
		data = (char *) plane.data;
		plane.bytesused = 0;
		for (j = 0; j < plane.fmt.height; j++)
		{
			stream->read(data, bytes_to_read);
			if (stream->gcount() < bytes_to_read)
				return -1;
			data += plane.fmt.stride;
		}
		plane.bytesused = plane.fmt.stride * plane.fmt.height;
	}
	return 0;
}

int
write_video_frame(std::ofstream * stream, NvBuffer &buffer)
{
	uint32_t i, j;
	char *data;
	
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		size_t bytes_to_write =
		plane.fmt.bytesperpixel * plane.fmt.width;
		
		data = (char *) plane.data;
		for (j = 0; j < plane.fmt.height; j++)
		{
			stream->write(data, bytes_to_write);
			if (!stream->good())
				return -1;
			data += plane.fmt.stride;
		}
	}
	return 0;
}
*/
void Nvidia::TEncoder::Shutdown()
{
	WaitForEnd();
/*
	if(ctx.b_use_enc_cmd)
	{
		//	Send v4l2 command for encoder stop
		ret = ctx.enc->setEncoderCommand(V4L2_ENC_CMD_STOP, 1);
		eos = true;
		break;
	}
	else
	{
		eos = true;
		v4l2_buf.m.planes[0].m.userptr = 0;
		v4l2_buf.m.planes[0].bytesused = v4l2_buf.m.planes[1].bytesused = v4l2_buf.m.planes[2].bytesused = 0;
	}
 */
}


void Nvidia::TEncoder::FinishEncoding()
{
	
}



void Nvidia::TEncoder::Encode(const SoyPixelsImpl& Pixels,const std::string& Meta,bool Keyframe)
{
	//	gotta be in yuv_8_8_8
	auto* pSrcPixels = &Pixels;
	SoyPixels Yuv_8_8_8_TempPixels;
	if ( Pixels.GetFormat() != SoyPixelsFormat::Yuv_8_8_8 )
	{
		pSrcPixels = &Yuv_8_8_8_TempPixels;
		Yuv_8_8_8_TempPixels.Copy( Pixels );
		Yuv_8_8_8_TempPixels.SetFormat(SoyPixelsFormat::Yuv_8_8_8);
	}
	auto& SrcPixels = *pSrcPixels;

	//	figure out our input
	BufferArray<std::shared_ptr<SoyPixelsImpl>,MAX_PLANES> SrcPlanes;
	SrcPixels.SplitPlanes(GetArrayBridge(SrcPlanes));

	Encode( *SrcPlanes[0], *SrcPlanes[1], *SrcPlanes[2], Meta, Keyframe );
}


uint32_t Nvidia::TEncoder::PopYuvUnusedBufferIndex()
{
	while(IsRunning())
	{
		//	try and get next
		{
			std::lock_guard<std::mutex> Lock(mYuvBufferLock);
			if ( !mYuvBufferIndexesUnused.IsEmpty() )
			{
				auto Popped = GetArrayBridge(mYuvBufferIndexesUnused).PopAt(0);
				return Popped;
			}
		}

		//	none availible, wait for notification that there is one
		//	gr: would be nice to turn this into a promise!
		std::Debug << __PRETTY_FUNCTION__ << " waiting for semaphore for unused buffer to be availible..." << std::endl;
		mYuvBufferSemaphore.WaitAndReset(__PRETTY_FUNCTION__);
	}

	throw Soy::AssertException("Failed to get unused yuv buffer index");
}

void Nvidia::TEncoder::PushYuvUnusedBufferIndex(uint32_t Index)
{
	std::Debug << __PRETTY_FUNCTION__ << "(" << Index << ")" << std::endl;
	//	add to list and notify
	{
		std::lock_guard<std::mutex> Lock(mYuvBufferLock);
		if ( mYuvBufferIndexesUnused.Find(Index) )
			std::Debug << __PRETTY_FUNCTION__ << " warning: yuvbuffer list already contains " << Index << std::endl;
		mYuvBufferIndexesUnused.PushBackUnique(Index);
	}

	//	notify anything waiting on this
	mYuvBufferSemaphore.OnCompleted();
}


//	this function gets the next buffer, calls the callback, then queues for us
//	it will block until there is a buffer ready (encode() is expected to block and be called on your own thread)
void Nvidia::TEncoder::QueueNextYuvBuffer(std::function<void(NvBuffer&)> FillBuffer)
{
	auto& Encoder = *mEncoder;
	auto& YuvPlane = GetYuvPlane();

	//	gr; get a free buffer... need to block here until one is availible
	auto BufferIndex = PopYuvUnusedBufferIndex();
	auto& Buffer = *YuvPlane.getNthBuffer(BufferIndex);

	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
	v4l2_buf.index = BufferIndex;
	//	gr: this is a pointer, so when will it go out of scope?
	//		is it copied in qBuffer? (I think it is)
	v4l2_buf.m.planes = planes;

	//	gr: can we set iframe here?
	//	gr: set buffer meta here?
	v4l2_buf.flags = 0;	//	no flags = eof, so what should this be?
	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;

	static int timestamp = 0;
	timestamp += 33;
    v4l2_buf.timestamp.tv_sec = timestamp / (MICROSECOND_UNIT);
    v4l2_buf.timestamp.tv_usec = timestamp % (MICROSECOND_UNIT);

	auto& mMemoryMode = mNative->mYuvMemoryMode;
	
	if ( mMemoryMode == V4L2_MEMORY_DMABUF)
	{
		throw Soy::AssertException("todo: DMA map file mode");
		/*
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		//	Map output plane buffer for memory type DMABUF.
		auto Result = YuvPlane.mapOutputBuffers(v4l2_buf, ctx.output_plane_fd[i]);
		IsOkay(Result,"Error while mapping buffer at output plane");
		*/
	}
	std::Debug << "FillBuffer(" << BufferIndex << ")..." << std::endl;
	//	gr: bytesused in the planes gets set in dma mode
	FillBuffer(Buffer);

	//	we're filling nvbuffer data, but we send v4l2_buf meta, so need to update it
	//	gr: maybe provide a copy-plane ArrayBridge or something to FillBuffer
	//	gr: this should be done in fillbuffer? but will always be the same
	for ( auto p=0;	p<Buffer.n_planes; p++)
	{
		NvBuffer::NvBufferPlane& BufferPlane = Buffer.planes[p];
		auto& V4lPlane = planes[p];
		//	gr: when dequeued the nv buffer bytesused is 0 so FillBuffer() may not have filled it
		if ( BufferPlane.bytesused == 0 )
		{
			auto DefaultBytesUsed = BufferPlane.fmt.sizeimage;
			std::Debug << "Post FillBuffer buffer plane bytesused=" << BufferPlane.bytesused << " defaulting to full size " << DefaultBytesUsed << std::endl;
			BufferPlane.bytesused = DefaultBytesUsed;
		}
		V4lPlane.bytesused = BufferPlane.bytesused;
	}

	//	if DMA or MMAP need to sync
	Sync();
	//	DMA also needs to set bytes used

	std::Debug << "Queuing YUV buffer " << BufferIndex << std::endl;
	//	final queue
	auto Result = YuvPlane.qBuffer(v4l2_buf, nullptr);
	IsOkay(Result,"Error while queueing buffer at output plane");
}

void Nvidia::TEncoder::Encode(const SoyPixelsImpl& Luma, const SoyPixelsImpl& ChromaU, const SoyPixelsImpl& ChromaV, const std::string& Meta, bool Keyframe)
{
	//std::Debug << "Skipping encode" << std::endl;
	//return;

	std::Debug << __PRETTY_FUNCTION__ << std::endl;
	SoyPixelsMeta PixelsMeta( Luma.GetWidth(), Luma.GetHeight(), SoyPixelsFormat::Yuv_8_8_8 );
	InitEncoder( PixelsMeta);

	auto FillBuffer = [&](NvBuffer& Buffer)
	{
		BufferArray<const SoyPixelsImpl*,3> SrcPlanes;
		SrcPlanes.PushBack(&Luma);
		SrcPlanes.PushBack(&ChromaU);
		SrcPlanes.PushBack(&ChromaV);

		std::Debug << "Filling YUV buffer x" << Buffer.n_planes << "planes..." << std::endl;
		for ( auto p=0;	p<Buffer.n_planes; p++)
		{
			NvBuffer::NvBufferPlane& DstPlane = Buffer.planes[p];
			//	gr: 640x480, but stride is 768???
			std::Debug << "Fill buffer plane; " << p << "; bpp=" << DstPlane.fmt.bytesperpixel << " width=" << DstPlane.fmt.width << " height=" << DstPlane.fmt.height << " stride=" << DstPlane.fmt.stride << " sizeimage=" << DstPlane.fmt.sizeimage << std::endl;

			if ( p >= SrcPlanes.GetSize() )
			{
				std::Debug << "no src plane(x" << SrcPlanes.GetSize() << ") for dst plane " << p << std::endl;
				continue;
			}
			auto& SrcPlane = *SrcPlanes[p];

			//	gr: it seems output is a bit messed up, so maybe stride is important
			//	to make things a little simpler, make a destination image with stride width
			if ( DstPlane.fmt.bytesperpixel != 1 )
				throw Soy::AssertException("Currently can only handle 1BPP planes");
			auto* DstPlaneData = DstPlane.data + DstPlane.mem_offset;
			//	todo: check real buffer size to avoid bad memread
			auto DstPlaneDataSize = DstPlane.fmt.stride * DstPlane.fmt.height;	
			SoyPixelsRemote DstPlanePixels( DstPlaneData, DstPlane.fmt.stride, DstPlane.fmt.height, DstPlaneDataSize, SoyPixelsFormat::Greyscale );

			auto Height = std::min<uint32_t>(DstPlane.fmt.height,SrcPlane.GetHeight());
			auto Width = std::min<uint32_t>(DstPlane.fmt.width,SrcPlane.GetWidth());
			for ( int y=0;	y<Height;	y++ )
			{
				auto DstLine = GetRemoteArray( &DstPlanePixels.GetPixelPtr(0,y,0), Width );
				auto SrcLine = GetRemoteArray( &SrcPlane.GetPixelPtr(0,y,0), Width );
				DstLine.Copy(SrcLine);
			}
			DstPlane.bytesused = DstPlaneDataSize;
		}
	};

	QueueNextYuvBuffer(FillBuffer);
}


