import gc
import importlib
import json
import os
import re
import subprocess
import sys
import sysconfig
import textwrap
import warnings
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier
from typing import Annotated

import pytest

import msgspec


@pytest.fixture(scope="module")
def pcre2_available():
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        available = False
    else:
        try:
            provider = importlib.import_module("pcre2._cy")
        except ModuleNotFoundError as exc:
            if exc.name != "pcre2":
                raise
            available = False
        else:
            available = hasattr(provider, "_MATCHER_API_v1")
    expected = os.environ.get("MSGSPEC_TEST_PCRE2")
    if expected is not None:
        assert expected in ("0", "1")
        assert available == (expected == "1")
    return available


@pytest.fixture
def native(pcre2_available):
    if not pcre2_available:
        pytest.skip("Optional pcre2 matcher API unavailable")


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Subprocesses unavailable"
)
def test_backend_missing_and_broken_imports():
    code = textwrap.dedent(
        r"""
        import importlib.abc
        import importlib.machinery
        import sys
        import sysconfig
        from typing import Annotated
        import msgspec

        class BrokenBackend(importlib.abc.MetaPathFinder, importlib.abc.Loader):
            error = "pcre2"
            failure = None
            calls = 0

            def find_spec(self, fullname, path=None, target=None):
                if fullname == "pcre2":
                    self.calls += 1
                    return importlib.machinery.ModuleSpec(fullname, self)

            def create_module(self, spec):
                if self.failure is not None:
                    raise self.failure
                raise ModuleNotFoundError("missing", name=self.error)

            def exec_module(self, module):
                pass

        finder = BrokenBackend()
        sys.meta_path.insert(0, finder)
        sys.modules.pop("pcre2._cy", None)
        sys.modules.pop("pcre2", None)
        typ = Annotated[str, msgspec.Meta(pattern="^a$")]
        assert msgspec.json.decode(b'"a"', type=typ) == "a"
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            assert finder.calls == 0
            print("no SDK import")
            sys.exit(0)
        assert finder.calls == 1
        finder.error = "missing_backend_dependency"
        try:
            msgspec.Meta(pattern="^a$")
        except ModuleNotFoundError as exc:
            assert exc.name == finder.error
        else:
            raise AssertionError("Broken backend hidden")
        finder.failure = RuntimeError("SDK initialization failure")
        try:
            msgspec.Meta(pattern="^a$")
        except RuntimeError as exc:
            assert exc is finder.failure
        else:
            raise AssertionError("SDK initialization error hidden")
        calls = finder.calls
        msgspec.Meta(pattern=r"\w+")
        assert finder.calls == calls
        """
    )
    subprocess.run([sys.executable, "-c", code], check=True, timeout=30)


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Subprocesses unavailable"
)
def test_backend_module_removal_preserves_matcher(native):
    code = textwrap.dedent(
        """
        import gc
        import importlib
        import sys
        from typing import Annotated
        import msgspec

        typ = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2}$")]
        decoder = msgspec.json.Decoder(typ)
        for index in range(10):
            package = importlib.import_module("pcre2")
            module = importlib.import_module("pcre2._cy")
            del sys.modules["pcre2._cy"]
            del sys.modules["pcre2"]
            del package
            del module
            gc.collect()
            assert decoder.decode(b'"US"') == "US"
            # Each alias is fresh, independent of Annotated's metadata cache.
            pattern = "^[A-Z]{2," + str(index + 3) + "}$"
            next_type = Annotated[str, msgspec.Meta(pattern=pattern)]
            assert msgspec.json.decode(b'"GB"', type=next_type) == "GB"
        """
    )
    subprocess.run([sys.executable, "-c", code], check=True, timeout=30)


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="ctypes and subprocesses unavailable"
)
def test_incompatible_backend_interface():
    code = textwrap.dedent(
        """
        import ctypes
        import sys
        import sysconfig
        import types
        import msgspec

        class API(ctypes.Structure):
            _fields_ = [
                ("version", ctypes.c_uint), ("size", ctypes.c_size_t),
                ("type", ctypes.c_void_p), ("new_matcher", ctypes.c_void_p),
                ("search", ctypes.c_void_p),
            ]

        name = b"pcre2._cy._MATCHER_API_v1"
        capsule_new = ctypes.pythonapi.PyCapsule_New
        capsule_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
        capsule_new.restype = ctypes.py_object
        package = types.ModuleType("pcre2")
        package.__path__ = []
        sys.modules["pcre2"] = package
        cases = [
            (2, ctypes.sizeof(API), None, None, None),
            (1, 0, None, None, None),
            (1, ctypes.sizeof(API), None, 1, 1),
            (1, ctypes.sizeof(API), id(str), None, 1),
            (1, ctypes.sizeof(API), id(str), 1, None),
        ]
        for values in cases:
            # A complete table is allocated, but invalid entries are never called.
            api = API(*values)
            module = types.ModuleType("pcre2._cy")
            module._MATCHER_API_v1 = capsule_new(ctypes.addressof(api), name, None)
            sys.modules["pcre2._cy"] = module
            if sysconfig.get_config_var("Py_GIL_DISABLED"):
                msgspec.Meta(pattern="^a$")
                continue
            try:
                msgspec.Meta(pattern="^a$")
            except ImportError as exc:
                assert "Incompatible" in str(exc)
            else:
                raise AssertionError("Incompatible interface accepted")
        """
    )
    subprocess.run([sys.executable, "-c", code], check=True, timeout=30)


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Subprocesses unavailable"
)
def test_optional_provider_lookup_errors():
    code = textwrap.dedent(r"""
        import sys
        import sysconfig
        import types
        from typing import Annotated
        import msgspec

        package = types.ModuleType("pcre2")
        package.__path__ = []
        def unexpected_compile(*args, **kwargs):
            raise AssertionError("SDK compile used without a matcher API")
        package.compile = unexpected_compile
        sys.modules["pcre2"] = package
        provider = types.ModuleType("pcre2._cy")
        sys.modules["pcre2._cy"] = provider
        typ = Annotated[str, msgspec.Meta(pattern="^a$")]
        assert msgspec.json.decode(b'"a"', type=typ) == "a"
        try:
            msgspec.json.decode(b'"b"', type=typ)
        except msgspec.ValidationError:
            pass
        else:
            raise AssertionError("Fallback constraint omitted")

        for error in (AttributeError("dynamic provider failure"),
                      RuntimeError("dynamic provider failure")):
            def lookup(name):
                raise error
            provider.__getattr__ = lookup
            if sysconfig.get_config_var("Py_GIL_DISABLED"):
                msgspec.Meta(pattern="^a$")
                continue
            try:
                msgspec.Meta(pattern="^a$")
            except type(error) as exc:
                assert exc is error
            else:
                raise AssertionError("Dynamic provider error hidden")
    """)
    subprocess.run([sys.executable, "-c", code], check=True, timeout=30)


@pytest.mark.skipif(
    bool(sysconfig.get_config_var("Py_GIL_DISABLED")),
    reason="Free-threaded builds do not import the optional SDK",
)
def test_sdk_without_matcher_api(monkeypatch):
    try:
        provider = importlib.import_module("pcre2._cy")
    except ModuleNotFoundError as exc:
        if exc.name != "pcre2":
            raise
        pytest.skip("Optional pcre2 package not installed")
    package = importlib.import_module("pcre2")
    # This also exercises a published SDK that never defined the capsule.
    monkeypatch.delattr(provider, "_MATCHER_API_v1", raising=False)

    def unexpected_compile(*args, **kwargs):
        raise AssertionError("SDK compile used without a matcher API")

    monkeypatch.setattr(package, "compile", unexpected_compile)
    typ = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2,17}$")]
    assert msgspec.json.decode(b'"US"', type=typ) == "US"
    with pytest.raises(msgspec.ValidationError):
        msgspec.json.decode(b'"bad"', type=typ)


def test_sdk_newline_setting_preserves_python_semantics(native, monkeypatch):
    package = importlib.import_module("pcre2")
    original_compile = package.compile
    pattern = "^[A-Z]{2,2}$"
    crlf_pattern = original_compile("(*CRLF)" + pattern)
    assert crlf_pattern.search("US\n") is None
    assert crlf_pattern.search("US\r\n") is not None
    compiled = []

    def crlf_compile(pattern):
        compiled.append(pattern)
        return original_compile("(*CRLF)" + pattern)

    monkeypatch.setattr(package, "compile", crlf_compile)
    typ = Annotated[str, msgspec.Meta(pattern=pattern)]
    assert len(compiled) == 1
    assert msgspec.inspect.type_info(typ).pattern == pattern
    assert msgspec.json.schema(typ)["pattern"] == pattern
    for subject in ("US", "US\n", "US\r\n", "US\r", "bad"):
        if re.search(pattern, subject) is None:
            with pytest.raises(msgspec.ValidationError):
                msgspec.convert(subject, type=typ)
        else:
            assert msgspec.convert(subject, type=typ) == subject


@pytest.mark.parametrize("code", [120, -45])
def test_sdk_compile_resource_fallback(native, monkeypatch, code):
    package = importlib.import_module("pcre2")
    error = package.LibraryError(code)

    def fail_compile(*args, **kwargs):
        raise error

    monkeypatch.setattr(package, "compile", fail_compile)
    typ = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2,18}$")]
    assert msgspec.json.decode(b'"US"', type=typ) == "US"
    with pytest.raises(msgspec.ValidationError):
        msgspec.json.decode(b'"bad"', type=typ)


@pytest.mark.parametrize("kind", ["library", "memory", "unexpected"])
def test_sdk_compile_errors_propagate(native, monkeypatch, kind):
    package = importlib.import_module("pcre2")
    if kind == "library":
        error = package.LibraryError(-1)
    elif kind == "memory":
        error = MemoryError("SDK allocation failure")
    else:
        error = RuntimeError("SDK compilation failure")

    def fail_compile(*args, **kwargs):
        raise error

    monkeypatch.setattr(package, "compile", fail_compile)
    with pytest.raises(type(error)) as exc:
        msgspec.Meta(pattern="^[A-Z]{2}$")
    assert exc.value is error


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Subprocesses unavailable"
)
def test_native_provider_cycle_and_partial_cleanup(native):
    code = textwrap.dedent(r"""
        import gc
        import sys
        import types
        import typing
        import weakref
        import pcre2
        import msgspec

        original = pcre2._cy
        provider = types.ModuleType("pcre2._cy")
        provider._MATCHER_API_v1 = original._MATCHER_API_v1
        provider.LibraryError = original.LibraryError
        provider._BooleanMatcher = original._BooleanMatcher
        sys.modules["pcre2._cy"] = provider
        try:
            initial_refs = sys.getrefcount(provider)
            meta = msgspec.Meta(pattern="^[A-Z]{2,19}$")
            assert sys.getrefcount(provider) > initial_refs
            typ = typing.Annotated[str, meta]
            class Record(msgspec.Struct):
                value: typ
            provider.owner = Record
            record_ref = weakref.ref(Record)
            provider_ref = weakref.ref(provider)
            decoder = msgspec.json.Decoder(Record)
            assert decoder.decode(b'{"value":"US"}').value == "US"
            invalid = typing.Annotated[int, msgspec.Meta(min_length=1)]
            before = sys.getrefcount(provider)
            for _ in range(50):
                try:
                    msgspec.json.Decoder(tuple[typ, invalid])
                except TypeError:
                    pass
                else:
                    raise AssertionError("Invalid annotation accepted")
                temporary = msgspec.json.Decoder(list[typ])
                assert temporary.decode(b'["US"]') == ["US"]
                del temporary
            assert sys.getrefcount(provider) == before
            del decoder, Record, typ, meta, provider
        finally:
            sys.modules["pcre2._cy"] = original
        for clear in typing._cleanups:
            clear()
        gc.collect()
        assert record_ref() is None
        assert provider_ref() is None
    """)
    subprocess.run([sys.executable, "-c", code], check=True, timeout=30)


@pytest.fixture(params=["json", "msgpack", "convert"])
def validate(request):
    if request.param == "convert":
        return lambda value, typ: msgspec.convert(value, typ, strict=True)
    proto = getattr(msgspec, request.param)
    return lambda value, typ: proto.decode(proto.encode(value), type=typ, strict=True)


def test_pattern_validation(validate):
    typ = Annotated[str, msgspec.Meta(pattern="^a+$")]
    assert validate("aaa", typ) == "aaa"
    with pytest.raises(msgspec.ValidationError):
        validate("b", typ)


@pytest.mark.parametrize(
    "pattern",
    [
        "^[a-zA-Z][a-zA-Z0-9_.:-]{0,63}$",
        "^[A-Z]{2}$",
        "^[a]*$",
        "^[a]+?$",
        "^[a]?b$",
        "^[a]{2}$",
        "^[a]{1,3}$",
        "^[a]{1,}$",
        "^[a]{1,3}?$",
        r"^[a]\.[0-9]+$",
        r"^[a]\$$",
        r"^[a]\\$",
        "cat",
        "",
        r"^\w+$",
        r"^\d+$",
        r"^\s$",
        r"^\v$",
        r"(?a)^\w+$",
        "(?i)^[a-z]$",
        "(?m)^a$",
        "(?s)^.$",
        r"a\Z",
        r"a(?=b)",
        r"(?<=a)b",
        r"(a)\1",
        r"(a)?(?(1)b|c)",
        "[^a]",
        "a|b",
        ".",
    ],
)
def test_patterns_match_re(pattern, validate):
    oracle = re.compile(pattern)
    subjects = [
        "",
        "a",
        "aa",
        "aaa",
        "aaaa",
        "ab",
        "b",
        "c",
        "US",
        "a.12",
        "a$",
        "a\\",
        "xxcatyy",
        "a\n",
        "a\n\n",
        "a\r\n",
        "é",
        "İ",
        "K",
        "²",
        "١",
        "\u0301",
        "\u203f",
        "\u001c",
        "\v",
        "\n",
        "\r",
        "\x00",
        "🙂",
    ]
    typ = Annotated[str, msgspec.Meta(pattern=pattern)]
    for subject in subjects:
        if oracle.search(subject) is None:
            with pytest.raises(msgspec.ValidationError):
                validate(subject, typ)
        else:
            assert validate(subject, typ) == subject


@pytest.mark.parametrize("pattern", [r"a\z", r"^\B$", "a(?i)b", "a*+a", "(?>a|ab)c"])
def test_version_sensitive_python_contract(pattern, validate):
    error = None
    with warnings.catch_warnings(record=True) as reference_warnings:
        warnings.simplefilter("always")
        re.purge()
        try:
            oracle = re.compile(pattern)
        except re.error as exc:
            error = (type(exc), str(exc))
    expected_warnings = [
        (item.category, str(item.message)) for item in reference_warnings
    ]
    with warnings.catch_warnings(record=True) as actual_warnings:
        warnings.simplefilter("always")
        re.purge()
        if error is not None:
            with pytest.raises(error[0]) as exc:
                msgspec.Meta(pattern=pattern)
            assert str(exc.value) == error[1]
        else:
            meta = msgspec.Meta(pattern=pattern)
    assert [
        (item.category, str(item.message)) for item in actual_warnings
    ] == expected_warnings
    if error is not None:
        return
    typ = Annotated[str, meta]
    for subject in ("", "a", "a\n", "aaaa", "abc", "ac"):
        if oracle.search(subject) is None:
            with pytest.raises(msgspec.ValidationError):
                validate(subject, typ)
        else:
            assert validate(subject, typ) == subject


@pytest.mark.parametrize("pattern", ["[", r"[\8]", r"\q", r"(?au)a", r"(?L)a"])
def test_python_compile_errors(pattern):
    with pytest.raises(re.error):
        re.compile(pattern)
    with pytest.raises(re.error):
        msgspec.Meta(pattern=pattern)


@pytest.mark.parametrize("pattern", ["^[A-Z]{2}$", ".", "a", r"^\w+$"])
def test_surrogates_follow_python_convert(pattern):
    oracle = re.compile(pattern)
    typ = Annotated[str, msgspec.Meta(pattern=pattern)]
    for subject in ("\ud800", "\udc00", "a\ud800", "US\ud800"):
        if oracle.search(subject) is None:
            with pytest.raises(msgspec.ValidationError):
                msgspec.convert(subject, typ)
        else:
            assert msgspec.convert(subject, typ) == subject


def test_pattern_preserved_for_schema_and_error(native, validate):
    pattern = r"^[A-Z]{2}$"
    meta = msgspec.Meta(pattern=pattern)
    typ = Annotated[str, meta]
    assert meta.pattern == pattern
    assert msgspec.inspect.type_info(typ).pattern == pattern
    assert msgspec.json.schema(typ)["pattern"] == pattern
    with pytest.raises(msgspec.ValidationError) as exc:
        validate("1", typ)
    assert repr(pattern) in str(exc.value)


def test_nested_and_dict_key_constraints(validate):
    key = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2}$")]
    typ = dict[key, list[key]]
    assert validate({"US": ["GB", "DE"]}, typ) == {"US": ["GB", "DE"]}
    for value in ({"bad": ["GB"]}, {"US": ["bad"]}):
        with pytest.raises(msgspec.ValidationError):
            validate(value, typ)


def test_partial_type_construction_cleanup():
    text = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2}$")]
    invalid = Annotated[int, msgspec.Meta(min_length=1)]
    # The first node owns a pattern before the second field is rejected.
    for _ in range(50):
        with pytest.raises(TypeError):
            msgspec.json.Decoder(tuple[text, invalid])
        decoder = msgspec.json.Decoder(list[text])
        assert decoder.decode(b'["US"]') == ["US"]
        del decoder
    gc.collect()


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Threads unavailable"
)
def test_shared_decoder_threaded():
    typ = Annotated[str, msgspec.Meta(pattern="^[A-Z]{2}$")]
    decoder = msgspec.json.Decoder(typ)
    barrier = Barrier(8)

    def work(worker):
        barrier.wait(timeout=10)
        for index in range(200):
            value = "US" if (worker + index) % 2 else "bad"
            if value == "US":
                assert decoder.decode(b'"US"') == value
            else:
                with pytest.raises(msgspec.ValidationError):
                    decoder.decode(b'"bad"')
        return True

    with ThreadPoolExecutor(max_workers=8) as pool:
        assert all(pool.map(work, range(8)))


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"},
    reason="Threads and subprocesses unavailable",
)
def test_first_concurrent_pattern_constructors(native):
    code = textwrap.dedent(r"""
        import json
        import sys
        from concurrent.futures import ThreadPoolExecutor
        from threading import Barrier
        from typing import Annotated

        before = getattr(sys, "_is_gil_enabled", lambda: True)()
        import msgspec
        after_import = getattr(sys, "_is_gil_enabled", lambda: True)()
        assert after_import == before
        barrier = Barrier(8)

        def work(worker):
            barrier.wait(timeout=10)
            for index in range(12):
                width = 1 + (worker + index) % 3
                pattern = "^[A-Z]{" + str(width) + "}$"
                typ = Annotated[str, msgspec.Meta(pattern=pattern)]
                decoder = msgspec.json.Decoder(typ)
                value = "A" * width
                assert decoder.decode(msgspec.json.encode(value)) == value
                try:
                    decoder.decode(b'"bad"')
                except msgspec.ValidationError:
                    pass
                else:
                    raise AssertionError("Invalid subject accepted")
            return worker

        with ThreadPoolExecutor(max_workers=8) as pool:
            assert sorted(pool.map(work, range(8))) == list(range(8))
        after_calls = getattr(sys, "_is_gil_enabled", lambda: True)()
        assert after_calls == before
        print(json.dumps({"before": before, "after_import": after_import,
                          "after_calls": after_calls, "constructors": 96}))
    """)
    result = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    evidence = json.loads(result.stdout)
    assert evidence["before"] == evidence["after_import"] == evidence["after_calls"]
    assert evidence["constructors"] == 96


@pytest.mark.skipif(
    sys.platform in {"emscripten", "wasi"}, reason="Subprocesses unavailable"
)
def test_native_and_fallback_call_paths_and_lifetime(pcre2_available):
    code = textwrap.dedent(r"""
        import gc
        import re
        import sys
        import typing
        import weakref

        compile_original = re.compile
        refs = []

        class Spy:
            def __init__(self, regex):
                self.regex = regex
                self.pattern = regex.pattern
                self.calls = 0
                self.owner = None
                refs.append(weakref.ref(self))

            def search(self, value):
                self.calls += 1
                return self.regex.search(value)

        def compile_spy(pattern, flags=0):
            regex = compile_original(pattern, flags)
            if pattern in ("^[A-Z]{2}$", r"^\w+$", r"^\w{1,16}$") and flags == 0:
                return Spy(regex)
            return regex

        re.compile = compile_spy
        import msgspec

        available = sys.argv[1] == "1"
        if available:
            import pcre2
            def unexpected_search(*args, **kwargs):
                raise AssertionError("Public SDK search used on native path")
            pcre2.Pattern.search = unexpected_search
        for pattern, expected_calls in [
            ("^[A-Z]{2}$", 0 if available else 1),
            (r"^\w+$", 1),
        ]:
            meta = msgspec.Meta(pattern=pattern)
            spy = refs[-1]()
            typ = typing.Annotated[str, meta]
            decoder = msgspec.json.Decoder(typ)
            assert decoder.decode(b'"US"') == "US"
            assert spy.calls == expected_calls, (pattern, spy.calls)
            if pattern == "^[A-Z]{2}$":
                for value in ("ÜS", "US\ud800"):
                    before = spy.calls
                    try:
                        msgspec.convert(value, typ)
                    except msgspec.ValidationError:
                        pass
                    else:
                        raise AssertionError("Invalid subject accepted")
                    assert spy.calls == before + 1
            del decoder, typ, meta, spy

        # A distinct pattern prevents reuse of either alias from the routing check.
        meta = msgspec.Meta(pattern=r"^\w{1,16}$")
        spy = refs[-1]()
        typ = typing.Annotated[str, meta]
        class Record(msgspec.Struct):
            value: typ
        spy.owner = Record
        record_ref = weakref.ref(Record)
        spy_ref = weakref.ref(spy)
        decoder = msgspec.json.Decoder(Record)
        assert decoder.decode(b'{"value":"US"}').value == "US"
        assert spy.calls == 1
        before = sys.getrefcount(spy)
        for _ in range(50):
            temporary = msgspec.json.Decoder(list[typ])
            assert temporary.decode(b'["US"]') == ["US"]
            del temporary
        assert sys.getrefcount(spy) == before
        del decoder, Record, typ, meta, spy
        # Annotated's global cache otherwise remains an external cycle root.
        for clear in typing._cleanups:
            clear()
        gc.collect()
        assert record_ref() is None
        assert spy_ref() is None
        print("ok")
    """)
    result = subprocess.run(
        [sys.executable, "-c", code, "1" if pcre2_available else "0"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "ok"
