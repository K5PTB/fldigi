// ---------------------------------------------------------------------
//
// WSclient.cxx, a part of fldigi (ported from flrig)
//
// Copyright (C) 2022
// Dave Freese, W1HKJ
//
// This code is derived from:
// https://github.com/dhbaird/easywsclient
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
// GNU General Public License for more detailrx.
//
// You should have received a copy of the GNU General Public License
// along with the program; if not, write to the
//
//  Free Software Foundation, Inc.
//  51 Franklin Street, Fifth Floor
//  Boston, MA  02110-1301 USA.
//
// ---------------------------------------------------------------------


#ifdef _WIN32
	#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
		#define _CRT_SECURE_NO_WARNINGS // _CRT_SECURE_NO_WARNINGS for sscanf errors in MSVC2013 Express
	#endif
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <fcntl.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <stdio.h>
	#include <stdlib.h>
	#include <string.h>
	#include <sys/types.h>
	#include <io.h>

	#ifndef _SSIZE_T_DEFINED
		typedef int ssize_t;
		#define _SSIZE_T_DEFINED
	#endif
	#ifndef _SOCKET_T_DEFINED
		typedef SOCKET socket_t;
		#define _SOCKET_T_DEFINED
	#endif
	#ifndef snprintf
		#define snprintf _snprintf_s
	#endif

	#include <stdint.h>

	#define socketerrno WSAGetLastError()
	#define SOCKET_EAGAIN_EINPROGRESS WSAEINPROGRESS
	#define SOCKET_EWOULDBLOCK WSAEWOULDBLOCK
#else
	#include <fcntl.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <stdio.h>
	#include <stdlib.h>
	#include <string.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <sys/types.h>
	#include <unistd.h>
	#include <stdint.h>
	#ifndef _SOCKET_T_DEFINED
		typedef int socket_t;
		#define _SOCKET_T_DEFINED
	#endif
	#ifndef INVALID_SOCKET
		#define INVALID_SOCKET (-1)
	#endif
	#ifndef SOCKET_ERROR
		#define SOCKET_ERROR   (-1)
	#endif
	#define closesocket(s) ::close(s)
	#include <errno.h>
	#define socketerrno errno
	#define SOCKET_EAGAIN_EINPROGRESS EAGAIN
	#define SOCKET_EWOULDBLOCK EWOULDBLOCK
#endif

#include <vector>
#include <string>

#include <atomic>
#include <pthread.h>
#include <cctype>

#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "WSclient.h"

using WSclient::Callback_Imp;
using WSclient::BytesCallback_Imp;

namespace { // private module-only namespace

// Bounds against an untrusted, unauthenticated peer. The ws:// link has no
// authentication, so any framing field is attacker-controlled; these keep a
// bad or hostile declaration from turning into unbounded memory growth on the
// receiver thread (a pthread entry point with no exception handler).
const int WS_MAX_FRAME = 1 << 20;   // 1 MiB; TCI RX_AUDIO frames are ~8 KB
const int WS_MAX_TXBUF = 8 << 20;   // 8 MiB of unsent TX before we give up

// WS_MAX_FRAME bounds a single frame, but a fragmented message accumulates
// across continuation frames into receivedData; without a separate ceiling a
// peer could stream unbounded sub-WS_MAX_FRAME fragments with fin=0 and defeat
// the per-frame cap. TCI messages are never fragmented in practice, so this is
// generous headroom.
const size_t WS_MAX_MESSAGE = 8 << 20; // 8 MiB reassembled across fragments

// RFC 6455 4.1: this magic GUID is appended to the client's Sec-WebSocket-Key
// and SHA1'd; the server returns base64 of that as Sec-WebSocket-Accept, and a
// conformant client MUST verify it.
const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// One CSPRNG for the whole module, seeded once. mbedtls_ctr_drbg_random() is
// not thread-safe and both threads draw from it (the main thread for the
// handshake nonce, the receiver thread for per-frame masking keys), so every
// draw goes through this mutex. Mirrors the mbedtls setup already used by the
// Url class in network.cxx.
pthread_mutex_t ws_rng_mutex = PTHREAD_MUTEX_INITIALIZER;
mbedtls_ctr_drbg_context ws_ctr_drbg;
mbedtls_entropy_context ws_entropy;
bool ws_rng_ready = false;

// Fill out[0..n) with cryptographic random bytes. Falls back to nothing usable
// only if seeding fails, which mbedtls treats as fatal; callers here always
// have a key either way, just not a random one on a broken-entropy system.
void ws_random_bytes(uint8_t* out, size_t n)
{
	pthread_mutex_lock(&ws_rng_mutex);
	if (!ws_rng_ready) {
		mbedtls_ctr_drbg_init(&ws_ctr_drbg);
		mbedtls_entropy_init(&ws_entropy);
		const char* pers = "fldigi-wsclient";
		if (mbedtls_ctr_drbg_seed(&ws_ctr_drbg, mbedtls_entropy_func, &ws_entropy,
					  (const unsigned char*)pers, strlen(pers)) == 0)
			ws_rng_ready = true;
	}
	if (!ws_rng_ready || mbedtls_ctr_drbg_random(&ws_ctr_drbg, out, n) != 0) {
		// Seeding/draw failed. Do not emit a predictable constant silently;
		// leave whatever the caller passed. In practice ctr_drbg_seed does
		// not fail on any platform fldigi runs on.
		fprintf(stderr, "WSclient: RNG unavailable\n");
	}
	pthread_mutex_unlock(&ws_rng_mutex);
}

std::string ws_base64(const uint8_t* in, size_t n)
{
	unsigned char buf[128];
	size_t olen = 0;
	if (mbedtls_base64_encode(buf, sizeof(buf), &olen, in, n) != 0)
		return std::string();
	return std::string((char*)buf, olen);
}

// base64( SHA1( key + GUID ) ) -- the expected Sec-WebSocket-Accept.
std::string ws_accept_for(const std::string& key)
{
	std::string s = key + WS_GUID;
	unsigned char digest[20];
	mbedtls_sha1_ret((const unsigned char*)s.data(), s.size(), digest);
	return ws_base64(digest, sizeof(digest));
}

// Cap on the TCP connect. tci_open() runs on the FLTK main thread, so a
// blocking connect() to a host that silently drops SYNs freezes the whole UI
// for the OS timeout (~75s on Linux/macOS) with no way to cancel. The earlier
// SO_RCVTIMEO fix bounded only the post-connect handshake reads, not this.
const int WS_CONNECT_TIMEOUT_SEC = 10;

// connect() with a bounded wait: switch the socket to non-blocking, start the
// connect, select() on writability, then restore blocking mode so the existing
// handshake code (which uses blocking recv + SO_RCVTIMEO) runs unchanged.
// Returns true on success. Errors and timeouts leave the caller to close the fd.
bool connect_with_timeout(socket_t sockfd, const struct sockaddr* addr,
			  socklen_t addrlen, int timeout_sec) {
#ifdef _WIN32
	u_long nb = 1; ioctlsocket(sockfd, FIONBIO, &nb);
	const int inprogress = WSAEWOULDBLOCK;
#else
	// If F_GETFL fails, don't force a bogus flag value onto the socket (which
	// would corrupt its blocking mode on restore); fall back to a plain
	// blocking connect() -- no bounded wait, but correct. connect() below then
	// returns 0/-1 directly rather than EINPROGRESS.
	int flags = fcntl(sockfd, F_GETFL, 0);
	bool nonblock = (flags != -1) && (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) != -1);
	const int inprogress = EINPROGRESS;
#endif

	bool ok = false;
	if (connect(sockfd, addr, addrlen) == 0) {
		ok = true;                       // immediate (loopback/cached)
	}
	else if (socketerrno == inprogress) {
		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(sockfd, &wfds);
		timeval tv = { timeout_sec, 0 };
		int s = select(sockfd + 1, NULL, &wfds, NULL, &tv);
		if (s > 0) {
			// Writable does not mean connected -- check the pending error.
			int err = 0; socklen_t len = sizeof(err);
			if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
				ok = true;
		}
		// s == 0 is the timeout; s < 0 is a select() error. Both fail.
	}

#ifdef _WIN32
	nb = 0; ioctlsocket(sockfd, FIONBIO, &nb);
#else
	if (nonblock)
		fcntl(sockfd, F_SETFL, flags);   // restore original (blocking) flags
#endif
	return ok;
}

socket_t hostname_connect(const std::string& hostname, int port) {
	struct addrinfo hints;
	struct addrinfo *result;
	struct addrinfo *p;
	int ret;
	socket_t sockfd = INVALID_SOCKET;
	char sport[16];
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(sport, 16, "%d", port);
	if ((ret = getaddrinfo(hostname.c_str(), sport, &hints, &result)) != 0)
	{
	  fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
	  // Not "return 1": the caller tests against INVALID_SOCKET (-1), so a
	  // literal 1 sails through the guard -- and 1 is stdout. An unresolvable
	  // host then had the handshake ::send() onto fd 1, recv() read back from
	  // it, and _RealWebSocket's destructor closesocket(1) the process's
	  // stdout for good.
	  return INVALID_SOCKET;
	}
	for(p = result; p != NULL; p = p->ai_next)
	{
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == INVALID_SOCKET) { continue; }
		if (connect_with_timeout(sockfd, p->ai_addr, p->ai_addrlen, WS_CONNECT_TIMEOUT_SEC)) {
			break;
		}
		closesocket(sockfd);
		sockfd = INVALID_SOCKET;
	}
	freeaddrinfo(result);
	return sockfd;
}

// Send the whole buffer, looping over short writes and retrying on EINTR. The
// framed data path (poll()) already loops; the blocking handshake writes did
// not, so a partial write or a signal could truncate a request line.
static bool ws_send_all(socket_t fd, const char* buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = ::send(fd, buf + off, len - off, 0);
		if (n > 0) { off += (size_t)n; continue; }
		if (n < 0 && socketerrno == EINTR) continue;
		return false;
	}
	return true;
}


class _DummyWebSocket : public WSclient::WebSocket
{
  public:
	void poll(int timeout) { }
	void send(const std::string& message) { }
	void sendBinary(const std::string& message) { }
	void sendBinary(const std::vector<uint8_t>& message) { }
	void sendPing() { }
	void close() { }
	readyStateValues getReadyState() const { return CLOSED; }
	void _dispatchCombined(Callback_Imp&, BytesCallback_Imp&) { }
};


class _RealWebSocket : public WSclient::WebSocket
{
  public:
	// http://tools.ietf.org/html/rfc6455#section-5.2  Base Framing Protocol
	//
	//  0                   1                   2                   3
	//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	// +-+-+-+-+-------+-+-------------+-------------------------------+
	// |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
	// |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
	// |N|V|V|V|       |S|             |   (if payload len==126/127)   |
	// | |1|2|3|       |K|             |                               |
	// +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
	// |     Extended payload length continued, if payload len == 127  |
	// + - - - - - - - - - - - - - - - +-------------------------------+
	// |                               |Masking-key, if MASK set to 1  |
	// +-------------------------------+-------------------------------+
	// | Masking-key (continued)       |          Payload Data         |
	// +-------------------------------- - - - - - - - - - - - - - - - +
	// :                     Payload Data continued ...                :
	// + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
	// |                     Payload Data continued ...                |
	// +---------------------------------------------------------------+
	struct wsheader_type {
		unsigned header_size;
		bool fin;
		bool mask;
		enum opcode_type {
			CONTINUATION = 0x0,
			TEXT_FRAME = 0x1,
			BINARY_FRAME = 0x2,
			CLOSE = 8,
			PING = 9,
			PONG = 0xa,
		} opcode;
		int N0;
		uint64_t N;
		uint8_t masking_key[4];
	};

	std::vector<uint8_t> rxbuf;
	std::vector<uint8_t> txbuf;
	std::vector<uint8_t> receivedData;

	socket_t sockfd;
	// Written by the receiver thread (poll()/dispatch/close_socket) and read
	// from trx_thread and the main thread (getReadyState() via tci_running()/
	// tci_connected()). Atomic so those cross-thread reads are well-defined
	// without a lock -- the ws pointer's lifetime is what run_mutex guards.
	std::atomic<readyStateValues> readyState;
	bool useMask;
	bool isRxBad;
	int fragment_opcode; // opcode of the fragmented message _dispatchCombined() is currently reassembling

	_RealWebSocket(socket_t sockfd, bool useMask)
			: sockfd(sockfd)
			, readyState(OPEN)
			, useMask(useMask)
			, isRxBad(false)
			, fragment_opcode(wsheader_type::TEXT_FRAME) {
	}

	// close()/poll() only queue a close frame and rely on a later poll() to
	// actually call closesocket() once it's flushed -- if the owner deletes
	// the object without that full round-trip (e.g. after joining the
	// thread that would have driven poll()), the fd leaks and the peer
	// still sees an ESTABLISHED connection indefinitely. Guarantee cleanup
	// here regardless of how the object was torn down.
	~_RealWebSocket() {
		close_socket();
	}

	readyStateValues getReadyState() const {
	  return readyState;
	}

	// Close exactly once, and never leave sockfd holding a number the kernel
	// has freed. The old code closed the socket in whichever loop hit EOF and
	// then let the OTHER loop keep running on the same descriptor: the recv
	// loop's close was followed straight into the send loop, which ::send()'d
	// on the closed fd and closesocket()'d it a second time. On POSIX the fd
	// number is reusable the instant the first close returns, and fldigi runs
	// several other socket threads (xmlrpc, ARQ, KISS, flrig, the logbook) --
	// so that second operation could hit an unrelated connection that had just
	// been handed the same number. Setting sockfd = INVALID_SOCKET turns any
	// use-after-close into a harmless EBADF instead of silent cross-talk.
	void close_socket() {
		if (sockfd != INVALID_SOCKET) {
			closesocket(sockfd);
			sockfd = INVALID_SOCKET;
		}
		readyState = CLOSED;
	}

	void poll(int timeout) { // timeout in milliseconds
		if (readyState == CLOSED) {
			if (timeout > 0) {
				timeval tv = { timeout/1000, (timeout%1000) * 1000 };
				select(0, NULL, NULL, NULL, &tv);
			}
			return;
		}
		if (timeout != 0) {
			fd_set rfds;
			fd_set wfds;
			timeval tv = { timeout/1000, (timeout%1000) * 1000 };
			FD_ZERO(&rfds);
			FD_ZERO(&wfds);
			FD_SET(sockfd, &rfds);
			if (txbuf.size()) { FD_SET(sockfd, &wfds); }
			select(sockfd + 1, &rfds, &wfds, 0, timeout > 0 ? &tv : 0);
		}
		while (true) {
			// FD_ISSET(0, &rfds) will be true
			size_t N = rxbuf.size();
			ssize_t ret;
			rxbuf.resize(N + 1500);
			ret = recv(sockfd, (char*)&rxbuf[0] + N, 1500, 0);
			if (false) { }
			else if (ret < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
				rxbuf.resize(N);
				break;
			}
			else if (ret <= 0) {
				rxbuf.resize(N);
				fputs(ret < 0 ? "Connection error!\n" : "Connection closed!\n", stderr);
				close_socket();
				break;
			}
			else {
				rxbuf.resize(N + ret);
			}
		}
		// The recv loop may have closed the connection; do not run the send
		// loop or the CLOSING check on a dead descriptor.
		if (readyState == CLOSED)
			return;
		while (txbuf.size()) {
			int ret = ::send(sockfd, (char*)&txbuf[0], txbuf.size(), 0);
			if (false) { } // ??
			else if (ret < 0 && (socketerrno == SOCKET_EWOULDBLOCK || socketerrno == SOCKET_EAGAIN_EINPROGRESS)) {
				break;
			}
			else if (ret <= 0) {
				fputs(ret < 0 ? "Connection error!\n" : "Connection closed!\n", stderr);
				close_socket();
				break;
			}
			else {
				txbuf.erase(txbuf.begin(), txbuf.begin() + ret);
			}
		}
		if (readyState == CLOSED)
			return;
		if (!txbuf.size() && readyState == CLOSING)
			close_socket();
	}


	// Single pass over rxbuf that routes each complete frame to the
	// callback matching its real opcode, instead of _dispatch()/
	// _dispatchBinary() each independently draining (and mis-typing) every
	// frame regardless of opcode -- see the dispatchCombined() comment in
	// WSclient.h for why that combination is unsafe on a connection
	// carrying both text and binary frames.
	virtual void _dispatchCombined(Callback_Imp& textCallable, BytesCallback_Imp& binCallable) {
		if (isRxBad) {
			return;
		}
		while (true) {
			wsheader_type ws;
			if (rxbuf.size() < 2) { return; }
			const uint8_t * data = (uint8_t *) &rxbuf[0]; // peek, but don't consume
			ws.fin = (data[0] & 0x80) == 0x80;
			ws.opcode = (wsheader_type::opcode_type) (data[0] & 0x0f);
			ws.mask = (data[1] & 0x80) == 0x80;
			ws.N0 = (data[1] & 0x7f);
			ws.header_size = 2 + (ws.N0 == 126? 2 : 0) + (ws.N0 == 127? 8 : 0) + (ws.mask? 4 : 0);
			if (rxbuf.size() < ws.header_size) { return; }

			// RFC 6455 5.2: no extension is negotiated (the handshake sends no
			// Sec-WebSocket-Extensions and discards the response headers), so
			// any RSV bit set must fail the connection. Accepting it silently
			// stripped RSV and fed e.g. a permessage-deflate payload into
			// handle_binary() as if it were raw audio.
			if (data[0] & 0x70) {
				isRxBad = true;
				fprintf(stderr, "ERROR: reserved bits set in frame. Closing.\n");
				close();
				return;
			}

			// RFC 6455 5.5: control frames (CLOSE/PING/PONG) carry at most 125
			// bytes and must not be fragmented. Without this a peer could send
			// a PING with a 64-bit length, and the PING branch below echoed
			// whatever it received straight back as a PONG -- an invalid
			// control frame fldigi itself generated, which a conformant peer
			// answers with a protocol-error close mid-QSO.
			if ((ws.opcode & 0x8) && (ws.N0 > 125 || !ws.fin)) {
				isRxBad = true;
				fprintf(stderr, "ERROR: malformed control frame. Closing.\n");
				close();
				return;
			}
			int i = 0;
			if (ws.N0 < 126) {
				ws.N = ws.N0;
				i = 2;
			}
			else if (ws.N0 == 126) {
				ws.N = 0;
				ws.N |= ((uint64_t) data[2]) << 8;
				ws.N |= ((uint64_t) data[3]) << 0;
				i = 4;
			}
			else if (ws.N0 == 127) {
				ws.N = 0;
				ws.N |= ((uint64_t) data[2]) << 56;
				ws.N |= ((uint64_t) data[3]) << 48;
				ws.N |= ((uint64_t) data[4]) << 40;
				ws.N |= ((uint64_t) data[5]) << 32;
				ws.N |= ((uint64_t) data[6]) << 24;
				ws.N |= ((uint64_t) data[7]) << 16;
				ws.N |= ((uint64_t) data[8]) << 8;
				ws.N |= ((uint64_t) data[9]) << 0;
				i = 10;
				if (ws.N & 0x8000000000000000ull) {
					isRxBad = true;
					fprintf(stderr, "ERROR: Frame has invalid frame length. Closing.\n");
					close();
					return;
				}
			}
			if (ws.mask) {
				ws.masking_key[0] = ((uint8_t) data[i+0]) << 0;
				ws.masking_key[1] = ((uint8_t) data[i+1]) << 0;
				ws.masking_key[2] = ((uint8_t) data[i+2]) << 0;
				ws.masking_key[3] = ((uint8_t) data[i+3]) << 0;
			}
			else {
				ws.masking_key[0] = 0;
				ws.masking_key[1] = 0;
				ws.masking_key[2] = 0;
				ws.masking_key[3] = 0;
			}

			// Cap the declared frame size. The MSB check above only rejects
			// lengths >= 2^63; everything below that was accepted, and until
			// the whole frame arrives the code just `return`s and waits while
			// poll() keeps appending to rxbuf. So an oversized declaration was
			// not a bad frame, it was unbounded memory growth ending in
			// std::bad_alloc on the receiver thread -- which is a pthread entry
			// point with no handler, so std::terminate(). A buggy server does
			// this as readily as a hostile one. TCI RX_AUDIO frames are ~8 KB;
			// WS_MAX_FRAME (1 MiB) is orders of magnitude of headroom.
			if (ws.N > WS_MAX_FRAME) {
				isRxBad = true;
				fprintf(stderr, "ERROR: frame length exceeds %d bytes. Closing.\n", WS_MAX_FRAME);
				close();
				return;
			}

			if (rxbuf.size() < ws.header_size+ws.N) { return; }

			if (false) { }
			else if (
				   ws.opcode == wsheader_type::TEXT_FRAME
				|| ws.opcode == wsheader_type::BINARY_FRAME
				|| ws.opcode == wsheader_type::CONTINUATION
			) {
				// Bound the reassembled total, not just each frame: the
				// per-frame WS_MAX_FRAME check above does nothing against a peer
				// that streams many sub-1-MiB continuation frames with fin=0,
				// which would grow receivedData without limit.
				if (receivedData.size() + (size_t)ws.N > WS_MAX_MESSAGE) {
					isRxBad = true;
					fprintf(stderr, "ERROR: reassembled message exceeds %zu bytes. Closing.\n", WS_MAX_MESSAGE);
					close();
					return;
				}
				if (ws.mask) { for (size_t i = 0; i != ws.N; ++i) { rxbuf[i+ws.header_size] ^= ws.masking_key[i&0x3]; } }
				if (ws.opcode != wsheader_type::CONTINUATION)
					fragment_opcode = ws.opcode; // remember for the CONTINUATION frames that follow
				receivedData.insert(receivedData.end(), rxbuf.begin()+ws.header_size, rxbuf.begin()+ws.header_size+(size_t)ws.N);
				if (ws.fin) {
					if (fragment_opcode == wsheader_type::BINARY_FRAME)
						binCallable(receivedData);
					else
						textCallable(std::string(receivedData.begin(), receivedData.end()));
					receivedData.erase(receivedData.begin(), receivedData.end());
					std::vector<uint8_t> ().swap(receivedData);
				}
			}
			else if (ws.opcode == wsheader_type::PING) {
				if (ws.mask) { for (size_t i = 0; i != ws.N; ++i) { rxbuf[i+ws.header_size] ^= ws.masking_key[i&0x3]; } }
				std::string data(rxbuf.begin()+ws.header_size, rxbuf.begin()+ws.header_size+(size_t)ws.N);
				sendData(wsheader_type::PONG, data.size(), data.begin(), data.end());
			}
			else if (ws.opcode == wsheader_type::PONG) { }
			else if (ws.opcode == wsheader_type::CLOSE) {
				// RFC 6455 5.5.1: once a Close is received, echo the Close and
				// process no further frames. isRxBad short-circuits later
				// _dispatchCombined() calls; poll() completes the teardown.
				close();
				isRxBad = true;
				return;
			}
			else { fprintf(stderr, "ERROR: Got unexpected WebSocket message.\n"); close(); }

			rxbuf.erase(rxbuf.begin(), rxbuf.begin() + ws.header_size+(size_t)ws.N);
		}
	}

	void sendPing() {
		std::string empty;
		sendData(wsheader_type::PING, empty.size(), empty.begin(), empty.end());
	}

	void send(const std::string& message) {
		sendData(wsheader_type::TEXT_FRAME, message.size(), message.begin(), message.end());
	}

	void sendBinary(const std::string& message) {
		sendData(wsheader_type::BINARY_FRAME, message.size(), message.begin(), message.end());
	}

	void sendBinary(const std::vector<uint8_t>& message) {
		sendData(wsheader_type::BINARY_FRAME, message.size(), message.begin(), message.end());
	}

	template<class Iterator>
	void sendData(wsheader_type::opcode_type type, uint64_t message_size, Iterator message_begin, Iterator message_end) {
		// TODO:
		// Masking key should (must) be derived from a high quality random
		// number generator, to mitigate attacks on non-WebSocket friendly
		// middleware:
		// RFC 6455 5.3: a fresh, unpredictable masking key per frame. The old
		// fixed constant made every byte fldigi sent trivially predictable,
		// defeating the anti-cache-poisoning purpose masking exists for.
		uint8_t masking_key[4];
		ws_random_bytes(masking_key, sizeof(masking_key));
		// TODO: consider acquiring a lock on txbuf...
		if (readyState == CLOSING || readyState == CLOSED) { return; }

		// Shed rather than buffer without bound. poll() flushes txbuf on a
		// non-blocking socket, so if the peer stops reading (stalled but not
		// dropped -- a paused VM, a congested link) the send loop just breaks
		// on EWOULDBLOCK and this keeps appending ~8 KB per TX_CHRONO, ~380
		// KB/s, until bad_alloc terminates the receiver thread. Past the cap
		// the connection is already failing to keep up, so drop the frame and
		// fail the socket rather than the process. The cap is well above the
		// handful of frames of legitimate in-flight TX audio.
		if (txbuf.size() > WS_MAX_TXBUF) {
			fprintf(stderr, "ERROR: send backlog exceeds %d bytes, peer not reading. Closing.\n", WS_MAX_TXBUF);
			close_socket();
			return;
		}
		std::vector<uint8_t> header;
		header.assign(2 + (message_size >= 126 ? 2 : 0) + (message_size >= 65536 ? 6 : 0) + (useMask ? 4 : 0), 0);
		header[0] = 0x80 | type;
		if (false) { }
		else if (message_size < 126) {
			header[1] = (message_size & 0xff) | (useMask ? 0x80 : 0);
			if (useMask) {
				header[2] = masking_key[0];
				header[3] = masking_key[1];
				header[4] = masking_key[2];
				header[5] = masking_key[3];
			}
		}
		else if (message_size < 65536) {
			header[1] = 126 | (useMask ? 0x80 : 0);
			header[2] = (message_size >> 8) & 0xff;
			header[3] = (message_size >> 0) & 0xff;
			if (useMask) {
				header[4] = masking_key[0];
				header[5] = masking_key[1];
				header[6] = masking_key[2];
				header[7] = masking_key[3];
			}
		}
		else { // TODO: run coverage testing here
			header[1] = 127 | (useMask ? 0x80 : 0);
			header[2] = (message_size >> 56) & 0xff;
			header[3] = (message_size >> 48) & 0xff;
			header[4] = (message_size >> 40) & 0xff;
			header[5] = (message_size >> 32) & 0xff;
			header[6] = (message_size >> 24) & 0xff;
			header[7] = (message_size >> 16) & 0xff;
			header[8] = (message_size >>  8) & 0xff;
			header[9] = (message_size >>  0) & 0xff;
			if (useMask) {
				header[10] = masking_key[0];
				header[11] = masking_key[1];
				header[12] = masking_key[2];
				header[13] = masking_key[3];
			}
		}
		// N.B. - txbuf will keep growing until it can be transmitted over the socket:
		txbuf.insert(txbuf.end(), header.begin(), header.end());
		txbuf.insert(txbuf.end(), message_begin, message_end);
		if (useMask) {
			size_t message_offset = txbuf.size() - message_size;
			for (size_t i = 0; i != message_size; ++i) {
				txbuf[message_offset + i] ^= masking_key[i&0x3];
			}
		}
	}

	void close() {
		if(readyState == CLOSING || readyState == CLOSED) { return; }
		readyState = CLOSING;
		// FIN + CLOSE, zero-length payload. Honor useMask (the old frame set
		// the MASK bit unconditionally, even for from_url_no_mask connections)
		// and use a random key rather than a hardcoded zero one.
		if (useMask) {
			uint8_t key[4];
			ws_random_bytes(key, sizeof(key));
			uint8_t frame[6] = { 0x88, 0x80, key[0], key[1], key[2], key[3] };
			txbuf.insert(txbuf.end(), frame, frame + 6);
		}
		else {
			uint8_t frame[2] = { 0x88, 0x00 };
			txbuf.insert(txbuf.end(), frame, frame + 2);
		}
	}

};


WSclient::WebSocket::pointer from_url(const std::string& url, bool useMask, const std::string& origin) {
	char host[512];
	int port;
	char path[512];
	if (url.size() >= 512) {
	  fprintf(stderr, "ERROR: url size limit exceeded: %s\n", url.c_str());
	  return NULL;
	}
	if (origin.size() >= 200) {
	  fprintf(stderr, "ERROR: origin size limit exceeded: %s\n", origin.c_str());
	  return NULL;
	}
	if (false) { }
	else if (sscanf(url.c_str(), "ws://%[^:/]:%d/%s", host, &port, path) == 3) {
	}
	else if (sscanf(url.c_str(), "ws://%[^:/]/%s", host, path) == 2) {
		port = 80;
	}
	else if (sscanf(url.c_str(), "ws://%[^:/]:%d", host, &port) == 2) {
		path[0] = '\0';
	}
	else if (sscanf(url.c_str(), "ws://%[^:/]", host) == 1) {
		port = 80;
		path[0] = '\0';
	}
	else {
		fprintf(stderr, "ERROR: Could not parse WebSocket url: %s\n", url.c_str());
		return NULL;
	}
	//fprintf(stderr, "WSclient: connecting: host=%s port=%d path=/%s\n", host, port, path);
	socket_t sockfd = hostname_connect(host, port);
	if (sockfd == INVALID_SOCKET) {
		fprintf(stderr, "Unable to connect to %s:%d\n", host, port);
		return NULL;
	}
	{
		// XXX: this should be done non-blocking,
		char line[1024];
		int status;
		int i;

		// Bound the handshake reads. The socket is still blocking here --
		// O_NONBLOCK is only set further down, after the handshake -- and the
		// loops below recv() one byte at a time with no timeout of their own.
		// A peer that completes the TCP connect and then says nothing (port
		// 22, a hung TCI server) would block here forever, and tci_open() runs
		// on the FLTK main thread, so that freezes the entire UI with no way
		// to cancel. Harmless once the socket goes non-blocking below, where
		// SO_RCVTIMEO no longer applies.
#ifdef _WIN32
		DWORD tv = 5000;
#else
		struct timeval tv;
		tv.tv_sec = 5;
		tv.tv_usec = 0;
#endif
		setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*) &tv, sizeof(tv));

		// Fresh random 16-byte nonce per connection, base64'd, per RFC 6455
		// 4.1 -- not the fixed easywsclient example key. The expected
		// Sec-WebSocket-Accept is base64(SHA1(key + GUID)); we verify it below.
		uint8_t nonce[16];
		ws_random_bytes(nonce, sizeof(nonce));
		std::string ws_key = ws_base64(nonce, sizeof(nonce));
		std::string expect_accept = ws_accept_for(ws_key);

		// Build the whole request and send it with a short-write/EINTR-safe
		// loop. The old code fired ~10 bare ::send() calls and discarded every
		// return value, so a partial write or an EINTR truncated a header line
		// and the server rejected the upgrade with no useful diagnostic.
		std::string req;
		req.reserve(320);
		snprintf(line, 1024, "GET /%s HTTP/1.1\r\n", path); req += line;
		if (port == 80) snprintf(line, 1024, "Host: %s\r\n", host);
		else            snprintf(line, 1024, "Host: %s:%d\r\n", host, port);
		req += line;
		req += "Upgrade: websocket\r\n";
		req += "Connection: Upgrade\r\n";
		if (!origin.empty()) {
			snprintf(line, 1024, "Origin: %s\r\n", origin.c_str()); req += line;
		}
		snprintf(line, 1024, "Sec-WebSocket-Key: %s\r\n", ws_key.c_str()); req += line;
		req += "Sec-WebSocket-Version: 13\r\n";
		req += "\r\n";
		if (!ws_send_all(sockfd, req.data(), req.size())) {
			fprintf(stderr, "ERROR: handshake request send failed to %s\n", url.c_str());
			closesocket(sockfd);
			return NULL;
		}
		// recv() <= 0, not == 0: the original only treated a clean EOF as
		// failure, so an error or (now) a timeout returning -1 fell through
		// and spun i up to 1023 over uninitialized line[] bytes, reporting a
		// bogus "invalid status line" instead of the real failure.
		//
		// Every failure exit below closes the socket. They all used to return
		// NULL with it still open, and tci_init() retries on each Rig Control
		// apply -- so pointing TCI at a plain HTTP server burned one fd per
		// attempt and left the peer holding an ESTABLISHED connection until
		// fldigi exited.
		for (i = 0; i < 2 || (i < 1023 && line[i-2] != '\r' && line[i-1] != '\n'); ++i) { if (recv(sockfd, line+i, 1, 0) <= 0) { closesocket(sockfd); return NULL; } }
		line[i] = 0;
		if (i == 1023) { fprintf(stderr, "ERROR: Got invalid status line connecting to: %s\n", url.c_str()); closesocket(sockfd); return NULL; }
		if (sscanf(line, "HTTP/1.1 %d", &status) != 1 || status != 101) { fprintf(stderr, "ERROR: Got bad status connecting to %s: %s", url.c_str(), line); closesocket(sockfd); return NULL; }

		// Read the response headers, capturing Sec-WebSocket-Accept. RFC 6455
		// 4.1 requires the client to fail the connection unless it equals
		// base64(SHA1(key + GUID)) -- without this, any HTTP server returning
		// 101 was accepted as a WebSocket peer and its response body was then
		// parsed as frames.
		bool accept_ok = false;
		while (true) {
			for (i = 0; i < 2 || (i < 1023 && line[i-2] != '\r' && line[i-1] != '\n'); ++i) { if (recv(sockfd, line+i, 1, 0) <= 0) { closesocket(sockfd); return NULL; } }
			line[i] = 0;
			if (line[0] == '\r' && line[1] == '\n') { break; }

			// Case-insensitive match on the header name, then compare the
			// value (trimming spaces and the trailing CRLF) against expected.
			const char* hdr = "sec-websocket-accept:";
			size_t hlen = strlen(hdr);
			bool match = true;
			for (size_t k = 0; k < hlen; k++)
				if (tolower((unsigned char)line[k]) != hdr[k]) { match = false; break; }
			if (match) {
				const char* v = line + hlen;
				while (*v == ' ' || *v == '\t') v++;
				std::string got(v);
				while (!got.empty() && (got.back() == '\r' || got.back() == '\n' || got.back() == ' '))
					got.erase(got.size() - 1);
				accept_ok = (got == expect_accept);
			}
		}
		if (!accept_ok) {
			fprintf(stderr, "ERROR: Sec-WebSocket-Accept mismatch from %s -- not a WebSocket peer\n", url.c_str());
			closesocket(sockfd);
			return NULL;
		}
	}
	int flag = 1;
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(flag)); // Disable Nagle's algorithm
#ifdef _WIN32
	u_long on = 1;
	ioctlsocket(sockfd, FIONBIO, &on);
#else
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
#endif
	//fprintf(stderr, "Connected to: %s\n", url.c_str());
	return WSclient::WebSocket::pointer(new _RealWebSocket(sockfd, useMask));
}

} // end of module-only namespace



namespace WSclient {

WebSocket::pointer WebSocket::create_dummy() {
	static pointer dummy = pointer(new _DummyWebSocket);
	return dummy;
}


WebSocket::pointer WebSocket::from_url(const std::string& url, const std::string& origin) {
	return ::from_url(url, true, origin);
}

WebSocket::pointer WebSocket::from_url_no_mask(const std::string& url, const std::string& origin) {
	return ::from_url(url, false, origin);
}


} // namespace WSclient
