//
//  CloudWebSocket.h
//  voice_common_ios
//
//  Created by sunnydu on 2025/4/29.
//

#import <Foundation/Foundation.h>
#import <Security/SecCertificate.h>
NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, QCReadyState) {
    QC_CONNECTING   = 0,//互联中
    QC_OPEN         = 1,//开启中
    QC_CLOSING      = 2,//关闭中
    QC_CLOSED       = 3,//已关闭
};

typedef enum QCloudWebSocketStatusCode : NSInteger {
    // 0–999: Reserved and not used.
    QCloudWebSocketStatusCodeNormal = 1000,
    QCloudWebSocketStatusCodeGoingAway = 1001,//断开
    QCloudWebSocketStatusCodeProtocolError = 1002,//协议错误
    QCloudWebSocketStatusCodeUnhandledType = 1003,//未处理类型
    // 1004 reserved.
    SRStatusNoStatusReceived = 1005,//已收到
    QCloudWebSocketStatusCodeAbnormal = 1006,//异常
    QCloudWebSocketStatusCodeInvalidUTF8 = 1007,//无效utf8编码
    QCloudWebSocketStatusCodePolicyViolated = 1008,//违反协议
    QCloudWebSocketStatusCodeMessageTooBig = 1009,//消息过大
    QCloudWebSocketStatusCodeMissingExtension = 1010,//缺少扩展名
    QCloudWebSocketStatusCodeInternalError = 1011,//内部错误
    QCloudWebSocketStatusCodeServiceRestart = 1012,//服务重启
    QCloudWebSocketStatusCodeTryAgainLater = 1013,//稍后尝试
    // 1014: Reserved for future use by the WebSocket standard.
    QCloudWebSocketStatusCodeTLSHandshake = 1015,//TLS握手

    // 1016–1999: Reserved for future use by the WebSocket standard.
    // 1016–1999：保留供WebSocket标准将来使用。

    // 2000–2999: Reserved for use by WebSocket extensions.
    // 2000–2999：保留供WebSocket扩展使用。

    // 3000–3999: Available for use by libraries and frameworks. May not be used by applications. Available for registration at the IANA via first-come, first-serve.
    // 3000–3999：可供库和框架使用。可能无法被应用程序使用。可通过先到先得的方式在IANA上进行注册。

    // 4000–4999: Available for use by applications.
    // 4000–4999：可供应用程序使用。
} QCloudWebSocketStatusCode;

@class QCloudWebSocket;

//extern NSString *const QCloudWebSocketErrorDomain;//错误范围提示
//extern NSString *const QCHTTPResponseErrorKey;//

#pragma mark - QCloudWebSocketDelegate

@protocol QCloudWebSocketDelegate;

#pragma mark - QCloudWebSocket

@interface QCloudWebSocket : NSObject <NSStreamDelegate>

@property (nonatomic, weak) id <QCloudWebSocketDelegate> delegate;

@property (nonatomic, readonly) QCReadyState readyState;
@property (nonatomic, readonly, retain) NSURL *url;


@property (nonatomic, readonly) CFHTTPMessageRef receivedHTTPHeaders;

// Optional array of cookies (NSHTTPCookie objects) to apply to the connections
//应用于连接的可选cookie数组（NSHTTPCookie对象）
@property (nonatomic, readwrite) NSArray * requestCookies;

// This returns the negotiated protocol.
// It will be nil until after the handshake completes.
//返回协商的协议。
//握手完成之前将为零。
@property (nonatomic, readonly, copy) NSString *protocol;

/**
 允许将内部TX队列大小限制为特定的字节数限制
 达到发送队列大小后，将发送以下调用：将阻塞直到发送数据
 通过websocket连接。
 将此设置为0不会限制发送队列的大小（默认）
 */
@property (nonatomic, assign) NSUInteger maxTxQueueSize;


// Protocols should be an array of strings that turn into Sec-WebSocket-Protocol.
//协议应该是一个字符串数组，这些字符串会变成Sec-WebSocket-Protocol。
- (id)initWithURLRequest:(NSURLRequest *)request protocols:(NSArray *)protocols allowsUntrustedSSLCertificates:(BOOL)allowsUntrustedSSLCertificates;
- (id)initWithURLRequest:(NSURLRequest *)request protocols:(NSArray *)protocols;
- (id)initWithURLQCRequest:(NSURLRequest *)request;

// Some helper constructors.
//一些辅助构造函数。
- (id)initWithURL:(NSURL *)url protocols:(NSArray *)protocols allowsUntrustedSSLCertificates:(BOOL)allowsUntrustedSSLCertificates;
- (id)initWithURL:(NSURL *)url protocols:(NSArray *)protocols;
- (id)initWithURL:(NSURL *)url;

// Delegate queue will be dispatch_main_queue by default.
// You cannot set both OperationQueue and dispatch_queue.
//默认情况下，代表队列将为dispatch_main_queue。
//您不能同时设置OperationQueue和dispatch_queue。
- (void)setDelegateOperationQueue:(NSOperationQueue*) queue;
- (void)setDelegateDispatchQueue:(dispatch_queue_t) queue;

// By default, it will schedule itself on +[NSRunLoop QC_networkRunLoop] using defaultModes.
//默认情况下，它将使用defaultModes在+ [NSRunLoop QC_networkRunLoop]上进行调度。
- (void)scheduleInRunLoop:(NSRunLoop *)aRunLoop forMode:(NSString *)mode;
- (void)unscheduleFromRunLoop:(NSRunLoop *)aRunLoop forMode:(NSString *)mode;

// QCloudWebSockets are intended for one-time-use only.  Open should be called once and only once.
// QCloudWebSockets仅用于一次使用。打开应该只调用一次。
- (void)open;

- (void)close;
- (void)closeWithCode:(NSInteger)code reason:(NSString *)reason;

// Send a UTF8 String or Data.
- (void)send:(id)data;

// Send Data (can be nil) in a ping message.
//在ping消息中发送数据（可以为nil）。
- (void)sendPing:(NSData *)data;

@end

#pragma mark - QCloudWebSocketDelegate

@protocol QCloudWebSocketDelegate <NSObject>

// message will either be an NSString if the server is using text
// or NSData if the server is using binary.
//如果服务器使用文本，则消息将为NSString
//或NSData（如果服务器使用的是二进制文件）。
- (void)webSocket:(QCloudWebSocket *)webSocket didReceiveMessage:(id)message;

@optional

- (void)webSocketDidOpen:(QCloudWebSocket *)webSocket;
- (void)webSocket:(QCloudWebSocket *)webSocket didFailWithError:(NSError *)error;
- (void)webSocket:(QCloudWebSocket *)webSocket didCloseWithCode:(NSInteger)code reason:(NSString *)reason wasClean:(BOOL)wasClean;
- (void)webSocket:(QCloudWebSocket *)webSocket didReceivePong:(NSData *)pongPayload;

// Return YES to convert messages sent as Text to an NSString. Return NO to skip NSData -> NSString conversion for Text messages. Defaults to YES.
//返回YES，将以文本形式发送的消息转换为NSString。返回NO跳过文本数据的NSData-> NSString转换。默认为是。
- (BOOL)webSocketShouldConvertTextFrameToString:(QCloudWebSocket *)webSocket;

//是否还有输出空间 isAvailable 为NO 弱网情况下需要控制
- (void)webSocket:(QCloudWebSocket *)webSocket outputStreamHasSpaceAvailable:(BOOL)isAvailable;

@end

#pragma mark - NSURLRequest (SRCertificateAdditions)

@interface NSURLRequest (SRCertificateAdditions)

@property (nonatomic, retain, readonly) NSArray *QC_SSLPinnedCertificates;

@end

#pragma mark - NSMutableURLRequest (SRCertificateAdditions)

@interface NSMutableURLRequest (SRCertificateAdditions)

@property (nonatomic, retain) NSArray *QC_SSLPinnedCertificates;

@end

#pragma mark - NSRunLoop (QCloudWebSocket)

@interface NSRunLoop (QCloudWebSocket)

+ (NSRunLoop *)QC_networkRunLoop;

@end


NS_ASSUME_NONNULL_END
