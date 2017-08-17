#include <mruby.h>
#include <mruby/marshal.h>

static mrb_value marshal_load(mrb_state *M, mrb_value self) {
  mrb_value o;
  mrb_get_args(M, "o", &o);
  return mrb_marshal_load(M, o);
}

static mrb_value marshal_dump(mrb_state *M, mrb_value self) {
  mrb_value o;
  mrb_get_args(M, "o", &o);
  return mrb_marshal_dump(M, o, mrb_nil_value());
}

void mrb_mruby_marshal_gem_test(mrb_state *M) {
  struct RClass *cls = mrb_module_get(M, "Marshal");
  mrb_define_module_function(M, cls, "mrb_marshal_load", marshal_load, MRB_ARGS_REQ(1));
  mrb_define_module_function(M, cls, "mrb_marshal_dump", marshal_dump, MRB_ARGS_REQ(1));
}
