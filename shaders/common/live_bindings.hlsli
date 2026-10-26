#ifndef DVE_LIVE_BINDINGS_HLSLI
#define DVE_LIVE_BINDINGS_HLSLI
#ifdef __spirv__
#define DVE_LIVE_BIND(B,S) [[vk::binding(B,S)]]
#define DVE_LIVE_IMAGE(B,S) [[vk::binding(B,S)]] [[vk::combinedImageSampler]]
#define DVE_LIVE_LOCATION(L) [[vk::location(L)]]
#else
#define DVE_LIVE_BIND(B,S)
#define DVE_LIVE_IMAGE(B,S)
#define DVE_LIVE_LOCATION(L)
#endif
#endif
