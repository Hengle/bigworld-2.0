/******************************************************************************
BigWorld Technology 
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

WARNING: This computer program is protected by copyright law and international
treaties. Unauthorized use, reproduction or distribution of this program, or
any portion of this program, may result in the imposition of civil and
criminal penalties as provided by law.
******************************************************************************/

#include "pch.hpp"

#include "error_reporter.hpp"

#include "basictypes.hpp"
#include "event_dispatcher.hpp"

#include "nub_exception.hpp"

#include "cstdmf/timestamp.hpp"

#include <sstream>

namespace Mercury
{

/**
 *	The minimum time that an exception can be reported from when it was first
 *	reported.
 */
const uint ErrorReporter::ERROR_REPORT_MIN_PERIOD_MS = 2000; // 2 seconds

/**
 *	The nominal maximum time that a report count for a Mercury address and
 *	error is kept after the last raising of the error.
 */
const uint ErrorReporter::ERROR_REPORT_COUNT_MAX_LIFETIME_MS = 10000; // 10 seconds

/**
 *	Constructor.
 */
ErrorReporter::ErrorReporter( EventDispatcher & dispatcher ) :
	reportLimitTimerHandle_(),
	errorsAndCounts_()
{
	// report any pending exceptions every so often
	reportLimitTimerHandle_ = dispatcher.addTimer(
							ERROR_REPORT_MIN_PERIOD_MS * 1000, this );
}


/**
 *	Destructor.
 */
ErrorReporter::~ErrorReporter()
{
	reportLimitTimerHandle_.cancel();
}


std::string ErrorReporter::addressErrorToString( const Address & address,
		const std::string & errorString )
{
	std::ostringstream out;
	out << address.c_str() << ": " << errorString;
	return out.str();
}


std::string ErrorReporter::addressErrorToString(
		const Address & address,
		const std::string & errorString,
		const ErrorReportAndCount & reportAndCount,
		const uint64 & now )
{
	int64 deltaStamps = now - reportAndCount.lastReportStamps;
	double deltaMillis = 1000 * deltaStamps / stampsPerSecondD();

	char * buf = NULL;
	int bufLen = 64;
	int strLen = bufLen;
	do
	{
		bufLen = strLen + 1;
		delete [] buf;
		buf = new char[ bufLen ];
#ifdef _WIN32
		strLen = _snprintf( buf, bufLen, "%d reports of '%s' "
				"in the last %.00fms",
			reportAndCount.count,
			addressErrorToString( address, errorString ).c_str(),
			deltaMillis );
		if (strLen == -1) strLen = (bufLen - 1) * 2;
#else
		strLen = snprintf( buf, bufLen, "%d reports of '%s' "
				"in the last %.00fms",
			reportAndCount.count,
			addressErrorToString( address, errorString ).c_str(),
			deltaMillis );
#endif
	} while (strLen >= bufLen);

	std::string out( buf );
	delete [] buf;
	return out;
}


/**
 *	Report a general error with printf style format string. If repeatedly the
 *	resulting formatted string is reported within the minimum output window,
 *	they are accumulated and output after the minimum output window has passed.
 */
void ErrorReporter::reportError(
		const Address & address, const char* format, ... )
{
	char * buf = NULL;
	int bufLen = 32;
	int strLen = bufLen;
	do
	{
		delete [] buf;
		bufLen = strLen + 1;
		buf = new char[ bufLen ];

		va_list va;
		va_start( va, format );
#ifdef _WIN32
		strLen = _vsnprintf( buf, bufLen, format, va );
		if (strLen == -1) strLen = (bufLen - 1) * 2;
#else
		strLen = vsnprintf( buf, bufLen, format, va );
#endif
		va_end( va );
		buf[bufLen -1] = '\0';

	} while (strLen >= bufLen );

	std::string error( buf );

	delete [] buf;

	this->addReport( address, error );
}


/**
 *
 */
void ErrorReporter::reportException( Reason reason,
		const Address & addr,
		const char* prefix )
{
	NubException ne( reason, addr );
	this->reportException( ne, prefix );

}


/**
 *	Output the exception if it has not occurred before, otherwise only
 *	output the exception if the minimum period has elapsed since the
 *	last outputting of this exception.
 *
 *	@param ne 		the NubException
 *	@param prefix 	any prefix to add to the error message, or NULL if no prefix
 *
 */
void ErrorReporter::reportException( const NubException & ne, const char* prefix )
{
	Address offender( 0, 0 );
	ne.getAddress( offender );

	if (prefix)
	{
		this->reportError( offender,
			"%s: Exception occurred: %s",
			prefix, reasonToString( ne.reason() ) );
	}
	else
	{
		this->reportError( offender, "Exception occurred: %s",
				reasonToString( ne.reason() ) );
	}
}


/**
 *	Adds a new error message for an address to the reporter count map.
 *	Emits an error message if there has been no previous equivalent error
 *	string provider for this address.
 */
void ErrorReporter::addReport( const Address & address, const std::string & errorString )
{
	AddressAndErrorString addressError( address, errorString );
	ErrorsAndCounts::iterator searchIter =
		errorsAndCounts_.find( addressError );

	uint64 now = timestamp();
	// see if we have ever reported this error
	if (searchIter != errorsAndCounts_.end())
	{
		// this error has been reported recently..
		ErrorReportAndCount & reportAndCount = searchIter->second;
		reportAndCount.count++;

		int64 millisSinceLastReport = 1000 *
			(now - reportAndCount.lastReportStamps) /
			stampsPerSecond();

		reportAndCount.lastRaisedStamps = now;

		if (millisSinceLastReport >= ERROR_REPORT_MIN_PERIOD_MS)
		{
			ERROR_MSG( "%s\n",
				addressErrorToString( address, errorString,
					reportAndCount, now ).c_str() );
			reportAndCount.count = 0;
			reportAndCount.lastReportStamps = now;
		}

	}
	else
	{
		ERROR_MSG( "%s\n",
			addressErrorToString( address, errorString ).c_str() );

		ErrorReportAndCount reportAndCount = {
			now, 	// lastReportStamps,
			now, 	// lastRaisedStamps,
			0,		// count
		};

		errorsAndCounts_[ addressError ] = reportAndCount;
	}
}


/**
 *	Output all exception's reports that have not yet been output.
 */
void ErrorReporter::reportPendingExceptions( bool reportBelowThreshold )
{
	uint64 now = timestamp();

	// this is set to any iterator slated for removal
	ErrorsAndCounts::iterator staleIter = this->errorsAndCounts_.end();

	for (	ErrorsAndCounts::iterator exceptionCountIter =
				this->errorsAndCounts_.begin();
			exceptionCountIter != this->errorsAndCounts_.end();
			++exceptionCountIter )
	{
		// remove any stale mappings from the last loop's run
		if (staleIter != this->errorsAndCounts_.end())
		{
			this->errorsAndCounts_.erase( staleIter );
			staleIter = this->errorsAndCounts_.end();
		}

		// check this iteration's last report and see if we need to output
		// anything
		const AddressAndErrorString & addressError =
			exceptionCountIter->first;
		ErrorReportAndCount & reportAndCount = exceptionCountIter->second;

		int64 millisSinceLastReport = 1000 *
			(now - reportAndCount.lastReportStamps) / stampsPerSecond();
		if (reportBelowThreshold ||
				millisSinceLastReport >= ERROR_REPORT_MIN_PERIOD_MS)
		{
			if (reportAndCount.count)
			{
				ERROR_MSG( "%s\n",
					addressErrorToString(
						addressError.first, addressError.second,
						reportAndCount, now ).c_str()
				);
				reportAndCount.count = 0;
				reportAndCount.lastReportStamps = now;

			}
		}

		// see if we can remove this mapping if it has not been raised in a
		// while
		uint64 sinceLastRaisedMillis = 1000 * (now - reportAndCount.lastRaisedStamps) /
			stampsPerSecond();
		if (sinceLastRaisedMillis > ERROR_REPORT_COUNT_MAX_LIFETIME_MS)
		{
			// it's hung around for too long without being raised again,
			// so remove it in the next iteration
			staleIter = exceptionCountIter;
		}
	}

	// remove the last mapping if it is marked stale
	if (staleIter != this->errorsAndCounts_.end())
	{
		this->errorsAndCounts_.erase( staleIter );
	}
}


/**
 *	This method handles the timer event and checks to see if any delayed
 *	messages should be reported.
 */
void ErrorReporter::handleTimeout( TimerHandle handle, void * arg )
{
	MF_ASSERT( handle == reportLimitTimerHandle_ );

	this->reportPendingExceptions();
}

} // namespace Mercury

// error_reporter.cpp
