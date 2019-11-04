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

#include "event_poller.hpp"

#include "event_dispatcher.hpp"

#include "interfaces.hpp"

#include "cstdmf/concurrency.hpp"
#include "cstdmf/profile.hpp"
#include "cstdmf/timestamp.hpp"

#ifndef _WIN32
#define HAS_EPOLL
#endif

#include <string.h>
#include <errno.h>

#if ENABLE_WATCHERS
namespace
{
ProfileVal g_idleProfile( "Idle" );
}
#endif // ENABLE_WATCHERS

namespace Mercury
{

// -----------------------------------------------------------------------------
// Section: EventPoller
// -----------------------------------------------------------------------------

/**
 *	Constructor.
 */
EventPoller::EventPoller() : 
	fdReadHandlers_(), 
	fdWriteHandlers_(), 
	spareTime_( 0 )
{
}


/**
 *	Destructor.
 */
EventPoller::~EventPoller()
{
}


/**
 *	This method registers a file descriptor for reading.
 */
bool EventPoller::registerForRead( int fd,
		InputNotificationHandler * handler )
{
	if (!this->doRegisterForRead( fd ))
	{
		return false;
	}

	fdReadHandlers_[ fd ] = handler;

	return true;
}


/**
 *	This method registers a file descriptor for writing.
 */
bool EventPoller::registerForWrite( int fd,
		InputNotificationHandler * handler )
{
	if (!this->doRegisterForWrite( fd ))
	{
		return false;
	}

	fdWriteHandlers_[ fd ] = handler;

	return true;
}


/**
 *	This method deregisters a file previously registered for reading.
 */
bool EventPoller::deregisterForRead( int fd )
{
	fdReadHandlers_.erase( fd );

	return this->doDeregisterForRead( fd );
}


/**
 *	This method deregisters a file previously registered for writing.
 */
bool EventPoller::deregisterForWrite( int fd )
{
	fdWriteHandlers_.erase( fd );

	return this->doDeregisterForWrite( fd );
}


/**
 *	This method is called when there is data to be read on the input file
 *	descriptor.
 */
void EventPoller::triggerRead( int fd )
{
	InputNotificationHandler * pHandler = fdReadHandlers_[ fd ];

	if (pHandler)
	{
		pHandler->handleInputNotification( fd );
	}
}


/**
 *	This method is called when the input file descriptor is available for
 *	writing.
 */
void EventPoller::triggerWrite( int fd )
{
	InputNotificationHandler * pHandler = fdWriteHandlers_[ fd ];

	if (pHandler)
	{
		pHandler->handleInputNotification( fd );
	}
}


/**
 *	This method returns whether or not a file descriptor is already registered.
 *	
 *	@param fd The file descriptor to test.
 *	@param isForRead Indicates whether checking for read or write registration.
 *
 *	@return True if registered, otherwise false.
 */
bool EventPoller::isRegistered( int fd, bool isForRead ) const
{
	const FDHandlers & handlers =
		isForRead ? fdReadHandlers_ : fdWriteHandlers_;

	return handlers.find( fd ) != handlers.end();
}


/**
 *	This method overrides the InputNotificationHandler. It is called when there
 *	are events to process.
 */
int EventPoller::handleInputNotification( int fd )
{

	this->processPendingEvents( 0.0 );

	return 0;
}


/**
 *	This method returns the file descriptor for this EventPoller or -1 if it
 *	does not have one. For epoll, this is used to register with the parent
 *	EventDispatcher's poller.
 */
int EventPoller::getFileDescriptor() const
{
	return -1;
}


/**
 *	Returns the largest file descriptor present in both the read or write 
 *	handler maps. If neither have entries, -1 is returned.
 */
int EventPoller::maxFD() const
{
	int readMaxFD = EventPoller::maxFD( fdReadHandlers_ );
	int writeMaxFD = EventPoller::maxFD( fdWriteHandlers_ );
	return std::max( readMaxFD, writeMaxFD );
}


/**
 *	Return the largest file descriptor present in the given map. If the given 
 *	map is empty, -1 is returned.
 */
int EventPoller::maxFD( const FDHandlers & handlerMap )
{
	int maxFD = -1;
	FDHandlers::const_iterator iFDHandler = handlerMap.begin();
	while (iFDHandler != handlerMap.end())
	{
		if (iFDHandler->first > maxFD)
		{
			maxFD = iFDHandler->first;
		}
		++iFDHandler;
	}
	return maxFD;
}


// -----------------------------------------------------------------------------
// Section: SelectPoller
// -----------------------------------------------------------------------------

/**
 *	This class is an EventPoller that uses select.
 */
class SelectPoller : public EventPoller
{
public:
	SelectPoller();

protected:
	virtual bool doRegisterForRead( int fd );
	virtual bool doRegisterForWrite( int fd );

	virtual bool doDeregisterForRead( int fd );
	virtual bool doDeregisterForWrite( int fd );

	virtual int processPendingEvents( double maxWait );

private:
	void handleInputNotifications( int &countReady,
		fd_set &readFDs, fd_set &writeFDs );

	void updateLargestFileDescriptor();

	// The file descriptor sets used in the select call.
	fd_set						fdReadSet_;
	fd_set						fdWriteSet_;

	// This is the largest registered file descriptor (read or write). It's used
	// in the call to select.
	int							fdLargest_;

	// This is the number of file descriptors registered for the write event.
	int							fdWriteCount_;
};


/**
 *	Constructor.
 */
SelectPoller::SelectPoller() :
	EventPoller(),
	fdReadSet_(),
	fdWriteSet_(),
	fdLargest_( -1 ),
	fdWriteCount_( 0 )
{
	// set up our list of file descriptors
	FD_ZERO( &fdReadSet_ );
	FD_ZERO( &fdWriteSet_ );
}


/**
 *  This method finds the highest file descriptor in the read and write sets
 *  and writes it to fdLargest_.
 */
void SelectPoller::updateLargestFileDescriptor()
{
	fdLargest_ = this->maxFD();
}


/**
 *  Trigger input notification handlers for ready file descriptors.
 */
void SelectPoller::handleInputNotifications( int &countReady,
	fd_set &readFDs, fd_set &writeFDs )
{
#ifdef _WIN32
	// X360 fd_sets don't look like POSIX ones, we know exactly what they are
	// and can just iterate over the provided FD arrays

	for (unsigned i=0; i < readFDs.fd_count; i++)
	{
		int fd = readFDs.fd_array[ i ];
		--countReady;
		this->triggerRead( fd );
	}

	for (unsigned i=0; i < writeFDs.fd_count; i++)
	{
		int fd = writeFDs.fd_array[ i ];
		--countReady;
		this->triggerWrite( fd );
	}

#else
	// POSIX fd_sets are more opaque and we just have to count up blindly until
	// we hit valid FD's with FD_ISSET

	for (int fd = 0; fd <= fdLargest_ && countReady > 0; ++fd)
	{
		if (FD_ISSET( fd, &readFDs ))
		{
			--countReady;
			this->triggerRead( fd );
		}

		if (FD_ISSET( fd, &writeFDs ))
		{
			--countReady;
			this->triggerWrite( fd );
		}
	}
#endif
}


/*
 *	Override from EventPoller.
 */
int SelectPoller::processPendingEvents( double maxWait )
{
	fd_set	readFDs;
	fd_set	writeFDs;
	struct timeval		nextTimeout;
	// Is this needed?
	FD_ZERO( &readFDs );
	FD_ZERO( &writeFDs );

	readFDs = fdReadSet_;
	writeFDs = fdWriteSet_;

	nextTimeout.tv_sec = (int)maxWait;
	nextTimeout.tv_usec =
		(int)( (maxWait - (double)nextTimeout.tv_sec) * 1000000.0 );

#if ENABLE_WATCHERS
	g_idleProfile.start();
#else
	uint64 startTime = ::timestamp();
#endif

	BWConcurrency::startMainThreadIdling();

	int countReady = 0;

#ifdef _WIN32
	if (fdLargest_ == -1)
	{
		// Windows can't handle it if we don't have any FDs to select on, but
		// we have a non-NULL timeout.
		Sleep( int( maxWait * 1000.0 ) );
	}
	else
#endif
	{
		countReady = select( fdLargest_+1, &readFDs,
				fdWriteCount_ ? &writeFDs : NULL, NULL, &nextTimeout );
	}

	BWConcurrency::endMainThreadIdling();

#if ENABLE_WATCHERS
	g_idleProfile.stop();
	spareTime_ += g_idleProfile.lastTime_;
#else
	spareTime_ += ::timestamp() - startTime;
#endif

	if (countReady > 0)
	{
		this->handleInputNotifications( countReady, readFDs, writeFDs );
	}
	else if (countReady == -1)
	{
		// TODO: Clean this up on shutdown
		// if (!breakProcessing_)
		{
			WARNING_MSG( "EventDispatcher::processContinuously: "
				"error in select(): %s\n", strerror( errno ) );
		}
	}

	return countReady;
}


/**
 *	This method implements a virtual method from EventPoller to do the real
 *	work of registering a file descriptor for reading.
 */
bool SelectPoller::doRegisterForRead( int fd )
{
#ifndef _WIN32
	if ((fd < 0) || (FD_SETSIZE <= fd))
	{
		ERROR_MSG( "EventDispatcher::registerFileDescriptor: "
			"Tried to register invalid fd %d. FD_SETSIZE (%d)\n",
			fd, FD_SETSIZE );

		return false;
	}
#endif

	// Bail early if it's already in the read set
	if (FD_ISSET( fd, &fdReadSet_ ))
		return false;

	FD_SET( fd, &fdReadSet_ );

	if (fd > fdLargest_)
	{
		fdLargest_ = fd;
	}

	return true;
}


/**
 *	This method implements a virtual method from EventPoller to do the real
 *	work of registering a file descriptor for writing.
 */
bool SelectPoller::doRegisterForWrite( int fd )
{
#ifndef _WIN32
	if ((fd < 0) || (FD_SETSIZE <= fd))
	{
		ERROR_MSG( "EventDispatcher::registerWriteFileDescriptor: "
			"Tried to register invalid fd %d. FD_SETSIZE (%d)\n",
			fd, FD_SETSIZE );

		return false;
	}
#endif

	if (FD_ISSET( fd, &fdWriteSet_ ))
	{
		return false;
	}

	FD_SET( fd, &fdWriteSet_ );

	if (fd > fdLargest_)
	{
		fdLargest_ = fd;
	}

	++fdWriteCount_;
	return true;
}


/**
 *	This method implements a virtual method from EventPoller to do the real
 *	work of deregistering a file descriptor for reading.
 */
bool SelectPoller::doDeregisterForRead( int fd )
{
#ifndef _WIN32
	if ((fd < 0) || (FD_SETSIZE <= fd))
	{
		return false;
	}
#endif

	if (!FD_ISSET( fd, &fdReadSet_ ))
	{
		return false;
	}

	FD_CLR( fd, &fdReadSet_ );

	if (fd == fdLargest_)
	{
		this->updateLargestFileDescriptor();
	}

	return true;
}


/**
 *	This method implements a virtual method from EventPoller to do the real
 *	work of deregistering a file descriptor for reading.
 */
bool SelectPoller::doDeregisterForWrite( int fd )
{
#ifndef _WIN32
	if ((fd < 0) || (FD_SETSIZE <= fd))
	{
		return false;
	}
#endif

	if (!FD_ISSET( fd, &fdWriteSet_ ))
	{
		return false;
	}

	FD_CLR( fd, &fdWriteSet_ );

	if (fd == fdLargest_)
	{
		this->updateLargestFileDescriptor();
	}

	--fdWriteCount_;
	return true;
}


// -----------------------------------------------------------------------------
// Section: EPoller
// -----------------------------------------------------------------------------

// TODO: Probably not on the PS3 too.
#ifdef HAS_EPOLL
#include <sys/epoll.h>


/**
 *	This class is an EventPoller that uses epoll.
 */
class EPoller : public EventPoller
{
public:
	EPoller( int expectedSize = 10 );
	virtual ~EPoller();

	int getFileDescriptor() const { return epfd_; }

protected:
	virtual bool doRegisterForRead( int fd )
		{ return this->doRegister( fd, true, true ); }

	virtual bool doRegisterForWrite( int fd )
		{ return this->doRegister( fd, false, true ); }

	virtual bool doDeregisterForRead( int fd )
		{ return this->doRegister( fd, true, false ); }

	virtual bool doDeregisterForWrite( int fd )
		{ return this->doRegister( fd, false, false ); }

	virtual int processPendingEvents( double maxWait );

	bool doRegister( int fd, bool isRead, bool isRegister );

private:
	// epoll file descriptor
	int epfd_;
};


/**
 *	Constructor.
 */
EPoller::EPoller( int expectedSize ) :
	epfd_( epoll_create( expectedSize ) )
{
	if (epfd_ == -1)
	{
		CRITICAL_MSG( "EPoller::EPoller: epoll_create failed: %s\n",
				strerror( errno ) );
	}
};


/**
 *	Destructor.
 */
EPoller::~EPoller()
{
	if (epfd_ != -1)
	{
		close( epfd_ );
	}
}


/**
 *	This method does the work registering and deregistering file descriptors
 *	from the epoll descriptor.
 */
bool EPoller::doRegister( int fd, bool isRead, bool isRegister )
{
	struct epoll_event ev;
	memset( &ev, 0, sizeof( ev ) ); // stop valgrind warning
	int op;

	ev.data.fd = fd;

	// Handle the case where the file is already registered for the opposite
	// action.
	if (this->isRegistered( fd, !isRead ))
	{
		op = EPOLL_CTL_MOD;

		ev.events = isRegister ? EPOLLIN|EPOLLOUT :
					isRead ? EPOLLOUT : EPOLLIN;
	}
	else
	{
		// TODO: Could be good to use EPOLLET (leave like select for now).
		ev.events = isRead ? EPOLLIN : EPOLLOUT;
		op = isRegister ? EPOLL_CTL_ADD : EPOLL_CTL_DEL;
	}

	if (epoll_ctl( epfd_, op, fd, &ev ) < 0)
	{
		ERROR_MSG( "EPoller::doRegister: Failed to %s %s file descriptor %d "
					"(%s)\n",
				isRegister ? "add" : "remove",
				isRead ? "read" : "write",
				fd,
				strerror( errno ) );

		return false;
	}

	return true;
}


/*
 *	Override from EventPoller.
 */
int EPoller::processPendingEvents( double maxWait )
{
	const int MAX_EVENTS = 10;

	struct epoll_event events[ MAX_EVENTS ];

	int maxWaitInMilliseconds = int( ceil( maxWait * 1000 ) );

#if ENABLE_WATCHERS
	g_idleProfile.start();
#else
	uint64 startTime = ::timestamp();
#endif

	BWConcurrency::startMainThreadIdling();
	int nfds = epoll_wait( epfd_, events, MAX_EVENTS, maxWaitInMilliseconds );
	BWConcurrency::endMainThreadIdling();

#if ENABLE_WATCHERS
	g_idleProfile.stop();
	spareTime_ += g_idleProfile.lastTime_;
#else
	spareTime_ += ::timestamp() - startTime;
#endif

	for (int i = 0; i < nfds; ++i)
	{
		if (events[i].events & (EPOLLIN|EPOLLERR|EPOLLHUP))
		{
			this->triggerRead( events[i].data.fd );
		}

		if (events[i].events & EPOLLOUT)
		{
			this->triggerWrite( events[i].data.fd );
		}
	}

	return nfds;
}

#endif // HAS_EPOLL


// -----------------------------------------------------------------------------
// Section: EventPoller::create
// -----------------------------------------------------------------------------

/**
 *	This static method creates an appropriate EventPoller. It may use select or
 *	epoll.
 */
EventPoller * EventPoller::create()
{
#ifdef HAS_EPOLL
	return new EPoller();
#else
	return new SelectPoller();
#endif // HAS_EPOLL
}

} // namespace Mercury

// event_poller.cpp
