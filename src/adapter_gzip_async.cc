/**
 * Was VIGOS eCAP GZIP Adapter now GZIP + DEFLATE featured by Joe Lawand and Yuri Voinov.
 * Added deflate compression and optimized for speed and should free of bugs.
 * squid.conf settings very important to have rep_mime_type instead of http_status 200.
 * We're dont want every single status 200 to go trough the adapter for speed and better control.
 *
 * Just put this in squid.conf:
 * ----------------------------
 * acl gzipmimes rep_mime_type -i "/usr/local/squid/etc/acl.gzipmimes"
 * loadable_modules /usr/local/lib/ecap_adapter_gzip.so
 * ecap_service gzip_service respmod_precache ecap://www.thecacheworks.com/ecap_gzip_deflate [maxsize=16777216] [level=6] [errlog=0] [complog=0] [workers=1-64] bypass=off
 * adaptation_access gzip_service allow gzipmimes
 *
 * Note: You can specify also parameters:
 * 	 errlogname=<full error log name>
 *	 complogname=<full compression log name>
 *
 *	 This permits to define arbitrary (instead of defaults) log files. Proxy should have permissions
 *	 to write to this directory(-ies). If file(s) exists - it will appends. It not exists - will be created.
 *
 * acl.gzipmimes contents:
 * -----------------------
 * # Note: single "/" produces error in simulators,
 * #       but works in squid's regex.
 * ^application/atom+xml
 * ^application/dash+xml
 * ^application/javascript
 * ^application/json
 * .....
 *
 * Copyright (C) 2008-2016 Constantin Rack, VIGOS AG, Germany
 * Copyright (C) 2016-2026 Joe Lawand, Yuri Voinov
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the authors, nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without
 *       specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  -----------------------------------------------------------------
 *
 * This eCAP adapter is based on the eCAP adapter sample,
 * available under the following license:
 *
 * Copyright 2008 The Measurement Factory.
 * All rights reserved.
 *
 * This Software is licensed under the terms of the eCAP library (libecap),
 * including warranty disclaimers and liability limitations.
 *
 * http://www.e-cap.org/
 *
 * For testing to force deflate or gzip compression to the clients,
 * between the browser and squid, type about:config in the URL bar (Accept the disclaimer).
 * Type encoding in the filter field underneath the URL bar.
 * Doubleclick the line network.http.accept-encoding.
 * Enter the following value instead of the default: gzip or deflate
 *
 * Thanks to Constantin Rack his first GZIP adapter and to the Measurement Factory guys for the libecap.
 */

#include "adapter_gzip_async.h"
#include <libecap/common/registry.h>
#include <libecap/common/errors.h>
#include <libecap/common/message.h>
#include <libecap/common/header.h>
#include <libecap/common/names.h>
#include <libecap/host/host.h>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <algorithm>
#include <array>
#ifdef HAVE_SYS_TIME_H
#	include <sys/time.h>
#else
#	error Require sys/time.h to build.
#endif

namespace {

constexpr std::size_t c_z_compression_level = 6;
constexpr std::size_t c_min_compression_file_size = 512;
constexpr std::size_t c_max_compression_file_size = 16777216;
constexpr std::size_t c_zlib_overhead = 64;

const libecap::Name acceptEncodingName("Accept-Encoding");
const libecap::Name teEncodingName("TE");
const libecap::Name cacheControlName("Cache-Control");
const libecap::Name contentRangeName("Content-Range");
const libecap::Name contentEncodingName("Content-Encoding");
const libecap::Name contentTypeName("Content-Type");
const libecap::Name eTagName("ETag");
const libecap::Name contentXecapName("X-Ecap");
const libecap::Name varyName("Vary");

constexpr auto c_EcapGzip = "gzip";
constexpr auto c_EcapDeflate = "deflate";
const libecap::Header::Value XEcapDefgzipValue = libecap::Area::FromTempString(c_EcapGzip);
const libecap::Header::Value XEcapDefdeflateValue = libecap::Area::FromTempString(c_EcapDeflate);
const libecap::Header::Value varyValue = libecap::Area::FromTempString("Accept-Encoding");

const char *ECAP_SERVICE_NAME = "ecap://www.thecacheworks.com/ecap_gzip_deflate";
const char *ECAP_ERROR_LOG = "/var/log/ecap_gzip_err.log";
const char *ECAP_COMPRESSION_LOG = "/var/log/ecap_gzip_comp.log";

const char *C_ERR_STARTING_MSG = "eCAP gzip adapter starting";
const char *C_ERR_UNSUPP_PARAM = "Unsupported parameter: ";
const char *C_ERR_INVALID_PARAM = "Invalid zlib parameter";
const char *C_ERR_INSUFF_MEMORY = "Insufficient memory in zlib";
const char *C_ERR_VERSION_ZLIB = "zlib version mismatch";
const char *C_ERR_UNKNOWN = "Unknown zlib error: ";
const char *C_ERR_GZINIT_FAILED = "zlib initialization failed";
const char *C_ERR_UNKNOWN_2 = "Unknown zlib finish error: ";
const char *C_ERR_WORKER = "Unable to start worker thread: ";

constexpr long c_async_event_poll_interval_usec = 10000;
constexpr std::size_t c_max_worker_count = 64;

std::size_t defaultWorkerCount() {
	const unsigned int c_thr_count = std::thread::hardware_concurrency();
	return c_thr_count > 0 ? static_cast<std::size_t>(c_thr_count) : 1;
}

std::string v_ErrLogName;
std::string v_CompLogName;

const std::array<unsigned char, 10> c_gzipHeader = {{ 31, 139, 8, 0, 0, 0, 0, 0, 0, 3 }};

} /* namespace */

namespace Adapter {

Service::Service(): v_MaxSize(0), v_Level(0), v_ErrLog(false), v_CompLog(false), m_WorkerCount(0), m_Stopping(false), m_ActiveWork(0) {}

Service::~Service() { stop(); }

std::string Service::uri() const { return ECAP_SERVICE_NAME; }

std::string Service::tag() const { return PACKAGE_VERSION; }

void Service::describe(std::ostream &os) const { os << PACKAGE_NAME; }

bool Service::makesAsyncXactions() const { return true; }

void Service::configure(const libecap::Options &cfg) {
	Cfgtor cfgtor(*this);
	cfg.visitEachOption(cfgtor);
	if (v_MaxSize == 0)
		v_MaxSize = c_max_compression_file_size;
	if (v_Level == 0)
		v_Level = c_z_compression_level;
	if (v_ErrLogName.empty())
		v_ErrLogName = ECAP_ERROR_LOG;
	if (v_CompLogName.empty())
		v_CompLogName = ECAP_COMPRESSION_LOG;
	if (v_ErrLog > 0)
		v_ErrLog = true;
	else
		v_ErrLog = false;
	if (v_CompLog > 0)
		v_CompLog = true;
	else
		v_CompLog = false;
	if (m_WorkerCount == 0)
		m_WorkerCount = defaultWorkerCount();
}

void Service::reconfigure(const libecap::Options &cfg) {
	configure(cfg);
}

void Service::setOne(const libecap::Name &name, const libecap::Area &valArea) {
	const std::string value = valArea.toString();
	if (name == "maxsize")
		v_MaxSize = (std::stoi(value) > 0 ? std::stoi(value) : v_MaxSize);
	else if (name == "level")
		v_Level = (std::abs(std::stoi(value)) > 9 ? c_z_compression_level : std::abs(std::stoi(value)));
	else if (name == "errlog")
		v_ErrLog = (std::abs(std::stoi(value)) > 0);
	else if (name == "errlogname")
		v_ErrLogName = (value.empty() ? ECAP_ERROR_LOG : value);
	else if (name == "complogname")
		v_CompLogName = (value.empty() ? ECAP_COMPRESSION_LOG : value);
	else if (name == "complog")
		v_CompLog = (std::abs(std::stoi(value)) > 0);
	else if (name == "workers") {
		const int count = std::stoi(value);
		m_WorkerCount = static_cast<std::size_t>(std::max(1, std::min(count, static_cast<int>(c_max_worker_count))));
	} else if (name == "bypassable") {
	} else
		Xaction::ErrorLog(C_ERR_UNSUPP_PARAM + name.image(), true);
}

void Service::start() {
	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		if (!m_Workers.empty()) return;
		m_Stopping = false;
		if (m_WorkerCount == 0) m_WorkerCount = defaultWorkerCount();
	}

	try {
		for (std::size_t i = 0; i < m_WorkerCount; ++i)
			m_Workers.push_back(std::thread(&Service::worker, this));
	} catch (const std::system_error &e) {
		{
			std::lock_guard<std::mutex> lock(m_WorkMutex);
			m_Stopping = true;
		}
		m_WorkCondition.notify_all();

		for (std::vector<std::thread>::iterator i = m_Workers.begin(); i != m_Workers.end(); ++i) {
			if (i->joinable()) i->join();
		}
		m_Workers.clear();

		Xaction::ErrorLog(std::string(C_ERR_WORKER) + e.what(), v_ErrLog);
		return;
	}

	Xaction::ErrorLog(C_ERR_STARTING_MSG, v_ErrLog);
}

void Service::suspend(timeval &timeout) {
	bool active = false;
	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		active = m_ActiveWork != 0 || !m_WorkQueue.empty();
	}
	if (!active) {
		std::lock_guard<std::mutex> lock(m_ReadyMutex);
		active = !m_ReadyQueue.empty();
	}
	if (active) {
		if (timeout.tv_sec > 0 || timeout.tv_usec > c_async_event_poll_interval_usec) {
			timeout.tv_sec = 0;
			timeout.tv_usec = c_async_event_poll_interval_usec;
		}
	}
}

void Service::resume() {
	std::deque<libecap::shared_ptr<Xaction> > readyQueue;
	{
		std::lock_guard<std::mutex> lock(m_ReadyMutex);
		readyQueue.swap(m_ReadyQueue);
	}
	for (std::deque<libecap::shared_ptr<Xaction> >::iterator i = readyQueue.begin(); i != readyQueue.end(); ++i)
		if (*i)	(*i)->resumeHost();
}

void Service::stop() {
	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		m_Stopping = true;
	}
	m_WorkCondition.notify_all();
	for (std::vector<std::thread>::iterator i = m_Workers.begin(); i != m_Workers.end(); ++i)
		if (i->joinable()) i->join();
	m_Workers.clear();
	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		m_WorkQueue.clear();
	}
	{
		std::lock_guard<std::mutex> lock(m_ReadyMutex);
		m_ReadyQueue.clear();
	}
}

void Service::retire() {
	stop();
}

bool Service::wantsUrl(const char *url) const {
	static_cast<void>(url);
	return true;
}

Adapter::Service::MadeXactionPointer Service::makeXaction(libecap::host::Xaction *hostx) {
	libecap::shared_ptr<Xaction> x(new Xaction(std::tr1::static_pointer_cast<Service>(self), hostx));
	x->setSelf(x);
	return x;
}

void Service::enqueue(libecap::shared_ptr<Xaction> x) {
	if (!x)	return;
	// workScheduled is the single scheduling token for this transaction.
	// Only one queue entry may own it at a time.
	bool expected = false;
	if (!x->workScheduled.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
		return;

	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		if (m_Stopping) {
			x->workScheduled.store(false, std::memory_order_release);
			return;
		}
		m_WorkQueue.push_back(x);
	}
	m_WorkCondition.notify_one();
}

bool Service::requeue(libecap::shared_ptr<Xaction> x) {
	if (!x)	return false;

	{
		std::lock_guard<std::mutex> lock(m_WorkMutex);
		if (m_Stopping)	return false;
		m_WorkQueue.push_back(x);
	}
	m_WorkCondition.notify_one();
	return true;
}

void Service::ready(libecap::shared_ptr<Xaction> x) {
	if (!x)	return;
	{
		std::lock_guard<std::mutex> lock(m_ReadyMutex);
		m_ReadyQueue.push_back(x);
	}
}

void Service::worker() {
	for (;;) {
		libecap::shared_ptr<Xaction> x;
		{
			std::unique_lock<std::mutex> lock(m_WorkMutex);
			m_WorkCondition.wait(lock, [this] { return m_Stopping || !m_WorkQueue.empty(); });
			if (m_Stopping && m_WorkQueue.empty()) return;
			x = m_WorkQueue.front();
			m_WorkQueue.pop_front();
			++m_ActiveWork;
		}
		if (x) {
			const bool moreWork = x->processOne();
			if (moreWork) {
				// Process one input chunk per queue turn so a large transaction
				// cannot monopolize a worker while other transactions are waiting.
				if (!requeue(x))
					x->workScheduled.store(false, std::memory_order_release);
			} else x->workScheduled.store(false, std::memory_order_release);
		}

		{
			std::lock_guard<std::mutex> lock(m_WorkMutex);
			if (m_ActiveWork > 0) --m_ActiveWork;
		}
	}
}

void Xaction::setSelf(const libecap::shared_ptr<Xaction> &aSelf) {
	self = aSelf;
}

Xaction::Xaction(libecap::shared_ptr<Service> aService, libecap::host::Xaction *x):
	service(aService), hostx(x), receivingVb(OpState::opUndecided), sendingAb(OpState::opUndecided),
	inputDone(false), finalPending(false), compressionFinished(false), finalSent(false), stopped(false),
	workScheduled(false), gzipMode(false), checksum(crc32(0L, Z_NULL, 0)), originalSize(0), compressedSize(0),
	zstreamInitialized(false), atEnd(false) {
	zstream.zalloc = Z_NULL;
	zstream.zfree = Z_NULL;
	zstream.opaque = Z_NULL;
	zstream.next_in = Z_NULL;
	zstream.avail_in = 0;
	zstream.next_out = Z_NULL;
	zstream.avail_out = 0;
	zstream.total_in = 0;
	zstream.total_out = 0;
	zstream.msg = Z_NULL;
	zstream.state = Z_NULL;
}

Xaction::~Xaction() {
	if (zstreamInitialized)	deflateEnd(&zstream);
	if (libecap::host::Xaction *x = hostx) {
		hostx = nullptr;
		x->adaptationAborted();
	}
}

const libecap::Area Xaction::option(const libecap::Name&) const {
	return libecap::Area();
}

void Xaction::visitEachOption(libecap::NamedValueVisitor&) const {
}

bool Xaction::gzipInitialize() {
	int rc;
	if (controlFlags.requestAcceptEncodingGzip) {
		gzipMode = true;
		rc = deflateInit2(&zstream, static_cast<int>(service->v_Level), Z_DEFLATED, -MAX_WBITS, 9, Z_DEFAULT_STRATEGY);
	} else {
		gzipMode = false;
		rc = deflateInit(&zstream, static_cast<int>(service->v_Level));
	}
	if (rc == Z_OK) {
		zstreamInitialized = true;
		return true;
	}
	if (rc == Z_STREAM_ERROR)
		ErrorLog(C_ERR_INVALID_PARAM, service->v_ErrLog);
	else if (rc == Z_MEM_ERROR)
		ErrorLog(C_ERR_INSUFF_MEMORY, service->v_ErrLog);
	else if (rc == Z_VERSION_ERROR)
		ErrorLog(C_ERR_VERSION_ZLIB, service->v_ErrLog);
	else
		ErrorLog(C_ERR_UNKNOWN + std::to_string(rc), service->v_ErrLog);
	return false;
}

void Xaction::start() {
	if (!hostx) return;
	if (hostx->virgin().body()) {
		receivingVb = OpState::opOn;
		hostx->vbMake();
	} else receivingVb = OpState::opNever;

	libecap::FirstLine *firstLine = &(hostx->virgin().firstLine());
	libecap::StatusLine *statusLine = static_cast<libecap::StatusLine*>(firstLine);
	libecap::shared_ptr<libecap::Message> adapted = hostx->virgin().clone();
	if (!adapted) return;

	const libecap::Header::Value contentType = adapted->header().value(contentTypeName);
	if (adapted->header().hasAny(contentTypeName) && contentType.size > 0)
		controlFlags.responseContentTypeOk = true;
	if (adapted->header().hasAny(contentXecapName))
		controlFlags.requestContentXecapOk = false;
	if (hostx->cause().header().hasAny(acceptEncodingName) && statusLine->statusCode() == 200) {
		const libecap::Header::Value acceptEncoding = hostx->cause().header().value(acceptEncodingName);
		if (acceptEncoding.size > 0) {
			controlFlags.requestAcceptEncodingOk = true;
			if (acceptEncoding.toString().find(c_EcapGzip) != std::string::npos)
				controlFlags.requestAcceptEncodingGzip = true;
			else if (acceptEncoding.toString().find(c_EcapDeflate) != std::string::npos)
				controlFlags.requestAcceptEncodingDeflate = true;
		}
	}
	if (hostx->cause().header().hasAny(teEncodingName) || adapted->header().hasAny(libecap::headerTransferEncoding) ||
			adapted->header().hasAny(contentRangeName) || adapted->header().hasAny(contentEncodingName) ||
			(adapted->header().hasAny(cacheControlName) && adapted->header().value(cacheControlName).size > 0 &&
			adapted->header().value(cacheControlName).toString().find("no-transform") != std::string::npos))
		controlFlags.responseReject = false;

	contentTypeString = contentType.toString();
	if (adapted->header().hasAny(libecap::headerContentLength)) {
		const std::size_t contentLength = std::stoi(adapted->header().value(libecap::headerContentLength).toString());
		controlFlags.requestContentLengthOk = contentLength >= c_min_compression_file_size && contentLength < service->v_MaxSize;
	}

	adapted->header().removeAny(libecap::headerContentLength);
	adapted->header().removeAny(eTagName);
	if (controlFlags.requestAcceptEncodingGzip) {
		adapted->header().add(contentXecapName, XEcapDefgzipValue);
		adapted->header().add(contentEncodingName, XEcapDefgzipValue);
	} else if (controlFlags.requestAcceptEncodingDeflate) {
		adapted->header().add(contentXecapName, XEcapDefdeflateValue);
		adapted->header().add(contentEncodingName, XEcapDefdeflateValue);
	} else {
		controlFlags.requestAcceptEncodingGzip = false;
		controlFlags.requestAcceptEncodingDeflate = false;
		controlFlags.requestAcceptEncodingOk = false;
	}
	adapted->header().add(varyName, varyValue);

	if (!adapted->body()) {
		sendingAb = OpState::opNever;
		hostx->useAdapted(adapted);
		return;
	}

	if (requirementsAreMet()) {
		if (gzipInitialize())
			hostx->useAdapted(adapted);
		else {
			ErrorLog(C_ERR_GZINIT_FAILED, service->v_ErrLog);
			hostx->useVirgin();
			if (receivingVb == OpState::opOn)
				receivingVb = OpState::opComplete;
			abDiscard();
		}
	} else {
		hostx->useVirgin();
		if (receivingVb == OpState::opOn)
			receivingVb = OpState::opComplete;
		abDiscard();
	}
}

void Xaction::stop() {
	std::lock_guard<std::mutex> lock(queueMutex);
	stopped = true;
	hostx = nullptr;
	inputQueue.clear();
	outputQueue.clear();
	workScheduled.store(false, std::memory_order_release);
}

void Xaction::resumeHost() {
	libecap::host::Xaction *x = hostx;
	if (x) x->resume();
}

void Xaction::resume() {
	libecap::host::Xaction *x = hostx;
	if (!x)	return;

	bool notifyAvailable = false;
	bool notifyDone = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (stopped) return;
		notifyAvailable = !outputQueue.empty() && sendingAb == OpState::opOn;
		notifyDone = compressionFinished && outputQueue.empty() && !finalSent && sendingAb == OpState::opOn;
		if (notifyDone)	finalSent = true;
	}

	if (notifyAvailable) x->noteAbContentAvailable();
	if (notifyDone) {
		x->noteAbContentDone(atEnd);
		sendingAb = OpState::opComplete;
		receivingVb = OpState::opComplete;
	}
}

void Xaction::abDiscard() {
	if (sendingAb != OpState::opUndecided) return;
	sendingAb = OpState::opNever;
	stopVb();
}

void Xaction::abMake() {
	if (sendingAb != OpState::opUndecided || !hostx) return;
	if (!hostx->virgin().body()) return;
	sendingAb = OpState::opOn;
	bool available = false;
	bool done = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		available = !outputQueue.empty();
		done = compressionFinished && outputQueue.empty() && !finalSent;
	}
	if (available || done) signalReady();
}

void Xaction::abMakeMore() {
	if (receivingVb == OpState::opOn && hostx) hostx->vbMakeMore();
}

void Xaction::abStopMaking() {
	sendingAb = OpState::opComplete;
	stopVb();
}

libecap::Area Xaction::abContent(libecap::size_type offset, libecap::size_type size) {
	static_cast<void>(size);
	std::lock_guard<std::mutex> lock(queueMutex);
	if (sendingAb != OpState::opOn || outputQueue.empty())
		return libecap::Area::FromTempString("");
	OutputChunk &chunk = outputQueue.front();
	if (offset >= chunk.data.size() - chunk.offset)
		return libecap::Area::FromTempString("");
	const std::size_t start = chunk.offset + offset;
	const std::size_t available = chunk.data.size() - start;
	const std::size_t returned = std::min<std::size_t>(available, size == libecap::nsize ? available : size);
	return libecap::Area::FromTempBuffer(reinterpret_cast<const char*>(chunk.data.data() + start), returned);
}

void Xaction::abContentShift(libecap::size_type size) {
	bool more = false;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (outputQueue.empty()) return;
		OutputChunk &chunk = outputQueue.front();
		const std::size_t available = chunk.data.size() - chunk.offset;
		if (size > available) size = available;
		chunk.offset += size;
		if (chunk.offset == chunk.data.size())
			outputQueue.pop_front();
		more = !outputQueue.empty();
	}
	if (more && hostx) hostx->noteAbContentAvailable();
}

void Xaction::noteVbContentDone(bool aAtEnd) {
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (stopped || receivingVb != OpState::opOn) return;
		inputDone = true;
		atEnd = aAtEnd;
		finalPending = true;
	}
	service->enqueue(self.lock());
	signalReady();
}

void Xaction::noteVbContentAvailable() {
	if (!hostx) return;
	if (receivingVb != OpState::opOn) return;
	const libecap::Area vb = hostx->vbContent(0, libecap::nsize);
	if (!vb.size) return;

	InputChunk input;
	input.data.assign(reinterpret_cast<const unsigned char *>(vb.start),
		reinterpret_cast<const unsigned char *>(vb.start) + vb.size);
	hostx->vbContentShift(vb.size);
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (stopped) return;
		inputQueue.push_back(std::move(input));
	}
	service->enqueue(self.lock());
	signalReady();
}

bool Xaction::processOne() {
	InputChunk input;
	bool haveInput = false;
	bool doFinish = false;

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (stopped) return false;

		if (!inputQueue.empty()) {
			input = std::move(inputQueue.front());
			inputQueue.pop_front();
			haveInput = true;
		} else if (finalPending && !finalSent) doFinish = true;
	}

	if (haveInput) {
		processInput(input);
		signalReady();
	} else if (doFinish) {
		finishCompression(atEnd);
		signalReady();
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (stopped) return false;

		return !inputQueue.empty() || (finalPending && !finalSent);
	}
}

void Xaction::processInput(InputChunk &input) {
	if (input.data.empty()) return;
	originalSize += input.data.size();
	if (gzipMode) checksum = crc32(checksum, input.data.data(), static_cast<uInt>(input.data.size()));

	OutputChunk output;
	const std::size_t headerSize = gzipMode && originalSize == input.data.size() ? c_gzipHeader.size() : 0;
	output.data.resize(headerSize + input.data.size() + input.data.size() / 100 + c_zlib_overhead);
	if (headerSize)	std::copy(c_gzipHeader.begin(), c_gzipHeader.end(), output.data.begin());

	zstream.next_in = input.data.data();
	zstream.avail_in = static_cast<uInt>(input.data.size());
	zstream.next_out = output.data.data() + headerSize;
	zstream.avail_out = static_cast<uInt>(output.data.size() - headerSize);
	zstream.total_out = 0;

	int rc = deflate(&zstream, Z_SYNC_FLUSH);
	if (rc != Z_OK) {
		ErrorLog(C_ERR_UNKNOWN + std::to_string(rc), service->v_ErrLog);
		return;
	}
	output.data.resize(headerSize + zstream.total_out);
	if (!output.data.empty()) {
		std::lock_guard<std::mutex> lock(queueMutex);
		outputQueue.push_back(std::move(output));
		compressedSize += zstream.total_out + headerSize;
	}
}

void Xaction::finishCompression(bool aAtEnd) {
	OutputChunk output;
	output.data.resize(c_zlib_overhead + 32);
	zstream.next_in = Z_NULL;
	zstream.avail_in = 0;
	zstream.next_out = output.data.data();
	zstream.avail_out = static_cast<uInt>(output.data.size());
	zstream.total_out = 0;

	int rc = deflate(&zstream, Z_FINISH);
	if (rc != Z_STREAM_END) {
		ErrorLog(C_ERR_UNKNOWN_2 + std::to_string(rc), service->v_ErrLog);
		if (zstreamInitialized) {
			deflateEnd(&zstream);
			zstreamInitialized = false;
		}
		std::lock_guard<std::mutex> lock(queueMutex);
		finalPending = false;
		compressionFinished = true;
		finalSent = false;
		return;
	}

	output.data.resize(zstream.total_out);
	if (gzipMode) {
		const std::size_t oldSize = output.data.size();
		output.data.resize(oldSize + 8);
		output.data[oldSize + 0] = static_cast<unsigned char>(checksum & 0xff);
		output.data[oldSize + 1] = static_cast<unsigned char>((checksum >> 8) & 0xff);
		output.data[oldSize + 2] = static_cast<unsigned char>((checksum >> 16) & 0xff);
		output.data[oldSize + 3] = static_cast<unsigned char>((checksum >> 24) & 0xff);
		output.data[oldSize + 4] = static_cast<unsigned char>(originalSize & 0xff);
		output.data[oldSize + 5] = static_cast<unsigned char>((originalSize >> 8) & 0xff);
		output.data[oldSize + 6] = static_cast<unsigned char>((originalSize >> 16) & 0xff);
		output.data[oldSize + 7] = static_cast<unsigned char>((originalSize >> 24) & 0xff);
	}
	compressedSize += output.data.size();
	if (zstreamInitialized) {
		deflateEnd(&zstream);
		zstreamInitialized = false;
	}

	if (service->v_CompLog) {
		const double ratio = originalSize > 0 ? 1.0 - static_cast<double>(compressedSize) / static_cast<double>(originalSize) : 0.0;
		CompressionLog(originalSize, compressedSize, ratio, contentTypeString, gzipMode ? c_EcapGzip : c_EcapDeflate);
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (!output.data.empty()) outputQueue.push_back(std::move(output));
		finalPending = false;
		compressionFinished = true;
		finalSent = false;
		atEnd = aAtEnd;
	}
}

void Xaction::signalReady() {
	libecap::shared_ptr<Xaction> x = self.lock();
	if (x) service->ready(x);
}

void Xaction::stopVb() {
	if (receivingVb == OpState::opOn) {
		if (hostx) hostx->vbStopMaking();
		receivingVb = OpState::opComplete;
	}
}

libecap::host::Xaction *Xaction::lastHostCall() {
	libecap::host::Xaction *x = hostx;
	hostx = nullptr;
	return x;
}

void Xaction::ErrorLog(const std::string &p_log_entry, bool p_ErrLog) {
	if (p_ErrLog) {
		std::ofstream v_file(v_ErrLogName, std::ios_base::app | std::ios_base::out);
		if (v_file.is_open())
			v_file << p_log_entry << '\n';
	}
}

void Xaction::CompressionLog(std::size_t p_origSize, std::size_t p_compSize, double p_compRatio,
		const std::string &p_Type, const std::string &p_compType) {
	std::ofstream v_file(v_CompLogName, std::ios_base::app | std::ios_base::out);
	if (v_file.is_open())
		v_file << time(nullptr) << ' ' << p_origSize << ' ' << p_compSize << ' ' << p_compRatio << ' ' << p_Type << ' ' << p_compType << '\n';
}

const bool Registered = (libecap::RegisterVersionedService(new Adapter::Service));

} /* namespace Adapter */
