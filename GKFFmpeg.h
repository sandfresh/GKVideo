#pragma once
#include <thread>
#include <mutex>


struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVPacket;
struct AVRational;

class GKFFmpeg
{
public:

	enum { eNoLoop, eLoop, eLoopBidi };
	enum { eOpened, ePlaying, ePaused, eStopped, eEof, eError };
	enum { eForward = 1, eBackward = -1 };
	enum { eMajorVersion = 0, eMinorVersion = 1, ePatchVersion = 0 };

	GKFFmpeg(std::string);
	~GKFFmpeg();
	void init();

	bool open(std::string);

	void play();
	void pause();
	void stop();
	void close();
	void update();

private:
	void	initialProperty();
	bool	openVideoStream();
	bool	openAudioStream();

	bool	decodeFrame();
	bool	decodeVideoFrame(AVPacket* pAVPacket);
	bool	decodeAudioFrame(AVPacket* pAVPacket);
	bool	hasVideo();
	bool	hasAudio();
	bool	isImage();
	bool    decodeImage();
	bool	seekFrame(long lFrameNumber);
	bool	seekTime(double dTimeInMs);

	AVPacket* fetchAVPacket();
	unsigned int	getWidth();
	unsigned int	getHeight();
	long	calculateFrameNumberFromTime(long lTime);
	double  r2d(AVRational);
	void    retrieveFileInfo();
	void	retrieveVideoInfo();
	void	retrieveAudioInfo();
	typedef struct AudioData
	{
		int						m_iSampleRate;
		int						m_iChannels;
		int						m_iSamplesCount;
		long					m_lSizeInBytes;
		long					m_lPts;
		long					m_lDts;
		unsigned char*			m_pData;
	} AudioData;

	typedef struct VideoData
	{
		int						m_iWidth;
		int						m_iHeight;
		int						m_iChannels;
		long					m_lPts;
		long					m_lDts;
		unsigned char*			m_pData;
	} VideoData;

	typedef struct AVData
	{
		VideoData				m_VideoData;
		AudioData				m_AudioData;
	} AVData;

	AVFormatContext*		m_pFormatContext;
	AVCodecContext*			m_pVideoCodecContext;
	AVCodecContext*			m_pAudioCodecContext;
	SwsContext*				m_pSwScalingContext;
	AVFrame*				m_pVideoFrame;
	AVFrame*				m_pVideoFrameRGB;
	AVFrame*				m_pAudioFrame;
	AVData m_AVData;

	std::string				m_strFileName;
	std::string				m_strVideoCodecName;
	std::string				m_strAudioCodecName;
	unsigned char*			m_pVideoBuffer;
	double					m_dCurrentTimeInMs;
	double					m_dTargetTimeInMs;
	double					m_dDurationInMs;
	double					m_dFps;
	float					m_fSpeedMultiplier;			// default 1.0, no negative values
	unsigned long			m_lDurationInFrames;			// length in frames of file, or if cueIn and out are set frames between this range 
	long					m_lCurrentFrameNumber;		// current framePosition ( if cue positions are set e.g. startCueFrame = 10, currentframe at absolute pos 10 is set to 0 (range between 10 and 500 --> current frame 0 .. 490)
	unsigned long			m_lFramePosInPreLoadedFile;
	unsigned long			m_lCueInFrameNumber;   // default = 0
	unsigned long			m_lCueOutFrameNumber;  // default = maxNrOfFrames (end of file)
	int						m_iVideoStream;
	int						m_iAudioStream;
	int						m_iContentType;				// 0 .. video with audio, 1 .. just video, 2 .. just audio, 3 .. image	// 2RealEnumeration
	int						m_iBitrate;
	int						m_iDirection;
	int						m_iLoopMode;					// 0 .. once, 1 .. loop normal, 2 .. loop bidirectional, default is loop
	int						m_iState;
	bool					m_bIsInitialized;
	bool					m_bIsFileOpen;
	bool					m_bIsThreadRunning;
	std::thread			m_PlayerThread;
	std::mutex			m_Mutex;
	
	bool	isFrameDecoded;
};