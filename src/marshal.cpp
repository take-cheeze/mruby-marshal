#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/variable.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef MRUBY_VERSION
#define mrb_module_get mrb_class_get
#define mrb_args_int int
#define mrb_symlen size_t
#else
#define mrb_args_int mrb_int
#define mrb_symlen mrb_int
#endif

bool operator==(mrb_value const& lhs, mrb_sym const sym) {
  return mrb_symbol(lhs) == sym;
}

bool operator==(mrb_value const& lhs, mrb_value const& rhs) {
  return mrb_cptr(lhs) == mrb_cptr(rhs) and mrb_type(lhs) == mrb_type(rhs);
}

namespace {

enum { MAJOR_VERSION = 4, MINOR_VERSION = 8, };

struct utility {
  utility(mrb_state* M) : M(M) {}
  mrb_state* M;

  RClass* path2class(char const* const path) {
    char const* begin = path;
    char const* p = begin;
    RClass* ret = M->object_class;

    while(true) {
      while(*p and *p != ':') ++p;
      ret = mrb_class_ptr(mrb_mod_cv_get(M, ret, mrb_intern(M, begin, p - begin)));

      if(!(*p)) { break; }

      assert(p[0] == ':' and p[1] == ':');
      p += 2;
      begin = p;
    }
    return ret;
  }
};

struct write_context : public utility {
  write_context(mrb_state* M, mrb_value const& str)
      : utility(M), out(str), symbols(mrb_ary_new(M))
      , objects(mrb_ary_new(M)), regexp_class(mrb_class_get(M, "Regexp")) {}

  mrb_value const out;
  mrb_value const symbols, objects;
  RClass* const regexp_class;

  write_context& byte(uint8_t const v) {
    char const buf[] = {char(v)};
    return mrb_str_buf_cat(M, out, buf, sizeof(buf)), *this;
  }

  write_context& symbol(mrb_sym const sym) {
    size_t const len = RARRAY_LEN(symbols);
    mrb_value const* const begin = RARRAY_PTR(symbols);
    mrb_value const* const ptr = std::find(begin, begin + len, sym);

    if(ptr == begin + len) { // define real symbol if not defined
      mrb_ary_push(M, symbols, mrb_symbol_value(sym));
      return tag<':'>().string(sym);
    }
    else { return tag<';'>().fixnum(ptr - begin); } // write index to symbol table
  }

  write_context& version() {
    RClass* const mod = mrb_module_get(M, "Marshal");
    return
        byte(mrb_fixnum(mrb_mod_cv_get(M, mod, mrb_intern_lit(M, "MAJOR_VERSION")))).
        byte(mrb_fixnum(mrb_mod_cv_get(M, mod, mrb_intern_lit(M, "MINOR_VERSION"))));
  }

  template<char T>
  write_context& tag() {
    char const buf[] = {T};
    return mrb_str_buf_cat(M, out, buf, sizeof(buf)), *this;
  }

  write_context& fixnum(mrb_int const v) {
    if(v == 0) { return byte(0); }
    else if(0 < v and v < 123) { return byte(v + 5); }
    else if(-124 < v and v < 0) { return byte((v - 5) & 0xff); }
    else {
      char buf[sizeof(mrb_int) + 1];
      mrb_int x = v;
      size_t i = 1;
      for(; i <= sizeof(mrb_int); ++i) {
        buf[i] = x & 0xff;
        x = x < 0 ? ~((~x) >> 8) : (x >> 8);
        if(x ==  0) { buf[0] =  i; break; }
        if(x == -1) { buf[0] = -i; break; }
      }
      return mrb_str_buf_cat(M, out, buf, i + 1), *this;
    }
  }

  write_context& string(char const* str, size_t len) {
    fixnum(len);
    return mrb_str_buf_cat(M, out, str, len), *this;
  }
  write_context& string(char const* str)
  { return string(str, std::strlen(str)); }
  write_context& string(mrb_sym const sym) {
    mrb_symlen len;
    char const* const str = mrb_sym2name_len(M, sym, &len);
    return string(str, len);
  }
  write_context& string(mrb_value const& v)
  { return string(RSTRING_PTR(v), RSTRING_LEN(v)); }

  write_context& marshal(mrb_value const& v);

  // returns -1 if not found
  int find_link(mrb_value const& obj) const {
    size_t const len = RARRAY_LEN(objects);
    mrb_value const* const begin = RARRAY_PTR(objects);
    mrb_value const* const ptr = std::find(begin, begin + len, obj);
    return ptr == (begin + len)? -1 : (ptr - begin);
  }

  bool is_struct(mrb_value const& v) const {
    return mrb_class_defined(M, "Struct") and mrb_obj_is_kind_of(M, v, mrb_class_get(M, "Struct"));
  }

  write_context& link(int const l) {
    assert(l != -1);
    return tag<'@'>().fixnum(l);
  }

  write_context& class_symbol(RClass* const v) {
    return symbol(mrb_intern_str(M, mrb_class_path(M, v)));
  }

  write_context& extended(mrb_value const& v, bool const check) {
    if(check) {} // check singleton

    RClass* cls = mrb_class(M, v);

    while(cls->tt == MRB_TT_ICLASS) {
      tag<'e'>().symbol(mrb_intern_cstr(M, mrb_class_name(M, cls->c)));
      cls = cls->super;
    }
    return *this;
  }

  write_context& uclass(mrb_value const& v, RClass* const super) {
    extended(v, true);
    RClass* const real_class = mrb_class_real(mrb_class(M, v));
    if(real_class != super) { tag<'C'>().class_symbol(real_class); }
    return *this;
  }

  template<char Tag>
  write_context& klass(mrb_value const& v, bool const check) {
    // TODO: compat table

    return extended(v, check)
        .tag<Tag>().class_symbol(mrb_class_real(mrb_class(M, v)));
  }
};

write_context& write_context::marshal(mrb_value const& v) {
  if(mrb_nil_p(v)) { return tag<'0'>(); }

  // check for link
  int const link = find_link(v);
  if(link != -1) { return tag<'@'>().fixnum(link); }

  RClass* const cls = mrb_obj_class(M, v);

  // basic types without instance variables
  switch(mrb_vtype(mrb_type(v))) {
    case MRB_TT_FALSE: return tag<'F'>();
    case MRB_TT_TRUE : return tag<'T'>();
    case MRB_TT_FIXNUM: return tag<'i'>().fixnum(mrb_fixnum(v));
    case MRB_TT_SYMBOL: return symbol(mrb_symbol(v));

    default: break;
  }

  mrb_ary_push(M, objects, v);

  // check marshal_dump
  if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "marshal_dump"))) {
    return klass<'U'>(v, false).class_symbol(cls)
        .marshal(mrb_funcall(M, v, "_dump", 1, mrb_nil_value()));
  }
  // check _dump
  if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "_dump"))) {
    // TODO: dump instance variables
    return klass<'u'>(v, false).class_symbol(cls)
        .string(mrb_funcall(M, v, "_dump", 1, mrb_nil_value()));
  }

  mrb_value const iv_keys = mrb_obj_instance_variables(M, v);

  if(mrb_type(v) != MRB_TT_OBJECT and cls != regexp_class and RARRAY_LEN(iv_keys) > 0) { tag<'I'>(); }

  if(cls == regexp_class) {
    uclass(v, regexp_class).tag<'/'>().string(mrb_funcall(M, v, "source", 0));
    if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "options"))) {
      byte(mrb_fixnum(mrb_funcall(M, v, "options", 0)));
    } else { byte(0); } // workaround
    return *this;
  } else if(is_struct(v)) {
    // TODO
    klass<'S'>(v, true);
  } else if(mrb_type(v) == MRB_TT_OBJECT) {
    klass<'o'>(v, true).class_symbol(cls).fixnum(RARRAY_LEN(iv_keys));
    for(int i = 0; i < RARRAY_LEN(iv_keys); ++i) {
      symbol(mrb_symbol(RARRAY_PTR(iv_keys)[i])).marshal(RARRAY_PTR(iv_keys)[i]);
    }
    return *this;
  } else switch(mrb_vtype(mrb_type(v))) {
      case MRB_TT_CLASS : return tag<'c'>().string(mrb_class_path(M, cls));
      case MRB_TT_MODULE: return tag<'m'>().string(mrb_class_path(M, cls));

      case MRB_TT_STRING:
        uclass(v, M->string_class).tag<'"'>().string(v);
        break;

      case MRB_TT_FLOAT: {
        // TODO: make platform independent
        char buf[256];
        sprintf(buf, "%.16g", mrb_float(v));
        tag<'f'>().string(buf);
      } break;

      case MRB_TT_ARRAY: {
        uclass(v, M->array_class).tag<'['>().fixnum(RARRAY_LEN(v));
        for(int i = 0; i < RARRAY_LEN(v); ++i) { marshal(RARRAY_PTR(v)[i]); }
      } break;

      case MRB_TT_HASH: {
        uclass(v, M->hash_class);

        // TODO: check proc default
        mrb_value const default_val = mrb_iv_get(M, v, mrb_intern_lit(M, "ifnone"));
        mrb_nil_p(default_val)? tag<'{'>() : tag<'}'>();

        mrb_value const keys = mrb_hash_keys(M, v);
        fixnum(RARRAY_LEN(keys));
        for(int i = 0; i < RARRAY_LEN(keys); ++i) {
          marshal(RARRAY_PTR(keys)[i]).marshal(mrb_hash_get(M, v, RARRAY_PTR(keys)[i]));
        }

        if(not mrb_nil_p(default_val)) { marshal(default_val); }
      } break;

      case MRB_TT_DATA: {
        if(not mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "_dump_data"))) {
          mrb_raise(M, mrb_class_get(M, "TypeError"), "_dump_data isn't defined'");
        }
        klass<'d'>(v, true).marshal(mrb_funcall(M, v, "_dump_data", 0));
      } break;


      default:
        mrb_raise(M, mrb_class_get(M, "TypeError"), "unsupported type");
        return *this;
    }

  // write instance variables
  if(RARRAY_LEN(iv_keys) > 0) {
    fixnum(RARRAY_LEN(iv_keys));
    RObject* const obj = mrb_obj_ptr(v);
    for(int i = 0; i < RARRAY_LEN(iv_keys); ++i) {
      mrb_sym const key = mrb_symbol(RARRAY_PTR(iv_keys)[i]);
      symbol(key).marshal(mrb_obj_iv_get(M, obj, key));
    }
  }
  return *this;
}

struct read_context : public utility {
  read_context(mrb_state* M, char const* begin, char const* end)
      : utility(M), begin(begin), end(end), current(begin)
      , symbols(mrb_ary_new(M)), objects(mrb_ary_new(M)) {}

  char const* const begin;
  char const* const end;
  char const* current;

  mrb_value const symbols; // symbol table -> array
  mrb_value const objects; // object table -> array

  uint8_t byte() {
    if(current >= end) mrb_raise(M, mrb_class_get(M, "RangeError"), "read_byte error");
    return *(current++);
  }

  void number_too_big() {
  }

  mrb_int fixnum() {
    mrb_int const c = static_cast<signed char>(byte());

    if(c == 0) return 0;
    else if(c > 0) {
      if(4 < c and c < 128) { return c - 5; }
      if(c > int(sizeof(mrb_int))) { number_too_big(); }
      mrb_int ret = 0;
      for(mrb_int i = 0; i < c; ++i) {
        ret |= static_cast<mrb_int>(byte()) << (8*i);
      }
      return ret;
    }
    else {
      if(-129 < c and c < -4) { return c + 5; }
      mrb_int const len = -c;
      if(len > int(sizeof(mrb_int))) { number_too_big(); }
      mrb_int ret = ~0;
      for(mrb_int i = 0; i < len; ++i) {
        ret &= ~(0xff << (8*i));
        ret |= static_cast<mrb_int>(byte()) << (8*i);
      }
      return ret;
    }
  }

  mrb_value string() {
    size_t const len = fixnum();
    if((current + len) > end) {
      mrb_raise(M, mrb_class_get(M, "RangeError"), "string out of range");
    }
    mrb_value const ret = mrb_str_new(M, current, len);
    current += len;
    return ret;
  }

  mrb_sym symbol() {
    switch(byte()) {
      case ':': {
        mrb_sym const ret = mrb_intern_str(M, string());
        mrb_ary_push(M, symbols, mrb_symbol_value(ret));
        return ret;
      }
      case ';': { // get symbol from table
        mrb_int const id = fixnum();
        assert(id < RARRAY_LEN(symbols));
        return mrb_symbol(RARRAY_PTR(symbols)[id]);
      }
      default: assert(false);
    }
  }

  void register_link(mrb_int id, mrb_value const& v) {
    mrb_ary_set(M, objects, id, v);
  }

  mrb_value marshal();
};

mrb_value read_context::marshal() {
  char const tag = byte();
  mrb_int const id = RARRAY_LEN(objects);

  mrb_value ret = mrb_nil_value();

  switch(tag) {
    case '0': return mrb_nil_value  (); // nil
    case 'T': return mrb_true_value (); // true
    case 'F': return mrb_false_value(); // false

    case 'i': // fixnum
      return mrb_fixnum_value(fixnum());

    case 'e': // extended
      break; // TODO

    case ':': // symbol
    case ';': // symbol link
      --current; // restore tag
      return mrb_symbol_value(symbol());

    case 'I': { // instance variable
      ret = marshal();
      size_t const len = fixnum();
      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < len; ++i) {
        mrb_sym const key = symbol();
        mrb_iv_set(M, ret, key, marshal());
        mrb_gc_arena_restore(M, ai);
      }
      return ret;
    }

    case '@': {// link
      mrb_int const id = fixnum();
      if (id >= RARRAY_LEN(objects) or mrb_nil_p(RARRAY_PTR(objects)[id])) {
        mrb_raisef(M, mrb_class_get(M, "ArgumentError"), "Invalid link ID: %S (table size: %S)",
                   mrb_fixnum_value(id), mrb_fixnum_value(RARRAY_LEN(objects)));
      }
      return RARRAY_PTR(objects)[id];
    }

    case 'C': { // sub class instance variable of string, regexp, array, hash
      RClass* const klass = path2class(mrb_sym2name(M, symbol()));
      ret = marshal();
      mrb_basic_ptr(ret)->c = klass; // set class
      return ret;
    }

    case 'u': { // _dump / _load defined class
      mrb_sym const cls = symbol();
      ret = mrb_funcall(M, mrb_obj_value(path2class(mrb_sym2name(M, cls))),
                         "_load", 1, string());
      break;
    }

    case 'U': { // marshal_load / marshal_dump defined class
      mrb_sym const cls = symbol();
      ret = mrb_funcall(M, mrb_obj_value(path2class(mrb_sym2name(M, cls))),
                         "marshal_load", 1, string());
      break;
    }

    case 'o': { // object
      ret = mrb_obj_value(mrb_obj_alloc(
          M, MRB_TT_OBJECT, mrb_class_get(M, mrb_sym2name(M, symbol()))));
      register_link(id, ret);
      size_t const len = fixnum();
      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < len; ++i) {
        mrb_sym const key = symbol();
        mrb_iv_set(M, ret, key, marshal());
        mrb_gc_arena_restore(M, ai);
      }
      break;
    }

    case 'f': { // float
      mrb_value const str = string();
      register_link(id, ret = mrb_float_value(M, std::strtod(RSTRING_PTR(str), NULL)));
      break;
    }

    case '"': register_link(id, ret = string()); break; // string

    case '/': { // regexp
      // TODO: check Regexp class is defined
      mrb_value args[] = { string(), mrb_fixnum_value(byte()) };
      register_link(id, ret = mrb_funcall_argv(M, mrb_obj_value(mrb_class_get(M, "Regexp")),
                                               mrb_intern_lit(M, "new"), 2, args));
      break;
    }

    case '[': { // array
      size_t const len = fixnum();
      register_link(id, ret = mrb_ary_new_capa(M, len));
      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < len; ++i) {
        mrb_ary_push(M, ret, marshal());
        mrb_gc_arena_restore(M, ai);
      }
      break;
    }

    case '{': // hash
    case '}': { // hash with default value
      size_t const len = fixnum();
      register_link(id, ret = mrb_hash_new_capa(M, len));
      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < len; ++i) {
        mrb_value const key = marshal();
        mrb_hash_set(M, ret, key, marshal());
        mrb_gc_arena_restore(M, ai);
      }
      // set default value
      if(tag == '}') { mrb_iv_set(M, ret, mrb_intern_lit(M, "ifnone"), marshal()); }
      break;
    }

    case 'S': { // struct
      mrb_sym const name = symbol();
      char const* const name_cstr = mrb_sym2name(M, name);
      size_t const member_count = fixnum();

      mrb_value const symbols = mrb_ary_new_capa(M, member_count);
      mrb_value const members = mrb_ary_new_capa(M, member_count);

      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < member_count; ++i) {
        mrb_ary_push(M, symbols, mrb_symbol_value(symbol()));
        mrb_ary_push(M, members, marshal());
        mrb_gc_arena_restore(M, ai);
      }

      // TODO: define struct from symbol list if not defined
      // if(not mrb_const_defined(M, mrb_class_get(M, "Struct"), name)) {}

      // TODO: get struct class
      mrb_value const struct_class = mrb_obj_value(path2class(name_cstr));

      // call new of struct
      // TODO: create struct instance before members
       ret = mrb_funcall_argv(
          M, struct_class, mrb_intern_lit(M, "new"), member_count, RARRAY_PTR(members));
       register_link(id, ret);
       break;
    }

    case 'M': // old format class/module
      // check class or module
      register_link(id, ret = mrb_obj_value(path2class(RSTRING_PTR(string()))));
      break;

    case 'c': // class
      // check class
      register_link(id, ret = mrb_obj_value(path2class(RSTRING_PTR(string()))));
      break;

    case 'm': // module
      // check module
      register_link(id, ret = mrb_obj_value(path2class(RSTRING_PTR(string()))));
      break;

    case 'l': // bignum (unsupported)

    default:
      mrb_raise(M, mrb_class_get(M, "TypeError"), "Unsupported type");
      return mrb_nil_value();
  }

  assert(not mrb_nil_p(ret));
  assert(not mrb_nil_p(mrb_ary_ref(M, objects, id)));
  return ret;
}

mrb_value mrb_marshal_dump(mrb_state* M, mrb_value) {
  mrb_value obj;
  mrb_get_args(M, "o", &obj);

  write_context ctx(M, mrb_str_new(M, NULL, 0));
  return ctx.version().marshal(obj).out;
}

mrb_value mrb_marshal_load(mrb_state* M, mrb_value) {
  char* str;
  mrb_args_int len;
  mrb_get_args(M, "s", &str, &len);

  read_context ctx(M, str, str + len);

  uint8_t const major_version = ctx.byte();
  uint8_t const minor_version = ctx.byte();

  assert(major_version == MAJOR_VERSION);
  assert(minor_version == MINOR_VERSION);

  return ctx.marshal();
}

}

extern "C"
void mrb_mruby_marshal_gem_init(mrb_state* M) {
  RClass* const mod = mrb_define_module(M, "Marshal");

  mrb_define_module_function(M, mod, "load", &mrb_marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function(M, mod, "restore", &mrb_marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function(M, mod, "dump", &mrb_marshal_dump, MRB_ARGS_REQ(1));

  mrb_define_const(M, mod, "MAJOR_VERSION", mrb_fixnum_value(MAJOR_VERSION));
  mrb_define_const(M, mod, "MINOR_VERSION", mrb_fixnum_value(MINOR_VERSION));
}


extern "C"
void mrb_mruby_marshal_gem_final(mrb_state*) {}
