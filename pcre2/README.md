# msgspec-pcre2

Optional native PCRE2 backend for msgspec. Install through `msgspec[pcre2]`.
The distribution owns only the `_msgspec_pcre2` extension and its metadata;
it does not replace files in `msgspec`.

PCRE2 10.49 and SLJIT sources and licenses are bundled. No system PCRE2 library
is required. JIT is optional, with an interpreter fallback. A source installation
requires a C compiler and Python development headers.

Build standalone distributions with `python -m build pcre2` from the repository
root. The backend artifacts use their own version and PyPI publisher. Publish
the pinned backend version before releasing a `msgspec` version that references
it. Backend CI artifacts are separate from the core release upload.
