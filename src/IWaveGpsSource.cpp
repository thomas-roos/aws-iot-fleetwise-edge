// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "IWaveGpsSource.h"
#include "LoggingModule.h"
#include "TraceModule.h"
#include <boost/filesystem.hpp>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace Aws
{
namespace IoTFleetWise
{

// NOLINT below due to C++17 warning of redundant declarations that are required to maintain C++14 compatibility
constexpr const char *IWaveGpsSource::PATH_TO_NMEA;        // NOLINT
constexpr const char *IWaveGpsSource::CAN_CHANNEL_NUMBER;  // NOLINT
constexpr const char *IWaveGpsSource::CAN_RAW_FRAME_ID;    // NOLINT
constexpr const char *IWaveGpsSource::LATITUDE_START_BIT;  // NOLINT
constexpr const char *IWaveGpsSource::LONGITUDE_START_BIT; // NOLINT
const std::string DEFAULT_NMEA_SOURCE = "ttyUSB1";
const std::string DEFAULT_PATH_TO_NMEA_SOURCE = "/dev/" + DEFAULT_NMEA_SOURCE;
const std::string SYS_USB_DEVICES_PATH = "/sys/bus/usb/devices";
const std::string QUECTEL_VENDOR_ID = "2c7c";

static std::string
getFileContents( const std::string &p )
{
    constexpr auto NUM_CHARS = 1;
    std::string ret;
    std::ifstream fs{ p };
    // False alarm: uninit_use_in_call: Using uninitialized value "fs._M_streambuf_state" when calling "good".
    // coverity[uninit_use_in_call : SUPPRESS]
    while ( fs.good() )
    {
        auto c = static_cast<char>( fs.get() );
        ret.append( NUM_CHARS, c );
    }

    return ret;
}

static bool
detectQuectelDevice()
{
    if ( !boost::filesystem::exists( DEFAULT_PATH_TO_NMEA_SOURCE ) )
    {
        return false;
    }
    for ( boost::filesystem::directory_iterator it( SYS_USB_DEVICES_PATH );
          it != boost::filesystem::directory_iterator();
          ++it )
    {
        if ( !boost::filesystem::is_directory( *it ) || !boost::filesystem::exists( it->path().string() + "/uevent" ) ||
             !boost::filesystem::exists( it->path().string() + "/" + DEFAULT_NMEA_SOURCE ) )
        {
            continue;
        }
        auto result = getFileContents( it->path().string() + "/uevent" );
        if ( result.find( QUECTEL_VENDOR_ID ) != std::string::npos )
        {
            return true;
        }
    }
    return false;
}

IWaveGpsSource::IWaveGpsSource( SignalBufferPtr signalBufferPtr )
    : mSignalBufferPtr{ std::move( signalBufferPtr ) }
{
}

bool
IWaveGpsSource::init( const std::string &pathToNmeaSource,
                      CANChannelNumericID canChannel,
                      CANRawFrameID canRawFrameId,
                      uint16_t latitudeStartBit,
                      uint16_t longitudeStartBit )
{
    if ( canChannel == INVALID_CAN_SOURCE_NUMERIC_ID )
    {
        FWE_LOG_ERROR( "Invalid CAN channel" );
        return false;
    }
    // If no configuration was provided, auto-detect the GPS presence for backwards compatibility
    if ( pathToNmeaSource.empty() )
    {
        if ( !detectQuectelDevice() )
        {
            FWE_LOG_TRACE( "No Quectel device detected" );
            return false;
        }
        FWE_LOG_INFO( "Quectel device detected at " + DEFAULT_PATH_TO_NMEA_SOURCE );
        mPathToNmeaSource = DEFAULT_PATH_TO_NMEA_SOURCE;
    }
    else
    {
        mPathToNmeaSource = pathToNmeaSource;
    }
    mLatitudeStartBit = latitudeStartBit;
    mLongitudeStartBit = longitudeStartBit;
    mCanChannel = canChannel;
    mCanRawFrameId = canRawFrameId;
    setFilter( mCanChannel, mCanRawFrameId );
    mCyclicLoggingTimer.reset();
    return true;
}
const char *
IWaveGpsSource::getThreadName()
{
    return "IWaveGpsSource";
}

void
IWaveGpsSource::pollData()
{
    // Read from NMEA formatted file
    auto bytes = read( mFileHandle, mBuffer, MAX_BYTES_READ_PER_POLL - 1 );
    if ( bytes < 0 )
    {
        FWE_LOG_ERROR( "Error reading from file" );
        return;
    }

    // search for $GPGGA line and extract data from it
    double lastValidLongitude = 0;
    double lastValidLatitude = 0;
    bool foundValid = false;
    int i = 0;
    while ( i < bytes - 7 )
    {
        if ( strncmp( "$GPGGA,", &mBuffer[i], 7 ) == 0 )
        {
            mGpggaLineCounter++;
            double longitudeRaw = HUGE_VAL;
            double latitudeRaw = HUGE_VAL;
            bool north = true;
            bool east = true;
            int processedBytes = extractLongAndLatitudeFromLine(
                &mBuffer[i + 7], static_cast<int>( bytes ) - ( i + 7 ), longitudeRaw, latitudeRaw, north, east );
            i += processedBytes;
            double longitude = convertDmmToDdCoordinates( longitudeRaw, east );
            double latitude = convertDmmToDdCoordinates( latitudeRaw, north );
            if ( validLatitude( latitude ) && validLongitude( longitude ) )
            {
                lastValidLongitude = longitude;
                lastValidLatitude = latitude;
                foundValid = true;
            }
        }
        i++;
    }

    // If values were found pass them on as Signals similar to CAN Signals
    if ( foundValid && mSignalBufferPtr != nullptr )
    {
        mValidCoordinateCounter++;
        auto timestamp = mClock->systemTimeSinceEpochMs();

        CollectedSignalsGroup collectedSignalsGroup;
        collectedSignalsGroup.push_back(
            CollectedSignal( getSignalIdFromStartBit( mLatitudeStartBit ), timestamp, lastValidLatitude ) );
        collectedSignalsGroup.push_back(
            CollectedSignal( getSignalIdFromStartBit( mLongitudeStartBit ), timestamp, lastValidLongitude ) );

        TraceModule::get().addToAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_SIGNALS, 2 );
        TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_DATA_FRAMES );

        if ( !mSignalBufferPtr->push( CollectedDataFrame( collectedSignalsGroup ) ) )
        {
            TraceModule::get().subtractFromAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_SIGNALS,
                                                           2 );
            TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_DATA_FRAMES );
            FWE_LOG_WARN( "Signal buffer full" );
        }
    }
    if ( mCyclicLoggingTimer.getElapsedMs().count() > CYCLIC_LOG_PERIOD_MS )
    {
        FWE_LOG_TRACE( "In the last " + std::to_string( CYCLIC_LOG_PERIOD_MS ) + " millisecond found " +
                       std::to_string( mGpggaLineCounter ) + " lines with $GPGGA and extracted " +
                       std::to_string( mValidCoordinateCounter ) + " valid coordinates from it" );
        mCyclicLoggingTimer.reset();
        mGpggaLineCounter = 0;
        mValidCoordinateCounter = 0;
    }
}

bool
IWaveGpsSource::validLatitude( double latitude )
{
    return ( latitude >= -90.0 ) && ( latitude <= 90.0 );
}
bool
IWaveGpsSource::validLongitude( double longitude )
{
    return ( longitude >= -180.0 ) && ( longitude <= 180.0 );
}

double
IWaveGpsSource::convertDmmToDdCoordinates( double dmm, bool positive )
{
    double degrees = floor( dmm / 100.0 );
    degrees += ( dmm / 100.0 - degrees ) * ( 100.0 / 60.0 );
    if ( !positive )
    {
        degrees *= -1.0;
    }
    return degrees;
}

int
IWaveGpsSource::extractLongAndLatitudeFromLine(
    const char *start, int limit, double &longitude, double &latitude, bool &north, bool &east )
{
    int commaCounter = 0;
    int lastCommaPosition = 0;
    int i = 0;
    for ( i = 0; i < limit; i++ )
    {
        // the line is comma separated
        if ( start[i] == ',' )
        {
            commaCounter++;
            // First comes latitude
            if ( commaCounter == 1 )
            {
                if ( i - lastCommaPosition > 1 )
                {
                    char *end = nullptr;
                    latitude = std::strtod( &start[i + 1], &end );
                    if ( end == &start[i + 1] )
                    {
                        latitude = HUGE_VAL;
                    }
                }
            }
            // Then 'N' or 'S' for north south
            else if ( commaCounter == 2 )
            {
                north = start[i + 1] == 'N';
            }

            // Then the longitude
            else if ( commaCounter == 3 )
            {
                if ( i - lastCommaPosition > 1 )
                {
                    char *end = nullptr;
                    longitude = std::strtod( &start[i + 1], &end );
                    if ( end == &start[i + 1] )
                    {
                        longitude = HUGE_VAL;
                    }
                }
            }
            // Then 'E' or 'W' for East or West
            else if ( commaCounter == 4 )
            {
                east = start[i + 1] == 'E';
                return i;
            }
            lastCommaPosition = i;
        }
    }
    return i;
}

bool
IWaveGpsSource::connect()
{
    mFileHandle = open( mPathToNmeaSource.c_str(), O_RDONLY | O_NOCTTY );
    if ( mFileHandle == -1 )
    {
        FWE_LOG_ERROR( "Could not open GPS NMEA file:" + mPathToNmeaSource );
        return false;
    }
    return true;
}

bool
IWaveGpsSource::disconnect()
{
    if ( close( mFileHandle ) != 0 )
    {
        return false;
    }
    mFileHandle = -1;
    return true;
}

} // namespace IoTFleetWise
} // namespace Aws
