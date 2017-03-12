#include "GKFFmpeg.h"  

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavutil/avutil.h" 
};
#define EPS 0.000025	// epsilon for checking unsual results as taken from OpenCV FFmeg player

GKFFmpeg::GKFFmpeg(std::string strUrlFileName)
{
	init();
	open(strUrlFileName);
}

GKFFmpeg::~GKFFmpeg()
{
	close();
}

void GKFFmpeg::init()
{
	avformat_network_init();
	av_register_all();
	av_log_set_level(AV_LOG_ERROR);
}

bool GKFFmpeg::open(std::string strFileName)
{
	if (avformat_open_input(&m_pFormatContext, strFileName.c_str(), NULL, NULL) != 0)
		return false; // couldn't open file

					  // Retrieve stream information
	if (av_find_stream_info(m_pFormatContext)<0)
		return false; // couldn't find stream information

					  // Find the first video stream
	m_iVideoStream = m_iAudioStream = -1;
	for (unsigned int i = 0; i<m_pFormatContext->nb_streams; i++)
	{
		if ((m_iVideoStream < 0) && (m_pFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO))
		{
			m_iVideoStream = i;
		}
		if ((m_iAudioStream < 0) && (m_pFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO))
		{
			m_iAudioStream = i;
		}
	}

	if (!(hasVideo() || hasAudio()))
		return false; // Didn't find video or audio stream

	if (hasVideo())
	{
		if (!openVideoStream())
			return false;
	}

	if (hasAudio())
	{
		if (!openAudioStream())
			return false;
	}

	// general file info equal for audio and video stream
	retrieveFileInfo();

	m_bIsFileOpen = true;
	m_strFileName = strFileName;

	// content is image, just decode once 
	if (isImage())
	{
		m_dDurationInMs = 0;
		m_dFps = 0;
		m_lCurrentFrameNumber = 1;
		decodeImage();
	}

	// start timer
	//m_OldTime = boost::chrono::system_clock::now();

	return m_bIsFileOpen;
}

void GKFFmpeg::play()
{ 
	if (!isImage())
	{
		m_iState = ePlaying; 
	}
}

void GKFFmpeg::pause()
{
	m_iState = ePaused;
}

void GKFFmpeg::stop()
{
	m_lCurrentFrameNumber = -1;	// set to invalid, as it is not decoded yet
	m_dTargetTimeInMs = 0;
	m_iState = eStopped;
	if (m_bIsFileOpen)
		seekFrame(0); // so unseekable files get reset too
}

void GKFFmpeg::close()
{
	stop();

	// Free the RGB image
	if (m_pVideoBuffer != nullptr)
	{
		delete m_pVideoBuffer;
		m_pVideoBuffer = nullptr;
	}

	if (m_pVideoFrameRGB != nullptr)
	{
		av_free(m_pVideoFrameRGB);
		m_pVideoFrameRGB = nullptr;
	}

	// Free the YUV frame
	if (m_pVideoFrame != nullptr)
	{
		av_free(m_pVideoFrame);
		m_pVideoFrame = nullptr;
	}

	if (m_pAudioFrame != nullptr)
	{
		av_free(m_pAudioFrame);
		m_pAudioFrame = nullptr;
	}


	if (m_pSwScalingContext != nullptr)
	{
		sws_freeContext(m_pSwScalingContext);
		m_pSwScalingContext = nullptr;
	}

	// Close the codecs
	if (m_pVideoCodecContext != nullptr)
	{
		avcodec_close(m_pVideoCodecContext);
		m_pVideoCodecContext = nullptr;
	}
	if (m_pAudioCodecContext != nullptr)
	{
		avcodec_close(m_pAudioCodecContext);
		m_pAudioCodecContext = nullptr;
	}

	// Close the file
	if (m_pFormatContext != nullptr)
	{
		//avformat_close_input(&m_pFormatContext);
		avformat_free_context(m_pFormatContext);	// this line should free all the associated mem with file, todo seriously check on lost mem blocks
		m_pFormatContext = nullptr;
	}
}

void GKFFmpeg::update()
{
	isFrameDecoded = false;
	static bool bIsSeekable = true;

	if (isImage())	// no update needed for already decoded image
		return;

	// update timer for correct video sync to fps
	//updateTimer();

	// make sure always to decode 0 frame first, even if the first delta time would suggest differently
	if (m_dCurrentTimeInMs<0)
		m_dTargetTimeInMs = 0;

	long lTargetFrame = m_lCurrentFrameNumber;//calculateFrameNumberFromTime(m_dTargetTimeInMs);
											  //if(lTargetFrame != m_lCurrentFrameNumber)
	{
		// probe to jump to target frame directly

		if (bIsSeekable)
		{
			bIsSeekable = seekFrame(lTargetFrame);
			isFrameDecoded = decodeFrame();
			if (!isFrameDecoded)
			{
				/*	seekFrame(0);
				bIsSeekable = false;*/
			}
		}

		if (!bIsSeekable)	// just forward decoding, this is a huge performance issue when playing backwards
		{
			/*if(lTargetFrame<=0)
			{
			seekFrame(0);
			m_lCurrentFrameNumber = m_dCurrentTimeInMs = 0;
			}*/

			if (m_iState == ePlaying)
			{
				isFrameDecoded = decodeFrame();
				m_dTargetTimeInMs = (float)(lTargetFrame) * 1.0 / m_dFps * 1000.0;

			}
		}

		m_lCurrentFrameNumber++;// = lTargetFrame;
		m_dCurrentTimeInMs = m_dTargetTimeInMs;
	}
}

void GKFFmpeg::initialProperty()
{
	m_bIsFileOpen = false;
	m_bIsThreadRunning = false;
	m_pFormatContext = nullptr;
	m_pVideoCodecContext = nullptr;
	m_pAudioCodecContext = nullptr;
	m_pSwScalingContext = nullptr;
	m_pVideoFrame = nullptr;
	m_pVideoFrameRGB = nullptr;
	m_pVideoBuffer = nullptr;
	m_pAudioFrame = nullptr;
}

void GKFFmpeg::retrieveFileInfo()
{
	m_iBitrate = m_pFormatContext->bit_rate / 1000.0;

	int iStream = m_iVideoStream;
	if (iStream<0)						// we just have an audio stream so seek in this stream
		iStream = m_iAudioStream;

	if (iStream >= 0)
	{
		m_dDurationInMs = m_pFormatContext->duration * 1000.0 / (float)AV_TIME_BASE;
		if (m_dDurationInMs / 1000.0 < EPS)
			m_dDurationInMs = m_pFormatContext->streams[iStream]->duration * r2d(m_pFormatContext->streams[iStream]->time_base) * 1000.0;
		m_dFps = r2d(m_pFormatContext->streams[iStream]->r_frame_rate);
		if (m_dFps < EPS)
			m_dFps = r2d(m_pFormatContext->streams[iStream]->avg_frame_rate);

		m_lDurationInFrames = m_pFormatContext->streams[iStream]->nb_frames;		// for some codec this return wrong numbers so calc from time
		if (m_lDurationInFrames == 0)
			m_lDurationInFrames = calculateFrameNumberFromTime(m_dDurationInMs);
	}
}
bool GKFFmpeg::openVideoStream()
{
	// Get a pointer to the codec context for the video stream
	m_pVideoCodecContext = m_pFormatContext->streams[m_iVideoStream]->codec;

	// Find the decoder for the video stream
	AVCodec* pCodec = avcodec_find_decoder(m_pVideoCodecContext->codec_id);	// guess this is deleted with the avcodec_close, that's what the docs say
	if (pCodec == NULL)
		return false; // Codec not found

					  // Open codec
	AVDictionary* options;
	if (avcodec_open2(m_pVideoCodecContext, pCodec, nullptr)<0)
		return false; // Could not open codec

					  // Allocate video frame
	m_pVideoFrame = avcodec_alloc_frame();

	// Allocate an AVFrame structure
	m_pVideoFrameRGB = avcodec_alloc_frame();
	if (m_pVideoFrameRGB == nullptr)
		return false;

	retrieveVideoInfo();

	// Determine required buffer size and allocate buffer
	m_pVideoBuffer = new uint8_t[avpicture_get_size(PIX_FMT_RGB24, getWidth(), getHeight())];

	// Assign appropriate parts of buffer to image planes in pFrameRGB
	avpicture_fill((AVPicture*)m_pVideoFrameRGB, m_pVideoBuffer, PIX_FMT_RGB24, getWidth(), getHeight());

	//Initialize Context
	m_pSwScalingContext = sws_getContext(getWidth(), getHeight(), m_pVideoCodecContext->pix_fmt, getWidth(), getHeight(), PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);

	return true;
}

bool GKFFmpeg::openAudioStream()
{
	// Get a pointer to the codec context for the video stream
	m_pAudioCodecContext = m_pFormatContext->streams[m_iAudioStream]->codec;

	// Find the decoder for the video stream
	AVCodec* pCodec = avcodec_find_decoder(m_pAudioCodecContext->codec_id);	// guess this is deleted with the avcodec_close, that's what the docs say
	if (pCodec == NULL)
		return false; // Codec not found

					  // Open codec
	AVDictionary* options;
	if (avcodec_open2(m_pAudioCodecContext, pCodec, nullptr)<0)
		return false; // Could not open codec

					  // Allocate video frame
	m_pAudioFrame = avcodec_alloc_frame();

	retrieveAudioInfo();

	return true;
}

void GKFFmpeg::retrieveVideoInfo()
{
	m_strVideoCodecName = std::string(m_pVideoCodecContext->codec->long_name);
	m_AVData.m_VideoData.m_iWidth = m_pVideoCodecContext->width;
	m_AVData.m_VideoData.m_iHeight = m_pVideoCodecContext->height;
}

void GKFFmpeg::retrieveAudioInfo()
{
	m_strAudioCodecName = std::string(m_pAudioCodecContext->codec->long_name);
	m_AVData.m_AudioData.m_iSampleRate = m_pAudioCodecContext->sample_rate;
	m_AVData.m_AudioData.m_iChannels = m_pAudioCodecContext->channels;
}

AVPacket* GKFFmpeg::fetchAVPacket()
{
	AVPacket *pAVPacket = nullptr;

	pAVPacket = new AVPacket();
	if (av_read_frame(m_pFormatContext, pAVPacket) >= 0)
		return pAVPacket;
	else
		return nullptr;
}

bool GKFFmpeg::decodeFrame()
{
	bool bRet = false;

	for (int i = 0; i<m_pFormatContext->nb_streams; i++)
	{
		AVPacket* pAVPacket = fetchAVPacket();

		if (pAVPacket != nullptr)
		{
			// Is this a packet from the video stream?
			if (pAVPacket->stream_index == m_iVideoStream)
			{
				bRet = false;
				bRet = decodeVideoFrame(pAVPacket);
			}
			else if (pAVPacket->stream_index == m_iAudioStream)
			{
				bRet = false;
				bRet = decodeAudioFrame(pAVPacket);
			}

			av_free_packet(pAVPacket);

			if (!bRet)
				return false;


		}
	}

	if (!bRet)
		printf("ficke");
	return bRet;
}

bool GKFFmpeg::decodeVideoFrame(AVPacket* pAVPacket)
{
	int isFrameDecoded = 0;

	// Decode video frame
	if (avcodec_decode_video2(m_pVideoCodecContext, m_pVideoFrame, &isFrameDecoded, pAVPacket)<0)
		return false;

	// Did we get a video frame?
	if (isFrameDecoded)
	{
		//Convert YUV->RGB
		sws_scale(m_pSwScalingContext, m_pVideoFrame->data, m_pVideoFrame->linesize, 0, getHeight(), m_pVideoFrameRGB->data, m_pVideoFrameRGB->linesize);
		m_AVData.m_VideoData.m_pData = m_pVideoFrameRGB->data[0];
		m_AVData.m_VideoData.m_lPts = m_pVideoFrame->pkt_pts;
		m_AVData.m_VideoData.m_lDts = m_pVideoFrame->pkt_dts;
		if (m_AVData.m_VideoData.m_lPts == AV_NOPTS_VALUE)
			m_AVData.m_VideoData.m_lPts = 0;
		if (m_AVData.m_VideoData.m_lDts == AV_NOPTS_VALUE)
			m_AVData.m_VideoData.m_lDts = 0;

		printf("video frame %d\n", m_AVData.m_VideoData.m_lPts);

		return true;
	}
	return false;
}

bool GKFFmpeg::decodeAudioFrame(AVPacket* pAVPacket)
{
	int isFrameDecoded = 0;

	if (avcodec_decode_audio4(m_pAudioCodecContext, m_pAudioFrame, &isFrameDecoded, pAVPacket)<0)
	{
		m_AVData.m_AudioData.m_pData = nullptr;
		return false;
	}
	m_AVData.m_AudioData.m_pData = m_pAudioFrame->data[0];
	m_AVData.m_AudioData.m_lSizeInBytes = av_samples_get_buffer_size(NULL, m_pAudioCodecContext->channels, m_pAudioFrame->nb_samples, m_pAudioCodecContext->sample_fmt, 1);	// 1 stands for don't align size
	m_AVData.m_AudioData.m_lPts = m_pAudioFrame->pkt_pts;
	m_AVData.m_AudioData.m_lDts = m_pAudioFrame->pkt_dts;
	if (m_AVData.m_AudioData.m_lPts == AV_NOPTS_VALUE)
		m_AVData.m_AudioData.m_lPts = 0;
	if (m_AVData.m_AudioData.m_lDts == AV_NOPTS_VALUE)
		m_AVData.m_AudioData.m_lDts = 0;

	printf("audio frame %d\n", m_AVData.m_AudioData.m_lPts);
	return true;
}

bool GKFFmpeg::hasVideo()
{
	return m_iVideoStream >= 0;
}

bool GKFFmpeg::hasAudio()
{
	return m_iAudioStream >= 0;
}

bool GKFFmpeg::isImage()
{
	return m_iBitrate <= 0 && m_AVData.m_AudioData.m_iSampleRate <= 0;
}

long GKFFmpeg::calculateFrameNumberFromTime(long lTime)
{
	long lTargetFrame = floor((double)lTime / 1000.0 * m_dFps);	//the 0.5 is taken from the opencv player, this might be useful for floating point rounding problems to be on the safe side not to miss one frame
	return lTargetFrame;
}

double GKFFmpeg::r2d(AVRational r)
{
	return r.num == 0 || r.den == 0 ? 0. : (double)r.num / (double)r.den;
}
 
bool GKFFmpeg::decodeImage()
{
	int isFrameDecoded = -1;

	AVPacket packet;
	// alloc img buffer
	FILE *imgFile = fopen(m_strFileName.c_str(), "rb");
	fseek(imgFile, 0, SEEK_END);
	long imgFileSize = ftell(imgFile);
	fseek(imgFile, 0, SEEK_SET);
	void *imgBuffer = malloc(imgFileSize);
	fread(imgBuffer, 1, imgFileSize, imgFile);
	fclose(imgFile);
	packet.data = (uint8_t*)imgBuffer;
	packet.size = imgFileSize;
	av_init_packet(&packet);
	//decode image
	avcodec_decode_video2(m_pVideoCodecContext, m_pVideoFrame, &isFrameDecoded, &packet);

	if (isFrameDecoded)	// Did we get a video frame? 
	{
		//Convert YUV->RGB
		sws_scale(m_pSwScalingContext, m_pVideoFrame->data, m_pVideoFrame->linesize, 0, getHeight(), m_pVideoFrameRGB->data, m_pVideoFrameRGB->linesize);
		av_free_packet(&packet);
		free(imgBuffer);			// we have to free this buffer separately don't ask me why, otherwise leak
		return true;
	}
	else
	{
		free(imgBuffer);
		av_free_packet(&packet);
		return false;
	}
}

bool GKFFmpeg::seekFrame(long lTargetFrameNumber)
{
	int iDirectionFlag = 0;
	if (m_iDirection == eBackward)
		iDirectionFlag = AVSEEK_FLAG_BACKWARD;

	int iStream = m_iVideoStream;
	if (iStream<0)						// we just have an audio stream so seek in this stream
		iStream = m_iAudioStream;

	if (iStream >= 0)
	{
		if (avformat_seek_file(m_pFormatContext, iStream, lTargetFrameNumber, lTargetFrameNumber, lTargetFrameNumber, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD) < 0)
		{
			return false;
		}
		if (m_pVideoCodecContext != nullptr)
			avcodec_flush_buffers(m_pVideoCodecContext);
		if (m_pAudioCodecContext != nullptr)
			avcodec_flush_buffers(m_pAudioCodecContext);

		return true;
	}
	return false;
}

bool GKFFmpeg::seekTime(double dTimeInMs)
{
	int iDirectionFlag = 0;
	if (m_iDirection == eBackward)
		iDirectionFlag = AVSEEK_FLAG_BACKWARD;

	int iStream = m_iVideoStream;
	if (iStream<0)						// we just have an audio stream so seek in this stream
		iStream = m_iAudioStream;

	if (iStream >= 0)
	{
		if (avformat_seek_file(m_pFormatContext, -1, dTimeInMs * 1000 - 2, dTimeInMs * 1000, dTimeInMs * 1000 + 2, AVSEEK_FLAG_ANY | iDirectionFlag) < 0)
		{
			return false;
		}
		if (m_pVideoCodecContext != nullptr)
			avcodec_flush_buffers(m_pVideoCodecContext);
		if (m_pAudioCodecContext != nullptr)
			avcodec_flush_buffers(m_pAudioCodecContext);

		return true;
	}
	return false;
	//return seekFrame( calculateFrameNumberFromTime(dTimeInMs) );
}

unsigned GKFFmpeg::getWidth()
{
	return m_AVData.m_VideoData.m_iWidth;
}

unsigned GKFFmpeg::getHeight()
{
	return m_AVData.m_VideoData.m_iHeight;
}
