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
 * Note: You can also specify parameters:
 *	 errlogname=<full error log name>
 *	 complogname=<full compression log name>
 *
 *	 This permits to define arbitrary (instead of defaults) log files. Proxy should have permissions
 *	 to write to this directory(-ies). If file(s) exists - it will appends. It not exists - will be created.
 *
 *       The default number of workers is equal to the number of logical processor cores.
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
#include <utility>
#ifdef HAVE_SYS_TIME_H
#	include <sys/time.h>
#else
#	error Require sys/time.h to build.
#endif

namespace {

constexpr std::size_t z_compression_level = 6;
constexpr std::size_t min_compression_file_size = 512;
constexpr std::size_t max_compression_file_size = 16777216;
constexpr std::size_t zlib_overhead = 64;

const libecap::Name acceptEncodingName("Accept-Encoding");
const libecap::Name teEncodingName("TE");
const libecap::Name cacheControlName("Cache-Control");
const libecap::Name contentRangeName("Content-Range");
const libecap::Name contentEncodingName("Content-Encoding");
const libecap::Name contentTypeName("Content-Type");
const libecap::Name eTagName("ETag");
const libecap::Name contentXecapName("X-Ecap");
const libecap::Name varyName("Vary");

constexpr auto EcapGzip = "gzip";
constexpr auto EcapDeflate = "deflate";
const libecap::Header::Value XEcapDefgzipValue = libecap::Area::FromTempString(EcapGzip);
const libecap::Header::Value XEcapDefdeflateValue = libecap::Area::FromTempString(EcapDeflate);
const libecap::Header::Value varyValue = libecap::Area::FromTempString("Accept-Encoding");

const char *ECAP_SERVICE_NAME = "ecap://www.thecacheworks.com/ecap_gzip_deflate";
const char *ECAP_ERROR_LOG = "/var/log/ecap_gzip_err.log";
const char *ECAP_COMPRESSION_LOG = "/var/log/ecap_gzip_comp.log";

const char *ERR_STARTING_MSG = "eCAP gzip adapter starting";
const char *ERR_UNSUPP_PARAM = "Unsupported parameter: ";
const char *ERR_INVALID_PARAM = "Invalid zlib parameter";
const char *ERR_INSUFF_MEMORY = "Insufficient memory in zlib";
const char *ERR_VERSION_ZLIB = "zlib version mismatch";
const char *ERR_UNKNOWN = "Unknown zlib error: ";
const char *ERR_GZINIT_FAILED = "zlib initialization failed";
const char *ERR_UNKNOWN_2 = "Unknown zlib finish error: ";
const char *ERR_WORKER = "Unable to start worker thread: ";

constexpr long async_event_poll_interval_usec = 10000;
constexpr std::size_t max_worker_count = 64;

std::size_t defaultWorkerCount() {
	const unsigned int thr_count = std::thread::hardware_concurrency();
	return thr_count > 0 ? static_cast<std::size_t>(thr_count) : 1;
}

const std::array<unsigned char, 10> gzipHeader = {{ 31, 139, 8, 0, 0, 0, 0, 0, 0, 3 }};

} /* namespace */

namespace Adapter {

Service::Service(): MaxSize(0), Level(0), ErrLog(false), CompLog(false),
	ErrLogName(), CompLogName(), WorkerCount(0), Stopping(false), ActiveWork(0) {}

Service::~Service() { stop(); }

std::string Service::uri() const { return ECAP_SERVICE_NAME; }

std::string Service::tag() const { return PACKAGE_VERSION; }

void Service::describe(std::ostream &os) const { os << PACKAGE_NAME; }

bool Service::makesAsyncXactions() const { return true; }

void Service::configure(const libecap::Options &cfg) {
	Cfgtor cfgtor(*this);
	cfg.visitEachOption(cfgtor);
	if (MaxSize == 0)
		MaxSize = max_compression_file_size;
	if (Level == 0)
		Level = z_compression_level;
	if (ErrLogName.empty())
		ErrLogName = ECAP_ERROR_LOG;
	if (CompLogName.empty())
		CompLogName = ECAP_COMPRESSION_LOG;
	if (WorkerCount == 0)
		WorkerCount = defaultWorkerCount();
}

void Service::reconfigure(const libecap::Options &cfg) {
	configure(cfg);
}

void Service::setOne(const libecap::Name &name, const libecap::Area &valArea) {
	std::string value = valArea.toString();
	if (name == "maxsize") {
		const int v = std::stoi(value);
		if (v > 0) MaxSize = static_cast<std::size_t>(v);
	} else if (name == "level") {
		const int v = std::abs(std::stoi(value));
		Level = v > 9 ? z_compression_level : static_cast<std::size_t>(v);
	} else if (name == "errlog")
		ErrLog = std::abs(std::stoi(value)) > 0;
	else if (name == "errlogname") {
		if (value.empty())
			ErrLogName = ECAP_ERROR_LOG;
		else
			ErrLogName = std::move(value);
	} else if (name == "complogname") {
		if (value.empty())
			CompLogName = ECAP_COMPRESSION_LOG;
		else
			CompLogName = std::move(value);
	} else if (name == "complog")
		CompLog = std::abs(std::stoi(value)) > 0;
	else if (name == "workers") {
		const int count = std::stoi(value);
		WorkerCount = static_cast<std::size_t>(std::max(1, std::min(count, static_cast<int>(max_worker_count))));
	} else if (name == "bypassable") {
		// Squid passes this parameter; the adapter does not use it.
	} else Xaction::ErrorLog(ERR_UNSUPP_PARAM + name.image(), ErrLog, ErrLogName);
}

void Service::start() {
	{
		std::lock_guard<std::mutex> lock(WorkMutex);
		if (!Workers.empty()) return;
	}
	Stopping.store(false, std::memory_order_relaxed);

	try {
		for (std::size_t i = 0; i < WorkerCount; ++i)
			Workers.emplace_back(&Service::worker, this);
	} catch (const std::system_error &e) {
		Stopping.store(true, std::memory_order_relaxed);
		WorkCondition.notify_all();

		for (std::thread &worker : Workers)
			if (worker.joinable()) worker.join();
		Workers.clear();

		Xaction::ErrorLog(std::string(ERR_WORKER) + e.what(), ErrLog, ErrLogName);
		return;
	}

	Xaction::ErrorLog(ERR_STARTING_MSG, ErrLog, ErrLogName);
}

void Service::suspend(timeval &timeout) {
	bool active = false;
	{
		std::lock_guard<std::mutex> lock(WorkMutex);
		active = ActiveWork != 0 || !WorkQueue.empty();
	}

	if (!active) {
		std::lock_guard<std::mutex> lock(ReadyMutex);
		active = !ReadyQueue.empty();
	}

	if (active && (timeout.tv_sec > 0 || timeout.tv_usec > async_event_poll_interval_usec)) {
		timeout.tv_sec = 0;
		timeout.tv_usec = async_event_poll_interval_usec;
	}
}

void Service::resume() {
	std::deque<libecap::shared_ptr<Xaction> > readyQueue;
	{
		std::lock_guard<std::mutex> lock(ReadyMutex);
		readyQueue.swap(ReadyQueue);
	}
	for (const libecap::shared_ptr<Xaction> &x : readyQueue) {
		x->scheduler.readyScheduled.store(false, std::memory_order_relaxed);
		x->resumeHost();
	}
}

void Service::stop() {
	Stopping.store(true, std::memory_order_relaxed);
	WorkCondition.notify_all();

	for (std::thread &worker : Workers)
		if (worker.joinable()) worker.join();
	Workers.clear();

	{
		std::lock_guard<std::mutex> lock(WorkMutex);
		for (const libecap::shared_ptr<Xaction> &x : WorkQueue)
			x->scheduler.workScheduled = false;
		WorkQueue.clear();
	}

	{
		std::lock_guard<std::mutex> lock(ReadyMutex);
		for (const libecap::shared_ptr<Xaction> &x : ReadyQueue)
			x->scheduler.readyScheduled.store(false, std::memory_order_relaxed);
		ReadyQueue.clear();
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
	libecap::shared_ptr<Xaction> x(new Xaction(this, hostx));
	x->setSelf(x);
	return x;
}

void Service::enqueue(libecap::shared_ptr<Xaction> x) {
	// workScheduled is the single scheduling token for this transaction.
	// WorkMutex protects both the token and WorkQueue, so only one queue entry may own it at a time.
	{
		std::lock_guard<std::mutex> lock(WorkMutex);
		if (Stopping.load(std::memory_order_relaxed) || x->scheduler.workScheduled)
			return;
		x->scheduler.workScheduled = true;
		WorkQueue.emplace_back(std::move(x));
	}
	WorkCondition.notify_one();
}

void Service::ready(libecap::shared_ptr<Xaction> x) {
	std::lock_guard<std::mutex> lock(ReadyMutex);
	if (Stopping.load(std::memory_order_relaxed)) {
		x->scheduler.readyScheduled.store(false, std::memory_order_relaxed);
		return;
	}
	ReadyQueue.emplace_back(std::move(x));
}

void Service::worker() {
	for (;;) {
		libecap::shared_ptr<Xaction> x;
		{
			std::unique_lock<std::mutex> lock(WorkMutex);
			WorkCondition.wait(lock, [this] {
				return Stopping.load(std::memory_order_relaxed) ||
					!WorkQueue.empty();
			});
			if (Stopping.load(std::memory_order_relaxed) && WorkQueue.empty())
				return;

			x = std::move(WorkQueue.front());
			WorkQueue.pop_front();
			++ActiveWork;
		}

		const bool moreWork = x->processOne();
		bool requeued = false;
		{
			std::lock_guard<std::mutex> workLock(WorkMutex);
			if (!Stopping.load(std::memory_order_relaxed)) {
				std::lock_guard<std::mutex> queueLock(x->compression.queueMutex);
				if (!x->scheduler.stopped && !x->compression.processingFailed &&
					(moreWork || !x->compression.inputQueue.empty() ||
						(x->compression.finalPending && !x->compression.compressionFinished))) {
					// Process one input chunk per queue turn so a large transaction
					// cannot monopolize a worker while other transactions are waiting.
					WorkQueue.emplace_back(std::move(x));
					requeued = true;
				} else x->scheduler.workScheduled = false;
			} else x->scheduler.workScheduled = false;
		}

		if (requeued) WorkCondition.notify_one();

		{
			std::lock_guard<std::mutex> lock(WorkMutex);
			--ActiveWork;
		}
	}
}

void Xaction::setSelf(const libecap::shared_ptr<Xaction> &aSelf) {
	self = aSelf;
}

Xaction::Xaction(Service *aService, libecap::host::Xaction *x):
	service(aService), hostx(x), receivingVb(OpState::opUndecided), sendingAb(OpState::opUndecided), atEnd(false) {
}

Xaction::~Xaction() {
	if (compression.zstreamInitialized)
		deflateEnd(&compression.zstream);
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
		rc = deflateInit2(&compression.zstream, static_cast<int>(service->Level), Z_DEFLATED, -MAX_WBITS, 9, Z_DEFAULT_STRATEGY);
	} else {
		rc = deflateInit(&compression.zstream, static_cast<int>(service->Level));
	}
	if (rc == Z_OK) {
		compression.zstreamInitialized = true;
		return true;
	}
	if (rc == Z_STREAM_ERROR)
		ErrorLog(ERR_INVALID_PARAM, service->ErrLog, service->ErrLogName);
	else if (rc == Z_MEM_ERROR)
		ErrorLog(ERR_INSUFF_MEMORY, service->ErrLog, service->ErrLogName);
	else if (rc == Z_VERSION_ERROR)
		ErrorLog(ERR_VERSION_ZLIB, service->ErrLog, service->ErrLogName);
	else
		ErrorLog(ERR_UNKNOWN + std::to_string(rc), service->ErrLog, service->ErrLogName);
	return false;
}

void Xaction::start() {
	if (hostx->virgin().body()) {
		receivingVb = OpState::opOn;
		hostx->vbMake();
	} else
		receivingVb = OpState::opNever;

	libecap::FirstLine *firstLine = &(hostx->virgin().firstLine());
	libecap::StatusLine *statusLine = static_cast<libecap::StatusLine*>(firstLine);
	libecap::shared_ptr<libecap::Message> adapted = hostx->virgin().clone();

	const libecap::Header::Value contentType = adapted->header().value(contentTypeName);
	if (adapted->header().hasAny(contentTypeName) && contentType.size > 0)
		controlFlags.responseContentTypeOk = true;
	if (adapted->header().hasAny(contentXecapName))
		controlFlags.requestContentXecapOk = false;
	if (hostx->cause().header().hasAny(acceptEncodingName) && statusLine->statusCode() == 200) {
		const libecap::Header::Value acceptEncoding = hostx->cause().header().value(acceptEncodingName);
		if (acceptEncoding.size > 0) {
			const std::string acceptEncodingValue = acceptEncoding.toString();
			controlFlags.requestAcceptEncodingOk = true;
			if (acceptEncodingValue.find(EcapGzip) != std::string::npos)
				controlFlags.requestAcceptEncodingGzip = true;
			else if (acceptEncodingValue.find(EcapDeflate) != std::string::npos)
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
		controlFlags.requestContentLengthOk = contentLength >= min_compression_file_size && contentLength < service->MaxSize;
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
			ErrorLog(ERR_GZINIT_FAILED, service->ErrLog, service->ErrLogName);
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
	std::lock_guard<std::mutex> lock(compression.queueMutex);
	scheduler.stopped = true;
	hostx = nullptr;
	compression.inputQueue.clear();
	compression.outputQueue.clear();
}

void Xaction::resumeHost() {
	libecap::host::Xaction *x;
	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped) return;
		x = hostx;
	}
	x->resume();
}

void Xaction::resume() {
	libecap::host::Xaction *x;
	bool notifyAvailable = false;
	bool notifyDone = false;
	bool notifyAbort = false;
	bool doneAtEnd = false;

	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped) return;
		x = hostx;
		if (compression.processingFailed) {
			compression.processingFailed = false;
			compression.inputQueue.clear();
			compression.outputQueue.clear();
			compression.finalPending = false;
			compression.compressionFinished = false;
			notifyAbort = true;
		} else {
			notifyAvailable = !compression.outputQueue.empty() && sendingAb == OpState::opOn;
			notifyDone = compression.compressionFinished && compression.outputQueue.empty() && sendingAb == OpState::opOn;
			doneAtEnd = atEnd;
			if (notifyDone) {
				sendingAb = OpState::opComplete;
				receivingVb = OpState::opComplete;
			}
		}
	}

	if (notifyAbort)
		x->adaptationAborted();
	else if (notifyAvailable)
		x->noteAbContentAvailable();
	else if (notifyDone)
		x->noteAbContentDone(doneAtEnd);
}

void Xaction::abDiscard() {
	if (sendingAb != OpState::opUndecided) return;
	sendingAb = OpState::opNever;
	stopVb();
}

void Xaction::abMake() {
	if (sendingAb != OpState::opUndecided) return;
	if (!hostx->virgin().body()) return;
	sendingAb = OpState::opOn;
	bool available = false;
	bool done = false;
	bool failed = false;
	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		available = !compression.outputQueue.empty();
		done = compression.compressionFinished && compression.outputQueue.empty();
		failed = compression.processingFailed;
	}
	if (available || done || failed)
		signalReady();
}

void Xaction::abMakeMore() {
	if (receivingVb == OpState::opOn)
		hostx->vbMakeMore();
}

void Xaction::abStopMaking() {
	sendingAb = OpState::opComplete;
	stopVb();
}

libecap::Area Xaction::abContent(libecap::size_type offset, libecap::size_type size) {
	std::lock_guard<std::mutex> lock(compression.queueMutex);
	if (sendingAb != OpState::opOn || compression.outputQueue.empty())
		return libecap::Area::FromTempString("");
	OutputChunk &chunk = compression.outputQueue.front();
	if (offset >= chunk.data.size() - chunk.offset)
		return libecap::Area::FromTempString("");
	const std::size_t start = chunk.offset + offset;
	const std::size_t available = chunk.data.size() - start;
	const std::size_t returned = std::min<std::size_t>(available, size == libecap::nsize ? available : size);
	return libecap::Area::FromTempBuffer(reinterpret_cast<const char*>(chunk.data.data() + start), returned);
}

void Xaction::abContentShift(libecap::size_type size) {
	bool more = false;
	bool done = false;
	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (compression.outputQueue.empty()) return;
		OutputChunk &chunk = compression.outputQueue.front();
		chunk.offset += size;
		if (chunk.offset == chunk.data.size())
			compression.outputQueue.pop_front();
		more = !compression.outputQueue.empty();
		done = !more && compression.compressionFinished && sendingAb == OpState::opOn;
	}

	if (more) {
		hostx->noteAbContentAvailable();
	} else if (done)
		signalReady();
}

void Xaction::noteVbContentDone(bool aAtEnd) {
	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped || receivingVb != OpState::opOn) return;
		atEnd = aAtEnd;
		compression.finalPending = true;
	}
	service->enqueue(self.lock());
	signalReady();
}

void Xaction::noteVbContentAvailable() {
	if (receivingVb != OpState::opOn) return;
	const libecap::Area vb = hostx->vbContent(0, libecap::nsize);
	if (!vb.size) return;

	InputChunk input;
	input.data.assign(reinterpret_cast<const unsigned char *>(vb.start),
		reinterpret_cast<const unsigned char *>(vb.start) + vb.size);
	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped) return;
		compression.inputQueue.push_back(std::move(input));
	}
	hostx->vbContentShift(vb.size);
	service->enqueue(self.lock());
	signalReady();
}

bool Xaction::processOne() {
	InputChunk input;
	bool haveInput = false;
	bool doFinish = false;
	bool finishAtEnd = false;

	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped || compression.processingFailed) return false;

		if (!compression.inputQueue.empty()) {
			input = std::move(compression.inputQueue.front());
			compression.inputQueue.pop_front();
			haveInput = true;
		} else if (compression.finalPending && !compression.compressionFinished) {
			doFinish = true;
			finishAtEnd = atEnd;
		}
	}

	if (haveInput)
		processInput(input);
	else if (doFinish)
		finishCompression(finishAtEnd);

	if (haveInput || doFinish)
		signalReady();

	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped || compression.processingFailed) return false;
		return !compression.inputQueue.empty() || (compression.finalPending && !compression.compressionFinished);
	}
}

void Xaction::processInput(InputChunk &input) {
	if (input.data.empty()) return;

	const std::size_t inputSize = input.data.size();
	compression.originalSize += inputSize;
	if (controlFlags.requestAcceptEncodingGzip)
		compression.checksum = crc32(compression.checksum, input.data.data(), static_cast<uInt>(inputSize));

	OutputChunk output;
	const std::size_t headerSize = controlFlags.requestAcceptEncodingGzip && compression.originalSize == inputSize ? gzipHeader.size() : 0;
	const uLong bound = deflateBound(&compression.zstream, static_cast<uLong>(inputSize));
	output.data.resize(headerSize + static_cast<std::size_t>(bound) + zlib_overhead);
	if (headerSize)
		std::copy(gzipHeader.begin(), gzipHeader.end(), output.data.begin());

	compression.zstream.next_in = input.data.data();
	compression.zstream.avail_in = static_cast<uInt>(inputSize);
	compression.zstream.next_out = output.data.data() + headerSize;
	compression.zstream.avail_out = static_cast<uInt>(output.data.size() - headerSize);

	const int rc = deflate(&compression.zstream, Z_SYNC_FLUSH);
	if (rc != Z_OK || compression.zstream.avail_in != 0) {
		ErrorLog(ERR_UNKNOWN + std::to_string(rc), service->ErrLog, service->ErrLogName);
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		compression.processingFailed = true;
		compression.inputQueue.clear();
		compression.outputQueue.clear();
		return;
	}

	const std::size_t produced = output.data.size() - headerSize - compression.zstream.avail_out;
	output.data.resize(headerSize + produced);
	if (!output.data.empty()) {
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped) return;
		compression.outputQueue.push_back(std::move(output));
		compression.compressedSize += headerSize + produced;
	}
}

void Xaction::finishCompression(bool aAtEnd) {
	OutputChunk output;
	output.data.resize(zlib_overhead);
	compression.zstream.next_in = Z_NULL;
	compression.zstream.avail_in = 0;
	compression.zstream.next_out = output.data.data();
	compression.zstream.avail_out = static_cast<uInt>(output.data.size());

	const int rc = deflate(&compression.zstream, Z_FINISH);
	if (rc != Z_STREAM_END) {
		ErrorLog(ERR_UNKNOWN_2 + std::to_string(rc), service->ErrLog, service->ErrLogName);
		if (compression.zstreamInitialized) {
			deflateEnd(&compression.zstream);
			compression.zstreamInitialized = false;
		}
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		compression.finalPending = false;
		compression.processingFailed = true;
		compression.outputQueue.clear();
		return;
	}

	const std::size_t produced = output.data.size() - compression.zstream.avail_out;
	output.data.resize(produced);
	if (controlFlags.requestAcceptEncodingGzip) {
		// GZIP stores CRC32 and ISIZE as 32-bit little-endian values.
		const std::array<unsigned char, 8> trailer = {{
			static_cast<unsigned char>(compression.checksum),
			static_cast<unsigned char>(compression.checksum >> 8),
			static_cast<unsigned char>(compression.checksum >> 16),
			static_cast<unsigned char>(compression.checksum >> 24),
			static_cast<unsigned char>(compression.originalSize),
			static_cast<unsigned char>(compression.originalSize >> 8),
			static_cast<unsigned char>(compression.originalSize >> 16),
			static_cast<unsigned char>(compression.originalSize >> 24)
		}};
		output.data.insert(output.data.end(), trailer.begin(), trailer.end());
	}

	compression.compressedSize += output.data.size();
	if (compression.zstreamInitialized) {
		deflateEnd(&compression.zstream);
		compression.zstreamInitialized = false;
	}

	if (service->CompLog) {
		const double ratio = compression.originalSize > 0 ? 1.0 - static_cast<double>(compression.compressedSize) / static_cast<double>(compression.originalSize) : 0.0;
		CompressionLog(compression.originalSize, compression.compressedSize, ratio, contentTypeString, controlFlags.requestAcceptEncodingGzip ? EcapGzip : EcapDeflate);
	}

	{
		std::lock_guard<std::mutex> lock(compression.queueMutex);
		if (scheduler.stopped) return;
		if (!output.data.empty())
			compression.outputQueue.push_back(std::move(output));
		compression.finalPending = false;
		compression.compressionFinished = true;
			atEnd = aAtEnd;
	}
}

void Xaction::signalReady() {
	libecap::shared_ptr<Xaction> x = self.lock();
	if (!x) return;

	bool expected = false;
	if (!scheduler.readyScheduled.compare_exchange_strong(expected, true,
			std::memory_order_relaxed, std::memory_order_relaxed))
		return;
	service->ready(std::move(x));
}

void Xaction::stopVb() {
	if (receivingVb == OpState::opOn) {
		hostx->vbStopMaking();
		receivingVb = OpState::opComplete;
	}
}

void Xaction::ErrorLog(const std::string &p_log_entry, bool p_ErrLog, const std::string &p_log_name) {
	if (!p_ErrLog) return;
	std::ofstream file(p_log_name, std::ios_base::app | std::ios_base::out);
	if (file.is_open())
		file << p_log_entry << '\n';
}

void Xaction::CompressionLog(std::size_t p_origSize, std::size_t p_compSize, double p_compRatio,
		const std::string &p_Type, const std::string &p_compType) {
	std::ofstream file(service->CompLogName, std::ios_base::app | std::ios_base::out);
	if (file.is_open())
		file << time(nullptr) << ' ' << p_origSize << ' ' << p_compSize << ' ' << p_compRatio << ' ' << p_Type << ' ' << p_compType << '\n';
}

const bool Registered = (libecap::RegisterVersionedService(new Adapter::Service));

} /* namespace Adapter */
