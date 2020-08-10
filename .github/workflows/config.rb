MRuby::Build.new do |conf|
  toolchain :gcc
  enable_test
  enable_bintest
  enable_debug

  conf.gem '.'
  conf.cxx.flags << '-std=c++11'
end
