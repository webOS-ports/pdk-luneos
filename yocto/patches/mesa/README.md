# Mesa patches

These are not part of the `meta-pdk` layer. Mesa lives in
`meta-webos-ports/meta-luneos-backports-6.0`, which this project does not own,
so the patches are kept here for version control and applied by copying them
next to that recipe:

```sh
cp 0001-*.patch  <tree>/meta-webos-ports/meta-luneos-backports-6.0/recipes-graphics/mesa/mesa/
# then add the filename to SRC_URI in mesa_26.2.0.bb
```

## 0001-gallivm-check-ExecutionEngine-create-for-NULL-before-.patch

`lp_build_create_jit_compiler_for_module()` dereferences the `ExecutionEngine`
returned by `builder.create()` and only tests it for NULL eight lines later, so
a failed MCJIT creation segfaults instead of taking the error path written
immediately below it.

Found while chasing why llvmpipe crashes under `qemu-arm`. It does **not** make
llvmpipe work — MCJIT creation genuinely fails for the emulated target, which is
a separate LLVM problem — but it turns the crash into the intended error return.

Verified: with the patch the fault moves from `lp_bld_misc.cpp:635` to
`LLVMGetExecutionEngineTargetMachine` with a NULL engine, called from
`gallivm_compile_module()` at `lp_bld_init.c:441`. That is a **second** missing
check further down the same failure path; fixing the whole chain means auditing
gallivm's error propagation and is not attempted here.

Worth sending upstream: the first one is a plain use-before-check that affects
anyone whose MCJIT creation fails, not only emulated ARM.

## 0002-gallivm-handle-a-failed-execution-engine-instead-of-a.patch

`gallivm_compile_module()` reacted to `init_gallivm_engine()` failing with
`assert(0)`, and `gallivm_jit_function()` guarded the engine with asserts. Mesa
builds with `NDEBUG`, so all of them are compiled out and the failure was not
handled at all.

With both patches applied the failure finally prints its own cause instead of
crashing:

```
Unable to find target for this triple (no targets are registered)
gallivm: failed to create an LLVM execution engine; cannot JIT shaders
```

which is what led to the real bug — see `llvm-native-target.md`.
