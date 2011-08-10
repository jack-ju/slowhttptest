/*****************************************************************************
*  Copyright 2011 Sergey Shekyan
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
* *****************************************************************************/

/*****
 * Author: Sergey Shekyan sshekyan@qualys.com
 *
 * Slow HTTP attack  vulnerability test tool
 *  http://code.google.com/p/slowhttptest/
 *****/

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <algorithm>
#include <string>

#include <openssl/ssl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "slowlog.h"
#include "slowsocket.h"
#include "slowurl.h"

namespace slowhttptest {
SlowSocket::SlowSocket()
    : sockfd_(-1), requests_to_send_(0),
      followups_to_send_(0), last_followup_timing_(0),
      offset_(0), ssl_(0), buf_(0), 
      start_in_millisecs_(0), connected_in_millisecs_(0),
      stop_in_millisecs_(0), state_(eInit) {
}

SlowSocket::~SlowSocket() {
  close();
}

int SlowSocket::set_nonblocking() {
  int flags;

  if(-1 == (flags = fcntl(sockfd_, F_GETFL, 0))) {
    flags = 0;
  }
  return fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);
}

bool SlowSocket::init(addrinfo* addr, const Url* url, int& maxfd,
                      int followups_to_send) {
  addrinfo* res;
  bool connect_initiated_ = false;
  for (res = addr; !connect_initiated_ && res; res = res->ai_next) {
    sockfd_ = socket(res->ai_family, res->ai_socktype,
                     res->ai_protocol);
    if(-1 == sockfd_) {
      slowlog(LOG_ERROR, "failed to create socket: %s\n", strerror(errno));
      return false;
    }

    if(-1 == set_nonblocking()) {
      slowlog(LOG_ERROR, "failed to set socket %d to non-blocking \n", sockfd_);
      return false;
    }

    slowlog(LOG_DEBUG, "non-blocking socket %d created\n", sockfd_);
    if(connect_initiated_ = url->isSSL() ? connect_ssl(addr) : connect_plain(addr)) {
      break; //found right addrinfo
    }
  }


  followups_to_send_ = followups_to_send;
  requests_to_send_ = 1;

  maxfd = std::max(sockfd_, maxfd);
  return true;
}

bool SlowSocket::connect_plain(addrinfo* addr) {
  errno = 0;

  if (connect(sockfd_, addr->ai_addr, addr->ai_addrlen) < 0
      && EINPROGRESS != errno) {
    slowlog(LOG_ERROR, "cannot connect socket %d: %s\n", sockfd_, strerror(errno));
    close();
    return false;
  }
  return true;
}

bool SlowSocket::connect_ssl(addrinfo* addr) {
  // Establish regular connection.
  if(!connect_plain(addr))  return false;
   
  // Init SSL related stuff.
  // TODO(vagababov): this is not thread safe of pretty.
  static bool ssl_is_initialized = false;
  if (!ssl_is_initialized) {
    SSL_library_init();
    ssl_is_initialized = true;
  }
  SSL_METHOD* method = NULL;
  SSL_CTX* ssl_ctx = NULL;
  method = (SSL_METHOD*)SSLv23_client_method();
  ssl_ctx = SSL_CTX_new(method);
  if(!ssl_ctx) {
    slowlog(LOG_ERROR, "cannot create new SSL context\n");
    close();
    return false;
  }
  ssl_ = SSL_new(ssl_ctx);
  if(!ssl_) {
    slowlog(LOG_ERROR, "cannot create SSL structure for a connection\n");
    close();
    return false;
  }
  SSL_set_fd(ssl_, sockfd_);
  int ret = SSL_connect(ssl_);
  if(ret <= 0) {
    int err = SSL_get_error(ssl_, ret);
    //slowlog(LOG_ERROR, "socket %d: SSL connect error: %d\n", sockfd_, err);
    if(SSL_ERROR_WANT_READ != err && SSL_ERROR_WANT_WRITE != err) {
      close();
      return false;
    }
  }
  return true;
}

int SlowSocket::recv_slow(void* buf, size_t len) {
  int ret = ssl_ ? SSL_read(ssl_, buf, len)
                 : recv(sockfd_, buf, len, 0);
  if(ssl_) {
    if(ret < 0) { 
      int err = SSL_get_error(ssl_, ret);
      if(err == SSL_ERROR_WANT_WRITE) {
        requests_to_send_ = 1;
      }
    } 
    if(SSL_is_init_finished(ssl_) && (state_ == eConnecting)) {
      requests_to_send_ = 1;
    }
  } 
  return ret;
}

int SlowSocket::send_slow(const void* buf, size_t len, const SendType type) {
  int ret;
  if(ssl_) {
    if(!SSL_is_init_finished(ssl_)) {
      ret = SSL_do_handshake(ssl_);
      if(ret <= 0) {
        int err = SSL_get_error(ssl_, ret);
        if(SSL_ERROR_WANT_READ != err && SSL_ERROR_WANT_WRITE != err) {
          slowlog(LOG_ERROR, "socket %d: SSL connect error: %d\n", sockfd_, err);
          close();
          return -1;
        } else {
          if(SSL_ERROR_WANT_READ == err) {
            requests_to_send_=0;
          }
          else {
            requests_to_send_=1;
          }
          errno = EAGAIN;
          return -1;
        }
      } else { //connected and handhsake finished
        requests_to_send_=1;
      }
    } else {
      if(requests_to_send_ > 0) { //report for initial data only
        slowlog(LOG_DEBUG, "SSL connection is using %s\n", SSL_get_cipher(ssl_));
      }
    }
  }
  // VA: this is not good. create a "prepare" method.
  // initial send
  if(buf_ == 0) {
    buf_ = buf;
    offset_ = len;
  }

  ret = ssl_ ? SSL_write(ssl_, buf_, offset_)
                 : send(sockfd_, buf_, offset_, 0);

  // entire data was sent
  if(ret > 0 && ret == offset_) {
    if(eInitialSend == type) {
      --requests_to_send_;
    } else if(eFollowUpSend == type) {
      --followups_to_send_;
    }
    buf_ = 0;
    offset_ = 0;
  } else if(ret > 0 && ret < offset_) {
    buf_ = static_cast<const char*>(buf_) + ret;
    offset_ -= ret;
  }  
  return ret;
}

void SlowSocket::close() {
  if (-1 == sockfd_) return;

  slowlog(LOG_DEBUG, "closing slow, sock is %d\n", sockfd_);
  if(ssl_) {
    SSL_free(ssl_);
    ssl_ = NULL;
  }
  requests_to_send_ = 0;
  followups_to_send_ = 0;
  ::close(sockfd_);
  sockfd_ = -1;
}

void SlowSocket::set_state(SocketState state) {
  timeval t;
  gettimeofday(&t, 0);
  switch(state) {
    case eInit:
      break;
    case eConnecting:
      set_start(&t);
      break;
    case eConnected:
      set_connected(&t);
      break;
    case eError:
      break;
    case eClosed:
      set_stop(&t);
      break;
    default:
      break;
  } 
  state_ = state;
}



}  // namespace slowhttptest