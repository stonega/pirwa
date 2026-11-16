/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pirwa-proxy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <libslirp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

/* This process owns one VM's network and outlives the application window.
 * Only the main thread touches libslirp or Ethernet frames. Blocking upstream
 * connects run on a bounded pool. Established streams share one poll loop. */
#define MAX_JOBS 32
#define MAX_RELAYS 256
#define WORKER_THREADS 8
#define MAX_TARGETS 256
#define MAX_TCP_STATES 2048
#define MAX_FRAME 65536
#define MAX_OUTPUT (4 * 1024 * 1024)

typedef struct Gateway Gateway;
typedef struct
{
  guint32 source, destination;
  guint16 source_port, destination_port;
} FlowKey;

typedef struct
{
  FlowKey key;
  gint64 created, closed;
  gboolean admitted, guest_fin, server_fin;
} Flow;

static guint
flow_hash (gconstpointer data)
{
  const FlowKey *key = data;
  return key->source ^ key->destination ^ ((guint)key->source_port << 16) ^ key->destination_port;
}

static gboolean
flow_equal (gconstpointer a, gconstpointer b)
{
  return memcmp (a, b, sizeof (FlowKey)) == 0;
}

static FlowKey
flow_key (const guint8 *frame, gsize header, gboolean reverse)
{
  FlowKey key;
  memcpy (&key.source, frame + (reverse ? 30 : 26), 4);
  memcpy (&key.destination, frame + (reverse ? 26 : 30), 4);
  memcpy (&key.source_port, frame + 14 + header + (reverse ? 2 : 0), 2);
  memcpy (&key.destination_port, frame + 14 + header + (reverse ? 0 : 2), 2);
  return key;
}
typedef struct
{
  guint32 address;
  guint16 port;
  GSocket *listener;
  char *path;
  gint users;
  gint64 used;
} Target;

typedef struct
{
  Gateway *gateway;
  Target *target;
  GSocketConnection *connection;
  GSocketConnection *upstream;
  GBytes *query;
  GBytes *answer;
} Job;

typedef struct
{
  Target *target;
  GSocketConnection *connections[2];
  int fds[2];
  guint8 buffer[2][32768];
  gsize used[2], sent[2];
  gboolean eof[2], shut[2];
} Relay;

typedef struct
{
  SlirpTimerCb callback;
  void *data;
  gint64 expires;
} Timer;

struct Gateway
{
  Slirp *slirp;
  PirwaProxy *proxy;
  GCancellable *cancel;
  GThreadPool *workers;
  GAsyncQueue *finished;
  GPtrArray *targets;
  GPtrArray *relays;
  GHashTable *flows;
  GPtrArray *timers;
  GArray *polls;
  GByteArray *input;
  GByteArray *output;
  char *directory;
  int peer;
  int wake[2];
  gint jobs;
  guint sequence;
  guint pending;
  gint64 reaped;
  gboolean accept_pending;
  gboolean failed;
};

static volatile sig_atomic_t stopped;
static int signal_fd = -1;

static void
stop_signal (int sig)
{
  (void)sig;
  int saved_errno = errno;
  stopped = 1;
  if (signal_fd >= 0)
    {
      /* A full nonblocking pipe already wakes poll; retry interrupted writes. */
      ssize_t written;
      do
        written = write (signal_fd, "s", 1);
      while (written < 0 && errno == EINTR);
    }
  errno = saved_errno;
}

static guint16
read16 (const guint8 *p)
{
  return ((guint16)p[0] << 8) | p[1];
}

static void
write16 (guint8 *p, guint16 value)
{
  p[0] = value >> 8;
  p[1] = value;
}

static guint16
checksum (const guint8 *p, gsize size)
{
  guint32 sum = 0;
  for (; size > 1; size -= 2, p += 2)
    sum += read16 (p);
  if (size)
    sum += (guint16)*p << 8;
  while (sum >> 16)
    sum = (sum & 65535) + (sum >> 16);
  return ~sum;
}

static gboolean
ipv4 (const guint8 *frame, gsize size, gsize *header, gsize *total)
{
  if (size < 34 || read16 (frame + 12) != 0x0800 || frame[14] >> 4 != 4)
    return FALSE;
  *header = (frame[14] & 15) * 4;
  *total = read16 (frame + 16);
  return *header >= 20 && *total >= *header && *total <= size - 14
         && !(read16 (frame + 20) & 0x3fff) && checksum (frame + 14, *header) == 0;
}

static gboolean
tcp_valid (const guint8 *frame, gsize header, gsize total)
{
  const guint8 *tcp = frame + 14 + header;
  gsize size = total - header;
  if (size < 20 || (tcp[12] >> 4) * 4 < 20 || (gsize)(tcp[12] >> 4) * 4 > size)
    return FALSE;
  guint32 sum = 6 + size;
  for (guint i = 26; i < 34; i += 2)
    sum += read16 (frame + i);
  for (; size > 1; size -= 2, tcp += 2)
    sum += read16 (tcp);
  if (size)
    sum += (guint16)*tcp << 8;
  while (sum >> 16)
    sum = (sum & 65535) + (sum >> 16);
  return sum == 65535;
}

static void
queue_frame (Gateway *gateway, const guint8 *frame, gsize size)
{
  if (size > MAX_FRAME || gateway->output->len + size + 4 > MAX_OUTPUT)
    return; /* TCP retransmits dropped frames; never grow without a bound. */
  guint32 length = GUINT32_TO_BE (size);
  g_byte_array_append (gateway->output, (guint8 *)&length, 4);
  g_byte_array_append (gateway->output, frame, size);
}

static slirp_ssize_t
send_frame (const void *buffer, size_t size, void *opaque)
{
  Gateway *gateway = opaque;
  g_autofree guint8 *frame = g_memdup2 (buffer, size);
  gsize header, total;
  if (ipv4 (frame, size, &header, &total) && frame[23] == 6 && total >= header + 20)
    {
      FlowKey key = flow_key (frame, header, TRUE);
      Flow *flow = g_hash_table_lookup (gateway->flows, &key);
      guint8 flags = frame[14 + header + 13];
      if (flow && (flags & 4))
        g_hash_table_remove (gateway->flows, &key);
      else if (flow && (flags & 1))
        {
          flow->server_fin = TRUE;
          if (flow->guest_fin)
            flow->closed = g_get_monotonic_time ();
        }
    }
  if (ipv4 (frame, size, &header, &total) && frame[23] == 17 && total >= header + 248)
    {
      guint8 *udp = frame + 14 + header;
      guint8 *bootp = udp + 8;
      if (read16 (udp) == 67 && read16 (udp + 2) == 68
          && memcmp (bootp + 236, "\x63\x82\x53\x63", 4) == 0)
        {
          guint8 type = 0;
          for (gsize i = 240; i < total - header - 8;)
            {
              guint8 code = bootp[i++];
              if (code == 255)
                break;
              if (!code)
                continue;
              if (i >= total - header - 8 || bootp[i] > total - header - 9 - i)
                break;
              guint8 length = bootp[i++];
              if (code == 53 && length == 1)
                type = bootp[i];
              i += length;
            }
          if ((type == 2 || type == 5) && total >= header + 8 + 240 + 40)
            {
              /* A /0 internal slirp network lets the public guestfwd API
               * intercept every IPv4 destination. The guest receives a normal
               * private /24; restricted slirp never opens an Internet socket. */
              const guint8 options[]
                  = { 53, 1, type, 54, 4, 10, 0,  2, 2, 1, 4,  255, 255, 255, 0,  3,   4,
                      10, 0, 2,    2,  6, 4,  10, 0, 2, 3, 51, 4,   0,   1,   81, 128, 255 };
              memset (bootp + 240, 0, total - header - 8 - 240);
              memcpy (bootp + 240, options, sizeof options);
              write16 (udp + 6, 0); /* IPv4 permits an omitted UDP checksum. */
            }
        }
    }
  queue_frame (gateway, frame, size);
  return size;
}

static int64_t
clock_ns (void *opaque)
{
  (void)opaque;
  return g_get_monotonic_time () * 1000;
}

static void
poll_fd_changed (int fd, void *opaque)
{
  (void)fd;
  Gateway *gateway = opaque;
  gateway->accept_pending = TRUE;
  /* The poll set itself is rebuilt by slirp_pollfds_fill each turn. */
}

static void
notify (void *opaque)
{
  (void)opaque; /* libslirp calls this on the gateway's own polling thread. */
}

static void *
timer_new (SlirpTimerCb callback, void *data, void *opaque)
{
  Gateway *gateway = opaque;
  Timer *timer = g_new0 (Timer, 1);
  *timer = (Timer){ callback, data, G_MAXINT64 };
  g_ptr_array_add (gateway->timers, timer);
  return timer;
}

static void
timer_free (void *timer, void *opaque)
{
  Gateway *gateway = opaque;
  g_ptr_array_remove_fast (gateway->timers, timer);
}

static void
timer_mod (void *data, int64_t expires, void *opaque)
{
  (void)opaque;
  ((Timer *)data)->expires = expires;
}

static int
add_poll (int fd, int events, void *opaque)
{
  Gateway *gateway = opaque;
  struct pollfd poll = { .fd = fd };
  if (events & SLIRP_POLL_IN)
    poll.events |= POLLIN;
  if (events & SLIRP_POLL_OUT)
    poll.events |= POLLOUT;
  if (events & SLIRP_POLL_PRI)
    poll.events |= POLLPRI;
  g_array_append_val (gateway->polls, poll);
  return gateway->polls->len - 1;
}

static int
get_events (int index, void *opaque)
{
  Gateway *gateway = opaque;
  short events = g_array_index (gateway->polls, struct pollfd, index).revents;
  int result = 0;
  if (events & POLLIN)
    result |= SLIRP_POLL_IN;
  if (events & POLLOUT)
    result |= SLIRP_POLL_OUT;
  if (events & POLLPRI)
    result |= SLIRP_POLL_PRI;
  if (events & POLLERR)
    result |= SLIRP_POLL_ERR;
  if (events & POLLHUP)
    result |= SLIRP_POLL_HUP;
  return result;
}

static gboolean
socket_write (GSocketConnection *connection, const void *data, gsize size, GCancellable *cancel)
{
  return g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (connection)), data,
                                    size, NULL, cancel, NULL);
}

static gboolean
socket_read (GSocketConnection *connection, void *data, gsize size, GCancellable *cancel)
{
  gsize received = 0;
  return g_input_stream_read_all (g_io_stream_get_input_stream (G_IO_STREAM (connection)), data,
                                  size, &received, cancel, NULL)
         && received == size;
}

static void
relay_free (gpointer data)
{
  Relay *relay = data;
  relay->target->users--;
  g_clear_object (&relay->connections[0]);
  g_clear_object (&relay->connections[1]);
  g_free (relay);
}

static struct pollfd
relay_poll (Relay *relay, guint i)
{
  struct pollfd fd = { .fd = relay->fds[i] };
  if (!relay->eof[i] && !relay->used[i])
    fd.events |= POLLIN;
  if (relay->used[1 - i])
    fd.events |= POLLOUT;
  if (relay->eof[i] && !relay->used[i] && !relay->shut[1 - i])
    {
      shutdown (relay->fds[1 - i], SHUT_WR);
      relay->shut[1 - i] = TRUE;
    }
  if (!fd.events)
    fd.fd = -1;
  return fd;
}

static gboolean
relay_step (Relay *relay, const struct pollfd *polls)
{
  for (guint i = 0; i < 2; i++)
    {
      if (polls[i].revents & (POLLERR | POLLNVAL))
        return FALSE;
      if (!relay->eof[i] && !relay->used[i] && (polls[i].revents & (POLLIN | POLLHUP)))
        {
          ssize_t count = recv (relay->fds[i], relay->buffer[i], sizeof relay->buffer[i], 0);
          if (count > 0)
            relay->used[i] = count;
          else if (!count)
            relay->eof[i] = TRUE;
          else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return FALSE;
        }
      guint other = 1 - i;
      if (relay->used[other] && (polls[i].revents & POLLOUT))
        {
          ssize_t count = send (relay->fds[i], relay->buffer[other] + relay->sent[other],
                                relay->used[other], MSG_NOSIGNAL);
          if (count > 0)
            {
              relay->sent[other] += count;
              relay->used[other] -= count;
              if (!relay->used[other])
                relay->sent[other] = 0;
            }
          else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return FALSE;
        }
    }
  return !(relay->eof[0] && relay->eof[1] && !relay->used[0] && !relay->used[1]);
}

static void
run_job (gpointer data, gpointer user_data)
{
  (void)user_data;
  Job *job = data;
  Gateway *gateway = job->gateway;
  guint32 address;
  guint16 port;
  if (job->query)
    {
      const guint8 *query = g_bytes_get_data (job->query, NULL);
      memcpy (&address, query + 30, 4);
      port = 53;
    }
  else
    {
      address = job->target->address;
      port = job->target->port;
    }
  char host[INET_ADDRSTRLEN];
  inet_ntop (AF_INET, &address, host, sizeof host);
  if (port == 53 && g_str_equal (host, "10.0.2.3"))
    g_strlcpy (host, gateway->proxy->dns, sizeof host);
  g_autoptr (GSocketConnection) upstream
      = pirwa_proxy_connect (gateway->proxy, host, port, gateway->cancel, NULL);
  if (upstream && job->query)
    {
      gsize size;
      const guint8 *query = g_bytes_get_data (job->query, &size);
      gsize header = (query[14] & 15) * 4;
      guint16 length = read16 (query + 14 + header + 4) - 8;
      guint8 prefix[2];
      write16 (prefix, length);
      if (socket_write (upstream, prefix, 2, gateway->cancel)
          && socket_write (upstream, query + 14 + header + 8, length, gateway->cancel)
          && socket_read (upstream, prefix, 2, gateway->cancel))
        {
          length = read16 (prefix);
          if (length >= 12 && length <= 16384)
            {
              g_autofree guint8 *answer = g_malloc (length);
              if (socket_read (upstream, answer, length, gateway->cancel))
                job->answer = g_bytes_new_take (g_steal_pointer (&answer), length);
            }
        }
    }
  else if (upstream)
    job->upstream = g_steal_pointer (&upstream);
  g_async_queue_push (gateway->finished, job);
  ssize_t written;
  do
    written = write (gateway->wake[1], "j", 1);
  while (written < 0 && errno == EINTR);
}

static void
dns_reply (Gateway *gateway, Job *job)
{
  gsize size;
  const guint8 *query = g_bytes_get_data (job->query, &size);
  gsize header = (query[14] & 15) * 4;
  const guint8 *dns = query + 14 + header + 8;
  gsize dns_size = read16 (query + 14 + header + 4) - 8;
  g_autofree guint8 *failure = NULL;
  if (job->answer)
    dns = g_bytes_get_data (job->answer, &dns_size);
  if (!job->answer || dns_size > 1232)
    {
      /* A short SERVFAIL, or TC response asking the guest to retry over TCP. */
      failure = g_memdup2 (query + 14 + header + 8, 12);
      failure[2] |= job->answer ? 0x82 : 0x80;
      failure[3] = job->answer ? 0x80 : 0x82;
      memset (failure + 4, 0, 8);
      dns = failure;
      dns_size = 12;
    }
  guint8 frame[14 + 20 + 8 + 1232] = { 0 };
  memcpy (frame, query + 6, 6);
  memcpy (frame + 6, query, 6);
  write16 (frame + 12, 0x0800);
  guint8 *ip = frame + 14;
  ip[0] = 0x45;
  write16 (ip + 2, 20 + 8 + dns_size);
  ip[8] = 64;
  ip[9] = 17;
  memcpy (ip + 12, query + 30, 4);
  memcpy (ip + 16, query + 26, 4);
  write16 (ip + 10, checksum (ip, 20));
  guint8 *udp = ip + 20;
  memcpy (udp, query + 14 + header + 2, 2);
  memcpy (udp + 2, query + 14 + header, 2);
  write16 (udp + 4, 8 + dns_size);
  memcpy (udp + 8, dns, dns_size);
  queue_frame (gateway, frame, 14 + 20 + 8 + dns_size);
}

static void
target_free (gpointer data)
{
  Target *target = data;
  g_clear_object (&target->listener);
  g_unlink (target->path);
  g_free (target->path);
  g_free (target);
}

static Target *
get_target (Gateway *gateway, guint32 address, guint16 port)
{
  for (guint i = 0; i < gateway->targets->len; i++)
    {
      Target *target = g_ptr_array_index (gateway->targets, i);
      if (target->address == address && target->port == port)
        {
          target->used = g_get_monotonic_time ();
          return target;
        }
    }
  if (gateway->targets->len >= MAX_TARGETS)
    {
      /* Unix guest forwards do not retain the rule in established slirp
       * sockets. An unused listener can be removed without closing TCP. */
      Target *oldest = NULL;
      for (guint i = 0; i < gateway->targets->len; i++)
        {
          Target *target = g_ptr_array_index (gateway->targets, i);
          if (!target->users && !(target->address == htonl (0x0a000203) && target->port == 53)
              && (!oldest || target->used < oldest->used))
            oldest = target;
        }
      if (!oldest)
        return NULL;
      struct in_addr old = { .s_addr = oldest->address };
      slirp_remove_guestfwd (gateway->slirp, old, oldest->port);
      g_ptr_array_remove_fast (gateway->targets, oldest);
    }
  Target *target = g_new0 (Target, 1);
  target->address = address;
  target->port = port;
  target->used = g_get_monotonic_time ();
  target->path = g_strdup_printf ("%s/%u", gateway->directory, gateway->sequence++);
  target->listener = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, 0, NULL);
  g_autoptr (GSocketAddress) socket_address = g_unix_socket_address_new (target->path);
  struct in_addr guest = { .s_addr = address };
  if (!target->listener || !g_socket_bind (target->listener, socket_address, FALSE, NULL)
      || !g_socket_listen (target->listener, NULL)
      || slirp_add_unix (gateway->slirp, target->path, &guest, port) < 0)
    {
      target_free (target);
      return NULL;
    }
  g_socket_set_blocking (target->listener, FALSE);
  g_ptr_array_add (gateway->targets, target);
  return target;
}

static void accept_target (Gateway *gateway, Target *target);

static void
receive_frame (Gateway *gateway, const guint8 *frame, gsize size)
{
  if (size >= 42 && read16 (frame + 12) == 0x0806)
    {
      slirp_input (gateway->slirp, frame, size);
      return;
    }
  gsize header, total;
  if (!ipv4 (frame, size, &header, &total))
    return; /* No IPv6, fragments, or other Ethernet protocols can escape. */
  const guint8 *transport = frame + 14 + header;
  if (frame[23] == 6 && tcp_valid (frame, header, total) && read16 (transport + 2))
    {
      FlowKey key = flow_key (frame, header, FALSE);
      Flow *flow = g_hash_table_lookup (gateway->flows, &key);
      if ((transport[13] & 0x12) == 0x02)
        {
          if (gateway->jobs + gateway->pending >= MAX_JOBS
              || gateway->relays->len + gateway->jobs + gateway->pending >= MAX_RELAYS)
            return;
          if (!flow)
            {
              if (g_hash_table_size (gateway->flows) >= MAX_TCP_STATES)
                return;
              flow = g_new0 (Flow, 1);
              flow->key = key;
              flow->created = g_get_monotonic_time ();
              g_hash_table_insert (gateway->flows, flow, flow);
            }
          gateway->pending++;
        }
      if (!flow)
        return;
      if (!flow->admitted && (transport[13] & 0x10)
          && (gateway->jobs >= MAX_JOBS || gateway->relays->len + gateway->jobs >= MAX_RELAYS))
        return;
      if (transport[13] & 1)
        {
          flow->guest_fin = TRUE;
          if (flow->server_fin)
            flow->closed = g_get_monotonic_time ();
        }
      guint32 address;
      memcpy (&address, frame + 30, 4);
      Target *target = get_target (gateway, address, read16 (transport + 2));
      if (target)
        {
          gateway->accept_pending = FALSE;
          slirp_input (gateway->slirp, frame, size);
          /* open_unix connects synchronously in libslirp. Drain its listener
           * before another ACK can fill the Unix backlog and block this loop. */
          if (gateway->accept_pending)
            {
              flow = g_hash_table_lookup (gateway->flows, &key);
              if (flow)
                flow->admitted = TRUE;
              accept_target (gateway, target);
            }
        }
      if (transport[13] & 4)
        g_hash_table_remove (gateway->flows, &key);
    }
  else if (frame[23] == 17 && total >= header + 8 && read16 (transport + 4) >= 8
           && read16 (transport + 4) <= total - header)
    {
      guint16 source = read16 (transport), destination = read16 (transport + 2);
      if (source == 68 && destination == 67)
        slirp_input (gateway->slirp, frame, size);
      else if (destination == 53 && read16 (transport + 4) >= 20 && read16 (transport + 4) <= 4104
               && gateway->jobs < MAX_JOBS)
        {
          Job *job = g_new0 (Job, 1);
          job->gateway = gateway;
          job->query = g_bytes_new (frame, 14 + total);
          gateway->jobs++;
          g_thread_pool_push (gateway->workers, job, NULL);
        }
    }
}

static void
accept_target (Gateway *gateway, Target *target)
{
  while (gateway->jobs < MAX_JOBS && gateway->relays->len + gateway->jobs < MAX_RELAYS)
    {
      GSocket *socket = g_socket_accept (target->listener, NULL, NULL);
      if (!socket)
        break;
      Job *job = g_new0 (Job, 1);
      job->gateway = gateway;
      job->target = target;
      job->connection = g_socket_connection_factory_create_connection (socket);
      g_object_unref (socket);
      target->users++;
      gateway->jobs++;
      g_thread_pool_push (gateway->workers, job, NULL);
    }
}

static void
finish_jobs (Gateway *gateway)
{
  Job *job;
  while ((job = g_async_queue_try_pop (gateway->finished)))
    {
      gateway->jobs--;
      if (job->upstream && !g_cancellable_is_cancelled (gateway->cancel))
        {
          Relay *relay = g_new0 (Relay, 1);
          relay->target = job->target;
          relay->connections[0] = g_steal_pointer (&job->connection);
          relay->connections[1] = g_steal_pointer (&job->upstream);
          for (guint i = 0; i < 2; i++)
            {
              GSocket *socket = g_socket_connection_get_socket (relay->connections[i]);
              g_socket_set_blocking (socket, FALSE);
              relay->fds[i] = g_socket_get_fd (socket);
            }
          g_ptr_array_add (gateway->relays, relay);
        }
      else if (job->target)
        job->target->users--;
      if (job->query && !g_cancellable_is_cancelled (gateway->cancel))
        dns_reply (gateway, job);
      g_clear_object (&job->connection);
      g_clear_object (&job->upstream);
      g_clear_pointer (&job->query, g_bytes_unref);
      g_clear_pointer (&job->answer, g_bytes_unref);
      g_free (job);
    }
}

static gboolean
reap_flow (gpointer key, gpointer value, gpointer data)
{
  (void)key;
  Flow *flow = value;
  gint64 now = *(gint64 *)data;
  /* Half-open TCP has already exceeded slirp's initial connection timeout;
   * both FINs have exceeded its TIME_WAIT interval. Live streams stay tracked. */
  return (!flow->admitted && now - flow->created > 300 * G_USEC_PER_SEC)
         || (flow->closed && now - flow->closed > 120 * G_USEC_PER_SEC);
}

static void
run_gateway (Gateway *gateway)
{
  while (!stopped && !gateway->failed)
    {
      finish_jobs (gateway);
      gint64 current = g_get_monotonic_time ();
      if (current - gateway->reaped >= G_USEC_PER_SEC)
        {
          g_hash_table_foreach_remove (gateway->flows, reap_flow, &current);
          gateway->reaped = current;
        }
      gateway->pending = 0;
      g_array_set_size (gateway->polls, 0);
      guint32 timeout = 1000;
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      slirp_pollfds_fill (gateway->slirp, &timeout, add_poll, gateway);
      G_GNUC_END_IGNORE_DEPRECATIONS
      guint peer_index = gateway->polls->len;
      struct pollfd peer = { gateway->peer, POLLIN | (gateway->output->len ? POLLOUT : 0), 0 };
      struct pollfd wake = { gateway->wake[0], POLLIN, 0 };
      g_array_append_val (gateway->polls, peer);
      g_array_append_val (gateway->polls, wake);
      guint count = gateway->targets->len;
      for (guint i = 0; i < count; i++)
        {
          Target *target = g_ptr_array_index (gateway->targets, i);
          struct pollfd listener
              = { g_socket_get_fd (target->listener),
                  gateway->jobs < MAX_JOBS && gateway->relays->len + gateway->jobs < MAX_RELAYS
                      ? POLLIN
                      : 0,
                  0 };
          g_array_append_val (gateway->polls, listener);
        }
      guint relay_index = gateway->polls->len;
      guint relays = gateway->relays->len;
      for (guint i = 0; i < relays; i++)
        for (guint side = 0; side < 2; side++)
          {
            struct pollfd fd = relay_poll (g_ptr_array_index (gateway->relays, i), side);
            g_array_append_val (gateway->polls, fd);
          }
      gint64 now = g_get_monotonic_time () / 1000;
      for (guint i = 0; i < gateway->timers->len; i++)
        {
          Timer *timer = g_ptr_array_index (gateway->timers, i);
          timeout = MIN ((gint64)timeout, MAX (0, timer->expires - now));
        }
      int result = poll ((struct pollfd *)gateway->polls->data, gateway->polls->len, timeout);
      if (result < 0 && errno == EINTR)
        continue;
      if (result < 0)
        break;
      slirp_pollfds_poll (gateway->slirp, FALSE, get_events, gateway);
      now = g_get_monotonic_time () / 1000;
      for (guint i = 0; i < gateway->timers->len; i++)
        {
          Timer *timer = g_ptr_array_index (gateway->timers, i);
          if (timer->expires <= now)
            {
              timer->expires = G_MAXINT64;
              timer->callback (timer->data);
            }
        }
      if (g_array_index (gateway->polls, struct pollfd, peer_index + 1).revents)
        {
          char bytes[64];
          while (read (gateway->wake[0], bytes, sizeof bytes) > 0)
            ;
        }
      for (guint i = 0; i < count; i++)
        if (g_array_index (gateway->polls, struct pollfd, peer_index + 2 + i).revents & POLLIN)
          accept_target (gateway, g_ptr_array_index (gateway->targets, i));
      for (guint i = relays; i > 0; i--)
        if (!relay_step (g_ptr_array_index (gateway->relays, i - 1),
                         &g_array_index (gateway->polls, struct pollfd, relay_index + (i - 1) * 2)))
          g_ptr_array_remove_index_fast (gateway->relays, i - 1);
      short events = g_array_index (gateway->polls, struct pollfd, peer_index).revents;
      if (events & POLLOUT)
        {
          ssize_t sent
              = send (gateway->peer, gateway->output->data, gateway->output->len, MSG_NOSIGNAL);
          if (sent > 0)
            g_byte_array_remove_range (gateway->output, 0, sent);
          else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
        }
      if (events & (POLLERR | POLLNVAL))
        break;
      if (events & (POLLIN | POLLHUP))
        {
          guint8 buffer[32768];
          ssize_t received = recv (gateway->peer, buffer, sizeof buffer, 0);
          if (!received)
            break;
          if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
          if (received > 0)
            g_byte_array_append (gateway->input, buffer, received);
          while (gateway->input->len >= 4)
            {
              guint32 length;
              memcpy (&length, gateway->input->data, 4);
              length = GUINT32_FROM_BE (length);
              if (length < 14 || length > MAX_FRAME)
                {
                  gateway->failed = TRUE;
                  break;
                }
              if (gateway->input->len < length + 4)
                break;
              receive_frame (gateway, gateway->input->data + 4, length);
              g_byte_array_remove_range (gateway->input, 0, length + 4);
            }
        }
    }
}

static void
gateway_clear (Gateway *gateway)
{
  g_cancellable_cancel (gateway->cancel);
  if (gateway->workers)
    g_thread_pool_free (gateway->workers, FALSE, TRUE);
  finish_jobs (gateway);
  if (gateway->slirp)
    slirp_cleanup (gateway->slirp);
  signal_fd = -1;
  for (guint i = 0; i < 2; i++)
    if (gateway->wake[i] >= 0)
      close (gateway->wake[i]);
  g_ptr_array_unref (gateway->relays);
  g_hash_table_unref (gateway->flows);
  g_ptr_array_unref (gateway->targets);
  g_ptr_array_unref (gateway->timers);
  g_array_unref (gateway->polls);
  g_byte_array_unref (gateway->input);
  g_byte_array_unref (gateway->output);
  g_async_queue_unref (gateway->finished);
  g_object_unref (gateway->cancel);
  if (gateway->directory)
    g_rmdir (gateway->directory);
  g_free (gateway->directory);
}

int
main (int argc, char **argv)
{
  int listen_fd = 3, control_fd = 4;
  if (argc == 5 && g_str_equal (argv[1], "--listen-fd") && g_str_equal (argv[3], "--control-fd"))
    {
      listen_fd = atoi (argv[2]);
      control_fd = atoi (argv[4]);
    }
  else if (argc != 1)
    return 1;
  if (listen_fd < 3 || control_fd < 3 || listen_fd == control_fd)
    return 1;
  signal (SIGPIPE, SIG_IGN);
  g_autoptr (GSocket) listener = g_socket_new_from_fd (listen_fd, NULL);
  g_autoptr (GSocket) control = g_socket_new_from_fd (control_fd, NULL);
  if (!listener || !control)
    return 1;
  g_socket_set_timeout (control, 10);
  g_autoptr (GSocketConnection) configuration
      = g_socket_connection_factory_create_connection (control);
  guint32 length;
  if (!socket_read (configuration, &length, 4, NULL))
    return 1;
  length = GUINT32_FROM_BE (length);
  if (!length || length > 8192)
    return 1;
  g_autofree char *text = g_malloc0 (length + 1);
  if (!socket_read (configuration, text, length, NULL))
    return 1;
  g_autoptr (PirwaProxy) proxy = pirwa_proxy_parse (text, NULL);
  if (!proxy)
    return 1;
  Gateway gateway = { .proxy = proxy, .peer = -1, .wake = { -1, -1 } };
  gateway.directory = g_dir_make_tmp ("pirwa-network-XXXXXX", NULL);
  gateway.cancel = g_cancellable_new ();
  gateway.targets = g_ptr_array_new_with_free_func (target_free);
  gateway.relays = g_ptr_array_new_with_free_func (relay_free);
  gateway.flows = g_hash_table_new_full (flow_hash, flow_equal, NULL, g_free);
  gateway.timers = g_ptr_array_new_with_free_func (g_free);
  gateway.polls = g_array_new (FALSE, FALSE, sizeof (struct pollfd));
  gateway.input = g_byte_array_new ();
  gateway.output = g_byte_array_new ();
  gateway.finished = g_async_queue_new ();
  gateway.workers = g_thread_pool_new (run_job, NULL, WORKER_THREADS, FALSE, NULL);
  if (!gateway.directory || !gateway.workers || pipe (gateway.wake) < 0)
    {
      gateway_clear (&gateway);
      return 1;
    }
  for (guint i = 0; i < 2; i++)
    fcntl (gateway.wake[i], F_SETFL, O_NONBLOCK);
  signal_fd = gateway.wake[1];
  signal (SIGTERM, stop_signal);
  signal (SIGINT, stop_signal);
  SlirpConfig config = { .version = 3,
                         .restricted = 1,
                         .in_enabled = TRUE,
                         .disable_host_loopback = TRUE,
                         .disable_dns = TRUE };
  inet_pton (AF_INET, "10.0.2.2", &config.vhost);
  inet_pton (AF_INET, "10.0.2.15", &config.vdhcp_start);
  /* Keep the disabled built-in DNS endpoint separate from our TCP forward.
   * Older slirp versions reserve vnameserver even with disable_dns enabled. */
  inet_pton (AF_INET, "10.0.2.1", &config.vnameserver);
  SlirpCb callbacks = { .send_packet = send_frame,
                        .clock_get_ns = clock_ns,
                        .timer_new = timer_new,
                        .timer_free = timer_free,
                        .timer_mod = timer_mod,
                        .register_poll_fd = poll_fd_changed,
                        .unregister_poll_fd = poll_fd_changed,
                        .notify = notify };
  gateway.slirp = slirp_new (&config, &callbacks, &gateway);
  if (!gateway.slirp || !get_target (&gateway, htonl (0x0a000203), 53)
      || !socket_write (configuration, "1", 1, NULL))
    {
      gateway_clear (&gateway);
      return 1;
    }
  g_io_stream_close (G_IO_STREAM (configuration), NULL, NULL);
  g_socket_set_blocking (listener, FALSE);
  g_autoptr (GSocket) peer = NULL;
  gint64 deadline = g_get_monotonic_time () + 30 * G_USEC_PER_SEC;
  while (!stopped && g_get_monotonic_time () < deadline && !peer)
    {
      struct pollfd waiting[]
          = { { g_socket_get_fd (listener), POLLIN, 0 }, { gateway.wake[0], POLLIN, 0 } };
      int result = poll (waiting, 2, 1000);
      if (result > 0 && (waiting[0].revents & POLLIN))
        peer = g_socket_accept (listener, NULL, NULL);
      else if (result < 0 && errno != EINTR)
        break;
    }
  g_socket_close (listener, NULL);
  if (peer)
    {
      g_socket_set_blocking (peer, FALSE);
      gateway.peer = g_socket_get_fd (peer);
      run_gateway (&gateway);
    }
  gateway_clear (&gateway);
  return gateway.failed ? 1 : 0;
}
