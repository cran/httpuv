#include "httpresponse.h"
#include "httprequest.h"
#include "constants.h"
#include "thread.h"
#include "utils.h"
#include "gzipdatasource.h"
#include <uv.h>


void on_response_written(uv_write_t* handle, int status) {
  ASSERT_BACKGROUND_THREAD()
  // Make a local copy of the shared_ptr before deleting the original one.
  std::shared_ptr<HttpResponse> pResponse(*(std::shared_ptr<HttpResponse>*)handle->data);

  delete (std::shared_ptr<HttpResponse>*)handle->data;
  free(handle);

  pResponse->onResponseWritten(status);
}


ResponseHeaders& HttpResponse::headers() {
  return _headers;
}

void HttpResponse::addHeader(const std::string& name, const std::string& value) {
  _headers.push_back(std::pair<std::string, std::string>(name, value));
}

// Set a header to a particular value. If the header already exists, delete
// it, and add the header with the new value. The new header will be the last
// item.
void HttpResponse::setHeader(const std::string& name, const std::string& value) {
  // Look for existing header with same name, and delete if present
  ResponseHeaders::iterator it = _headers.begin();
  while (it != _headers.end()) {
    if (strcasecmp(it->first.c_str(), name.c_str()) == 0) {
      it = _headers.erase(it);
    } else {
      ++it;
    }
  }

  addHeader(name, value);
}

class HttpResponseExtendedWrite : public ExtendedWrite {
  std::shared_ptr<HttpResponse> _pParent;
public:
  HttpResponseExtendedWrite(std::shared_ptr<HttpResponse> pParent,
                            uv_stream_t* pHandle,
                            std::shared_ptr<DataSource> pDataSource,
                            bool chunked) :
      ExtendedWrite(pHandle, pDataSource, chunked), _pParent(pParent) {}

  void onWriteComplete(int status) {
    delete this;
  }
};

void HttpResponse::writeResponse() {
  ASSERT_BACKGROUND_THREAD()
  debug_log("HttpResponse::writeResponse", LOG_DEBUG);
  // TODO: Optimize
  std::ostringstream response(std::ios_base::binary);
  response << "HTTP/1.1 " << _statusCode << " " << _status << "\r\n";
  bool contentEncoding = false;
  std::string contentLength;
  for (ResponseHeaders::const_iterator it = _headers.begin();
     it != _headers.end();
     it++) {
    if (strcasecmp(it->first.c_str(), "Content-Length") == 0) {
      contentLength = it->second;
    } else {
      response << it->first << ": " << it->second << "\r\n";
      if (strcasecmp(it->first.c_str(), "Content-Encoding") == 0) {
        contentEncoding = true;
      }
    }
  }

  // Determine if gzip compression should be used
  bool gzip;
  if (contentEncoding) {
    // The response already has a Content-Encoding
    gzip = false;
  } else if (_statusCode == 101 || _pBody == nullptr) {
    gzip = false;
  } else {
    RequestHeaders h = _pRequest->headers();
    auto acceptEncoding = h.find("Accept-Encoding");
    if (acceptEncoding != h.end()) {
      std::string enc = acceptEncoding->second;
      if (enc.find("gzip") != std::string::npos) {
        gzip = true;
      } else {
        // There was an "Accept-Encoding", but it didn't include gzip
        gzip = false;
      }
    } else {
      // No "Accept-Encoding" header
      gzip = false;
    }
  }

  if (gzip) {
    response << "Content-Encoding: gzip\r\n";
    _chunked = true;
    _pBody = std::make_shared<GZipDataSource>(_pBody);
  }

  if (_statusCode == 101) {
    // HTTP 101 must not set this header, even if there *is* body data (which is
    // actually not a true HTTP body, but instead, just the first bytes for the
    // switched-to protocol)
  } else if (_chunked) {
    response << "Transfer-Encoding: chunked\r\n";
  } else if (!contentLength.empty()) {
    response << "Content-Length: " << contentLength << "\r\n";
  } else if (_pBody != nullptr) {
    response << "Content-Length: " << _pBody->size() << "\r\n";
  } else {
    // Some valid responses (such as HTTP 204 and 304) must not set this header,
    // since they can't have a body.
    //
    // See: https://tools.ietf.org/html/rfc7230#section-3.3.2
  }

  response << "\r\n";
  std::string responseStr = response.str();
  _responseHeader.assign(responseStr.begin(), responseStr.end());

  // For Hixie-76 and HyBi-03, it's important that the body be sent immediately,
  // before any WebSocket traffic is sent from the server
  if (_statusCode == 101 && _pBody != NULL && _pBody->size() > 0 && _pBody->size() < 256) {
    uv_buf_t buffer = _pBody->getData(_pBody->size());
    if (buffer.len > 0) {
      _responseHeader.reserve(_responseHeader.size() + buffer.len);
    }
    _responseHeader.insert(_responseHeader.end(), buffer.base, buffer.base + buffer.len);
    if (buffer.len == _pBody->size()) {
      // We used up the body, kill it
      _pBody.reset();
    }
  }

  uv_buf_t headerBuf = uv_buf_init(safe_vec_addr(_responseHeader), _responseHeader.size());
  uv_write_t* pWriteReq = (uv_write_t*)malloc(sizeof(uv_write_t));
  memset(pWriteReq, 0, sizeof(uv_write_t));
  // Pointer to shared_ptr
  pWriteReq->data = new std::shared_ptr<HttpResponse>(shared_from_this());

  int r = uv_write(pWriteReq, _pRequest->handle(), &headerBuf, 1,
      &on_response_written);
  if (r) {
    debug_log(std::string("uv_write() error:") + uv_strerror(r), LOG_INFO);
    delete (std::shared_ptr<HttpResponse>*)pWriteReq->data;
    free(pWriteReq);
  } else {
    _pRequest->requestCompleted();
  }
}

void HttpResponse::onResponseWritten(int status) {
  ASSERT_BACKGROUND_THREAD()
  debug_log("HttpResponse::onResponseWritten", LOG_DEBUG);
  if (status != 0) {
    err_printf("Error writing response: %d\n", status);
    _closeAfterWritten = true; // Cause the request connection to close.
    return;
  }

  if (_pBody != NULL) {
    HttpResponseExtendedWrite* pResponseWrite = new HttpResponseExtendedWrite(
      shared_from_this(), _pRequest->handle(), _pBody, this->_chunked);
    pResponseWrite->begin();
  }
}

// This sets a flag so that the connection is closed after the response is
// written. It also adds a "Connection: close" header to the response.
void HttpResponse::closeAfterWritten() {
  setHeader("Connection", "close");
  _closeAfterWritten = true;
}

HttpResponse::~HttpResponse() {
  ASSERT_BACKGROUND_THREAD()
  debug_log("HttpResponse::~HttpResponse", LOG_DEBUG);
  if (_closeAfterWritten) {
    _pRequest->close();
  }
  _pBody.reset();
}
