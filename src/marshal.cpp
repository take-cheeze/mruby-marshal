#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/khash.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/marshal.h>

#if MRUBY_RELEASE_MAJOR >= 3 && MRUBY_RELEASE_MINOR >= 3
extern "C" {
#include <mruby/internal.h>
}
#endif

#include <stdlib.h>
#include <string.h>

#ifndef MRUBY_VERSION
#define mrb_module_get mrb_class_get
#define mrb_args_int int
#define mrb_symlen size_t
#else
#define mrb_args_int mrb_int
#define mrb_symlen mrb_int
#endif

#if MRUBY_RELEASE_MAJOR <= 1 && MRUBY_RELEASE_MINOR <= 2
typedef struct {
  mrb_value v;
  mrb_int n;
} mrb_hash_value;

KHASH_DECLARE(ht, mrb_value, mrb_hash_value, TRUE)
#endif

bool operator==(mrb_value const& lhs, mrb_sym const sym) {
  return mrb_symbol_p(lhs) && mrb_symbol(lhs) == sym;
}

bool operator!=(mrb_value const& lhs, mrb_sym const sym) {
  return !mrb_symbol_p(lhs) || mrb_symbol(lhs) != sym;
}

bool operator==(mrb_value const& lhs, mrb_value const& rhs) {
  return mrb_cptr(lhs) == mrb_cptr(rhs) and mrb_type(lhs) == mrb_type(rhs);
}

bool operator!=(mrb_value const& lhs, mrb_value const& rhs) {
  return mrb_type(lhs) != mrb_type(rhs) or mrb_cptr(lhs) != mrb_cptr(rhs);
}

namespace {

enum { MAJOR_VERSION = 4, MINOR_VERSION = 8, };

struct utility {
  utility(mrb_state* M)
      : M(M), regexp_class(mrb_class_get(M, "Regexp"))
      , symbols(mrb_ary_new(M)), objects(mrb_ary_new(M)) {}
  mrb_state* M;

  RClass* const regexp_class;
  mrb_value const symbols; // symbol table -> array
  mrb_value const objects; // object table -> array

  RClass* path2class(mrb_sym sym) const {
    mrb_int len;
    char const* begin = mrb_sym2name_len(M, sym, &len);
    return path2class(begin, len);
  }

  RClass* path2class(char const* path_begin, mrb_int len) const {
    char const* begin = path_begin;
    char const* p = begin;
    char const* end = begin + len;
    struct RClass* ret = M->object_class;

    while(true) {
      while((p < end and p[0] != ':') or
            ((p + 1) < end and p[1] != ':')) ++p;

      mrb_sym const cls = mrb_intern(M, begin, p - begin);
      if (!mrb_cv_defined(M, mrb_obj_value(ret), cls)) {
        mrb_raisef(M, mrb_class_get(M, "ArgumentError"), "undefined class/module %S",
                   mrb_str_new(M, path_begin, p - path_begin));
      }

      mrb_value const cnst = mrb_cv_get(M, mrb_obj_value(ret), cls);
      if (mrb_type(cnst) != MRB_TT_CLASS &&  mrb_type(cnst) != MRB_TT_MODULE) {
        mrb_raisef(M, mrb_class_get(M, "TypeError"), "%S does not refer to class/module",
                   mrb_str_new(M, path_begin, p - path_begin));
      }
      ret = mrb_class_ptr(cnst);

      if(p >= end) { break; }

      p += 2;
      begin = p;
    }
    return ret;
  }
};

template<class Out>
struct write_context : public utility {
  write_context(mrb_state *M, Out out) : utility(M), out_(out) {}

  typedef Out out_type;
  out_type out_;

  write_context& symbol(mrb_sym const sym) {
    size_t const len = RARRAY_LEN(symbols);
    mrb_value const* const begin = RARRAY_PTR(symbols);
    mrb_value const* ptr = begin;
    for (; ptr < (begin + len) && *ptr != sym; ++ptr);

    if(ptr == begin + len) { // define real symbol if not defined
      mrb_ary_push(M, symbols, mrb_symbol_value(sym));
      return tag(':').string(sym);
    }
    else { return tag(';').fixnum(ptr - begin); } // write index to symbol table
  }

  write_context& version() {
    RClass* const mod = mrb_module_get(M, "Marshal");
    out_.byte(mrb_fixnum(mrb_cv_get(M, mrb_obj_value(mod), mrb_intern_lit(M, "MAJOR_VERSION"))));
    out_.byte(mrb_fixnum(mrb_cv_get(M, mrb_obj_value(mod), mrb_intern_lit(M, "MINOR_VERSION"))));
    return *this;
  }

  write_context& tag(char t) { out_.byte(t); return *this; }

  write_context& fixnum(mrb_int const v) {
    if(v == 0) { out_.byte(0); return *this; }
    else if(0 < v and v < 123) { out_.byte(v + 5); return *this; }
    else if(-124 < v and v < 0) { out_.byte((v - 5) & 0xff); return *this; }
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
      out_.byte_array(buf, i + 1);
      return *this;
    }
  }

  write_context& string(char const* str, size_t len) {
    fixnum(len);
    out_.byte_array(str, len);
    return *this;
  }
  write_context& string(char const* str)
  { return string(str, strlen(str)); }
  write_context& string(mrb_sym const sym) {
    mrb_symlen len;
    char const* const str = mrb_sym2name_len(M, sym, &len);
    return string(str, len);
  }
  write_context& string(mrb_value const& v)
  { return string(RSTRING_PTR(v), RSTRING_LEN(v)); }

  write_context& marshal(mrb_value const& v, mrb_int limit = -1);

  bool is_struct(mrb_value const& v) const {
    return mrb_class_defined(M, "Struct") and mrb_obj_is_kind_of(M, v, mrb_class_get(M, "Struct"));
  }

  write_context& link(int const l) {
    mrb_assert(l != -1);
    return tag('@').fixnum(l);
  }

  write_context& class_symbol(RClass* const v) {
    return symbol(mrb_intern_str(M, mrb_class_path(M, v)));
  }

  write_context& extended(mrb_value const& v, bool const check) {
    if(check) {} // check singleton

    RClass* cls = mrb_class(M, v);

    while(cls->tt == MRB_TT_ICLASS) {
      tag('e').symbol(mrb_intern_cstr(M, mrb_class_name(M, cls->c)));
      cls = cls->super;
    }
    return *this;
  }

  write_context& uclass(mrb_value const& v, RClass* const super) {
    extended(v, true);
    RClass* const real_class = mrb_class_real(mrb_class(M, v));
    if(real_class != super) { tag('C').class_symbol(real_class); }
    return *this;
  }

  write_context& klass(char t, mrb_value const& v, bool const check) {
    // TODO: compat table

    return extended(v, check).tag(t).class_symbol(mrb_class_real(mrb_class(M, v)));
  }

 private:
  struct hash_marshal_meta {
    write_context& ctx;
    mrb_int limit;
    hash_marshal_meta(write_context& ctx, mrb_int limit)
      : ctx(ctx), limit(limit) {};
  };

  static int marshal_hash_each(mrb_state *mrb, mrb_value key, mrb_value val, void *meta_) {
    hash_marshal_meta *meta = (hash_marshal_meta*)meta_;
    meta->ctx.marshal(key, meta->limit).marshal(val, meta->limit);
    return 0;
  }
};

template<class Out>
write_context<Out>& write_context<Out>::marshal(mrb_value const& v, mrb_int limit) {
  if (limit == 0) { mrb_raise(M, mrb_class_get(M, "ArgumentError"), "depth limit"); }
  --limit;

  if(mrb_nil_p(v)) { return tag('0'); }

  // check for link
  {
    mrb_value const *b = RARRAY_PTR(objects);
    mrb_value const *l = b;
    mrb_value const *e = b + RARRAY_LEN(objects);
    for (; l < e && !mrb_obj_equal(M, *l, v); ++l);
    if (l != e) return tag('@').fixnum(l - b);
  }

  RClass* const cls = mrb_obj_class(M, v);

  // basic types without instance variables
  switch(mrb_vtype(mrb_type(v))) {
    case MRB_TT_FALSE: return tag('F');
    case MRB_TT_TRUE : return tag('T');
    case MRB_TT_FIXNUM: return tag('i').fixnum(mrb_fixnum(v));
    case MRB_TT_SYMBOL: return symbol(mrb_symbol(v));

    default: break;
  }

  mrb_ary_push(M, objects, v);

  // check marshal_dump
  if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "marshal_dump"))) {
    return klass('U', v, false).marshal(mrb_funcall(M, v, "marshal_dump", 1, mrb_nil_value()), limit);
  }
  // check _dump
  if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "_dump"))) {
    // TODO: dump instance variables
    return klass('u', v, false).string(mrb_funcall(M, v, "_dump", 1, mrb_nil_value()));
  }

  mrb_value const iv_keys = mrb_funcall(M, v, "instance_variables", 0);
  mrb_funcall(M, iv_keys, "sort!", 0);

  if(mrb_type(v) != MRB_TT_OBJECT and cls != regexp_class and RARRAY_LEN(iv_keys) > 0) { tag('I'); }

  if(cls == regexp_class) {
    uclass(v, regexp_class).tag('/').string(mrb_funcall(M, v, "source", 0));
    if(mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "options"))) {
      out_.byte(mrb_fixnum(mrb_funcall(M, v, "options", 0)));
    } else { out_.byte(0); } // workaround
    return *this;
  } else if(is_struct(v)) {
    mrb_value const members = mrb_iv_get(M, mrb_obj_value(mrb_class(M, v)), mrb_intern_lit(M, "__members__"));
    klass('S', v, true).fixnum(RARRAY_LEN(members));
    for (mrb_int i = 0; i < RARRAY_LEN(members); ++i) {
      mrb_check_type(M, RARRAY_PTR(members)[i], MRB_TT_SYMBOL);
      symbol(mrb_symbol(RARRAY_PTR(members)[i])).marshal(RARRAY_PTR(v)[i], limit);
    }
  } else if(mrb_type(v) == MRB_TT_OBJECT) {
    klass('o', v, true).fixnum(RARRAY_LEN(iv_keys));
    for(int i = 0; i < RARRAY_LEN(iv_keys); ++i) {
      symbol(mrb_symbol(RARRAY_PTR(iv_keys)[i]))
          .marshal(mrb_iv_get(M, v, mrb_symbol(RARRAY_PTR(iv_keys)[i])), limit);
    }
    return *this;
  } else switch(mrb_vtype(mrb_type(v))) {
      case MRB_TT_CLASS : return tag('c').string(mrb_class_path(M, cls));
      case MRB_TT_MODULE: return tag('m').string(mrb_class_path(M, cls));

      case MRB_TT_STRING:
        uclass(v, M->string_class).tag('"').string(v);
        break;

      case MRB_TT_FLOAT: {
        // TODO: make platform independent
        char buf[256];
        sprintf(buf, "%.16g", mrb_float(v));
        tag('f').string(buf);
      } break;

      case MRB_TT_ARRAY: {
        uclass(v, M->array_class).tag('[').fixnum(RARRAY_LEN(v));
        for(int i = 0; i < RARRAY_LEN(v); ++i) { marshal(RARRAY_PTR(v)[i], limit); }
      } break;

      case MRB_TT_HASH: {
        uclass(v, M->hash_class);

        // TODO: check proc default
        mrb_value const default_val = mrb_iv_get(M, v, mrb_intern_lit(M, "ifnone"));
        tag(mrb_nil_p(default_val)? '{' : '}');

#if MRUBY_RELEASE_MAJOR >= 2 && MRUBY_RELEASE_MINOR >= 1
        fixnum(mrb_hash_size(M, v));
        auto meta = hash_marshal_meta(*this, limit);
        mrb_hash_foreach(M, RHASH(v), &marshal_hash_each, &meta);
#elif MRUBY_RELEASE_MAJOR >= 2 && MRUBY_RELEASE_MINOR >= 0
        mrb_value const keys = mrb_hash_keys(M, v);
        mrb_funcall(M, keys, "sort!", 0);

        fixnum(RARRAY_LEN(keys));
        for(mrb_int i = 0; i < RARRAY_LEN(keys); ++i) {
          mrb_value const k = RARRAY_PTR(keys)[i];
          marshal(k, limit).marshal(mrb_hash_get(M, v, k), limit);
        }
#else
        khash_t(ht) const * const h = RHASH_TBL(v);

        fixnum(kh_size(h));
        for(khiter_t k = kh_begin(h); k != kh_end(h); ++k) {
          if (!kh_exist(h, k)) { continue; }
          marshal(kh_key(h, k), limit).marshal(kh_value(h, k).v, limit);
        }
#endif

        if(not mrb_nil_p(default_val)) { marshal(default_val, limit); }
      } break;

      case MRB_TT_DATA: {
        if(not mrb_obj_respond_to(M, cls, mrb_intern_lit(M, "_dump_data"))) {
          mrb_raise(M, mrb_class_get(M, "TypeError"), "_dump_data isn't defined'");
        }
        klass('d', v, true).marshal(mrb_funcall(M, v, "_dump_data", 0), limit);
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
      symbol(key).marshal(mrb_obj_iv_get(M, obj, key), limit);
    }
  }
  return *this;
}

struct string_out {
  string_out(mrb_state* M, mrb_value const& str) : M(M), out(str) {}

  mrb_state * const M;
  mrb_value out;

  void byte(uint8_t const v) {
    char const buf[] = {static_cast<char>(v)};
    mrb_str_buf_cat(M, out, buf, 1);
  }

  void byte_array(char const *buf, size_t len) {
    mrb_str_buf_cat(M, out, buf, len);
  }
};

struct io_out {
  io_out(mrb_state* M, mrb_value const& out)
      : M(M), out(out), buf(mrb_str_new(M, NULL, 0)) {}

  mrb_state * const M;
  mrb_value const out, buf;

  void byte(uint8_t const v) {
    mrb_str_resize(M, buf, 1);
    RSTRING_PTR(buf)[0] = v;
    mrb_funcall(M, out, "write", 1, buf);
  }

  void byte_array(char const *ary, size_t len) {
    mrb_str_resize(M, buf, len);
    memcpy(RSTRING_PTR(buf), ary, len);
    mrb_funcall(M, out, "write", 1, buf);
  }
};

template<class In>
struct read_context : public utility {
  typedef In in_type;
  read_context(mrb_state* M, in_type in) : utility(M), in_(in) {}

  in_type in_;

  read_context& version() {
    uint8_t const major_version = in_.byte();
    uint8_t const minor_version = in_.byte();

    if (major_version != MAJOR_VERSION ||
        minor_version != MINOR_VERSION) {
      mrb_raisef(M, mrb_class_get(M, "TypeError"), "invalid marshal version: %S.%S (expected: %S.%S)",
                 mrb_fixnum_value(major_version), mrb_fixnum_value(minor_version),
                 mrb_fixnum_value(MAJOR_VERSION), mrb_fixnum_value(MINOR_VERSION));
    }

    return *this;
  }

  void number_too_big() {
    mrb_assert(false);
  }

  mrb_int fixnum() {
    mrb_int const c = static_cast<signed char>(in_.byte());

    if(c == 0) return 0;
    else if(c > 0) {
      if(4 < c and c < 128) { return c - 5; }
      if(c > int(sizeof(mrb_int))) { number_too_big(); }
      mrb_int ret = 0;
      for(mrb_int i = 0; i < c; ++i) {
        ret |= static_cast<mrb_int>(in_.byte()) << (8*i);
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
        ret |= static_cast<mrb_int>(in_.byte()) << (8*i);
      }
      return ret;
    }
  }

  mrb_value string() { return in_.byte_array(fixnum()); }

  mrb_sym symbol() {
    switch(in_.byte()) {
      case ':': {
        mrb_sym const ret = mrb_intern_str(M, string());
        mrb_ary_push(M, symbols, mrb_symbol_value(ret));
        return ret;
      }
      case ';': { // get symbol from table
        mrb_int const id = fixnum();
        mrb_assert(id < RARRAY_LEN(symbols));
        return mrb_symbol(RARRAY_PTR(symbols)[id]);
      }
      default:
        mrb_assert(false);
        return 0;
    }
  }

  void register_link(mrb_int id, mrb_value const& v) {
    mrb_ary_set(M, objects, id, v);
  }

  mrb_value marshal();
};

template<class In>
mrb_value read_context<In>::marshal() {
  char const tag = in_.byte();
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
      in_.restore_byte(tag); // restore tag
      return mrb_symbol_value(symbol());

    case 'I': { // instance variable
      ret = marshal();
      size_t const len = fixnum();
      int const ai = mrb_gc_arena_save(M);
      for(size_t i = 0; i < len; ++i) {
        mrb_sym const key = symbol();
        mrb_int key_len;
        char const* sym = mrb_sym2name_len(M, key, &key_len);

        if (key_len == 1 and sym[0] == 'E') {
          marshal(); // TODO: store ignored encoding
        } else {
          mrb_iv_set(M, ret, key, marshal());
        }
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
      RClass* const klass = path2class(symbol());
      ret = marshal();
      mrb_basic_ptr(ret)->c = klass; // set class
      return ret;
    }

    case 'u': { // _dump / _load defined class
      mrb_sym const cls = symbol();
      ret = mrb_funcall(M, mrb_obj_value(path2class(cls)),
                         "_load", 1, string());
      register_link(id, ret);
      break;
    }

    case 'U': { // marshal_load / marshal_dump defined class
      mrb_sym const cls = symbol();
      ret = mrb_funcall(M, mrb_obj_value(path2class(cls)),
                         "marshal_load", 1, marshal());
      register_link(id, ret);
      break;
    }

    case 'o': { // object
      ret = mrb_obj_value(mrb_obj_alloc(M, MRB_TT_OBJECT, path2class(symbol())));
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
      register_link(id, ret = mrb_float_value(M, strtod(RSTRING_PTR(str), NULL)));
      break;
    }

    case '"': register_link(id, ret = string()); break; // string

    case '/': { // regexp
      // TODO: check Regexp class is defined
      mrb_value args[] = { string(), mrb_fixnum_value(in_.byte()) };
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
      mrb_sym const cls_name = symbol();
      struct RClass *cls = path2class(cls_name);
      mrb_int const member_count = fixnum();

      mrb_value const struct_symbols = mrb_iv_get(M, mrb_obj_value(cls), mrb_intern_lit(M, "__members__"));
      mrb_check_type(M, struct_symbols, MRB_TT_ARRAY);
      if (member_count != RARRAY_LEN(struct_symbols)) {
        mrb_raisef(M, mrb_class_get(M, "TypeError"),
                   "struct %S not compatible (struct size differs)", mrb_symbol_value(cls_name));
      }

      mrb_value const symbols = mrb_ary_new_capa(M, member_count);
      mrb_value const values = mrb_ary_new_capa(M, member_count);

      int const ai = mrb_gc_arena_save(M);
      for (mrb_int i = 0; i < member_count; ++i) {
        mrb_ary_push(M, symbols, mrb_symbol_value(symbol()));
        mrb_ary_push(M, values, marshal());
        mrb_gc_arena_restore(M, ai);
      }

      for (mrb_int i = 0; i < member_count; ++i) {
        mrb_value src_sym = mrb_ary_ref(M, symbols, i);
        mrb_value dst_sym = mrb_ary_ref(M, struct_symbols, i);
        if (not mrb_obj_eq(M, src_sym, dst_sym)) {
          mrb_raisef(M, mrb_class_get(M, "TypeError"), "struct %S not compatible (:%S for :%S)",
                     mrb_symbol_value(cls_name), src_sym, dst_sym);
        }
      }

      ret = mrb_funcall_argv(M, mrb_obj_value(cls), mrb_intern_lit(M, "new"), member_count, RARRAY_PTR(values));
      register_link(id, ret);
      break;
    }

    case 'M': // old format class/module
    case 'c': // class
    case 'm': {// module
      // check class or module
      // check class
      // check module
      mrb_value str = string();
      register_link(id, ret = mrb_obj_value(path2class(RSTRING_PTR(str), RSTRING_LEN(str))));
      break;
    }

    case 'l': // bignum (unsupported)

    default:
      mrb_raise(M, mrb_class_get(M, "TypeError"), "Unsupported type");
      return mrb_nil_value();
  }

  mrb_assert(not mrb_nil_p(ret));
  mrb_assert(not mrb_nil_p(mrb_ary_ref(M, objects, id)));
  return ret;
}

struct string_in {
  string_in(mrb_state* M, char const* begin, size_t len)
      : M(M), begin(begin), end(begin + len), current(begin) {}

  mrb_state * const M;
  char const* const begin;
  char const* const end;
  char const* current;

  uint8_t byte() {
    if(current >= end) mrb_raise(M, mrb_class_get(M, "RangeError"), "read_byte error");
    return *(current++);
  }

  void restore_byte(char) { --current; }

  mrb_value byte_array(size_t len) {
    if((current + len) > end) {
      mrb_raise(M, mrb_class_get(M, "RangeError"), "string out of range");
    }
    mrb_value const ret = mrb_str_new(M, current, len);
    current += len;
    return ret;
  }
};

struct io_in {
  io_in(mrb_state* M, mrb_value io)
      : M(M), io(io), buf(mrb_str_new(M, NULL, 0)) {}

  mrb_state * const M;
  mrb_value const io;
  mrb_value const buf;

  uint8_t byte() {
    mrb_value const buf = mrb_funcall(M, io, "getc", 0);
    return RSTRING_PTR(buf)[0];
  }

  void restore_byte(char c) {
    mrb_funcall(M, io, "ungetc", 1, mrb_str_new(M, &c, 1));
  }

  mrb_value byte_array(size_t len) {
    mrb_value const ret = mrb_funcall(M, io, "read", 1, mrb_fixnum_value(len));
    if(static_cast<size_t>(RSTRING_LEN(ret)) < len) {
      mrb_raise(M, mrb_class_get(M, "RangeError"), "string out of range");
    }
    return ret;
  }
};

mrb_value marshal_dump(mrb_state* M, mrb_value) {
  mrb_value obj, io = mrb_nil_value();
  mrb_int limit = -1;
  mrb_int const arg_count = mrb_get_args(M, "o|oi", &obj, &io, &limit);

  if (arg_count == 2 && mrb_fixnum_p(io)) {
    limit = mrb_fixnum(io);
    io = mrb_nil_value();
  }

  if (mrb_nil_p(io)) {
    mrb_value const str = mrb_str_new(M, NULL, 0);
    write_context<string_out>(M, string_out(M, str)).version().marshal(obj, limit);
    return str;
  } else {
    write_context<io_out>(M, io_out(M, io)).version().marshal(obj, limit);
    return io;
  }
}

mrb_value marshal_load(mrb_state* M, mrb_value) {
  mrb_value obj;
  mrb_get_args(M, "o", &obj);

  return mrb_string_p(obj)?
      read_context<string_in>(M, string_in(M, RSTRING_PTR(obj), RSTRING_LEN(obj))).version().marshal():
      read_context<io_in>(M, io_in(M, obj)).version().marshal();
}

}

extern "C" {

mrb_value mrb_marshal_dump(mrb_state* M, mrb_value obj, mrb_value io) {
  if (mrb_nil_p(io)) {
    mrb_value const str = mrb_str_new(M, NULL, 0);
    write_context<string_out>(M, string_out(M, str)).version().marshal(obj);
    return str;
  } else {
    write_context<io_out>(M, io_out(M, io)).version().marshal(obj);
    return io;
  }
}

mrb_value mrb_marshal_load(mrb_state* M, mrb_value obj) {
  return mrb_string_p(obj)?
      read_context<string_in>(M, string_in(M, RSTRING_PTR(obj), RSTRING_LEN(obj))).version().marshal():
      read_context<io_in>(M, io_in(M, obj)).version().marshal();
}

void mrb_mruby_marshal_gem_init(mrb_state* M) {
  RClass* const mod = mrb_define_module(M, "Marshal");

  mrb_define_module_function(M, mod, "load", &marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function(M, mod, "restore", &marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function(M, mod, "dump", &marshal_dump, MRB_ARGS_REQ(1));

  mrb_define_const(M, mod, "MAJOR_VERSION", mrb_fixnum_value(MAJOR_VERSION));
  mrb_define_const(M, mod, "MINOR_VERSION", mrb_fixnum_value(MINOR_VERSION));
}

void mrb_mruby_marshal_gem_final(mrb_state*) {}

}
