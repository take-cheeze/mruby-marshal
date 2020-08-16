MRuby::Build.new do |conf|
  toolchain :gcc
  enable_test
  enable_bintest
  enable_debug

  conf.cc.flags << '-fsanitize=address,undefined'
  conf.cxx.flags << '-fsanitize=address,undefined'
  conf.linker.flags << '-fsanitize=address,undefined'

  conf.gem "#{MRUBY_ROOT}/.."
  conf.cxx.flags << '-std=c++11'

  conf.gem github: 'take-cheeze/mruby-onig-regexp', branch: '1.2'
end
