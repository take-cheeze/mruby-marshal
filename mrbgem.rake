MRuby::Gem::Specification.new('mruby-marshal') do |spec|
  spec.license = 'MIT'
  spec.author = 'take-cheeze'
  spec.summary = 'Marhshal module for mruby'

  add_dependency 'mruby-onig-regexp', :github => 'mattn/mruby-onig-regexp'
  add_dependency 'mruby-string-ext', :core => 'mruby-string-ext'
  add_dependency 'mruby-struct', :core => 'mruby-struct'

  add_test_dependency 'mruby-io', github: 'iij/mruby-io'
end
