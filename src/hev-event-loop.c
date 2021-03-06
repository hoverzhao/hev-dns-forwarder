/*
 ============================================================================
 Name        : hev-event-loop.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2013 everyone.
 Description : An event loop
 ============================================================================
 */

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "hev-slist.h"
#include "hev-event-loop.h"

struct _HevEventLoop
{
	int epoll_fd;
	unsigned int ref_count;

	bool run;
	HevSList *sources;
	HevSList *fd_list;
};

HevEventLoop *
hev_event_loop_new (void)
{
	HevEventLoop *self = HEV_MEMORY_ALLOCATOR_ALLOC (sizeof (HevEventLoop));

	if (self) {
		self->epoll_fd = epoll_create (1024);
		self->ref_count = 1;
		self->run = true;
		self->sources = NULL;
		self->fd_list = NULL;
	}

	return self;
}

HevEventLoop *
hev_event_loop_ref (HevEventLoop *self)
{
    if (self)
        self->ref_count ++;
	return self;
}

static void
fd_list_free_handler (void *data)
{
	HevEventSourceFD *fd = data;
	_hev_event_source_fd_dispatch_finish (fd);
}

static void
sources_free_handler (void *data)
{
	HevEventSource *source = data;
	_hev_event_source_set_loop (source, NULL);
	hev_event_source_unref (source);
}

void
hev_event_loop_unref (HevEventLoop *self)
{
    if (!self)
        return;
    
	self->ref_count --;
	if (0 < self->ref_count)
	  return;

	hev_slist_free_notify (self->fd_list, fd_list_free_handler);
	hev_slist_free_notify (self->sources, sources_free_handler);
	close (self->epoll_fd);
	HEV_MEMORY_ALLOCATOR_FREE (self);
}

static HevSList *
insert_event_source_fd_sorted (HevSList *fd_list, HevEventSourceFD *fd)
{
	HevSList *list = NULL, *prev = NULL;

	for (list=fd_list; list; prev=list,list=hev_slist_next (list)) {
		HevEventSourceFD *_fd  = hev_slist_data (list);
		if (hev_event_source_get_priority (fd->source) >
				hev_event_source_get_priority (_fd->source))
		  break;
	}
	return hev_slist_insert_after (fd_list, fd, prev);
}

static void
invalid_sources_free_handler (void *data)
{
	HevEventSource *source = data;
	hev_event_loop_del_source (source->loop, source);
}

static inline int
dispatch_events (HevEventLoop *self)
{
	HevSList *invalid_sources = NULL;
	HevEventSourceFD *fd;
	HevEventSource *source;

	if (!self || !self->fd_list)
	  return -1;

	/* get highest priority source fd, check & dispatch */
	fd = hev_slist_data (self->fd_list);
	source = fd->source;
	if (source && (hev_event_source_get_loop (source) == self) &&
				source->funcs.check (source, fd)) {
		bool res = source->funcs.dispatch (source, fd,
					source->callback.callback, source->callback.data);
		/* recheck, in user's dispatch, source and fd may be remove. */
		if (fd->source) {
			if (res) {
				if (hev_event_source_get_loop (source) == self)
				  source->funcs.prepare (source);
			} else {
				fd->revents = 0;
				invalid_sources = hev_slist_prepend (invalid_sources, source);
			}
		}
	}

	if (!(fd->_events & fd->revents) || !fd->source) {
		self->fd_list = hev_slist_remove (self->fd_list, fd);
		_hev_event_source_fd_dispatch_finish (fd);
	}

	/* delete invalid sources */
	if (invalid_sources)
	  hev_slist_free_notify (invalid_sources, invalid_sources_free_handler);

	return 0;
}

void
hev_event_loop_run (HevEventLoop *self)
{
	int timeout = -1;
    if (!self)
        return;

	while (self->run) {
		int i = 0, nfds = 0;
		struct epoll_event events[256];

		/* waiting events */
		nfds = epoll_wait (self->epoll_fd, events, 256, timeout);
		if (-1 == nfds && EINTR != errno) {
			fprintf (stderr, "EPoll wait failed!\n");
			break;
		}

		/* insert to fd_list, sorted by source priority (highest ... lowest) */
		for (i=0; i<nfds; i++) {
			HevEventSourceFD *fd = events[i].data.ptr;
			fd->revents |= events[i].events;
			if (fd->_dispatched)
			  continue;
			_hev_event_source_fd_dispatch (fd);
			self->fd_list = insert_event_source_fd_sorted (self->fd_list, fd);
		}

		/* dispatch */
		timeout = dispatch_events (self);
	}
}

void
hev_event_loop_quit (HevEventLoop *self)
{
    if (self)
        self->run = false;
}

bool
hev_event_loop_add_source (HevEventLoop *self, HevEventSource *source)
{
	HevSList *list = NULL;

    if(!self || !source)
        return false;

	if (source->loop == self)
	  return false;

	_hev_event_source_set_loop (source, self);
	self->sources = hev_slist_prepend (self->sources, hev_event_source_ref (source));
	for (list=source->fds; list; list=hev_slist_next (list)) {
		HevEventSourceFD *fd = hev_slist_data (list);
		_hev_event_loop_add_fd (self, fd);
	}
	source->funcs.prepare (source);

	return true;
}

bool
hev_event_loop_del_source (HevEventLoop *self, HevEventSource *source)
{
	HevSList *list = NULL;

    if(!self || !source)
        return false;

	if (source->loop != self)
	  return false;

	_hev_event_source_set_loop (source, NULL);
	self->sources = hev_slist_remove (self->sources, source);
	for (list=source->fds; list; list=hev_slist_next (list)) {
		HevEventSourceFD *fd = hev_slist_data (list);
		_hev_event_loop_del_fd (self, fd);
	}
	hev_event_source_unref (source);

	return true;
}

bool
_hev_event_loop_add_fd (HevEventLoop *self, HevEventSourceFD *fd)
{
	if (self && fd) {
		struct epoll_event event;
		event.events = fd->_events | EPOLLET;
		event.data.ptr = fd;
		return (0 == epoll_ctl (self->epoll_fd,
					EPOLL_CTL_ADD, fd->fd, &event));
	}

	return false;
}

bool
_hev_event_loop_del_fd (HevEventLoop *self, HevEventSourceFD *fd)
{
	if (self && fd) {
		return (0 == epoll_ctl (self->epoll_fd,
					EPOLL_CTL_DEL, fd->fd, NULL));
	}

	return false;
}

