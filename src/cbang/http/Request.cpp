/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2024, Cauldron Development LLC
                               All rights reserved.

         The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
       as published by the Free Software Foundation, either version 2.1 of
               the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                 Lesser General Public License for more details.

         You should have received a copy of the GNU Lesser General Public
                 License along with the C! library.  If not, see
                         <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
           You may request written permission by emailing the authors.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "Request.h"
#include "Conn.h"
#include "Cookie.h"

#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cbang/event/Event.h>
#include <cbang/event/BufferStream.h>
#include <cbang/event/JSONBufferWriter.h>
#include <cbang/openssl/SSL.h>
#include <cbang/log/Logger.h>
#include <cbang/json/JSON.h>
#include <cbang/time/Time.h>
#include <cbang/util/Regex.h>
#include <cbang/comp/CompressionFilter.h>
#include <cbang/boost/IOStreams.h>

using namespace cb::HTTP;
using namespace cb;
using namespace std;


namespace {
  class JSONWriter : public Event::JSONBufferWriter {
    typedef std::function<void (Event::Buffer &buffer)> callback_t;
    callback_t cb;

  public:
    JSONWriter(callback_t cb, unsigned indent = 0, bool compact = false) :
      Event::JSONBufferWriter(indent, compact), cb(cb) {}

    ~JSONWriter() {
      close();
      TRY_CATCH_ERROR(if (cb) cb(*this));
    }
  };


  const char *getContentEncoding(Compression compression) {
    switch (compression) {
    case Compression::COMPRESSION_ZLIB:  return "zlib";
    case Compression::COMPRESSION_GZIP:  return "gzip";
    case Compression::COMPRESSION_BZIP2: return "bzip2";
    case Compression::COMPRESSION_LZ4:   return "lz4";
    default: return 0;
    }
  }
}


#undef CBANG_LOG_PREFIX
#define CBANG_LOG_PREFIX (isIncoming() ? "REQ" : "OUT") << getID() << ':'


Request::Request(
  const SmartPointer<Conn> &connection, Method method,
  const URI &uri, const Version &version) :
  connection(connection), method(method), uri(uri), version(version),
  args(new JSON::Dict) {}


Request::~Request() {}


uint64_t Request::getID() const {
  return connection.isSet() ? connection->getID() : 0;
}


bool Request::isOk() const {
  return 200 <= responseCode && responseCode < 300 && !connError;
}


string Request::getResponseLine() const {
  return SSTR("HTTP/" << version << ' ' << (int)responseCode << ' '
              << (responseCodeLine.empty() ?
                  responseCode.getDescription() : responseCodeLine));
}


string Request::getRequestLine() const {
  return SSTR(method << ' ' << uri << " HTTP/" << version);
}


bool Request::logResponseErrors() const {
  if (connError) LOG_ERROR("Failed response: " << connError);
  else if (!isOk()) LOG_ERROR(responseCode << ": " << getInput());
  else return false;

  return true;
}


bool Request::isConnected() const {
  return hasConnection() && connection->isConnected();
}


string Request::getSessionID(const string &cookie, const string &header) const {
  return inHas(header) ? inGet(header) : findCookie(cookie);
}


const string &Request::getUser() const {
  return (session.isNull() || !session->hasUser()) ? user : session->getUser();
}


void Request::setUser(const string &user) {
  this->user = user;
  if (!session.isNull()) session->setUser(user);
}


bool Request::isIncoming() const {
  return !hasConnection() || connection->isIncoming();
}


bool Request::isSecure() const {
  return connection.isSet() && connection->getSSL().isSet();
}


SSL Request::getSSL() const {return *connection->getSSL();}


void Request::parseJSONArgs() {
  switch (method) {
  case HTTP_POST: case HTTP_PUT: case HTTP_DELETE: case HTTP_PATCH:
    getJSONMessage();
    break;

  default: break;
  }

  // Message may have been set with setJSONMessage()
  if (msg.isSet() && msg->isDict()) args->merge(*msg);
}


void Request::parseQueryArgs() {
  for (auto it: uri) args->insert(it.first, it.second);
}


const JSON::ValuePtr &Request::parseArgs() {
  parseJSONArgs();
  parseQueryArgs();
  return args;
}


SockAddr Request::getClientAddr() const {
  return connection.isSet() ? connection->getPeerAddr() : SockAddr();
}


bool Request::inHas(const string &name) const {return inputHeaders.has(name);}


string Request::inFind(const string &name) const {
  return inputHeaders.find(name);
}


string Request::inGet(const string &name) const {return inputHeaders.get(name);}


void Request::inSet(const string &name, const string &value) {
  inputHeaders.insert(name, value);
}


void Request::inRemove(const string &name) {inputHeaders.remove(name);}
bool Request::outHas(const string &name) const {return outputHeaders.has(name);}


string Request::outFind(const string &name) const {
  return outputHeaders.find(name);
}


string Request::outGet(const string &name) const {
  return outputHeaders.get(name);
}


void Request::outSet(const string &name, const string &value) {
  outputHeaders.insert(name, value);
}


void Request::outRemove(const string &name) {outputHeaders.remove(name);}


bool Request::isPersistent() const {
  bool keepAlive = inputHeaders.connectionKeepAlive();
  return (Version(1, 1) <= version || keepAlive) && !needsClose();
}


void Request::setPersistent(bool x) {
  if (version < Version(1, 1)) {
    if (x) outSet("Connection", "Keep-Alive");
    else outRemove("Connection");

  } else if (x) outRemove("Connection");
  else outSet("Connection", "close");
}


void Request::setCache(uint32_t age) {
  string now = Time().toString(Time::httpFormat);

  outSet("Date", now);

  if (age) {
    outSet("Cache-Control", "max-age=" + String(age));
    outSet("Expires", Time(Time::now() + age).toString(Time::httpFormat));

  } else {
    outSet("Cache-Control", "max-age=0, no-cache, no-store");
    outSet("Expires", now);
  }
}


bool Request::hasContentType() const {return outputHeaders.hasContentType();}
string Request::getContentType() const {return outputHeaders.getContentType();}


void Request::setContentType(const string &contentType) {
  outputHeaders.setContentType(contentType);
}


void Request::guessContentType() {
  outputHeaders.guessContentType(uri.getExtension());
}


void Request::outSetContentEncoding(Compression compression) {
  switch (compression) {
  case COMPRESSION_NONE: break;
  case COMPRESSION_ZLIB:
  case COMPRESSION_GZIP:
  case COMPRESSION_BZIP2:
  case COMPRESSION_LZ4:
    outSet("Content-Encoding", getContentEncoding(compression));
    break;
  case COMPRESSION_AUTO: THROW("Unexected compression method");
  }
}


Compression Request::getRequestedCompression() const {
  if (!inHas("Accept-Encoding")) return COMPRESSION_NONE;

  vector<string> accept;
  String::tokenize(inGet("Accept-Encoding"), accept, ", \t");

  double maxQ = 0;
  double otherQ = 0;
  set<string> named;
  Compression compression = COMPRESSION_NONE;

  for (unsigned i = 0; i < accept.size(); i++) {
    double q = 1;
    string name = String::toLower(accept[i]);

    // Check for quality value
    size_t pos = name.find_first_of(';');
    if (pos != string::npos) {
      string arg = name.substr(pos + 1);
      name = name.substr(0, pos);

      if (2 < arg.length() && arg[0] == 'q' && arg[1] == '=') {
        q = String::parseDouble(arg.substr(2));
        if (name == "*") otherQ = q;
      }
    }

    named.insert(name);

    if (maxQ < q) {
      if (name == "identity")   compression = COMPRESSION_NONE;
      else if (name == "gzip")  compression = COMPRESSION_GZIP;
      else if (name == "zlib")  compression = COMPRESSION_ZLIB;
      else if (name == "bzip2") compression = COMPRESSION_BZIP2;
      else if (name == "lz4")   compression = COMPRESSION_LZ4;
      else q = 0;
    }

    if (maxQ < q) maxQ = q;
  }

  // Currently, the only standard compression format we support is gzip, so
  // if the user specifies something like "*;q=1" and doesn't give gzip
  // an explicit quality value then we select gzip compression.
  if (maxQ < otherQ && named.find("gzip") == named.end())
    compression = COMPRESSION_GZIP;

  return compression;
}


bool Request::hasCookie(const string &name) const {
  if (!inHas("Cookie")) return false;

  vector<string> cookies;
  String::tokenize(inGet("Cookie"), cookies, "; \t\n\r");

  for (unsigned i = 0; i < cookies.size(); i++)
    if (name == cookies[i].substr(0, cookies[i].find('='))) return true;

  return false;
}


string Request::findCookie(const string &name) const {
  if (inHas("Cookie")) {
    // Return only the first matching cookie
    vector<string> cookies;
    String::tokenize(inGet("Cookie"), cookies, "; \t\n\r");

    for (unsigned i = 0; i < cookies.size(); i++) {
      size_t pos = cookies[i].find('=');

      if (name == cookies[i].substr(0, pos))
        return pos == string::npos ? string() : cookies[i].substr(pos + 1);
    }
  }

  return "";
}


string Request::getCookie(const string &name) const {
  if (!hasCookie(name)) THROW("Cookie '" << name << "' not set");
  return findCookie(name);
}


void Request::setCookie(const string &name, const string &value,
                        const string &domain, const string &path,
                        uint64_t expires, uint64_t maxAge, bool httpOnly,
                        bool secure, const string &sameSite) {
  outSet("Set-Cookie", Cookie(
           name, value, domain, path, expires, maxAge, httpOnly, secure,
           sameSite).toString());
}


string Request::getInput() const {return inputBuffer.toString();}
string Request::getOutput() const {return outputBuffer.toString();}


SmartPointer<JSON::Value> Request::getInputJSON() const {
  Event::Buffer buf = inputBuffer;
  if (!buf.getLength()) return 0;
  Event::BufferStream<> stream(buf);
  return JSON::Reader(stream).parse();
}


const SmartPointer<JSON::Value> &Request::getJSONMessage() {
  if (msg.isNull()) {
    const Headers &hdrs = inputHeaders;

    if (hdrs.hasContentType() &&
        String::startsWith(hdrs.getContentType(), "application/json"))
      msg = getInputJSON();
  }

  return msg;
}


SmartPointer<JSON::Writer> Request::getJSONWriter() {
  outputBuffer.clear();

  auto cb = [this] (Event::Buffer &buffer) {
    if (!buffer.getLength()) return;

    setContentType("application/json");
    send(buffer);
  };

  return new JSONWriter(cb, 0, !uri.has("pretty"));
}


SmartPointer<istream> Request::getInputStream() const {
  return new Event::BufferStream<>(inputBuffer);
}


SmartPointer<ostream> Request::getOutputStream(Compression compression) {
  // Auto select compression type based on Accept-Encoding
  if (compression == COMPRESSION_AUTO) compression = getRequestedCompression();
  outSetContentEncoding(compression);

  SmartPointer<ostream> target = new Event::BufferStream<>(outputBuffer);

  if (compression == Compression::COMPRESSION_NONE) return target;

  struct FilteringOStreamWithRef : public io::filtering_ostream {
    SmartPointer<ostream> ref;
    virtual ~FilteringOStreamWithRef() {reset();}
  };

  SmartPointer<FilteringOStreamWithRef> out = new FilteringOStreamWithRef;
  pushCompression(compression, *out);
  out->ref = target;
  out->push(*target);

  return out;
}


void Request::sendError(Status code) {
  if (getContentType() == "application/json") return sendJSONError(code, "");

  outSet("Content-Type", "text/html");
  outSet("Connection", "close");

  if (!code) code = HTTP_INTERNAL_SERVER_ERROR;
  string msg = String((int)code) + " " + code.getDescription();

  send(SSTR("<html><head><title>" << msg << "</title></head><body><h1>"
            << msg << "</h1></body></html>"));

  reply(code);
}


void Request::sendError(Status code, const string &msg) {
  if (getContentType() == "application/json") return sendJSONError(code, msg);

  outputBuffer.clear();
  setContentType("text/plain");
  send(msg);
  sendError(code);
}


void Request::sendError(Status code, const Exception &e) {
  if (getContentType() == "application/json") {
    auto writer = getJSONWriter();

    writer->beginDict();
    writer->beginInsert("error");
    e.write(*writer, false);
    writer->endDict();
    writer->close();

    reply(code);

  } else sendError(code, e.getMessages());
}


void Request::sendError(const Exception &e) {
  Status code = (Status::enum_t)e.getTopCode();
  if (!code) code = HTTP_INTERNAL_SERVER_ERROR;
  sendError(code, e);
}


void Request::sendError(const exception &e) {
  sendError(Status::HTTP_INTERNAL_SERVER_ERROR, e.what());
}


void Request::sendJSONError(Status code, const string &message) {
  auto writer = getJSONWriter();

  writer->beginDict();
  writer->insert("error", message);
  writer->endDict();
  writer->close();

  if (!code) code = HTTP_INTERNAL_SERVER_ERROR;
  reply(code);
}


void Request::send(const Event::Buffer &buf) {outputBuffer.add(buf);}


void Request::send(const char *data, unsigned length) {
  outputBuffer.add(data, length);
}


void Request::send(const char *s) {outputBuffer.add(s);}
void Request::send(const string &s) {outputBuffer.add(s);}
void Request::sendFile(const string &path) {outputBuffer.addFile(path);}


void Request::reply(Status::enum_t code) {
  if (replying && !isWebsocket()) THROW("Request already replying");

  responseCode = code ? code : HTTP_INTERNAL_SERVER_ERROR;
  write();
  replying = true;
}


void Request::reply(const Event::Buffer &buf) {reply(HTTP_OK, buf);}


void Request::reply(const char *data, unsigned length) {
  reply(HTTP_OK, data, length);
}


void Request::reply(Status code, const char *data, unsigned length) {
  reply(code, Event::Buffer(data, length));
}


void Request::startChunked(Status code) {
  if (outHas("Content-Length"))
    THROW("Cannot start chunked with Content-Length set");
  if (version < Version(1, 1))
    THROW("Cannot start chunked with HTTP version " << version);
  if (!mustHaveBody()) THROW("Cannot start chunked with " << method);
  if (outputBuffer.getLength())
    THROW("Cannot start chunked data in output buffer");

  outSet("Transfer-Encoding", "chunked");
  chunked = true;
  reply(code);
}


void Request::sendChunk(const Event::Buffer &buf) {
  if (!chunked) THROW("Not chunked");

  LOG_DEBUG(4, "Sending " << buf.getLength() << " byte chunk");

  // Check for final empty chunk.  Must be before add() below
  if (!buf.getLength()) chunked = false;

  if (connection.isNull()) return; // Ignore write

  Event::Buffer out;
  out.add(String::printf("%x\r\n", buf.getLength()));
  out.add(buf);
  out.add("\r\n");

  auto cb = [this] (bool success) {onWriteComplete(success);};
  connection->writeRequest(this, out, chunked || isWebsocket(), cb);
}


void Request::sendChunk(const char *data, unsigned length) {
  sendChunk(Event::Buffer(data, length));
}


SmartPointer<JSON::Writer> Request::getJSONChunkWriter() {
  auto cb = [this] (Event::Buffer &buffer) {
    if (buffer.getLength()) sendChunk(buffer);
  };

  return new JSONWriter(cb);
}


void Request::endChunked() {sendChunk(Event::Buffer(""));}


void Request::redirect(const URI &uri, Status code) {
  outSet("Location", uri);
  outSet("Content-Length", "0");
  reply(code, "", 0);
}


void Request::onResponse(Event::ConnectionError error) {
  if (error) LOG_DEBUG(4, "< " << error);
  else {
    LOG_INFO(1, "< " << getResponseLine());
    LOG_DEBUG(5, inputHeaders << '\n');
    LOG_DEBUG(6, inputBuffer.hexdump() << '\n');
  }

  setConnectionError(error);
}


void Request::onRequest() {
  LOG_INFO(1, "< " << getRequestLine());
  LOG_DEBUG(5, inputHeaders << '\n');
  LOG_DEBUG(6, inputBuffer.hexdump() << '\n');
}


bool Request::mustHaveBody() const {
  return responseCode != HTTP_NO_CONTENT && responseCode != HTTP_NOT_MODIFIED &&
    (200 <= responseCode || responseCode < 100) && method != HTTP_HEAD;
}


bool Request::mayHaveBody() const {
  return method == HTTP_POST || method == HTTP_PUT || method == HTTP_PATCH;
}


bool Request::needsClose() const {
  return inputHeaders.needsClose() || outputHeaders.needsClose();
}


Version Request::parseHTTPVersion(const string &s) {
  if (!String::startsWith(s, "HTTP/"))
    THROW("Expected 'HTTP/' got '" << s << "'");
  return Version(s.substr(5));
}


void Request::parseResponseLine(const string &line) {
  vector<string> parts;
  String::tokenize(line, parts, " ", false, 3);

  if (parts.size() < 2) THROW("Invalid HTTP response line: " << line);
  version = parseHTTPVersion(parts[0]);

  responseCode = Status::parse(parts[1]);
  if (!responseCode) THROW("Bad response code " << parts[1]);

  if (parts.size() == 3) responseCodeLine = parts[2];
}


void Request::write() {
  if (connection.isNull()) return onWriteComplete(false); // Ignore write

  Event::Buffer out;

  writeHeaders(out);
  if (outputBuffer.getLength()) out.add(outputBuffer);

  bytesWritten += out.getLength();

  auto cb = [this] (bool success) {onWriteComplete(success);};
  connection->writeRequest(this, out, chunked || isWebsocket(), cb);
}


void Request::writeResponse(Event::Buffer &buf) {
  buf.add(getResponseLine() + "\r\n");

  if (version.getMajor() == 1) {
    if (1 <= version.getMinor() && !outHas("Date"))
      outSet("Date", Time().toString("%a, %d %b %Y %H:%M:%S GMT"));

    // If the protocol is 1.0 and connection was keep-alive add keep-alive
    bool keepAlive = inputHeaders.connectionKeepAlive();
    if (!version.getMinor() && keepAlive) outSet("Connection", "keep-alive");

    if ((0 < version.getMinor() || keepAlive) && mustHaveBody() &&
        !outHas("Transfer-Encoding") && !outHas("Content-Length"))
      outSet("Content-Length", String(outputBuffer.getLength()));
  }

  // Add Content-Type
  if (mustHaveBody() && !hasContentType()) guessContentType();

  // If request asked for close, send close
  if (inputHeaders.needsClose()) outSet("Connection", "close");

  LOG_INFO(300 <= responseCode ? 1 : 4, "> " << getResponseLine());
  LOG_DEBUG(5, outputHeaders << '\n');
  LOG_DEBUG(6, outputBuffer.hexdump() << '\n');
}


void Request::writeRequest(Event::Buffer &buf) {
  // Generate request line
  buf.add(getRequestLine() + "\r\n");

  if (!outHas("Host")) outSet("Host", uri.getHost());
  if (!outHas("Connection")) outSet("Connection", "close"); // Don't persist

  // Add missing content length if may have body
  if (mayHaveBody() && !outHas("Content-Length"))
    outSet("Content-Length", String(outputBuffer.getLength()));

  LOG_INFO(1, "> " << getRequestLine());
  LOG_DEBUG(5, outputHeaders << '\n');
  LOG_DEBUG(6, outputBuffer.hexdump() << '\n');
}


void Request::writeHeaders(Event::Buffer &buf) {
  if (connection->isIncoming()) writeResponse(buf);
  else writeRequest(buf);

  for (auto it : outputHeaders) {
    const string &key   = it.first;
    const string &value = it.second;
    if (!value.empty())
      buf.add(String::printf("%s: %s\r\n", key.c_str(), value.c_str()));
  }

  buf.add("\r\n");
}
