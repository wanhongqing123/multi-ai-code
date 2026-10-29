# 文件作用：固定阿里云官方 NUI 云端 SDK，只引入 Release 的平台分离 XCFramework。
Pod::Spec.new do |spec|
  spec.name = 'AliyunNUI'
  spec.version = '2.7.6'
  spec.summary = 'Alibaba Cloud NLS real-time speech SDK (01B)'
  spec.homepage = 'https://help.aliyun.com/zh/isi/user-guide/nui-sdk-for-ios'
  spec.author = 'Alibaba Cloud'
  spec.platform = :ios, '12.0'
  spec.source = {
    :http => 'https://help-static-aliyun-doc.aliyuncs.com/file-manage-files/zh-CN/20261027/slwtfu/V2.7.6-01B-20261026_iOS_TLS.tar.gz',
    :sha256 => 'd26a8276e386abf44d569f3cd5480d4abaf7de6aebb30a4e5eb16c9389f87207'
  }
  spec.vendored_frameworks = 'Release/nuisdk.xcframework'
  spec.frameworks = 'AVFoundation', 'AudioToolbox'
  spec.libraries = 'c++', 'iconv'
end
