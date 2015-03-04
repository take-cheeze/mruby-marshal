MRuby::Gem::Specification.new('mruby-marshal') do |spec|
  spec.license = 'MIT'
  spec.author = 'take-cheeze'
  spec.summary = 'Marhshal module for mruby'

  spec.cxx.flags.push('-fpermissive')
  
  add_dependency 'mruby-onig-regexp', :github => 'mattn/mruby-onig-regexp'
  add_dependency 'mruby-string-ext', :core => 'mruby-string-ext'
end
