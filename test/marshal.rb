def strip_version(str)
  raise 'version error' unless
    str.length > 2 and str.getbyte(0) == 4 and str.getbyte(1) == 8
  str[2, str.length - 2]
end

def dump(v) strip_version Marshal.dump v end
def load(v) Marshal.load("\x04\x08" + v) end

def check_load_dump(obj, data)
  assert_equal data, dump(obj)
  assert_equal obj, load(data)
end

assert('check marshal dump version') {
  Marshal.dump(nil) == "\x04\x080"
}

assert('Marshal.restore') {
  Marshal.restore("\x04\x080") == nil
}

assert('marshal true' ) { check_load_dump true , 'T' }
assert('marshal false') { check_load_dump false, 'F' }
assert('marshal nil') { check_load_dump nil, '0' }

assert('marshal symbol') { check_load_dump :hogehoge, ":\x0dhogehoge" }

assert('marshal fixnum') {
  result = true;
  {
    0 => "i\x00",
    1 => "i\x06",
    -1 => "i\xfa",
    124 => "i\x01\x7c",
    256 => "i\x02\x00\x01",
    -125 => "i\xff\x83",
    -254 => "i\xff\x02",
    -255 => "i\xff\x01",
    -256 => "i\xff\x00",
    -257 => "i\xfe\xff\xfe",
  }.each { |k,v| result = result and check_load_dump(k, v) }
}

assert("marshal string") {
  check_load_dump "hogehoge", "\"\x0dhogehoge"
}

class StringSub < String
end

class HashSubIV < Hash
  def initialize(obj)
    super obj
    @val = "foo"
    self["val"] = nil
  end
end

assert('marshal subclass of string') {
  check_load_dump StringSub.new('foo'), "C:\x0eStringSub\"\x08foo"
}
assert('marshal subclass of hash with instance variable') {
  check_load_dump(HashSubIV.new('foo'),
                  "IC:\x0eHashSubIV}\x06\"\x08val0\"\x08foo\x06:\x09@val\"\x08foo")
}

assert("marshal regexp") {
  check_load_dump(/hogehoge/, "/\x0dhogehoge\x00")
} if Object.const_defined? :Regexp

assert('marshal array') {
  check_load_dump ["hogehoge", :hogehoge], "[\x07\"\x0dhogehoge:\x0dhogehoge"
}

assert('marshal hash') {
  check_load_dump({"hogehoge" => :hogehoge}, "{\x06\"\x0dhogehoge:\x0dhogehoge")
}

assert('marshal hash with default') {
  h = Hash.new true
  h["hoo"] = "boo"
  check_load_dump h, "}\x06\"\x08hoo\"\x08booT"
}

assert 'marshal float' do
  check_load_dump 1.0, "f\x061"
end

assert 'marshal link' do
  check_load_dump({ :a => [1.0, 2, 3, 2], :b => [4, 5, 1.0, 2, 2] },
                  "{\a:\x06a[\tf\x061i\ai\bi\a:\x06b[\ni\ti\n@\ai\ai\a")
end
