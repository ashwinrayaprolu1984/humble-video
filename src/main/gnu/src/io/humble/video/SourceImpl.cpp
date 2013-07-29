/*******************************************************************************
 * Copyright (c) 2013, Art Clarke.  All rights reserved.
 *  
 * This file is part of Humble-Video.
 *
 * Humble-Video is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Humble-Video is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Humble-Video.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
/*
 * SourceImpl.cpp
 *
 *  Created on: Jul 2, 2013
 *      Author: aclarke
 */
#include <io/humble/ferry/Logger.h>
#include <io/humble/ferry/JNIHelper.h>
#include <io/humble/video/customio/URLProtocolManager.h>
#include "Global.h"
#include "SourceImpl.h"
#include "MediaPacketImpl.h"
#include "KeyValueBagImpl.h"
#include "SourceStreamImpl.h"

VS_LOG_SETUP(VS_CPP_PACKAGE);

using namespace io::humble::video::customio;
using namespace io::humble::ferry;

namespace io {
namespace humble {
namespace video {

SourceImpl::SourceImpl() {
  mNumStreams = 0;
  mStreamInfoGotten = 0;
  mReadRetryMax = 1;
  mInputBufferLength = 2048;
  mIOHandler = 0;
  mCtx = avformat_alloc_context();
  if (!mCtx) throw std::bad_alloc();
  // Set up thread interrupt capabilities
  mCtx->interrupt_callback.callback = Global::avioInterruptCB;
  mCtx->interrupt_callback.opaque = this;
  mState = Container::STATE_INITED;
}

SourceImpl::~SourceImpl() {
  if (mState == Container::STATE_OPENED ||
      mState == Container::STATE_PLAYING ||
      mState == Container::STATE_PAUSED) {
    VS_LOG_ERROR("Open Source destroyed without Source.close() being called. Closing anyway: %s",
        this->getURL());
    (void) this->close();
  }
  if (mCtx)
    avformat_free_context(mCtx);
}
AVFormatContext*
SourceImpl::getFormatCtx() {
  // throw an exception if in wrong state.
  if (mState == Container::STATE_CLOSED || mState == Container::STATE_ERROR) {
    const char * MESSAGE="Method called on Source in CLOSED or ERROR state.";
    VS_LOG_ERROR(MESSAGE);
    std::exception e = std::runtime_error(MESSAGE);
    throw e;
  }
  return mCtx;
}

SourceImpl*
SourceImpl::make() {
  Global::init();
  SourceImpl *retval = new SourceImpl();
  VS_REF_ACQUIRE(retval);
  return retval;
}

int32_t
SourceImpl::open(const char *url, SourceFormat* format,
    bool streamsCanBeAddedDynamically, bool queryMetaData,
    KeyValueBag* options, KeyValueBag* optionsNotSet)
{
  AVFormatContext* ctx = this->getFormatCtx();
  int retval = -1;
  if (mState != Container::STATE_INITED) {
    VS_LOG_DEBUG("Open can only be called when container is in init state. Current state: %d", mState);
    return retval;
  }
  if (!url || !*url) {
    VS_LOG_DEBUG("Open cannot be called with empty URL");
    return retval;
  }

  AVDictionary* tmp=0;

  if (format) {
    // acquire a long-lived reference
    mFormat.reset(format, true);
    ctx->iformat = mFormat->getCtx();
  }

  AVInputFormat* oldFormat = ctx->iformat;

  // Let's check for custom IO
  mIOHandler = URLProtocolManager::findHandler(
         url,
         URLProtocolHandler::URL_RDONLY_MODE,
         0);

  if (mIOHandler) {
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    // free and realloc the input buffer length
    uint8_t* buffer = (uint8_t*)av_malloc(mInputBufferLength);
    if (!buffer) {
      mState = Container::STATE_ERROR;
      return retval;
    }

    // we will allocate ourselves an io context;
    // ownership of buffer passes here.
    ctx->pb = avio_alloc_context(
        buffer,
        mInputBufferLength,
        0,
        mIOHandler,
        Container::url_read,
        Container::url_write,
        Container::url_seek);
    if (!ctx->pb) {
      av_free(buffer);
      mState = Container::STATE_ERROR;
      return retval;
    }
  }
  // Check for passed in options
  KeyValueBagImpl* realOpts = dynamic_cast<KeyValueBagImpl*>(options);
  if (realOpts)
    av_dict_copy(&tmp, realOpts->getDictionary(), 0);

  // Now call the real open method; this is done
  // in another function to ensure we clean up tmp
  // afterwards.
  retval = doOpen(url, &tmp);
  VS_CHECK_INTERRUPT(retval, true);

  if (retval >= 0) {
    mState = Container::STATE_OPENED;

    if (oldFormat != ctx->iformat)
      mFormat = SourceFormat::make(ctx->iformat);

    if (streamsCanBeAddedDynamically)
      ctx->ctx_flags |= AVFMTCTX_NOHEADER;

    KeyValueBagImpl* realUnsetOpts = dynamic_cast<KeyValueBagImpl*>(optionsNotSet);
    if (realUnsetOpts)
      realUnsetOpts->copy(tmp);

    if (queryMetaData)
      retval = this->queryStreamMetaData();
  }
  if (tmp)
    av_dict_free(&tmp);
  if (retval < 0)
    mState = Container::STATE_ERROR;
  return retval;
}

void
SourceImpl::setInputBufferLength(int32_t size) {
  if (size <= 0)
    VS_THROW(HumbleInvalidArgument("size <= 0"));
  if (mState != Container::STATE_INITED)
    VS_THROW(HumbleRuntimeError("Source object has already been opened"));
  mInputBufferLength = size;
}

int32_t
SourceImpl::getInputBufferLength() {
  return mInputBufferLength;
}

int32_t
SourceImpl::getNumStreams() {
  int32_t retval = 0;
  if (!(mState == Container::STATE_OPENED ||
      mState == Container::STATE_PLAYING ||
      mState == Container::STATE_PAUSED)) {
    VS_THROW(HumbleRuntimeError("Attempt to query number of streams in source when not opened, playing or paused is ignored"));
  }
  if ((int32_t) mCtx->nb_streams != mNumStreams)
    doSetupSourceStreams();
  retval = mNumStreams;

  VS_CHECK_INTERRUPT(retval, true);
  return retval;
}

int32_t
SourceImpl::close() {
  int32_t retval=-1;
  if (!(mState == Container::STATE_OPENED ||
      mState == Container::STATE_PLAYING ||
      mState == Container::STATE_PAUSED)) {
    VS_THROW(HumbleRuntimeError("Attempt to close container when not opened, playing or paused"));
  }

  // tell the streams we're closing.
  while(mStreams.size() > 0)
  {
    RefPointer<SourceStreamImpl> * stream=mStreams.back();

    VS_ASSERT(stream && *stream, "no stream?");
    if (stream && *stream) {
      (*stream)->containerClosed(this);
      delete stream;
    }
    mStreams.pop_back();
  }
  mNumStreams = 0;


  // we need to remember the avio context
  AVIOContext* pb = this->getFormatCtx()->pb;

  avformat_close_input(&mCtx);
  // avformat_close_input frees the context, so...
  mCtx = 0;

  retval = doCloseFileHandles(pb);
  if (retval < 0) {
    VS_LOG_ERROR("Error when closing container (%s): %d", getURL(), retval);
    mState = Container::STATE_ERROR;
  }

  if (mState != Container::STATE_ERROR)
    mState = Container::STATE_CLOSED;
  return retval;
}

int32_t
SourceImpl::doCloseFileHandles(AVIOContext* pb)
{
  int32_t retval = -1;
  if (mIOHandler) {
    // make sure all data is pushed out.
    if (pb) avio_flush(pb);
    // close our handle
    retval = mIOHandler->url_close();
    if (pb) av_freep(&pb->buffer);
    av_free(pb);
    delete mIOHandler;
    mIOHandler = 0;
  } else
    retval = 0;
  return retval;
}

SourceStream*
SourceImpl::getSourceStream(int32_t position) {
  if (!(mState == Container::STATE_OPENED ||
      mState == Container::STATE_PLAYING ||
      mState == Container::STATE_PAUSED)) {
    VS_THROW(HumbleRuntimeError("Attempt to get source stream from source when not opened, playing or paused is ignored"));
  }
  SourceStream *retval = 0;
  if ((int32_t)mCtx->nb_streams != mNumStreams)
    doSetupSourceStreams();

  if (position < mNumStreams)
  {
    // will acquire for caller.
    RefPointer<SourceStreamImpl> * p = mStreams.at(position);
    retval = p ? p->get() : 0;
  }
  return retval;

}

int32_t
SourceImpl::read(MediaPacket* ipkt) {
  int32_t retval = -1;
  MediaPacketImpl* pkt = dynamic_cast<MediaPacketImpl*>(ipkt);
  if (pkt)
  {
    pkt->reset(0);
    AVPacket* packet=pkt->getCtx();

    int32_t numReads=0;
    pkt->setComplete(false, pkt->getSize());
    do
    {
      retval = av_read_frame(this->getFormatCtx(),
          packet);
      ++numReads;
    }
    while (retval == AVERROR(EAGAIN) &&
        (mReadRetryMax < 0 || numReads <= mReadRetryMax));

    // and dump it's contents
    VS_LOG_TRACE("read: %lld, %lld, %d, %d, %d, %lld, %lld: %p",
        pkt->getDts(),
        pkt->getPts(),
        pkt->getFlags(),
        pkt->getStreamIndex(),
        pkt->getSize(),
        pkt->getDuration(),
        pkt->getPosition(),
        packet->data);

    // and let's try to set the packet time base if known
    if (retval >= 0) {
      if (pkt->getStreamIndex() >= 0)
      {
        RefPointer<SourceStream> stream = this->getSourceStream(pkt->getStreamIndex());
        if (stream)
        {
          RefPointer<Rational> streamBase = stream->getTimeBase();
          if (streamBase)
          {
            pkt->setTimeBase(streamBase.value());
          }
        }
      }
      pkt->setComplete(true, pkt->getSize());
    }
  }
  VS_CHECK_INTERRUPT(retval, true);
  return retval;
}

int32_t
SourceImpl::queryStreamMetaData() {
  int32_t retval = -1;
  if (!(mState == Container::STATE_OPENED ||
      mState == Container::STATE_PLAYING ||
      mState == Container::STATE_PAUSED)) {
    VS_THROW(HumbleRuntimeError("Attempt to query stream information from container when not opened, playing or paused"));
  }
  if (!mStreamInfoGotten) {
    retval = avformat_find_stream_info(this->getFormatCtx(), 0);
    if (retval >= 0)
      mStreamInfoGotten = true;
  } else
    retval = 0;

  if (retval >= 0 && mCtx->nb_streams > 0)
  {
    doSetupSourceStreams();
  } else {
    VS_LOG_WARN("Could not find streams in input container");
  }

  VS_CHECK_INTERRUPT(retval, true);
  return retval;
}

int64_t
SourceImpl::getDuration() {
  return this->getFormatCtx()->duration;
}

int64_t
SourceImpl::getStartTime() {
  return this->getFormatCtx()->start_time;
}

int64_t
SourceImpl::getFileSize() {
  int64_t retval = -1;
  AVFormatContext* ctx = this->getFormatCtx();
  if (ctx->iformat && (ctx->iformat->flags & AVFMT_NOFILE))
    retval = 0;
  else {
    retval = avio_size(ctx->pb);
    retval = FFMAX(0, retval);
  }
  return retval;
}

int32_t
SourceImpl::getBitRate() {
  return this->getFormatCtx()->bit_rate;
}

int32_t
SourceImpl::getFlags() {
  int32_t flags = this->getFormatCtx()->flags;
  return flags;
}

void
SourceImpl::setFlags(int32_t newFlags) {
  AVFormatContext* ctx=this->getFormatCtx();
  ctx->flags = newFlags;
  if (mIOHandler)
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
}

bool
SourceImpl::getFlag(Flag flag) {
  return this->getFormatCtx()->flags & flag;
}

void
SourceImpl::setFlag(Flag flag, bool value) {
  AVFormatContext* ctx=this->getFormatCtx();

  if (value)
    ctx->flags |= flag;
  else
    ctx->flags &= (~flag);
}

const char*
SourceImpl::getURL() {
  return this->getFormatCtx()->filename;
}

int32_t
SourceImpl::getReadRetryCount() {
  return mReadRetryMax;
}

void
SourceImpl::setReadRetryCount(int32_t count) {
  if (count >= 0)
    mReadRetryMax = count;
}

bool
SourceImpl::canStreamsBeAddedDynamically() {
  return this->getFormatCtx()->ctx_flags & AVFMTCTX_NOHEADER;
}

KeyValueBag*
SourceImpl::getMetaData() {
  if (!mMetaData)
    mMetaData = KeyValueBagImpl::make(this->getFormatCtx()->metadata);
  return mMetaData.get();
}

int32_t
SourceImpl::setForcedAudioCodec(Codec::ID id) {
  this->getFormatCtx()->audio_codec_id = (enum AVCodecID) id;
  return 0;
}

int32_t
SourceImpl::setForcedVideoCodec(Codec::ID id) {
  this->getFormatCtx()->video_codec_id = (enum AVCodecID) id;
  return 0;
}

int32_t
SourceImpl::setForcedSubtitleCodec(Codec::ID id) {
  this->getFormatCtx()->subtitle_codec_id = (enum AVCodecID)id;
  return 0;
}

int32_t
SourceImpl::getMaxDelay() {
  return this->getFormatCtx()->max_delay;
}

int32_t
SourceImpl::seek(int32_t stream_index, int64_t min_ts, int64_t ts,
    int64_t max_ts, int32_t flags) {
  if (mState != Container::STATE_OPENED)
  {
    VS_THROW(HumbleRuntimeError("Can only seek on OPEN (not paused or playing) sources"));
  }
  int32_t retval = avformat_seek_file(this->getFormatCtx(),
      stream_index,
      min_ts,
      ts,
      max_ts,
      flags);
  // TODO: Make sure all FFmpeg input buffers are cleared after seek

  VS_CHECK_INTERRUPT(retval, true);
  return retval;
}

int32_t
SourceImpl::pause() {
  if (mState != Container::STATE_PLAYING)
  {
    VS_THROW(HumbleRuntimeError("Can only pause containers in PLAYING state."));
  }
  int32_t retval = av_read_pause(this->getFormatCtx());
  VS_CHECK_INTERRUPT(retval, true);
  if (retval >= 0)
    mState = Container::STATE_PAUSED;
  return retval;
}

int32_t
SourceImpl::play() {
  if (mState != Container::STATE_PAUSED || mState != Container::STATE_OPENED)
  {
    VS_THROW(HumbleRuntimeError("Can only play containers in OPENED or PAUSED states"));
    return -1;
  }
  int32_t retval = av_read_play(this->getFormatCtx());
  VS_CHECK_INTERRUPT(retval, true);
  if (retval >= 0)
    mState = Container::STATE_PLAYING;
  return retval;
}

int32_t
SourceImpl::doOpen(const char* url, AVDictionary** options)
{
  int32_t retval=0;
  AVFormatContext* ctx = this->getFormatCtx();

  if (mIOHandler)
    retval = mIOHandler->url_open(url, URLProtocolHandler::URL_RDONLY_MODE);

  if (retval >= 0) {
    AVIOContext* pb = ctx->pb;
    retval = avformat_open_input(&mCtx,
        url,
        ctx->iformat,
        options);
    if (retval < 0)
      // close open filehandle
      doCloseFileHandles(pb);
  }

  return retval;
}

int32_t
SourceImpl::doSetupSourceStreams()
{
  // do nothing if we're already all set up.
  if (mNumStreams == (int32_t) mCtx->nb_streams)
    return 0;

  int32_t retval = -1;
  // loop through and find the first non-zero time base
  AVRational *goodTimebase = 0;
  for(uint32_t i = 0;i < mCtx->nb_streams;i++){
    AVStream *avStream = mCtx->streams[i];
    if(avStream && avStream->time_base.num && avStream->time_base.den){
      goodTimebase = &avStream->time_base;
      break;
    }
  }

  // Only look for new streams
  for (uint32_t i =mNumStreams ; i < mCtx->nb_streams; i++)
  {
    AVStream *avStream = mCtx->streams[i];
    if (avStream)
    {
      if (goodTimebase && (!avStream->time_base.num || !avStream->time_base.den))
      {
        avStream->time_base = *goodTimebase;
      }

      RefPointer<SourceStreamImpl>* stream = new RefPointer<SourceStreamImpl>(
          SourceStreamImpl::make(this, avStream, 0)
      );

      if (stream)
      {
        if (stream->value())
        {
          mStreams.push_back(stream);
          mNumStreams++;
        } else {
          VS_LOG_ERROR("Couldn't make a stream %d", i);
          delete stream;
        }
        stream = 0;
      }
      else
      {
        VS_LOG_ERROR("Could not make Stream %d", i);
        retval = -1;
      }
    } else {
      VS_LOG_ERROR("no FFMPEG allocated stream: %d", i);
      retval = -1;
    }
  }
  return retval;

}

} /* namespace video */
} /* namespace humble */
} /* namespace io */
