# 文件作用：固定腾讯普通实时 ASR SDK，显式选择 standard 目录并保留临时 token 鉴权。
Pod::Spec.new do |spec|
  spec.name = 'QCloudRealTime'
  spec.version = '3.1.40'
  spec.summary = 'Tencent Cloud standard real-time ASR SDK'
  spec.homepage = 'https://cloud.tencent.com/document/product/1093/35723'
  spec.author = 'Tencent Cloud'
  spec.license = { :type => 'Copyright', :text => 'Copyright Tencent. All rights reserved.' }
  spec.platform = :ios, '9.0'
  # 分发包与 standard/sdk/sdk_version.h 版本均为 3.1.40；v2 目录不适用于普通引擎。
  spec.source = {
    :http => 'https://sdk-1300466766.cos.ap-shanghai.myqcloud.com/asr_sdk/QCloudAsrSDK_iOS_v3.1.40.zip',
    :sha256 => 'cb58cbf609f73b4a6a7840a09574ba933a3c54a472ab0f441931774dc3b878f6'
  }
  spec.static_framework = true
  spec.vendored_frameworks = 'standard/sdk/QCloudRealTime.xcframework', 'standard/sdk/VoiceCommon.xcframework'
  spec.frameworks = 'AVFoundation', 'AudioToolbox'
  spec.libraries = 'c++'
end
