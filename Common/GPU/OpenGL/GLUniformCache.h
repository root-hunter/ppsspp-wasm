#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>

// Owned by a single linked program and used only by the GL submission thread.
// Compare raw bits so signed zero and NaN payloads retain their GL semantics.
class GLUniformValueCache {
public:
	bool Update(int location, int kind, const void *value, size_t bytes) {
		if (location < 0 || bytes == 0 || bytes > 64)
			return true;
		auto &entry = values_[location];
		if (entry.bytes == bytes && entry.kind == kind && !std::memcmp(entry.value.data(), value, bytes))
			return false;
		entry.kind = kind;
		entry.bytes = bytes;
		std::memcpy(entry.value.data(), value, bytes);
		return true;
	}

	void Clear() { values_.clear(); }

private:
	struct Entry {
		std::array<uint8_t, 64> value{};
		size_t bytes = 0;
		int kind = 0;
	};
	std::unordered_map<int, Entry> values_;
};
