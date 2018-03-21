//
// Created by Administrator on 2018/3/21.
//

#include "Timer.h"
#include "Demuxer.h"
#include "native_log.h"

Demuxer::Demuxer(VideoDecoder *videoDecoder, AudioDecoder *audioDecoder) {
    mVideoDecoder = videoDecoder;
    mAudioDecoder = audioDecoder;
    mMutex = MutexCreate();
    mCondition = CondCreate();
    mThread = NULL;
    mPrepared = false;
    mAbortRequest = true;
    mPaused = true;
    mLastPaused = true;
    mOpenSuccess = false;
    mSeekRequest = false;
    mReadFinish = false;
    fileName = NULL;
    pFormatCtx = NULL;
    mStartTime = AV_NOPTS_VALUE;
    av_register_all();
    avformat_network_init();
}

Demuxer::~Demuxer() {
    ThreadDestroy(mThread);
    MutexDestroy(mMutex);
    CondDestroy(mCondition);
    pFormatCtx = NULL;
    mAudioDecoder = NULL;
    mVideoDecoder = NULL;
    avformat_network_deinit();
    mAbortRequest = true;
    mPaused = true;
    mLastPaused = true;
    mOpenSuccess = false;
    mSeekRequest = false;
    mReadFinish = false;
}

/**
 * 设置开始时间
 * @param startTime
 */
void Demuxer::setStartTime(int64_t startTime) {
    mStartTime = startTime;
}

/**
 * 定位
 * @param pos
 */
void Demuxer::streamSeek(int64_t pos) {

    // TODO 定位
}

/**
 * 打开文件
 * @param filename
 * @return
 */
int Demuxer::open(const char *filename) {
    fileName = av_strdup(filename);
    if (!fileName) {
        ALOGE("Failed to find file name: %s", fileName);
        return -1;
    }

    // 创建解复用上下文
    pFormatCtx = avformat_alloc_context();
    if (!pFormatCtx) {
        ALOGE("Failed to create AVFormatContext\n");
        return -1;
    }

    // 打开文件
    if (avformat_open_input(&pFormatCtx, fileName, NULL, NULL) < 0) {
        ALOGE("Failed to open input file\n");
        return -1;
    }

    // 查找音视频流和索引
    int videoIndex = -1;
    int audioIndex = -1;
    for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIndex = i;
            if (mVideoDecoder != NULL) {
                AVStream *videoStream = pFormatCtx->streams[i];
                mVideoDecoder->setAVStream(videoStream, videoIndex);
            } else if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioIndex = i;
                if (mAudioDecoder != NULL) {
                    AVStream *audioStream = pFormatCtx->streams[i];
                    mAudioDecoder->setAVStream(audioStream, audioIndex);
                }
            }
        }
    }

    // 判断能否找得到媒体流
    if (videoIndex == -1 && audioIndex == -1) {
        ALOGE("Failed to find media stream\n");
        return -1;
    }

    // 打开媒体流
    int audioOpenned = -1, videoOpenned = -1;
    if (mAudioDecoder != NULL) {
        audioOpenned = mAudioDecoder->openStream();
    }
    if (mVideoDecoder != NULL) {
        videoOpenned = mVideoDecoder->openStream();
    }
    // 判断是否成功打开媒体流
    if (audioOpenned == -1 && videoOpenned == -1) {
        ALOGE("Failed to open media stream\n");
        return -1;
    }
    // 创建解复用线程
    mThread = ThreadCreate(demuxThread, this, "DemuxThread");
    if (!mThread) {
        ALOGE("Failed To Create Thread");
        return -1;
    }
    mOpenSuccess = true;
    return 0;
}

/**
 * 开始解复用
 */
void Demuxer::start() {
    if (!mPrepared) {
        mPrepared = true;
    }
    if (mOpenSuccess) {
        mAbortRequest = false;
        mPaused = false;
    }
}

/**
 * 停止解复用
 */
void Demuxer::stop() {
    mAbortRequest = true;
}

/**
 * 暂停解复用
 */
void Demuxer::paused() {
    mPaused = true;
}

/**
 * 解复用线程
 * @param arg
 * @return
 */
int Demuxer::demuxThread(void *arg) {
    Demuxer *demuxer = (Demuxer *) arg;
    demuxer->demux();
    return 0;
}

/**
 * 解复用
 */
void Demuxer::demux() {
    int ret = 0;

    // 还没开始
    while (!mPrepared) {
        continue;
    }

    // 定位到开始时间
    if (mStartTime != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = mStartTime;
        /* add the stream start time */
        if (pFormatCtx->start_time != AV_NOPTS_VALUE)
            timestamp += pFormatCtx->start_time;
        // 定位文件
        ret = avformat_seek_file(pFormatCtx, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   pFormatCtx->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    while (true) {
        // 停止
        if (mAbortRequest) {
            break;
        }

        // 停止状态是否发生变化
        if (mPaused && mPaused != mLastPaused) {
            mLastPaused = mPaused;
            if (mPaused) {
                av_read_pause(pFormatCtx);
            } else {
                av_read_play(pFormatCtx);
            }
        }

        // 定位请求
        if (mSeekRequest) {
            // 清空原来的数据
            if (mAudioDecoder != NULL) {
                mAudioDecoder->packetFlush();
            }
            if (mVideoDecoder != NULL) {
                mVideoDecoder->packetFlush();
            }

            // TODO 定位到最新的位置


            mReadFinish = false;
            mSeekRequest = false;
        }

        // 如果此时处于停止状态，则等待10毫秒再继续
        if (mPaused) {
            DelayMs(10);
            continue;
        }

        // 读取数据
        AVPacket *pkt = (AVPacket *) malloc(sizeof(AVPacket));
        ret = av_read_frame(pFormatCtx, pkt);
        if (ret < 0) {
            // 判断是否读到文件结尾了
            if (((ret == AVERROR_EOF) || avio_feof(pFormatCtx->pb)) && !mReadFinish) {
                mReadFinish = true;
            }
            // 如果是出错，则退出解复用线程
            if (pFormatCtx->pb && pFormatCtx->pb->error) {
                break;
            }
            // 如果仅仅是没有读到数据，则等待10毫秒继续
            MutexLock(mMutex);
            CondWaitTimeout(mCondition, mMutex, 10);
            MutexUnlock(mMutex);
            continue;
        } else {
            mReadFinish = false;
        }

        // 将裸数据包入队
        if (mAudioDecoder != NULL && pkt->stream_index == mAudioDecoder->getStreamIndex()) {
            mAudioDecoder->put(pkt);
        } else if (mVideoDecoder != NULL && pkt->stream_index == mVideoDecoder->getStreamIndex()) {
            mVideoDecoder->put(pkt);
        } else {
            av_packet_unref(pkt);
            pkt = NULL;
        }
        // 判断队列中的数量是否超过最大值，则进入等待状态
        bool aOver = (mAudioDecoder == NULL)
                     || (mAudioDecoder != NULL && mAudioDecoder->packetSize() > MAX_PACKET_COUNT);
        bool vOver = (mVideoDecoder == NULL)
                     || (mVideoDecoder != NULL && mVideoDecoder->packetSize() > MAX_PACKET_COUNT);
        if (aOver && vOver) {
            // 进入等待状态
            MutexLock(mMutex);
            // 该锁需要在解码器消耗裸数据，再调用Demuxer的notify方法来解除
            CondWait(mCondition, mMutex);
            MutexUnlock(mMutex);
        }
    }

    // 关闭解复用上下文
    avformat_close_input(&pFormatCtx);
    // 刷出剩余数据
    if (mVideoDecoder != NULL) {
        mVideoDecoder->packetFlush();
    }
    if (mAudioDecoder != NULL) {
        mAudioDecoder->packetFlush();
    }

    // TODO 发送数据通知解复用线程结束

}

/**
 * 通知线程发生变化，条件锁解锁
 */
void Demuxer::notify() {
    MutexLock(mMutex);
    CondSignal(mCondition);
    MutexUnlock(mMutex);
}

