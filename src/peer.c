/*
 * Peers
 */

#include <c-dvar.h>
#include <c-dvar-type.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bus.h"
#include "dbus/message.h"
#include "dbus/protocol.h"
#include "dbus/socket.h"
#include "dbus/unique-name.h"
#include "driver.h"
#include "match.h"
#include "peer.h"
#include "reply.h"
#include "user.h"
#include "util/dispatch.h"
#include "util/error.h"
#include "util/fdlist.h"

int peer_queue_message(Peer *receiver, Peer *sender, uint32_t serial, Message *message) {
        _c_cleanup_(reply_slot_freep) ReplySlot *slot = NULL;
        int r;

        if ((message->header->type == DBUS_MESSAGE_TYPE_METHOD_CALL) &&
            !(message->header->flags & DBUS_HEADER_FLAG_NO_REPLY_EXPECTED)) {
                /* XXX: handle duplicate serial numbers */
                r = reply_slot_new(&slot, &receiver->replies_outgoing, sender, serial);
                if (r)
                        return error_fold(r);
        }

        r = connection_queue_message(&receiver->connection, message);
        if (r)
                return error_fold(r);

        slot = NULL;
        return 0;
}

static int peer_forward_unicast(Peer *sender, const char *destination, uint32_t serial, Message *message) {
        Peer *receiver;
        int r;

        if (*destination != ':') {
                NameEntry *name;
                NameOwner *owner;

                name = name_registry_find_entry(&sender->bus->names, destination);
                if (!name)
                        return -EBADMSG;

                owner = c_list_first_entry(&name->owners, NameOwner, entry_link);
                if (!owner) {
                        r = name_entry_queue_message(name, message);
                        if (r)
                                return error_fold(r);

                        /* XXX: activate name */

                        return 0;
                } else {
                        receiver = owner->peer;
                }
        } else {
                uint64_t id;

                r = unique_name_to_id(destination, &id);
                if (r)
                        return error_trace(r);

                receiver = peer_registry_find_peer(&sender->bus->peers, id);
                if (!receiver)
                        return -EBADMSG;
        }

        r = peer_queue_message(receiver, sender, serial, message);
        if (r)
                return error_trace(r);

        return 0;
}

static int peer_forward_reply(Peer *sender, const char *destination, uint32_t reply_serial, Message *message) {
        ReplySlot *slot;
        uint64_t id;
        int r;

        r = unique_name_to_id(destination, &id);
        if (r < 0)
                return r;

        slot = reply_slot_get_by_id(&sender->replies_outgoing, id, reply_serial);
        if (!slot)
                return -EBADMSG;

        r = connection_queue_message(&slot->sender->connection, message);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        reply_slot_free(slot);

        return 0;
}

static int peer_forward_broadcast_to_matches(MatchRegistry *matches, MatchFilter *filter, Message *message) {
        MatchRule *rule;
        int r;

        for (rule = match_rule_next(matches, NULL, filter); rule; match_rule_next(matches, rule, filter)) {

                r = connection_queue_message(&rule->peer->connection, message);
                if (r)
                        return (r > 0) ? -ENOTRECOVERABLE : r;
        }

        return 0;
}

static int peer_forward_broadcast(Peer *sender, const char *interface, const char *member, const char *path, const char *siganture, Message *message) {
        MatchFilter filter = {
                .type = message->header->type,
                .interface = interface,
                .member = member,
                .path = path,
        };
        int r;

        /* XXX: parse the message to verify the marshalling and read out the arguments for filtering */

        r = peer_forward_broadcast_to_matches(&sender->bus->wildcard_matches, &filter, message);
        if (r < 0)
                return r;

        for (CRBNode *node = c_rbtree_first(&sender->names); node; c_rbnode_next(node)) {
                NameOwner *owner = c_container_of(node, NameOwner, rb);

                if (!name_owner_is_primary(owner))
                        continue;

                r = peer_forward_broadcast_to_matches(&owner->entry->matches, &filter, message);
                if (r < 0)
                        return r;
        }

        r = peer_forward_broadcast_to_matches(&sender->matches, &filter, message);
        if (r < 0)
                return r;

        return 0;
}

static int peer_dispatch_message(Peer *peer, Message *message) {
        MessageMetadata metadata;
        int r;

        r = message_parse_metadata(message, &metadata);
        if (r)
                return error_fold(r); /* XXX: >0 should result in disconnect */

        if (_c_unlikely_(c_string_equal(metadata.fields.destination, "org.freedesktop.DBus")))
                return driver_dispatch(peer,
                                       metadata.header.serial,
                                       metadata.fields.interface,
                                       metadata.fields.member,
                                       metadata.fields.path,
                                       metadata.fields.signature,
                                       message);

        /* XXX: append sender */

        if (message->header->type != DBUS_MESSAGE_TYPE_METHOD_CALL)
                message->header->flags |= DBUS_HEADER_FLAG_NO_REPLY_EXPECTED;

        if (!metadata.fields.destination) {
                if (metadata.header.type != DBUS_MESSAGE_TYPE_SIGNAL)
                        return -EBADMSG;

                return peer_forward_broadcast(peer,
                                              metadata.fields.interface,
                                              metadata.fields.member,
                                              metadata.fields.path,
                                              metadata.fields.signature,
                                              message);
        }

        switch (metadata.header.type) {
        case DBUS_MESSAGE_TYPE_SIGNAL:
        case DBUS_MESSAGE_TYPE_METHOD_CALL:
                return peer_forward_unicast(peer,
                                            metadata.fields.destination,
                                            metadata.header.serial,
                                            message);
        case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        case DBUS_MESSAGE_TYPE_ERROR:
                return peer_forward_reply(peer,
                                          metadata.fields.destination,
                                          metadata.fields.reply_serial,
                                          message);
        }

        return 0;
}

int peer_dispatch(DispatchFile *file, uint32_t mask) {
        Peer *peer = c_container_of(file, Peer, connection.socket_file);
        int r;

        if (dispatch_file_is_ready(file, EPOLLIN)) {
                r = connection_dispatch(&peer->connection, EPOLLIN);
                if (r)
                        return r;
        }

        if (dispatch_file_is_ready(file, EPOLLHUP)) {
                r = connection_dispatch(&peer->connection, EPOLLHUP);
                if (r)
                        return r;
        }

        for (;;) {
                _c_cleanup_(message_unrefp) Message *m = NULL;

                r = connection_dequeue(&peer->connection, &m);
                if (r)
                        return r;
                if (!m)
                        break;

                r = peer_dispatch_message(peer, m);
                if (r)
                        return r;
        }

        if (dispatch_file_is_ready(file, EPOLLOUT)) {
                r = connection_dispatch(&peer->connection, EPOLLOUT);
                if (r)
                        return r;
        }

        return 0;
}

static int peer_get_peersec(int fd, char **labelp, size_t *lenp) {
        _c_cleanup_(c_freep) char *label = NULL;
        char *l;
        socklen_t len = 1023;
        int r;

        label = malloc(len + 1);
        if (!label)
                return -ENOMEM;

        for (;;) {
                r = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, label, &len);
                if (r >= 0) {
                        label[len] = '\0';
                        *lenp = len;
                        *labelp = label;
                        label = NULL;
                        break;
                } else if (errno == ENOPROTOOPT) {
                        *lenp = 0;
                        *labelp = NULL;
                        break;
                } else if (errno != ERANGE)
                        return -errno;

                l = realloc(label, len + 1);
                if (!l)
                        return -ENOMEM;

                label = l;
        }

        return 0;
}

static int peer_compare(CRBTree *tree, void *k, CRBNode *rb) {
        Peer *peer = c_container_of(rb, Peer, rb);
        uint64_t id = *(uint64_t*)k;

        if (peer->id < id)
                return -1;
        if (peer->id > id)
                return 1;

        return 0;
}

/**
 * peer_new() - XXX
 */
int peer_new(Peer **peerp,
             Bus *bus,
             int fd) {
        _c_cleanup_(peer_freep) Peer *peer = NULL;
        _c_cleanup_(user_entry_unrefp) UserEntry *user = NULL;
        _c_cleanup_(c_freep) char *seclabel = NULL;
        CRBNode **slot, *parent;
        size_t n_seclabel;
        struct ucred ucred;
        socklen_t socklen = sizeof(ucred);
        int r;

        r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &socklen);
        if (r < 0)
                return -errno;

        r = user_registry_ref_entry(&bus->users, &user, ucred.uid);
        if (r < 0)
                return r;

        if (user->n_peers < 1)
                return -EDQUOT;

        r = peer_get_peersec(fd, &seclabel, &n_seclabel);
        if (r < 0)
                return r;

        peer = calloc(1, sizeof(*peer));
        if (!peer)
                return -ENOMEM;

        user->n_peers --;

        peer->bus = bus;
        peer->connection = (Connection)CONNECTION_NULL(peer->connection);
        c_rbnode_init(&peer->rb);
        peer->user = user;
        user = NULL;
        peer->pid = ucred.pid;
        peer->seclabel = seclabel;
        seclabel = NULL;
        peer->n_seclabel = n_seclabel;
        match_registry_init(&peer->matches);
        reply_registry_init(&peer->replies_outgoing);
        peer->replies_incoming = (CList)C_LIST_INIT(peer->replies_incoming);

        r = connection_init_server(&peer->connection,
                                   &bus->dispatcher,
                                   &bus->ready_list,
                                   &bus->hup_list,
                                   peer_dispatch,
                                   peer->user,
                                   bus->guid,
                                   fd);
        if (r < 0)
                return r;

        peer->id = bus->peers.ids++;
        slot = c_rbtree_find_slot(&bus->peers.peers, peer_compare, &peer->id, &parent);
        assert(slot); /* peer->id is guaranteed to be unique */
        c_rbtree_add(&bus->peers.peers, parent, slot, &peer->rb);

        *peerp = peer;
        peer = NULL;
        return 0;
}

/**
 * peer_free() - XXX
 */
Peer *peer_free(Peer *peer) {
        ReplySlot *reply, *safe;

        if (!peer)
                return NULL;

        assert(!peer->match_rules.root);
        assert(!peer->names.root);
        assert(!peer->registered);

        peer->user->n_peers ++;

        c_list_for_each_entry_safe(reply, safe, &peer->replies_incoming, link)
                reply_slot_free(reply);

        c_rbtree_remove_init(&peer->bus->peers.peers, &peer->rb);

        reply_registry_deinit(&peer->replies_outgoing);
        match_registry_deinit(&peer->matches);
        connection_deinit(&peer->connection);
        user_entry_unref(peer->user);
        free(peer->seclabel);
        free(peer);

        return NULL;
}

int peer_start(Peer *peer) {
        return error_fold(connection_start(&peer->connection));
}

void peer_stop(Peer *peer) {
        connection_stop(&peer->connection);
}

void peer_register(Peer *peer) {
        assert(!peer->registered);

        peer->registered = true;
}

void peer_unregister(Peer *peer) {
        assert(peer->registered);

        peer->registered = false;
}

void peer_registry_init(PeerRegistry *registry) {
        registry->peers = (CRBTree){};
        registry->ids = 0;
}

void peer_registry_deinit(PeerRegistry *registry) {
        assert(!registry->peers.root);
}

void peer_registry_flush(PeerRegistry *registry) {
        CRBNode *node, *next;
        int r;

        for (node = c_rbtree_first_postorder(&registry->peers), next = c_rbnode_next_postorder(node);
             node;
             node = next, next = c_rbnode_next(node)) {
                Peer *peer = c_container_of(node, Peer, rb);

                if (peer_is_registered(peer)) {
                        r = driver_goodbye(peer, true);
                        assert(!r); /* can not fail in silent mode */
                }

                peer_free(peer);
        }
}

Peer *peer_registry_find_peer(PeerRegistry *registry, uint64_t id) {
        Peer *peer;

        peer = c_rbtree_find_entry(&registry->peers, peer_compare, &id, Peer, rb);

        return peer->registered ? peer : NULL;
}
