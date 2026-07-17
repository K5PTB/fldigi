// ---------------------------------------------------------------------
//
// WSclient, a part of fldigi (ported from flrig)
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

#ifndef WSCLIENT_HP
#define WSCLIENT_HP

#include <string>
#include <vector>

#ifdef __WIN32__
	typedef unsigned char uint8_t;
#else
	#include <stdint.h>
#endif

namespace WSclient {

struct Callback_Imp { virtual void operator()(const std::string& message) = 0; };
struct BytesCallback_Imp { virtual void operator()(const std::vector<uint8_t>& message) = 0; };

class WebSocket {
  public:
    typedef WebSocket * pointer;
    typedef enum readyStateValues { CLOSING, CLOSED, CONNECTING, OPEN } readyStateValues;

    // Factories:
    static pointer create_dummy();
    static pointer from_url(const std::string& url, const std::string& origin = std::string());
    static pointer from_url_no_mask(const std::string& url, const std::string& origin = std::string());

    // Interfaces:
    virtual ~WebSocket() { }
    virtual void poll(int timeout = 0) = 0; // timeout in milliseconds
    virtual void send(const std::string& message) = 0;
    virtual void sendBinary(const std::string& message) = 0;
    virtual void sendBinary(const std::vector<uint8_t>& message) = 0;
    virtual void sendPing() = 0;
    virtual void close() = 0;
    virtual readyStateValues getReadyState() const = 0;

    // A connection carrying both text and binary frames on one socket (TCI CAT
    // + audio) must dispatch in a single pass that routes each frame to the
    // callback matching its real opcode. The earlier design had separate
    // dispatch()/dispatchBinary() passes, each of which drained every buffered
    // frame regardless of opcode -- so dispatch() converted binary frames to
    // garbled strings for the text callback and dispatchBinary() then found
    // nothing left. Those are gone; this is the only dispatcher.
    template<class TextCallable, class BinCallable>
    void dispatchCombined(TextCallable textCallable, BinCallable binCallable)
    {
        struct _TextCallback : public Callback_Imp {
            TextCallable& callable;
            _TextCallback(TextCallable& callable) : callable(callable) { }
            void operator()(const std::string& message) { callable(message); }
        };
        struct _BinCallback : public BytesCallback_Imp {
            BinCallable& callable;
            _BinCallback(BinCallable& callable) : callable(callable) { }
            void operator()(const std::vector<uint8_t>& message) { callable(message); }
        };
        _TextCallback textCb(textCallable);
        _BinCallback binCb(binCallable);
        _dispatchCombined(textCb, binCb);
    }

  protected:
    virtual void _dispatchCombined(Callback_Imp& textCallable, BytesCallback_Imp& binCallable) = 0;
};

} // namespace WSclient

#endif /* EASYWSCLIENT_HPP_20120819_MIOFVASDTNUASZDQPLFD */
