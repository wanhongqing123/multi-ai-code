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
#ifdef __cplusplus
}
#endif
