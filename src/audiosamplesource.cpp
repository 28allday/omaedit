#include "audiosamplesource.h"
#include "renderrequest.h"
#include <QCoreApplication>
#include <functional>
#include <QFile>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <climits>
#include <atomic>
#include <cstring>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {
struct Decoder {
    AVFormatContext *file = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *resample = nullptr;
    SwrContext *linearConvert = nullptr;
    // A stretch of an AAC stream as float samples at its own rate, on a fixed
    // grid of 16 seconds: `samples` of it hold sound (fewer than the grid's
    // only where the stream ends inside it).
    struct Chunk {
        qint64 index = -1, samples = 0;
        bool ended = false;
        quint64 used = 0;
        QVector<float> pcm;
    };
    Chunk chunks[2];
    quint64 chunkClock = 0;
    // Where the stream's first decoded frame ends: every later frame of an
    // unbroken stream starts a whole number of frames after it.
    qint64 frameGrid = 0;
    bool frameGridKnown = false, frameGridSought = false;
    bool linear = false;
    // The stream's rate and channel count as the decoder first gave them: an
    // AAC chunk's decoder replaces `codec`, and these are read from other
    // threads meanwhile.
    int sourceRate = 0, sourceChannels = 0;
    int stream = -1, cacheRate = 0, cacheChannels = 0;
    qint64 cacheFirst = 0;
    qint64 originSample = 0, audioEnd = 0, timestampTolerance = 0;
    bool knownAudioEnd = false;
    QVector<int> channelMap;
    double duration = 0, fps = 0;
    bool reverse = false;
    qint64 cacheByteLimit = 0;
    QVector<float> cache;
    std::mutex mutex;
    QString error;
    ~Decoder() {
        swr_free(&resample); swr_free(&linearConvert); av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&file);
    }
    bool fail(const char *text) { error = QCoreApplication::translate("AudioSampleSource",text); return false; }
    bool load(const QString &path, const AudioSampleSource::Options &options) {
        const auto name = QFile::encodeName(path);
        cacheByteLimit=options.cacheByteLimit;
        if(cacheByteLimit<0)return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The decoded sound cache limit is invalid."));
        file=avformat_alloc_context();
        if(!file)return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare the source sound decoder."));
        file->interrupt_callback={[](void *) {return RenderScope::cancelled()?1:0;},nullptr};
        if (avformat_open_input(&file,name.constData(),nullptr,nullptr) < 0 || avformat_find_stream_info(file,nullptr) < 0)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not open the source sound."));
        const AVCodec *decoder = nullptr;
        if (options.streamIndex < -1 || options.videoStreamIndex < -1)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The selected source stream is invalid."));
        stream = av_find_best_stream(file,AVMEDIA_TYPE_AUDIO,options.streamIndex,-1,&decoder,0);
        if (options.streamIndex >= 0 && stream != options.streamIndex)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The selected source stream is invalid."));
        if (stream < 0 || !decoder) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source has no decodable sound stream."));
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec,file->streams[stream]->codecpar) < 0)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare the source sound decoder."));
        // The decoder adjusts frame timestamps when skipping encoder priming
        // or trailing samples. Those packet timestamps need their real clock.
        codec->pkt_timebase = file->streams[stream]->time_base;
        if (avcodec_open2(codec,decoder,nullptr) < 0)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare the source sound decoder."));
        if (codec->sample_rate<=0 || codec->sample_rate>768000 || codec->ch_layout.nb_channels<=0 || codec->ch_layout.nb_channels>64)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound duration or format is unavailable."));
        sourceRate = codec->sample_rate; sourceChannels = codec->ch_layout.nb_channels;
        const auto *track = file->streams[stream];
        const int video = av_find_best_stream(file,AVMEDIA_TYPE_VIDEO,options.videoStreamIndex,-1,nullptr,0);
        if (options.videoStreamIndex >= 0 && video != options.videoStreamIndex)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The selected source stream is invalid."));
        // Switching audio streams must retain one common source clock, including
        // multi-stream field recordings with no picture.
        const int clockIndex = video >= 0 ? video : av_find_best_stream(file,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        const auto *clock = file->streams[clockIndex];
        const qint64 origin = clock->start_time == AV_NOPTS_VALUE ? 0 : clock->start_time;
        originSample = av_rescale_q(origin,clock->time_base,{1,codec->sample_rate});
        const double originSeconds = origin*av_q2d(clock->time_base);
        knownAudioEnd = track->duration != AV_NOPTS_VALUE;
        const double soundEnd = knownAudioEnd ? ((track->start_time == AV_NOPTS_VALUE ? 0 : track->start_time)
            +track->duration)*av_q2d(track->time_base)-originSeconds : 0.;
        audioEnd = std::llround(soundEnd*codec->sample_rate);
        duration = soundEnd;
        if (clock->duration != AV_NOPTS_VALUE)
            duration = std::max(duration,clock->duration*av_q2d(clock->time_base));
        if (video<0) for (unsigned i=0;i<file->nb_streams;++i) {
            const auto *other = file->streams[i];
            if (other->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && other->duration != AV_NOPTS_VALUE)
                duration = std::max(duration,((other->start_time == AV_NOPTS_VALUE ? 0 : other->start_time)
                    +other->duration)*av_q2d(other->time_base)-originSeconds);
        }
        if ((!knownAudioEnd || (video >= 0 && clock->duration == AV_NOPTS_VALUE)) && file->duration != AV_NOPTS_VALUE)
            duration = std::max(duration,double(file->duration)/AV_TIME_BASE
                +(file->start_time == AV_NOPTS_VALUE ? 0. : double(file->start_time)/AV_TIME_BASE)-originSeconds);
        if (!std::isfinite(duration) || duration <= 0 || duration > double(LLONG_MAX)/768000.-1.
            || codec->sample_rate <= 0 || codec->ch_layout.nb_channels <= 0)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound duration or format is unavailable."));
        // Coarse container timestamps may round an otherwise continuous sample
        // boundary by half a clock tick. Do not turn that rounding into gaps.
        timestampTolerance = std::floor(.5*av_q2d(track->time_base)*codec->sample_rate);
        channelMap = options.channels;
        if (channelMap.size()>64 || std::any_of(channelMap.cbegin(),channelMap.cend(),[&](int ch) { return ch < -1 || ch >= codec->ch_layout.nb_channels; }))
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The selected sound channel is invalid."));
        packet = av_packet_alloc(); frame = av_frame_alloc();
        // AAC noise synthesis depends on preceding decoded packets, so a stretch
        // of an AAC stream is always decoded the same way: in chunks on a fixed
        // grid, each from the same packet a second before its start with the
        // decoder afresh. Random seeks, preview and export then read the same
        // samples whatever was read before, and only two chunks are kept
        // however long the recording is.
        linear = codec->codec_id == AV_CODEC_ID_AAC || codec->codec_id == AV_CODEC_ID_AAC_LATM;
        if (linear) {
            if (swr_alloc_set_opts2(&linearConvert,&codec->ch_layout,AV_SAMPLE_FMT_FLT,codec->sample_rate,
                &codec->ch_layout,codec->sample_fmt,codec->sample_rate,0,nullptr) < 0 || !linearConvert || swr_init(linearConvert) < 0)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare sound sample conversion."));
        }
        return packet && frame;
    }
    qint64 chunkLength() const { return qint64(sourceRate)*16; }
    // The chunk holding grid position `index`, decoded if it is not one of the two kept.
    const Chunk *chunk(qint64 index) {
        for (auto &kept : chunks) if (kept.index == index) { kept.used = ++chunkClock; return &kept; }
        Chunk &into = chunks[0].used <= chunks[1].used ? chunks[0] : chunks[1];
        into.index = -1;
        if (!decodeChunk(index,into)) return nullptr;
        into.index = index; into.used = ++chunkClock;
        return &into;
    }
    bool decodeChunk(qint64 index, Chunk &out) {
        const auto *track = file->streams[stream];
        const int channels = codec->ch_layout.nb_channels;
        const qint64 length = chunkLength(), begin = index*length, end = begin+length;
        if (cacheByteLimit && (__int128)length*channels*sizeof(float) > cacheByteLimit)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The decoded sound cache reached its size limit."));
        // Coarse container timestamps place a frame only to within the
        // tolerance. Read from its start the stream is placed by its first
        // frame and each frame follows the one before; a chunk further on is
        // placed the same way, on the grid of frames that first one sets.
        if (index > 0 && timestampTolerance > 0 && !frameGridSought) {
            Chunk first;
            if (!decodeChunk(0,first)) return false;
        }
        // A second of run-in settles the decoder; the first chunk starts with
        // the stream's own first packet (its priming, before time zero).
        const auto timestamp = av_rescale_q(originSample+begin-codec->sample_rate,{1,codec->sample_rate},track->time_base);
        if (av_seek_frame(file,stream,timestamp,AVSEEK_FLAG_BACKWARD) < 0 && (index > 0 || av_seek_frame(file,stream,timestamp,0) < 0))
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not seek the source sound."));
        // A decoder of its own for the chunk: flushing one keeps the state its
        // noise synthesis has reached, which depends on everything decoded before.
        AVCodecContext *fresh = avcodec_alloc_context3(codec->codec);
        if (fresh) fresh->pkt_timebase = track->time_base;
        if (!fresh || avcodec_parameters_to_context(fresh,track->codecpar) < 0 || avcodec_open2(fresh,fresh->codec,nullptr) < 0
            || fresh->sample_rate != sourceRate || fresh->ch_layout.nb_channels != sourceChannels) {
            avcodec_free_context(&fresh);
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare the source sound decoder."));
        }
        avcodec_free_context(&codec); codec = fresh;
        av_packet_unref(packet); av_frame_unref(frame);
        out.pcm.fill(0.f,length*channels);
        out.samples = 0; out.ended = false;
        qint64 at = 0;  // the sample the next frame continues from, once one has come
        bool any = false, draining = false;
        while (!any || at < end) {
            if (RenderScope::cancelled()) return false;
            const int result = avcodec_receive_frame(codec,frame);
            if (result == AVERROR(EAGAIN)) {
                if (draining) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The sound decoder did not finish its sample interval."));
                int read;
                do {
                    if (RenderScope::cancelled()) return false;
                    av_packet_unref(packet); read = av_read_frame(file,packet);
                } while (read >= 0 && packet->stream_index != stream);
                if (read < 0 && read != AVERROR_EOF) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not read the source sound packets."));
                draining = read == AVERROR_EOF;
                if (avcodec_send_packet(codec,draining ? nullptr : packet) < 0)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not decode the source sound packets."));
                continue;
            }
            if (result == AVERROR_EOF) { out.ended = true; break; }
            if (result < 0) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not decode the source sound samples."));
            if (frame->sample_rate != codec->sample_rate || frame->format != codec->sample_fmt
                || av_channel_layout_compare(&frame->ch_layout,&codec->ch_layout))
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound format changed during decoding."));
            if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound lacks sample timing for precise seeking."));
            qint64 start = av_rescale_q(frame->best_effort_timestamp,track->time_base,{1,codec->sample_rate})-originSample;
            if (any && std::abs(start-at)<=timestampTolerance) start = at;
            if (!any && index == 0) { frameGrid = start+frame->nb_samples; frameGridKnown = true; }
            if (!any && index > 0 && frameGridKnown && timestampTolerance > 0 && frame->nb_samples > 0) {
                const qint64 nearest = frameGrid+std::llround(double(start-frameGrid)/frame->nb_samples)*frame->nb_samples;
                if (std::abs(nearest-start)<=timestampTolerance) start = nearest;
            }
            // Frames before the chunk (the run-in, a leading interval before
            // picture time zero) are discarded. Backward timestamps inside it
            // are ambiguous and must not silently overlap.
            if (any && start < at && at > begin)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound has a discontinuous sample interval."));
            QVector<float> pcm(swr_get_out_samples(linearConvert,frame->nb_samples)*channels);
            auto *output = reinterpret_cast<uint8_t *>(pcm.data());
            const int converted = swr_convert(linearConvert,&output,pcm.size()/channels,
                const_cast<const uint8_t **>(frame->extended_data),frame->nb_samples);
            if (converted != frame->nb_samples)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not convert the source sound samples."));
            // A gap before this frame stays the silence the chunk started as.
            const qint64 from = std::max(start,begin), to = std::min(start+converted,end);
            if (from < to)
                std::copy_n(pcm.constData()+(from-start)*channels,(to-from)*channels,out.pcm.data()+(from-begin)*channels);
            at = start+converted; any = true;
            av_frame_unref(frame);
        }
        out.samples = any ? std::clamp<qint64>(at-begin,0,length) : 0;
        if (index == 0) frameGridSought = true;
        return true;
    }
    bool prepareConversion(int frequency, int channels, AVSampleFormat inputFormat) {
        if (!channelMap.isEmpty() && channels != channelMap.size())
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The requested sound channel count differs from the selected channels."));
        swr_free(&resample);
        AVChannelLayout layout{}; av_channel_layout_default(&layout,channels);
        const int configured = swr_alloc_set_opts2(&resample,&layout,AV_SAMPLE_FMT_FLT,frequency,
            &codec->ch_layout,inputFormat,codec->sample_rate,0,nullptr);
        av_channel_layout_uninit(&layout);
        if (configured < 0 || !resample)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare sound sample conversion."));
        if (!channelMap.isEmpty()) {
            QVector<double> matrix(channels*codec->ch_layout.nb_channels,0.);
            for (int ch=0;ch<channels;++ch) if (channelMap[ch]>=0) matrix[ch*codec->ch_layout.nb_channels+channelMap[ch]]=1.;
            if (swr_set_matrix(resample,matrix.constData(),codec->ch_layout.nb_channels)<0)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare sound sample conversion."));
        }
        if (swr_init(resample)<0) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not prepare sound sample conversion."));
        return true;
    }
    bool fillLinear(qint64 anchor, qint64 target, int frequency, int channels) {
        const qint64 needed = av_rescale_rnd(target,codec->sample_rate,frequency,AV_ROUND_UP)+64;
        if (!prepareConversion(frequency,channels,AV_SAMPLE_FMT_FLT)) return false;
        qint64 at = anchor*codec->sample_rate;
        const int sourceChannels = codec->ch_layout.nb_channels;
        const qint64 length = chunkLength();
        qint64 streamEnd = -1;  // where the stream's sound ends, once a chunk has shown it
        QVector<float> block;
        while (cacheFirst+cache.size()/channels < target) {
            if (RenderScope::cancelled()) return false;
            const int count = int(qMin<qint64>(4096,qMax<qint64>(0,std::llround(duration*codec->sample_rate)-at)));
            // The recorded part of this block, chunk by chunk; the rest is silence.
            block.fill(0.f,count*sourceChannels);
            const qint64 last = knownAudioEnd ? std::min(at+count,audioEnd) : at+count;
            for (qint64 position = at; position < last && streamEnd < 0;) {
                const Chunk *part = chunk(position/length);
                if (!part) return false;
                const qint64 offset = position-part->index*length, n = std::min(last-position,part->samples-offset);
                if (n > 0) {
                    std::copy_n(part->pcm.constData()+offset*sourceChannels,n*sourceChannels,block.data()+(position-at)*sourceChannels);
                    position += n;
                }
                if (part->ended && position >= part->index*length+part->samples) streamEnd = std::max<qint64>(position,part->index*length+part->samples);
                else if (n <= 0) break;
            }
            if (streamEnd >= 0 && streamEnd < needed && knownAudioEnd && streamEnd+timestampTolerance < audioEnd)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source ended before its declared sound interval."));
            const auto *input = reinterpret_cast<const uint8_t *>(block.constData());
            QVector<float> output(qMax(32,swr_get_out_samples(resample,count))*channels);
            auto *data = reinterpret_cast<uint8_t *>(output.data());
            const int converted = swr_convert(resample,&data,output.size()/channels,count ? &input : nullptr,count);
            if (converted < 0) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not convert the source sound samples."));
            const qint64 keep = qMin<qint64>(converted,target-cacheFirst-cache.size()/channels);
            const auto before = cache.size(); cache.resize(before+keep*channels);
            std::copy_n(output.constData(),keep*channels,cache.data()+before);
            at += count;
            if (!count && !converted) break;
        }
        return true;
    }
    bool fill(qint64 first, int needed, int frequency, int channels) {
        cache.clear(); cacheRate = frequency; cacheChannels = channels;
        // Start conversion on an integral second, one second before the needed
        // samples. This retains the resampling phase on random seeks and gives
        // both the codec and resampler preroll. No video-frame quantization.
        const qint64 anchor = std::max<qint64>(0,first/frequency-1);
        cacheFirst = anchor*frequency;
        const qint64 target = std::min<qint64>(std::llround(duration*frequency),
            std::max(first+needed,cacheFirst+2*frequency));
        // A movie can continue long after this sound stream. Seeking the sound
        // demuxer beyond its end is unnecessary and may be rejected by it.
        if (knownAudioEnd && anchor*codec->sample_rate>=audioEnd) {
            cache.fill(0.f,(target-cacheFirst)*channels); return true;
        }
        if (linear) return fillLinear(anchor,target,frequency,channels);
        const auto *track = file->streams[stream];
        // Stateful AAC streams use the chunked path above, including their
        // pre-zero priming packets. Other sources seek on the shared movie clock.
        const auto timestamp = av_rescale_q(originSample+std::max<qint64>(0,anchor-1)*codec->sample_rate,{1,codec->sample_rate},track->time_base);
        if (av_seek_frame(file,stream,timestamp,AVSEEK_FLAG_BACKWARD) < 0)
            return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not seek the source sound."));
        avcodec_flush_buffers(codec); av_packet_unref(packet); av_frame_unref(frame);
        if (!prepareConversion(frequency,channels,codec->sample_fmt)) return false;
        qint64 inputAt = anchor*codec->sample_rate;
        bool draining = false, receivedSamples = false;
        while (cacheFirst+cache.size()/channels < target) {
            if (RenderScope::cancelled()) return false;
            const int result = avcodec_receive_frame(codec,frame);
            if (result == AVERROR(EAGAIN)) {
                if (draining) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The sound decoder did not finish its sample interval."));
                int read;
                do {
                    if (RenderScope::cancelled()) return false;
                    av_packet_unref(packet); read = av_read_frame(file,packet);
                } while (read >= 0 && packet->stream_index != stream);
                if (read < 0 && read != AVERROR_EOF)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not read the source sound packets."));
                draining = read == AVERROR_EOF;
                if (avcodec_send_packet(codec,draining ? nullptr : packet) < 0)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not decode the source sound packets."));
                continue;
            }
            if (result < 0 && result != AVERROR_EOF)
                return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not decode the source sound samples."));
            const uint8_t **input = nullptr;
            QVector<const uint8_t *> planes;
            int available = 0;
            if (result >= 0) {
                if (frame->sample_rate != codec->sample_rate || frame->format != codec->sample_fmt
                    || av_channel_layout_compare(&frame->ch_layout,&codec->ch_layout))
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound format changed during decoding."));
                const qint64 pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound lacks sample timing for precise seeking."));
                qint64 begin = av_rescale_q(pts,track->time_base,{1,codec->sample_rate})-originSample;
                if (std::abs(begin-inputAt)<=timestampTolerance) begin = inputAt;
                if (receivedSamples && begin<inputAt)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source sound has a discontinuous sample interval."));
                if (begin+frame->nb_samples <= inputAt) { av_frame_unref(frame); continue; }
                if (begin > inputAt && !appendSilence(begin-inputAt,target,channels)) return false;
                if (cacheFirst+cache.size()/channels>=target) { av_frame_unref(frame); break; }
                inputAt = std::max(inputAt,begin);
                const int skip = int(inputAt-begin); available = frame->nb_samples-skip;
                const bool planar = av_sample_fmt_is_planar(codec->sample_fmt);
                const int bytes = av_get_bytes_per_sample(codec->sample_fmt);
                for (int ch = 0; ch < (planar ? codec->ch_layout.nb_channels : 1); ++ch)
                    planes.append(frame->extended_data[ch]+qsizetype(skip)*bytes*(planar ? 1 : codec->ch_layout.nb_channels));
                input = planes.data();
                if (knownAudioEnd) available = int(qMax<qint64>(0,qMin<qint64>(available,audioEnd-inputAt)));
                inputAt += available;
                receivedSamples = receivedSamples || available>0;
            } else {
                if (knownAudioEnd && inputAt+timestampTolerance < audioEnd)
                    return fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source ended before its declared sound interval."));
                const qint64 tail = std::llround(duration*codec->sample_rate)-inputAt;
                if (tail>0 && !appendSilence(tail,target,channels)) return false;
                inputAt += qMax<qint64>(0,tail);
            }
            const int capacity = swr_get_out_samples(resample,available);
            QVector<float> output(qMax(capacity,32)*channels);
            auto *data = reinterpret_cast<uint8_t *>(output.data());
            const int converted = swr_convert(resample,&data,output.size()/channels,input,available);
            if (converted < 0) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not convert the source sound samples."));
            const qint64 keep = std::min<qint64>(converted,target-cacheFirst-cache.size()/channels);
            const auto before = cache.size(); cache.resize(before+keep*channels);
            std::copy_n(output.constData(),keep*channels,cache.data()+before);
            av_frame_unref(frame);
            if (result == AVERROR_EOF && converted == 0) break;
        }
        return true;
    }
    bool appendSilence(qint64 samples, qint64 target, int channels) {
        const int sourceChannels = codec->ch_layout.nb_channels;
        const bool planar = av_sample_fmt_is_planar(codec->sample_fmt);
        const int sampleBytes = av_get_bytes_per_sample(codec->sample_fmt);
        QByteArray silence(4096*sourceChannels*sampleBytes,0);
        // Unsigned 8-bit PCM is centred at 128.
        if (codec->sample_fmt==AV_SAMPLE_FMT_U8 || codec->sample_fmt==AV_SAMPLE_FMT_U8P) silence.fill(char(128));
        QVector<const uint8_t *> planes;
        for (int ch=0;ch<(planar ? sourceChannels : 1);++ch)
            planes.append(reinterpret_cast<const uint8_t *>(silence.constData())+(planar ? ch*4096*sampleBytes : 0));
        const qint64 limit = target-cacheFirst;
        while (samples>0 && cache.size()/channels<limit) {
            if (RenderScope::cancelled()) return false;
            const int count = int(qMin<qint64>(4096,samples));
            QVector<float> output(qMax(32,swr_get_out_samples(resample,count))*channels);
            auto *data = reinterpret_cast<uint8_t *>(output.data());
            const int converted = swr_convert(resample,&data,output.size()/channels,planes.data(),count);
            if (converted<0) return fail(QT_TRANSLATE_NOOP("AudioSampleSource","Could not convert the source sound samples."));
            const auto keep = qMin<qint64>(converted,limit-cache.size()/channels);
            const auto before = cache.size(); cache.resize(before+keep*channels);
            std::copy_n(output.constData(),keep*channels,cache.data()+before); samples-=count;
        }
        return true;
    }
    QVector<float> read(qint64 first, int count, int frequency, int channels) {
        QVector<float> output(count*channels,0.f); error.clear();
        if (!channelMap.isEmpty() && channels!=channelMap.size()) {
            fail(QT_TRANSLATE_NOOP("AudioSampleSource","The requested sound channel count differs from the selected channels.")); return {};
        }
        const qint64 total = std::llround(duration*frequency);
        const qint64 begin = std::max<qint64>(first,0), end = std::min<qint64>(first+count,total);
        if (begin >= end) return output;
        if (cacheRate != frequency || cacheChannels != channels || begin < cacheFirst
            || end > cacheFirst+cache.size()/channels)
            if (!fill(begin,end-begin,frequency,channels)) return {};
        if (end > cacheFirst+cache.size()/channels) {
            fail(QT_TRANSLATE_NOOP("AudioSampleSource","The source ended before its declared sound interval.")); return {};
        }
        std::copy_n(cache.constData()+(begin-cacheFirst)*channels,(end-begin)*channels,output.data()+(begin-first)*channels);
        return output;
    }
};
QVector<float> readSamples(Decoder &source,qint64 first,int count,int frequency,int channels,QString *error) {
    if(error)error->clear();
    if(RenderScope::cancelled())return {};
    std::lock_guard<std::mutex> lock(source.mutex);
    if(RenderScope::cancelled())return {};
    if(count<1 || frequency<1 || frequency>768000 || channels<1 || channels>64
        || qint64(count)*channels>16*1024*1024) {
        source.fail(QT_TRANSLATE_NOOP("AudioSampleSource","The requested sound sample interval is invalid or too large."));
        if(error)*error=source.error;
        RenderScope::fail(source.error);return {};
    }
    const __int128 sourceFirst=source.reverse ? (__int128)std::llround(source.duration*frequency)-first-count : first;
    if(sourceFirst<LLONG_MIN || sourceFirst>LLONG_MAX-count) {
        source.fail(QT_TRANSLATE_NOOP("AudioSampleSource","The requested sound sample interval is invalid or too large."));
        if(error)*error=source.error;
        RenderScope::fail(source.error);return {};
    }
    auto samples=source.read(qint64(sourceFirst),count,frequency,channels);
    if(samples.isEmpty()) {
        if(error)*error=source.error;
        RenderScope::fail(source.error);return {};
    }
    if(source.reverse)for(int a=0,b=count-1;a<b;++a,--b)
        for(int ch=0;ch<channels;++ch)std::swap(samples[a*channels+ch],samples[b*channels+ch]);
    return samples;
}
}

struct AudioSampleSource::Reader::State { Decoder decoder; QString path; };

namespace {
std::function<void(const QString &, qint64, int)> &readHook() {
    static std::function<void(const QString &, qint64, int)> hook;
    return hook;
}
std::atomic<quint64> opened{0};
}  // namespace

quint64 AudioSampleSource::openCount() { return opened.load(); }

void AudioSampleSource::setReadHook(std::function<void(const QString &path, qint64 first, int frequency)> hook) { readHook() = std::move(hook); }
AudioSampleSource::Reader::Reader(std::unique_ptr<State> state):m_state(std::move(state)) {}
AudioSampleSource::Reader::~Reader()=default;
std::unique_ptr<AudioSampleSource::Reader> AudioSampleSource::Reader::open(const QString &path,const Options &options,QString *error) {
    if(error)error->clear();
    ++opened;
    auto state=std::make_unique<State>();state->decoder.reverse=options.reverse;state->path=path;
    if(!state->decoder.load(path,options)) {if(error)*error=state->decoder.error;return {};}
    return std::unique_ptr<Reader>(new Reader(std::move(state)));
}
int AudioSampleSource::Reader::sampleRate() const {return m_state->decoder.sourceRate;}
int AudioSampleSource::Reader::channelCount() const {return m_state->decoder.sourceChannels;}
qint64 AudioSampleSource::Reader::sampleCount(int frequency) const {
    return frequency>0 && frequency<=768000 ? std::llround(m_state->decoder.duration*frequency) : 0;
}
qint64 AudioSampleSource::Reader::cacheBytes() const {
    auto &source=m_state->decoder;std::lock_guard<std::mutex> lock(source.mutex);
    return (source.cache.size()+source.chunks[0].pcm.size()+source.chunks[1].pcm.size())*qint64(sizeof(float));
}
QVector<float> AudioSampleSource::Reader::read(qint64 first,int count,int frequency,int channels,QString *error) {
    if(readHook())readHook()(m_state->path,first,frequency);
    return readSamples(m_state->decoder,first,count,frequency,channels,error);
}

