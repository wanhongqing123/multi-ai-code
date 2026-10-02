#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// 进程内 C ABI。宿主必须从同一条后台串行队列调用，包括销毁。
// create 仅分配句柄；configure 才打开库。request 返回拥有的 UTF-8 JSON，必须
// free。 密钥只进 configure，不从响应/事件返回。异常不能跨语言边界传播。
void* maiMobileAgentCreate(void);
void maiMobileAgentDestroy(void* handle);
char* maiMobileAgentRequest(void* handle, const char* request);
void maiMobileAgentFree(char* response);
// Register the address returned by OrtGetApiBase from a statically linked iOS ONNX Runtime
// framework before configure. Android and desktop use a bundled dynamic library path instead.
// The pointer is borrowed for the lifetime of the host process; call only on the Agent worker.
int maiMobileAgentSetOrtApiBase(void* handle, const void* apiBase);

// Direct host entry for video matting, sharing the same parameter schema and processor as the
// cv_video_matting Agent tool. Paths are UTF-8; output_path in argumentsJson remains relative to
// workspace. Returns an owned {ok,output|error} JSON string; release with maiMobileAgentFree.
char* maiMobileMatteVideo(const char* argumentsJson, const char* workspace,
                          const char* modelPath, const char* runtimePath,
                          const void* apiBase);

// MaiChat-specific tools are implemented by the embedding application. MaiAgent invokes handler
// on an agent worker thread after its normal approval gate. The handler may be called concurrently
// by different sessions and must marshal platform state access to the correct host thread.
//
// toolName and argumentsJson are borrowed UTF-8 strings valid only during the callback. The
// handler returns an owned, null-terminated UTF-8 JSON response:
//   {"ok":true,"output":"text returned to the model"}
//   {"ok":false,"error":"actionable failure"}
// responseFree must release that response with the host's allocator. contextRelease is called once
// after all agent work has stopped, when the handler is replaced or the bridge is destroyed.
typedef const char* (*MaiMobileHostToolHandler)(void* context, const char* toolName,
                                                const char* argumentsJson);
typedef void (*MaiMobileHostToolResponseFree)(void* context, const char* response);
typedef void (*MaiMobileHostToolContextRelease)(void* context);

// Returns 1 on success. Returns 0 for an invalid handle/handler set or while a tool callback is in
// flight. Passing a null handler clears the current registration; the three callback pointers must
// otherwise all be non-null.
int maiMobileAgentSetHostToolHandler(void* handle, void* context,
                                    MaiMobileHostToolHandler handler,
                                    MaiMobileHostToolResponseFree responseFree,
                                    MaiMobileHostToolContextRelease contextRelease);

#ifdef __cplusplus
}
#endif
