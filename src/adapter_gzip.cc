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
 * ecap_service gzip_service respmod_precache ecap://www.thecacheworks.com/ecap_gzip_deflate [maxsize=16777216] [level=6] [errlog=0] [complog=0] bypass=off
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
 * Copyright (C) 2016-2020 Joe Lawand, Yuri Voinov
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

#include "adapter_gzip.h"
#include <cstddef>	/* For std::size_t */
#include <ctime>
#include <string>	/* For std::to_string */
#include <iostream>
#include <fstream>
#include <memory>	/* For std::shared_ptr */
#include <vector>	/* For compresscontext Buffer */
#include <zlib.h>
#include <libecap/common/registry.h>
#include <libecap/common/errors.h>
#include <libecap/common/message.h>
#include <libecap/common/header.h>
#include <libecap/common/names.h>
#include <libecap/adapter/service.h>
#include <libecap/adapter/xaction.h>
#include <libecap/host/xaction.h>
#include <libecap/common/named_values.h>
#include <libecap/host/host.h>

namespace {

/* Set default compression level */
constexpr std::size_t c_z_compression_level = 6;
/* Limit min/max compression file size */
constexpr std::size_t c_min_compression_file_size = 512;
constexpr std::size_t c_max_compression_file_size = 16777216;
/* Checks the request's Accept-Encoding header if the client does understand gzip compression at all */
const libecap::Name acceptEncodingName("Accept-Encoding");
/* The header has TE encoding so don't compress and exit */
const libecap::Name teEncodingName("TE");
/* Checks if the response Cache-Control header allows transformation of the response */
const libecap::Name cacheControlName("Cache-Control");
/* Do not compress if response has a content-range header and status code "206 Partial content" */
const libecap::Name contentRangeName("Content-Range");
/* Checks the Content-Encoding response header. If this header is present, we must not compress the respone */
const libecap::Name contentEncodingName("Content-Encoding");
/* Checks the Content-Type response header. At this time, only responses are allowed to be compressed */
const libecap::Name contentTypeName("Content-Type");
/* Remove ETag response header */
const libecap::Name eTagName("ETag");
/* Add a custom header */
const libecap::Name contentXecapName("X-Ecap");
/* Add Vary "Accept-Encoding" response header */
const libecap::Name varyName("Vary");
/* Add new header value gzip or deflate */
constexpr auto c_EcapGzip = "gzip";
constexpr auto c_EcapDeflate = "deflate";
const libecap::Header::Value XEcapDefgzipValue = libecap::Area::FromTempString(c_EcapGzip);
const libecap::Header::Value XEcapDefdeflateValue = libecap::Area::FromTempString(c_EcapDeflate);

const libecap::Header::Value varyValue = libecap::Area::FromTempString("Accept-Encoding");

constexpr auto c_sp = " ";
constexpr auto c_cr = "\n";
constexpr auto c_emptyString("");

/* Arbitrary log names (if specified in config) */
std::string v_ErrLogName;
std::string v_CompLogName;

void ErrorLog(const std::string &p_log_entry, bool p_ErrLog = false) {
	if (p_ErrLog) {
		std::ofstream v_file(v_ErrLogName, std::ios_base::app|std::ios_base::out);
		if (v_file.is_open()) {
			v_file << p_log_entry << c_cr;
			v_file.close();
		}
	}
}

void CompressionLog(std::size_t p_origSize, std::size_t p_compSize, double p_compRatio, std::string &p_Type) {
	std::ofstream v_file(v_CompLogName, std::ios_base::app|std::ios_base::out);
	if (v_file.is_open()) {
		v_file << time(nullptr) << c_sp << p_origSize << c_sp << p_compSize << c_sp << p_compRatio << c_sp << p_Type.c_str() << c_cr;
		v_file.close();
	}
}

}	/* namespace */

namespace Adapter { /* Non-required, but adds clarity */

using libecap::size_type;

class Service: public libecap::adapter::Service {
	public:
		/* About */
		virtual std::string uri() const;	/* Unique across all vendors */
		virtual std::string tag() const;	/* Changes with version and config */
		virtual void describe(std::ostream &os) const;	/* Free-format info */

		/* Configuration */
		virtual void configure(const libecap::Options &cfg);
		virtual void reconfigure(const libecap::Options &cfg);
		void setOne(const libecap::Name &name, const libecap::Area &valArea);

		/* Lifecycle */
		virtual void start();	/* Expect makeXaction() calls */
		virtual void stop();	/* No more makeXaction() calls until start() */
		virtual void retire();	/* No more makeXaction() calls */

		/* Scope (XXX: this may be changed to look at the whole header) */
		virtual bool wantsUrl(const char *url) const;

		/* Work */
		virtual Adapter::Service::MadeXactionPointer makeXaction(libecap::host::Xaction *hostx);

		/* Configuration storage */
		std::size_t v_MaxSize;
		std::size_t v_Level;
		bool v_ErrLog;
		bool v_CompLog;
};

/**
 * Calls Service::setOne() for each host-provided configuration option.
 * See Service::configure().
 */
class Cfgtor: public libecap::NamedValueVisitor {
	public:
		Cfgtor(Service &aSvc): svc(aSvc) {}
		virtual void visit(const libecap::Name &name, const libecap::Area &value) {
			svc.setOne(name, value);
		}
		Service &svc;
};

class Xaction: public libecap::adapter::Xaction {
	public:
		Xaction(libecap::shared_ptr<Service> s, libecap::host::Xaction *x);
		virtual ~Xaction();

		/* Meta-information for the host transaction */
		virtual const libecap::Area option(const libecap::Name &name) const;
		virtual void visitEachOption(libecap::NamedValueVisitor &visitor) const;

		/* Lifecycle */
		virtual void start();
		virtual void stop();

		/* Adapted body transmission control */
		virtual void abDiscard();
		virtual void abMake();
		virtual void abMakeMore();
		virtual void abStopMaking();

		/* Adapted body content extraction and consumption */
		virtual libecap::Area abContent(size_type offset, size_type size);
		virtual void abContentShift(size_type size);

		/* Virgin body state notification */
		virtual void noteVbContentDone(bool atEnd);
		virtual void noteVbContentAvailable();

		std::string contentTypeString;
	protected:
		void stopVb();	/* Stops receiving vb (if we are receiving it) */
		libecap::host::Xaction *lastHostCall();		/* Clears hostx */
	private:
		libecap::shared_ptr<const Service> service;	/* Configuration access */
		libecap::host::Xaction *hostx;			/* Host transaction rep */

		enum class OpState { opUndecided, opOn, opComplete, opNever };
		OpState receivingVb;
		OpState sendingAb;

		struct CompressContext {
			z_stream zstream;
			std::vector<unsigned char> Buffer;
			unsigned long checksum;
			std::size_t originalSize;
			std::size_t compressedSize;
			std::size_t sendingOffset;
			std::size_t lastChunkSize;

			CompressContext() : checksum(crc32(0L, Z_NULL, 0)), originalSize(0), compressedSize(0), sendingOffset(0), lastChunkSize(0) {
				zstream.zalloc = Z_NULL;
				zstream.zfree = Z_NULL;
				zstream.opaque = Z_NULL;
			}
		};

		CompressContext compresscontext;

		struct Controls {
			bool responseReject;
			bool responseContentTypeOk;
			bool requestAcceptEncodingOk;
			bool requestContentLenghtOk;
			bool requestContentXecapOk;
			bool requestAcceptEncodingGzip;
			bool requestAcceptEncodingDeflate;
		};

		Controls controlFlags;

		bool requirementsAreMet();
		bool gzipInitialize();
};

}				/* namespace Adapter */

/**
 * Determines if the response can be compressed or not
 */
inline bool Adapter::Xaction::requirementsAreMet() {
	if (!controlFlags.responseReject ||
		!controlFlags.responseContentTypeOk ||
		!controlFlags.requestAcceptEncodingOk ||
		!controlFlags.requestContentXecapOk ||
		!controlFlags.requestContentLenghtOk)
		return false;
	else return true;
}

std::string Adapter::Service::uri() const {
	ErrorLog(C_ERR_STARTING_MSG, v_ErrLog);
	return ECAP_SERVICE_NAME;
}

std::string Adapter::Service::tag() const {
	return PACKAGE_VERSION;
}

void Adapter::Service::describe(std::ostream &os) const {
	os << PACKAGE_NAME;
}

void Adapter::Service::configure(const libecap::Options &cfg) {
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
		v_ErrLog  = true;
	else
		v_ErrLog  = false;

	if (v_CompLog > 0)
		v_CompLog = true;
	else
		v_CompLog = false;
}

void Adapter::Service::reconfigure(const libecap::Options &cfg) {
	v_MaxSize = c_max_compression_file_size;
	v_Level = c_z_compression_level;
	v_ErrLogName = ECAP_ERROR_LOG;
	v_CompLogName = ECAP_COMPRESSION_LOG;
	v_ErrLog = false;
	v_CompLog = false;
}

void Adapter::Service::setOne(const libecap::Name &name, const libecap::Area &valArea) {
	const std::string value = valArea.toString();

	if (name == "maxsize")
		v_MaxSize = (atoi(value.c_str()) > 0 ? atoi(value.c_str()) : v_MaxSize);
	else if (name == "level")
		v_Level = (abs(atoi(value.c_str())) > 9 ? c_z_compression_level : abs(atoi(value.c_str())));
	else if (name == "errlog")
		v_ErrLog = (abs(atoi(value.c_str())) > 0 ? true : false);
	else if (name == "errlogname")
		v_ErrLogName = (value.empty() ? ECAP_ERROR_LOG : value.c_str());
	else if (name == "complogname")
		v_CompLogName = (value.empty() ? ECAP_COMPRESSION_LOG : value.c_str());
	else if (name == "complog")
		v_CompLog = (abs(atoi(value.c_str())) > 0 ? true : false);
	else if (name == "bypassable") {	/* Do nothing. This parameter comes from squid and can be safely ignore */
	} else ErrorLog(C_ERR_UNSUPP_PARAM + name.image(), true);
}

/**
 * Initializes the zlib data structures
 */
bool Adapter::Xaction::gzipInitialize() {
	auto rc = 0;
	if (controlFlags.requestAcceptEncodingGzip)		/* Init zlib's gzip zstream */
		rc = deflateInit2(&compresscontext.zstream, service->v_Level, Z_DEFLATED, -MAX_WBITS , 9, Z_DEFAULT_STRATEGY);
	else if (controlFlags.requestAcceptEncodingDeflate)	/* Init zlib's deflate zstream */
		rc = deflateInit(&compresscontext.zstream, service->v_Level);

	switch(rc) {
		case Z_OK:
			return true;
		case Z_STREAM_ERROR:
			ErrorLog(C_ERR_INVALID_PARAM, service->v_ErrLog);
			return false;
		case Z_MEM_ERROR:
			ErrorLog(C_ERR_INSUFF_MEMORY, service->v_ErrLog);
			return false;
		case Z_VERSION_ERROR:
			ErrorLog(C_ERR_VERSION_ZLIB, service->v_ErrLog);
			return false;
		default:
			ErrorLog(C_ERR_UNKNOWN + std::to_string(rc), service->v_ErrLog);
			return false;
	}
}

void Adapter::Service::start() {
	/* Custom code would go here, but this service does not have one */
}

void Adapter::Service::stop() {
	/* Custom code would go here, but this service does not have one */
	libecap::adapter::Service::stop();
}

void Adapter::Service::retire() {
	/* Custom code would go here, but this service does not have one */
	libecap::adapter::Service::stop();
}

bool Adapter::Service::wantsUrl(const char *url) const {
	return true;			/* No-op is applied to all messages */
}

Adapter::Service::MadeXactionPointer Adapter::Service::makeXaction(libecap::host::Xaction *hostx) {
	return Service::MadeXactionPointer(new Adapter::Xaction(std::tr1::static_pointer_cast<Service>(self), hostx));
}

Adapter::Xaction::Xaction(libecap::shared_ptr<Service> aService,
	libecap::host::Xaction *x):
	service(aService),
	hostx(x),
	receivingVb(OpState::opUndecided), sendingAb(OpState::opUndecided) {
}

Adapter::Xaction::~Xaction() {
	if (libecap::host::Xaction *x = hostx) {
		hostx = nullptr;
		x->adaptationAborted();
	}
}

const libecap::Area Adapter::Xaction::option(const libecap::Name&) const {
	return libecap::Area();		/* This transaction has no meta-information */
}

void Adapter::Xaction::visitEachOption(libecap::NamedValueVisitor&) const {
	/* This transaction has no meta-information to pass to the visitor */
}

void Adapter::Xaction::start() {
	Must(hostx);

	if (hostx->virgin().body()) {
		receivingVb = OpState::opOn;
		hostx->vbMake();	/* Ask host to supply virgin body */
    	} else /* We are not interested in vb if there is not one */
		receivingVb = OpState::opNever;

	/* Reading http status */
	libecap::FirstLine *firstLine = &(hostx->virgin().firstLine());
	libecap::StatusLine *statusLine = nullptr;
	statusLine = dynamic_cast<libecap::StatusLine*>(firstLine);
	/* Adapt message header */
	libecap::shared_ptr<libecap::Message> adapted = hostx->virgin().clone();
	Must(adapted);

	const libecap::Header::Value contentType = adapted->header().value(contentTypeName);
	if (adapted->header().hasAny(contentTypeName) && contentType.size > 0)
		controlFlags.responseContentTypeOk = true;

	/* Checks the X-Ecap response header. If this header is present, we must not compress the respone */
	if (adapted->header().hasAny(contentXecapName))
		controlFlags.requestContentXecapOk = false;

	/* Condition has to have acceptEncoding and the http_status=200; */
	/* we're only do the 200 status and c_min_compression_file_size byte size */
	if (hostx->cause().header().hasAny(acceptEncodingName) && statusLine->statusCode() == 200) {
        	const libecap::Header::Value acceptEncoding = hostx->cause().header().value(acceptEncodingName);
		/* Return correct call gzip or deflate */
	        if (acceptEncoding.size > 0) {
			controlFlags.requestAcceptEncodingOk = true;
			if (acceptEncoding.toString().find(c_EcapGzip) != std::string::npos)
				controlFlags.requestAcceptEncodingGzip = true;
		        else if (acceptEncoding.toString().find(c_EcapDeflate) != std::string::npos)
				controlFlags.requestAcceptEncodingDeflate = true;
		}
	}
	/* Trying to have all the requirements on one control, so it does not go trough all if one fail */
	if (hostx->cause().header().hasAny(teEncodingName) ||
		adapted->header().hasAny(libecap::headerTransferEncoding) ||
		adapted->header().hasAny(contentRangeName) ||
		adapted->header().hasAny(contentEncodingName) ||
		((adapted->header().hasAny(cacheControlName) && adapted->header().value(cacheControlName).size > 0) &&
		adapted->header().value(cacheControlName).toString().find("no-transform") != std::string::npos))
			controlFlags.responseReject = false;

	contentTypeString = contentType.toString();
	if (adapted->header().hasAny(libecap::headerContentLength)) {
		const std::size_t v_ContentLength = atoi(adapted->header().value(libecap::headerContentLength).toString().c_str());
		if (v_ContentLength >= c_min_compression_file_size && v_ContentLength < service->v_MaxSize)
			controlFlags.requestContentLenghtOk = true;
		else
			controlFlags.requestContentLenghtOk = false;
	}
	/* Add extra response header if Content-Type is OK delete ContentLength header; */
	/* unknown length may have performance implications for the host */
	adapted->header().removeAny(libecap::headerContentLength);
	/* Remove eTag from headers */
	adapted->header().removeAny(eTagName);
	/* Add a custom X-Ecap Header + value */
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

	/* Add vary: Accept-Encoding */
	adapted->header().add(varyName, varyValue);

	if (!adapted->body()) {
		sendingAb = OpState::opNever;	/* There is nothing to send */
		lastHostCall()->useAdapted(adapted);
	} else {
		/* If all the requirements are met, then compress the obj else send Virgin headers */
		if (requirementsAreMet()) {
			if (gzipInitialize())
				hostx->useAdapted(adapted);
			else {
				ErrorLog(C_ERR_GZINIT_FAILED, service->v_ErrLog);
				hostx->useVirgin();
				abDiscard();
			}
		} else {
			hostx->useVirgin();
			abDiscard();
		}
	}
}

void Adapter::Xaction::stop() {
	hostx = nullptr;			/* The caller will delete */
}

void Adapter::Xaction::abDiscard() {
	Must(sendingAb == OpState::opUndecided);/* Have not started yet */
	sendingAb = OpState::opNever;
	/* We do not need more vb if the host is not interested in ab */
	stopVb();
}

void Adapter::Xaction::abMake() {
	Must(sendingAb == OpState::opUndecided);/* Have not yet started or decided not to send */
	Must(hostx->virgin().body());		/* That is our only source of ab content */

	/* We are or were receiving vb */
	Must(receivingVb == OpState::opOn || receivingVb == OpState::opComplete);

	sendingAb = OpState::opOn;
	if (receivingVb == OpState::opOn)
		hostx->noteAbContentAvailable();
}

void Adapter::Xaction::abMakeMore() {
	Must(receivingVb == OpState::opOn);	/* A precondition for receiving more vb */
	hostx->vbMakeMore();
}

void Adapter::Xaction::abStopMaking() {
	sendingAb = OpState::opComplete;
	/* We do not need more vb if the host is not interested in more ab */
	stopVb();
}

libecap::Area Adapter::Xaction::abContent(size_type offset, size_type size) {
	Must(sendingAb == OpState::opOn || sendingAb == OpState::opComplete);

	/* If complete, there is nothing more to return */
	if (sendingAb == OpState::opComplete)
		return libecap::Area::FromTempString(c_emptyString);

	offset = compresscontext.sendingOffset + offset;
	size = compresscontext.compressedSize - offset;
	/* Compressed data need to be send */
	return libecap::Area::FromTempBuffer((const char*)&compresscontext.Buffer[offset], size);
}

void Adapter::Xaction::abContentShift(size_type size) {
	Must(sendingAb == OpState::opOn || sendingAb == OpState::opComplete);
	compresscontext.sendingOffset += size;
	hostx->vbContentShift(compresscontext.lastChunkSize);
}

void Adapter::Xaction::noteVbContentDone(bool atEnd) {
	compresscontext.zstream.total_out = 0;

	auto rc = deflate(&compresscontext.zstream,  Z_FINISH);
	if (rc != Z_STREAM_END) {
		switch (rc) {
		case Z_ERRNO:
			ErrorLog(C_ERR_READING, service->v_ErrLog);
			break;
		case Z_STREAM_ERROR:
			ErrorLog(C_ERR_STREAM_INCONS, service->v_ErrLog);
			break;
		case Z_DATA_ERROR:
			ErrorLog(C_ERR_DEFLATE_INVALID, service->v_ErrLog);
			break;
		case Z_MEM_ERROR:
			ErrorLog(C_ERR_MALLOC_ERROR, service->v_ErrLog);
			break;
		case Z_BUF_ERROR:
			ErrorLog(C_ERR_RANOUT_OUT_BUFFER, service->v_ErrLog);
			break;
		case Z_VERSION_ERROR:
			ErrorLog(C_ERR_VERSION_ZLIB_2, service->v_ErrLog);
			break;
		default:
			ErrorLog(C_ERR_UNKNOWN_2 + std::to_string(rc), service->v_ErrLog);
			break;
		}
	}

	rc = deflateEnd(&compresscontext.zstream);
	compresscontext.compressedSize += compresscontext.zstream.total_out;

	if (service->v_CompLog) {
		double v_comp_ratio;
		if (compresscontext.originalSize > 0)
			v_comp_ratio = 1.0 - ((double)compresscontext.compressedSize / (double)compresscontext.originalSize);
		else
			v_comp_ratio = 0.0;

		if (v_comp_ratio > 0.0)
			CompressionLog(compresscontext.originalSize, compresscontext.compressedSize, v_comp_ratio, std::ref(contentTypeString));
	}

	/* Gzip footer */
	if (controlFlags.requestAcceptEncodingGzip && rc == Z_OK) {
		/* Write crc & stream.total_in in LSB order */
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) compresscontext.checksum & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.checksum >> 8) & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.checksum >> 16) & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.checksum >> 24) & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) compresscontext.originalSize & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.originalSize >> 8) & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.originalSize >> 16) & 0xff;
		compresscontext.Buffer[compresscontext.compressedSize++] = (char) (compresscontext.originalSize >> 24) & 0xff;
	}

	Must(receivingVb == OpState::opOn);
	receivingVb = OpState::opComplete;
	if (sendingAb == OpState::opOn) {
		hostx->noteAbContentDone(atEnd);
		sendingAb = OpState::opComplete;
	}
}

void Adapter::Xaction::noteVbContentAvailable() {
	Must(receivingVb == OpState::opOn);

	const libecap::Area vb = hostx->vbContent(0, libecap::nsize);	/* Get all vb */
	hostx->vbContentShift(vb.size);					/* New */
	compresscontext.originalSize += vb.size;			/* Calculate original byte size for GZIP footer */
	compresscontext.lastChunkSize = vb.size;			/* Store chunk size for contentShift() */

	std::size_t v_bufSize = 12 + compresscontext.originalSize*1.01;	/* Buffers size by Zlib */

	/* Allocate the Buffer and initialize it */
	compresscontext.Buffer.resize(v_bufSize);
	/* If this is the first content chunk, add the gzip header not need for deflate */
	if (compresscontext.originalSize == vb.size && controlFlags.requestAcceptEncodingGzip) {
		compresscontext.Buffer[0] = (unsigned char) 31;          /* Magic number #1 */
		compresscontext.Buffer[1] = (unsigned char) 139;         /* Magic number #2 */
		compresscontext.Buffer[2] = (unsigned char) Z_DEFLATED;  /* Method */
		compresscontext.Buffer[3] = (unsigned char) 0;           /* Flags */
		compresscontext.Buffer[4] = (unsigned char) 0;           /* Mtime #1 */
		compresscontext.Buffer[5] = (unsigned char) 0;           /* Mtime #2 */
		compresscontext.Buffer[6] = (unsigned char) 0;           /* Mtime #3 */
		compresscontext.Buffer[7] = (unsigned char) 0;           /* Mtime #4 */
		compresscontext.Buffer[8] = (unsigned char) 0;           /* Extra flags */
		compresscontext.Buffer[9] = (unsigned char) 3;           /* Operating system */
		compresscontext.compressedSize = 10;
	}

	compresscontext.zstream.next_in = (Bytef*)vb.start;	/* Pointer to input bytes */
	compresscontext.zstream.avail_in = vb.size;		/* Number of input bytes left to process */
	compresscontext.zstream.next_out = (Bytef*)&compresscontext.Buffer[compresscontext.compressedSize];
	compresscontext.zstream.avail_out = v_bufSize;

	compresscontext.zstream.total_out = 0;			/* Total number of output bytes produced so far */

	auto rc = deflate(&compresscontext.zstream, Z_SYNC_FLUSH);
	if (rc == Z_OK && controlFlags.requestAcceptEncodingGzip)	/* Calculate CRC32 for GZIP footer no need for deflate */
		compresscontext.checksum = crc32(compresscontext.checksum, (Bytef*)vb.start, vb.size);

	compresscontext.compressedSize += compresscontext.zstream.total_out;

	if (sendingAb == OpState::opOn)
		hostx->noteAbContentAvailable();
}

/**
 * Tells the host that we are not interested in [more] vb if the host does not know that already
 */
void Adapter::Xaction::stopVb() {
	if (receivingVb == OpState::opOn) {
		hostx->vbStopMaking();
		receivingVb = OpState::opComplete;
	} else /* We already got the entire body or refused it earlier */
		Must(receivingVb != OpState::opUndecided);
}

/**
 * This method is used to make the last call to hostx transaction
 * last call may delete adapter transaction if the host no longer needs it
 */
libecap::host::Xaction *Adapter::Xaction::lastHostCall() {
	libecap::host::Xaction *x = hostx;
	Must(x);
	hostx = nullptr;
	return x;
}

namespace {

/**
 * Create the adapter and register with libecap to reach the host application
 */
const bool Registered = (libecap::RegisterVersionedService(new Adapter::Service));

}	/* namespace */
