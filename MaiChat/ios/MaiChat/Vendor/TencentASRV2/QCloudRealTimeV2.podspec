# 文件作用：固定腾讯官方 ASR V2 二进制来源，仅引入 v2 目录，防止同名 standard SDK 混入构建。
Pod::Spec.new do |spec|
  spec.name = 'QCloudRealTimeV2'
  spec.version = '2.0.1'
  spec.summary = 'Tencent Cloud real-time ASR V2 SDK'
  spec.homepage = 'https://cloud.tencent.com/document/product/1093/133355'
  spec.author = 'Tencent Cloud'
  spec.license = { :type => 'Copyright', :text => 'Copyright Tencent. All rights reserved.' }
  spec.platform = :ios, '9.0'
  # 分发包版本为 3.1.40，v2/sdk/sdk_version.h 内的 SDK 版本为 v2.0.1。
  spec.source = {
    :http => 'https://sdk-1300466766.cos.ap-shanghai.myqcloud.com/asr_sdk/QCloudAsrSDK_iOS_v3.1.40.zip',
    :sha256 => 'cb58cbf609f73b4a6a7840a09574ba933a3c54a472ab0f441931774dc3b878f6'
  }
  spec.static_framework = true
  spec.vendored_frameworks = 'v2/sdk/QCloudRealTime.xcframework', 'v2/sdk/VoiceCommon.xcframework'
  spec.frameworks = 'AVFoundation', 'AudioToolbox'
  spec.libraries = 'c++'
end
