#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint system_integrator.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'system_integrator'
  s.version          = '0.1.0'
  s.summary          = 'Hybrid desktop system integration (FFI): files, storage, power, startup, tray.'
  s.description      = <<-DESC
Dart core (Process.run + FFI) plus native tray (NSStatusItem) in the same FFI library.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  # Classes/system_integrator_tray.m (AppKit / NSStatusItem) needs Cocoa + ARC.
  s.frameworks = 'Cocoa'
  s.requires_arc = true

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'system_integrator_privacy' => ['system_integrator/Sources/system_integrator/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.14'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
