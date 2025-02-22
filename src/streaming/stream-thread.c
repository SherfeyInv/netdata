// SPDX-License-Identifier: GPL-3.0-or-later

#include "stream-thread.h"

struct stream_thread_globals stream_thread_globals = {
    .assign = {
        .spinlock = NETDATA_SPINLOCK_INITIALIZER,
    }
};

// --------------------------------------------------------------------------------------------------------------------
// pipe messages

static void stream_thread_handle_op(struct stream_thread *sth, struct stream_opcode *msg) {
    internal_fatal(sth->tid != gettid_cached(), "Function %s() should only be used by the dispatcher thread", __FUNCTION__ );

    sth->messages.processed++;

    struct sender_state *s = msg->sender ? SENDERS_GET(&sth->snd.senders, (Word_t)msg->sender) : NULL;

    if (msg->session &&                         // there is a session
        s &&                                    // there is a sender
        (size_t)msg->thread_slot == sth->id)    // same thread
    {
        if(msg->opcode & STREAM_OPCODE_SENDER_POLLOUT) {
            if(!nd_poll_upd(sth->run.ndpl, s->sock.fd, ND_POLL_READ|ND_POLL_WRITE, &s->thread.meta))
                internal_fatal(true, "Failed to update sender socket in nd_poll()");
            msg->opcode &= ~(STREAM_OPCODE_SENDER_POLLOUT);
        }

        if(msg->opcode)
            stream_sender_handle_op(sth, s, msg);
    }
    else {
        // this may happen if we receive a POLLOUT opcode, but the sender has been disconnected
        nd_log(NDLS_DAEMON, NDLP_DEBUG, "STREAM THREAD[%zu]: OPCODE %u ignored.", sth->id, (unsigned)msg->opcode);
    }
}

void stream_sender_send_msg_to_dispatcher(struct sender_state *s, struct stream_opcode msg) {
    if (!msg.session || !msg.sender || !s)
        return;

    internal_fatal(msg.sender != s, "the sender pointer in the message does not match this sender");

    struct stream_thread *sth = stream_thread_by_slot_id(msg.thread_slot);
    if(!sth) {
        internal_fatal(true,
                       "STREAM SEND[x] [%s] thread pointer in the opcode message does not match the expected",
                       rrdhost_hostname(s->host));
        return;
    }

    bool send_pipe_msg = false;

    // check if we can execute the message now
    if(sth->tid == gettid_cached()) {
        // we are running at the dispatcher thread
        // no need for locks or queuing
        sth->messages.bypassed++;
        stream_thread_handle_op(sth, &msg);
        return;
    }

    // add it to the message queue of the thread
    spinlock_lock(&sth->messages.spinlock);
    {
        sth->messages.added++;
        if (s->thread.msg_slot >= sth->messages.used || sth->messages.array[s->thread.msg_slot].sender != s) {
            if (unlikely(sth->messages.used >= sth->messages.size)) {
                // this should never happen, but let's find the root cause

                if (!sth->messages.size) {
                    // we are exiting
                    spinlock_unlock(&sth->messages.spinlock);
                    return;
                }

                // try to find us in the list
                for (size_t i = 0; i < sth->messages.size; i++) {
                    if (sth->messages.array[i].sender == s) {
                        s->thread.msg_slot = i;
                        sth->messages.array[s->thread.msg_slot].opcode |= msg.opcode;
                        spinlock_unlock(&sth->messages.spinlock);
                        internal_fatal(true, "the dispatcher message queue is full, but this sender is already on slot %zu", i);
                        return;
                    }
                }

                fatal("the dispatcher message queue is full, but this should never happen");
            }

            // let's use a new slot
            send_pipe_msg = !sth->messages.used; // write to the pipe, only when the queue was empty before this msg
            s->thread.msg_slot = sth->messages.used++;
            sth->messages.array[s->thread.msg_slot] = msg;
        }
        else
            // the existing slot is good
            sth->messages.array[s->thread.msg_slot].opcode |= msg.opcode;
    }
    spinlock_unlock(&sth->messages.spinlock);

    // signal the streaming thread to wake up and process messages
    if(send_pipe_msg &&
        sth->pipe.fds[PIPE_WRITE] != -1 &&
        write(sth->pipe.fds[PIPE_WRITE], " ", 1) != 1) {
        nd_log_limit_static_global_var(erl, 1, 1 * USEC_PER_MS);
        nd_log_limit(&erl, NDLS_DAEMON, NDLP_ERR,
                     "STREAM SEND [%s]: cannot write to signal pipe",
                     rrdhost_hostname(s->host));
    }
}

static void stream_thread_read_pipe_messages(struct stream_thread *sth) {
    internal_fatal(sth->tid != gettid_cached(), "Function %s() should only be used by the dispatcher thread", __FUNCTION__ );

    if(read(sth->pipe.fds[PIPE_READ], sth->pipe.buffer, sth->pipe.size * sizeof(*sth->pipe.buffer)) <= 0)
        nd_log(NDLS_DAEMON, NDLP_ERR, "STREAM THREAD[%zu]: signal pipe read error", sth->id);

    size_t used = 0;
    spinlock_lock(&sth->messages.spinlock);
    if(sth->messages.used) {
        used = sth->messages.used;
        memcpy(sth->messages.copy, sth->messages.array, used * sizeof(*sth->messages.copy));
        sth->messages.used = 0;
    }
    spinlock_unlock(&sth->messages.spinlock);

    for(size_t i = 0; i < used ;i++) {
        struct stream_opcode *msg = &sth->messages.copy[i];
        stream_thread_handle_op(sth, msg);
    }
}

// --------------------------------------------------------------------------------------------------------------------

static int set_pipe_size(int pipe_fd, int new_size) {
    int default_size = new_size;
    int result = new_size;

#ifdef F_GETPIPE_SZ
    // get the current size of the pipe
    result = fcntl(pipe_fd, F_GETPIPE_SZ);
    if(result > 0)
        default_size = result;
#endif

#ifdef F_SETPIPE_SZ
    // set the new size to the pipe
    if(result <= new_size) {
        result = fcntl(pipe_fd, F_SETPIPE_SZ, new_size);
        if (result <= 0)
            return default_size;
    }
#endif

    // we return either:
    // 1. the new_size (after setting it)
    // 2. the current size (if we can't set it, but we can read it)
    // 3. the new_size (without setting it when we can't read the current size)
    return result;  // Returns the new pipe size
}

// --------------------------------------------------------------------------------------------------------------------

static void stream_thread_messages_resize_unsafe(struct stream_thread *sth) {
    internal_fatal(sth->tid != gettid_cached(), "Function %s() should only be used by the dispatcher thread", __FUNCTION__ );

    if(sth->nodes_count >= sth->messages.size) {
        size_t new_size = sth->messages.size ? sth->messages.size * 2 : 2;
        sth->messages.array = reallocz(sth->messages.array, new_size * sizeof(*sth->messages.array));
        sth->messages.copy = reallocz(sth->messages.copy, new_size * sizeof(*sth->messages.copy));
        sth->messages.size = new_size;
    }
}

// --------------------------------------------------------------------------------------------------------------------

static bool stream_thread_process_poll_slot(struct stream_thread *sth, nd_poll_result_t *ev, usec_t now_ut, size_t *replay_entries) {
    struct pollfd_meta *m = ev->data;
    internal_fatal(!m, "Failed to get meta from event");

    switch(m->type) {
        case POLLFD_TYPE_SENDER: {
            struct sender_state *s = m->s;
            internal_fatal(SENDERS_GET(&sth->snd.senders, (Word_t)s) == NULL, "Sender is not found in the senders list");
            stream_sender_process_poll_events(sth, s, ev->events, now_ut);
            *replay_entries += dictionary_entries(s->replication.requests);
            break;
        }

        case POLLFD_TYPE_RECEIVER: {
            struct receiver_state *rpt = m->rpt;
            internal_fatal(RECEIVERS_GET(&sth->rcv.receivers, (Word_t)rpt) == NULL, "Receiver is not found in the receiver list");
            stream_receive_process_poll_events(sth, rpt, ev->events, now_ut);
            break;
        }

        case POLLFD_TYPE_PIPE:
            if (likely(ev->events & ND_POLL_READ)) {
                worker_is_busy(WORKER_SENDER_JOB_PIPE_READ);
                stream_thread_read_pipe_messages(sth);
            }
            else if(unlikely(ev->events & ND_POLL_ERROR)) {
                // we have errors on this pipe
                nd_log(NDLS_DAEMON, NDLP_ERR,
                       "STREAM THREAD[%zu]: got errors on pipe - exiting to be restarted.", sth->id);
                return true;
            }
            break;

        case POLLFD_TYPE_EMPTY:
            // should never happen - but make sure it never happens again
            internal_fatal(true, "What is this?");
            break;
    }

    return false;
}

void *stream_thread(void *ptr) {
    struct stream_thread *sth = ptr;

    worker_register("STREAM");

    // stream thread main event loop
    worker_register_job_name(WORKER_STREAM_JOB_LIST, "list");
    worker_register_job_name(WORKER_STREAM_JOB_DEQUEUE, "dequeue");
    worker_register_job_name(WORKER_STREAM_JOB_PREP, "prep");
    worker_register_job_name(WORKER_STREAM_JOB_POLL_ERROR, "poll error");
    worker_register_job_name(WORKER_SENDER_JOB_PIPE_READ, "pipe read");

    // both sender and receiver
    worker_register_job_name(WORKER_STREAM_JOB_SOCKET_RECEIVE, "receive");
    worker_register_job_name(WORKER_STREAM_JOB_SOCKET_SEND, "send");
    worker_register_job_name(WORKER_STREAM_JOB_SOCKET_ERROR, "sock error");

    // receiver
    worker_register_job_name(WORKER_STREAM_JOB_COMPRESS, "compress");
    worker_register_job_name(WORKER_STREAM_JOB_DECOMPRESS, "decompress");

    // sender
    worker_register_job_name(WORKER_SENDER_JOB_EXECUTE, "execute");
    worker_register_job_name(WORKER_SENDER_JOB_EXECUTE_REPLAY, "replay");
    worker_register_job_name(WORKER_SENDER_JOB_EXECUTE_FUNCTION, "function");
    worker_register_job_name(WORKER_SENDER_JOB_EXECUTE_META, "meta");

    // disconnection reasons
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_OVERFLOW, "disconnect overflow");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_TIMEOUT, "disconnect timeout");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_SOCKET_ERROR, "disconnect socket error");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_PARENT_CLOSED, "disconnect parent closed");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_RECEIVE_ERROR, "disconnect receive error");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_SEND_ERROR, "disconnect send error");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_COMPRESSION_ERROR, "disconnect compression error");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_RECEIVER_LEFT, "disconnect receiver left");
    worker_register_job_name(WORKER_SENDER_JOB_DISCONNECT_HOST_CLEANUP, "disconnect host cleanup");

    // metrics
    worker_register_job_custom_metric(WORKER_STREAM_METRIC_NODES,
                                      "nodes", "nodes",
                                      WORKER_METRIC_ABSOLUTE);

    worker_register_job_custom_metric(WORKER_RECEIVER_JOB_BYTES_READ,
                                      "receiver received bytes", "bytes/s",
                                      WORKER_METRIC_INCREMENT);

    worker_register_job_custom_metric(WORKER_RECEIVER_JOB_BYTES_UNCOMPRESSED,
                                      "receiver received uncompressed bytes", "bytes/s",
                                      WORKER_METRIC_INCREMENT);

    worker_register_job_custom_metric(WORKER_RECEIVER_JOB_REPLICATION_COMPLETION,
                                      "receiver replication completion", "%",
                                      WORKER_METRIC_ABSOLUTE);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BUFFER_RATIO,
                                      "sender used buffer ratio", "%",
                                      WORKER_METRIC_ABSOLUTE);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BYTES_RECEIVED,
                                      "sender bytes received", "bytes/s",
                                      WORKER_METRIC_INCREMENT);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BYTES_SENT,
                                      "sender bytes sent", "bytes/s",
                                      WORKER_METRIC_INCREMENT);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BYTES_COMPRESSED,
                                      "sender bytes compressed", "bytes/s",
                                      WORKER_METRIC_INCREMENTAL_TOTAL);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BYTES_UNCOMPRESSED,
                                      "sender bytes uncompressed", "bytes/s",
                                      WORKER_METRIC_INCREMENTAL_TOTAL);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_BYTES_COMPRESSION_RATIO,
                                      "sender cumulative compression savings ratio", "%",
                                      WORKER_METRIC_ABSOLUTE);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_REPLAY_DICT_SIZE,
                                      "sender replication dict entries", "entries",
                                      WORKER_METRIC_ABSOLUTE);

    worker_register_job_custom_metric(WORKER_SENDER_JOB_MESSAGES,
                                      "ops processed", "messages",
                                      WORKER_METRIC_INCREMENTAL_TOTAL);

    if(pipe(sth->pipe.fds) != 0) {
        nd_log(NDLS_DAEMON, NDLP_ERR, "STREAM THREAD[%zu]: cannot create required pipe.", sth->id);
        sth->pipe.fds[PIPE_READ] = -1;
        sth->pipe.fds[PIPE_WRITE] = -1;
        return NULL;
    }

    sth->tid = gettid_cached();

    sth->pipe.size = set_pipe_size(sth->pipe.fds[PIPE_READ], 65536 * sizeof(*sth->pipe.buffer)) / sizeof(*sth->pipe.buffer);
    sth->pipe.buffer = mallocz(sth->pipe.size * sizeof(*sth->pipe.buffer));

    usec_t last_check_all_nodes_ut = 0;
    usec_t last_dequeue_ut = 0;

    sth->run.pipe = (struct pollfd_meta){
        .type = POLLFD_TYPE_PIPE,
    };
    sth->run.ndpl = nd_poll_create();
    if(!sth->run.ndpl)
        fatal("Cannot create nd_poll()");

    if(!nd_poll_add(sth->run.ndpl, sth->pipe.fds[PIPE_READ], ND_POLL_READ, &sth->run.pipe))
        internal_fatal(true, "Failed to add pipe to nd_poll()");

    bool exit_thread = false;
    size_t replay_entries = 0;
    sth->snd.bytes_received = 0;
    sth->snd.bytes_sent = 0;

    while(!exit_thread && !nd_thread_signaled_to_cancel() && service_running(SERVICE_STREAMING)) {
        usec_t now_ut = now_monotonic_usec();

        if(now_ut - last_dequeue_ut >= 100 * USEC_PER_MS) {
            worker_is_busy(WORKER_STREAM_JOB_DEQUEUE);

            // move any pending hosts in the inbound queue, to the running list
            spinlock_lock(&sth->queue.spinlock);
            stream_thread_messages_resize_unsafe(sth);
            stream_receiver_move_queue_to_running_unsafe(sth);
            stream_sender_move_queue_to_running_unsafe(sth);
            spinlock_unlock(&sth->queue.spinlock);
            last_dequeue_ut = now_ut;
        }

        if(now_ut - last_check_all_nodes_ut >= USEC_PER_SEC) {
            worker_is_busy(WORKER_STREAM_JOB_LIST);

            // periodically check the entire list of nodes
            // this detects unresponsive parents too (timeout)
            stream_sender_check_all_nodes_from_poll(sth, now_ut);
            worker_set_metric(WORKER_SENDER_JOB_MESSAGES, (NETDATA_DOUBLE)(sth->messages.processed));
            worker_set_metric(WORKER_STREAM_METRIC_NODES, (NETDATA_DOUBLE)sth->nodes_count);

            worker_set_metric(WORKER_SENDER_JOB_BYTES_RECEIVED, (NETDATA_DOUBLE)sth->snd.bytes_received);
            worker_set_metric(WORKER_SENDER_JOB_BYTES_SENT, (NETDATA_DOUBLE)sth->snd.bytes_sent);
            worker_set_metric(WORKER_SENDER_JOB_REPLAY_DICT_SIZE, (NETDATA_DOUBLE)replay_entries);
            replay_entries = 0;
            sth->snd.bytes_received = 0;
            sth->snd.bytes_sent = 0;

            last_check_all_nodes_ut = now_ut;
        }

        worker_is_idle();

        nd_poll_result_t ev;
        int poll_rc = nd_poll_wait(sth->run.ndpl, 100, &ev);

        worker_is_busy(WORKER_STREAM_JOB_PREP);

        if (poll_rc == 0)
            // nd_poll() timed out - just loop again
            continue;

        if(unlikely(poll_rc == -1)) {
            // nd_poll() returned an error
            internal_fatal(true, "nd_poll() failed");
            worker_is_busy(WORKER_STREAM_JOB_POLL_ERROR);
            nd_log_limit_static_thread_var(erl, 1, 1 * USEC_PER_MS);
            nd_log_limit(&erl, NDLS_DAEMON, NDLP_ERR, "STREAM THREAD[%zu] poll() returned error", sth->id);
            continue;
        }

        if(nd_thread_signaled_to_cancel() || !service_running(SERVICE_STREAMING))
            break;

        now_ut = now_monotonic_usec();
        exit_thread = stream_thread_process_poll_slot(sth, &ev, now_ut, &replay_entries);
    }

    // dequeue
    spinlock_lock(&sth->queue.spinlock);
    stream_sender_move_queue_to_running_unsafe(sth);
    stream_receiver_move_queue_to_running_unsafe(sth);
    spinlock_unlock(&sth->queue.spinlock);

    // cleanup receiver and dispatcher
    stream_sender_cleanup(sth);
    stream_receiver_cleanup(sth);

    // cleanup the thread structures
    spinlock_lock(&sth->messages.spinlock);
    freez(sth->messages.array);
    sth->messages.array = NULL;
    sth->messages.size = 0;
    sth->messages.used = 0;
    spinlock_unlock(&sth->messages.spinlock);

    freez(sth->pipe.buffer);
    sth->pipe.buffer = NULL;
    sth->pipe.size = 0;

    nd_poll_destroy(sth->run.ndpl);
    sth->run.ndpl = NULL;

    close(sth->pipe.fds[PIPE_READ]);
    close(sth->pipe.fds[PIPE_WRITE]);
    sth->pipe.fds[PIPE_READ] = -1;
    sth->pipe.fds[PIPE_WRITE] = -1;

    sth->thread = NULL;
    sth->tid = 0;

    worker_unregister();

    return NULL;
}

// --------------------------------------------------------------------------------------------------------------------

void stream_thread_node_queued(RRDHOST *host) {
    spinlock_lock(&stream_thread_globals.assign.spinlock);
    host->stream.refcount++;
    internal_fatal(host->stream.refcount > 2, "invalid stream refcount %u (while adding node)", host->stream.refcount);
    spinlock_unlock(&stream_thread_globals.assign.spinlock);
}

void stream_thread_node_removed(RRDHOST *host) {
    spinlock_lock(&stream_thread_globals.assign.spinlock);
    internal_fatal(!host->stream.refcount, "invalid stream refcount %u (while stopping node)", host->stream.refcount);

    if(--host->stream.refcount == 0) {
        struct stream_thread *sth = host->stream.thread;
        sth->nodes_count--;
        host->stream.thread = NULL;
    }

    spinlock_unlock(&stream_thread_globals.assign.spinlock);
}

static struct stream_thread *stream_thread_get_unsafe(RRDHOST *host) {
    if(host->stream.thread)
        return host->stream.thread;

    if(!stream_thread_globals.assign.cores) {
        stream_thread_globals.assign.cores = get_netdata_cpus() - 1;
        if(stream_thread_globals.assign.cores < 4)
            stream_thread_globals.assign.cores = 4;
        else if(stream_thread_globals.assign.cores > STREAM_MAX_THREADS)
            stream_thread_globals.assign.cores = STREAM_MAX_THREADS;
    }

    size_t selected_thread_slot = 0;
    size_t min_nodes = stream_thread_globals.threads[0].nodes_count;
    for(size_t i = 1; i < stream_thread_globals.assign.cores ; i++) {
        if(stream_thread_globals.threads[i].nodes_count < min_nodes) {
            selected_thread_slot = i;
            min_nodes = stream_thread_globals.threads[i].nodes_count;
        }
    }

    struct stream_thread *sth = host->stream.thread = &stream_thread_globals.threads[selected_thread_slot];
    host->stream.refcount = 0;
    sth->nodes_count++;

    return host->stream.thread;
}

static struct stream_thread * stream_thread_assign_and_start(RRDHOST *host) {
    spinlock_lock(&stream_thread_globals.assign.spinlock);

    struct stream_thread *sth = stream_thread_get_unsafe(host);

    if(!sth->thread) {
        sth->id = (sth - stream_thread_globals.threads); // find the slot number
        if(&stream_thread_globals.threads[sth->id] != sth)
            fatal("STREAM THREAD[x] [%s]: thread id and slot do not match!", rrdhost_hostname(host));

        sth->pipe.fds[PIPE_READ] = -1;
        sth->pipe.fds[PIPE_WRITE] = -1;
        spinlock_init(&sth->pipe.spinlock);
        spinlock_init(&sth->queue.spinlock);
        spinlock_init(&sth->messages.spinlock);
        sth->messages.used = 0;

        char tag[NETDATA_THREAD_TAG_MAX + 1];
        snprintfz(tag, NETDATA_THREAD_TAG_MAX, THREAD_TAG_STREAM "[%zu]", sth->id);

        sth->thread = nd_thread_create(tag, NETDATA_THREAD_OPTION_DEFAULT, stream_thread, sth);
        if (!sth->thread)
            nd_log_daemon(NDLP_ERR, "STREAM THREAD[%zu]: failed to create new thread for client.", sth->id);
    }

    spinlock_unlock(&stream_thread_globals.assign.spinlock);

    return sth;
}

void stream_sender_add_to_connector_queue(RRDHOST *host) {
    ND_LOG_STACK lgs[] = {
        ND_LOG_FIELD_STR(NDF_NIDL_NODE, host->hostname),
        ND_LOG_FIELD_UUID(NDF_MESSAGE_ID, &streaming_to_parent_msgid),
        ND_LOG_FIELD_END(),
    };
    ND_LOG_STACK_PUSH(lgs);

    stream_connector_init(host->sender);
    rrdhost_stream_parent_ssl_init(host->sender);
    stream_connector_add(host->sender);
}

void stream_receiver_add_to_queue(struct receiver_state *rpt) {
    struct stream_thread *sth = stream_thread_assign_and_start(rpt->host);

    stream_thread_node_queued(rpt->host);

    nd_log(NDLS_DAEMON, NDLP_DEBUG,
           "STREAM RECEIVE[%zu] [%s]: moving host to receiver queue...",
           sth->id, rrdhost_hostname(rpt->host));

    spinlock_lock(&sth->queue.spinlock);
    internal_fatal(RECEIVERS_GET(&sth->queue.receivers, (Word_t)rpt) != NULL, "Receiver is already in the receivers queue");
    RECEIVERS_SET(&sth->queue.receivers, (Word_t)rpt, rpt);
    spinlock_unlock(&sth->queue.spinlock);
}

void stream_sender_add_to_queue(struct sender_state *s) {
    struct stream_thread *sth = stream_thread_assign_and_start(s->host);

    stream_thread_node_queued(s->host);

    nd_log(NDLS_DAEMON, NDLP_DEBUG,
           "STREAM THREAD[%zu] [%s]: moving host to dispatcher queue...",
           sth->id, rrdhost_hostname(s->host));

    spinlock_lock(&sth->queue.spinlock);
    internal_fatal(SENDERS_GET(&sth->queue.senders, (Word_t)s) != NULL, "Sender is already in the senders queue");
    SENDERS_SET(&sth->queue.senders, (Word_t)s, s);
    spinlock_unlock(&sth->queue.spinlock);
}

void stream_threads_cancel(void) {
    stream_connector_cancel_threads();
    for(size_t i = 0; i < STREAM_MAX_THREADS ;i++)
        nd_thread_signal_cancel(stream_thread_globals.threads[i].thread);
}

struct stream_thread *stream_thread_by_slot_id(size_t thread_slot) {
    if(thread_slot < STREAM_MAX_THREADS && stream_thread_globals.threads[thread_slot].thread)
        return &stream_thread_globals.threads[thread_slot];

    return NULL;
}
