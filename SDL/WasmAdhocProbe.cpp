// Protocol-level integration probe. Optional; never accesses PSP game memory.
// Exercises the same Emscripten socket syscalls as the Ad Hoc HLE.
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "Core/HLE/AdhocSocket.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/HW/Display.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

static unsigned char probeBuffer[65536];

extern "C" {
EMSCRIPTEN_KEEPALIVE int ppssppNetProbe(int op, int fd, uint32_t ip, int port, int value) {
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(ip);
	address.sin_port = htons(port);
	int result = -1;
	switch (op) {
	case 0: // fd parameter is SOCK_STREAM (1) / SOCK_DGRAM (2)
		result = CreateAdhocSocket(AF_INET, fd, 0, value != 0);
		if (result >= 0) fcntl(result, F_SETFL, O_NONBLOCK);
		break;
	case 1: result = bind(fd, (sockaddr *)&address, sizeof(address)); break;
	case 2: result = connect(fd, (sockaddr *)&address, sizeof(address)); break;
	case 3: result = listen(fd, value); break;
	case 4: result = accept(fd, nullptr, nullptr); break;
	case 5: // Raw buffer populated byte-by-byte by JS, then send through WASM.
		result = send(fd, probeBuffer, value, 0); break;
	case 6: result = sendto(fd, probeBuffer, value, 0, (sockaddr *)&address, sizeof(address)); break;
	case 7:
		result = recv(fd, probeBuffer, sizeof(probeBuffer), value);
		break;
	case 8: result = close(fd); break;
	case 9: {
		pollfd pfd{fd, static_cast<short>(value), 0};
		result = poll(&pfd, 1, 0);
		if (result >= 0) result = pfd.revents;
		break;
	}
	case 10: {
		socklen_t size = sizeof(address);
		result = getsockname(fd, (sockaddr *)&address, &size);
		if (result == 0) return ntohl(address.sin_addr.s_addr);
		break;
	}
	case 11: return g_Config.bEnableWlan ? 1 : 0;
	case 12: return snprintf(reinterpret_cast<char *>(probeBuffer), sizeof(probeBuffer), "%s", g_Config.sNickName.c_str());
	case 13: return snprintf(reinterpret_cast<char *>(probeBuffer), sizeof(probeBuffer), "%s", g_Config.sMACAddress.c_str());
	case 14: return g_Config.iAdhocServerRelayMode;
	case 15: return PSP_IsInited() ? 1 : 0;
	case 16:
	case 17:
	case 18: {
		float vps, fps, actual;
		__DisplayGetFPS(&vps, &fps, &actual);
		return static_cast<int>((op == 16 ? vps : op == 17 ? fps : actual) * 1000);
	}
	case 19: return g_Config.iCpuCore;
	case 20:
		if (value >= 0) g_Config.bForceLagSync = value != 0;
		return g_Config.bForceLagSync ? 1 : 0;
	case 21:
		if (value >= 0) g_Config.bAutoFrameSkip = value != 0;
		return g_Config.bAutoFrameSkip ? 1 : 0;
	case 22:
		if (value >= 0 && value <= 2) g_Config.iFrameSkip = value;
		return g_Config.iFrameSkip;
	case 23:
		if (value >= 0) g_Config.bSoftwareSkinning = value != 0;
		return g_Config.bSoftwareSkinning ? 1 : 0;

	default: return -EINVAL;
	}
	return result < 0 ? -errno : result;
}

EMSCRIPTEN_KEEPALIVE int ppssppNetProbeByte(int offset, int value) {
	if (offset < 0 || offset >= static_cast<int>(sizeof(probeBuffer))) return -EINVAL;
	if (value >= 0 && value <= 255) probeBuffer[offset] = value;
	return probeBuffer[offset];
}
}
#endif
