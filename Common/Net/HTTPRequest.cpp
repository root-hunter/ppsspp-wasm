#include "Common/Net/HTTPRequest.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPNaettRequest.h"
#include "Common/TimeUtil.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/Log.h"
#include "Common/System/OSD.h"
#include "Common/System/System.h"

#ifdef __EMSCRIPTEN__
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/threading.h>
#endif

namespace http {

#ifdef __EMSCRIPTEN__
// Completed responses are consumed by RequestManager::Update on the UI thread.
// No synchronous XHR or native sockets on the browser main thread.
class BrowserRequest final : public Request {
public:
	BrowserRequest(RequestMethod method, std::string_view url, std::string_view data, std::string_view mime,
                const Path &outfile, RequestFlags flags, std::string_view name)
		: Request(method, url, name, &cancelled_, flags), data_(data), mime_(mime) { outfile_ = outfile; }
	~BrowserRequest() override { Join(); }
	void Start() override {
		// The WLAN worker never yields to a JS event loop. Own XHRs on the
		// browser thread so responses arrive during threaded gameplay too.
		if (!emscripten_is_main_runtime_thread()) {
			emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, StartOnMain, this);
			return;
		}
		emscripten_fetch_attr_t attr;
		emscripten_fetch_attr_init(&attr);
		strcpy(attr.requestMethod, method_ == RequestMethod::POST ? "POST" : "GET");
		attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
		attr.timeoutMSecs = 30000;
		attr.userData = this;
		attr.onsuccess = attr.onerror = [](emscripten_fetch_t *fetch) {
			auto self = static_cast<BrowserRequest *>(fetch->userData);
			self->resultCode_ = fetch->status ? fetch->status : -1;
			if (fetch->numBytes && fetch->data) {
				memcpy(self->buffer_.Append(fetch->numBytes), fetch->data, fetch->numBytes);
			}
			self->completed_ = true;
		};
		const char *headers[] = {"Accept", acceptMime_, nullptr, nullptr, nullptr};
		if (!mime_.empty()) { headers[2] = "Content-Type"; headers[3] = mime_.c_str(); }
		attr.requestHeaders = headers;
		attr.requestData = data_.data();
		attr.requestDataSize = data_.size();
		// The shell supplies an HTTPS gateway for RA's API and badge images (CORS).
		char *mapped = (char *)EM_ASM_PTR({
			const url = new URL(UTF8ToString($0));
			let target = url.href;
			if (['retroachievements.org', 'media.retroachievements.org'].includes(url.hostname)) {
				url.protocol = 'https:';
				const proxy = Module['retroAchievementsProxy'];
				if (proxy) target = proxy + '?url=' + encodeURIComponent(url.href);
				else target = url.href;
			}
			return stringToNewUTF8(target);
		}, url_.c_str());
		fetch_ = emscripten_fetch(&attr, mapped);
		free(mapped);
		if (!fetch_) { resultCode_ = -1; completed_ = true; }
	}
	bool Done() override {
		if (cancelled_ && !completed_) { Join(); resultCode_ = -1000; completed_ = true; }
		if (!completed_) return false;
		if (!finalized_) {
			failed_ = resultCode_ != 200;
			if (!failed_ && !outfile_.empty())
				failed_ = !buffer_.FlushToFile(outfile_, !(flags_ & RequestFlags::KeepInMemory));
			progress_.Update(0, 0, true);
			finalized_ = true;
		}
		return true;
	}
	bool Failed() const override { return failed_; }
	void Join() override {
		if (!emscripten_is_main_runtime_thread()) {
			emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, JoinOnMain, this);
			return;
		}
		if (fetch_) { emscripten_fetch_close(fetch_); fetch_ = nullptr; }
	}
private:
	static void StartOnMain(BrowserRequest *request) { request->Start(); }
	static void JoinOnMain(BrowserRequest *request) { request->Join(); }
	std::string data_, mime_;
	emscripten_fetch_t *fetch_ = nullptr;
	std::atomic<bool> completed_{false};
	bool finalized_ = false, failed_ = false;
};
#endif

Request::Request(RequestMethod method, std::string_view url, std::string_view name, bool *cancelled, RequestFlags flags)
	: method_(method), url_(url), name_(name), progress_(cancelled), flags_(flags) {
	INFO_LOG(Log::HTTP, "HTTP %s request: %.*s (%.*s)", RequestMethodToString(method), (int)url.size(), url.data(), (int)name.size(), name.data());

	progress_.callback = [this](int64_t bytes, int64_t contentLength, bool done) {
		std::string message;
		if (!name_.empty()) {
			message = name_;
		} else {
			std::size_t pos = url_.rfind('/');
			if (pos != std::string::npos) {
				message = url_.substr(pos + 1);
			} else {
				message = url_;
			}
		}
		if (flags_ & RequestFlags::ProgressBar) {
			if (!done) {
				g_OSD.SetProgressBar(url_, std::move(message), 0.0f, (float)contentLength, (float)bytes, flags_ & RequestFlags::ProgressBarDelayed ? 3.0f : 0.0f);  // delay 3 seconds before showing.
			} else {
				g_OSD.RemoveProgressBar(url_, Failed() ? false : true, 0.5f);
			}
		}
	};
}

static bool IsHttpsUrl(std::string_view url) {
	return startsWith(url, "https:");
}

Path UrlToCachePath(const Path &cacheDir, std::string_view url) {
	std::string fn = "DLCACHE_";
	for (auto c : url) {
		if (isalnum(c) || c == '.' || c == '-' || c == '_') {
			fn.push_back(tolower(c));
		} else {
			fn.push_back('_');
		}
	}
	return cacheDir / fn;
}

Path RequestManager::UrlToCachePath(const std::string_view url) {
	if (cacheDir_.empty()) {
		return Path();
	}
	return http::UrlToCachePath(cacheDir_, url);
}

static std::shared_ptr<Request> CreateRequest(RequestMethod method, std::string_view url, std::string_view postdata, std::string_view postMime, const Path &outfile, RequestFlags flags, net::ResolveFunc customResolve, std::string_view name) {
#ifdef __EMSCRIPTEN__
	return std::make_shared<BrowserRequest>(method, url, postdata, postMime, outfile, flags, name);
#else
	if (IsHttpsUrl(url) && System_GetPropertyBool(SYSPROP_SUPPORTS_HTTPS)) {
#ifndef HTTPS_NOT_AVAILABLE
		return std::make_shared<HTTPSRequest>(method, url, postdata, postMime, outfile, flags, name);
#else
		return std::shared_ptr<Request>();
#endif
	} else {
		return std::make_shared<HTTPRequest>(method, url, postdata, postMime, outfile, flags, customResolve, name);
	}
#endif
}

// Compatible with StartDownload
// Synchronous (no callback).
bool RequestManager::ReadFileFromCache(std::string_view url, std::string *data) {
	Path cacheFile = UrlToCachePath(url);
	return File::ReadBinaryFileToString(cacheFile, data);
}

std::shared_ptr<Request> RequestManager::StartDownload(std::string_view url, const Path &outfile, RequestFlags flags, const char *acceptMime, std::string_view name, std::function<void(Request &)> callback) {
	const bool enableCache = !cacheDir_.empty() && (flags & RequestFlags::Cached24H);

	// Come up with a cache file path.
	Path cacheFile = UrlToCachePath(url);

	if (enableCache) {
		_dbg_assert_(outfile.empty());  // It's automatically replaced below

		// TODO: This should be done on the thread, maybe. But let's keep it simple for now.
		time_t cacheFileTime;
		if (File::GetModifTimeT(cacheFile, &cacheFileTime)) {
			time_t now = (time_t)time_now_unix_utc();
			if (cacheFileTime > now - 24 * 60 * 60) {
				// The file is new enough. Let's construct a fake, already finished download so we don't need
				// to modify the calling code.
				std::string contents;
				if (File::ReadBinaryFileToString(cacheFile, &contents)) {
					INFO_LOG(Log::sceNet, "Returning cached file for %.*s: %s", (int)url.size(), url.data(), cacheFile.c_str());
					// All is well, but we've indented a bit much here.
					std::shared_ptr<Request> dl(new CachedRequest(RequestMethod::GET, url, KeepAfterLast(url, '/'), nullptr, flags, contents));
					newDownloads_.push_back(dl);
					dl->SetCallback(callback);
					return dl;
				} else {
					INFO_LOG(Log::sceNet, "Failed reading from cache, proceeding with request");
				}
			} else {
				INFO_LOG(Log::sceNet, "Cached file too old, proceeding with request");
			}
		} else {
			INFO_LOG(Log::sceNet, "Failed to check time modified. Proceeding with request.");
		}
	}

	std::shared_ptr<Request> dl = CreateRequest(RequestMethod::GET, url, "", "", outfile, flags, nullptr, name);

	// OK, didn't get it from cache, so let's continue with the download, putting it in the cache.
	if (enableCache) {
		dl->OverrideOutFile(cacheFile);
		dl->AddFlag(RequestFlags::KeepInMemory);
	}

	if (!userAgent_.empty()) {
		dl->SetUserAgent(userAgent_);
	}
	if (acceptMime) {
		dl->SetAccept(acceptMime);
	}
	dl->SetCallback(callback);
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}

std::shared_ptr<Request> RequestManager::AsyncPostWithCallback(
	std::string_view url,
	std::string_view postData,
	std::string_view postMime,
	RequestFlags flags,
	std::function<void(Request &)> callback,
	std::string_view name) {
	std::shared_ptr<Request> dl = CreateRequest(RequestMethod::POST, url, postData, postMime, Path(), flags, nullptr, name);
	if (!userAgent_.empty())
		dl->SetUserAgent(userAgent_);
	dl->SetCallback(callback);
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}

void RequestManager::Update() {
	for (auto &iter : newDownloads_) {
		downloads_.push_back(iter);
	}
	newDownloads_.clear();

restart:
	for (size_t i = 0; i < downloads_.size(); i++) {
		auto dl = downloads_[i];
		if (dl->Done()) {
			dl->RunCallback();
			dl->Join();
			downloads_.erase(downloads_.begin() + i);
			goto restart;
		}
	}
}

void RequestManager::CancelAll() {
	for (size_t i = 0; i < downloads_.size(); i++) {
		downloads_[i]->Cancel();
	}
	for (size_t i = 0; i < downloads_.size(); i++) {
		downloads_[i]->Join();
	}
	downloads_.clear();
}

}  // namespace
