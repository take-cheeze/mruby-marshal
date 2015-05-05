#ifndef MRUBY_ARRAY_H
#define MRUBY_ARRAY_H

#if defined(__cplusplus)
extern "C" {
#endif

mrb_value mrb_marshal_dump(mrb_state* M, mrb_value);
mrb_value mrb_marshal_load(mrb_state* M, mrb_value);


#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif  /* MRUBY_ARRAY_H */
