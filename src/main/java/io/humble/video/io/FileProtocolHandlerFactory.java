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
package io.humble.video.io;

import io.humble.video.io.FileProtocolHandler;
import io.humble.video.io.IURLProtocolHandler;
import io.humble.video.io.IURLProtocolHandlerFactory;


/**
 * Implementation of {@link IURLProtocolHandlerFactory} that demonstrates how
 * you can have Java act just like FFMPEG's internal "file:" protocol.
 *  
 * This just duplicates all the functionality in the default "file:" protocol
 * that FFMPEG implemements, but demonstrates how you can have FFMPEG
 * call back into Java.
 * 
 */
public class FileProtocolHandlerFactory implements
    IURLProtocolHandlerFactory
{
  public IURLProtocolHandler getHandler(String protocol, String url,
      int flags)
  {
    return new FileProtocolHandler(url);
  }

}
