/*
FatRat download manager
http://fatrat.dolezel.info

Copyright (C) 2006-2008 Lubos Dolezel <lubos a dolezel.info>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/


#include "CurlPoller.h"
#include <QtDebug>
#include <sys/epoll.h>
#include <errno.h>

CurlPoller* CurlPoller::m_instance = 0;

static const int TRANSFER_TIMEOUT = 20;

CurlPoller::CurlPoller()
	: m_bAbort(false), m_usersLock(QMutex::Recursive)
{
	if(m_instance)
		abort();
	
	curl_global_init(CURL_GLOBAL_SSL);
	m_curlm = curl_multi_init();
	m_epoll = epoll_create(20);
	
	curl_multi_setopt(m_curlm, CURLMOPT_SOCKETFUNCTION, socket_callback);
	curl_multi_setopt(m_curlm, CURLMOPT_SOCKETDATA, this);
	
	m_instance = this;
	start();
}

CurlPoller::~CurlPoller()
{
	m_bAbort = true;
	
	if(isRunning())
		wait();
	
	m_instance = 0;
	curl_multi_cleanup(m_curlm);
	curl_global_cleanup();
	close(m_epoll);
}

bool operator<(const timeval& t1, const timeval& t2)
{
	if(t1.tv_sec < t2.tv_sec)
		return true;
	else if(t1.tv_sec > t2.tv_sec)
		return false;
	else
		return t1.tv_usec < t2.tv_usec;
}

void CurlPoller::run()
{
	long timeout = 0, curl_timeout;
	
	curl_multi_setopt(m_curlm, CURLMOPT_TIMERFUNCTION, timer_callback);
	curl_multi_setopt(m_curlm, CURLMOPT_TIMERDATA, &curl_timeout);
	
	while(!m_bAbort)
	{
		epoll_event events[20];
		int nfds, dummy;
		timeval tvNow;
		QList<CurlUser*> timedOut;
		
		nfds = epoll_wait(m_epoll, events, sizeof(events)/sizeof(events[0]), timeout);
		
		if(!nfds)
		{
			//qDebug() << "No events";
			curl_multi_socket_action(m_curlm, CURL_SOCKET_TIMEOUT, 0, &dummy);
		}
		
		for(int i=0;i<nfds;i++)
		{
			int mask = 0;
			int fd = events[i].data.fd;
			
			if(events[i].events & EPOLLIN)
				mask |= CURL_CSELECT_IN;
			if(events[i].events & EPOLLOUT)
				mask |= CURL_CSELECT_OUT;
			if(events[i].events & (EPOLLERR | EPOLLHUP))
				mask |= CURL_CSELECT_ERR;
			
			//qDebug() << "Events:" << mask;
			curl_multi_socket_action(m_curlm, fd, mask, &dummy);
		}
		
		gettimeofday(&tvNow, 0);
		
		QMutexLocker locker(&m_usersLock);
		
		if(curl_timeout <= 0 || curl_timeout > 500)
			timeout = 500;
		else
			timeout = curl_timeout;
		
		for(QHash<int,QPair<int, CurlUser*> >::iterator it = m_sockets.begin(); it != m_sockets.end(); it++)
		{
			int mask = 0;
			CurlUser* user = it.value().second;
			timeval lastOp = user->lastOperation();
			
			int seconds = tvNow.tv_sec - lastOp.tv_sec;
			
			if(seconds > TRANSFER_TIMEOUT)
				timedOut << user;
			else if(seconds > 1)
			{
				CurlUser::read_function(0, 0, 0, user);
				CurlUser::write_function(0, 0, 0, user);
			}
			
			if(user->hasNextReadTime())
			{
				if(user->nextReadTime() < tvNow)
					mask |= CURL_CSELECT_IN;
			}
			if(user->hasNextWriteTime())
			{
				if(user->nextWriteTime() < tvNow)
					mask |= CURL_CSELECT_OUT;
			}
			
			if(mask)
				curl_multi_socket_action(m_curlm, it.key(), mask, &dummy);
		}
		for(QHash<int,QPair<int, CurlUser*> >::iterator it = m_sockets.begin(); it != m_sockets.end(); it++)
		{
			int msec = -1;
			CurlUser* user = it.value().second;
			
			if(user->hasNextReadTime())
			{
				timeval tv = user->nextReadTime();
				msec = (tv.tv_sec-tvNow.tv_sec)*1000 + (tv.tv_usec-tvNow.tv_usec)/1000;
			}
			if(user->hasNextWriteTime())
			{
				int mmsec;
				timeval tv = user->nextWriteTime();
				mmsec = (tv.tv_sec-tvNow.tv_sec)*1000 + (tv.tv_usec-tvNow.tv_usec)/1000;
				
				if(mmsec < msec || msec < 0)
					msec = mmsec;
			}
			
			if(msec > 0)
			{
				if(msec < timeout)
					timeout = msec;
			}
			else
			{
				int& flags = it.value().first;
				if(user->performsLimiting())
					flags |= EPOLLONESHOT;
				else if(flags & EPOLLONESHOT)
					flags ^= EPOLLONESHOT;
				epollEnable(it.key(), flags);
			}
		}
		
		foreach(CurlUser* user, timedOut)
			user->transferDone(CURLE_OPERATION_TIMEDOUT);
		
		while(CURLMsg* msg = curl_multi_info_read(m_curlm, &dummy))
		{
			if(msg->msg != CURLMSG_DONE)
				continue;
			
			CurlUser* user = m_users[msg->easy_handle];
			
			if(user)
				user->transferDone(msg->data.result);
		}
	}
}

void CurlPoller::epollEnable(int socket, int events)
{
	epoll_event event;
	
	event.events = events;
	event.data.fd = socket;
	
	epoll_ctl(m_epoll, EPOLL_CTL_MOD, socket, &event);
}

int CurlPoller::timer_callback(CURLM* multi, long newtimeout, long* timeout)
{
	*timeout = newtimeout;
	return 0;
}

int CurlPoller::socket_callback(CURL* easy, curl_socket_t s, int action, CurlPoller* This, void* socketp)
{
	epoll_event event;
	event.events = EPOLLONESHOT;
	event.data.fd = s;
	
	if(action == CURL_POLL_IN || action == CURL_POLL_INOUT)
		event.events |= EPOLLIN;
	if(action == CURL_POLL_OUT || action == CURL_POLL_INOUT)
		event.events |= EPOLLOUT;
	
	if(action == CURL_POLL_REMOVE)
	{
		qDebug() << "CurlPoller::socket_callback - remove";
		
		This->m_sockets.remove(s);
		epoll_ctl(This->m_epoll, EPOLL_CTL_DEL, s, 0);
	}
	else
	{
		qDebug() << "CurlPoller::socket_callback - add/mod";
		
		This->m_sockets[s] = QPair<int,CurlUser*>(event.events, This->m_users[easy]);
		
		if(epoll_ctl(This->m_epoll, EPOLL_CTL_MOD, s, &event))
		{
			if(errno == ENOENT)
			{
				if(epoll_ctl(This->m_epoll, EPOLL_CTL_ADD, s, &event))
					return errno;
			}
			else
				return errno;
		}
	}
	
	return 0;
}

void CurlPoller::addTransfer(CurlUser* obj)
{
	QMutexLocker locker(&m_usersLock);
	
	qDebug() << "CurlPoller::addTransfer" << obj;
	
	obj->resetStatistics();
	CURL* handle = obj->curlHandle();
	m_users[handle] = obj;
	curl_multi_add_handle(m_curlm, handle);
}

void CurlPoller::removeTransfer(CurlUser* obj)
{
	QMutexLocker locker(&m_usersLock);
	
	qDebug() << "CurlPoller::removeTransfer" << obj;
	
	CURL* handle = obj->curlHandle();
	curl_multi_remove_handle(m_curlm, handle);
	m_users.remove(handle);
}
