MRuby::Build.new do |conf|
  toolchain :gcc
  enable_bintest
  enable_debug

  conf.gem '.'
end
