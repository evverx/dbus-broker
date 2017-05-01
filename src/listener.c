/*
 * Socket Listener
 */

#include <c-list.h>
#include <c-macro.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "bus.h"
#include "listener.h"
#include "util/dispatch.h"
#include "util/error.h"

/**
 * listener_init_with_fd() - XXX
 */
int listener_init_with_fd(Listener *listener,
                          Bus *bus,
                          DispatchFn dispatch_fn,
                          int socket_fd) {
        _c_cleanup_(listener_deinitp) Listener *l = listener;
        int r;

        *l = (Listener)LISTENER_NULL(*l);

        r = dispatch_file_init(&l->socket_file,
                               &bus->dispatcher,
                               &bus->ready_list,
                               dispatch_fn,
                               socket_fd,
                               EPOLLIN);
        if (r)
                return error_fold(r);

        dispatch_file_select(&l->socket_file, EPOLLIN);
        c_list_link_tail(&bus->listener_list, &l->bus_link);

        l->socket_fd = socket_fd;
        l = NULL;
        return 0;
}

/**
 * listener_deinit() - XXX
 */
void listener_deinit(Listener *listener) {
        assert(c_list_is_empty(&listener->peer_list));
        c_list_unlink_init(&listener->bus_link);
        dispatch_file_deinit(&listener->socket_file);
        listener->socket_fd = c_close(listener->socket_fd);
}

/**
 * listener_accept() - XXX
 */
int listener_accept(Listener *listener, int *fdp) {
        int fd;

        fd = accept4(listener->socket_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
                if (errno == EAGAIN) {
                        /*
                         * EAGAIN implies there are no pending incoming
                         * connections. Catch this, clear EPOLLIN and tell the
                         * caller about it.
                         */
                        dispatch_file_clear(&listener->socket_file, EPOLLIN);
                } else {
                        /*
                         * The linux UDS layer does not return pending errors
                         * on the child socket (unlike the TCP layer). Hence,
                         * there are no known errors to check for.
                         */
                        return error_origin(-errno);
                }
        }

        *fdp = fd;
        return 0;
}
