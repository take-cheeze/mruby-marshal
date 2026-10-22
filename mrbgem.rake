MRuby::Gem::Specification.new('mruby-marshal') do |spec|
  spec.license = 'MIT'
  spec.author = 'take-cheeze'
  spec.summary = 'Marhshal module for mruby'

  # psp/wio/maix/closed (build_config.rb's single_format_only) exclude mruby-onig-regexp's
  # own top-level conf.gem call (ADR 0098 -- nothing there ever marshals or
  # matches against a real Regexp), but this add_dependency was unconditional
  # and pulled it right back in, defeating that trim (and opening bc2cpp's closed world: its String reopening
  # aliases computed names). marshal.cpp's own utility constructor no longer requires the Regexp class to exist
  # (mrb_class_defined guard) so this is safe now, not just possible.
  add_dependency 'mruby-onig-regexp', github: 'mattn/mruby-onig-regexp' unless %w[psp wio maix closed].include?(build.name)
  add_dependency 'mruby-string-ext', core: 'mruby-string-ext'
  add_dependency 'mruby-struct', core: 'mruby-struct'
  add_dependency 'mruby-metaprog', core: 'mruby-metaprog' if Dir.exist? "#{MRUBY_ROOT}/mrbgems/mruby-metaprog"

  add_test_dependency 'mruby-stringio', github: 'ksss/mruby-stringio'
end
