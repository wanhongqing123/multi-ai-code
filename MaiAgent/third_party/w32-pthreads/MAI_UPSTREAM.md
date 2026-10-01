# Windows pthread dependency for OBS Graphics

These files were copied from `deps/w32-pthreads` in the local OBS Studio source
at commit `3975a181157dce888984191bfe0a04ee93f61590`. Only the files reached
through the library's local `#include` graph, plus its `README`, `COPYING`, and
`COPYING.LIB`, are included. The source is licensed under LGPL-2.1 as described
in those upstream files.

The dependency is built only for the Windows D3D11 Graphics runtime. It is
kept separate from the FFplay SDL compatibility layer.
