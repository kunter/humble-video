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
 * Muxer.cpp
 *
 *  Created on: Aug 14, 2013
 *      Author: aclarke
 */

#include <io/humble/ferry/JNIHelper.h>
#include <io/humble/ferry/Logger.h>
#include <io/humble/video/customio/URLProtocolManager.h>

#include "Muxer.h"
#include "VideoExceptions.h"
#include "KeyValueBagImpl.h"

VS_LOG_SETUP(VS_CPP_PACKAGE);

using namespace io::humble::ferry;
using namespace io::humble::video::customio;

namespace io {
namespace humble {
namespace video {

Muxer::Muxer(MuxerFormat* format, const char* filename,
    const char* formatName) {
  mState = STATE_INITED;
  mIOHandler = 0;
  mBufferLength = 2048;

  mCtx = 0;
  int e = avformat_alloc_output_context2(&mCtx, format ? format->getCtx() : 0, formatName,
      filename);
  FfmpegException::check(e, "could not allocate output context ");
  if (!mCtx) {
    VS_THROW(HumbleBadAlloc());
  }
  if (!filename || !*filename) mCtx->filename[0] = 0;
  mCtx->interrupt_callback.callback = Global::avioInterruptCB;
  mCtx->interrupt_callback.opaque = this;

  // now let's look at the output format; it should have been guessed.
  if (!format) {
    if (!mCtx->oformat) {
      avformat_free_context(mCtx);
      VS_THROW(
          HumbleRuntimeError::make(
              "could not determine format to use for muxer. Filename: %s. FormatName: %s",
              filename, formatName));
    }
    mFormat = MuxerFormat::make(mCtx->oformat);
  } else mFormat.reset(format, true);

  // determine if this format NEEDs a file.
  if (!mFormat->getFlag(ContainerFormat::NO_FILE)) {
    if (!*mCtx->filename) {
      avformat_free_context(mCtx);
      VS_THROW(
          HumbleRuntimeError::make(
              "No filename specified, but MuxerFormat needs a file. Filename: %s. FormatName: %s",
              filename, formatName));
    }
  }
}

Muxer::~Muxer() {
  if (mState == STATE_OPENED) {
    VS_LOG_ERROR(
        "Open Muxer destroyed without Muxer.close() being called. Closing anyway: %s",
        this->getURL());
    (void) this->close();
  }
  if (mCtx) {
    for(uint32_t i = 0; i < mCtx->nb_streams; i++) {
      if (mCtx->streams[i]->codec)
        (void) avcodec_close(mCtx->streams[i]->codec);
    }
    avformat_free_context(mCtx);
  }
}

Muxer*
Muxer::make(MuxerFormat *format, const char* filename, const char* formatName) {
  Global::init();

  RefPointer<Muxer> retval;

  if (!format) {
    if ((!filename || !*filename) && (!formatName && !*formatName)) {
      VS_THROW(HumbleInvalidArgument("cannot pass in all nulll parameters"));
    }
  }
  retval.reset(new Muxer(format, filename, formatName), true);
  return retval.get();
}

void
Muxer::setOutputBufferLength(int32_t size) {
  if (size <= 0)
  VS_THROW(HumbleInvalidArgument("size <= 0"));
  if (mState != STATE_INITED)
  VS_THROW(HumbleRuntimeError("Muxer object has already been opened"));
  mBufferLength = size;
}

int32_t
Muxer::getOutputBufferLength() {
  return mBufferLength;
}

void
Muxer::open(KeyValueBag *aInputOptions, KeyValueBag* aOutputOptions) {
  AVFormatContext* ctx = this->getFormatCtx();
  int retval = -1;
  if (mState != STATE_INITED) {
    VS_THROW(
        HumbleRuntimeError::make(
            "Open can only be called when container is in init state. Current state: %d",
            mState));
  }

  AVDictionary* tmp = 0;
  const char* url = ctx->filename;
  AVOutputFormat* fmt = mFormat ? mFormat->getCtx() : 0;


  // Let's check for custom IO
  mIOHandler = URLProtocolManager::findHandler(mCtx->filename,
      URLProtocolHandler::URL_WRONLY_MODE, 0);

  if (mIOHandler) {
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    // free and realloc the input buffer length
    uint8_t* buffer = (uint8_t*) av_malloc(mBufferLength);
    if (!buffer) {
      mState = STATE_ERROR;
      VS_THROW(HumbleBadAlloc());
    }

    // we will allocate ourselves an io context;
    // ownership of buffer passes here.
    ctx->pb = avio_alloc_context(buffer, mBufferLength, 1, mIOHandler,
        Container::url_read, Container::url_write, Container::url_seek);
    if (!ctx->pb) {
      av_free(buffer);
      mState = STATE_ERROR;
      VS_THROW(
          HumbleRuntimeError::make(
              "could not open url due to internal error: %s", url));
    }
  }
  // Check for passed in options
  KeyValueBagImpl* realOpts = dynamic_cast<KeyValueBagImpl*>(aInputOptions);
  if (realOpts) av_dict_copy(&tmp, realOpts->getDictionary(), 0);

  try {
    if (mIOHandler) {
      retval = mIOHandler->url_open(url, URLProtocolHandler::URL_WRONLY_MODE);
    } else if (!fmt || !(fmt->flags & AVFMT_NOFILE)) {
      retval = avio_open2(&ctx->pb, url, AVIO_FLAG_WRITE,
          &ctx->interrupt_callback, 0);
    }
    if (retval < 0) {
      mState = STATE_ERROR;
      FfmpegException::check(retval, "Error opening url: %s; ", url);
    }

    pushCoders();
    /* Write the stream header, if any. */
    retval = avformat_write_header(ctx, &tmp);
    popCoders();
    if (retval < 0) {
      mState = STATE_ERROR;
      FfmpegException::check(retval, "Could not write header for url: %s. ", url);
    }
  } catch (std::exception & e) {
    if (tmp) av_dict_free(&tmp);
    throw;
  }
  KeyValueBagImpl* realUnsetOpts =
      dynamic_cast<KeyValueBagImpl*>(aOutputOptions);
  if (realUnsetOpts) realUnsetOpts->copy(tmp);
  if (tmp) av_dict_free(&tmp);

  mState = STATE_OPENED;
}

void
Muxer::close() {
  if (getState() != STATE_OPENED) {
    VS_THROW(HumbleRuntimeError::make("closed container that was not open"));
  }
  AVFormatContext* ctx = getFormatCtx();
  pushCoders();
  int e = av_write_trailer(ctx);
  popCoders();
  if (e < 0) {
    mState = STATE_ERROR;
    FfmpegException::check(e, "could not write trailer ");
  }
  if (mIOHandler) {
    e = mIOHandler->url_close();
  } else
    e = avio_close(ctx->pb);
  FfmpegException::check(e, "could not close url ");
  mState = STATE_CLOSED;
}

const char*
Muxer::getURL() {
  return this->getFormatCtx()->filename;
}

MuxerStream*
Muxer::addNewStream(Encoder* encoder) {
  if (!encoder) {
    VS_THROW(HumbleInvalidArgument("encoder must be not null"));
  }
  if (encoder->getState() != Encoder::STATE_OPENED) {
    VS_THROW(HumbleInvalidArgument("encoder must be open"));
  }
  if (getState() != STATE_INITED) {
    VS_THROW(HumbleInvalidArgument("cannot add MuxerStream after Muxer is opened"));
  }

  RefPointer<MuxerStream> r;

  AVFormatContext* ctx = getFormatCtx();
  // Tell Ffmpeg about the new stream.
  AVStream* avStream = avformat_new_stream(ctx, encoder->getCodecCtx()->codec);
  if (!avStream) {
    VS_THROW(HumbleRuntimeError("Could not add new stream to container"));
  }
  // tell the container to update all the known streams.
  doSetupStreams();
  // and set the coder for the given stream
  Container::Stream* stream = Container::getStream(avStream->index);
  // grab a reference to the passed in coder.
  stream->setCoder(encoder);
  return r.get();
}


bool
Muxer::writePacket(MediaPacket* packet) {
  if (getState() != STATE_OPENED) {
    VS_THROW(HumbleRuntimeError("Cannot write to unopened Muxer"));
  }
  if (!packet) {
    VS_THROW(HumbleInvalidArgument("null packet"));
  }
  if (!packet->isComplete()) {
    VS_THROW(HumbleInvalidArgument("cannot write incomplete packet"));
  }
  int numStreams = getNumStreams();
  if (packet->getStreamIndex() >= numStreams) {
    VS_THROW(HumbleRuntimeError("attempt to write packet to stream that does not exist in this muxer"));
  }
  if (!packet->getSize()) {
    VS_THROW(HumbleRuntimeError("Cannot write empty packet"));
  }

  VS_THROW(HumbleRuntimeError("not yet implemented"));

  /// now, do the madness.
  pushCoders();
  popCoders();

  return false;
}

/*
void
Muxer::stampOutputPacket(MediaPacket* packet) {
  if (!packet) {
    VS_THROW(HumbleInvalidArgument("no packet specified"));
  }

  //    VS_LOG_DEBUG("input:  duration: %lld; dts: %lld; pts: %lld;",
  //        packet->getDuration(), packet->getDts(), packet->getPts());

  // Always just reset this; cheaper than checking if it's
  // already set
  packet->setStreamIndex(this->getIndex());

  io::humble::ferry::RefPointer<Rational> thisBase = getTimeBase();
  io::humble::ferry::RefPointer<Rational> packetBase = packet->getTimeBase();
  if (!thisBase || !packetBase) {
    VS_THROW(HumbleRuntimeError("no timebases on either stream or packet"));
  }
  if (thisBase->compareTo(packetBase.value()) == 0) {
    //      VS_LOG_DEBUG("Same timebase: %d/%d vs %d/%d",
    //          thisBase->getNumerator(), thisBase->getDenominator(),
    //          packetBase->getNumerator(), packetBase->getDenominator());
    // it's already got the right time values
    return;
  }

  int64_t duration = packet->getDuration();
  int64_t dts = packet->getDts();
  int64_t pts = packet->getPts();

  if (duration >= 0) duration = thisBase->rescale(duration, packetBase.value(),
      Rational::ROUND_DOWN);

  if (pts != Global::NO_PTS) {
    pts = thisBase->rescale(pts, packetBase.value(), Rational::ROUND_DOWN);
  }
  if (dts != Global::NO_PTS) {
    dts = thisBase->rescale(dts, packetBase.value(), Rational::ROUND_DOWN);
    if (mLastDts != Global::NO_PTS && dts == mLastDts) {
      // adjust for rounding; we never want to insert a frame that
      // is not monotonically increasing.  Note we only do this if
      // we're off by one; that's because we ROUND_DOWN and then assume
      // that can be off by at most one.  If we're off by more than one
      // then it's really an error on the person muxing to this stream.
      dts = mLastDts + 1;
      // and round up pts
      if (pts != Global::NO_PTS) ++pts;
      // and if after all that adjusting, pts is less than dts
      // let dts win.
      if (pts == Global::NO_PTS || pts < dts) pts = dts;
    }
    mLastDts = dts;
  }

  //    VS_LOG_DEBUG("output: duration: %lld; dts: %lld; pts: %lld;",
  //        duration, dts, pts);
  packet->setDuration(duration);
  packet->setPts(pts);
  packet->setDts(dts);
  packet->setTimeBase(thisBase.value());
  //    VS_LOG_DEBUG("Reset timebase: %d/%d",
  //        thisBase->getNumerator(), thisBase->getDenominator());
}
*/

} /* namespace video */
} /* namespace humble */
} /* namespace io */
