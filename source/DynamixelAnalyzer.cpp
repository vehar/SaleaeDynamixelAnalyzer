#include "DynamixelAnalyzer.h"
#include "DynamixelAnalyzerSettings.h"
#include <AnalyzerChannelData.h>

unsigned char reverse(unsigned char b)
{
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}

DynamixelAnalyzer::DynamixelAnalyzer()
:	Analyzer(),  
	mSettings( new DynamixelAnalyzerSettings() ),
	mSimulationInitilized( false ),
	DecodeIndex( 0 )
{
	SetAnalyzerSettings( mSettings.get() );
}

DynamixelAnalyzer::~DynamixelAnalyzer()
{
	KillThread();
}

void DynamixelAnalyzer::WorkerThread()
{
	mResults.reset( new DynamixelAnalyzerResults( this, mSettings.get() ) );
	SetAnalyzerResults( mResults.get() );
	mResults->AddChannelBubblesWillAppearOn( mSettings->mInputChannel );

	mSampleRateHz = GetSampleRate();

	mSerial = GetAnalyzerChannelData( mSettings->mInputChannel );

	if( mSerial->GetBitState() == BIT_LOW )
		mSerial->AdvanceToNextEdge();

	U32 samples_per_bit = mSampleRateHz / mSettings->mBitRate;
	U32 samples_to_first_center_of_first_current_byte_bit = U32( 1.5 * double( mSampleRateHz ) / double( mSettings->mBitRate ) );

	U64 starting_sample;

	for( ; ; )
	{
		U8 current_byte = 0;
		U8 mask = 1 << 7;
		
		mSerial->AdvanceToNextEdge(); //falling edge -- beginning of the start bit

		//U64 starting_sample = mSerial->GetSampleNumber();
		if ( DecodeIndex == DE_HEADER1 )
		{
			starting_sample = mSerial->GetSampleNumber();
		}
		mSerial->Advance( samples_to_first_center_of_first_current_byte_bit );

		for( U32 i=0; i<8; i++ )
		{
			//let's put a dot exactly where we sample this bit:
			//NOTE: Dot, ErrorDot, Square, ErrorSquare, UpArrow, DownArrow, X, ErrorX, Start, Stop, One, Zero
			//mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Start, mSettings->mInputChannel );

			if( mSerial->GetBitState() == BIT_HIGH )
				current_byte |= mask;

			mSerial->Advance( samples_per_bit );

			mask = mask >> 1;
		}

		//TODO: Inverting bits here because I cannot yet find how to add Inverstion to Settings
		current_byte = reverse( current_byte );

		//Process new byte
		
		switch ( DecodeIndex )
		{
			case DE_HEADER1:
				if ( current_byte == 0xFF )
				{
					DecodeIndex = DE_HEADER2;
					//starting_sample = mSerial->GetSampleNumber();
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Dot, mSettings->mInputChannel );
				}
				else
				{
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Stop, mSettings->mInputChannel );
				}
			break;
			case DE_HEADER2:
				if ( current_byte == 0xFF )
				{
					DecodeIndex = DE_ID;
					mChecksum = 1;
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Dot, mSettings->mInputChannel );
				}
				else
				{
					DecodeIndex = DE_HEADER1;
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::ErrorSquare, mSettings->mInputChannel );
				}
			break;
			case DE_ID:
				if ( current_byte != 0xFF )    // we are not allowed 3 0xff's in a row, ie. id != 0xff
				{   
					mID = current_byte;
					DecodeIndex = DE_LENGTH;
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::One, mSettings->mInputChannel );
				}
				else
				{
					DecodeIndex = DE_HEADER1;
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mInputChannel );
				}
			break;
			case DE_LENGTH:
				mLength = current_byte;
				DecodeIndex = DE_INSTRUCTION;
				mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Zero, mSettings->mInputChannel );
			break;
			case DE_INSTRUCTION:
				mInstruction = current_byte;
				mCount = 0;
				DecodeIndex = DE_DATA;
				mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::DownArrow, mSettings->mInputChannel );
				if ( mLength == 2 ) DecodeIndex = DE_CHECKSUM;
			break;
			case DE_DATA:
				mData[ mCount++ ] = current_byte;
				mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::UpArrow, mSettings->mInputChannel );
				if ( mCount >= mLength - 2 )
				{
					DecodeIndex = DE_CHECKSUM;
				}
			break;
			case DE_CHECKSUM:
				DecodeIndex = DE_HEADER1;
				if (  ( ~mChecksum & 0xff ) == ( current_byte & 0xff ) ) 
				{
					// Checksums match!
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::Dot, mSettings->mInputChannel );
				}
				else
				{
					mResults->AddMarker( mSerial->GetSampleNumber(), AnalyzerResults::ErrorDot, mSettings->mInputChannel );
				}

				//We have a new frame to save! 
				Frame frame;
				frame.mData1 = mID;
				frame.mData2 = mInstruction;
				frame.mData2 |= mChecksum << (1*8);
				frame.mData2 |= mLength << (2*8);
				//TODO: Use remaining bits in mData1&2 to present more packet information in the results. 
				frame.mFlags = 0;
				frame.mStartingSampleInclusive = starting_sample;
				frame.mEndingSampleInclusive = mSerial->GetSampleNumber();

				mResults->AddFrame( frame );
				ReportProgress( frame.mEndingSampleInclusive );
			break;
		}
		mChecksum += current_byte;
		
		mResults->CommitResults();
	}
}

bool DynamixelAnalyzer::NeedsRerun()
{
	return false;
}

U32 DynamixelAnalyzer::GenerateSimulationData( U64 minimum_sample_index, U32 device_sample_rate, SimulationChannelDescriptor** simulation_channels )
{
	if( mSimulationInitilized == false )
	{
		mSimulationDataGenerator.Initialize( GetSimulationSampleRate(), mSettings.get() );
		mSimulationInitilized = true;
	}

	return mSimulationDataGenerator.GenerateSimulationData( minimum_sample_index, device_sample_rate, simulation_channels );
}

U32 DynamixelAnalyzer::GetMinimumSampleRateHz()
{
	return mSettings->mBitRate * 4;
}

const char* DynamixelAnalyzer::GetAnalyzerName() const
{
	return "Dynamixel Protocol";
}

const char* GetAnalyzerName()
{
	return "Dynamixel Protocol";
}

Analyzer* CreateAnalyzer()
{
	return new DynamixelAnalyzer();
}

void DestroyAnalyzer( Analyzer* analyzer )
{
	delete analyzer;
}