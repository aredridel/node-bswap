#include <nan.h>
#include <stdint.h>

///// Min compiler support and bswap intrins used in scalar region
#if defined(__GNUC__) // GCC, clang
# ifdef __clang__
#  if __clang_major__ < 3 || (__clang_major__ == 3 && __clang_minor__ < 4)
#   error("Requires clang >= 3.4")
#  endif
# else
#  if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
#   error("Requires GCC >= 4.8")
#  endif
# endif

# include <immintrin.h>
# define BSWAP_INTRINSIC_2(x) __builtin_bswap16(x)
# define BSWAP_INTRINSIC_4(x) __builtin_bswap32(x)
# define BSWAP_INTRINSIC_8(x) __builtin_bswap64(x)

#elif defined(_MSC_VER)
# include <intrin.h>
# define BSWAP_INTRINSIC_2(x) _byteswap_ushort(x);
# define BSWAP_INTRINSIC_4(x) _byteswap_ulong(x);
# define BSWAP_INTRINSIC_8(x) _byteswap_uint64(x);

#endif

///// CPU feature detection
const static uint8_t EAX = 0;
const static uint8_t EBX = 1;
const static uint8_t ECX = 2;
const static uint8_t EDX = 3;

static bool cpuid(uint8_t outreg, uint8_t bit, uint32_t initEax, uint32_t initEcx = 0) {
	uint32_t regs[4];
#ifdef _MSC_VER
	__cpuidex(reinterpret_cast<int32_t*>(regs), initEax, initEcx);
#else
	asm volatile("cpuid"
		: "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
		: "0" (initEax), "2" (initEcx));
#endif
	return regs[outreg] & (1 << bit);
}

// __builtin_cpu_supports("avx2") for GCC 4.8+, Clang 3.7+, ICC 16+ only
static bool supportsAVX2() { return cpuid(EBX, 5, 7); }
static bool supportsAVX512BW() { return cpuid(EBX, 30, 7); }

///// Vector class for templating
class Vec128 {
public:
	__m128i v;
	static uint8_t size() { return 16; }
	template<typename STYPE> static Vec128 getMask() {
		switch (sizeof(STYPE)) {
		case 2: return Vec128(_mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14));
		case 4: return Vec128(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
		case 8: return Vec128(_mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8));
		default: return Vec128(_mm_setzero_si128()); // squash warnings.
		}
	}

	Vec128() {};
	Vec128(__m128i const & _v) : v(_v) {};
	void load(uint8_t* addr) { v = _mm_loadu_si128((__m128i*)addr); }
	void shuffle(Vec128 mask) { v = _mm_shuffle_epi8(v, mask.v); }
	void store(uint8_t* addr) { _mm_storeu_si128((__m128i*)addr, v); }
};

class Vec256 {
public:
	__m256i v;
	static uint8_t size() { return 32; }
	template<typename STYPE> static Vec256 getMask() {
		return Vec256(_mm256_broadcastsi128_si256(Vec128::getMask<STYPE>().v));
	}

	Vec256() {};
	Vec256(__m256i const & _v) : v(_v) {};
	void load(uint8_t* addr) { v = _mm256_loadu_si256((__m256i*)addr); }
	void shuffle(Vec256 mask) { v = _mm256_shuffle_epi8(v, mask.v); }
	void store(uint8_t* addr) { _mm256_storeu_si256((__m256i*)addr, v); }
};

#ifndef _MSC_VER
// v15.3 starts to have AVX-512 support, but my install doesn't have proper
// zmmintrin intrinsics.
class Vec512 {
public:
	__m512i v;
	static uint8_t size() { return 64; }
	template<typename STYPE> static Vec512 getMask() {
		return Vec512(_mm512_broadcast_i32x4(Vec128::getMask<STYPE>().v));
	}

	Vec512() {};
	Vec512(__m512i const & _v) : v(_v) {};
	void load(uint8_t* addr) { v = _mm512_loadu_si512((void*)addr); }
	void shuffle(Vec512 mask) { v = _mm512_shuffle_epi8(v, mask.v); }
	void store(uint8_t* addr) { _mm512_storeu_si512((void*)addr, v); }
};
#endif

static inline void swap(uint16_t* val) { *val = BSWAP_INTRINSIC_2(*val); }
static inline void swap(uint32_t* val) { *val = BSWAP_INTRINSIC_4(*val); }
static inline void swap(uint64_t* val) { *val = BSWAP_INTRINSIC_8(*val); }

template<typename STYPE, class VTYPE>
static void shuffle(v8::Local<v8::TypedArray> data_ta) {
	Nan::TypedArrayContents<STYPE> data(data_ta);
	uint8_t* bytes = reinterpret_cast<uint8_t*>(*data);

	size_t byteLength = data_ta->ByteLength();
	size_t elemLength = byteLength / sizeof(STYPE);

	// Scalar until aligned
	size_t sIdx = 0;
	size_t preLength = ((uintptr_t)(void *)(bytes) % 32) / sizeof(STYPE);
	if (elemLength < preLength) preLength = elemLength;
	while (sIdx < preLength) swap(&(*data)[sIdx++]);

	size_t bIdx = sIdx * sizeof(STYPE);

	size_t vectSize = VTYPE::size();
	size_t tailLength = (byteLength - bIdx) % vectSize;
	size_t vectLength = byteLength - tailLength;

	VTYPE mask = VTYPE::template getMask<STYPE>();
	VTYPE vec;
	while (bIdx < vectLength) {
		vec.load(&bytes[bIdx]);
		vec.shuffle(mask);
		vec.store(&bytes[bIdx]);
		bIdx += vectSize;
	}

	sIdx = vectLength / sizeof(STYPE);
	while (sIdx < elemLength) swap(&(*data)[sIdx++]);
}

template <class VTYPE>
NAN_METHOD(flipBytes) {
	v8::Local<v8::Value> arr = info[0];
	if (arr->IsInt16Array() || arr->IsUint16Array()) {
		shuffle<uint16_t, VTYPE>(arr.As<v8::TypedArray>());
	} else if (arr->IsFloat32Array() || arr->IsInt32Array() || arr->IsUint32Array()) {
		shuffle<uint32_t, VTYPE>(arr.As<v8::TypedArray>());
	} else if (arr->IsFloat64Array()) {
		shuffle<uint64_t, VTYPE>(arr.As<v8::TypedArray>());
	} else if (arr->IsInt8Array() || arr->IsUint8Array() || arr->IsUint8ClampedArray()) {
		// noop
	} else {
		Nan::ThrowTypeError("Expected typed array");
	}
}

NAN_MODULE_INIT(Init) {
	v8::Local<v8::FunctionTemplate> ft;
	v8::MaybeLocal<v8::String> ise;

	// MSVC doesn't have any equivalent to -march=native, but it will emit
	// instructions from any instruction set when intrinsics are used. This lets
	// us set EnableEnhancedInstructionSet to a low/compatible value while using
	// run-time dispatch to pick a faster version.
#ifdef _MSC_VER
	// Warning: Do not put the ternary outside of the New. Performance will tank.
	ft = Nan::New<v8::FunctionTemplate>(supportsAVX2() ? flipBytes<Vec256> : flipBytes<Vec128>);
	ise = Nan::New(supportsAVX2() ? "AVX2" : "SSSE3");
#else
	// GNU-compatible compilers have -march=native, and refuse to emit
	// instructions from an instruction set less than the -m flags allow.
# if defined(__AVX512BW__) && defined(BSWAP_USE_AVX512)
	// Disabled by default because it is slower than AVX2.
	ft = Nan::New<v8::FunctionTemplate>(flipBytes<Vec512>);
	ise = Nan::New("AVX512");
# elif defined(__AVX2__)
	ft = Nan::New<v8::FunctionTemplate>(flipBytes<Vec256>);
	ise = Nan::New("AVX2");
# else
	ft = Nan::New<v8::FunctionTemplate>(flipBytes<Vec128>);
	ise = Nan::New("SSSE3");
# endif
#endif

	Nan::Set(target, Nan::New("flipBytes").ToLocalChecked(), Nan::GetFunction(ft).ToLocalChecked());
	Nan::Set(target, Nan::New("ISE").ToLocalChecked(), ise.ToLocalChecked());
}

NODE_MODULE(bswap, Init);
