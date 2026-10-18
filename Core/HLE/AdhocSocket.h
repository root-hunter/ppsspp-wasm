#pragma once

#include "Common/Net/SocketCompat.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

// Keep browser transport selection at the Ad Hoc boundary. sceNetInet/HTTP and
// other emulator sockets retain their normal backend. Native builds are unchanged.
inline int CreateAdhocSocket(int family, int type, int protocol, bool control = false) {
	int fd = socket(family, type, protocol);
#ifdef __EMSCRIPTEN__
	if (fd >= 0) {
		MAIN_THREAD_EM_ASM({
			var sock = Module['SOCKFS'].getSocket($0);
			if (sock) {
				sock.__ppssppAdhoc = true;
				sock.__ppssppControl = !!$1;
			}
		}, fd, control);
	}
#endif
	return fd;
}

