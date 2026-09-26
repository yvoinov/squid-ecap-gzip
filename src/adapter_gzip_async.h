#ifndef ADAPTER_GZIP_ASYNC_H
#define ADAPTER_GZIP_ASYNC_H

#ifdef HAVE_CONFIG_H
#	include "autoconf.h"
#endif

#include <libecap/adapter/service.h>
#include <libecap/adapter/xaction.h>
#include <libecap/common/named_values.h>
#include <libecap/common/area.h>
#include <libecap/common/message.h>
#include <libecap/host/xaction.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <string>
#ifdef HAVE_ZLIB_H
#	include <zlib.h>
#else
#	error Require zlib.h to build.
#endif

namespace Adapter {

class Xaction;

class Service: public libecap::adapter::Service {
	public:
		Service();
		virtual ~Service();
		virtual std::string uri() const;
		virtual std::string tag() const;
		virtual void describe(std::ostream &os) const;
		virtual bool makesAsyncXactions() const;
		virtual void configure(const libecap::Options &cfg);
		virtual void reconfigure(const libecap::Options &cfg);
		void setOne(const libecap::Name &name, const libecap::Area &valArea);
		virtual void start();
		virtual void suspend(timeval &timeout);
		virtual void resume();
		virtual void stop();
		virtual void retire();
		virtual bool wantsUrl(const char *url) const;
		virtual MadeXactionPointer makeXaction(libecap::host::Xaction *hostx);

		std::size_t MaxSize;
		std::size_t Level;
		bool ErrLog;
		bool CompLog;

		void enqueue(libecap::shared_ptr<Xaction> x);
		void ready(libecap::shared_ptr<Xaction> x);

	private:
		friend class Xaction;

		void worker();

		std::string ErrLogName;
		std::string CompLogName;
		std::mutex WorkMutex;
		std::condition_variable WorkCondition;
		std::deque<libecap::shared_ptr<Xaction> > WorkQueue;
		std::mutex ReadyMutex;
		std::deque<libecap::shared_ptr<Xaction> > ReadyQueue;
		std::vector<std::thread> Workers;
		std::size_t WorkerCount;
		std::atomic<bool> Stopping;// Atomic because worker threads check it while waiting without holding WorkMutex
		std::size_t ActiveWork;
};

class Cfgtor: public libecap::NamedValueVisitor {
	public:
		Cfgtor(Service &aSvc): svc(aSvc) {}
		virtual void visit(const libecap::Name &name, const libecap::Area &value) { svc.setOne(name, value); }
		Service &svc;
};

class Xaction: public libecap::adapter::Xaction {
	public:
		Xaction(Service *s, libecap::host::Xaction *x);
		virtual ~Xaction();
		void setSelf(const libecap::shared_ptr<Xaction> &self);

		virtual const libecap::Area option(const libecap::Name &name) const;
		virtual void visitEachOption(libecap::NamedValueVisitor &visitor) const;
		virtual void start();
		virtual void stop();
		virtual void resume();
		void resumeHost();
		virtual void abDiscard();
		virtual void abMake();
		virtual void abMakeMore();
		virtual void abStopMaking();
		virtual libecap::Area abContent(libecap::size_type offset, libecap::size_type size);
		virtual void abContentShift(libecap::size_type size);
		virtual void noteVbContentDone(bool atEnd);
		virtual void noteVbContentAvailable();

		std::string contentTypeString;
		static void ErrorLog(const std::string &p_log_entry, bool p_ErrLog, const std::string &p_log_name);

	private:
		friend class Service;

		struct InputChunk {
			std::vector<unsigned char> data;
		};

		struct OutputChunk {
			std::vector<unsigned char> data;
			std::size_t offset;

			OutputChunk(): offset(0) {}
		};

		enum class OpState { opUndecided, opOn, opComplete, opNever };

		bool processOne();
		void processInput(InputChunk &input);
		void finishCompression(bool atEnd);
		void signalReady();
		void stopVb();
		void CompressionLog(std::size_t p_origSize, std::size_t p_compSize, double p_compRatio,
			const std::string &p_Type, const std::string &p_compType = "deflate");
		bool requirementsAreMet();
		bool gzipInitialize();

		Service *service;
		libecap::host::Xaction *hostx;
		libecap::weak_ptr<Xaction> self;

		OpState receivingVb;
		OpState sendingAb;

		z_stream zstream;
		std::deque<InputChunk> inputQueue;
		std::deque<OutputChunk> outputQueue;
		mutable std::mutex queueMutex;
		bool finalPending;
		bool compressionFinished;
		bool finalSent;
		bool processingFailed;
		bool stopped;
		bool workScheduled;
		std::atomic<bool> readyScheduled;
		bool gzipMode;
		uLong checksum;
		std::size_t originalSize;
		std::size_t compressedSize;
		bool zstreamInitialized;
		bool atEnd;

		struct Controls {
			bool responseReject;
			bool responseContentTypeOk;
			bool requestAcceptEncodingOk;
			bool requestContentLengthOk;
			bool requestContentXecapOk;
			bool requestAcceptEncodingGzip;
			bool requestAcceptEncodingDeflate;

			Controls(): responseReject(true), responseContentTypeOk(false), requestAcceptEncodingOk(false),
				requestContentLengthOk(false), requestContentXecapOk(true), requestAcceptEncodingGzip(false),
				requestAcceptEncodingDeflate(false) {}
		} controlFlags;
};

inline bool Xaction::requirementsAreMet() {
	if (!controlFlags.responseReject || !controlFlags.responseContentTypeOk ||
			!controlFlags.requestAcceptEncodingOk || !controlFlags.requestContentXecapOk ||
			!controlFlags.requestContentLengthOk)
		return false;
	return true;
}

} /* namespace Adapter */

#endif
