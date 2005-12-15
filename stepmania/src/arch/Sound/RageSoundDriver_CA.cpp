#include "global.h"
#include "RageUtil.h"
#include "RageSoundDriver_CA.h"
#include "RageLog.h"
#include "RageTimer.h"
#include "PrefsManager.h"

#include <AudioToolbox/AudioConverter.h>
#include "CAAudioHardwareSystem.h"
#include "CAAudioHardwareDevice.h"
#include "CAAudioHardwareStream.h"
#include "CAStreamBasicDescription.h"
#include "CAException.h"
#include "archutils/Darwin/DarwinThreadHelpers.h"

static const UInt32 kFramesPerPacket = 1;
static const UInt32 kChannelsPerFrame = 2;
static const UInt32 kBitsPerChannel = 16;
static const UInt32 kBytesPerPacket = kChannelsPerFrame * kBitsPerChannel / 8;
static const UInt32 kBytesPerFrame = kBytesPerPacket;
static const UInt32 kFormatFlags = kAudioFormatFlagsNativeEndian |
kAudioFormatFlagIsSignedInteger;

static int64_t g_iLastSampleTime = 0;

typedef CAStreamBasicDescription Desc;

/* temporary hack: */
static float g_fLastIOProcTime = 0;
static const int NUM_MIX_TIMES = 16;
static float g_fLastMixTimes[NUM_MIX_TIMES];
static int g_iLastMixTimePos = 0;
static int g_iNumIOProcCalls = 0;

RageSound_CA::RageSound_CA()
{
	m_pOutputDevice = NULL;
	m_Converter = NULL;
}

CString RageSound_CA::Init()
{
	try
	{
		AudioDeviceID dID = CAAudioHardwareSystem::GetDefaultDevice( false, false );
		m_pOutputDevice = new CAAudioHardwareDevice( dID );
	}
	catch( const CAException& e )
	{
		return "Couldn't create default output device.";
	}
	
	m_iSampleRate = PREFSMAN->m_iSoundPreferredSampleRate;
	Float64 nominalSampleRate =  m_iSampleRate;
    
	try
	{
		m_pOutputDevice->SetNominalSampleRate(nominalSampleRate);
		LOG->Info( "Set the nominal sample rate to %f.", nominalSampleRate );
	}
	catch( const CAException& e )
	{
		LOG->Warn( "Couldn't set the nominal sample rate." );
		nominalSampleRate = m_pOutputDevice->GetNominalSampleRate();
		LOG->Warn( "Device's nominal sample rate is %f", nominalSampleRate );
		m_iSampleRate = int( nominalSampleRate );
	}
	AudioStreamID sID = m_pOutputDevice->GetStreamByIndex( kAudioDeviceSectionOutput, 0 );
	CAAudioHardwareStream stream( sID );

	try
	{
		m_pOutputDevice->AddPropertyListener( kAudioPropertyWildcardChannel, kAudioPropertyWildcardSection,
											kAudioDeviceProcessorOverload, OverloadListener, this );
	}
	catch( const CAException& e )
	{
		LOG->Warn("Could not install the overload listener.");
	}

	// The canonical format
	Desc IOProcFormat( nominalSampleRate, kAudioFormatLinearPCM, 8, 1, 8, 2, 32,
					   kAudioFormatFlagsNativeFloatPacked);
	const Desc SMFormat( nominalSampleRate, kAudioFormatLinearPCM, kBytesPerPacket, kFramesPerPacket,
						 kBytesPerFrame, kChannelsPerFrame, kBitsPerChannel, kFormatFlags );
	
	try
	{
		stream.SetCurrentIOProcFormat( IOProcFormat );
	}
	catch( const CAException& e )
	{
		LOG->Warn( "Could not set the IOProc format to the canonical format." );
		stream.GetCurrentIOProcFormat( IOProcFormat );
	}
	
	if( AudioConverterNew(&SMFormat, &IOProcFormat, &m_Converter) )
		return "Couldn't create the audio converter";

	UInt32 bufferSize;
	
	try
	{
		bufferSize = m_pOutputDevice->GetIOBufferSize();
		LOG->Info("I/O Buffer size: %lu frames", bufferSize);
	}
	catch( const CAException& e )
	{
		LOG->Warn( "Could not determine buffer size." );
		bufferSize = 0;
	}    
    
	try
	{
		UInt32 frames = m_pOutputDevice->GetLatency( kAudioDeviceSectionOutput );
		if( stream.HasProperty(0, kAudioDevicePropertyLatency) )
		{
			UInt32 t, size = 4;
            
			stream.GetPropertyData( 0, kAudioDevicePropertyLatency, size, &t );
			frames += t;
			LOG->Info( "Frames of stream latency: %lu", t );
		}
		else
		{
			LOG->Warn( "Stream does not report latency." );
		}
		frames += m_pOutputDevice->GetSafetyOffset( kAudioDeviceSectionOutput );
		frames += bufferSize;
		m_fLatency = frames / nominalSampleRate;
		LOG->Info( "Frames of latency:        %lu\n"
				   "Seconds of latency:       %f", frames, m_fLatency );
	}
	catch( const CAException& e )
	{
		return "Couldn't get latency.";
	}		

	StartDecodeThread();
    
	try
	{
		m_pOutputDevice->AddIOProc( GetData, this );
		m_pOutputDevice->StartIOProc( GetData );
	}
	catch( const CAException& e )
	{
		return "Couldn't start the IOProc.";
	}
	return "";
}

RageSound_CA::~RageSound_CA()
{
	if( m_pOutputDevice != NULL )
	{
		m_pOutputDevice->StopIOProc( GetData );
		m_pOutputDevice->RemoveIOProc( GetData );
	}
	delete m_pOutputDevice;

	if( m_Converter != NULL )
		AudioConverterDispose( m_Converter );
}

int64_t RageSound_CA::GetPosition( const RageSoundBase *sound ) const
{
	AudioTimeStamp time;
	
	try
	{
		m_pOutputDevice->GetCurrentTime( time );
		g_iLastSampleTime = int64_t( time.mSampleTime );
		return g_iLastSampleTime;
	}
	catch( const CAException& e )
	{
		if( e.GetError() == 'stop' )
			return g_iLastSampleTime;
		
		char error[5];
		
		*(int32_t*)error = e.GetError();
		error[4] = '\0';
		FAIL_M( ssprintf("GetCurrentTime() returned error '%s'.", error) );
	}
}

OSStatus RageSound_CA::GetData( AudioDeviceID inDevice,
								const AudioTimeStamp *inNow,
								const AudioBufferList *inInputData,
								const AudioTimeStamp *inInputTime,
								AudioBufferList *outOutputData,
								const AudioTimeStamp *inOutputTime,
								void *inClientData )
{
	RageTimer tm;
	RageSound_CA *This = (RageSound_CA *)inClientData;
	AudioBuffer& buf = outOutputData->mBuffers[0];
	UInt32 dataPackets = buf.mDataByteSize >> 3; // 8 byes per packet
	int64_t decodePos = int64_t( inOutputTime->mSampleTime );
	int64_t now = int64_t( inNow->mSampleTime );
	
	g_iLastSampleTime = now;
	RageTimer tm2;
	int16_t buffer[dataPackets * (kBytesPerPacket >> 1)];
		
	This->Mix( buffer, dataPackets, decodePos, now) ;
	g_fLastMixTimes[g_iLastMixTimePos] = tm2.GetDeltaTime();
	++g_iLastMixTimePos;
	wrap( g_iLastMixTimePos, NUM_MIX_TIMES );
		
	AudioConverterConvertBuffer( This->m_Converter, dataPackets * kBytesPerPacket,
								 buffer, &buf.mDataByteSize, buf.mData );
		
	g_fLastIOProcTime = tm.GetDeltaTime();
	++g_iNumIOProcCalls;
	
	return noErr;
}
		

OSStatus RageSound_CA::OverloadListener( AudioDeviceID inDevice,
										 UInt32 inChannel,
										 Boolean isInput,
										 AudioDevicePropertyID inPropertyID,
										 void *inData )
{
	CString Output;
	for( int i = NUM_MIX_TIMES-1; i >= 0; --i )
	{
		int pos = (g_iLastMixTimePos+i) % NUM_MIX_TIMES;
		Output += ssprintf( "%.3f ", g_fLastMixTimes[pos] );
	}

	LOG->Warn( "Audio overload.  Last IOProc time: %f IOProc calls: %i (%s)",
			   g_fLastIOProcTime, g_iNumIOProcCalls, Output.c_str() );
	g_iNumIOProcCalls = 0;
	return noErr;
}

void RageSound_CA::SetupDecodingThread()
{
	/* Increase the scheduling precedence of the decoder thread. */
	SetThreadPrecedence( 50 );
}

/*
 * (c) 2004, 2005 Steve Checkoway
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
